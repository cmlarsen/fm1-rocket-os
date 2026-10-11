# T6i: songs on real flash

Integration lane for T6 in [build-plan.md](../build-plan.md#lanes-v3-2026-10-11). RC1p runs in parallel and only touches debug code in `editor.c`. Don't edit `editor.c`.

Read first:
- `CLAUDE.md`: the flash safety rules, above all *save only while stopped and idle*, and never touch 0x0000–0x3FFF, 0xE8000, 0xFF000 or BTIF;
- `docs/engineering/model.md` (the storage layout and undo limits);
- `reports/T6c.md`, `core/model/song_store.h` and `undo_stack.h`;
- `firmware/src/app/plat_fm1.c` (X0X's flash driver, `plat_store_*`, erase and program, XIP stalls);
- `firmware/src/app/project.c` (X0X's autosave and `save_wait`);
- `firmware/src/app/live_bridge.c` (how LIVE builds its `song_t` today);
- `ui.c`'s autosave gate (T3 blocks autosave in LIVE);
- `tests/lint/flash_guard_test.c`.

**Ownership:**
- `firmware/src/app/plat_fm1.c` and `plat.h`: a `song_flash_t` backend over the region 0xA0000–0xDBFFF only, through the same guarded erase/program path X0X uses;
- the host runner's simulated flash (`host/x0x_host.c`) for the same region;
- a new `firmware/src/app/song_io.c/.h` that glues the store, the LIVE song and undo together;
- small hooks in `live_bridge.c`, `project.c` and the `ui.c` autosave gate;
- new tests and scenarios.

## Behaviour (v1, one current song; the songs list UI is T8)
1. **Boot.** `song_store` initialises over the region. If slot 0 holds a valid song, it becomes the current song. Otherwise the current song is built from the X0X project with `live_bridge_map` (as today) and marked dirty.
2. **LIVE uses the current song,** not a fresh mapping every time LIVE opens. Assigning keys, K1–K4 and clip launch defaults change the current song and mark it dirty.
3. **Autosave.** The current song saves to slot 0 under the same rule as X0X's project: stopped, quiet for 4 s, not in LIVE, and not mid-gesture. Save the X0X project and the song in the same idle window, one after the other, with no audio running. If a save fails (the store returns an error), show X0X's existing save-error indication and keep the song in RAM. Never retry in a loop.
4. **Flash guard.** The backend refuses any address outside 0xA0000–0xDBFFF; the existing flash guard test is extended to cover the new backend. Erase and program go through X0X's driver with whatever XIP and IRQ handling it already does.
5. **Undo.** Wire `undo_stack` into `song_io` (push and pop entry points), with no UI yet; T7 adds it. Durable deletes and takes go through the store APIs, as T6c designed.
6. **Takes:** `song_io_take_save/load(slot 0)` exist for T9i. Nothing calls them yet.

## Tests (write first)
- Host scenarios:
  - boot with an empty store builds the song from the project;
  - change a key assignment in LIVE, leave LIVE, stop, wait, and the song is saved; reboot (the runner's `reboot` keeps simulated flash) and the assignment is still there;
  - while playing, or while in LIVE, nothing writes the store (count the writes);
  - a corrupt slot-0 record falls back to the project mapping.
- Unit tests:
  - the backend refuses out-of-region addresses;
  - a power cut during an autosave, simulated with the runner's flash, leaves the previous song.

Run `make unit-core scenarios screens-lint parity check fw`, `tools/check_loader.sh build/x0x.fwsc`, and report the RAM and image numbers.

## Done
- Small commits with the attribution lines.
- `reports/T6i.md`.
- `worker_done` with `--outcome`, `--files-modified`, `--report-path reports/T6i.md`.
