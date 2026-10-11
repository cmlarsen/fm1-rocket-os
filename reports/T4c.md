# T4c: clip engine core

Core lane for T4 ([build-plan.md](../docs/engineering/build-plan.md#lanes-v3-2026-10-11)). Pure C in
new files only; T4i wires it into X0X's sequencer and the LIVE view. `core/model/model.h` is frozen
and unchanged.

- Files: `core/seq/clip_engine.h`, `core/seq/clip_engine.c`, `tests/host/clip_engine_test.c`,
  `tests/golden/clip_engine/*.txt` (20 files), `Makefile` (`unit-clip`), this report.
- `make unit-clip` builds the test with `-Werror` and runs it, then rebuilds it under
  `-fsanitize=address,undefined` and runs it again. Both pass on the Mac.

## API summary

`#include "core/seq/clip_engine.h"` (only pulls in `core/model/model.h`; standalone).

```c
void clip_engine_init(clip_engine_t *e, song_t *song);
void clip_engine_start(clip_engine_t *e);         /* transport start */
void clip_engine_stop(clip_engine_t *e);          /* transport stop */
void clip_engine_set_bpm(clip_engine_t *e, uint16_t bpm_x10);
void clip_engine_launch(clip_engine_t *e, uint8_t clip);              /* clip's own quantize */
void clip_engine_launch_quant(clip_engine_t *e, uint8_t clip, uint8_t quant);
void clip_engine_launch_now(clip_engine_t *e, uint8_t clip);          /* HOME + pad: next step */
void clip_engine_stop_role(clip_engine_t *e, uint8_t role);
void clip_engine_queue_bpm(clip_engine_t *e, uint16_t bpm_x10);
void clip_engine_render(clip_engine_t *e, uint32_t nframes, const clip_sink_t *sink);

uint8_t clip_engine_playing_clip(const clip_engine_t *e, uint8_t role); /* CLIP_NONE = stopped */
uint8_t clip_engine_queued_clip(const clip_engine_t *e, uint8_t role);  /* CLIP_NONE = none */
uint8_t clip_engine_progress(const clip_engine_t *e, uint8_t role);     /* 0..255 */
void    clip_engine_pos(const clip_engine_t *e, uint8_t *bar, uint8_t *beat, uint8_t *step);
```

- `clip` is a clip index in `model.h`'s key-zone order; its role is `clip_role(clip)`.
- Each UI call is a write into a 16-entry single-producer / single-consumer ring; `render` reads
  it at the top. No locks, no heap. There is no UI-side state to keep in sync.
- `render` returns void; it advances `nframes` and calls `sink->ev` for every event in the window
  with its exact sample offset.

### Events (`clip_event_t { sample, kind, role, a, b, c }`)

| kind | a | b | c |
|---|---|---|---|
| `CLIPEV_NOTE_ON` | note | velocity 0..255 | clip |
| `CLIPEV_NOTE_OFF` | note | — | — |
| `CLIPEV_DRUM` | lane | accent 0/1 | clip |
| `CLIPEV_MOTION` | param low | param high | value |
| `CLIPEV_CLIP_STARTED` / `_STOPPED` / `_QUEUED` / `_CANCELLED` | — | — | clip (`0xFF` = a stop) |
| `CLIPEV_PLAYING` | 1 | — | — |
| `CLIPEV_STOPPED` | 0 | — | — |

## Behaviour

- One clip per role at a time. A launch queues the clip on its role; a second launch of the clip
  that is already queued cancels the queue (`CLIPEV_CLIP_CANCELLED`); `clip_engine_stop_role`
  queues a stop (`CLIPEV_CLIP_QUEUED` with clip `0xFF`).
- Quantize, applied at the queued action's step: `Q_BAR` the next bar (16), `Q_BEAT` the next beat
  (4), `Q_END` the end of the role's clip (or the next bar if nothing plays). `launch_now` uses
  `Q_BEAT`-style "next step". A queued launch while the transport is stopped starts at transport
  start (step 0).
- `CLIP_LOOP` loops; `CLIP_ONCE` plays once and stops the role. A clip that starts plays from its
  step 0.
- Notes carry their length in steps (`note_t.len`): a note is held across steps and released either
  at its end, or when its clip stops, switches, or wraps at the loop point. A retrigger of the
  same pitch cuts the old note first. Note-offs run before note-ons at the same sample.
- Motion lanes fire every step whose value is not `MOTION_NONE`, before that step's drum/notes.
- `swing` (0..100) delays an odd 16th by `step_L * swing / 200` samples, as X0X does. Bars and
  beats land on even 16ths, so quantize points are never swung.

## Fixed-point scheme (no drift, no 64-bit division)

A 16th lasts `STEP_NUM / bpm_x10` samples, with `STEP_NUM = CLIP_ENGINE_FS * 150`
(`= FS * 60 / 4 * 10`; `7,200,000` at 48 kHz). `L = STEP_NUM / bpm_x10` and `f = STEP_NUM %
bpm_x10` drive a Bresenham accumulator that keeps the fractional part in units of `1/bpm_x10`
sample:

```
turn:  frac += f;  carry = frac / bpm_x10;  frac %= bpm_x10;  sample += L + carry;
```

By induction `step_sample(k) = floor(k * STEP_NUM / bpm_x10)`, so bar N starts on exactly the
sample that multiplication says and nothing drifts. Every division is 32-bit (`f < bpm_x10 <=
3000`, `frac < 2*bpm_x10`); the 64-bit products (e.g. note length x `step_L`) are 32x32->64
multiplies. Verified: `nm -u` of the host object and of the device object are empty — no
`__udivdi3`, no `double`, no libm.

Tempo comes from `song->bpm_x10` (re-read each render) or a ring BPM command. A tempo change
re-anchors the fractional part (`frac = 0`) and leaves queued actions on their global step
numbers, so a pending launch keeps its quantize point.

`CLIP_ENGINE_FS` defaults to `48000` and is overridable with `-DCLIP_ENGINE_FS=...`. **T4i must
set it to the rate of the sample counter the engine is rendered against** (the model says the
sample counter is the only clock; `firmware/src/seq/sequencer.h` still uses 44100 for its own
float tempo).

## Tests

`tests/host/clip_engine_test.c`. Each case is a script run against a fresh song + engine; the
event stream is written as `sample kind role a b c` and compared with
`tests/golden/clip_engine/<case>.txt`. `GOLDEN_UPDATE=1` rewrites the goldens. `probe`/`query`
lines are written by the harness and included in the golden.

| case | what it pins |
|---|---|
| `smoke_melodic_loop` | launch while stopped, note on/off, loop retrigger |
| `quantize_bar` / `quantize_beat` | a launch lands on the next bar / beat |
| `quantize_end` | switch at the end of the playing clip |
| `once_clip` / `once_beat` / `once_end` | Once x each quantize; the role stops at the clip end |
| `queue_then_cancel` | relaunching the queued clip cancels it |
| `switch_midbar_qend` | mid-bar switch with Q_END; the old clip's notes are released |
| `stop_role` | a queued stop at the clip end |
| `launch_while_stopped` | queued while stopped, starts at transport start |
| `launch_now` | HOME+pad switches at the next step |
| `tempo_change_midclip` | a tempo change retimes the following steps |
| `swing_drums` | an odd 16th is swung (66 -> 1980 samples at 120 BPM) |
| `drum_clip_accent` | drum lane + accent bit |
| `chord_and_hold` | a chord at one step, and a note held across the loop point |
| `motion_lane` | motion values at their steps |
| `ring_many_launches` | 40 launches one per block, then a 40-launch burst into the 16-deep ring |
| `all_five_roles` | five roles launched together |
| `queries` | playing / queued / progress for the UI |

Plus `drift_check` (in code, asserted): at 128.7 BPM (a fractional step, `L = 5594, f = 522`) bar
1 starts at sample 89510 and bar 1800 (~1 hour) at sample 161118881, each equal to
`floor(bars * 16 * STEP_NUM / bpm_x10)` computed in 64-bit. Deterministic and platform-independent.

## Device build (deltron.local, JieLi clang 4.0.1, pi32v2)

Compiled the way `tools/build.py` compiles X0X's seq units (`-mcpu=r3 -mfprev1 -ffp-contract=off
-ffunction-sections -fno-builtin -I.`):

| opt | `.text` | `.data`+`.rodata` | `.bss` | undefined syms |
|---|---|---|---|---|
| `-Os` | 2880 B | 0 B | 0 B | none |
| `-O2` | 3352 B | 0 B | 0 B | none |

The engine state is caller-allocated: `sizeof(clip_engine_t) = 440 B` (a `song_t` is 14352 B and
is owned elsewhere). No globals, no heap, no core-1 dependency.

## What T4i needs

- Own one `clip_engine_t` (440 B, put it in bss) and one `song_t`; `clip_engine_init(&eng, &song)`.
- Drive `clip_engine_render(&eng, nframes, &sink)` from the X0X render loop, in place of (or beside)
  `seq_*`. `nframes` is the audio block; the sink maps events onto X0X's engine commands.
- The UI calls in the list above are all safe to call from the main loop (they only write the ring).
  Query with `clip_engine_playing_clip` / `_queued_clip` / `_progress` / `_pos` for the key LEDs,
  the grid cells and the header.
- Set `CLIP_ENGINE_FS` to the sample counter's rate.
- `clip_engine_stop` while playing releases all sounding notes in the next `render`.
- The key -> clip map and the pad layout are `song.key_clip[]` and `clip_role()`; the engine only
  needs the clip index.
- The clip-is-melodic test is "has a valid note in `rng`" (`step < len && len > 0`), empty `rng` =
  drum. If the model later marks clips explicitly, swap `clip_is_melodic`.
