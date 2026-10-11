# T6i: songs on real flash

Integration lane for T6 ([build-plan.md](../docs/engineering/build-plan.md#lanes-v3-2026-10-11)), wiring
T6c's store and undo into X0X's project and UI, on the device and in the host runner, plus the
[review fixes](T6i-review.md). No hardware was flashed (`make check`/`make fw` only build).

**Files.** Added: `firmware/src/app/song_flash.h`, `firmware/src/app/song_io.{c,h}`,
`tests/host/song_io_test.c`, `tests/lint/no_sample_writers_test.sh`,
`tests/scenarios/t6i_{song,nosave,corrupt,powercut,remap,savefail}.x0x`. Changed:
`firmware/src/app/{plat_fm1.c,plat.h,live_bridge.c,live_bridge.h,project.c,ui.c,ui_live.c,main_fm1.c,editor.c,lz.c}`,
`firmware/src/x0x.c`, `firmware/hal/fm1_flash.h`, `core/model/song_store.{c,h}`,
`tests/host/{song_store_test.c,song_fuzz.c}`, `tests/lint/flash_guard_test.c`, `host/x0x_host.c`,
`host/build_host.sh`, `web/emu/build.sh`, `tools/build.py`, `tools/sampleio.py`, `Makefile`,
`tests/run_tests.sh`, `docs/product/interaction-model.md`, `web/EDITOR_PROTOCOL.md`, this report.
Deleted: `firmware/src/app/slots.c`, `tools/fm1_sample_upload.py` (review item 1).

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
they accept it, and the extra `FL_SONG_OK` pins the backend to 0xA0000–0xDBFFF alone. The prototypes
live in `song_flash.h` rather than `plat.h`, because `song_io.c` sees `model.h`, whose `NKEYS 27u`
clashes with `plat.h`'s `NKEYS 27`; `plat.h` includes it for the device and the host runner.

The host runner models the same region: 60 sectors, 0xFF at power-on, erase → 0xFF, program only
clears bits and never wraps a page, kept across `reboot`. `plat_song_*` there applies the same range
checks, counts erase+program ops (`expect songwrites`), and supports a one-shot program failure
(`songfail`) and a cut on the Nth op (`songcut`).

## `song_io`

New `firmware/src/app/song_io.{c,h}` glues the store, the LIVE song and undo. Its header is plain
(`void *`), so `ui.c` — one unit that already has X0X's own `song_t` in scope — can include it.

- **Boot** (`song_io_boot(pats, bpm_x10)`, called from `fm1_main`/the host `boot()`): `song_store_init`
  scans the region; if slot 0 holds a valid, undeleted song it becomes the current song. If the
  project's pattern hash differs from the hash stored with that song, the clips are re-derived from
  the project (key map, LIVE SETUP and tempo kept) and the song is marked dirty; otherwise the stored
  song is used as it is. With no valid slot the song is built from `proj.pat` and marked dirty.
- **Save** (`song_io_save`/`song_io_flush`): `song_store_save_tag` to slot 0 with the pattern hash.
  A failure sets `song_io_failed()`: the song stays in RAM and is **not** retried until the next edit
  (item 3).
- **Edits**: `song_io_set_key_clip` (T10's assign-keys) marks the song dirty and clears the failed
  state; `song_io_wants_save()` is what the autosave gate acts on.
- **Undo** (`song_io_push_*`, `song_io_undo/redo/…`): T6c's `undo_stack`, with the current song and
  the store; no UI yet (T7).
- **Takes** (`song_io_take_save/load`): T6c's take APIs, for T9i; nothing calls them yet.

The store (`song_store_t`), the undo ring (`undo_stack_t`, ~12 KiB) and the store's own scratch are
static `.pool` buffers.

## The hooks

- **`live_bridge.{c,h}`** — the song is `g_song`, owned by the bridge. It keeps an FNV-1a hash of the
  project's patterns (`live_bridge_pats_hash`) that the current clips were derived from
  (`live_bridge_song_hash`). `live_bridge_rebuild(pats)` re-derives the clips, keeping the song's own
  fields (key map, LIVE SETUP options, tempo, name); `live_bridge_song_baseline(hash)` marks a loaded
  song's clips as matching a hash. `live_bridge_enter()` re-derives only when the project hash no
  longer matches and returns whether it did, so LIVE uses the current song otherwise.
- **`project.c`** — `project_save()` writes the X0X project and then the song, in one idle window.
  Between the two it re-derives the clips if the saved project's patterns differ from the song's
  (`live_bridge_rebuild`), so the persisted song stays in step with the project — STUDIO edits keep
  reaching LIVE across a reboot (item 2) without a spurious rebuild at boot.
- **`ui.c`** — the autosave gate acts on `ui.dirty || song_io_wants_save()`; the T3 LIVE block (no
  flash write while LIVE is active) is unchanged. `ui_live.c` marks the song dirty when entering LIVE
  rebuilt the clips. `song_io_boot` is called once at real boot (`fm1_main`, host `boot`), not from
  `ui_init`, so the M-UPGRADE path does not re-init storage (item 5); that path flushes a dirty song
  first only when it is not in LIVE.

## Review fixes

| # | fix |
|---|---|
| 1 | **Removed the second song-region writer.** The `ED_SMP_*` commands and the sample-slot code are gone from `editor.c`; `slots.c` and its `smp_user_scan`/`plat_slot*` reads are deleted (the device's users, the host's slot simulator and the `slot`/`slotimg` commands with them); `tools/fm1_sample_upload.py` is deleted and `tools/sampleio.py`/`web/EDITOR_PROTOCOL.md`/`web/test_web.mjs` no longer carry the sample upload. A tree grep for any other erase/program: the only raw writers left are `firmware/src/x0x.c` (`st_erase`/`st_prog`, `FL_STORE_OK`, plus the OTA wrappers `ota_erase`/`ota_prog` in the OTA window), the app's song backend `plat_song_*`, and the update loader (`firmware/loader`, its own window) — the loader writes only the app area [0x4000,0x93000) and update records, never the song region. |
| 2 | **STUDIO edits reach LIVE after a reboot.** The song record header carries a 32-bit `phash` (`song_store_save_tag`/`song_store_tag`); `song_io_boot` and `live_bridge_enter` re-derive the clips when the project's pattern hash differs, and `project_save` keeps them in step. Key map/LIVE SETUP/tempo survive a rebuild. `t6i_remap`, `song_io_test::test_project_change_rebuilds`. |
| 3 | **No silent retry.** `song_store_t` marks the extent a save failed on bad for the session, so the next save picks another; `song_io_failed()` suppresses the song save (autosave and SAVE alike) until the next edit. `t6i_savefail`, `song_io_test::test_failed_save_no_retry`, `song_store_test` (`save_tag`/extent switch). |
| 4 | **The 14 KB stack frame is gone.** `sst_recommit_song` uses a static `song_t` scratch in `.pool`. |
| 5 | **The OTA path never re-inits storage.** `song_io_boot` moved out of `ui_init`; the M-UPGRADE path flushes a dirty song only when not in LIVE and then re-inits the UI alone. |
| 6 | **`lz_head` moved to `.pool`.** `lz.c` gained `LZ_HEAD_ATTR`; `song_store.c` pins its copy to `.pool`, `project.c` leaves its copy in `.bss`. `.bss` headroom is back (see the numbers). |

## Tests

- `make unit-core` (all suites pass). `unit-songio` covers: boot from an empty store; save → reboot;
  a project-hash change rebuilds and keeps the key map; the region guard; a **power cut during a
  save** leaves the previous song; a **failed save** shows an error, does not retry, and the next
  edit saves on another extent; a corrupt record falls back.
- `tests/lint/flash_guard_test.c` covers `FL_SONG_OK` and that the OTA and settings windows never
  cover the song region. `tests/lint/no_sample_writers_test.sh` (new, run by `make check`) fails if
  any sample-upload symbol or tool returns, or if a raw flash writer appears in `firmware/src`
  besides the guarded driver.
- New scenarios `t6i_song`, `t6i_nosave`, `t6i_corrupt`, `t6i_powercut`, `t6i_remap`, `t6i_savefail`.
  **Every existing scenario still passes with its golden unchanged**, including the LIVE set.

## Device build (deltron.local, JieLi clang 4.0.1, pi32v2)

`make check` / `make fw` and `tools/check_loader.sh build/x0x.fwsc` (loader matches X0X 1.0.5):

| | value |
|---|---|
| app image `x0x.bin` | **501,204 B** (of the app slot) |
| RAM `.data+.bss` | **95,720 B** of 98,304 (2,584 B spare) |
| POOL | **292,132 B** of 344,064 (spare 51,932; floor 8,192) |
| loader image / `ota.bin` | 7,764 B / 6,524 B |
| package `x0x.fwsc` | 609,680 B, identity FM-1_900 |
| checks | register access hal/-only; no soft-double; no heap / 64-bit division; flash guard ok; no sample writers |

The T5i-merge baseline was `.data+.bss` 96,204 / POOL 261,296. The store (144 B), the undo ring
(12,240 B), the recommit scratch (14,352 B) and `song_store.c`'s own `lz_head` (4,096 B) all moved to
`.pool` (+30,836 B); `.bss` went **down** 484 B because the sample-slot buffers (`slots.c`, the
editor's 512-byte ADPCM buffer) are gone.

## Limits and open items for the coordinator

- **The project→song mapping is transitional.** `project_save` keeps the clips in step and a boot/enter
  rebuild covers an unsaved edit or a power cut, but T7/T8 make the song the edited object; then the
  pattern-hash rebuild should become a one-time migration.
- **Undo has no UI** (T7); the take entry points are unused (T9i).
- `open-questions.md` #6 (LIVE → STUDIO transport) is unchanged: leaving LIVE still stops the clip
  engine.
