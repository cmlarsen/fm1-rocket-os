/* SPDX-License-Identifier: GPL-3.0-only */
/* T10c punch FX DSP core: the six LIVE performance effects.
 *
 * T10i calls perform_process() on the master stereo signal, after the mix
 * (fxbus_process_st) and before the output stage (master_process_st). The
 * engage / release calls come from the main loop and reach the audio ISR
 * through a small single-producer / single-consumer ring, like
 * core/seq/clip_engine.c's, so the two sides never take a lock.
 *
 *   out = perform( mix )   then   master_process_st( out )
 *
 * The six effects are:
 *   0 repeat 1/8   1 repeat 1/16   - a grid-aligned slice, looped until release;
 *   2 tape stop                   - the read head slows to zero over ~0.6 s;
 *   3 filter dive                 - a resonant low-pass, open down to 200 Hz;
 *   4 echo throw                  - drives X0X's fxbus delay (no line of its own);
 *   5 freeze                      - a ~120 ms grain, looped until release.
 * Only one buffer effect (0, 1, 2 or 5) is active at a time; a new one
 * replaces the old with a crossfade. Filter dive and echo can stack with
 * them. Every hand-off (engage / release) is crossfaded so nothing clicks.
 *
 * ECHO has no delay line (the RAM plan gives it 0 bytes): it is a small
 * controller that overrides X0X's fxbus delay while held and restores the
 * song's own settings on release, so the tail decays naturally. See
 * perform_echo_t / perform_echo_targets().
 *
 * MEMORY (the v1 RAM plan). One shared history buffer serves repeat, tape
 * stop and freeze:
 *     int16_t hist[PERFORM_HIST_N]   mono int16, 16384 samples = 32 KiB.
 * The master signal is summed to mono into it (the live passthrough stays
 * stereo; a buffer effect collapses the image on purpose, as a punch-in).
 * Mono was chosen over stereo so a slice still fits at slow tempos: a 1/16
 * note fits down to ~40 BPM and a 1/8 note down to ~81 BPM at 44.1 kHz.
 * Below that a slice is capped at PERFORM_HIST_N samples - the longest that
 * fits - and documented (see reports/T10c.md). Tape stop reads the same
 * history with a lag that peaks near 0.3 s, so it always fits.
 * The history keeps recording while a loop plays: the write head skips the
 * loop's slice (see perform.c), so tape and repeat after a loop start from
 * current audio. If a loop fills the whole buffer there is nowhere to write;
 * the tape stop then plays the live signal instead of stale audio.
 *
 * FIXED POINT / FLOAT. The buffers are int16 and the arithmetic is 32-bit
 * integer where it matters (indices, the grid, the slice length). Float is
 * used only where X0X's master path already uses it, and the host build's
 * -ffp-contract=off keeps the output bit-identical across runs. No heap, no
 * double, no 64-bit division. perform_process() is constant time per sample.
 */
#pragma once
#include <stdint.h>

enum {
    PERF_REPEAT_8 = 0,   /* repeat 1/8  */
    PERF_REPEAT_16,      /* repeat 1/16 */
    PERF_TAPE,           /* tape stop   */
    PERF_FILTER,         /* filter dive */
    PERF_ECHO,           /* echo throw  */
    PERF_FREEZE,         /* freeze      */
    PERF_NFX
};

/* The device's sample rate (sequencer.h FS); overridable for a host test. */
#ifndef PERFORM_FS
#define PERFORM_FS 44100u
#endif

#define PERFORM_HIST_N 16384u     /* shared history: 32 KiB of mono int16 */
#define PERFORM_CMD_RING 16u
#define PERFORM_NO_GRID 0xFFFFFFFFu   /* the transport is stopped: no grid point */
#define PERFORM_XFADE_IN 88u      /* engage hand-off: 2 ms */
#define PERFORM_XFADE_OUT 88u     /* release hand-off: 2 ms */
#define PERFORM_XFADE_FREEZE 441u /* freeze loop-wrap crossfade: 10 ms */
#define PERFORM_FREEZE_N 5292u    /* freeze grain: ~120 ms */

/* Echo throw targets for X0X's fxbus delay. T10i applies them with
 * engine_set(T_MIX, part, MX_DLY, send), engine_set(T_FX, 0, FX_DL_FDBK, fdbk)
 * and engine_set(T_FX, 0, FX_DL_TIME, time) whenever perform_echo_targets()
 * reports a change. `time` is an fxbus division index: 5 is the 1/8 note. */
typedef struct {
    uint8_t send;   /* the part's delay send, 0..127 (T_MIX MX_DLY) */
    uint8_t fdbk;   /* FX_DL_FDBK, 0..127                             */
    uint8_t time;   /* FX_DL_TIME division index (5 = 1/8)            */
    uint8_t on;     /* 1 while the throw is held                      */
} perform_echo_t;

/* internal buffer-effect states */
enum { PERF_ST_FREE = 0, PERF_ST_WAIT_GRID, PERF_ST_RUN };
/* crossfade modes */
enum { PERF_XF_NONE = 0, PERF_XF_IN, PERF_XF_OUT, PERF_XF_OLD };

#define PERFORM_ECHO_SEND 100u   /* ~0.79, opened while held */
#define PERFORM_ECHO_FDBK 75u    /* ~0.50 of the 0.85 range  */
#define PERFORM_ECHO_TIME 5u     /* fxbus "1/8"              */

