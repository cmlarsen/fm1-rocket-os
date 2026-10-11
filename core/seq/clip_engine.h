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

/* UI-side command: a clip index in model.h's key-zone order, a role stop, a role mute, a tempo, or
 * (T7b) a launch that takes effect on an exact global step (the recorded clip's close). */
enum { CLIP_CMD_LAUNCH, CLIP_CMD_STOP, CLIP_CMD_LAUNCH_NOW, CLIP_CMD_SET_BPM, CLIP_CMD_MUTE,
       CLIP_CMD_MUTE_CLEAR, CLIP_CMD_LAUNCH_AT, CLIP_CMD_SET_BPM_SOFT, CLIP_CMD_RESTART, CLIP_CMD_CONTINUE,
       CLIP_CMD_TRANSPORT_STOP, CLIP_CMD_START };
typedef struct {
    uint8_t  cmd;
    uint8_t  clip;     /* launch: the clip; stop / mute (T10i): the role */
    uint8_t  quant;    /* Q_BAR / Q_BEAT / Q_END; mute: 1 on, 0 off */
    uint16_t bpm_x10;  /* CLIP_CMD_SET_BPM */
    uint32_t at;       /* CLIP_CMD_LAUNCH_AT: the global step it takes effect on */
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
    uint16_t bpm_ext;        /* T11i: a live tempo (the external clock) overriding the song; 0 = none */
    uint16_t song_bpm;       /* T11i: the song's own bpm_x10 as last seen (a load/edit clears bpm_ext) */
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
    uint8_t  muted[NROLES];  /* T10i: the role is muted (no new notes; the transport keeps running) */

    /* T11i: MIDI Start / Continue. start() resets to bar 1, step 0; continue() resumes from the
     * stop point, which a stop saved here (the visible cur/pos are cleared as before). */
    uint8_t  resume;             /* the pending start is a Continue */
    uint8_t  restart;            /* clip_engine_start was called: force a reset */
    uint8_t  s_cur[NROLES];
    uint8_t  s_pos[NROLES];
    uint32_t s_elapsed, s_step_sample, s_step_frac, s_global;

    /* Single-producer / single-consumer command ring (UI writer, ISR reader). */
    clip_cmd_t cmd[CLIP_CMD_RING];
    volatile uint8_t cmd_head;   /* producer writes here */
    volatile uint8_t cmd_tail;   /* consumer reads here */

    /* Note pool: every note this engine turned on but has not turned off yet. */
    struct { uint8_t used, role, note; uint32_t end; } note[CLIP_ENGINE_MAX_VOICES];

    /* T7b: the main loop owns the snapshot; the audio ISR only ever reads play[]/next[] here,
     * never `song->clip[]`. `play[role]` is the role's playing copy and `next[role]` a launch
     * target staged by the main loop before the launch command; the ISR swaps next -> play at the
     * step boundary (apply_role). After an edit the main loop writes the changed clip into play[] /
     * next[] with the audio IRQs masked (clip_engine_clip_changed), so the ISR never sees a
     * half-written clip_t. */
    clip_t play[NROLES];
    clip_t next[NROLES];
    uint8_t next_set[NROLES];   /* next[role] holds a staged launch target */
    uint8_t next_clip[NROLES];  /* the clip index staged in next[role] */
} clip_engine_t;

void clip_engine_init(clip_engine_t *e, song_t *song);

/* Transport. Tempo and swing come from the song (or a tempo command).
 * start() plays (resetting to bar 1, step 0 when it was stopped); restart() (T11i) forces the reset
 * and relaunches what was playing (MIDI Start); continue() (T11i) resumes from where
 * transport_stop() left it. restart() and continue() reach the engine through its command ring, so
 * `playing` changes only in the ISR. */
void clip_engine_start(clip_engine_t *e);
void clip_engine_restart(clip_engine_t *e);
void clip_engine_continue(clip_engine_t *e);
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

/* T7b: launch so the clip starts on exactly global step `at_step` (the recorded clip's close
 * boundary). Armed before the boundary is reached, so there is no gap. */
void clip_engine_launch_at(clip_engine_t *e, uint8_t clip, uint32_t at_step);

/* Tempo change from the main loop, through the ring. It is applied at the next render and is
 * the one source of truth (it also writes song->bpm_x10). */
void clip_engine_queue_bpm(clip_engine_t *e, uint16_t bpm_x10);

/* T11i: a live/transient tempo (the external clock). It sets the running tempo but does NOT write
 * song->bpm_x10, so switching the clock source back to Internal restores the song's own tempo.
 * A later clip_engine_queue_bpm (or the song's own change) cancels it. */
void clip_engine_queue_bpm_soft(clip_engine_t *e, uint16_t bpm_x10);

/* T7b: after an edit to `clip`, the main loop writes the song's clip into the engine's play[] (when
 * it is the role's playing clip) and next[] (when a launch of it is staged), with the audio IRQs
 * masked by the caller. The ISR never reads song->clip[]. */
void clip_engine_clip_changed(clip_engine_t *e, uint8_t clip);

/* T7b: the 16th index since the transport started, and the fraction into the current step (0..255),
 * for recording (the loop owns these). */
uint32_t clip_engine_global_step(const clip_engine_t *e);
uint8_t  clip_engine_step_phase(const clip_engine_t *e);

/* T7b: the step inside a role's playing clip (the playhead), 0 when the role is stopped. */
uint8_t clip_engine_clip_step(const clip_engine_t *e, uint8_t role);

/* T10i: mute / unmute a role, through the same ring. Muting releases the role's sounding
 * notes immediately (note-offs at the window start) and suppresses new note-ons; the transport
 * and the role's position keep running, so unmuting rejoins the clip in time. `song->role[role].muted`
 * is updated, but the caller does not mark the song dirty for a live mute. */
void clip_engine_mute(clip_engine_t *e, uint8_t role, int on);
int  clip_engine_muted(const clip_engine_t *e, uint8_t role);

/* Clear every role mute. It zeroes the bits immediately (a caller that stops the engine, e.g.
 * leaving LIVE, has no later drain) AND queues CLIP_CMD_MUTE_CLEAR, which drains after any mute
 * command already in the ring, so a pending CLIP_CMD_MUTE cannot re-apply. The mute is performance
 * state, never song state; live_enter also clears. */
void clip_engine_mute_clear(clip_engine_t *e);

/* Add nframes of transport and call the sink for every event in that window, with its
 * exact sample offset into the window. */
void clip_engine_render(clip_engine_t *e, uint32_t nframes, const clip_sink_t *sink);

/* UI queries. */
uint8_t clip_engine_playing_clip(const clip_engine_t *e, uint8_t role); /* CLIP_NONE = stopped */
uint8_t clip_engine_queued_clip(const clip_engine_t *e, uint8_t role);  /* CLIP_NONE = none  */
uint8_t clip_engine_progress(const clip_engine_t *e, uint8_t role);     /* 0..255 through the clip */
void    clip_engine_pos(const clip_engine_t *e, uint8_t *bar, uint8_t *beat, uint8_t *step);
