# T7b review (Opus): request changes. Safe to install.

Coordinator decisions are in **bold**. Each fix needs a failing-then-passing scenario or unit test. Re-seed any golden that baked a bug in (`studio_clipoverdub`), and say why.

1. **Torn clip snapshot.** The ISR copies `song->clip[]` into `play[]` while the main loop may be mid-write (`song_io.c:332` increments `nnotes` first; `note_remove` shifts in place; undo, clear and copy memcpy with no reload).
   - **The ISR never reads `song->clip[]`.** The main loop owns the snapshot: after every edit (note add or remove, step toggle, length, mode, undo, clear, copy, record commit), it copies the clip into `play[role]` with the audio IRQ masked (X0X's existing IRQ lock; the copy is 528 B, a few µs), when that clip is the role's playing clip.
   - Launch and loop then use `play[]` as it stands. A launch of a different clip copies in the main loop before posting the launch command.
   - Keep `edit_while_playing` and add a stress test for edits on every step.
2. **Recording:**
   - **Pre-roll keys** (`ui_clip.c:385`): before `rec.start`, keys only audition. They never step-enter.
   - **Overdub phase** (`ui_clip.c:97`): use the clip's own playhead (`play_pos`), not `(global-start)%len`.
   - **Silence after close** (`ui_clip.c:317`): commit at `rec.close` and launch now, so the loop plays from the boundary with no gap.
   - **Motion in a new-clip pass** (`ui_clip.c:512`): use `cl_rec_step()`.
   - **Recorded notes keep their real length,** from note-on to note-off, rounded to steps with a minimum of 1. A note still held when the loop closes ends at the loop end.
   - Quantize rounds to the nearest 16th; a note in the last half-step wraps to step 0 of the next pass.
   - **Drum REC records hits:** during REC on a DRUMS clip, white keys 1–11 trigger and record lanes 1–11 (finger drumming); accent with HOME held. Update `screens.md` STUDIO 4 with one line.
   - **Undo of REC on an empty slot** removes the clip it created: the creation goes inside the same undo entry (`ui_studio.c:364`), so the slot is empty again and stops playing.
3. **Notes roll** (`ui_clip.c:544` against `:85`): the visible range follows the octave the keys play, and shows the notes actually present (scroll to the clip's note range on open). Add the design's step strip, note-length bars and "bar N of M".
4. **Visual:**
   - `clipdrums_playing.png` must show the drum grid with the playhead, not the song home;
   - the drum grid uses 14 px rows, as in the design;
   - the motion page gets the design's page tabs.
5. **Not now:** don't resolve the T10i merge. I'll do it (`CLIP_CMD_*` values, `consume_cmds`, the `clip_len` → `play_len` rename).

Then run `make unit-core scenarios screens-lint parity check fw` and `tools/check_loader.sh`, commit, update reports/T7b.md, and send worker_done.

## Item 5, now: merge integration (T10i, TRAM, T8i, T11-era fixes)
Merge `integration` into your branch and resolve the conflicts in `clip_engine.c/.h`, `engine.c`, `live_bridge.c/.h`, `ui.c` and `song_io_test.c`:
- **Keep T10i's role mute** (`CLIP_CMD_MUTE` and `CLIP_CMD_MUTE_CLEAR`, through the ring, ordered) with its own enum values. Your `CLIP_CMD_RELOAD` is gone, so check no stale value remains.
- **T10i's mute must use your staged `play[]` model:** the ISR never reads `song->clip[]`. Mute only suppresses and releases; it doesn't touch clip data.
- **Keep TRAM's shared scratch** (`song_store_scratch_acquire/release`) and T8i's songs ops in `song_io`. Your undoable clip create must not take the scratch while a songs operation holds it.
- **`live_bridge_grid()`** (T10i) must read the transport from the same place your staged model uses.
- After merging, run the full ladder on the merged branch:
  - `make unit unit-core scenarios screens-lint parity check fw fw-debug`;
  - `tools/check_loader.sh` on both packages;
  - `make fuzz-ui` if it exists.
  Re-seed only goldens that change because of the combination, listing each with its reason. Report the RAM numbers for both builds; the pool needs at least 8 KB spare.

Commit, update reports/T7b.md, and send worker_done.

## Re-review (merged branch): approve after these fixes. Safe to install.
1. **A recorded clip starts a 16th late after the close** (`ui_clip.c:458`). `launch_now` queues at `global_step+1` after the main loop sees the boundary. **Queue the commit so it lands at exactly `rec.close`:** use the clip engine's launch-at-sample or launch-at-step API (add one if needed, through the ring), armed before the boundary is reached. Test: close at step 32, and the clip starts at step 32, with its progress at step 48 equal to 16/32.
2. **Staging and the ring disagree** (`clip_engine.c:350-362`). **Stage only after `push_cmd` succeeds.** If it fails (ring full), roll back and return an error. An invalid launch must not clear `next_set` while a valid one is queued (`apply_role`, about `:427`). Restore `ring_many_launches.txt` so it ends with the last valid launch, and say why.
3. **A nest-safe IRQ mask** (`fm1_flash.h:233-234`, `engine.h:124`): `irq_save` returns the previous enable state and `irq_restore` restores it, rather than always running `sti`. Check the host/plat equivalents match. Add a host test for nesting.
4. **Undo re-inits the engine while it plays.** `song_reload` on undo must stage the restored clip through the same main-loop staging path as edits, not re-init the clip engine. Only song open or load (stopped) re-inits.
5. The drum grid shows 9 of 11 lanes. Add a small scroll indicator (lane N/11, or arrows) so you can see it scrolls.

Then run the full ladder on the branch (`make unit unit-core scenarios screens-lint parity check fw fw-debug`, `tools/check_loader.sh` on both), commit, update reports/T7b.md, and send worker_done.
