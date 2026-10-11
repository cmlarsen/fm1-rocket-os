/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * Ported from Felucca (hugelton/Felucca a4d7ca2)
 * fm1-rocket-os: the X0X adapter around Felucca's FM6 engine. */
#include "eng_fm6.h"
#include "eng_int.h"
#include "fm6_patches.h"          /* FM6_INIT, FM6_FACTORY (tools/gen_fm6_patches.py) */
#include "eng_mem.h"

#define FM6_VOICE_FS 24000                                  /* dsp.c VOICE_FS */

typedef struct {
    fm6_note_t n;
    int32_t pitch16;                                          /* 1/16 semitone */
    int32_t fine;                                             /* below 1/16 semitone, 1/4096 */
    int32_t amp0;                                             /* last block's amplitude (scaled, Q15*VOICE_FS) */
    uint32_t age;
    uint8_t note, vel, live, down;
} eng_fm6_voice_t;

static uint8_t fm6_patch[FP_SIZE + 1u];                        /* the own patch (main loop writes) */
static uint8_t fm6_slot;                                       /* the SLOT value last loaded */
static int16_t fm6_prm[E_FM6_NEDIT];                           /* the macros: ALG FB MLVL MRAT MEG VMOD DTUN */
static eng_fm6_voice_t fm6_voice[ENG_FM6_VOICES];
static fm6_lfo_t fm6_lfo;
static int32_t fm6_lfo_v, fm6_lfo_d;
static uint32_t fm6_age;
static const fm6_vbank_t *fm6_bank;                            /* the B slots' bank (0 = none) */
static int32_t fm6_tail[FM6_N];                                /* a rendered block not yet handed out */
static uint32_t fm6_tail_n;

static struct {                                                /* the patch through the macros */
    uint8_t p[FP_SIZE + 1u];
    uint8_t alg, fb;
    int32_t dt[6];
    uint8_t dirty;
} fm6_eff;

static void fm6_sync(void)                                     /* the patch through E0..E6 (Felucca fm6_sync) */
{
    const int16_t *e = fm6_prm;
    uint8_t *p = fm6_eff.p;
    uint32_t alg, car, k, j, ncar = 0;
    int32_t meg, step;
    if (!fm6_eff.dirty)
        return;
    eng_memcpy(fm6_eff.p, fm6_patch, FP_SIZE);
    alg = e[0] >= 1 && e[0] <= 32 ? (uint32_t)e[0] - 1u : p[FP_ALG] & 31u;
    fm6_eff.alg = (uint8_t)alg;
    fm6_eff.fb = (uint8_t)clamp(p[FP_FB] + e[1], 0, 7);
    car = fm6_carriers(alg);
    meg = clamp(e[4], -64, 63) * 40 / 64;                     /* MEG: rate steps (+ slower) */
    step = clamp(e[6], 0, 127) * (12 * 13981) / 127;          /* DTUN: up to 12 cents a step */
    for (k = 0; k < 6u; k++) {
        uint8_t *op = p + (5u - k) * FP_OP;                   /* OP1 first: carriers in their order */
        uint32_t ki = 5u - k;
        fm6_eff.dt[ki] = 0;
        if ((car >> ki) & 1u) {                               /* carriers 0, +1, -1, +2, -2, +3 steps apart */
            static const int8_t SPREAD[6] = {0, 1, -1, 2, -2, 3};
            fm6_eff.dt[ki] = SPREAD[ncar++] * step;
            continue;
        }
        if (!op[FP_MODE])
            op[FP_FC] = (uint8_t)clamp(op[FP_FC] + e[3], 0, 31);
        for (j = 0; j < 4u; j++)
            op[FP_R1 + j] = (uint8_t)clamp(op[FP_R1 + j] - meg, 0, 99);
        op[FP_KVS] = (uint8_t)clamp(op[FP_KVS] + e[5], 0, 7);
    }
    fm6_lfo_reset(&fm6_lfo, p);
    fm6_eff.dirty = 0;
}

