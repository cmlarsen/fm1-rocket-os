/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * Ported from Felucca (hugelton/Felucca a4d7ca2)
 * fm1-rocket-os: the X0X adapter around Felucca's NOISE engine. */
/* eng_noise: the thin X0X adapter around the ported NOISE DSP (Felucca's eng_noise.c).
 *
 * MODE picks the source: ANLG (white + random impulses), DUST (impulses only), LFSR (a shift register
 * clocked above the key), META (one register period per key period). COLR tilts white -> pink -> brown;
 * FREQ / RES a trapezoidal SVF; TRK key-tracks FREQ; DRFT a slow random wander; CRSH sample-and-hold.
 * The DSP (eng_noise_dsp) is Felucca's noise_render, verbatim; this file adds init, note on/off with
 * velocity and a small ADSR, the eight parameters, and a block render into X0X's sample format.
 *
 * Sample format: mono int32, Q15 (32768 = 1.0), one voice peaking near VOICE_FS (24000), Felucca's
 * voice.c convention. T5i divides by 32768.0f. No heap, no float, no double, no 64-bit division, and
 * nothing requires core 1. */
#pragma once
#include <stdint.h>

#ifndef ENG_NOISE_VOICES
#define ENG_NOISE_VOICES 2           /* a lead / FX part; T5i tunes this for CPU */
#endif

/* the eight EDIT parameters {MODE, COLR, FREQ, RES, TRK, DENS, DRFT, CRSH} */
enum { E_NZ_MODE, E_NZ_COLR, E_NZ_FREQ, E_NZ_RES, E_NZ_TRK, E_NZ_DENS, E_NZ_DRFT, E_NZ_CRSH, E_NZ_NEDIT };

/* the DSP state and the control-rate modulation, exposed for the parity test (fm6_parity.c style) */
typedef struct { int32_t s[8]; uint32_t ph[3]; } eng_noise_state_t;
typedef struct { uint32_t inc; int32_t pitch16, amp0, amp1, cutoff, shape; } eng_noise_vmod_t;

/* Felucca's noise_note_on: the register / drift / filter state from the note's seed */
void eng_noise_seed(eng_noise_state_t *v, int note, int fine);
/* Felucca's noise_render, verbatim; p8 = the eight parameters, out is added to (Q15) */
void eng_noise_dsp(const int16_t *p8, eng_noise_state_t *v, int32_t *out, uint32_t n, const eng_noise_vmod_t *m);

void eng_noise_init(void);
void eng_noise_set_param(int i, int value);
int eng_noise_param(int i);
void eng_noise_param_range(int i, int *lo, int *hi);
void eng_noise_set_env(int atk_ms, int dec_ms, int sus_pct, int rel_ms);
void eng_noise_note_on(int note, int vel);
void eng_noise_note_off(int note);
void eng_noise_all_off(void);
void eng_noise_render(int32_t *out, uint32_t n);   /* adds n mono int32 samples, Q15 */
int eng_noise_voices_active(void);
