# T7b: clip editors and recording into clips

Integration lane, after T7a. T10i (punch FX) runs in parallel and owns `ui_live.c` plus the master path in `engine.c`. Stay out of both.

Read first:
- `CLAUDE.md`;
- `docs/product/interaction-model.md` (STUDIO tasks: New clip, Edit a clip, Record knob motion);
- `docs/product/screens.md` (STUDIO 3, 4 and 15);
- the design in `design/project/Studio-ClipSteps.dc.html`, `Studio-ClipDrums`, `Studio-ClipMotion`, `Studio-EmptyClip`, plus the components and `fm1.css`;
- `reports/T7a.md`, `firmware/src/app/ui_studio.c`, `song_io.*`, `core/seq/clip_engine.h`, `core/model/model.h`.

**Ownership:** new `firmware/src/app/ui_clip.c/.h` (the editors); `ui_studio.c` (entering and leaving the editors); `song_io.*` (edit and undo entry points); clip-engine hooks for recording; new tests and scenarios. Use `ui.c` only for routing if you must.

## Build
1. **SEQ cycles NOTES (or the DRUM grid) → MOTION → SETTINGS → back to the song home,** for the clip under the cursor. SEQ on an empty slot creates the clip and opens NOTES or DRUM.
2. **STUDIO 4, the drum grid**, for DRUMS clips: 11 lanes × the clip length.
   - The white keys are steps 1–16 of the visible page; the black keys pick the lane, X0X-style.
   - OCT− and OCT+ page through the steps for clips longer than 16.
   - Tapping a white key toggles the step on the selected lane. HOME + white key toggles the accent.
   - The playhead shows while playing, and the lane names follow the 909 order.
3. **STUDIO 3, notes**, for melodic roles: a piano roll with a step cursor and the STEP, NOTE, VEL and LEN cards on K1–K4.
   - ALGORITHM moves the step cursor. Keys enter notes at the cursor: a press adds a note and its length defaults to 1 step.
   - Holding several keys adds a chord on the same step, up to `CLIP_MAX_NOTES`.
   - HOME + key removes that note at the cursor. OCT± shifts the octave.
4. **STUDIO 15, motion:** two lanes per clip.
   - Turning K1–K4 while the clip records, or while on this page with REC armed, writes motion values at the current step for the param that knob controls. The first two distinct params claim lane 0 and lane 1; a third shows "2 lanes max".
   - The page draws each lane's curve.
   - HOME + touching a knob clears its lane.
   - Motion values play back through the clip engine's motion events.
5. **REC on an empty slot (STUDIO 2)** arms recording from the next bar. Notes played on the keys (and the knob motion) record into a new clip until REC is pressed again. That second press closes the loop at the next bar boundary, sets the length to the recorded bars (1, 2 or 4, rounded up), and starts the clip looping.
   - REC on a loaded clip overdubs, adding notes, until REC is pressed again.
   - Quantize the recorded notes to the 16th grid.
6. **Undo:** each editing session, and each record pass, is one undo entry, through `song_io`.
7. **Editing a playing clip** must be ISR-safe. The audio ISR must never read a half-written `clip_t`: use the clip engine's ring or a double-buffer swap at a step boundary. Test it with edits while playing.

## Tests (write first)
Scenarios with goldens (PNG at 1:1, dump and events) for:
- a drum grid edit, with the edited hits played back as events;
- 4 notes stepped in, plus a chord;
- a long clip paged with OCT;
- motion recorded and played back;
- REC on an empty slot for 1 bar (the loop closes and plays);
- overdub;
- undo of a record pass;
- edits made while the clip plays (no torn reads; the event stream stays sane).

Run `make unit-core scenarios screens-lint parity check fw` and `tools/check_loader.sh build/x0x.fwsc`. List the new PNGs.

## Done
- Small commits with the attribution lines.
- `reports/T7b.md`.
- `worker_done`.
