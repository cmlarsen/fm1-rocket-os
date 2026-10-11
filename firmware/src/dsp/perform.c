/* SPDX-License-Identifier: GPL-3.0-only */
/* T10c punch FX DSP core; see perform.h for the model and the memory budget.
 *
 * Grid timing is integer. A 1/8 note lasts PERFORM_FS * 300 / bpm_x10 samples
 * and a 1/16 note PERFORM_FS * 150 / bpm_x10; both divides are 32-bit
 * (13,230,000 < 2^31 at 44.1 kHz). The caller hands in the distance to the
 * next grid point, so the slice starts on exactly the sample the transport
 * calls the grid - no drift is introduced here.
 *
 * Every hand-off is a linear crossfade over PERFORM_XFADE_IN / _OUT samples:
 *   out = live * (1 - a) + wet * a,   a = gain_act + gain_old
 * When nothing is engaged a == 0 and the output is the input, sample for
 * sample, so a released or reset effect leaves no residue.
 *
 * The history keeps recording while a loop plays. The recording head skips the
 * active loop's slice (it is the only place the loop's audio lives), so the
 * rolling history stays current and a later tape stop or repeat starts from
 * the live signal. A loop that fills the whole buffer leaves nowhere to write;
 * the head then stalls and a tape stop plays the live signal instead of the
 * stale snapshot (the documented fallback).
 */
#include "perform.h"
#include "fastmath.h"

#define HIST_MASK (PERFORM_HIST_N - 1u)

/* publish a ring slot before its index (the clip engine's barrier) */
#define PERF_BARRIER() __asm__ volatile("" ::: "memory")

/* tape stop: 0.6 s from full speed to zero, linear in time */
#define PERF_TAPE_MS 600u
/* filter dive: open 16 kHz down to 200 Hz, Q 0.707 -> 12, slewed at control rate */
#define FILT_FC_OPEN 16000.0f
#define FILT_FC_LO 200.0f
#define FILT_Q 12.0f
#define FILT_Q0 0.70710678f
#define FILT_CR 8u

/* ---------------------------------------------------------------- helpers --- */

static inline int16_t f2i16(float x)
{
    if (x >= 1.0f) return (int16_t)32767;
    if (x <= -1.0f) return (int16_t)-32768;
    return (int16_t)(x * 32767.0f);   /* truncate toward zero, both platforms */
}

static inline float i2f16(int16_t v) { return (float)v * (1.0f / 32768.0f); }

static inline int is_loop(uint8_t t)
{
    return t == PERF_REPEAT_8 || t == PERF_REPEAT_16 || t == PERF_FREEZE;
}

static inline int is_buffer(int fx)
{
    return fx == PERF_REPEAT_8 || fx == PERF_REPEAT_16 || fx == PERF_TAPE || fx == PERF_FREEZE;
}

/* ------------------------------------------------- buffer effect engine --- */

static void capture_repeat(perform_t *p, perf_slot_t *s)
{
    uint32_t len = (s->type == PERF_REPEAT_16)
                 ? ((uint32_t)(PERFORM_FS * 150u) / (uint32_t)p->bpm_x10)
                 : ((uint32_t)(PERFORM_FS * 300u) / (uint32_t)p->bpm_x10);
    if (len > PERFORM_HIST_N)
        len = PERFORM_HIST_N;        /* slow tempo: the longest slice that fits */
    if (len == 0)
        len = 1u;
    s->slice_start = (p->hist_w + PERFORM_HIST_N - len) & HIST_MASK;
    s->slice_len = len;
    s->loop_pos = 0;
    s->wrap = 0;                     /* repeat loops the slice exactly */
}

static void capture_freeze(perform_t *p, perf_slot_t *s)
{
    s->slice_start = (p->hist_w + PERFORM_HIST_N - PERFORM_FREEZE_N) & HIST_MASK;
    s->slice_len = PERFORM_FREEZE_N;
    s->loop_pos = 0;
    s->wrap = PERFORM_XFADE_FREEZE;     /* grain: 10 ms wrap crossfade */
}

/* the wet sample of one buffer effect (advances its read head); mono is the
 * live mono sample, used by a tape stop that could not record (fallback) */
