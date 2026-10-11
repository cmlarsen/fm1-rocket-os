# T9c: take recorder and player core

Core lane for T9 in [build-plan.md](../build-plan.md#lanes-v3-2026-10-11). **New files only**; T9i wires it into the engine tap, the LIVE view and storage later.

Read first: `CLAUDE.md`, the worker rules in `docs/engineering/build-plan.md`, `docs/engineering/model.md` (including **Command words**) and `core/model/model.h` (frozen; use your `ask` command for changes), `docs/product/interaction-model.md` (the take: one per song, REC records, hold REC clears, undo restores), and `core/seq/clip_engine.h` (the clip engine's events; the take must replay through the same paths).

**Ownership (new files only):** `core/seq/take.{c,h}`, `tests/host/take_test.c`, `tests/golden/take/`, and a `make unit-take` target appended to the Makefile (keep the existing Makefile content; just add your target).

## Behaviour
- **Recorder.** `take_arm(t, now)` starts recording at the next bar boundary (the caller passes bar boundaries in, or a function to query them). Then `take_feed(t, sample, cmd)` appends an event stamped with `sample - start`. It's called from the engine tap (one call per drained command, ISR context: constant time, no heap). `take_stop(t, now)` ends the take at the next bar boundary so loops line up. When the buffer is full (`TAKE_MAX_EVENTS`), recording stops cleanly with a `TAKE_MARK end` and a `full` flag the UI can show.
- **Prelude.** At the start of recording, the caller supplies the current state through a callback that writes setup events at offset 0: the playing clip per role (`TAKE_LAUNCH now`), mutes, tempo, and the current value of every parameter. Replay then starts from the same state.
- **Player.** `take_play(t, now)` replays events through a sink at exactly `start + offset` samples, render-window style like `clip_engine_render` (exact offsets inside each window, any window size). `take_stop_play`. During playback, LIVE input is ignored by the caller, not by this module.
- **Event-exact:** recording the events of a scripted session, then replaying them, produces the identical (sample, cmd) stream relative to the start.
- Header: `take_hdr_t` holds the event count, tempo and length in samples.

## Tests (write first)
Each test passes under `-fsanitize=address,undefined`:
- record and replay round trip, event-exact, with random windows;
- arm and stop on bar boundaries;
- the prelude restores the state;
- a full buffer;
- the 32-bit sample counter wrapping during a take;
- a 2,048-event take with events on every sample of a window;
- replay through a real `clip_engine` instance from `core/seq/clip_engine.c`: record its launches, replay them into a second engine, and require identical clip-engine event lists.

Compile the new files for the device on deltron and report the code and RAM sizes.

## Done
- `make unit-take` passes on the Mac.
- Small commits with the attribution lines from `CLAUDE.md`'s instructions.
- `reports/T9c.md` describes the API, what T9i must wire (the engine tap, the prelude callback, storage through `song_store` take functions), and the test list.
- `worker_done` with `--outcome`, `--files-modified`, `--report-path reports/T9c.md`.

## Notes for T9i (from the T2a review)
- `engine_tap` stamps each drained command with the sample at the **end** of the block (`eng_prof_frames` is advanced before `drain()`). Pass `eng_prof_frames - n` instead, so events carry the block-start sample where they take effect.
- Cue and mute changes don't go through the engine command queue. Record launches and mutes from the clip engine's events (`TAKE_LAUNCH`, `TAKE_MUTE`), not from the tap.
