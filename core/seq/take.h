/* SPDX-License-Identifier: GPL-3.0-only */
/* T9c take recorder and player: a session event log stamped with the sample counter.
 *
 * Pure C, no heap, no locks, no double and no 64-bit division. Time is the audio sample
 * counter only. A take is a `take_hdr_t` plus up to TAKE_MAX_EVENTS `take_event_t {sample, cmd}`
 * where `sample` is the offset from the take's start (so a take is independent of the counter's
 * 32-bit wrap) and `cmd` is X0X's 32-bit engine command word, `type:4 | a:8 | b:8 | c:12`.
 *
 *   Recorder. take_arm(t, now) starts recording at the next bar boundary. take_feed(t, sample,
 *   cmd) appends one event stamped `sample - start`; it is called from the engine tap, one call
 *   per drained command, in ISR context: constant time, no heap. A feed stamped before `start`
 *   is dropped (the prelude already set the state at offset 0). take_stop(t, now) requests the
 *   take end on the next bar boundary; only the ISR's take_tick() writes the end, exactly once.
 *   When the buffer fills, recording stops cleanly with a TAKE_MARK end and take_full() is set.
 *
 *   Prelude. Inside take_arm the caller's prelude callback writes the current state at offset 0
 *   through take_setup_event(): the playing clip per role (TAKE_LAUNCH now), mutes, tempo and
 *   the current value of every parameter. Replay then starts from the same state. The log and
 *   the prelude are built while state == IDLE, so an ISR feed cannot interleave with them; the
 *   arm publishes start and then the state behind a compiler barrier.
 *
 *   Player. take_play(t, now) starts replaying. take_render(t, nframes, sink) then advances the
 *   replay clock exactly like clip_engine_render(): the sink sees each event at its exact window
 *   offset and any window size works. During playback the caller ignores LIVE input; this module
 *   does not.
 *
 * This module is standalone: it only includes core/model/model.h (and the caller's clip engine
 * header in the tests). T9i wires it into X0X's engine tap, the prelude callback and storage.
 */
#pragma once
#include <stdint.h>
#include "core/model/model.h"

/* ---- command words (model.md, frozen v1) --------------------------------- *
 * type:4 | a:8 | b:8 | c:12. Types 0-11 are X0X's engine.h; a take adds 12-15. */
#define TAKE_CMD(type, a, b, c) \
    ((((uint32_t)(type) & 0xFu) << 28) | (((uint32_t)(a) & 0xFFu) << 20) | \
     (((uint32_t)(b) & 0xFFu) << 12) | ((uint32_t)(c) & 0xFFFu))
#define TAKE_CMD_TYPE(x) (((x) >> 28) & 0xFu)
#define TAKE_CMD_A(x)    (((x) >> 20) & 0xFFu)
#define TAKE_CMD_B(x)    (((x) >> 12) & 0xFFu)
#define TAKE_CMD_C(x)    ((x) & 0xFFFu)

#define TAKE_T_LAUNCH 12u
#define TAKE_T_MUTE   13u
#define TAKE_T_FX     14u
#define TAKE_T_MARK   15u

/* TAKE_LAUNCH: a = clip, b = how, c = role. TAKE_LAUNCH_STOP ignores a.
 * TAKE_LAUNCH_QUANT (S1): a Program Change launch, b = how, c = role | (quant << 8), so the replay
 * uses the recorded quantize rather than the clip's own. */
enum { TAKE_LAUNCH_QUEUE, TAKE_LAUNCH_NOW, TAKE_LAUNCH_STOP, TAKE_LAUNCH_QUANT, TAKE_LAUNCH_PHASE };
#define TAKE_LAUNCH(clip, how, role) TAKE_CMD(TAKE_T_LAUNCH, (clip), (how), (role))
#define TAKE_LAUNCH_Q(clip, quant, role) \
    TAKE_CMD(TAKE_T_LAUNCH, (clip), TAKE_LAUNCH_QUANT, \
             ((uint32_t)(role) & 0xFFu) | (((uint32_t)(quant) & 3u) << 8))
#define TAKE_LAUNCH_AT_PHASE(clip, role, phase) \
    TAKE_CMD(TAKE_T_LAUNCH, (clip), TAKE_LAUNCH_PHASE, ((role) & 7u) | (((phase) & 63u) << 3))
