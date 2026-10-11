/* SPDX-License-Identifier: GPL-3.0-only
 * fm1-rocket-os T5c: the note script the FM6 parity test plays through both cores.
 * Include this after fm6_core.c (reference) or fm6_core.h (port) is in view. It drives only the core's
 * public entry points, so the same bytes come out when the two cores agree. The framing reproduces
 * Felucca's eng_fm6.c fm6_render: ownenv (amplitude 1.0), no macros, no matrix, one voice. */
#pragma once
#include <string.h>

#define FM6_TEST_VOICE_FS 24000          /* dsp.c VOICE_FS: Felucca's per-voice level */

/* p: 155-byte patch; note: MIDI note; vel: 0..127; blocks: CTL-sample blocks; out: blocks * FM6_N int32 */
static void fm6_script(const uint8_t *p, int note, int vel, uint32_t blocks, int32_t *out)
{
    fm6_note_t n;
    fm6_lfo_t lfo;
    int32_t bus[FM6_N], dt[6];
    int32_t lf = fm6_note_logfreq(note * 16 + ((int32_t)p[FP_TRNSP] - 24) * 16, 0);
    int32_t fb = p[FP_FB];
    uint32_t alg = (uint32_t)(p[FP_ALG] & 31u);
    int32_t amp1 = (int32_t)(((int64_t)32767 * FM6_TEST_VOICE_FS) >> 15);   /* mulq15(32767, VOICE_FS) */
    int32_t amp0 = 0, a, da;
    uint32_t b, i;

    memset(&n, 0, sizeof n);
    memset(&lfo, 0, sizeof lfo);
    memset(dt, 0, sizeof dt);
    fm6_lfo_reset(&lfo, p);
    fm6_note_init(&n, p, note + (int32_t)p[FP_TRNSP] - 24, vel, 1);
    fm6_lfo_key(&lfo);                          /* Felucca's fm6_note_on keys the LFO after the note */
    for (b = 0; b < blocks; b++) {
        int32_t lv = fm6_lfo_sample(&lfo), ld = fm6_lfo_delay(&lfo);
        a = amp0;
        da = (amp1 - amp0) >> FM6_LG_N;
        if (fm6_note_compute(&n, p, bus, lv, ld, lf, alg, fb, dt, 0)) {
            for (i = 0; i < FM6_N; i++) {
                int32_t x = bus[i] - n.dc;
                n.dc += x >> 10;                         /* the one-pole high-pass, as fm6_render */
                a += da;
                out[b * FM6_N + i] += (int32_t)(((int64_t)x * a) >> 26);
            }
        }
        amp0 = amp1;
    }
}
