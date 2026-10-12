/* SPDX-License-Identifier: GPL-3.0-only */
#define main simulator_main
#include "../../host/x0x_host.c"
#undef main
#include "core/model/motion_contract.h"
int main(void)
{
    unsigned t, v, i;
    for (t = 0; t < 16; t++)
        for (v = 0; v < 16; v++)
            for (i = 0; i < 256; i++) {
                const x0x_param_t *p = t < NTARGETS && v < (unsigned)engine_nvoices(t) ? engine_param(t, v, i) : 0;
                int expected = p ? p->max : -1;
                int got = motion_param_max(t << 12 | v << 8 | i);
                if (got != expected) {
                    fprintf(stderr, "motion contract %x:%u:%u max %d != %d\n", t, v, i, got, expected);
                    return 1;
                }
            }
    puts("motion contract: all IDs and pot maxima match engine metadata");
    return 0;
}
