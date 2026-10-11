/* SPDX-License-Identifier: GPL-3.0-only */
/* T4c clip engine: a song_t plays as five per-role clips, launched at a quantize point.
 *
 * Pure C, no heap, no locks, no double and no 64-bit division. Time is the audio sample
 * counter only. The main loop calls clip_engine_launch()/clip_engine_stop()/
 * clip_engine_launch_now(); the audio ISR calls clip_engine_render() and every event comes
 * back through the sink with its exact sample offset. The UI calls reach the engine through
 * a small single-producer / single-consumer ring, so the two sides never take a lock.
 *
 * A clip's kind is decided by its role, not by its bytes: clip_role(clip) == ROLE_DRUMS uses
 * d.drum, every other role (FX included) uses d.notes[0..nnotes). An unused clip cannot be
 * launched.
 *
 * Step timing is exact integer arithmetic: a 16th lasts STEP_NUM/bpm_x10 samples with
 * STEP_NUM = FS*150, driven by a Bresenham accumulator that keeps the fractional part, so
 * step_sample(k) = floor(k * STEP_NUM / bpm_x10) and nothing drifts. Every division is
 * 32-bit; see clip_engine.c and reports/T4c.md.
 *
 * This module is standalone: it only includes core/model/model.h. T4i wires it into X0X's
 * sequencer and UI, behind the same LIVE view.
 */
#pragma once
#include <stdint.h>
#include "core/model/model.h"

enum { CLIPEV_OFF, CLIPEV_NOTE_ON, CLIPEV_NOTE_OFF, CLIPEV_DRUM, CLIPEV_MOTION,
       CLIPEV_CLIP_STARTED, CLIPEV_CLIP_STOPPED, CLIPEV_CLIP_QUEUED, CLIPEV_CLIP_CANCELLED,
       CLIPEV_PLAYING, CLIPEV_STOPPED };

typedef struct {
    uint32_t sample;   /* offset into the window passed to clip_engine_render() */
    uint8_t  kind;
    uint8_t  role;
    uint8_t  a;        /* note / drum lane / param low / 1 | 0 */
    uint8_t  b;        /* velocity / drum accent / param high */
    uint8_t  c;        /* clip index, where meaningful */
} clip_event_t;

typedef struct {
    void (*ev)(void *ctx, const clip_event_t *e);
    void *ctx;
} clip_sink_t;

/* UI-side command: a clip index in model.h's key-zone order, a role stop, a tempo, or (T7b) a
 * reload of a playing clip's private copy after an edit. */
enum { CLIP_CMD_LAUNCH, CLIP_CMD_STOP, CLIP_CMD_LAUNCH_NOW, CLIP_CMD_SET_BPM, CLIP_CMD_RELOAD };
typedef struct {
    uint8_t  cmd;
    uint8_t  clip;     /* launch: the clip; stop: the role */
    uint8_t  quant;    /* Q_BAR / Q_BEAT / Q_END */
    uint16_t bpm_x10;  /* CLIP_CMD_SET_BPM */
} clip_cmd_t;

#define CLIP_CMD_RING 16u
#define CLIP_ENGINE_MAX_VOICES 32u
#define CLIP_ENGINE_BPM_MIN 200u     /* 20.0 BPM */
#define CLIP_ENGINE_BPM_MAX 3000u    /* 300.0 BPM */