static float slot_wet(perform_t *p, perf_slot_t *s, float mono)
{
    switch (s->type) {
    case PERF_REPEAT_8:
    case PERF_REPEAT_16: {
        float y = i2f16(p->hist[(s->slice_start + s->loop_pos) & HIST_MASK]);
        if (++s->loop_pos >= s->slice_len)
            s->loop_pos = 0;
        return y;
    }
    case PERF_FREEZE: {
        uint32_t pos = s->loop_pos, L = s->slice_len, X = s->wrap;
        float y = i2f16(p->hist[(s->slice_start + pos) & HIST_MASK]);
        if (X && pos + X >= L) {                 /* tail morphs into the head */
            uint32_t j = pos + X - L;
            float g = (float)j * (1.0f / (float)X);
            float head = i2f16(p->hist[(s->slice_start + j) & HIST_MASK]);
            y = y * (1.0f - g) + head * g;
        }
        if (++s->loop_pos >= L)
            s->loop_pos = X;                     /* do not replay the head */
        return y;
    }
    case PERF_TAPE: {
        uint32_t i0;
        float fr, b0, b1, y;
        if (s->degraded)                         /* nowhere to record: live signal */
            return mono;
        i0 = (uint32_t)s->tape_read;
        fr = s->tape_read - (float)i0;
        b0 = i2f16(p->hist[i0 & HIST_MASK]);
        b1 = i2f16(p->hist[(i0 + 1u) & HIST_MASK]);
        y = (b0 + (b1 - b0) * fr) * s->tape_rate;   /* sag to silence with the rate */
        s->tape_read += s->tape_rate;
        if (s->tape_read >= (float)PERFORM_HIST_N)
            s->tape_read -= (float)PERFORM_HIST_N;
        if (s->tape_rate > 0.0f) {
            s->tape_rate -= s->tape_dec;
            if (s->tape_rate < 0.0f)
                s->tape_rate = 0.0f;
        }
        return y;
    }
    default:
        return 0.0f;
    }
}

/* is w inside an active loop's protected slice? (circular) */
static int in_region(const perf_slot_t *s, int on, uint32_t w)
{
    if (!on || !is_loop(s->type) || s->slice_len == 0)
        return 0;
    return ((w - s->slice_start) & HIST_MASK) < s->slice_len;
}

/* the recording head skips active loop slices (at most two) */
static uint32_t skip_loops(perform_t *p, uint32_t w)
{
    int g = 0;
    while (g++ < 4) {
        if (in_region(&p->act, p->act_on, w)) {
            w = (p->act.slice_start + p->act.slice_len) & HIST_MASK;
            continue;
        }
        if (in_region(&p->old, p->old_on, w)) {
            w = (p->old.slice_start + p->old.slice_len) & HIST_MASK;
            continue;
        }
        break;
    }
    return w;
}

/* Promote the pending effect to act and start its engage crossfade. */
static void slot_swap_in(perform_t *p)
{
    p->old = p->act;
    p->old_on = p->act_on;
    p->gain_old = p->act_on ? p->gain_act : 0.0f;
    p->act = p->pend;
    p->act_on = 1;
    p->gain_act = 0.0f;
    p->pend_on = 0;
    p->fade_inc = 1.0f / (float)PERFORM_XFADE_IN;
    p->xmode = PERF_XF_IN;
}

static void engage_buffer(perform_t *p, int fx)
{
    perf_slot_t s;
    uint8_t *z = (uint8_t *)&s;
    unsigned k;
    for (k = 0; k < sizeof s; k++)
        z[k] = 0;
    s.type = (uint8_t)fx;
    if (fx == PERF_REPEAT_8 || fx == PERF_REPEAT_16) {
        s.state = PERF_ST_WAIT_GRID;      /* capture at the next grid, then loop */
        p->pend = s;
        p->pend_on = 1;
    } else if (fx == PERF_TAPE) {
        s.state = PERF_ST_RUN;
        s.degraded = p->hist_stalled;     /* no history room: play the live signal */
        s.tape_rate = 1.0f;
        s.tape_dec = 1.0f / ((float)PERF_TAPE_MS * 0.001f * (float)PERFORM_FS);
        s.tape_read = (float)((p->hist_w + PERFORM_HIST_N - 1u) & HIST_MASK);
        p->pend = s;
        p->pend_on = 1;
        slot_swap_in(p);
    } else {                              /* freeze */
        s.state = PERF_ST_RUN;
        capture_freeze(p, &s);
        p->pend = s;
        p->pend_on = 1;
        slot_swap_in(p);
    }
}

