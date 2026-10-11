/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * Ported from Felucca (hugelton/Felucca a4d7ca2)
 * fm1-rocket-os: the X0X adapter around Felucca's FM6 engine. */
/* eng_fm6: the thin X0X adapter around the ported FM6 core (fm6_core.c) and voice bank (fm6_vbank.c).
 *
 * Felucca's eng_fm6.c is welded to its track_t / voice_t / vmod_t and its SLOT / macro / matrix
 * machinery. This file keeps the parts X0X needs and drops the rest:
 *   init            the INIT patch and the macros at 0
 *   note on / off   velocity; FM6 is polyphonic with its own voice allocation (PRIO / released-first
 *                   stealing, Felucca's voice_alloc idea), the count a constant T5i tunes
 *   parameter set   the eight EDIT macros ALG FB MLVL MRAT MEG VMOD DTUN SLOT (fm6_sync, verbatim)
 *   render          a block of the voices into mono int32 in Felucca's voice convention
 *
 * Sample format: mono int32, Q15 (32768 = 1.0), one voice at full scale peaks near VOICE_FS (24000),
 * exactly Felucca's voice.c convention. T5i divides by 32768.0f to add it to X0X's float part bus (or
 * scales by X0X's out_scale / 32768 for the int32 output). No heap, no float, no double, no 64-bit
 * division. Nothing here touches core 1.
 *
 * The patch is the 155-byte single-voice layout (fm6_vbank.h): set_patch() puts it in as OWN, and a SLOT
 * turn loads a factory voice (FM6_FACTORY, tools/gen_fm6_patches.py) or a voice bank (fm6_vbank.h) as
 * Felucca's SLOT does. A bank slot without a voice is refused, exactly as Felucca's is. */
#pragma once
#include <stdint.h>
#include "fm6_core.h"
#include "fm6_vbank.h"

#ifndef ENG_FM6_VOICES
#define ENG_FM6_VOICES 6             /* polyphony: T5i tunes this for CPU (Felucca's FM6_POLY is 6) */
#endif

#define ENG_FM6_OWN 8                /* SLOT: the track's own patch (F1..F8 come before it) */
#define ENG_FM6_B1 (ENG_FM6_OWN + 1) /* SLOT: B1..B32 follow OWN */

/* the switch value (a preset load) -> the parameter, and the descriptor of each (k 0..7) */
enum { E_FM6_ALG, E_FM6_FB, E_FM6_MLVL, E_FM6_MRAT, E_FM6_MEG, E_FM6_VMOD, E_FM6_DTUN, E_FM6_SLOT, E_FM6_NEDIT };
/* the range of the seven macro parameters, {ALG, FB, MLVL, MRAT, MEG, VMOD, DTUN} */
void eng_fm6_param_range(int i, int *lo, int *hi);

void eng_fm6_init(void);
/* the track's own patch (155 bytes); sanitized. SLOT goes to OWN */
void eng_fm6_set_patch(const uint8_t *v155);
const uint8_t *eng_fm6_patch(void);
/* SLOT s: 0..ENG_FM6_OWN-1 a factory voice, ENG_FM6_OWN the own patch, then B1..B32 a bank voice.
 * Returns 0 when it loaded, 1 when refused (a bank slot with no voice, or s out of range) */
int eng_fm6_load_slot(int s);
int eng_fm6_slot(void);
/* the voice bank the B slots read (Felucca's flash bank, in RAM here; T5i fills it from storage) */
void eng_fm6_set_vbank(const fm6_vbank_t *b);

/* an EDIT macro; the SLOT parameter (E_FM6_SLOT) is a call to eng_fm6_load_slot() */
void eng_fm6_set_param(int i, int value);
int eng_fm6_param(int i);

void eng_fm6_note_on(int note, int vel);
void eng_fm6_note_off(int note);
void eng_fm6_all_off(void);
void eng_fm6_render(int32_t *out, uint32_t n);   /* adds n mono int32 samples, Q15 */
int eng_fm6_voices_active(void);
