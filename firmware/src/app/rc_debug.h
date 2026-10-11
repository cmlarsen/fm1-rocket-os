/* SPDX-License-Identifier: GPL-3.0-only */
/* RC1p: the debug-only device stress + stats surface (measuring CPU headroom without hands).
 *
 * `rc_debug.c` holds the implementation; it is #included by `editor.c` (a device build with
 * `X0X_DEBUG=1`) and by `host/x0x_host.c` (the scenario runner), so the ED_STRESS / ED_STATS
 * commands and the runner's `stress` / `stats` commands reach exactly the same code.
 *
 * This header is the only part shared with the Python tool (`tools/rc_probe.py`); keep the
 * command numbers and the stats field order in step with it. */
#pragma once
#include <stdint.h>

/* The development-only editor commands (editor.c's ED_HEADER + cmd; see rc_debug.c). */
enum { RC_ED_STRESS = 60, RC_ED_STATS = 61, RC_ED_STATS_RESET = 62 };

/* ED_STATS's reply: RC_ST_N 35-bit words, in this order. The PROF buckets follow RC_ST_PROF0
 * (engine.h's PR_909, PR_808, PR_303A, PR_303B, PR_FX, PR_MST, PR_SEQ, PR_FM6, PR_NOISE). */
enum {
    RC_ST_CPU = 0,       /* plat_cpu_pct: the mean block load, %         */
    RC_ST_PEAK,          /* the highest block load since the last reset, % */
    RC_ST_XRUNS,         /* dropped audio halves since boot              */
    RC_ST_GUARD,         /* the overload guard is engaged (1/0)          */
    RC_ST_GUARDCOUNT,    /* how many times it has engaged                */
    RC_ST_CPU2,          /* the second core is running (1/0)             */
    RC_ST_CPU2_GAVEUP,   /* it stopped itself (0 no)                     */
    RC_ST_FM6,           /* FM6 voices sounding now                      */
    RC_ST_PROF0,         /* .. + ENG_PROF_N: the PERF stage totals       */
    RC_ST_BSS = RC_ST_PROF0 + 9,   /* .data+.bss in use (device; 0 on host) */
    RC_ST_POOL,          /* the .pool section in use                     */
    RC_ST_POOL_TOTAL,    /* the .pool region's size                      */
    RC_ST_IRQ,           /* the interrupts' stack high-water, bytes      */
    RC_ST_N
};

/* Start stress level n (0 = stop; 1 = X0X sequencer; 2 = LIVE; 3 = LIVE + wired punch FX;
 * 4 = LIVE with the FM6 voice cap raised). `cap` selects the FM6 voice count for level 4
 * (0 = 6). Returns 0 on success, -1 for an unknown level. */
int rc_stress(int n, int cap);
int rc_stress_level(void);           /* the running level, 0 = none */
int rc_stress_active(void);          /* 1 while a level is running */

/* Fill `out` with RC_ST_N fields. Returns the count actually written (never more than
 * RC_ST_N or `max`). Does not clear the peak: a sampler takes the max across samples. */
int rc_stats(uint32_t *out, int max);
void rc_stats_reset(void);           /* clear the peak and the xrun count */
