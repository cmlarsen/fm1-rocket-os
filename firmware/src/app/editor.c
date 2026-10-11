/* SPDX-License-Identifier: GPL-3.0-only
 * Editor-frame plumbing adapted from Felucca's editor.c, Copyright (C) 2026 Leo Kuroshita
 * (@kurogedelic), Hugelton Instruments. */
/* X0X's subset of Felucca's editor protocol (F0 7D 46 4C cmd args.. F7): INFO (1) and PING (25),
 * plus the debug commands in an X0X_DEBUG build. The parameter / preset / track commands are
 * Felucca's instrument and are not answered. T6i removed the sample-slot commands (SMP_* 11..15):
 * they erased and programmed 0xA0000.., the region T6 now owns as the song store, and nothing plays
 * user samples (the part that looped a slot was removed in T1). */
#define ED_HDR0 0x7D
#define ED_HDR1 0x46
#define ED_HDR2 0x4C
enum { ED_INFO = 1, ED_PING = 25 };

static uint8_t ed_out[288];
static uint32_t ed_n;

static void ed_begin(uint32_t cmd)
{
    ed_out[0] = 0xF0;
    ed_out[1] = ED_HDR0;
    ed_out[2] = ED_HDR1;
    ed_out[3] = ED_HDR2;
    ed_out[4] = (uint8_t)cmd;
    ed_n = 5;
}
static void ed_b(uint32_t v)
{
    if (ed_n < sizeof ed_out - 1u)
        ed_out[ed_n++] = (uint8_t)(v & 0x7Fu);
}
static void ed_str(const char *s, uint32_t max)
{
    uint32_t i;
    for (i = 0; s && s[i] && i < max; i++)
        ed_b((uint8_t)s[i] & 0x7Fu);
    ed_b(0);
}
static void ed_send(void)
{
    ed_out[ed_n++] = 0xF7;
    ota_wire_send(ed_out, ed_n);
}

#ifndef X0X_DEBUG
#define X0X_DEBUG 0
#endif
#if X0X_DEBUG
/* development builds only (X0X_DEBUG=1; tools/build.py refuses it in a release): the clock work
 * reads and writes the chip's registers live over USB.
 *   PEEK  40 addr n        -> addr, n words      (n <= 16)
 *   POKE  41 addr value    -> addr, old, new
 *   CLOCK 42 iterations    -> 24 MHz ticks a fixed loop took (IRQs off), C0_TL_CKCNT before, after
 * A number is 5 x 7 bits, least significant first. Only these ranges, word aligned: an address
 * outside them gets no reply (a stray read of a gated block could fault). */
enum { ED_PEEK = 40, ED_POKE, ED_CLOCK };
#include "fm1_debug.h"
/* RC1p: the debug-only stress / stats commands (rc_debug.c). Same code the host runner's
 * `stress` / `stats` commands use. */
#include "rc_debug.h"
#include "rc_debug.c"
static uint32_t ed_u35(const uint8_t *a)
{
    return (uint32_t)a[0] | (uint32_t)a[1] << 7 | (uint32_t)a[2] << 14 | (uint32_t)a[3] << 21 | (uint32_t)a[4] << 28;
}
static void ed_w35(uint32_t v)
{
    uint32_t k;
    for (k = 0; k < 5u; k++)
        ed_b(v >> (7u * k));
}
static int ed_debug(uint32_t cmd, const uint8_t *a, uint32_t na)
{
    uint32_t addr, i;
#if X0X_DEBUG > 1                                     /* the clock work's timing loop: level 2 (room) */
    if (cmd == ED_CLOCK) {
        uint32_t it, c0, c1;
        if (na < 5u)
            return 0;
        it = ed_u35(a);
        if (!it || it > 50000000u)
            return 0;
        ed_w35(fm1_dbg_clock(it, &c0, &c1));
        ed_w35(c0);
        ed_w35(c1);
        return 1;
    }
#endif
    if (na < 10u)
        return 0;
    addr = ed_u35(a);
    if (cmd == ED_PEEK) {
        uint32_t n = ed_u35(a + 5);
        if (!n || n > 16u || !fm1_dbg_ok(addr, n))
            return 0;
        ed_w35(addr);
        for (i = 0; i < n; i++)
            ed_w35(fm1_dbg_rd(addr + 4u * i));
        return 1;
    }
    if (!fm1_dbg_ok(addr, 1))
        return 0;
    ed_w35(addr);
    ed_w35(fm1_dbg_rd(addr));
    fm1_dbg_wr(addr, ed_u35(a + 5));
    ed_w35(fm1_dbg_rd(addr));
    return 1;
}
#endif

static void ed_handle(const uint8_t *f, uint32_t n)
{
    uint32_t cmd = f[3];
    const uint8_t *a = f + 4;
    uint32_t na = n - 4u;
    (void)a;
    (void)na;                                         /* only the debug commands below use them */
    ed_begin(cmd);
    switch (cmd) {
    case ED_INFO:
        ed_str("X0X " X0X_VERSION, 24);
        ed_b(0);                                      /* no engines / parameters over this protocol */
        ed_b(0);
        ed_b(0);
        ed_b(NSTEPS);
        ed_b(0);
        ed_b(0);
        break;
    case ED_PING:
        ed_b(0);
        break;
#if X0X_DEBUG
    case ED_PEEK:
    case ED_POKE:
    case ED_CLOCK:
        if (!ed_debug(cmd, a, na))
            return;
        break;
    case RC_ED_STRESS: {                              /* RC1p: n [cap] -> n, ok */
        uint32_t n, cap;
        int rc;
        if (na < 10u)
            return;
        n = ed_u35(a);
        cap = ed_u35(a + 5);
        rc = rc_stress((int)n, (int)cap);
        ed_w35(n);
        ed_w35(rc == 0 ? 1u : 0u);
        break;
    }
    case RC_ED_STATS: {                               /* RC1p: count, then every field */
        uint32_t s[RC_ST_N];
        int k, cnt = rc_stats(s, RC_ST_N);
        ed_w35((uint32_t)cnt);
        for (k = 0; k < cnt; k++)
            ed_w35(s[k]);
        break;
    }
    case RC_ED_STATS_RESET:
        rc_stats_reset();
        ed_b(1);
        break;
#endif
    default:
        return;                                       /* not ours: no reply */
    }
    ed_send();
}

static void ed_service(void)
{
    const uint8_t *p;
    uint32_t n;
    static uint8_t f[64];
    uint32_t k, i;
    if (!ota_frame_get(&p, &n) || n < 4u || p[0] != ED_HDR0 || p[1] != ED_HDR1 || p[2] != ED_HDR2)
        return;                                       /* not an editor frame: ota_service() reads it */
    k = n > sizeof f ? sizeof f : n;
    for (i = 0; i < k; i++)
        f[i] = p[i];
    ota_frame_done();
    ed_handle(f, k);
}
