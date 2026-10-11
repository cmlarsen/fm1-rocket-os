# T6c: song storage and undo core

Core lane for T6 ([build-plan.md](../docs/engineering/build-plan.md#lanes-v3-2026-10-11)), plus the
fixes from [T6c-review.md](T6c-review.md). Pure C in new files only; T6i wires it into X0X's project
code and UI. `core/model/model.h` is frozen and unchanged.

**Files:** `core/model/song_store.{h,c}`, `core/model/undo_stack.{h,c}`, `tests/host/song_store_test.c`,
`tests/host/undo_stack_test.c`, `tests/host/song_fuzz.c`, `Makefile` (`unit-store`), this report.

`make unit-store` builds each test with `-Werror` and runs it, then rebuilds it under
`-fsanitize=address,undefined` and runs it again. All of it passes on the Mac in ~20 s.

## Storage

### Region and slotted extents

T6 owns **0xA0000–0xDBFFF** (60 × 4096 B = 240 KiB), the user-sample area, which is unused after T1
dropped BREAK. It is the space beside X0X's own objects: `0x97000–0x9EFFF` (SOUND/PAT0/PAT1/SONG)
and `0xDC000–0xDFFFF` (PANEL/MOTION) are left alone, as is `0x9F000` (FM6 patches). Compile-time
asserts pin this (`song_store.c`): base ≥ 0xA0000, end ≤ 0xDC000, extents fit, and both worst-case
records fit their extent.

A record lives in an **extent**. Song extents are 4 sectors; take extents are 5:

| extent | count | sectors | bytes | holds |
|---|---|---|---|---|
| song | 6 | 4 | 16,384 | one `song_t`, compressed |
| take | 6 | 5 | 20,480 | one take, raw |

6 song + 6 take = 54 sectors; the last 6 sectors (0xD6000–0xDBFFF) are reserved. The pool is
`N + 1` extents of each kind, so a save always has a spare: it writes the new record into the free
extent and only then un-owns the old one. With one spare of each kind, **N = 5 slots** fit worst-case
(`(5+1)*4 + (5+1)*5 = 54 ≤ 60`), and `model.h` carries `SONG_SLOTS 5`. T6c does not edit `model.h`.

Wear-levelling: among the free extents of a kind the one holding the **oldest record** (lowest
sequence number, 0 if erased) is chosen, so erases move around the pool.

This replaces A/B: **there is no directory on flash.** `song_store_init` scans every extent header,
checks its payload CRC and keeps the **newest valid record per slot** (sequence number). A power cut
during a save leaves the new header absent or partial (it is written last), so the scan finds the
previous record. A torn-record byte flip makes the payload CRC fail and the next-newest record
loads; if none is valid, the slot reads as a valid empty song, never as garbage.

### Record format (header written last)

```
off  size  field
0    4     magic      'SST1' = 0x31545353
4    2     version    1
6    2     kind       1 = song, 2 = take
8    1     slot       0 .. SONG_STORE_SLOTS-1
9    1     flags      songs: bit 0 = deleted; takes: 0
10   2     rawlen     uncompressed payload bytes
12   4     seq        sequence number (newest wins)
16   4     len        stored payload bytes (song: compressed; take: raw)
20   4     crc        CRC-32 (zlib) of the stored payload
24   12    name        song name; list() reads it without decompressing
36   4     hcrc        CRC-32 (zlib) of bytes 0..35
40   ..    payload
```

A save: erase **only the sectors the record needs** (`ceil((40 + len)/4096)`) → program the payload
(256-byte pages, never crossing a page) → program the 40-byte header last → read the header and
payload back and verify. **If the post-commit check fails, the header sector is erased** so the
record cannot win a later scan, and the save returns an error. Everything is 32-bit integer; no
heap, no `double`, no 64-bit division, no library (the device object's only undefined symbol is
`memcpy`, which X0X's `libc.c` provides).

Songs are compressed with X0X's `lz.c` (included directly), streamed straight to flash: the first
pass counts the compressed size (to size the erase), the second fills 256-byte pages and programs
them, so no multi-KiB scratch buffer is needed. Takes are stored raw. Load always rejects malformed
records — bad CRC, wrong version, `len`/`rawlen` out of range, or a decompressed song that fails
`song_validate`.

### Deleted slots persist

Delete and undelete **re-commit the record** with the `deleted` flag set or cleared and a new
sequence number, into a free extent. The data is not touched in place, so both survive a reboot and
a power cut; undelete reads the flagged record's bytes and writes them back unflagged. A deleted
slot reads as **free** (`song_store_list` reports `used 0`, empty name, no take; `song_store_load`
refuses it), but its take and its bytes stay until the slot is reused. A save into a deleted slot
simply supersedes it.

### Take undo persists

`song_store_take_restore` (the undo/redo of a replace-take) loads the previous take into the
**caller's buffer** and commits it as a new take record with a new sequence number, reusing the
previous take's own extent. The replaced take lives in another extent and stays valid until the
commit lands, so a power cut here leaves the newer take in charge and only loses the undo. The
replaced take becomes the new previous, so redo is the same call.

### Measured sizes

| song | compressed | + header | extent (4 sectors) |
|---|---|---|---|
| empty | **417 B** | 457 B | 3% |
| starter (4 drum, 3 bass, 3 lead, 2 chord, 1 FX, 2 motion lanes) | **999 B** | 1,039 B | 6% |
| worst case, every clip full of random data | **12,715 B** | 12,755 B | 78% |
| fully random `song_t` (the incompressible bound; not a valid song) | **14,465 B** | 14,505 B | 89% |

A song's 4-sector extent leaves room for the 14,505-byte incompressible bound, so any valid song
fits. 8 slots + 8 takes would need `8*2*14.5 KiB + 8*2*16.4 KiB ≈ 494 KiB` with A/B, far past the
240 KiB region; the extent pool fits **5**.

## Undo

A fixed RAM ring, no heap. `undo_entry_t` is `4 + max(sizeof(clip_t), 4) = 532 B`;
`UNDO_STACK_DEPTH = 23` gives `sizeof(undo_stack_t) = 12,240 B` (~12 KiB). Entries:

| type | payload | undo / redo |
|---|---|---|
| `UNDO_CLEAR_CLIP` | old `clip_t` at a clip index | swap the stored clip with `song->clip[i]` |
| `UNDO_COPY_CLIP` | old destination `clip_t` (+ source index) | same swap (the entry holds the copied value after the swap) |
| `UNDO_RECORD_CLIP` | old `clip_t` | same swap |
| `UNDO_REPLACE_TAKE` | slot + the sequence number of the take to restore | `song_store_take_restore` (re-commits; survives reboot) |
| `UNDO_DELETE_SONG` | slot + the sequence number of the deleted record | `song_store_undelete` / `song_store_delete`, gated on that sequence number |

One entry serves undo and redo (the swap), the ring drops the oldest entry when full, and a new edit
clears the redo tail. Only the **newest replace-take entry per slot is undoable**: pushing a new one
sets older same-slot take entries' sequence number to 0, so they fail with `UNDO_CANNOT` (`-1`)
rather than a false success. Take and delete undo also return `UNDO_CANNOT` when their expected
sequence number no longer matches (the record was superseded, e.g. the slot was reused), distinct
from `UNDO_EMPTY` (`0`, nothing to undo) and `UNDO_DONE` (`1`). The sequence-number check is what
makes delete-undo safe after a slot is reused.

## API summary

```c
#include "core/model/song_store.h"          /* model.h only otherwise */

typedef struct {
    int (*read)(void *ctx, uint32_t off, void *dst, uint32_t n);
    int (*erase)(void *ctx, uint32_t off);                              /* one 4096 sector */
    int (*program)(void *ctx, uint32_t off, const void *src, uint32_t n); /* <= 256 B, no page wrap */
    void *ctx;
} song_flash_t;

int song_store_init(song_store_t *st, const song_flash_t *fb);          /* boot scan */
int song_store_save(song_store_t *st, int slot, const song_t *song);    /* 0 ok, -1 slot, -2 malformed, -3 full, -4 io */
int song_store_load(song_store_t *st, int slot, song_t *out);           /* newest valid; a deleted slot reads as free (-1, empty) */
int song_store_delete(song_store_t *st, int slot);                      /* re-commits with the flag; survives reboot */
int song_store_undelete(song_store_t *st, int slot);                    /* re-commits with the flag cleared */
int song_store_is_deleted(const song_store_t *st, int slot);
int song_store_list(const song_store_t *st, int slot, song_slot_t *out);/* name, used, deleted, has_take; deleted = free */

int song_store_take_save(song_store_t *st, int slot, const take_hdr_t *hdr, const take_event_t *ev);
int song_store_take_load(song_store_t *st, int slot, take_hdr_t *hdr, take_event_t *ev, uint32_t max_events);
int song_store_take_restore(song_store_t *st, int slot, take_hdr_t *hdr, take_event_t *ev, uint32_t max_events);

void song_store_empty(song_t *out);      /* a valid empty song */
int  song_validate(const song_t *song);  /* 0 = ok; used by save and load */
int  song_store_measure(const song_t *song);  /* compressed size, for the slot budget */

#include "core/model/undo_stack.h"
enum { UNDO_CANNOT = -1, UNDO_EMPTY = 0, UNDO_DONE = 1 };
typedef struct { take_hdr_t *hdr; take_event_t *ev; uint32_t max_events; } undo_take_scratch_t;

void undo_stack_init(undo_stack_t *u);
void undo_stack_push_clear(undo_stack_t *u, uint8_t clip, const clip_t *old);
void undo_stack_push_record(undo_stack_t *u, uint8_t clip, const clip_t *old);
void undo_stack_push_copy(undo_stack_t *u, uint8_t dst, uint8_t src, const clip_t *old_dst);
void undo_stack_push_take(undo_stack_t *u, const song_store_t *store, uint8_t slot);   /* call after the replace */
void undo_stack_push_delete(undo_stack_t *u, const song_store_t *store, uint8_t slot); /* call after the delete */
int undo_stack_undo(undo_stack_t *u, song_t *song, song_store_t *store, const undo_take_scratch_t *ts);
int undo_stack_redo(undo_stack_t *u, song_t *song, song_store_t *store, const undo_take_scratch_t *ts);
uint8_t undo_stack_undo_type(const undo_stack_t *u);   /* for the "UNDO: cleared Bass B" toast */
uint8_t undo_stack_redo_type(const undo_stack_t *u);
```

`push_take` / `push_delete` are called **after** the store edit they undo, and read the sequence
number to restore from the store. The take undo needs the caller's take buffer (`undo_take_scratch_t`);
the UI's recorder already owns one.

### Validator (`song_validate`, used by save and load)

Rejects: bad magic/version; `bpm_x10` outside 200–3000 (20.0–300.0 BPM); a name with no NUL inside
`SONG_NAME_LEN`; `swing > 100`, bad default quantize, bad live channel, bad flags; a role whose range
is not the fixed model range, or a bad engine/mute/volume; a `key_clip` entry that is not `CLIP_NONE`
and ≥ `NCLIPS`; a clip with bad `used`/`mode`/`quant`, `nnotes > 96`, or a bad `len`; a note with
`step ≥ clip.len`, `len` 0 or > 64, `note > 127`, or `vel` 0 or > 127; a motion parameter that is not
`PARAM_NONE` and is ≥ `0x0800`; a motion value that is not `MOTION_NONE` and is > 127.

## Tests

`tests/host/song_store_test.c`
- round trip for the empty, starter and worst-case songs (all three validate);
- newest valid wins, a corrupt newest falls back to the older copy, and a fully broken slot loads a
  valid empty song; every erase/program stays inside 0xA0000–0xDC000;
- **delete survives reboot** and reads as free; undelete survives reboot and the data is back; a
  save into a deleted slot supersedes it;
- take save / replace / restore, the restored take survives reboot, and a too-small buffer is refused;
- the validator's new cases (5a–5e: unterminated name, bpm range, note step vs clip length, note
  length/velocity, motion value/param, live channel);
- **power cut at every byte** of a save (small record, all 1,040 offsets; worst-case record, all
  12,756 offsets) then a reboot scan: old or new, never garbage;
- **power cut at every flash operation** across a burst of three saves over a full 5-slot pool (the
  reviewer's `cut.c`): every slot loads its old or its new song.

`tests/host/undo_stack_test.c` — clip clear/copy/record undo+redo, a new edit clearing redo, the ring
dropping the oldest, and against a RAM store: a replace-take undo survives a reboot, only the newest
take entry per slot is live (the older returns `UNDO_CANNOT`), delete/undelete survive a reboot, and
undo-delete after the slot was reused returns `UNDO_CANNOT`.

`tests/host/song_fuzz.c`
- phase A: 100,000 random mutations (1–5 bytes) of a valid stored region — no crash, no invalid song;
- phase B: 100,000 mutations of the decoded song fields, each re-encoded with a valid CRC and loaded
  back, so every load decompresses and **reaches the validator** (100%); the store's verdict is
  cross-checked against a direct `song_validate` (41,409 of the mutations were rejected). The reached
  ratio is printed and must be above 90%.

All run under `-fsanitize=address,undefined`.

## Device build (deltron.local, JieLi clang 4.0.1, pi32v2)

Compiled the way `tools/build.py` compiles X0X's units (`-target pi32v2 -mcpu=r3 -mfprev1
-ffp-contract=off -ffunction-sections -fno-builtin -I.`):

| unit | opt | `.text` | `.rodata` | `.data` | `.bss` | undefined |
|---|---|---|---|---|---|---|
| `song_store.c` | `-Os` | 4,952 B | 80 B | 0 | 4,096 B | `memcpy` |
| `song_store.c` | `-O2` | 7,540 B | 64 B | 0 | 4,096 B | — |
| `undo_stack.c` | `-Os` | 1,004 B | 0 | 0 | 0 | `memcpy`, `song_store_{delete,take_restore,undelete}` |
| `undo_stack.c` | `-O2` | 2,888 B | 0 | 0 | 0 | as above |

The 4 KiB `.bss` is X0X's `lz_head` from `lz.c` (the compressor's 3-byte hash), the same table
`project.c` already pays for. There is no other static RAM: the store's streaming buffers are on the
stack, plus the caller-owned `song_store_t` (144 B) and `undo_stack_t` (12,240 B). No `__udivdi3`,
no libm, no `double`; `memcpy` comes from X0X's `libc.c`.

## What T6i needs

- **Back `song_flash_t` with the real driver**: `read` → `st_read`, `erase` → `st_erase` (a 4 KiB
  sector), `program` → `st_prog`/`fl_write` (a ≤256-byte page). The region is fixed in
  `song_store.h`; nothing above 0xDFFFF or at 0x9F000 is touched.
- **Compile `song_store.c` as its own unit** (it includes `lz.c`, as `project.c` does, so the two
  must not land in one translation unit).
- Call `song_store_init` once at boot (it scans about 120 KiB; a few tens of ms).
- Save only while stopped and idle (UX principle 8); a `song_store_save` erases the sectors it needs
  (1 for a small song, 4 for the worst case) and programs up to ~15 KiB.
- Deletion persists: check `song_store_is_deleted` / `song_slot_t.deleted`; `song_store_load` and
  `song_store_list` treat a deleted slot as free.
- Give `undo_stack_undo` / `_redo` a caller-owned take buffer (`undo_take_scratch_t`; the recorder's
  own); clip-only stacks may pass NULL. Call `undo_stack_push_take` / `push_delete` after the store
  edit, and show "cannot undo" when they return `UNDO_CANNOT`.
- The previous take is kept per slot and evicted only when the 6-extent pool is reused, so a
  replace-take is undoable exactly once.
- `model.h` carries `SONG_SLOTS 5`, matching `SONG_STORE_SLOTS`.

## Review fixes

| review item | fix |
|---|---|
| 1 delete survives reboot | `delete`/`undelete` re-commit a new record with a `deleted` header flag; `list` reports deleted slots as free; a save supersedes it |
| 2 take undo survives reboot | `song_store_take_restore` loads the previous take into the caller's buffer and re-commits it; replaces the RAM swap |
| 3 take depth 1 per slot | entries store the expected sequence number; a new take entry invalidates older ones for the slot; stale returns `UNDO_CANNOT` |
| 4 undo-delete after reuse | the delete entry's sequence-number check fails cleanly (`UNDO_CANNOT`) |
| 5 validator | name NUL, bpm 200–3000, note `step < clip.len`, note `len`/`vel` range, motion value/param range, `key_clip` range |
| 6 fuzzer reaches the validator | phase B re-encodes mutated songs with a valid CRC and loads them; 100% reach, printed against a 90% bar |
| 7 wear / erase sizing / post-commit | oldest free extent chosen; only the needed sectors erased; a failed post-commit check erases the header sector; wrap left alone |

## Open items for the coordinator

- The 6 reserved sectors at the end of the region are unallocated headroom (e.g. a future format).
