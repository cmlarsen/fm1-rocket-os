# T6c: song storage and undo core

Core lane for T6 in [build-plan.md](../build-plan.md#lanes-v3-2026-10-11). **New files only**; T6i wires it into X0X's project code and UI later.

Read first: `CLAUDE.md`, the worker rules in `docs/engineering/build-plan.md`, `docs/engineering/model.md` and `core/model/model.h` (frozen; use your `ask` command if you need a change), `docs/product/ux-principles.md` (principle 8: one save model, autosave while stopped; principle 9: one take per song), and X0X's storage: `firmware/src/app/plat.h` (`plat_store_load/save`, `PLAT_STORE_MAX`, the flash map comment), `firmware/src/app/project.c`, `firmware/src/app/lz.c`, `firmware/src/app/undo.c`, and how `plat_fm1.c` implements the store (records, CRC, sectors).

**Ownership (new files only):** `core/model/song_store.{c,h}`, `core/model/undo_stack.{c,h}`, `tests/host/song_store_test.c`, `tests/host/undo_stack_test.c`, `tests/host/song_fuzz.c`, and a `make unit-store` target appended to the Makefile.

## Storage
- Store `SONG_SLOTS` songs and one take per song through an abstract flash interface (`erase sector`, `program`, `read`; sector size and region from X0X's flash map). Tests use a RAM flash simulator; T6i will back it with the real driver.
- Use X0X's `lz.c` to compress songs. Measure the compressed size of: an empty song, the starter song (create a plausible one in the test: 4 drum clips, 3 bass, 3 lead, 2 chords, 1 FX, 2 motion lanes used), and a worst case (every clip full of random data). If 8 songs + 8 takes do not fit in the space left beside X0X's own objects, lower `SONG_SLOTS` in your report (do not edit `model.h`) and say how many fit.
- **Power-safe records:** each record is written as payload first, then a header with magic, version, length, sequence number and CRC32 last. Two copies (A/B) per slot; load picks the newest valid copy. A power cut at any byte of a write leaves the previous version loadable.
- API: `song_store_save(slot, const song_t*)`, `song_store_load(slot, song_t*)`, `song_store_delete(slot)` (recoverable: it marks the slot deleted but keeps the data until the slot is reused), `song_store_undelete(slot)`, `song_store_list(...)` (name, used, has_take), `take_save/take_load(slot, ...)`, keeping the previous take recoverable once (for undo).
- Never write outside the region you are given; assert it in code. Saving runs only while stopped and idle; that policy lives in T6i, but make the API fast to call (no allocation, bounded time per call).
- Loading rejects anything malformed (bad CRC, wrong version, length out of range, clip fields out of range such as `len > 64`, `nnotes > 96`, bad role ranges) and falls back to the other copy or an empty song. It never crashes.

## Undo
- A fixed RAM ring of undo entries (no heap) for: clear clip, copy clip over clip, a record pass into a clip, replace take, delete song. Clip entries store the old `clip_t`; take and delete entries refer to the recoverable copy in the store. Redo after undo; a new edit clears redo. Choose a depth that fits in about 12 KiB and report it.

## Tests (write them first)
- Round trip for all three sample songs; A/B fallback; **power cut at every byte offset** of a save (the simulator stops programming at offset N), each must load the old or the new song, never garbage; delete then undelete; take replace then undo.
- `song_fuzz.c`: 100,000 random mutations of a valid stored record must never crash or load an invalid song. Run all tests under `-fsanitize=address,undefined`.
- Compile the new files for the device on deltron; report code and RAM sizes.

## Done
- `make unit-store` passes on the Mac.
- Small commits with the attribution lines from `CLAUDE.md`'s instructions.
- `reports/T6c.md`: the record format, measured sizes and slot count, undo depth, API summary, anything T6i needs.
- `worker_done` with `--outcome`, `--files-modified`, `--report-path reports/T6c.md`.
