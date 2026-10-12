# TFIXS: save-path fixes from the code review

Source: [reports/review/model.md](../../../reports/review/model.md) and its verification, [reports/review/verify-model.md](../../../reports/review/verify-model.md). Read both, plus [architecture.md](../architecture.md) and `docs/engineering/model.md`.

## Scope, in order
1. **New song overflows the device stack** (finding 1). `song_io_songs_create` (14,388 B) plus `song_template_starter` (14,420 B) need 28.8 KB, and the user stack is 24,320 B (`firmware/app.ld`).
   - Build templates directly into the destination, and use the shared song scratch in create and copy.
   - Deleting the last song (`ui.c:~2344`) takes the same path.
2. **A stack-usage gate for the device build.** Add a check to `make fw` and `make fw-debug` that parses the `sp` prologues in the disassembly and fails when any reachable product call chain could pass the user stack minus a margin. At minimum, fail when any single frame is over 8 KB, and report the largest frames. Prove it fails on the old code.
3. **One model contract for the validator and the editor** (findings 2 and 6).
   - The validator rejects real motion lanes (0x4xxx/0x6xxx params) and notes past a shortened clip's length, so the song stops saving silently.
   - Make `song_validate` accept every encoding the UI writes.
   - Shortening a clip trims notes past the new length, undoably.
   - Note setters clamp to `len`.
   - Add tests: record motion then save and reload; shorten a clip then save and reload.
4. **Swallowed failures** (findings 4 and 7).
   - `song_io_flush` returns an error when dirty && failed, and SONGS refuses to switch songs while a failed save is still dirty, with a toast.
   - `take_store_empty` propagates the write rc and keeps the request pending.

Findings 3, 5 and 9 (fallback extent, take restore writing flash in LIVE, failed take undo corrupting RAM) are the next round. Don't do them here unless they fall out of the fixes above.

## Rules
- Write a failing test first for each item.
- No flash writes while LIVE is active. Never touch flash regions 0x0000–0x3FFF, 0xE8000, 0xFF000 or BTIF.
- Keep X0X's layout. Never flash hardware.

## Done when
- `make unit unit-core scenarios screens-lint fuzz-ui check fw fw-debug` pass on the committed tree, and `tools/check_loader.sh` passes on both packages.
- The new stack gate passes, and the report gives the deepest chain's stack use against 24,320 B.
- `reports/TFIXS.md` lists each finding, the test that proves it, and the pool numbers.
