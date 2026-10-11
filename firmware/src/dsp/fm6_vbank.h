/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * Ported from Felucca (hugelton/Felucca a4d7ca2)
 * fm1-rocket-os: the FM6 voice bank format and its 32 packed voices (fm6_vbank.c). */
/* fm6_vbank: the FM6 patch format and the 32-voice bank, ported from Felucca's eng_fm6.c (pack/unpack)
 * and fm6_vbank.c (the bank).
 *
 * The patch is the generic 155-byte single-voice layout (FP_* in fm6_core.h): six operators with their
 * 4-rate / 4-level envelopes, keyboard level and rate scaling, velocity, ratio or fixed frequency,
 * detune; 32 algorithms, feedback, LFO, pitch envelope, transpose. A 32-voice bank stores it as the
 * 128-byte packed record (7-bit bytes) that travels in SysEx, exactly as Felucca's B1..B32 slots do.
 *
 * Felucca keeps the bank in a flash sector (0xE7000) and copies a voice into the track's patch when SLOT
 * loads it. X0X has no such sector yet, so this is the format and the bank data only: fm6_vbank_read()
 * gives a voice, fm6_vbank_put() stores one in RAM. The flash transfer (Felucca's FM6B_BEGIN / WRITE /
 * END and its pre-erase CRC) is T5i's to wire to X0X's storage. */
#pragma once
#include <stdint.h>
#include "fm6_core.h"

#define FM6_PACKED 128u              /* bytes of a packed voice record */
#define FM6_NBANK 32u                /* voices in a bank (SLOT B1..B32) */

/* the highest value of each byte of the 155-byte voice */
uint32_t fm6_max(uint32_t i);
/* every value inside its range (a name byte outside 32..126 becomes a space); v[FP_SIZE] = 0 */
void fm6_sanitize(uint8_t *v);
/* 128-byte packed record -> 155-byte voice (bits a record does not use are ignored) */
void fm6_unpack(const uint8_t *b, uint8_t *v);
/* 155-byte voice -> 128-byte packed record (7-bit bytes: it travels in SysEx as it is) */
void fm6_pack(const uint8_t *v, uint8_t *b);

/* a 32-voice bank in RAM (Felucca's memory of the flash sector) */
typedef struct {
    uint32_t used;                   /* bit k: B k+1 holds a voice */
    uint8_t rec[FM6_NBANK][FM6_PACKED];
} fm6_vbank_t;

void fm6_vbank_init(fm6_vbank_t *b);                       /* empty */
int fm6_vbank_put(fm6_vbank_t *b, uint32_t k, const uint8_t *v155);   /* 0 ok, 1 bad index */
/* voice k -> v155 (sanitized); 0 ok, 1 = no voice there (as Felucca's bank slots skip an empty one) */
int fm6_vbank_read(const fm6_vbank_t *b, uint32_t k, uint8_t *v155);
