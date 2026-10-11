# T9i: the take, recorded in LIVE

Integration lane. T7b (clip engine staging, ending), T11i (MIDI) and T11h (help) run in parallel. **You own `ui_live.c`'s REC handling, the take hooks in `engine.c`, and `song_io`'s take calls.** Merge `integration` before you finish.

Read first:
- `CLAUDE.md`;
- the invariants in `docs/engineering/architecture.md` (no hung notes, the RAM plan);
- `docs/product/interaction-model.md`, "Recording the take" (one take per song; REC records the session from the next bar; REC again stops; hold REC 1.5 s clears; undo restores);
- `docs/product/screens.md` (LIVE 4, 8 and 10);
- `design/project/Live-Pads-Recording.dc.html`, `Live-ClearTake.dc.html`, `Live-Takes.dc.html`;
- `reports/T9c.md` (the recorder and player API, with the T9i notes at the end of `docs/engineering/tasks/T9c.md`);
- `docs/engineering/model.md` (command words, `take_t` size, undo limits);
- `firmware/src/app/engine.c` (`engine_tap`, `live_apply`, the clip engine events);
- `song_io` (`song_io_take_save/load`, `take_restore`).

## Build
1. **Recording.** REC in LIVE arms `take_arm` at the next bar.
   - The prelude writes the current state: the playing clip per role, mutes, tempo and every parameter value.
   - Every engine command drained in the audio path, plus clip launches and stops (`TAKE_LAUNCH`), mutes (`TAKE_MUTE`) and punch FX engage/release (`TAKE_FX`), is fed to `take_feed` from the ISR, stamped with the **block-start sample plus offset** (see the T9i notes).
   - REC again stops at the next bar.
   - A full buffer stops cleanly and shows "take full".
   - The header shows a red REC chip and the elapsed bars.
2. **Saving.** When recording stops, the take is saved with `song_io_take_save`, deferred until the transport is stopped and quiet, under the normal save rules. It replaces the song's previous take, which stays recoverable once through undo (T6c's durable take undo). `song.has_take` is set.
3. **Clearing.** Holding REC for 1.5 s in LIVE with nothing recording shows the clear-take ring (LIVE 8). On completion the take is cleared, and undo restores it.
4. **Playback (LIVE 10).** LIVE SETUP isn't built yet, so for now **EDIT in LIVE opens the take card.** It shows the take's length and bars, with SEL to play and stop.
   - Playback drives the same engine paths through `take_play`.
   - LIVE input is ignored while the take plays, apart from stop.
   - When the take ends, the transport stops and everything goes all-off.
5. **Undo:** HOME+REC in LIVE restores the previous take, after a replace or a clear.

## Tests
Event-exact tests:
- record a session (launches, mutes, a punch FX, knob moves), stop, replay, and the replayed event stream equals the recorded one relative to the start;
- record, reboot, then replay is still exact;
- a full buffer;
- clear plus undo;
- replace plus undo;
- REC while the transport is stopped (arms, then starts at the first bar after PLAY);
- no hung notes when a take ends mid-note, or when you leave LIVE during playback.

Plus screen goldens for LIVE 4, 8 and 10. Run `make unit unit-core scenarios screens-lint parity fuzz-ui check fw fw-debug` and `tools/check_loader.sh` on both packages. RAM: the take buffer (about 16 KB) must fit the pool with at least 8 KB spare. Report the numbers. If it doesn't fit, ask before shrinking anything.

## Done
- Small commits with the attribution lines.
- `reports/T9i.md`.
- `worker_done`.