void eng_fm6_init(void)
{
    uint8_t v[FP_SIZE + 1u];
    fm6_unpack(FM6_INIT, v);
    eng_memcpy(fm6_patch, v, FP_SIZE + 1u);
    eng_memset(fm6_prm, 0, sizeof fm6_prm);
    fm6_slot = ENG_FM6_OWN;
    fm6_eff.dirty = 1;
    eng_memset(fm6_voice, 0, sizeof fm6_voice);
    eng_memset(&fm6_lfo, 0, sizeof fm6_lfo);
    fm6_lfo_v = fm6_lfo_d = 0;
    fm6_age = 0;
    fm6_bank = 0;
    fm6_tail_n = 0;
}

void eng_fm6_set_patch(const uint8_t *v155)
{
    uint8_t s[FP_SIZE + 1u];
    eng_memcpy(s, v155, FP_SIZE);
    fm6_sanitize(s);
    eng_memcpy(fm6_patch, s, FP_SIZE + 1u);
    fm6_slot = ENG_FM6_OWN;
    fm6_eff.dirty = 1;
}

const uint8_t *eng_fm6_patch(void) { return fm6_patch; }

void eng_fm6_set_vbank(const fm6_vbank_t *b) { fm6_bank = b; }

int eng_fm6_load_slot(int s)
{
    uint8_t v[FP_SIZE + 1u];
    if (s >= 0 && s < FM6_NFACTORY) {
        fm6_unpack(FM6_FACTORY[s], v);
    } else if (s > ENG_FM6_OWN && s <= (int)(ENG_FM6_OWN + FM6_NBANK)) {
        uint32_t k = (uint32_t)(s - ENG_FM6_B1);
        if (!fm6_bank || fm6_vbank_read(fm6_bank, k, v))
            return 1;                                          /* a bank slot without a voice: refused */
    } else if (s != ENG_FM6_OWN) {
        return 1;
    } else {
        return 0;                                              /* OWN: the patch is already there */
    }
    eng_memcpy(fm6_patch, v, FP_SIZE + 1u);
    fm6_slot = (uint8_t)s;
    fm6_eff.dirty = 1;
    return 0;
}

int eng_fm6_slot(void) { return fm6_slot; }

void eng_fm6_set_param(int i, int value)
{
    if (i < 0 || i >= E_FM6_NEDIT)
        return;
    if (i == E_FM6_SLOT) {
        eng_fm6_load_slot(value);
        return;
    }
    fm6_prm[i] = (int16_t)value;
    fm6_eff.dirty = 1;
}

int eng_fm6_param(int i) { return i >= 0 && i < E_FM6_NEDIT ? fm6_prm[i] : 0; }

void eng_fm6_param_range(int i, int *lo, int *hi)
{
    static const int8_t L[E_FM6_NEDIT] = {0, -7, -64, -16, -64, -7, 0, 0};
    static const int8_t H[E_FM6_NEDIT] = {32, 7, 63, 16, 63, 7, 127, ENG_FM6_OWN + FM6_NBANK};
    if (i < 0 || i >= E_FM6_NEDIT) { if (lo) *lo = 0; if (hi) *hi = 0; return; }
    if (lo) *lo = L[i];
    if (hi) *hi = H[i];
}

/* Felucca's voice_alloc, trimmed: a free voice, then the one furthest through its release, then the
 * oldest. Deterministic (a fixed scan order), no heap. */
static eng_fm6_voice_t *fm6_alloc(void)
{
    int i, best = -1;
    for (i = 0; i < ENG_FM6_VOICES; i++)
        if (!fm6_voice[i].live)
            return &fm6_voice[i];
    for (i = 0; i < ENG_FM6_VOICES; i++)
        if (!fm6_voice[i].down && (best < 0 || fm6_voice[i].age < fm6_voice[best].age))
            best = i;
    if (best < 0)
        for (i = 0; i < ENG_FM6_VOICES; i++)
            if (best < 0 || fm6_voice[i].age < fm6_voice[best].age)
                best = i;
    return best < 0 ? 0 : &fm6_voice[best];
}