/* Filter coefficients from the dive amount: the cutoff sweeps linearly in Hz
 * from 16 kHz down to 200 Hz and Q from neutral to 12, so an engage never steps
 * the resonance in. No fm_exp2f: the sanitizer build flags the inherited
 * fastmath's 32-bit shift. */
static void filt_set(perform_t *p)
{
    float fc = FILT_FC_OPEN + (FILT_FC_LO - FILT_FC_OPEN) * p->filt_amt;
    float q = FILT_Q0 + (FILT_Q - FILT_Q0) * p->filt_amt;
    float g = fm_tanf(FM_PI * fc / (float)PERFORM_FS);
    p->filt_g = g;
    p->filt_k = 1.0f / q;
    p->filt_a1 = 1.0f / (1.0f + g * (g + p->filt_k));
}

static void engage_filter(perform_t *p)
{
    uint32_t beat = (uint32_t)(PERFORM_FS * 600u) / (uint32_t)p->bpm_x10;
    if (beat == 0u)
        beat = 1u;
    p->filt_on = 1;
    if (p->filt_amt >= 1.0f) {            /* re-engaged at full dive: just hold */
        p->filt_state = 2;
        p->filt_step = 0.0f;
    } else {                              /* continue from the current amount */
        p->filt_state = 1;
        p->filt_step = (1.0f - p->filt_amt) / (float)beat;
    }
    filt_set(p);                          /* the state is not reset: no click */
}

static void echo_apply(perform_t *p, int on)
{
    p->echo_on = on ? 1u : 0u;
    if (on) {
        p->echo_send = (uint8_t)PERFORM_ECHO_SEND;
        p->echo_fdbk = (uint8_t)PERFORM_ECHO_FDBK;
        p->echo_time = (uint8_t)PERFORM_ECHO_TIME;
    } else {
        p->echo_send = p->echo_base_send;
        p->echo_fdbk = p->echo_base_fdbk;
        p->echo_time = p->echo_base_time;
    }
    p->echo_changed = 1;
}

static void engage_fx(perform_t *p, int fx)
{
    if (is_buffer(fx))
        engage_buffer(p, fx);
    else if (fx == PERF_FILTER)
        engage_filter(p);
    else if (fx == PERF_ECHO)
        echo_apply(p, 1);
}

static void release_fx(perform_t *p, int fx)
{
    if (is_buffer(fx)) {
        if (p->pend_on && p->pend.type == (uint8_t)fx)
            p->pend_on = 0;
        if (p->act_on && p->act.type == (uint8_t)fx) {
            p->fade_inc = 1.0f / (float)PERFORM_XFADE_OUT;
            p->xmode = PERF_XF_OUT;
        }
        if (p->old_on && p->old.type == (uint8_t)fx) {
            /* the outgoing effect of a replacement: fade it, never cut it */
            if (p->xmode != PERF_XF_IN) {
                p->fade_inc = 1.0f / (float)PERFORM_XFADE_OUT;
                p->xmode = PERF_XF_OLD;
            }
        }
    } else if (fx == PERF_FILTER) {
        if (p->filt_on) {
            uint32_t qbeat = ((uint32_t)(PERFORM_FS * 600u) / (uint32_t)p->bpm_x10) / 4u;
            if (qbeat == 0u)
                qbeat = 1u;
            if (p->filt_amt <= 0.0f) {
                p->filt_state = 0;
            } else {                      /* sweep back over 1/4 beat */
                p->filt_state = 3;
                p->filt_step = -p->filt_amt / (float)qbeat;
            }
        }
    } else if (fx == PERF_ECHO) {
        echo_apply(p, 0);
    }
}

