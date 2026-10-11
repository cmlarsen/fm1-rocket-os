# T4c: clip engine core

Core lane for T4 ([build-plan.md](../docs/engineering/build-plan.md#lanes-v3-2026-10-11)). Pure C in
new files only; T4i wires it into X0X's sequencer and the LIVE view. `core/model/model.h` is frozen
and unchanged. This revision applies the fixes in [T4c-review.md](T4c-review.md).

- Files: `core/seq/clip_engine.h`, `core/seq/clip_engine.c`, `tests/host/clip_engine_test.c`,
  `tests/golden/clip_engine/*.txt` (30 files), `Makefile` (`unit-clip`), this report.
- `make unit-clip` builds the test with `-Werror` and runs it, then rebuilds it under
  `-fsanitize=address,undefined` and runs it again. Both pass on the Mac.

## API summary

`#include "core/seq/clip_engine.h"` (only pulls in `core/model/model.h`; standalone).

```c
void clip_engine_init(clip_engine_t *e, song_t *song);
void clip_engine_start(clip_engine_t *e);                 /* transport start */
void clip_engine_transport_stop(clip_engine_t *e);        /* transport stop */
void clip_engine_launch(clip_engine_t *e, uint8_t clip);  /* at the clip's own quantize */
void clip_engine_launch_quant(clip_engine_t *e, uint8_t clip, uint8_t quant);
void clip_engine_launch_now(clip_engine_t *e, uint8_t clip);  /* HOME + pad: next step */
void clip_engine_stop(clip_engine_t *e, uint8_t role);        /* queue a role stop */
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
- `clip_engine_stop(e, role)` queues a role stop; `clip_engine_transport_stop(e)` stops the
  transport. The old public `clip_engine_set_bpm` is gone: a tempo change goes through
  `clip_engine_queue_bpm` (the ring) or by writing `song->bpm_x10`; the engine applies it at the
  next `render`.
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

- **A clip's kind is its role's, not its bytes.** `clip_role(clip) == ROLE_DRUMS` plays
  `d.drum`; every other role, FX included, plays `d.notes[0..nnotes)`. An unused clip (`used == 0`)
  cannot be launched: the launch is ignored and emits nothing.
- One clip per role at a time. A launch queues the clip on its role; a second launch of the clip
  that is already queued cancels the queue (`CLIPEV_CLIP_CANCELLED`); `clip_engine_stop` queues a
  role stop (`CLIPEV_CLIP_QUEUED` with clip `0xFF`), and a second stop cancels that queued stop
  (`CLIPEV_CLIP_CANCELLED`).
- Quantize is applied at the queued action's step: `Q_BAR` the next bar (16), `Q_BEAT` the next
  beat (4), `Q_END` the end of the role's clip (or the next bar if nothing plays). A role stop uses
  the playing clip's own quantize (the next bar if the role is idle). `launch_now` switches at the
  next step and does **not** cancel an already-queued clip. A queued launch while the transport is
  stopped starts at transport start (step 0).
- `CLIP_LOOP` loops; `CLIP_ONCE` plays once and stops the role. A clip that starts plays from its
  step 0.
- Notes carry their length in steps (`note_t.len`): a note is held across steps and released either
  at its end, or when its clip stops, switches, or wraps at the loop point. A retrigger of the same
  pitch cuts the old note first. Note-offs run before note-ons at the same sample.
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
sample that multiplication says and nothing drifts (checked to bar 1800, ~1 hour). Every division
is 32-bit (`f < bpm_x10 <= 3000`, `frac < 2*bpm_x10`); the 64-bit products (e.g. note length x
`step_L`) are 32x32->64 multiplies. Verified: `nm -u` of the host object and of the device object
are empty — no `__udivdi3`, no `double`, no libm.

**Tempo.** `song->bpm_x10` is the single source of truth. The UI writes it directly, or sends
`clip_engine_queue_bpm`, which (in the ISR) writes `song->bpm_x10` and applies it, so a ring tempo
is not reverted on the next block. On a tempo change the elapsed fraction of the current step is
rescaled to the new step length (`scaled = phase * new_L / old_L`), so the current step keeps its
phase and no step fires twice even for a large jump (60 -> 300 BPM mid-step is tested).

`CLIP_ENGINE_FS` defaults to `48000` and is overridable with `-DCLIP_ENGINE_FS=...`. **T4i must
set it to the rate of the sample counter the engine is rendered against** (the model says the
sample counter is the only clock; `firmware/src/seq/sequencer.h` still uses 44100 for its own
float tempo).

## Threading

The ring is a single-producer / single-consumer queue with `volatile` head and tail indexes and a
compiler barrier (`__asm__ volatile("" ::: "memory")`) around the head store (publish) and the head
load (consume). `push_cmd` returns `int` (0 = ok, -1 = full). `clip_engine_queued_clip` takes a
barrier-guarded snapshot of `que_set`/`que`. Everything else the ISR reads is single-byte or
engine-owned.

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
| `stop_role` / `stop_role_qbeat` | a role stop uses the clip's quantize (Q_BAR / Q_BEAT) |
| `stop_then_cancel` | a second stop cancels, and emits `CLIPEV_CLIP_CANCELLED` |
| `launch_while_stopped` | queued while stopped, starts at transport start |
| `launch_now` | HOME+pad switches at the next step |
| `launch_now_no_cancel` | HOME+pad on the queued clip launches now, does not cancel |
| `drum_clip_accent` | drum lane + accent bit |
| `drum_lane0_steps_0_24` | drum clip with lane-0 hits on steps 0 and 24 |
| `melodic_shortened` | a melodic clip shortened below its notes: out-of-range notes never fire |
| `unused_clip_ignored` | an unused clip cannot be launched and emits nothing |
| `chord_and_hold` | a chord at one step, and a note held across the loop point |
| `motion_lane` | motion values at their steps |
| `tempo_change_midclip` | a direct `song->bpm_x10` change retimes the following steps |
| `ring_tempo` | a ring tempo survives later render blocks (one source of truth) |
| `tempo_jump_midstep` | 60 -> 300 BPM mid-step: no step fires twice |
| `swing_drums` | an odd 16th is swung (66 -> 1980 samples at 120 BPM) |
| `ring_many_launches` | 40 launches one per block, then a 40-launch burst into the 16-deep ring |
| `counter_wrap` | a launch near `2^32` and the events across the sample-counter wrap |
| `one_frame_windows` | a full bar rendered one sample at a time |
| `all_five_roles` | five roles launched together |
| `queries` | playing / queued / progress for the UI |

Two checks in code (asserted, not golden): `drift_check` at 128.7 BPM (a fractional step,
`L = 5594, f = 522`) — bar 1 starts at sample 89510 and bar 1800 (~1 hour) at sample 161118881,
each equal to `floor(bars * 16 * STEP_NUM / bpm_x10)` in 64-bit; and `window_check` — the same
script rendered in blocks and one sample at a time produces byte-identical event streams.

## Device build (deltron.local, JieLi clang 4.0.1, pi32v2)

Compiled the way `tools/build.py` compiles X0X's seq units (`-mcpu=r3 -mfprev1 -ffp-contract=off
-ffunction-sections -fno-builtin -I.`):

| opt | `.text` | `.data`+`.rodata` | `.bss` | undefined syms |
|---|---|---|---|---|
| `-Os` | 2932 B | 0 B | 0 B | none |
| `-O2` | 3494 B | 0 B | 0 B | none |

The engine state is caller-allocated: `sizeof(clip_engine_t) = 440 B` (a `song_t` is 14352 B and
is owned elsewhere). No globals, no heap, no core-1 dependency.

## What T4i needs

- Own one `clip_engine_t` (440 B, put it in bss) and one `song_t`; `clip_engine_init(&eng, &song)`.
- Drive `clip_engine_render(&eng, nframes, &sink)` from the X0X render loop, in place of (or beside)
  `seq_*`. `nframes` is the audio block; the sink maps events onto X0X's engine commands.
- The UI calls in the list above are all safe from the main loop (they only write the ring). Query
  with `clip_engine_playing_clip` / `_queued_clip` / `_progress` / `_pos` for the key LEDs, the grid
  cells and the header.
- Set `CLIP_ENGINE_FS` to the sample counter's rate.
- `clip_engine_transport_stop` while playing releases all sounding notes in the next `render`.
- The key -> clip map and the pad layout are `song.key_clip[]` and `clip_role()`; the engine only
  needs the clip index. Launch only clips whose `used` is set.
- A clip's kind is its role's: make sure a DRUMS clip's payload is in `d.drum` and a melodic clip
  sets `nnotes` to the number of notes it uses.
