# TRAM: restore pool headroom (priority)

`integration` fails the build's own check. Release: pool 339,548 of 344,064, so 4,516 B headroom, below the required 8,192 B. Debug: 341,340, so 2,724 B. T8i's 14 KB song-open scratch and T10i's 33 KB `perform_t` fit separately but not together.

Read first:
- `docs/engineering/architecture.md` (the RAM plan; update its table when you're done);
- `firmware/app.ld`, `tools/build.py` (the pool check);
- the build map: `make fw` on deltron, then look at the `.pool` symbols by size;
- `song_io.c` (T8i's scratch song for open);
- `core/model/song_store.c` (the recommit scratch and `lz_head` in the pool);
- `core/model/undo_stack.c` (12 KB ring);
- `firmware/src/dsp/perform.c`;
- `engine.c` (`dly_buf`).

**Ownership:** RAM placement and buffer sharing in those files only. **No behaviour change**: every scenario golden must stay byte-identical, apart from documented RAM numbers.

## Do, in this order, and stop once both builds have ≥ 10 KB headroom
1. **Share one 14,352 B `song_t` scratch** between `song_io`'s open/load path and `song_store`'s recommit path. They're both main-loop only and never nested; prove that by reading the call graph, and add a debug-build assert or guard flag that traps nested use.
2. Share `lz` work buffers between `song_store` and `project.c`, if they're still separate.
3. If still short: shrink the undo ring from about 12 KB to about 8 KB (fewer entries), and update `docs/engineering/model.md`'s undo depth.

**Never shrink `perform_t`'s history or `dly_buf` further** without asking the coordinator.

## Done
- Release and debug `make fw` / `make fw-debug` pass their checks, with headroom ≥ 10,240 B on both.
- `make unit-core scenarios screens-lint parity check fw fw-debug` and `tools/check_loader.sh` on both packages pass.
- Update the RAM plan table in `architecture.md` with the new numbers.
- Commit, write `reports/TRAM.md`, and send `worker_done`.
