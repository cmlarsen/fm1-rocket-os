/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * Ported from Felucca (hugelton/Felucca a4d7ca2)
 * fm1-rocket-os: the X0X adapter around Felucca's NOISE engine. */
#include "eng_noise.h"
#include "eng_int.h"
#include "eng_mem.h"

enum { NZ_ANLG, NZ_DUST, NZ_LFSR, NZ_META };
enum { P_E0, P_E1, P_E2, P_E3, P_E4, P_E5, P_E6, P_E7 };

/* Galois taps of the long registers (maximal length: 2^n - 1 clocks) */
static const uint32_t NOISE_TAPS[8] = {0x420000u, 0x90000u, 1u << 16 | 1u << 13, 0x6000u, 0x100Du, 0x500u, 0x110u, 0x60u};

/* META: one period of a register's output bit (bit i of the words, from the state 1), its length,
 * and the levels of a 1 and a 0 (no DC, the larger at 16384). */
static const struct { uint32_t w[4]; uint8_t len; int16_t hi, lo; } NOISE_SEQ[8] = {
    {{0xD13C50C1u, 0x91C2F95Cu, 0xA58DED6Cu, 0x01FD533Bu}, 127, 16128, -16384},
    {{0xDBD165F1u, 0x9E46A1FDu, 0x84C15692u, 0x019B1CEBu}, 127, 16128, -16384},
    {{0x41008001u, 0x00492402u, 0x00824824u, 0x00000000u}, 93, 16384, -3404},
    {{0x27179461u, 0x03F566EDu, 0x00000000u, 0x00000000u}, 63, 15872, -16384},
    {{0x75A6C487u, 0x02FCA8CFu, 0x00000000u, 0x00000000u}, 63, 15872, -16384},
    {{0x05763E69u, 0x00000000u, 0x00000000u, 0x00000000u}, 31, 15360, -16384},
    {{0x00000F59u, 0x00000000u, 0x00000000u, 0x00000000u}, 15, 14336, -16384},
    {{0x0000001Du, 0x00000000u, 0x00000000u, 0x00000000u}, 7, 12288, -16384},
};

/* phase increment of pitch p (1/16 semitone) past the table's top, up to one step a sample */
static uint32_t noise_inc(int32_t p)
{
    uint32_t sh = 0, inc;
    while (p > 2047) {
        p -= 192;
        sh++;
    }
    inc = pitch_inc((uint32_t)clamp(p, 0, 2047));
    return sh && inc >= 0x80000000u >> (sh - 1u) ? 0xFFFFFFFFu : inc << sh;
}

static uint32_t isqrt32(uint32_t x)
{
    uint32_t r = 0, b = 1u << 30;
    while (b > x)
        b >>= 2;
    while (b) {
        if (x >= r + b) {
            x -= r + b;
            r = (r >> 1) + b;
        } else {
            r >>= 1;
        }
        b >>= 2;
    }
    return r;
}

/* Felucca's noise_note_on, with the note and the fine factor as arguments */
void eng_noise_seed(eng_noise_state_t *v, int note, int fine)
{
    uint32_t seed = (uint32_t)(note + 1) * 0x9E3779B9u ^ (uint32_t)fine * 0x85EBCA6Bu;   /* UNISON: each its own */
    v->s[0] = v->s[1] = 0;
    v->s[2] = (int32_t)(seed ? seed : 1u);
    v->s[3] = 0;                                      /* a new drift target at once */
    v->s[4] = v->s[5] = v->s[6] = v->s[7] = 0;
    v->ph[0] = 0;
    v->ph[1] = 0;
    v->ph[2] = 0;
}

