/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2012 Google Inc.
 * Copyright 2016-2025 Pascal Gauthier.
 * C port for Felucca (integer only, no float), 2026: Leo Kuroshita (@kurogedelic), Hügelton Instruments
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/* Ported from Felucca (hugelton/Felucca a4d7ca2) to fm1-rocket-os as firmware/src/dsp/fm6_core.c.
 * See LICENSES/Apache-2.0-msfa.txt. The synthesis is unchanged from Felucca's fm6_core.c; this header
 * only moves the types and the entry points the X0X adapter (eng_fm6.c) and the parity test need into
 * view. The algorithmic body stays verbatim so the port is bit-identical to the original. */
#pragma once
#include <stdint.h>
#include "fm6_tables.h"

#define FM6_N CTL
#define FM6_LG_N 5
#if (1 << FM6_LG_N) != FM6_N
#error "FM6_LG_N does not match CTL"
#endif

/* the 155-byte patch: 6 x 21 operator bytes (the sixth operator first), then the voice */
enum {
    FP_R1 = 0, FP_L1 = 4, FP_BP = 8, FP_LD, FP_RD, FP_LC, FP_RC, FP_RS, FP_AMS, FP_KVS, FP_OL, FP_MODE, FP_FC, FP_FF,
    FP_DET, FP_OP = 21,
    FP_PR1 = 126, FP_PL1 = 130, FP_ALG = 134, FP_FB, FP_OKS, FP_LFS, FP_LFD, FP_LPMD, FP_LAMD, FP_LKS, FP_LFW,
    FP_LPMS, FP_TRNSP, FP_NAME, FP_SIZE = 155
};

typedef struct {                 /* msfa Env: level in Q24 log2 (2^24 = 6 dB), 16 more bits than the original */
    int32_t level, target, inc, statc;
    int16_t outlevel;            /* microsteps (99 * 32 = full) */
    uint8_t rscale, ix, rising, down;
} fm6_env_t;

typedef struct {
    int32_t level, target, inc;
    uint8_t ix, rising, down, rsv;
} fm6_penv_t;

typedef struct {
    uint32_t phase, delta, dstate, dinc, dinc2;
    uint8_t wave, rnd, sync, rsv;
} fm6_lfo_t;

typedef struct {                 /* msfa FmOpParams */
    int32_t level_in;            /* Q24 log2 */
    int32_t gain_out;            /* Q24 */
    int32_t freq;                /* Q24 cycles per sample */
    int32_t phase;               /* Q24 */
} fm6_op_t;

typedef struct {                 /* a sounding note (msfa's note): 244 bytes */
    fm6_env_t env[6];
    fm6_op_t op[6];
    int32_t base[6];             /* the operator's pitch: ratio: log2 offset from the note's (Q24); fixed: log2 Hz */
    int32_t fb[2];
    fm6_penv_t penv;
    uint8_t fixed;               /* bit k: operator k has a fixed frequency */
    uint8_t live, down, rsv;
    int32_t dc;                  /* (eng_fm6.c) the output's DC blocker */
} fm6_note_t;

/* the LFO (once a block) */
void fm6_lfo_reset(fm6_lfo_t *l, const uint8_t *p);
int32_t fm6_lfo_sample(fm6_lfo_t *l);
int32_t fm6_lfo_delay(fm6_lfo_t *l);
void fm6_lfo_key(fm6_lfo_t *l);

/* a note (msfa note): set up, key down/up, one CTL-sample block, and drop-when-done */
void fm6_note_init(fm6_note_t *n, const uint8_t *p, int32_t note, int32_t vel, int fresh);
void fm6_note_key(fm6_note_t *n, const uint8_t *p, int down);
int32_t fm6_note_logfreq(int32_t pitch16, int32_t fine);
int fm6_note_compute(fm6_note_t *n, const uint8_t *p, int32_t *out, int32_t lfo_val, int32_t lfo_delay,
                     int32_t logfreq, uint32_t alg, int32_t fb, const int32_t *dt, int32_t modlvl);
int fm6_note_done(const fm6_note_t *n, const uint8_t *p, uint32_t alg);

/* bit k: operator k (the sixth first) is a carrier of algorithm a (0..31) */
uint32_t fm6_carriers(uint32_t a);
