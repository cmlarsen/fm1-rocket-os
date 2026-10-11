# T6i: songs on real flash

Integration lane for T6 ([build-plan.md](../docs/engineering/build-plan.md#lanes-v3-2026-10-11)), wiring
T6c's store and undo into X0X's project and UI, on the device and in the host runner. RC1p edits
`editor.c` in parallel; it is untouched here. No hardware was flashed (`make check`/`make fw` only build).

**Files:** `firmware/src/app/plat_fm1.c`, `plat.h`, new `firmware/src/app/song_flash.h`; new
`firmware/src/app/song_io.{c,h}`; `firmware/src/app/live_bridge.{c,h}`, `project.c`, `ui.c`;
`firmware/hal/fm1_flash.h`; `host/x0x_host.c`, `host/build_host.sh`, `web/emu/build.sh`,
`tools/build.py`, `Makefile`, `tests/run_tests.sh`; `tests/lint/flash_guard_test.c`, new
`tests/host/song_io_test.c`, new `tests/scenarios/t6i_{song,nosave,corrupt,powercut}.x0x`;
`docs/product/interaction-model.md`, this report.

## The flash backend

`fm1_flash.h` gains the region guard:

```c
#define FL_SONG_LO  0x000A0000u
#define FL_SONG_HI  0x000DC000u
#define FL_SONG_OK(off, n) FL_IN(off, n, FL_SONG_LO, FL_SONG_HI)
```

`plat_fm1.c` implements the `song_flash_t` triple over X0X's own driver:

| backend | device | guard |
|---|---|---|
| `plat_song_read` | `st_read` (256-byte IRQ-off windows) | `flash_ok`, `FL_SONG_OK` |
| `plat_song_erase` | `st_erase` (one 4 KiB sector, then `audio_silence()` + `fl_erase4k`) | `flash_ok`, `FL_SONG_OK`, sector-aligned |
| `plat_song_program` | `st_prog` → `fl_write` (≤ 256 B, no page wrap) | `flash_ok`, `FL_SONG_OK`, page |

`st_erase`/`st_prog` already check `FL_STORE_OK` (0x97000–0xE0000): the song region is inside it, so
they accept it, and the extra `FL_SONG_OK` pins the backend to 0xA0000–0xDBFFF alone. Nothing at
0x9F000 (FM6 patches) or 0xDC000–0xDFFFF (PANEL/MOTION) is ever touched. The prototypes live in the
new `song_flash.h` rather than `plat.h`, because `song_io.c` sees `model.h`, whose `NKEYS 27u` clashes
with `plat.h`'s `NKEYS 27`; `plat.h` includes it for the device and the host runner.

The host runner models the same region: 60 sectors, 0xFF at power-on, erase → 0xFF, program only
clears bits and never wraps a page, and it is kept across `reboot`. `plat_song_*` there applies the
same range checks, counts erase+program ops (`expect songwrites`), and supports a cut on the Nth op
(the `songcut` command).

## `song_io`

New `firmware/src/app/song_io.{c,h}` glues the store, the LIVE song and undo. Its header is plain
(`void *`), so `ui.c` — one unit that already has X0X's own `song_t` in scope — can include it.

- **Boot** (`song_io_boot(pats, bpm_x10)`, called from `ui_init`): `song_store_init` scans the
  region; if slot 0 holds a valid, undeleted song it becomes the current song and LIVE is *locked*
  (no fresh mapping); otherwise the current song is `live_bridge_map`ped from `proj.pat`, its tempo
  clamped to `song_validate`'s 200–3000, and it is marked dirty.
- **Save** (`song_io_save`/`song_io_flush`): `song_store_save` to slot 0; a failure keeps the song in
  RAM, dirty.
- **Edits**: `song_io_set_key_clip` (T10's assign-keys) marks the song dirty.
- **Undo** (`song_io_push_clear/record/copy`, `song_io_undo/redo/undo_type/redo_type`): T6c's
  `undo_stack`, with the current song and the store; no UI yet (T7).
- **Takes** (`song_io_take_save/load`): T6c's take APIs, for T9i; nothing calls them yet.

The store (`song_store_t`) and the undo ring (`undo_stack_t`, ~12 KiB) are static `.pool` buffers.

## The hooks

- **`live_bridge.{c,h}`** — the song is `g_song`, owned by the bridge. New `live_bridge_song()`,
  `live_bridge_song_reload()` and `live_bridge_lock_song(on)`. `live_bridge_enter()` no longer rebuilds
  a fresh song every time: it re-derives the project's clips only when the project's **patterns**
  changed since the last mapping (an FNV-1a hash over `proj.pat`), keeps the song's `key_clip` across
  that re-derivation, and never re-derives over a stored song. So opening LIVE again with nothing
  changed uses the current song, while editing patterns in STUDIO (still the clip source in v1) still
  shows up in LIVE — which is what the existing LIVE scenarios rely on.
- **`project.c`** — `project_save()` writes the X0X project (settings, sound, the packed stream) and
  then the song, in that order, in the same idle window; a streaming error (`-7`) returns before the
  song is written. `project.c`'s `io_buf` moves to `.pool` (see RAM below).
- **`ui.c`** — `ui_init` calls `song_io_boot(&proj.pat[0], proj.set.bpm_x10)`; the autosave gate now
  treats `song_io_dirty()` like `ui.dirty`, so a song-only change saves. The T3 LIVE block (no flash
  write while LIVE is active) is unchanged.

## Behaviour

1. Boot adopts slot 0 or maps the project (and marks dirty) — `t6i_song`, `song_io_test`.
2. LIVE performs the current song; a key assignment made in LIVE is part of it — `t6i_song`,
   `t6i_nosave`.
3. Autosave: stopped, quiet 4 s, not in LIVE, not playing; project first, then the song; a failure
   shows X0X's existing "SAVE FAILED" and keeps the song in RAM (no loop; the 20 s gap still applies)
   — `t6i_nosave`, `t6i_powercut`.
4. The backend refuses anything outside 0xA0000–0xDBFFF — `flash_guard_test`, `song_io_test`.
5. `undo_stack` is wired (push/pop), no UI — `song_io_test` covers the store/undo linking.
6. `song_io_take_save/load` exist for T9i.

## Tests

- `make unit-core` (all suites pass; new `unit-songio` builds `tests/host/song_io_test.c` at
  `-Werror` and again under ASan/UBSan). It covers: boot with an empty store builds from the project;
  save → reboot → the stored song (locked, key map kept); the backend refuses out-of-region addresses;
  a **power cut during a save** leaves the previous song; a corrupt record falls back to the project.
- `tests/lint/flash_guard_test.c` is extended to `FL_SONG_OK`: the region base/top and the whole region
  are accepted, and 0x9F000, 0xDC000 and a range running into PANEL are refused (the region is also
  checked to sit inside `FL_STORE_OK`).
- New host scenarios `t6i_song`, `t6i_nosave`, `t6i_corrupt`, `t6i_powercut` (see Behaviour). The
  runner adds `expect songwrites|songsaves|songdirty|songloaded|keyclip<k>` and the `assignkey`,
  `songcorrupt`, `songcut` commands.
- **Every existing scenario still passes with its golden unchanged** — including the LIVE set
  (`live_pads`, `live_play`, `live_hold`, `live_leds`, `live_chordfx`, `live_entry_stop`,
  `live_autosave`), which edit patterns in STUDIO and then enter LIVE.

## Device build (deltron.local, JieLi clang 4.0.1, pi32v2)

`make check` / `make fw` and `tools/check_loader.sh build/x0x.fwsc` (loader matches X0X 1.0.5):

| | value |
|---|---|
| app image `x0x.bin` | **502,164 B** (of the app slot) |
| RAM `.data+.bss` | **96,492 B** of 98,304 |
| POOL | **277,500 B** of 344,064 (spare 66,564; floor 8,192) |
| loader image / `ota.bin` | 7,764 B / 6,524 B |
| package `x0x.fwsc` | 609,680 B, identity FM-1_900 |
| checks | register access hal/-only; no soft-double; no heap / 64-bit division; flash guard ok |

Baseline after the T5i merge was `.data+.bss` 96,204 / POOL 261,296. The change is `io_buf` (3,840 B)
into the pool, the store (144 B) and the undo ring (12,240 B) into the pool, and `song_store.c`'s own
`lz.c` copy (the 4 KiB `lz_head`, T6c's note) in `.bss`. Without moving `io_buf`, `.bss` would have
overflowed the 96 KiB RAM region by 2,028 B; with it, there is ~1.8 KiB headroom.

## Limits and open items for the coordinator

- **`song_store.c` carries a second `lz_head` (4 KiB `.bss`).** T6c accepts it as "the same table
  `project.c` already pays for"; here it forced `project.c`'s `io_buf` into the pool. Sharing one
  `lz.c` across the two units (or moving `lz_head` to `.pool`) would give the RAM back — a small
  follow-up, not T6i's ownership.
- **A stored song is authoritative after a reboot.** STUDIO pattern edits still reach LIVE within a
  session (the hash re-derives), but once slot 0 is valid, entering LIVE with changed patterns does
  not re-derive, and a re-derivation keeps only `key_clip` (not `name` or the LIVE SETUP fields).
  T7/T8 make the song the edited object, at which point the project→song mapping is transitional.
- **Undo is compiled in but has no UI** (T7); the take entry points are unused (T9i).
- `open-questions.md` #6 (LIVE → STUDIO transport) is unchanged by T6i: leaving LIVE still stops the
  clip engine.