/* the amplitude ramp (Felucca dsp.c amp_at / voice_amp) */
static inline int32_t amp_at(const eng_noise_vmod_t *m, uint32_t i)
{
    int32_t x = (m->amp1 - m->amp0) * (int32_t)i;
    return m->amp0 + ((x + ((x >> 31) & (CTL - 1))) >> CTL_LOG2);
}
static inline int32_t voice_amp(int32_t s, const eng_noise_vmod_t *m, uint32_t i)
{
    return mulq15(mulq15(s, amp_at(m, i)), VOICE_FS);
}

/* ---------------------------------------------------- Felucca noise_render (verbatim) --- */
void eng_noise_dsp(const int16_t *p, eng_noise_state_t *v, int32_t *out, uint32_t n, const eng_noise_vmod_t *m)
{
    uint32_t mode = (uint32_t)p[P_E0] & 3u, sel = ((uint32_t)p[P_E5] >> 4) & 7u, i;
    int32_t res = p[P_E3], drft = p[P_E6], crsh = mode < NZ_LFSR ? p[P_E7] : 0;
    int32_t ic1 = v->s[0], ic2 = v->s[1], rng = v->s[2], b0 = v->s[4], b1 = v->s[5], b2 = v->s[6], br = v->s[7];
    uint32_t ph = v->ph[0], reg = v->ph[1];         /* state in locals: out[] may alias v->s[] */
    int32_t drift = (int32_t)v->ph[2], d16 = 0, gain = 32767, gq, colr, cut, k, a1, a2, a3, g, bpg, mix, cf, thr = 0;
    uint32_t inc = 0, taps = NOISE_TAPS[sel];
    /* drift: a new random target every 0.7..3 s, followed with a 0.7 s lag */
    if (drft) {
        int32_t tg = v->s[3] >> 16, cnt = v->s[3] & 0xFFFF;
        if (--cnt <= 0) {
            uint32_t r = noise32(&rng);
            tg = (int32_t)(r >> 16) - 32768;
            cnt = 1024 + (int32_t)(r & 3071u);
        }
        v->s[3] = (int32_t)((uint32_t)tg << 16 | (uint32_t)cnt);
        drift += ((tg << 4) - drift) >> 10;           /* Q19 */
        d16 = ((drift >> 11) * drft * drft) >> 14;    /* up to about +-16 semitones */
        gain = 32767 - ((((32767 - (drift >> 4)) >> 1) * drft) >> 8);   /* down to -6 dB as it falls */
    }
    /* COLR (+ SHP), 0..127 << 8: white -> pink -> brown */
    colr = clamp((p[P_E1] << 8) + m->shape - (64 << 8), 0, 127 << 8);
    cf = colr <= (64 << 8) ? colr << 1 : ((colr - (64 << 8)) * 520) >> 8;   /* the crossfade, Q15 */
    /* FREQ (+ FLT), key-tracked by TRK around C4, the drift: SVF cutoff units (256 an index, 0.86 st) */
    cut = (p[P_E2] << 8) + m->cutoff + (((m->pitch16 - 60 * 16) * p[P_E4] * 151) >> 10) + ((d16 * 4785) >> 8);
    cut = clamp(cut, 0, 127 << 8);
    g = SVF_G[cut >> 8];
    if (cut < (127 << 8))
        g += ((SVF_G[(cut >> 8) + 1] - g) * (cut & 255)) >> 8;
    k = 8192 - res * 7800 / 127;                      /* damping, Q12: 2.0 .. 0.1 */
    a1 = (int32_t)((4096u << 13) / (uint32_t)(4096 + ((g * (g + k)) >> 12)));
    a2 = (a1 * g) >> 12;
    a3 = (a2 * g) >> 12;
    bpg = (int32_t)isqrt32((uint32_t)k << 12);        /* band-pass x 1.5 sqrt(damping): its level holds with the Q */
    mix = res * 258;                                  /* low-pass -> band-pass, Q15 */
    {   /* level: make up most of what the filter takes from white noise (less for pink and brown), +12 dB for
         * the sparse DUST, the drift's level; Q12, mk in 1/256 octave: 2^(mk / 256), at most x 16 */
        int32_t wf = 32767 - ((colr * 230) >> 8), mk = (((((127 << 8) - cut) >> 8) * 7) * wf) >> 15;
        mk += 214 + (mode == NZ_DUST ? 512 : 0);
        if (mk > 1279)
            mk = 1279;
        gq = (4096 + ((mk & 255) << 4)) << (mk >> 8);
        gq = (gq * (gain >> 3)) >> 12;
        if (gq > 65535)
            gq = 65535;
    }
    if (mode <= NZ_DUST && p[P_E5]) {                 /* impulses: 1 .. 2000 a second */
        uint32_t e = (uint32_t)p[P_E5] * 22u, r = 1u << (e >> 8);   /* 2^(DENS * 11 / 127), Q8 fraction */
        r += (r * (e & 255u)) >> 8;
        thr = (int32_t)((r * 97391u) >> 16);          /* per 2^16 of a sample: rate * 65536 / FS, Q16 */
    }
    if (mode == NZ_LFSR) {
        uint32_t mask = taps;                         /* the register's own bits (MODE / LEN changes) */
        inc = noise_inc(m->pitch16 + p[P_E7] * 16 + d16);
        mask |= mask >> 1;
        mask |= mask >> 2;
        mask |= mask >> 4;
        mask |= mask >> 8;
        mask |= mask >> 16;
        if (!(reg & mask))                            /* a new note: the register from the note's seed */
            reg = (uint32_t)rng | 1u;
        reg &= mask;
    } else if (mode == NZ_META) {
        inc = p[P_E7] || d16 ? noise_inc(m->pitch16 + p[P_E7] * 16 + d16) : m->inc;
    }
    for (i = 0; i < n; i++) {
        int32_t w, x, v1, v2, v3, y;
        switch (mode) {
        case NZ_DUST:
        case NZ_ANLG: {
            uint32_t r = noise32(&rng);
            w = mode == NZ_ANLG ? (int32_t)(r >> 17) - 16384 : 0;
            if ((int32_t)(r & 0xFFFFu) < thr) {       /* an impulse, random size and sign */
                int32_t a = 8192 + (int32_t)((r >> 18) & 0x7FFFu);
                w += (r & 0x20000u) ? a : -a;
            }
            break;
        }
        case NZ_LFSR: {
            uint32_t o = ph;
            ph += inc;
            if (ph < o)
                reg = (reg >> 1) ^ (-(reg & 1u) & taps);
            w = (reg & 1u) ? 12000 : -12000;
            break;
        }
        default: {
            uint32_t j = ((ph >> 16) * NOISE_SEQ[sel].len) >> 16;
            ph += inc;
            w = (NOISE_SEQ[sel].w[j >> 5] >> (j & 31u)) & 1u ? NOISE_SEQ[sel].hi : NOISE_SEQ[sel].lo;
            break;
        }
        }
        x = w;
        if (colr) {                                   /* pink: three leaky poles (Kellet), brown: a leaky integrator */
            int32_t pk;
            b0 = ((b0 * 32690) >> 15) + ((w * 3245) >> 15);
            b1 = ((b1 * 31555) >> 15) + ((w * 9716) >> 15);
            b2 = ((b2 * 18678) >> 15) + ((w * 17247) >> 14);
            pk = (b0 + b1 + b2 + ((w * 6055) >> 15)) >> 2;
            if (colr <= (64 << 8)) {
                x = w + mulq15(pk - w, cf);
            } else {
                br = ((br * 32580) >> 15) + ((w * 3480) >> 15);
                x = pk + mulq15(br - pk, cf);
            }
        }
        /* the SVF (dsp.c tsvf_lp, with the band-pass), rounded: at low cutoffs the floor of the shifts
         * walked the integrators off zero (DC) */
        v3 = x - ic2;
        v1 = (a1 * ic1 + a2 * v3 + 4096) >> 13;
        v2 = ic2 + ((a2 * ic1 + a3 * v3 + 4096) >> 13);
        ic1 = clamp(2 * v1 - ic1, -150000, 150000);
        ic2 = clamp(2 * v2 - ic2, -150000, 150000);
        v1 = (v1 * bpg * 3) >> 13;
        y = v2 + (((v1 - v2) * (mix >> 3)) >> 12);
        y = soft_knee(((y >> 4) * gq) >> 8, 24000);
        if (mode == NZ_META) {                        /* the knee bends a lopsided tone: a DC blocker, ~14 Hz */
            reg += (uint32_t)(((y << 8) - (int32_t)reg) >> 9);
            y -= (int32_t)reg >> 8;
        }
        if (crsh) {                                   /* hold 1..16 samples, 16 .. 4 bits */
            if ((int32_t)--ph <= 0 || ph > 16u) {
                ph = 1u + (uint32_t)crsh / 8u;
                reg = (uint32_t)(y >> (crsh / 11) << (crsh / 11));
            }
            y = (int32_t)reg;
        }
        out[i] += voice_amp(y, m, i);
    }
    v->s[0] = ic1;
    v->s[1] = ic2;
    v->s[2] = rng;
    v->s[4] = b0;
    v->s[5] = b1;
    v->s[6] = b2;
    v->s[7] = br;
    v->ph[0] = ph;
    v->ph[1] = reg;
    v->ph[2] = (uint32_t)drift;
}

