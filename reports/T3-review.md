# T3 review (Opus): merge after fixes. NOT safe to install yet.

Coordinator decisions are in **bold**. Every fix below needs a scenario or unit test that fails before the fix and passes after it.

1. **Hung 303 on entering LIVE** (`ui_live.c:42`). `seq_stop` only sets `req_stop`, and `seq_advance` never runs again.
   - **On entry, stop X0X's sequencer synchronously:** clear `playing` and the requests, and release every open gate and voice right away, from the main loop with the audio IRQ masked, or by handing a "stop now + all off" flag to the ISR that it handles at the start of the next block.
   - Test: enter LIVE from a playing STUDIO at 10 different step offsets, and the WAV goes silent within 50 ms every time.
2. **The long press runs play/stop on the press** (`ui_live.c:69-72`). **A short press on PLAY now acts on release, if released before the long-press threshold (600 ms). Holding past the threshold switches the mode and never starts or stops either transport, entering or leaving.** Apply this in STUDIO and in LIVE. Test both directions. Then update `docs/product/interaction-model.md`: one line under Modes saying the short press acts on release.
3. **The audio ISR enqueues into the main loop's SPSC command queue** (`live_bridge.c:207-235`, `engine.c:112`). **In the ISR, call the voice functions directly, as X0X's sequencer `SINK` does (`s_drum`, `bass303_note_*`, direct param apply). Never call `cq_put` from the ISR.** Add an assert or comment-free guard in debug builds, so `cq_put` from ISR context traps on the host.
4. **Hung note from a key held across a layer or focus change** (`ui_live.c:104`, `:155`). **Track the held notes per key, whatever the current layer or focus. A key release always releases the note that key started.** Changing layer, focus or octave doesn't retarget held notes. Test SCL tap and ALGO turn with a key held: the note releases on key-up.
5. **Autosave during LIVE playback** (`ui.c:3941`). **Autosave and `save_wait` never run while LIVE is active, playing or not, and saving resumes after leaving LIVE once STUDIO is stopped and quiet as before.** Test: enter LIVE with a dirty project and play for 10 s, and no flash write happens (count `plat_store_save` calls on the host).
6. **Sample-accurate events** (`engine.c:668-680`). **Render in sub-blocks split at clip-engine event offsets,** or apply events at their sample offsets the way X0X's sequencer does, so events are no longer rounded to 256-sample blocks. The events golden must show non-multiple-of-256 timestamps.
7. **Bridge drops 303 slides** (`live_bridge.c:52-79`). Carry `BS_SLIDE` through to `bass303_note_on`'s slide flag. Add a bridge test for slides, a 64-step pattern (the second `hit` word), the 7-pattern cap, and the event-to-command mapping with real (non-stub) capture.
8. **Spec change without an ADR.** **Revert your edit to "Playing clips carry on across mode changes" in `interaction-model.md`.** Add an entry to `docs/open-questions.md` saying that in this build, leaving LIVE hands control back to X0X's sequencer, so clips don't carry on, and that the owner decides whether that's acceptable until T6i/T7. Say the same in reports/T3.md.
9. **Screens:**
   - "DRUM" and "CHRD" collide with the first cell (`ui_live.c:271`): use 2-letter role labels (DR BA LE CH FX) or move the cells right, with at least 3 px clearance.
   - "octave 0" is clipped at the band edge (`ui_live.c:332`): move it fully inside a band.
   - The empty knob strip: show K1–K4's real LIVE function, or the four role levels, or remove the strip and give the space to the pad grid. Follow `Main.dc.html`.
   - Re-seed the LIVE PNG goldens and list them.
10. Add `_Static_assert(CLIP_ENGINE_FS == FS, …)` where both are visible.

Then run `make unit unit-core scenarios screens-lint parity check fw` and `tools/check_loader.sh build/x0x.fwsc`, commit, update reports/T3.md, and send worker_done.