static void reset_state(perform_t *p)
{
    p->act_on = p->old_on = p->pend_on = 0;
    p->gain_act = p->gain_old = 0.0f;
    p->xmode = PERF_XF_NONE;
    p->filt_on = 0;
    p->filt_state = 0;
    p->filt_amt = 0.0f;
    p->filt_ic1[0] = p->filt_ic1[1] = p->filt_ic2[0] = p->filt_ic2[1] = 0.0f;
    p->echo_on = 0;
    p->echo_send = p->echo_base_send;
    p->echo_fdbk = p->echo_base_fdbk;
    p->echo_time = p->echo_base_time;
    p->echo_changed = 1;
    p->rel_req[PERF_REPEAT_8] = p->rel_req[PERF_REPEAT_16] = 0;
    p->rel_req[PERF_TAPE] = p->rel_req[PERF_FILTER] = 0;
    p->rel_req[PERF_ECHO] = p->rel_req[PERF_FREEZE] = 0;
    p->latch[PERF_REPEAT_8] = p->latch[PERF_REPEAT_16] = 0;
    p->latch[PERF_TAPE] = p->latch[PERF_FILTER] = 0;
    p->latch[PERF_ECHO] = p->latch[PERF_FREEZE] = 0;
}

/* ------------------------------------------------------------- ring / ISR --- */

static int ring_put(perform_t *p, uint8_t cmd, uint8_t fx, uint8_t latch)
{
    uint8_t head = p->cmd_head;
    uint8_t next = (uint8_t)((head + 1u) % PERFORM_CMD_RING);
    if (next == p->cmd_tail)
        return 0;                         /* full */
    p->cmd[head].cmd = cmd;
    p->cmd[head].fx = fx;
    p->cmd[head].latch = latch;
    p->cmd[head].bpm_x10 = 0;
    PERF_BARRIER();                       /* publish the slot before the head */
    p->cmd_head = next;
    return 1;
}

static void drain(perform_t *p)
{
    int fx;
    for (;;) {
        uint8_t head;
        perform_cmd_t c;
        PERF_BARRIER();                   /* read the head before the slot */
        head = p->cmd_head;
        if (p->cmd_tail == head)
            break;
        c = p->cmd[p->cmd_tail];
        p->cmd_tail = (uint8_t)((p->cmd_tail + 1u) % PERFORM_CMD_RING);
        switch (c.cmd) {
        case PERF_CMD_ENGAGE:
            if (c.fx < PERF_NFX) {
                if (c.latch && p->latch[c.fx]) {    /* a tap on a latch releases it */
                    p->latch[c.fx] = 0;
                    release_fx(p, c.fx);
                } else {
                    p->latch[c.fx] = c.latch ? 1u : p->latch[c.fx];
                    engage_fx(p, c.fx);
                }
            }
            break;
        case PERF_CMD_RELEASE:
            if (c.fx < PERF_NFX) {
                p->latch[c.fx] = 0;
                release_fx(p, c.fx);
            }
            break;
        case PERF_CMD_RESET:
            reset_state(p);
            break;
        default:
            break;
        }
    }
    /* the ring was full: a release or a reset overrides, and the tempo is the
     * latest value (coalesced) */
    for (fx = 0; fx < PERF_NFX; fx++)
        if (p->rel_req[fx]) {
            p->rel_req[fx] = 0;
            p->latch[fx] = 0;
            release_fx(p, fx);
        }
    if (p->reset_req) {
        p->reset_req = 0;
        p->cmd_tail = p->cmd_head;        /* the stale queued commands go too */
        reset_state(p);
    }
    if (p->tempo_pending) {
        uint16_t b = p->tempo_req;
        p->tempo_pending = 0;
        if (b < 200u) b = 200u;
        if (b > 3000u) b = 3000u;
        p->bpm_x10 = b;
    }
}

/* --------------------------------------------------------------- public --- */