/* ------------------------------------------------------------------ the engine --- */
/* {MODE, COLR, FREQ, RES, TRK, DENS, DRFT, CRSH}: Felucca's WIND preset, the power-on sound */
static const int16_t NZ_DEF[8] = {NZ_ANLG, 64, 70, 100, 127, 0, 70, 0};

typedef struct {
    eng_noise_state_t st;
    int32_t amp0;                     /* last block's amplitude (Q15) */
    uint32_t age;
    uint8_t note, vel, stage;         /* 0 off, 1 attack, 2 decay, 3 sustain, 4 release */
    int32_t env;                      /* Q15 */
} eng_noise_voice_t;

static int16_t nz_p[8];
static eng_noise_voice_t nz_v[ENG_NOISE_VOICES];
static uint32_t nz_age;
static int32_t nz_tail[CTL];
static uint32_t nz_tail_n;
static int32_t nz_atk, nz_dec, nz_sus, nz_rel;   /* per CTL block, Q15 */
static int nz_sus_lvl;

static int32_t env_step(int ms)
{
    int32_t n = ms * FS / 1000 / CTL;
    return n < 1 ? 32767 : 32767 / n;
}

void eng_noise_init(void)
{
    eng_memcpy(nz_p, NZ_DEF, sizeof nz_p);
    eng_memset(nz_v, 0, sizeof nz_v);
    nz_age = 0;
    nz_tail_n = 0;
    eng_noise_set_env(5, 200, 80, 200);
}

