/* SPDX-License-Identifier: GPL-3.0-only */
/* Stored motion IDs: target:4 | voice:4 | index:8. Pot maxima match the
 * engine descriptors; tests/host/motion_contract_test.c checks every descriptor. */
#pragma once
#include <stdint.h>

static inline int motion_param_max(unsigned param)
{
    static const uint8_t d9_n[] = {11,10,9,9,9,7,8,8,8,8,8,2};
    static const uint8_t d9_dist[] = {7,6,5,5,5,3,4,4,4,4,4,255};
    static const uint8_t d8_n[] = {10,10,9,9,9,9,10,8,8,8,8,2};
    static const uint8_t d8_dist[] = {6,6,5,5,5,5,6,4,4,4,4,255};
    static const uint8_t bass[] = {127,127,127,127,127,1,127,127,127,2,127,127};
    static const uint8_t fm6[] = {32,14,127,32,127,14,127,40};
    static const uint8_t noise[] = {3,127,127,127,127,127,127,127};
    static const uint8_t fx[] = {127,7,127,127,127,127,127,127,12,127,127,127,127,1,127,1};
    static const uint8_t master[] = {127,9,127,127,127,127,127,2,3,127,127,1,127,127};
    unsigned t = param >> 12, v = (param >> 8) & 15u, i = param & 255u;
    switch (t) {
    case 0: return v < 12 && i < d9_n[v] ? (i == d9_dist[v] ? 6 : 127) : -1;
    case 1:
        if (v >= 12 || i >= d8_n[v]) return -1;
        if ((v >= 2 && v <= 5 && i == 3) || (v == 6 && i == 4)) return 1;
        return i == d8_dist[v] ? 6 : 127;
    case 2: return v < 2 && i < sizeof bass ? bass[i] : -1;
    case 3:
        if (i >= sizeof fm6) return -1;
        return v == 0 ? fm6[i] : v == 1 ? noise[i] : -1;
    case 4: return v == 0 && i < sizeof fx ? fx[i] : -1;
    case 5: return v < 6 && i < 4 ? 127 : -1;
    case 6: return v == 0 && i < sizeof master ? master[i] : -1;
    default: return -1;
    }
}
