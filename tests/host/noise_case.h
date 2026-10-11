/* SPDX-License-Identifier: GPL-3.0-only
 * fm1-rocket-os T5c: one note case for the NOISE parity test, shared by the reference and the port. */
#pragma once
#include <stdint.h>

typedef struct {
    int16_t p[8];             /* MODE, COLR, FREQ, RES, TRK, DENS, DRFT, CRSH */
    int note, fine;
    uint32_t inc;
    int32_t pitch16, amp0, amp1, cutoff, shape;
} noise_case_t;