void eng_noise_set_env(int atk_ms, int dec_ms, int sus_pct, int rel_ms)
{
    nz_atk = env_step(atk_ms < 0 ? 0 : atk_ms);
    nz_dec = env_step(dec_ms < 0 ? 0 : dec_ms);
    nz_rel = env_step(rel_ms < 0 ? 0 : rel_ms);
    nz_sus_lvl = sus_pct < 0 ? 0 : sus_pct > 100 ? 100 : sus_pct;
    nz_sus = 32767 * nz_sus_lvl / 100;
}

void eng_noise_set_param(int i, int value)
{
    if (i < 0 || i >= E_NZ_NEDIT)
        return;
    nz_p[i] = (int16_t)clamp(value, 0, 127);
}

int eng_noise_param(int i) { return i >= 0 && i < E_NZ_NEDIT ? nz_p[i] : 0; }

void eng_noise_param_range(int i, int *lo, int *hi)
{
    if (lo) *lo = 0;
    if (hi) *hi = i == E_NZ_MODE ? 3 : 127;
}

static eng_noise_voice_t *nz_alloc(void)
{
    int i, best = -1;
    for (i = 0; i < ENG_NOISE_VOICES; i++)
        if (!nz_v[i].stage)
            return &nz_v[i];
    for (i = 0; i < ENG_NOISE_VOICES; i++)               /* a releasing voice first, then the oldest */
        if (nz_v[i].stage == 4 && (best < 0 || nz_v[i].age < nz_v[best].age))
            best = i;
    if (best < 0)
        for (i = 0; i < ENG_NOISE_VOICES; i++)
            if (best < 0 || nz_v[i].age < nz_v[best].age)
                best = i;
    return best < 0 ? 0 : &nz_v[best];
}

