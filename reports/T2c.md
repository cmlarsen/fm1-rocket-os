# T2c: UI fuzz for hung notes and crashes

Enforces the "no hung notes" invariant in
[architecture.md](../docs/engineering/architecture.md#invariant-no-hung-notes-2026-10-11).
Lane [T2c](../docs/engineering/tasks/T2c.md), from `integration` (includes T7a's round 2).
No hardware was flashed; `make fw` only builds (on `deltron.local`).

## What landed

- **`host/x0x_host.c` — `fuzz SEED N`.** N random input events from a deterministic xorshift32
  PRNG: key down / up over a real held-key model, button tap / press / release, button holds
  (long PLAY, SEL/SCL, HOME, SEQ, EDIT, GLO, SAVE, FX, REC and the rest), encoder turns and
  `spin`s, and waits of 0–300 ms. It starts at the song home and a fixed prologue guarantees
  visits to the sound pages, Classic and LIVE before the random events. It never sends a factory
  reset (a host-only guard in `ui.c`'s `ACT_RESET`) and writes flash only through the normal
  save / autosave path.
- **`expect voices 0`.** `engine_voices_active()` (engine.c) sums the sounding voices of the
  909 (eleven), the 808 (one per lane), the two 303s (held or still sounding), T5i's FM6 and
  NOISE; the host's `expect` gained the `voices` key. A `stop` command stops both transports,
  aborts a running PERF test and releases every voice, so a reproduction and the fuzz run leave
  the app in the same state.
- **Release tail.** When the sequence ends the command releases every key and button still held,
  `stop`s the transport, waits 3 s and requires zero voices. The runner's crash check is the
  driver's: a trap (`__builtin_trap`, the `cq_put`-from-ISR invariant) or a signal exits non-zero.
- **`tests/fuzz/`.** `run_fuzz.sh` runs seeds 1..SEEDS and minimises any failure;
  `shrink.py` ddmin-deletes event lines while the host still fails and writes the minimised,
  ready-to-run `.x0x`. On failure the command first writes the whole expanded sequence to
  `build/fuzz/seed_<S>.full.x0x` (before running it, so a crash still leaves a reproduction).
- **`make fuzz-ui`** runs 200 seeds × 2,000 events. **`make check`** runs the short budget,
  20 × 500 (its `check` target now depends on `host` too).
- **`tests/scenarios/voices.x0x`** exercises the new `expect voices 0` key in the normal scenario
  suite (a sound-page note released, and a key held across a view change).

## Design notes

- **Determinism.** The event list is a pure function of the seed (no app state in the PRNG), and
  the reproduction replays the same commands line for line. On the host the overload guard sees
  0 % (`load_force` is unset), so wall-clock render timing never changes the DSP — repeat runs are
  identical. The `noreset` line the reproduction starts with keeps the factory-reset guard on for
  the replay too, so the two runs cannot diverge.
- **Ownership.** No UI bug is fixed here (the only `ui.c` change is the host-only factory-reset
  guard). Failing seeds go to the coordinator with their minimised scenario.
- **Device build** (`make fw` on deltron.local, JieLi clang 4.0.1, pi32v2): image **514,432 B**;
  RAM `.data+.bss` **95,824 B** of 98,304; POOL **292,132 B** of 344,064; package 609,680 B
  (identity FM-1_900) — the new voice-count accessors are dropped by the linker on device, so the
  sizes are unchanged from T7a.

## Results

- **`tests/fuzz/run_fuzz.sh 200 2000` (the `make fuzz-ui` budget).** Seeds 1–200 at 2,000 events
  each: **all passed**. No seed left a voice sounding after the 3 s release tail and none crashed
  (no `cq_put`-from-ISR trap). **No failing seeds to route.**
- **`tests/fuzz/run_fuzz.sh 20 500` (the `make check` budget).** All passed.
- **`tests/scenarios/voices.x0x`**: passes (`expect voices 0` after each release tail).
- **Regression:** `make unit`, `make scenarios` (all scenarios, goldens unchanged) and `make fw`
  pass; `make fw` reports the unchanged device sizes above.

The fuzzer did **not** reproduce T7a's hung-note defects: the one held-note table in the key path
(and the view/mode-change releases added in T7a round 2) hold under random key/button/encoder
input across STUDIO, LIVE, Classic and the sound pages. If a future change reintroduces a hang,
`make check` runs the short fuzz and `make fuzz-ui` the long one, both leaving a runnable
reproduction in `build/fuzz/`.

### Reproducing a failure by hand

A failure prints its seed and writes two files:

```sh
build/host/x0x_host build/fuzz/seed_<S>.x0x build/fuzz        # the failing seed (fuzz SEED N)
build/host/x0x_host build/fuzz/seed_<S>.min.x0x build/fuzz    # the minimised reproduction
```

`seed_<S>.full.x0x` is the whole expanded sequence; `seed_<S>.min.x0x` is the same failure with
event lines deleted until they cannot be removed without the run passing.

## Limits

- The fuzzer drives buttons, keys and encoders only: it does not touch MIDI in, the USB path or
  the take recorder (T7b/T9), which have their own tests.
- `make fuzz-ui` is the long run (about 15–20 minutes on a Mac); `make check` keeps the same
  invariant at 20 × 500.
