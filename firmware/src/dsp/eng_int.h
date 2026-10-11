/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * Ported from Felucca (hugelton/Felucca a4d7ca2)
 * fm1-rocket-os: the fixed-point building blocks the ported engines share (Felucca's dsp.c). */
/* A slice of Felucca's firmware/src/dsp.c: only the helpers eng_fm6.c / eng_noise.c read. Integer only
 * (no float, no double, no 64-bit division). The arithmetic is Felucca's, unchanged, so the ported
 * engines stay bit-identical. */
#pragma once
#include <stdint.h>
#include "fm6_tables.h"          /* SINE, TANH_Q15, CTL */

#define VOICE_FS 24000           /* per-voice level: one voice peaks near -6 dBFS before the master */
#define CTL_LOG2 5
#if (1 << CTL_LOG2) != CTL
#error "CTL_LOG2 does not match CTL"
#endif

static inline int32_t mulq15(int32_t a, int32_t b) { return (a * b) >> 15; }
static inline int32_t clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

/* xorshift32 */
static inline uint32_t noise32(int32_t *st)
{
    uint32_t s = (uint32_t)*st;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    *st = (int32_t)s;
    return s;
}

/* tanh(x / 32768) * 32767, linearly interpolated (Felucca dsp.c softclip) */
static inline int32_t softclip(int32_t x)
{
    int32_t a = x < 0 ? -x : x, i = a >> 8, y;
    if (i >= 256)
        y = TANH_Q15[256];
    else
        y = TANH_Q15[i] + (((TANH_Q15[i + 1] - TANH_Q15[i]) * (a & 255)) >> 8);
    return x < 0 ? -y : y;
}

/* linear up to k, then a soft knee: only the peaks above k saturate */
static inline int32_t soft_knee(int32_t y, int32_t k)
{
    int32_t a = y < 0 ? -y : y;
    if (a <= k)
        return y;
    a = k + (softclip((a - k) * 2) >> 1);
    return y < 0 ? -a : a;
}