void eng_noise_note_on(int note, int vel)
{
    eng_noise_voice_t *v = nz_alloc();
    if (!v)
        return;
    eng_noise_seed(&v->st, note, 0);
    v->note = (uint8_t)clamp(note, 0, 127);
    v->vel = (uint8_t)clamp(vel, 0, 127);
    v->stage = 1;
    v->env = 0;
    v->amp0 = 0;
    v->age = ++nz_age;
}

void eng_noise_note_off(int note)
{
    int i;
    for (i = 0; i < ENG_NOISE_VOICES; i++)
        if (nz_v[i].stage && nz_v[i].stage != 4 && nz_v[i].note == (uint8_t)note)
            nz_v[i].stage = 4;
}

void eng_noise_all_off(void)
{
    int i;
    for (i = 0; i < ENG_NOISE_VOICES; i++)
        if (nz_v[i].stage && nz_v[i].stage != 4)
            nz_v[i].stage = 4;
}

int eng_noise_voices_active(void)
{
    int i, n = 0;
    for (i = 0; i < ENG_NOISE_VOICES; i++)
        n += nz_v[i].stage != 0;
    return n;
}

void eng_noise_render(int32_t *out, uint32_t n)
{
    uint32_t done = 0;
    int vi, i;
    while (done < n) {
        uint32_t take;
        if (!nz_tail_n) {
            for (i = 0; i < CTL; i++)
                nz_tail[i] = 0;
            for (vi = 0; vi < ENG_NOISE_VOICES; vi++) {
                eng_noise_voice_t *v = &nz_v[vi];
                eng_noise_vmod_t m;
                int32_t amp1;
                if (!v->stage)
                    continue;
                switch (v->stage) {
                case 1: v->env += nz_atk; if (v->env >= 32767) { v->env = 32767; v->stage = 2; } break;
                case 2: v->env -= nz_dec; if (v->env <= nz_sus) { v->env = nz_sus; v->stage = 3; } break;
                case 3: v->env = nz_sus; break;
                default: v->env -= nz_rel; if (v->env <= 0) { v->env = 0; v->stage = 0; } break;
                }
                if (!v->stage)
                    continue;
                amp1 = mulq15(v->env, v->vel * 258);
                m.inc = pitch_inc((uint32_t)clamp((int32_t)v->note * 16, 0, 2047));
                m.pitch16 = (int32_t)v->note * 16;
                m.amp0 = v->amp0;
                m.amp1 = amp1;
                m.cutoff = 0;
                m.shape = 64 << 8;
                eng_noise_dsp(nz_p, &v->st, nz_tail, CTL, &m);
                v->amp0 = amp1;
            }
            nz_tail_n = CTL;
        }
        take = n - done < nz_tail_n ? n - done : nz_tail_n;
        for (i = 0; i < (int)take; i++)
            out[done + i] += nz_tail[i];
        eng_memmove(nz_tail, nz_tail + take, (nz_tail_n - take) * sizeof nz_tail[0]);
        nz_tail_n -= take;
        done += take;
    }
}
