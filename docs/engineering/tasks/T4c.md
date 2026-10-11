# T4c: clip engine core

Core lane for T4 in [build-plan.md](../build-plan.md#lanes-v3-2026-10-11). Pure C in **new files only**; it is wired into X0X's sequencer and UI later (T4i).

Read first: `CLAUDE.md`, the worker rules in `docs/engineering/build-plan.md`, `docs/engineering/model.md` and `core/model/model.h` (frozen: do not change it; if you need a change, use your `ask` command), `docs/product/interaction-model.md` (sections on launching, quantize, Loop/Once), and X0X's `firmware/src/seq/sequencer.c` for how its ISR-side code and sink callbacks are shaped.

**Ownership (new files only):** `core/seq/clip_engine.c`, `core/seq/clip_engine.h`, `tests/host/clip_engine_test.c`, `tests/golden/clip_engine/`, and a `make unit-clip` target appended to the Makefile.

## Behaviour
- The engine plays a `song_t`. Each role plays at most one clip at a time.
- `clip_engine_launch(e, clip)` queues the clip on its role (`clip_role()`); `clip_engine_stop(e, role)` queues a stop. Queued changes take effect at the clip's quantize point: `Q_BAR` the next global bar boundary, `Q_BEAT` the next beat, `Q_END` the end of the role's current clip (the next bar if nothing plays). Launching the clip that is already queued cancels the queue. Launching while the transport is stopped makes it start at transport start. A `launch_now` variant (HOME + pad) switches at the next step.
- `CLIP_LOOP` clips loop; `CLIP_ONCE` clips play once and then the role stops. A clip that starts plays from its step 0.
- Transport: start, stop, bpm (`bpm_x10`) and swing from the song. Time is the audio sample counter only. Compute step boundaries with integer fixed point that never drifts over hours (no `double`, no 64-bit division; 32×32→64 multiply is fine) and is bit-identical on every platform.
- Output: `clip_engine_render(e, nframes)` advances by `nframes` samples and calls the sink for every event in that window with its exact sample offset: note on (role, note, vel), note off, drum hit (role, lane, accent), motion value (role, param, value), clip started / stopped / queued (for LEDs and takes). Note lengths are in steps; notes still sounding are released when a clip stops or switches.
- Threading: UI calls (`launch`, `stop`, `bpm`) may come from the main loop while `render` runs in the audio ISR. Pass them through a small single-producer / single-consumer ring read at the start of `render`. No locks, no heap.
- Query functions for the UI: playing clip per role, queued clip per role, progress through the current clip (0–255), current bar / beat / step.

## Tests (write them first)
`tests/host/clip_engine_test.c` builds with the host cc. Each case writes the event list as text (`sample kind role a b`) and compares it to a golden in `tests/golden/clip_engine/` (`GOLDEN_UPDATE=1` rewrites goldens). Cases, at least: each quantize × Loop/Once; queue then cancel; switch clip mid-bar with Q_END; stop role; launch while stopped; launch_now; tempo change mid-clip; swing; a drum clip; a melodic clip with a chord and a note held across the loop point; 1 hour of transport with no drift (bar N starts at exactly the computed sample); the ring under many launches per block. Also run the test under `-fsanitize=address,undefined`.

## Done
- `make unit-clip` passes on the Mac. The files also compile for the device on deltron (with the JieLi toolchain, like `build.sh` compiles X0X's seq files); report code and RAM size.
- Small commits with the attribution lines from `CLAUDE.md`'s instructions.
- `reports/T4c.md`: API summary, the fixed-point scheme, test list, anything T4i needs.
- `worker_done` with `--outcome`, `--files-modified`, `--report-path reports/T4c.md`.