void perform_init(perform_t *p)
{
    unsigned k;
    uint8_t *z = (uint8_t *)p;
    for (k = 0; k < sizeof *p; k++)
        z[k] = 0;
    p->bpm_x10 = 1200u;
    p->echo_base_send = 0u;
    p->echo_base_fdbk = 52u;      /* X0X's default DlFdbk */
    p->echo_base_time = 7u;       /* X0X's default DlTime ("1/8.") */
    p->echo_send = p->echo_base_send;
    p->echo_fdbk = p->echo_base_fdbk;
    p->echo_time = p->echo_base_time;
    p->echo_changed = 1;
    filt_set(p);
}

void perform_engage(perform_t *p, int fx, int latch)
{
    if (fx < 0 || fx >= PERF_NFX)
        return;
    ring_put(p, PERF_CMD_ENGAGE, (uint8_t)fx, latch ? 1u : 0u);
}

void perform_release(perform_t *p, int fx)
{
    if (fx < 0 || fx >= PERF_NFX)
        return;
    if (!ring_put(p, PERF_CMD_RELEASE, (uint8_t)fx, 0u))
        p->rel_req[fx] = 1;               /* never dropped */
}

void perform_reset(perform_t *p)
{
    if (!ring_put(p, PERF_CMD_RESET, 0u, 0u))
        p->reset_req = 1;                 /* never dropped */
}

void perform_set_tempo(perform_t *p, uint16_t bpm_x10)
{
    p->tempo_req = bpm_x10;               /* coalesced to the latest value */
    p->tempo_pending = 1;
}

void perform_echo_throw(perform_t *p, int on)
{
    if (on)
        ring_put(p, PERF_CMD_ENGAGE, (uint8_t)PERF_ECHO, 0u);
    else
        perform_release(p, PERF_ECHO);
}

void perform_echo_base(perform_t *p, uint8_t send, uint8_t fdbk, uint8_t time)
{
    p->echo_base_send = send;
    p->echo_base_fdbk = fdbk;
    p->echo_base_time = time;
    if (!p->echo_on) {
        p->echo_send = send;
        p->echo_fdbk = fdbk;
        p->echo_time = time;
    }
    p->echo_changed = 1;
}

int perform_echo_targets(perform_t *p, perform_echo_t *out)
{
    PERF_BARRIER();
    if (!p->echo_changed)
        return 0;
    p->echo_changed = 0;                  /* clear BEFORE the reads: a toggle in the ISR between
                                           * the read and the clear would otherwise be lost */
    PERF_BARRIER();
    out->send = p->echo_send;
    out->fdbk = p->echo_fdbk;
    out->time = p->echo_time;
    out->on = p->echo_on;
    return 1;
}

int perform_latched(const perform_t *p, int fx)
{
    return (fx >= 0 && fx < PERF_NFX) ? (int)p->latch[fx] : 0;
}

/* Crossfade advance: one call per sample, after the sample is produced. */
static void advance_fades(perform_t *p)
{
    if (p->xmode == PERF_XF_IN) {
        p->gain_act += p->fade_inc;
        if (p->gain_act >= 1.0f)
            p->gain_act = 1.0f;
        if (p->old_on) {
            p->gain_old -= p->fade_inc;
            if (p->gain_old <= 0.0f) {
                p->gain_old = 0.0f;
                p->old_on = 0;
            }
        }
        if (p->gain_act >= 1.0f && !p->old_on)
            p->xmode = PERF_XF_NONE;
    } else if (p->xmode == PERF_XF_OUT) {
        if (p->gain_act > 0.0f) {
            p->gain_act -= p->fade_inc;
            if (p->gain_act < 0.0f)
                p->gain_act = 0.0f;
        }
        if (p->old_on) {
            p->gain_old -= p->fade_inc;
            if (p->gain_old <= 0.0f) {
                p->gain_old = 0.0f;
                p->old_on = 0;
            }
        }
        if (p->gain_act == 0.0f && !p->old_on) {
            p->xmode = PERF_XF_NONE;
            p->act_on = 0;
        }
    } else if (p->xmode == PERF_XF_OLD) {
        if (p->old_on) {
            p->gain_old -= p->fade_inc;
            if (p->gain_old <= 0.0f) {
                p->gain_old = 0.0f;
                p->old_on = 0;
            }
        }
        if (!p->old_on)
            p->xmode = PERF_XF_NONE;
    }
}

