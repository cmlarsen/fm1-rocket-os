# T9c: take recorder and player core

Core lane for T9 ([build-plan.md](../docs/engineering/build-plan.md#lanes-v3-2026-10-11)). New files
only; T9i wires it into X0X's engine tap, the LIVE view and storage later. `core/model/model.h` is
frozen and unchanged. Nothing was flashed. This revision applies the fixes in
[T9c-review.md](T9c-review.md).

- Files: `core/seq/take.h`, `core/seq/take.c`, `tests/host/take_test.c`,
  `tests/golden/take/*.txt` (9 files), `Makefile` (`unit-take`), this report.
- `make unit-take` builds the test with `-Werror` and runs it, then rebuilds it under
  `-fsanitize=address,undefined` and runs it again. Both pass on the Mac.
- A take is a `take_hdr_t` plus up to `TAKE_MAX_EVENTS` (2,048) `take_event_t {sample, cmd}`,
  where `sample` is the offset from the take's start and `cmd` is X0X's 32-bit engine command
  word. The state is caller-allocated: `sizeof(take_t) = 16,436 B` on the pi32v2 device (16,456 on
  a 64-bit host). No heap.

## Command words (model.md, frozen v1)

`type:4 | a:8 | b:8 | c:12`. Types 0–11 are X0X's (`engine.h`); a take adds 12–15. `take.h`
defines the encoding and the take-only helpers:

| type | meaning | `a` | `b` | `c` |
|---|---|---|---|---|
| 12 | `TAKE_LAUNCH` | clip | 0 queue / 1 now / 2 stop role | role |
| 13 | `TAKE_MUTE` | role | 0/1 | — |
| 14 | `TAKE_FX` | punch FX id | 0 off / 1 on / 2 latch | — |
| 15 | `TAKE_MARK` | 0 start / 1 end / 2 tempo | value high | value low |

`TAKE_MARK_TEMPO(bpm)` puts `bpm_x10` over `b:c` (`TAKE_MARK_TEMPO_GET(b, c)` reads it back).
Parameter events (types 0–7) are written by the caller through the prelude; the take does not
interpret them.

## API

`#include "core/seq/take.h"` (only pulls in `core/model/model.h`; standalone).

```c
void take_init(take_t *t);
void take_set_bar_fn(take_t *t, take_bar_fn fn, void *ctx);   /* the caller's bar grid */
void take_set_prelude(take_t *t, take_prelude_fn fn, void *ctx);
void take_set_bpm(take_t *t, uint16_t bpm_x10);

/* recorder */
void take_arm(take_t *t, uint32_t now);                        /* start at the next bar boundary */
void take_feed(take_t *t, uint32_t sample, uint32_t cmd);      /* ISR: one drained command */
void take_stop(take_t *t, uint32_t now);                       /* request the end at the next bar */
void take_tick(take_t *t, uint32_t now);                       /* every block: arm/stop, writes END */
void take_setup_event(take_t *t, uint32_t cmd);                /* offset-0 event, from the prelude */

/* player */
void take_play(take_t *t, uint32_t now);                       /* reset the cursor, start replaying */
void take_render(take_t *t, uint32_t nframes, const take_sink_t *sink);  /* the per-window pump */
void take_stop_play(take_t *t);

/* queries */
int take_state(const take_t *t);      /* TAKE_IDLE / _ARMED / _REC / _PLAY */
int take_recording(const take_t *t);
int take_armed(const take_t *t);
int take_playing(const take_t *t);
int take_full(const take_t *t);       /* the buffer filled: the UI can show it */
int take_has(const take_t *t);        /* a take was recorded */
uint16_t take_nevents(const take_t *t);
const take_hdr_t *take_header(const take_t *t);     /* nevents, bpm_x10, length_samples */
const take_event_t *take_events(const take_t *t);   /* the raw log, for storage */
```

- `take_bar_fn(ctx, sample)` returns the first bar boundary at or after `sample`. The sequencer
  knows transport alignment, so it supplies this; with none set, `sample` is itself treated as
  the boundary (the caller passes a boundary in).
- `take_prelude_fn(ctx, t)` is called once inside `take_arm` and writes the current state at
  offset 0 through `take_setup_event()`.
- `take_sink_t.ev(ctx, sample, cmd)` is the playback sink: `sample` is the offset into the window
  passed to `take_render()`, exactly like `clip_engine_t`'s events.

## Behaviour

- **Recorder.** `take_arm(t, now)` clears the log, writes `TAKE_MARK start`, runs the prelude and
  records `start`, the next bar boundary. `take_feed(t, sample, cmd)` advances the state machine
  and appends `{sample - start, cmd}` while recording; a feed stamped before `start` is **dropped**
  (not clamped), because the prelude already set the state at offset 0. `take_stop(t, now)` only
  *requests* the end at the next bar boundary (it stores `stop_at` and a flag); the ISR's
  `take_tick(t, now)` is what runs the close, writing `TAKE_MARK end` and `length_samples` exactly
  once. `take_tick` also advances the arm-to-recording transition, so it can close a take even when
  no command drains. An arm stopped before its boundary cancels and leaves no take.
- **Prelude.** `take_arm` runs the caller's callback in main-loop context (not the ISR): the
  playing clip per role (`TAKE_LAUNCH now`), the mutes, the tempo, and the current value of every
  parameter. If the prelude writes a `TAKE_MARK tempo`, the header's `bpm_x10` follows it, so the
  header and the log cannot disagree.
- **Player.** `take_play(t, now)` resets the replay cursor and starts; `take_render(t, nframes,
  sink)` advances the replay clock by `nframes` and calls the sink for every event in that window
  with its exact offset, then moves past it. Any window size produces the same firing times because
  the log is sorted by offset; an event exactly on a window boundary goes in the next window.
  `take_stop_play` ends playback and leaves the log intact for another pass.
- **Full buffer.** The last slot is reserved for the end mark. When a feed would leave no room,
  the take closes with `TAKE_MARK end` at that offset, sets `full`, and `take_state` returns to
  `TAKE_IDLE`; later feeds are ignored. A full take holds 2,048 entries (2,047 recorded + the end
  mark).
- **32-bit wrap.** Offsets and `length_samples` are unsigned `sample - start`, so a take recorded
  across the audio counter's wrap is contiguous and replays the same.
- **LIVE input** during playback is the caller's policy, not this module's.

## Threading (ISR safety)

The main loop only builds and requests; the ISR only appends and closes. Each handoff publishes
data, then a `__asm__ volatile("" ::: "memory")` compiler barrier, then the flag:

- `take_arm` builds the log and runs the prelude while `state == IDLE`, so an ISR `take_feed`
  cannot interleave with the offset-0 events; it writes `start`, then the barrier, then
  `state = ARMED/REC`. Only `take_feed` does `n++` once the take is armed.
- `take_play` resets `play_i`/`play_pos`, then the barrier, then `state = PLAY`.
- `take_stop` writes `stop_at`, then the barrier, then `stop_set`; the ISR's `take_tick` is the
  only writer of the end mark, and an `ended` guard makes `take_end` idempotent.
- `take_feed` is constant time and never allocates.

## Event-exactness

`take_feed` stamps each event with `sample - start`, and the log is append-only in feed order.
`take_render` delivers the log in exactly that order at the same offsets for any windowing, so
recording a scripted session and replaying it produces the identical `(sample, cmd)` stream. The
test proves this by replaying in random-width windows and comparing the reconstructed stream with
the log, and by recording a real `clip_engine` session and replaying it into a second engine that
must emit an identical clip-engine event list.

## Tests (`tests/host/take_test.c`, goldens in `tests/golden/take/`)

`GOLDEN_UPDATE=1` rewrites the goldens. `make unit-take` runs the whole set twice, once with
`-fsanitize=address,undefined`.

| case | what it pins |
|---|---|
| `record_replay` | a take with a bar-grid arm, pre-start events dropped, and a random-window replay that is event-exact (asserted and written as a golden); a second `take_play` pass is identical |
| `bar_boundaries` | arm lands on the next bar, stop lands on the next bar with `length_samples`; a cancel leaves no take; arming exactly on a boundary records at offset 0 |
| `prestart_drop` | a feed stamped before `start` is dropped, the log stays sorted, and the surviving events keep their offsets |
| `prelude_restore` | the prelude's playing clips, mutes, tempo and parameters restore the identical state on replay; the header tempo matches; a feed attempted *during* the prelude build is ignored (`bad_feed_present=0`) and no prelude event is lost |
| `full_buffer` | feeding past the cap stops cleanly with `TAKE_MARK end`, `full = 1`, 2,048 entries in order, and replays exactly |
| `counter_wrap` | events before and after the `2^32` wrap get the right offsets and `length_samples` |
| `dense_window` | a take with an event on every sample replays exactly in a sequence of one-frame windows (at most two events share sample 0: the start mark and the first event) |
| `stop_once` | a main-loop `take_stop` while feeds continue writes exactly one `TAKE_MARK end`, and feeds after it are ignored |
| `clip_engine_parity` | record a real clip-engine session's launches, tempo and stops; replay them into a second engine; the two engines' event lists are identical (47 events) |

## Device build (deltron.local, JieLi clang 4.0.1, pi32v2)

Compiled as `tools/build.py` compiles X0X's units (`-target pi32v2 -mcpu=r3 -mfprev1
-ffp-contract=off -ffunction-sections -fno-builtin -I.`):

| opt | `.text` | `.rodata` | `.data` | `.bss` | undefined syms |
|---|---:|---:|---:|---:|---|
| `-Os` | 832 B | 0 | 0 | 0 | none |
| `-O2` | 978 B | 0 | 0 | 0 | none |

No globals, no heap, no `double`, no 64-bit division (the `nm -u` of both the device object and the
host object is empty, so no `__udivdi3` and no libc). The take state is caller-allocated:
`sizeof(take_t) = 16,436 B` on the device (`hdr` 8 + 2,048 × `take_event_t` 8 + bookkeeping;
16,456 on a 64-bit host, whose pointers are wider). No core-1 dependency.

## What T9i must wire

1. **The engine tap.** Call `take_feed(t, sample, cmd)` once per drained engine command, in the
   ISR, in the drain loop that already sees X0X's command queue. Record clip launches as
   `TAKE_LAUNCH` (queue/now/stop + `clip_role()`), mutes as `TAKE_MUTE`, punch FX as `TAKE_FX`,
   and tempo as `TAKE_MARK tempo`. `sample` is the audio sample the command is applied on.
2. **The bar grid.** `take_set_bar_fn()` with a function that returns the next bar boundary from
   the running transport (the same grid `clip_engine_pos`/`global_step` uses), so REC arms and
   stops line up. `take_arm(t, now)` on the REC tap; `take_stop(t, now)` on the second tap (or
   the hold-to-clear path), and a 1.5 s hold clears the take (`take_init`). **Call `take_tick(t,
   now)` once every block** (main loop or ISR): it advances arm-to-recording and writes the END
   mark at the stop boundary, so a take closes even when no command drains or the transport is
   idle. Stamps fed before the take's `start` are dropped by the module, so a block-start stamp is
   safe.
3. **The prelude callback.** `take_set_prelude()` with a function that writes what is playing
   now: `TAKE_LAUNCH(clip, TAKE_LAUNCH_NOW, role)` for each playing role (or `TAKE_LAUNCH_STOP`
   for a stopped one), `TAKE_MUTE(role, on)` per role, `TAKE_MARK_TEMPO(bpm)`, then the current
   value of every parameter as its own command word (X0X's types 0–7). The take runs it at arm,
   so arm at the moment that state is current; queued actions issued after arming but before the
   boundary are not part of the take (the log's zero is the boundary).
4. **Playback.** `take_play()` then `take_render(t, nframes, sink)` from the render loop; the sink
   maps each command back onto X0X's engine queue and the clip engine (`TAKE_LAUNCH` →
   `clip_engine_launch` / `_now` / `_stop`, `TAKE_MARK tempo` → `clip_engine_queue_bpm`, mutes and
   parameter targets onto `engine_set`). Ignore LIVE input while `take_playing()`. Playback from
   LIVE SETUP → Take.
5. **Storage.** Persist through T6c's `song_store` take functions (`take_save`/`take_load(slot,
   …)`, with the previous take kept once for undo). The take bytes are exactly
   `take_header()` + `take_events()[0..nevents)`; a take is 16 KiB at most. `song_t.has_take`
   says whether one exists. The undo of "record a take" replaces the stored take; clips are never
   touched.
6. **UI.** `take_recording()` drives the solid-red REC and the red border; `take_full()` the
   buffer-full warning; the header's `length_samples`/bpm for the take row.

## Notes for the coordinator

- **Makefile.** This branch's `Makefile` already had `unit-clip` and `unit-engines` (T4c + T5c);
  this change only appends `unit-take` and its `.PHONY`, keeping the rest byte-for-byte.
- **`take_play` vs the pump.** The task names `take_play(t, now)`; that resets the cursor and
  starts playback. The per-window work (the `clip_engine_render` equivalent) is `take_render(t,
  nframes, sink)`, because a window needs an `nframes` and a sink to be render-window style.
- **Prelude context.** The prelude runs inside `take_arm`, which is a main-loop call, so
  `take_feed` stays constant time in the ISR. The log and the prelude are built while
  `state == IDLE`, so an ISR feed cannot interleave; the trade-off (the prelude reflects the arm
  moment, not the boundary) is noted in point 3 above.
- **Review fixes (this revision).** Pre-start feeds are dropped (`take_feed`); `take_arm` builds
  under `state == IDLE` and publishes `start` then the state behind a barrier; `take_play` publishes
  its cursor behind a barrier; `take_stop` only requests the end and the ISR's `take_tick` writes
  the END exactly once (guarded by `ended`); the unused `play_start` is gone; `dense_window` now
  replays in one-frame windows, distinct from `full_buffer`.
- **Nothing inherited was touched**; every new file carries `SPDX-License-Identifier: GPL-3.0-only`.
