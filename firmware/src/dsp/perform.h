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
 *   4 echo throw                  - a tempo-synced 1/8 note feedback echo;
 *   5 freeze                      - a ~120 ms grain, looped until release.
 * Only one buffer effect (0, 1, 2 or 5) is active at a time; a new one
 * replaces the old with a crossfade. Filter dive and echo can stack with
 * them. Every hand-off (engage / release) is crossfaded so nothing clicks.
 *
 * MEMORY. One shared history buffer serves repeat, tape stop and freeze:
 *     int16_t hist[PERFORM_HIST_N]   mono int16, 16384 samples = 32 KiB.
 * The master signal is summed to mono into it (the live passthrough stays
 * stereo; a buffer effect collapses the image on purpose, as a punch-in).
 * Mono was chosen over stereo so a slice still fits at slow tempos: a 1/16
 * note fits down to ~40 BPM and a 1/8 note down to ~81 BPM at 44.1 kHz.
 * Below that a slice is capped at PERFORM_HIST_N samples - the longest that
 * fits - and documented (see reports/T10c.md). Tape stop reads the same
 * history with a lag that peaks near 0.3 s, so it always fits.
 * The echo has its own line:
 *     int16_t echo[PERFORM_ECHO_N]   mono int16, 8192 samples = 16 KiB.
 * A 1/8 note at 44.1 kHz is 13,230,000 / bpm_x10 samples, so in 16 KiB it
 * fits at >= ~161 BPM; below that the delay is halved (once per octave)
 * until it fits, still on the beat (1/16, 1/32, ...).
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
#define PERFORM_ECHO_N 8192u      /* echo line: 16 KiB of mono int16 */
#define PERFORM_CMD_RING 16u
#define PERFORM_NO_GRID 0xFFFFFFFFu   /* the transport is stopped: no grid point */
#define PERFORM_XFADE_IN 88u      /* engage hand-off: 2 ms */
#define PERFORM_XFADE_OUT 88u     /* release hand-off: 2 ms */
#define PERFORM_XFADE_FREEZE 441u /* freeze loop-wrap crossfade: 10 ms */
#define PERFORM_FREEZE_N 5292u    /* freeze grain: ~120 ms */

/* internal buffer-effect states */
enum { PERF_ST_FREE = 0, PERF_ST_WAIT_GRID, PERF_ST_RUN };
/* crossfade modes */
enum { PERF_XF_NONE = 0, PERF_XF_IN, PERF_XF_OUT };

typedef struct {
    uint8_t  cmd;       /* PERF_CMD_* */
    uint8_t  fx;        /* PERF_* */
    uint8_t  latch;     /* engage: 1 = tap (latched), 0 = hold (momentary) */
    uint16_t bpm_x10;   /* PERF_CMD_TEMPO */
} perform_cmd_t;

enum { PERF_CMD_ENGAGE, PERF_CMD_RELEASE, PERF_CMD_RESET, PERF_CMD_TEMPO };

/* One buffer effect's own state. act is the active effect, old is crossfading
 * out during a replacement, pend waits for a grid before it is promoted. */
typedef struct {
    uint8_t  type;        /* PERF_REPEAT_8/16, PERF_TAPE, PERF_FREEZE */
    uint8_t  state;       /* PERF_ST_* */
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
    uint16_t bpm_x10;            /* the ISR's copy, clamped */

    /* shared history, mono int16 (32 KiB) */
    int16_t  hist[PERFORM_HIST_N];
    uint32_t hist_w;             /* next slot to write */

    /* buffer effects */
    perf_slot_t act, old, pend;
    uint8_t  act_on, old_on, pend_on;
    float    gain_act, gain_old; /* wet weights, 0..1 */
    float    fade_inc;
    uint8_t  xmode;              /* PERF_XF_* */
    volatile uint8_t latch[PERF_NFX];

    /* echo: its own mono int16 line (16 KiB) */
    int16_t  echo[PERFORM_ECHO_N];
    uint32_t echo_w;
    uint32_t echo_len;           /* current delay in samples */
    uint8_t  echo_on;            /* held: keep sending */
    uint8_t  echo_active;        /* tail still ringing */
    uint32_t echo_quiet;         /* consecutive zero writes */

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

/* Main loop: turn the effect off (the momentary button-up, or a tap release). */
void perform_release(perform_t *p, int fx);

/* HOME + FX: everything off at once, back to the dry signal. */
void perform_reset(perform_t *p);

/* Main loop: tempo in BPM x10 (applied at the next perform_process). */
void perform_set_tempo(perform_t *p, uint16_t bpm_x10);

/* Audio ISR: process nframes in place. next_8 / next_16 are the number of
 * samples from the start of this block to the transport's next 1/8 and 1/16
 * grid points (PERFORM_NO_GRID when the transport is stopped). They may
 * exceed nframes; the caller recomputes them every block. */
void perform_process(perform_t *p, float *l, float *r, uint32_t nframes,
                     uint32_t next_8, uint32_t next_16);

/* Best-effort query for the UI/LEDs: the effect was last latched on. */
int perform_latched(const perform_t *p, int fx);
