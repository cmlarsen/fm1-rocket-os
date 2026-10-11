# T6c: song storage and undo core

Core lane for T6 ([build-plan.md](../docs/engineering/build-plan.md#lanes-v3-2026-10-11)). Pure C in
new files only; T6i wires it into X0X's project code and UI. `core/model/model.h` is frozen and
unchanged.

**Files:** `core/model/song_store.{h,c}`, `core/model/undo_stack.{h,c}`, `tests/host/song_store_test.c`,
`tests/host/undo_stack_test.c`, `tests/host/song_fuzz.c`, `Makefile` (`unit-store`), this report.

`make unit-store` builds each test with `-Werror` and runs it, then rebuilds it under
`-fsanitize=address,undefined` and runs it again. All of it passes on the Mac in ~6 s.

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
extent and only then un-owns the old one. The previous take extent is kept until it is reused, so a
replace-take is undoable once. With one spare of each kind, **N = 5 slots** fit worst-case
`(5+1)*4 + (5+1)*5 = 54 ≤ 60`. The coordinator will set `SONG_SLOTS = 5` in `model.h`; T6c does not
edit `model.h`.

This replaces A/B: **there is no directory on flash.** `song_store_init` scans every extent header, checks
its payload CRC and keeps the **newest valid record per slot** (sequence number). A power cut during
a save leaves the new header absent or partial (it is written last), so the scan finds the previous
record. A torn-record byte flip makes the payload CRC fail and the next-newest record loads; if none
is valid, the slot reads as a valid empty song, never as garbage.

### Record format (header written last)

```
off  size  field
0    4     magic      'SST1' = 0x31545353
4    2     version    1
6    2     kind       1 = song, 2 = take
8    1     slot       0 .. SONG_STORE_SLOTS-1
9    1     flags      0
10   2     rawlen     uncompressed payload bytes
12   4     seq        sequence number (newest wins)
16   4     len        stored payload bytes (song: compressed; take: raw)
20   4     crc        CRC-32 (zlib) of the stored payload
24   12    name        song name; list() reads it without decompressing
36   4     hcrc        CRC-32 (zlib) of bytes 0..35
40   ..    payload
```

A save: erase the free extent's sectors → program the payload (256-byte pages, never crossing a
page) → program the 40-byte header last → read the header and payload back and verify. Everything
is 32-bit integer; no heap, no `double`, no 64-bit division, no library (the device object's only
undefined symbol is `memcpy`, which X0X's `libc.c` provides).

Songs are compressed with X0X's `lz.c` (included directly), streamed straight to flash: the
compression sink fills one 256-byte page and programs it, so no multi-KiB scratch buffer is needed.
Takes are stored raw (their worst case is 5 sectors either way, and raw keeps load to a straight
read). Load always rejects malformed records — bad CRC, wrong version, `len`/`rawlen` out of range,
or a decompressed song that fails `song_validate` (`len > 64`, `nnotes > 96`, bad role ranges,
note/velocity out of range, …).

### Measured sizes

| song | compressed | + header | extent (4 sectors) |
|---|---|---|---|
| empty | **417 B** | 457 B | 3% |
| starter (4 drum, 3 bass, 3 lead, 2 chord, 1 FX, 2 motion lanes) | **998 B** | 1,038 B | 6% |
| worst case, every clip full of random data | **12,495 B** | 12,535 B | 77% |
| fully random `song_t` (the incompressible bound; not a valid song) | **14,466 B** | 14,506 B | 89% |

`sizeof(song_store_t) = 144 B` (caller-owned). The song's 4-sector extent leaves room for the
14,506-byte incompressible bound, so any valid song fits. 8 slots + 8 takes would need
`8*2*14.5 KiB + 8*2*16.4 KiB ≈ 494 KiB` with A/B, far past the 240 KiB region; the extent pool fits
**5**, which is what this report recommends.

## Undo

A fixed RAM ring, no heap. `undo_entry_t` is `4 + sizeof(clip_t) = 532 B`; `UNDO_STACK_DEPTH = 23`
gives `sizeof(undo_stack_t) = 12,240 B` (~12 KiB). Entries:

| type | payload | undo / redo |
|---|---|---|
| `UNDO_CLEAR_CLIP` | old `clip_t` at a clip index | swap the stored clip with `song->clip[i]` |
| `UNDO_COPY_CLIP` | old destination `clip_t` (+ source index) | same swap (the entry holds the copied value after the swap) |
| `UNDO_RECORD_CLIP` | old `clip_t` | same swap |
| `UNDO_REPLACE_TAKE` | slot | `song_store_take_swap(slot)` |
| `UNDO_DELETE_SONG` | slot | `song_store_undelete` / `song_store_delete` |

One entry serves undo and redo (the swap), the ring drops the oldest entry when full, and a new edit
clears the redo tail. `delete` sets a RAM flag only — the bytes stay until the slot is reused, which
matches the plan's note that delete-undo lasts until power-off. Take and delete entries carry no
bytes; the recoverable copy lives in the store.

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
int song_store_load(song_store_t *st, int slot, song_t *out);           /* newest valid, or empty + -1 */
int song_store_delete(song_store_t *st, int slot);
int song_store_undelete(song_store_t *st, int slot);
int song_store_is_deleted(const song_store_t *st, int slot);
int song_store_list(const song_store_t *st, int slot, song_slot_t *out);/* name, used, deleted, has_take */

int song_store_take_save(song_store_t *st, int slot, const take_hdr_t *hdr, const take_event_t *ev);
int song_store_take_load(song_store_t *st, int slot, take_hdr_t *hdr, take_event_t *ev, uint32_t max_events);
int song_store_take_swap(song_store_t *st, int slot);                    /* undo/redo of a replace */

void song_store_empty(song_t *out);      /* a valid empty song */
int  song_validate(const song_t *song);  /* 0 = ok; used by save and load */
int  song_store_measure(const song_t *song);  /* compressed size, for the slot budget */

#include "core/model/undo_stack.h"
void undo_stack_init(undo_stack_t *u);
void undo_stack_push_clear(undo_stack_t *u, uint8_t clip, const clip_t *old);
void undo_stack_push_record(undo_stack_t *u, uint8_t clip, const clip_t *old);
void undo_stack_push_copy(undo_stack_t *u, uint8_t dst, uint8_t src, const clip_t *old_dst);
void undo_stack_push_take(undo_stack_t *u, uint8_t slot);
void undo_stack_push_delete(undo_stack_t *u, uint8_t slot);
int undo_stack_undo(undo_stack_t *u, song_t *song, song_store_t *store);  /* 1 = undone */
int undo_stack_redo(undo_stack_t *u, song_t *song, song_store_t *store);
uint8_t undo_stack_undo_type(const undo_stack_t *u);   /* for the "UNDO: cleared Bass B" toast */
uint8_t undo_stack_redo_type(const undo_stack_t *u);
```

No allocation; each call is bounded (a save compresses/streams one song, a load reads at most six
extents). Save while stopped and idle is T6i's policy; the API is cheap enough to call then.

## Tests

`tests/host/song_store_test.c`
- round trip for the empty, starter and worst-case songs;
- `list` name/used/has_take; delete + undelete keep the bytes;
- newest valid wins, a corrupt newest falls back to the older copy, and a fully broken slot loads a
  valid empty song;
- take save / replace / undo (swap) / redo, and a too-small event buffer is refused;
- every erase and program stayed inside 0xA0000–0xDC000;
- `song_validate` rejects `len > 64`, `nnotes > 96`, a bad role range, a bad magic and `note > 127`,
  and `song_store_save` refuses a malformed song;
- **power cut at every byte**: the simulator stops programming at offset N; for a small record
  (1,038 B, all 1,039 offsets) and the worst-case record (12,535 B, all 12,536 offsets), then a
  reboot scan: every one loads the old or the new song, never garbage.

`tests/host/undo_stack_test.c` — clip clear/copy/record undo+redo, a new edit clearing redo, the ring
dropping the oldest, and the take/delete entries against a RAM store.

`tests/host/song_fuzz.c` — 100,000 random mutations (1–5 bytes each) of a valid stored region:
never crashes and never loads an invalid song, under ASan/UBSan.

## Device build (deltron.local, JieLi clang 4.0.1, pi32v2)

Compiled the way `tools/build.py` compiles X0X's units (`-target pi32v2 -mcpu=r3 -mfprev1
-ffp-contract=off -ffunction-sections -fno-builtin -I.`):

| unit | opt | `.text` | `.rodata` | `.data` | `.bss` | undefined |
|---|---|---|---|---|---|---|
| `song_store.c` | `-Os` | 4,152 B | 80 B | 0 | 4,096 B | `memcpy` |
| `song_store.c` | `-O2` | 6,746 B | 64 B | 0 | 4,096 B | — |
| `undo_stack.c` | `-Os` | 620 B | 0 | 0 | 0 | `memcpy`, `song_store_{delete,take_swap,undelete}` |
| `undo_stack.c` | `-O2` | 938 B | 0 | 0 | 0 | as above |

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
- Call `song_store_init` once at boot (it scans ~120 KiB; a few tens of ms).
- Save only while stopped and idle (UX principle 8); a `song_store_save` erases 4 sectors and
  programs up to ~15 KiB.
- Deletion is a RAM flag: check `song_store_is_deleted` / `song_slot_t.deleted`; `song_store_load`
  still returns the bytes of a deleted slot (they are what undelete restores).
- The previous take is kept per slot and evicted only when the 6-extent pool is reused, so a
  replace-take is undoable exactly once; `undo_stack_undo` for a take returns 0 if it was evicted.
- `model.h` still says `SONG_SLOTS 8`; use `SONG_STORE_SLOTS` (5) until the coordinator lowers it.

## Open items for the coordinator

- Lower `SONG_SLOTS` to 5 in `model.h` (T6c does not edit it).
- The 6 reserved sectors at the end of the region are unallocated headroom (e.g. a future format).