void eng_fm6_note_on(int note, int vel)
{
    eng_fm6_voice_t *v;
    const uint8_t *p;
    fm6_sync();
    v = fm6_alloc();
    if (!v)
        return;
    p = fm6_eff.p;
    fm6_note_init(&v->n, p, note + (int32_t)p[FP_TRNSP] - 24, vel, !v->live);
    v->pitch16 = clamp(note, 0, 127) * 16;
    v->fine = 0;
    v->note = (uint8_t)clamp(note, 0, 127);
    v->vel = (uint8_t)clamp(vel, 0, 127);
    v->live = v->down = 1;
    v->age = ++fm6_age;
    v->amp0 = 0;                                               /* a fresh voice: the amplitude ramps from 0 */
    fm6_lfo_key(&fm6_lfo);
}

void eng_fm6_note_off(int note)
{
    int i;
    fm6_sync();
    for (i = 0; i < ENG_FM6_VOICES; i++)
        if (fm6_voice[i].live && fm6_voice[i].down && fm6_voice[i].note == (uint8_t)note) {
            fm6_note_key(&fm6_voice[i].n, fm6_eff.p, 0);
            fm6_voice[i].down = 0;
        }
}

void eng_fm6_all_off(void)
{
    int i;
    fm6_sync();
    for (i = 0; i < ENG_FM6_VOICES; i++)
        if (fm6_voice[i].live && fm6_voice[i].down) {
            fm6_note_key(&fm6_voice[i].n, fm6_eff.p, 0);
            fm6_voice[i].down = 0;
        }
}

int eng_fm6_voices_active(void)
{
    int i, n = 0;
    for (i = 0; i < ENG_FM6_VOICES; i++)
        n += fm6_voice[i].live;
    return n;
}

void eng_fm6_render(int32_t *out, uint32_t n)
{
    int32_t bus[FM6_N];
    int32_t fb, lvl, a, da, k = ((int32_t)(((int64_t)32767 * FM6_VOICE_FS) >> 15));   /* mulq15(1.0, VOICE_FS) */
    uint32_t i, done = 0;
    int vi;
    fm6_sync();
    while (done < n) {
        uint32_t take;
        if (!fm6_tail_n) {
            for (i = 0; i < FM6_N; i++)
                fm6_tail[i] = 0;
            fm6_lfo_v = fm6_lfo_sample(&fm6_lfo);
            fm6_lfo_d = fm6_lfo_delay(&fm6_lfo);
            for (vi = 0; vi < ENG_FM6_VOICES; vi++) {
                eng_fm6_voice_t *v = &fm6_voice[vi];
                const uint8_t *p = fm6_eff.p;
                int32_t lf;
                if (!v->live)
                    continue;
                lf = fm6_note_logfreq(v->pitch16 + ((int32_t)p[FP_TRNSP] - 24) * 16, v->fine);
                fb = clamp(fm6_eff.fb, 0, 7);
                lvl = clamp(fm6_prm[E_FM6_MLVL], -64, 63) * 24;
                if (fm6_note_compute(&v->n, p, bus, fm6_lfo_v, fm6_lfo_d, lf, fm6_eff.alg, fb, fm6_eff.dt,
                                     lvl << 16)) {
                    a = v->amp0;
                    da = (k - a) >> FM6_LG_N;
                    for (i = 0; i < FM6_N; i++) {
                        int32_t x = bus[i] - v->n.dc;
                        v->n.dc += x >> 10;                    /* the one-pole high-pass, ~7 Hz */
                        a += da;
                        fm6_tail[i] += (int32_t)(((int64_t)x * a) >> 26);
                    }
                }
                v->amp0 = k;
                if (fm6_note_done(&v->n, p, fm6_eff.alg))
                    v->live = 0;
            }
            fm6_tail_n = FM6_N;
        }
        take = n - done < fm6_tail_n ? n - done : fm6_tail_n;
        for (i = 0; i < take; i++)
            out[done + i] += fm6_tail[i];
        eng_memmove(fm6_tail, fm6_tail + take, (fm6_tail_n - take) * sizeof fm6_tail[0]);
        fm6_tail_n -= take;
        done += take;
    }
}