void perform_process(perform_t *p, float *l, float *r, uint32_t nframes,
                     uint32_t next_8, uint32_t next_16)
{
    uint32_t i;
    drain(p);
    for (i = 0; i < nframes; i++) {
        float ll = l[i], rr = r[i];
        float mono = (ll + rr) * 0.5f;
        float wet = 0.0f, base_l, base_r, a;
        uint32_t w;

        /* 1. the recording head writes the live signal, skipping the active
         *    loop's slice so a loop does not stop the history */
        w = skip_loops(p, p->hist_w);
        if (in_region(&p->act, p->act_on, w) || in_region(&p->old, p->old_on, w)) {
            p->hist_stalled = 1;          /* a loop fills the buffer: no room */
        } else {
            p->hist[w] = f2i16(mono);
            p->hist_w = (w + 1u) & HIST_MASK;
            p->hist_stalled = 0;
        }

        /* 2. a repeat waiting for its grid captures one slice and loops it */
        if (p->pend_on && p->pend.state == PERF_ST_WAIT_GRID) {
            uint32_t g = (p->pend.type == PERF_REPEAT_16) ? next_16 : next_8;
            if (g == PERFORM_NO_GRID)
                g = 0;                     /* stopped transport: freeze the last period */
            if (g == i) {
                capture_repeat(p, &p->pend);
                p->pend.state = PERF_ST_RUN;
                slot_swap_in(p);
            }
        }

        /* 3. buffer effect wet, crossfaded against the live signal */
        a = p->gain_act + p->gain_old;
        if (a <= 0.0f) {
            base_l = ll;
            base_r = rr;
        } else {
            if (p->gain_act > 0.0f && p->act_on)
                wet += slot_wet(p, &p->act, mono) * p->gain_act;
            if (p->gain_old > 0.0f && p->old_on)
                wet += slot_wet(p, &p->old, mono) * p->gain_old;
            {
                float ia = 1.0f - a;
                base_l = ll * ia + wet;
                base_r = rr * ia + wet;
            }
        }

        /* 4. filter dive: a resonant low-pass on the whole master signal */
        if (p->filt_state != 0) {
            if (p->filt_state == 1) {                 /* diving */
                p->filt_amt += p->filt_step;
                if (p->filt_amt >= 1.0f) {
                    p->filt_amt = 1.0f;
                    p->filt_state = 2;                /* hold at 200 Hz */
                }
            } else if (p->filt_state == 3) {          /* sweeping back */
                p->filt_amt += p->filt_step;
                if (p->filt_amt <= 0.0f) {
                    p->filt_amt = 0.0f;
                    p->filt_state = 0;
                }
            }
            if ((p->filt_cr++ & (FILT_CR - 1u)) == 0u || p->filt_state == 0)
                filt_set(p);
        }
        if (p->filt_state != 0) {
            float in_l = base_l, in_r = base_r;
            float amt, g = p->filt_g, a1 = p->filt_a1;
            float v3 = in_l - p->filt_ic2[0];
            float v1 = a1 * p->filt_ic1[0] + g * a1 * v3;
            float v2 = p->filt_ic2[0] + g * v1;
            float fl;
            p->filt_ic1[0] = fm_flush(2.0f * v1 - p->filt_ic1[0]);
            p->filt_ic2[0] = fm_flush(2.0f * v2 - p->filt_ic2[0]);
            fl = v2;
            v3 = in_r - p->filt_ic2[1];
            v1 = a1 * p->filt_ic1[1] + g * a1 * v3;
            v2 = p->filt_ic2[1] + g * v1;
            p->filt_ic1[1] = fm_flush(2.0f * v1 - p->filt_ic1[1]);
            p->filt_ic2[1] = fm_flush(2.0f * v2 - p->filt_ic2[1]);
            /* blend the filtered signal in by the dive amount so the engage and
             * release hand-offs cannot step, whatever the filter's state */
            amt = p->filt_amt;
            base_l = in_l * (1.0f - amt) + fl * amt;
            base_r = in_r * (1.0f - amt) + v2 * amt;
        }

        l[i] = base_l;
        r[i] = base_r;

        advance_fades(p);
    }
}