#define TAKE_LAUNCH_ROLE(cmd) (TAKE_CMD_B(cmd) == TAKE_LAUNCH_PHASE ? (TAKE_CMD_C(cmd) & 7u) : (TAKE_CMD_C(cmd) & 0xFFu))
/* TAKE_MUTE: a = role, b = 0/1 */
#define TAKE_MUTE(role, on)          TAKE_CMD(TAKE_T_MUTE, (role), (on), 0)
/* TAKE_FX: a = punch FX id, b = 0 off / 1 on / 2 latch */
enum { TAKE_FX_OFF, TAKE_FX_ON, TAKE_FX_LATCH };
#define TAKE_FX(id, how)             TAKE_CMD(TAKE_T_FX, (id), (how), 0)
/* TAKE_MARK: a = 0 start / 1 end / 2 tempo, value (tempo x10) over b:c */
#define TAKE_MARK_START              TAKE_CMD(TAKE_T_MARK, 0u, 0u, 0u)
#define TAKE_MARK_END                TAKE_CMD(TAKE_T_MARK, 1u, 0u, 0u)
#define TAKE_MARK_TEMPO(bpm_x10) \
    TAKE_CMD(TAKE_T_MARK, 2u, ((uint32_t)(bpm_x10) >> 12) & 0xFFu, \
             (uint32_t)(bpm_x10) & 0xFFFu)
#define TAKE_MARK_TEMPO_GET(b, c) \
    ((((uint32_t)(b) & 0xFFu) << 12) | ((uint32_t)(c) & 0xFFFu))

/* ---- types --------------------------------------------------------------- */

enum { TAKE_IDLE, TAKE_ARMED, TAKE_REC, TAKE_PLAY };

/* The caller tells the take where the bar boundaries are, so an arm or a stop lands on one:
 * return the first boundary at or after `sample` (the sequencer knows transport alignment).
 * If no function is set, `sample` is itself treated as the boundary. */
typedef uint32_t (*take_bar_fn)(void *ctx, uint32_t sample);

/* Called once inside take_arm (main-loop context, before a note of the take records) to write
 * the offset-0 setup events. Use take_setup_event() for each one. */
struct take;
typedef void (*take_prelude_fn)(void *ctx, struct take *t);

typedef struct take {
    take_hdr_t   hdr;                        /* nevents, bpm_x10, length_samples */
    take_event_t ev[TAKE_MAX_EVENTS];         /* sample is the offset from the take's start */
    uint16_t     n;                           /* events written, == hdr.nevents */
    uint8_t      state;                       /* TAKE_IDLE / _ARMED / _REC / _PLAY */
    uint8_t      full;                        /* the buffer filled: the UI can show it */
    uint8_t      stop_set;                    /* take_stop() requested: end on stop_at */
    uint8_t      arming;                      /* take_arm() is building the offset-0 log */
    uint8_t      ended;                       /* take_end() ran: the END mark is written once */
    uint8_t      rsv[3];
    uint32_t     start;                       /* absolute sample recording starts / started */
    uint32_t     stop_at;                     /* absolute boundary take_stop() ends on */
    uint32_t     play_pos;                    /* samples replayed so far */
    uint16_t     play_i;                      /* next event to replay */
    take_bar_fn     bar;
    void           *bar_ctx;
    take_prelude_fn pre;
    void           *pre_ctx;
} take_t;

/* Playback sink: `sample` is the offset into the window passed to take_render(). */
typedef struct {
    void (*ev)(void *ctx, uint32_t sample, uint32_t cmd);
    void *ctx;
} take_sink_t;

/* ---- setup --------------------------------------------------------------- */

void take_init(take_t *t);
void take_set_bar_fn(take_t *t, take_bar_fn fn, void *ctx);
void take_set_prelude(take_t *t, take_prelude_fn fn, void *ctx);
void take_set_bpm(take_t *t, uint16_t bpm_x10);          /* the take's tempo, into the header */
void take_setup_event(take_t *t, uint32_t cmd);          /* offset-0 event, from the prelude */

/* ---- recorder ------------------------------------------------------------ */

void take_arm(take_t *t, uint32_t now);                  /* start at the next bar boundary */
void take_feed(take_t *t, uint32_t sample, uint32_t cmd);/* ISR: append one drained command */
void take_stop(take_t *t, uint32_t now);                 /* request the end at the next boundary */
void take_tick(take_t *t, uint32_t now);                 /* call every block: arm/stop, END here */
void take_finish(take_t *t, uint32_t now);               /* T9i: end now, no bar wait (leaving LIVE) */

/* ---- player -------------------------------------------------------------- */

void take_play(take_t *t, uint32_t now);                 /* reset the cursor, start replaying */
void take_render(take_t *t, uint32_t nframes, const take_sink_t *sink);
void take_stop_play(take_t *t);

/* ---- queries ------------------------------------------------------------- */

int         take_state(const take_t *t);
int         take_recording(const take_t *t);
int         take_armed(const take_t *t);
int         take_playing(const take_t *t);
int         take_full(const take_t *t);
int         take_has(const take_t *t);                   /* a take was recorded */
uint16_t    take_nevents(const take_t *t);
const take_hdr_t *take_header(const take_t *t);
const take_event_t *take_events(const take_t *t);

/* ISR recording-start snapshot: rebuild offset-zero setup before boundary notes fire. */
void take_refresh_setup(take_t *t);

/* ISR: incomplete recording (ingress overflow), closed with an END and full indication. */
void take_overflow(take_t *t, uint32_t now);