typedef struct {
    uint8_t  cmd;       /* PERF_CMD_* */
    uint8_t  fx;        /* PERF_* */
    uint8_t  latch;     /* engage: 1 = tap (latched), 0 = hold (momentary) */
    uint16_t bpm_x10;   /* PERF_CMD_TEMPO (kept for the ring record) */
} perform_cmd_t;

enum { PERF_CMD_ENGAGE, PERF_CMD_RELEASE, PERF_CMD_RESET, PERF_CMD_TEMPO };

/* One buffer effect's own state. act is the active effect, old is crossfading
 * out during a replacement, pend waits for a grid before it is promoted. */
typedef struct {
    uint8_t  type;        /* PERF_REPEAT_8/16, PERF_TAPE, PERF_FREEZE */
    uint8_t  state;       /* PERF_ST_* */
    uint8_t  degraded;    /* tape engaged while the history could not record */
    uint8_t  rsv;
    /* loop (repeat / freeze) */
    uint32_t slice_start; /* index into hist */
    uint32_t slice_len;
    uint32_t loop_pos;
    uint32_t wrap;        /* loop-wrap crossfade length (freeze; repeat = 0) */
    /* tape stop */
    float    tape_rate;   /* current playback rate 0..1 */
    float    tape_dec;    /* rate decrement per sample */
    float    tape_read;   /* fractional read index into hist */
} perf_slot_t;

typedef struct {
    /* single-producer / single-consumer command ring (UI writer, ISR reader) */
    perform_cmd_t cmd[PERFORM_CMD_RING];
    volatile uint8_t cmd_head;   /* producer writes here */
    volatile uint8_t cmd_tail;   /* consumer reads here */
    /* coalesced tempo and override flags: a full ring never drops a release */
    volatile uint16_t tempo_req;
    volatile uint8_t  tempo_pending;
    volatile uint8_t  reset_req;
    volatile uint8_t  rel_req[PERF_NFX];
    uint16_t bpm_x10;            /* the ISR's copy, clamped */

    /* shared history, mono int16 (32 KiB) */
    int16_t  hist[PERFORM_HIST_N];
    uint32_t hist_w;             /* next slot to write */
    uint8_t  hist_stalled;       /* the last sample could not be recorded */

    /* buffer effects */
    perf_slot_t act, old, pend;
    uint8_t  act_on, old_on, pend_on;
    float    gain_act, gain_old; /* wet weights, 0..1 */
    float    fade_inc;
    uint8_t  xmode;              /* PERF_XF_* */
    volatile uint8_t latch[PERF_NFX];

    /* echo throw controller (drives X0X's fxbus delay; no line of its own) */
    uint8_t  echo_on;
    uint8_t  echo_base_send, echo_base_fdbk, echo_base_time;
    uint8_t  echo_send, echo_fdbk, echo_time;
    volatile uint8_t echo_changed;

    /* filter dive */
    uint8_t  filt_on;
    uint8_t  filt_state;         /* 0 off, 1 diving, 2 holding, 3 releasing */
    float    filt_amt;           /* 0 open .. 1 dived */
    float    filt_step;
    float    filt_g, filt_k, filt_a1;
    float    filt_ic1[2], filt_ic2[2];
    uint32_t filt_cr;
} perform_t;

/* Zero everything, set 120.0 BPM. */
void perform_init(perform_t *p);

/* Main loop: latch the effect on (latch 1) or start a momentary one (latch 0).
 * A latched tap on an already-latched effect releases it. */
void perform_engage(perform_t *p, int fx, int latch);

/* Main loop: turn the effect off (the momentary button-up, or a tap release).
 * Never dropped, even when the command ring is full. */
void perform_release(perform_t *p, int fx);

/* HOME + FX: everything off at once, back to the dry signal. Never dropped. */
void perform_reset(perform_t *p);

/* T9i: the take player's ISR-side FX apply, the same effect as the ring command but run directly on
 * the audio ISR (before the next perform_process), so a take replay never pushes onto the main-loop
 * ring. how: 0 off, 1 on (momentary), 2 latch (a tap on a latched effect releases it). */
void perform_take_fx(perform_t *p, int fx, int how);
/* T9i: end a take: every effect off, the queued main-loop commands dropped (ISR-safe). */
void perform_take_reset(perform_t *p);

/* Main loop: tempo in BPM x10 (coalesced to the latest value, applied at the
 * next perform_process). */
void perform_set_tempo(perform_t *p, uint16_t bpm_x10);

/* Echo throw: on = hold, off = release. Equivalent to engaging/releasing
 * PERF_ECHO; the audio is untouched, the fxbus delay is driven instead. */
void perform_echo_throw(perform_t *p, int on);

/* The song's own fxbus delay settings, which the throw overrides and restores.
 * T10i calls this whenever the project's delay parameters change. */
void perform_echo_base(perform_t *p, uint8_t send, uint8_t fdbk, uint8_t time);

/* If the echo targets changed since the last call, write them and return 1. */
int perform_echo_targets(perform_t *p, perform_echo_t *out);

/* Audio ISR: process nframes in place. next_8 / next_16 are the number of
 * samples from the start of this block to the transport's next 1/8 and 1/16
 * grid points (PERFORM_NO_GRID when the transport is stopped). They may
 * exceed nframes; the caller recomputes them every block. */
void perform_process(perform_t *p, float *l, float *r, uint32_t nframes,
                     uint32_t next_8, uint32_t next_16);

/* Best-effort query for the UI/LEDs: the effect was last latched on. */
int perform_latched(const perform_t *p, int fx);

/* T9i: the effect is engaged now, latched or momentary (for the take recorder's prelude). */
int perform_active(const perform_t *p, int fx);
