# Verification: model.md HIGH findings 1–9 (Opus, 2026-10-11, on integration)

The repro is a wrapper around `tests/host/song_io_test.c`, run under ASan/UBSan. Stack sizes were measured with JieLi clang `-target pi32v2 -O2` on deltron.

1. **New song overflows the device stack: CONFIRMED.**
   - The user stack is 24,320 B (`firmware/app.ld:32`, `crt0.S`).
   - Frames: `song_io_songs_create` 14,388 B, `song_template_starter` 14,420 B, `song_template_empty` 14,388 B, so 28.8 KB nested.
   - The EMU guard traps it ("X0X CRASH") and resets. Deleting the last song (`ui.c:2344`) hits the same path.
   - Fix: build the template directly into `*out`, and use the shared song scratch.
2. **Motion makes a song unsavable: CONFIRMED.**
   - Lane 0x6001 gives validate −1, save −2, and even 0x4000 is rejected. Every UI-written motion param is above the 0x0800 limit.
   - Fix: validate the real target/voice/index encoding.
3. **Fallback extent left unprotected: CONFIRMED (narrow trigger).**
   - A CRC-valid record that fails validation, plus a failed write, loses the slot.
   - Fix: adopt the fallback extent and its seq into `slot[]`; do the same for takes.
4. **Flush reports success after a failed save: CONFIRMED.**
   - `flush` returns 0 with dirty and failed both set, and opening another song then discards the edit.
   - Fix: return −1 when dirty && failed, and block song switches.
5. **Take undo writes flash in LIVE: CONFIRMED by trace.**
   - `undo_run` → `song_store_take_restore` → `sst_write_take`, with no transport or LIVE check. The MIDI-sink half is minor.
   - Fix: swap in RAM now, and defer the write until stopped and idle.
6. **Shortening a clip invalidates the song: CONFIRMED.**
   - BASS A 16 → 8 steps gives validate −1, save −2. `note_add` past `len` is also accepted.
   - Fix: trim notes past the new length (undoably), and clamp the setters.
7. **A failed take clear is acknowledged: CONFIRMED.** rc=0, pending=0, and the old take is still on flash. Fix: propagate rc and keep the request pending.
8. **Song/take split commit: REFUTED as high.** Every interrupted window reboots to the state before or after the operation. A stale `has_take=1` is corrected by the loader. Low.
9. **A failed take undo corrupts the RAM take: CONFIRMED.**
   - runtime_count=3, hdr_nevents=2, and redo and undo then both return −1.
   - Fix: restore into scratch and copy over only on success, or fold it into fix 5.

**Fix order:**
1. The stack (1).
2. One shared model contract for the validator and the editor (2, 6).
3. Propagate swallowed failures (4, 7).
4. Deferred take restore (5, 9).
5. Fallback extent adoption (3).
6. A pi32v2 stack-usage check that parses the `sp +=` prologues in `x0x.dis`.