typedef struct {
    song_t *song;
    uint8_t  playing;
    uint8_t  was_playing;
    uint16_t bpm_x10;        /* current tempo (clamped) */
    uint16_t bpm_src;        /* last tempo requested, to detect a real change */
    uint32_t step_L;         /* samples per 16th, integer part */
    uint32_t step_f;         /* remainder, in 1/bpm_x10 sample units */
    uint32_t step_sample;    /* absolute sample the current step starts on (unswung) */
    uint32_t step_frac;      /* Bresenham fraction, in 1/bpm_x10 sample units */
    uint32_t elapsed;        /* samples since transport start */
    uint32_t global_step;    /* 16th index since transport start */
    uint8_t  pos[NROLES];    /* step inside each role's clip */
    uint8_t  cur[NROLES];    /* playing clip, or CLIP_NONE */
    uint8_t  que[NROLES];    /* target clip of the pending action, or CLIP_NONE */
    uint8_t  que_set[NROLES];/* a queued action is pending: target que[role] */
    uint32_t que_at[NROLES]; /* global step the pending action takes effect on */

    /* Single-producer / single-consumer command ring (UI writer, ISR reader). */
    clip_cmd_t cmd[CLIP_CMD_RING];
    volatile uint8_t cmd_head;   /* producer writes here */
    volatile uint8_t cmd_tail;   /* consumer reads here */

    /* Note pool: every note this engine turned on but has not turned off yet. */
    struct { uint8_t used, role, note; uint32_t end; } note[CLIP_ENGINE_MAX_VOICES];

    /* T7b: each playing role's clip has its own copy. The audio ISR reads only this, never
     * `song->clip[]`, which the UI edits in the main loop. A copy is taken at launch, at the
     * loop point, and on CLIP_CMD_RELOAD (posted after an edit) — i.e. a double-buffer swap at a
     * step boundary, so the ISR never sees a half-written clip_t. */
    clip_t play[NROLES];
} clip_engine_t;

void clip_engine_init(clip_engine_t *e, song_t *song);

/* Transport. Tempo and swing come from the song (or a tempo command). */
void clip_engine_start(clip_engine_t *e);
void clip_engine_transport_stop(clip_engine_t *e);

/* Queue the clip on its role (clip_role()) at the clip's own quantize; a second launch of
 * the clip that is already queued cancels the queue. clip_engine_launch_quant uses an
 * explicit quantize. */
void clip_engine_launch(clip_engine_t *e, uint8_t clip);
void clip_engine_launch_quant(clip_engine_t *e, uint8_t clip, uint8_t quant);

/* Queue a stop on a role, at that role's current clip quantize (the next bar if it is idle).
 * A second stop cancels the queued one. */
void clip_engine_stop(clip_engine_t *e, uint8_t role);

/* HOME + pad: switch at the next step, whatever the clip's quantize says. It does not cancel
 * a queued clip; it launches the new one now. */
void clip_engine_launch_now(clip_engine_t *e, uint8_t clip);

/* Tempo change from the main loop, through the ring. It is applied at the next render and is
 * the one source of truth (it also writes song->bpm_x10). */
void clip_engine_queue_bpm(clip_engine_t *e, uint16_t bpm_x10);

/* T7b: re-take the playing copy of `clip` from song->clip[clip] at the next step boundary, after
 * the UI edited it. A no-op when the role is not playing that clip (it is loaded on launch). */
void clip_engine_reload(clip_engine_t *e, uint8_t clip);

/* T7b: the 16th index since the transport started (for recording and the playhead). */
uint32_t clip_engine_global_step(const clip_engine_t *e);

/* T7b: the step inside a role's playing clip (the playhead), 0 when the role is stopped. */
uint8_t clip_engine_clip_step(const clip_engine_t *e, uint8_t role);

/* Add nframes of transport and call the sink for every event in that window, with its
 * exact sample offset into the window. */
void clip_engine_render(clip_engine_t *e, uint32_t nframes, const clip_sink_t *sink);

/* UI queries. */
uint8_t clip_engine_playing_clip(const clip_engine_t *e, uint8_t role); /* CLIP_NONE = stopped */
uint8_t clip_engine_queued_clip(const clip_engine_t *e, uint8_t role);  /* CLIP_NONE = none  */
uint8_t clip_engine_progress(const clip_engine_t *e, uint8_t role);     /* 0..255 through the clip */
void    clip_engine_pos(const clip_engine_t *e, uint8_t *bar, uint8_t *beat, uint8_t *step);
