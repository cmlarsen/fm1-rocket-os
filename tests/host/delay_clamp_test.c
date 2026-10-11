/* SPDX-License-Identifier: GPL-3.0-only */
/* TCLS: the delay line is 0.75 s (DLY_LEN 33,075, was 44,100). Every tempo-synced division is
 * computed by fx_retime() and must be clamped so a read (or the ping's second tap) can never
 * reach past the buffer. This test pins the line length and the clamp edge: the longest synced
 * time that fits at 60 and at 80 bpm, and that no division overruns the line's usable length.
 *
 * The clamp is fxbus's own: max_ms = (dlen - 256) / D9_SR * 1000, then the division's time is
 * halved (still on the beat) until it fits. The 256-sample margin keeps a full-length read and
 * the TAPE wow/flutter excursion off the write head. */
#include <math.h>
#include <stdio.h>
#include "../../firmware/src/dsp/master.h"
#define X0X_DRUM_TABLES_DEFINE              /* no drum909.c in this link: the tables are defined here */
#include "../../firmware/src/dsp/fxbus.h"

#define DLY_LEN 33075                       /* firmware/src/app/engine.c */
#define NTDIV   13                          /* fx_beats' entries (FX_DL_TIME's range) */

static int16_t dl[DLY_LEN];
static int fails;
static void check(int ok, const char *what)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    fails += !ok;
}

int main(void)
{
    fxbus_t f;
    float line_ms = (float)DLY_LEN / D9_SR * 1000.0f;
    float max_ms = (float)(DLY_LEN - 256) / D9_SR * 1000.0f;
    int bpm_i;

    fxbus_init(&f, dl, DLY_LEN);
    check(fabsf(line_ms - 750.0f) < 0.01f, "the line is 0.75 s (DLY_LEN 33075 at 44.1 kHz)");

    for (bpm_i = 0; bpm_i < 2; bpm_i++) {
        float bpm = bpm_i ? 80.0f : 60.0f;
        float longest = 0.0f;
        int div, longest_div = -1, over = 0;
        fxbus_init(&f, dl, DLY_LEN);
        fxbus_set_bpm(&f, bpm);
        for (div = 0; div < NTDIV; div++) {
            fxbus_set(&f, FX_DL_TIME, div);
            if (!(f.dl_time_ms <= max_ms + 0.05f)) {
                printf("       div %d: %.1f ms > the line's %.1f ms\n", div, (double)f.dl_time_ms, (double)max_ms);
                over++;
            }
            if (f.dl_time_ms <= max_ms + 0.001f && f.dl_time_ms > longest) {
                longest = f.dl_time_ms;
                longest_div = div;
            }
        }
        printf("       %.0f bpm: longest synced time %.2f ms (division %d), max %.2f ms\n",
               (double)bpm, (double)longest, longest_div, (double)max_ms);
        check(!over, "every synced division is clamped inside the line");
        if (bpm_i == 0)
            check(fabsf(longest - 666.6667f) < 0.05f, "60 bpm: the longest synced time is 666.67 ms");
        else
            check(fabsf(longest - 562.5f) < 0.05f, "80 bpm: the longest synced time is 562.5 ms");
    }
    printf(fails ? "delay_clamp: %d FAILED\n" : "delay_clamp: ok\n", fails);
    return fails != 0;
}
