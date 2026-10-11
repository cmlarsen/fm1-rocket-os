/* SPDX-License-Identifier: GPL-3.0-only */
/* T11c host tests for the MIDI input core (core/midi/midi_in.c).
 *
 * Each case drives one midi_in_t with bytes, USB packets and timestamps and checks the
 * callbacks it produced. Assertions only: a failing check prints FAIL and the run exits
 * non-zero. Build:
 *   cc -std=c99 -I. tests/host/midi_in_test.c core/midi/midi_in.c
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "core/midi/midi_in.h"

#define FS 44100u

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

enum { E_ON, E_OFF, E_ALL, E_LAUNCH, E_START, E_CONT, E_STOP, E_SPP, E_LOST };
typedef struct { int kind; int a, b; } ev_t;
#define MAXEV 8192
static ev_t ev[MAXEV];
static int nev;
static midi_in_t m;
static uint32_t now;                       /* the running sample clock */

static void rec(int kind, int a, int b)
{
    if (nev < MAXEV) { ev[nev].kind = kind; ev[nev].a = a; ev[nev].b = b; }
    nev++;
}
static void on_(void *c, uint8_t note, uint8_t vel)   { (void)c; rec(E_ON, note, vel); }
static void off_(void *c, uint8_t note)               { (void)c; rec(E_OFF, note, 0); }
static void all_(void *c)                             { (void)c; rec(E_ALL, 0, 0); }
static void launch_(void *c, uint8_t clip, uint8_t q) { (void)c; rec(E_LAUNCH, clip, q); }
static void start_(void *c)                           { (void)c; rec(E_START, 0, 0); }
static void cont_(void *c)                            { (void)c; rec(E_CONT, 0, 0); }
static void stop_(void *c)                            { (void)c; rec(E_STOP, 0, 0); }
static void spp_(void *c, uint16_t p)                 { (void)c; rec(E_SPP, p, 0); }
static void lost_(void *c)                            { (void)c; rec(E_LOST, 0, 0); }

static const midi_sink_t SINK = { 0, on_, off_, all_, launch_, start_, cont_, stop_, spp_, lost_ };

static void reset(uint8_t live_ch, uint8_t pc_launch, uint8_t quant)
{
    midi_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.live_channel = live_ch;
    cfg.pc_launch = pc_launch;
    cfg.pc_quant = quant;
    cfg.cable = 0;
    memset(&m, 0, sizeof m);
    midi_in_init(&m, &cfg, &SINK);
    nev = 0;
    now = 0;
}

static void b(uint8_t byte) { midi_in_byte(&m, byte, now); }
static void pkt(uint8_t cable, uint8_t cin, uint8_t b1, uint8_t b2, uint8_t b3)
{
    uint32_t p = (uint32_t)((cable << 4) | cin)
               | ((uint32_t)b1 << 8) | ((uint32_t)b2 << 16) | ((uint32_t)b3 << 24);
    midi_in_packet(&m, p, now);
}
static int nk(int kind)
{
    int i, n = 0;
    for (i = 0; i < nev && i < MAXEV; i++)
        if (ev[i].kind == kind)
            n++;
    return n;
}
static int nth(int kind, int k, int *a, int *b)   /* k-th event of a kind; 1 = first */
{
    int i, n = 0;
    for (i = 0; i < nev && i < MAXEV; i++)
        if (ev[i].kind == kind && ++n == k) { if (a) *a = ev[i].a; if (b) *b = ev[i].b; return 1; }
    return 0;
}

/* ------------------------------------------------------------------- cases -- */

static void test_running_status(void)
{
    reset(0, 0, 0);
    b(0x90); b(60); b(100);
    b(62); b(101);                 /* running status: still 0x90 */
    b(0x80); b(60); b(0);          /* switch to note off */
    b(64); b(0);                   /* running status: still 0x80 */
    CHECK(nk(E_ON) == 2 && nk(E_OFF) == 2, "running status: on=%d off=%d", nk(E_ON), nk(E_OFF));
    {
        int a, d;
        CHECK(nth(E_ON, 1, &a, &d) && a == 60 && d == 100, "running: on1 %d %d", a, d);
        CHECK(nth(E_ON, 2, &a, &d) && a == 62 && d == 101, "running: on2 %d %d", a, d);
        CHECK(nth(E_OFF, 1, &a, &d) && a == 60, "running: off1 %d", a);
        CHECK(nth(E_OFF, 2, &a, &d) && a == 64, "running: off2 %d", a);
    }
}

static void test_realtime_inside(void)
{
    reset(0, 0, 0);
    b(0x90); b(0xF8); b(60); b(0xF8); b(100);   /* real time inside a note message */
    b(0xFA); b(0xFB); b(0xFC);                  /* transport, interleaved */
    b(0x80); b(0xF8); b(60); b(0);
    CHECK(nk(E_ON) == 1 && nk(E_OFF) == 1, "realtime inside: on=%d off=%d", nk(E_ON), nk(E_OFF));
    CHECK(nk(E_START) == 1 && nk(E_CONT) == 1 && nk(E_STOP) == 1,
          "realtime transport: %d %d %d", nk(E_START), nk(E_CONT), nk(E_STOP));
    {
        int a, d;
        CHECK(nth(E_ON, 1, &a, &d) && a == 60 && d == 100, "realtime: note %d %d", a, d);
    }
}

static void test_sysex(void)
{
    reset(0, 0, 0);
    b(0x90); b(60); b(100);        /* note before, running status 0x90 now active */
    b(0xF0); b(0x3E); b(0x7F); b(0x7F); b(0x01); b(0xF7);   /* must be swallowed */
    /* SysEx cancelled running status: these data bytes are ignored */
    b(70); b(100);
    CHECK(nk(E_ON) == 1, "sysex: running status must be cleared (%d)", nk(E_ON));
    b(0x90); b(64); b(110);        /* notes work again with a fresh status */
    CHECK(nk(E_ON) == 2, "sysex: on=%d (want 2)", nk(E_ON));
    {
        int a, d;
        CHECK(nth(E_ON, 1, &a, &d) && a == 60 && d == 100, "sysex: note1 %d %d", a, d);
        CHECK(nth(E_ON, 2, &a, &d) && a == 64 && d == 110, "sysex: note2 %d %d", a, d);
    }
    /* a real-time byte inside SysEx is still delivered */
    b(0xF0); b(0x7E); b(0xF8); b(0x00); b(0xF7);
    CHECK(m.have_tick == 1, "sysex: real-time inside is delivered");
}

static void test_vel0_off(void)
{
    reset(0, 0, 0);
    b(0x90); b(60); b(100);
    b(0x90); b(60); b(0);          /* velocity 0 = note off */
    CHECK(nk(E_ON) == 1 && nk(E_OFF) == 1, "vel0: on=%d off=%d", nk(E_ON), nk(E_OFF));
}

static void test_usb_packets(void)
{
    reset(0, 1, Q_BEAT);
    pkt(0, 0x9, 0x90, 60, 100);    /* CIN 9: note on */
    pkt(0, 0x8, 0x80, 60, 0);      /* CIN 8: note off */
    pkt(0, 0xC, 0xC0, 5, 0);       /* CIN 12: program change */
    pkt(1, 0x9, 0x90, 62, 90);     /* wrong cable: ignored */
    pkt(0, 0x0, 0x90, 64, 90);     /* reserved CIN: ignored */
    pkt(0, 0xF, 0xF8, 0, 0);       /* CIN 15: single real-time byte */
    CHECK(nk(E_ON) == 1, "usb: on=%d (want 1)", nk(E_ON));
    CHECK(nk(E_OFF) == 1, "usb: off=%d", nk(E_OFF));
    CHECK(nk(E_LAUNCH) == 1, "usb: launch=%d", nk(E_LAUNCH));
    CHECK(m.have_tick == 1, "usb: F8 in a packet must tick");
    {
        int a, d;
        CHECK(nth(E_ON, 1, &a, &d) && a == 60 && d == 100, "usb: note %d %d", a, d);
        CHECK(nth(E_LAUNCH, 1, &a, &d) && a == 5, "usb: pc %d", a);
    }
}

static void test_channel_filter(void)
{
    reset(5, 1, Q_BAR);            /* channel 5 (1-based) = status nibble 4 */
    b(0x94); b(60); b(100);        /* ch5: accepted */
    b(0x95); b(62); b(100);        /* ch6: ignored */
    b(0x85); b(62); b(0);          /* ch6 note off: ignored */
    b(0xB4); b(123); b(0);         /* ch5 CC123: all off */
    b(0xB5); b(123); b(0);         /* ch6 CC123: ignored */
    b(0xC4); b(3);                 /* ch5 PC: launch */
    b(0xC5); b(4);                 /* ch6 PC: ignored */
    CHECK(nk(E_ON) == 1, "filter: on=%d (want 1)", nk(E_ON));
    CHECK(nk(E_OFF) == 0, "filter: off=%d (want 0)", nk(E_OFF));
    CHECK(nk(E_ALL) == 1, "filter: all=%d (want 1)", nk(E_ALL));
    CHECK(nk(E_LAUNCH) == 1, "filter: launch=%d (want 1)", nk(E_LAUNCH));

    reset(0, 0, 0);                /* omni */
    b(0x91); b(60); b(100);
    b(0x9F); b(62); b(100);
    CHECK(nk(E_ON) == 2, "omni: on=%d (want 2)", nk(E_ON));
}

static void test_pc_launch(void)
{
    reset(0, 1, Q_END);
    b(0xC0); b(0);                 /* 0 in range */
    b(0xC0); b(26);                /* 26 in range */
    b(0xC0); b(27);                /* out of range */
    b(0xC0); b(127);               /* out of range */
    CHECK(nk(E_LAUNCH) == 2, "pc: launch=%d (want 2)", nk(E_LAUNCH));
    {
        int a, d;
        CHECK(nth(E_LAUNCH, 1, &a, &d) && a == 0 && d == Q_END, "pc: first %d q%d", a, d);
        CHECK(nth(E_LAUNCH, 2, &a, &d) && a == 26 && d == Q_END, "pc: second %d q%d", a, d);
    }
    reset(0, 0, Q_BAR);            /* pc_launch off */
    b(0xC0); b(3);
    CHECK(nk(E_LAUNCH) == 0, "pc disabled: launch=%d", nk(E_LAUNCH));
}

static void test_cc(void)
{
    reset(0, 0, 0);
    b(0xB0); b(123); b(0);
    b(0xB0); b(120); b(0);
    b(0xB0); b(7); b(100);         /* volume: not ours */
    CHECK(nk(E_ALL) == 2, "cc: all=%d (want 2)", nk(E_ALL));
}

static void test_spp(void)
{
    reset(0, 0, 0);
    b(0xF2); b(0x00); b(0x02);     /* 0x100 = 256 sixteenths */
    b(0xF2); b(0x7F); b(0x7F);     /* 0x3FFF = 16383 */
    CHECK(nk(E_SPP) == 2, "spp: %d", nk(E_SPP));
    {
        int a, d;
        CHECK(nth(E_SPP, 1, &a, &d) && a == 256, "spp: first %d", a);
        CHECK(nth(E_SPP, 2, &a, &d) && a == 16383, "spp: second %d", a);
    }
}

static void test_transport(void)
{
    reset(0, 0, 0);
    b(0xFA); b(0xFB); b(0xFC);
    CHECK(nk(E_START) == 1 && nk(E_CONT) == 1 && nk(E_STOP) == 1, "transport order");
    CHECK(ev[0].kind == E_START && ev[1].kind == E_CONT && ev[2].kind == E_STOP, "transport sequence");
}

static void test_stray_f7(void)
{
    reset(0, 0, 0);
    b(0x90); b(60); b(100);        /* note on, running status 0x90 */
    b(0xF7);                       /* a stray F7 cancels running status */
    b(62); b(100);                 /* so these data bytes are ignored */
    CHECK(nk(E_ON) == 1, "stray F7 clears running status (on=%d)", nk(E_ON));
    b(0x90); b(64); b(110);        /* a fresh status works */
    CHECK(nk(E_ON) == 2, "stray F7: fresh status works (on=%d)", nk(E_ON));
}

static void test_pc_running_status(void)
{
    /* Program Change via running status, across SysEx and a status-byte SysEx end. Expect 5. */
    static const uint8_t bb[] = { 5, 6, 0xC0, 3, 0xF8, 4, 0xF0, 1, 2, 0xC0, 7,
                                  0xF0, 9, 0xC1, 1, 26, 27 };
    unsigned i;
    reset(0, 1, Q_BEAT);
    for (i = 0; i < sizeof bb; i++) { b(bb[i]); }
    CHECK(nk(E_LAUNCH) == 5, "pc running status: launches=%d (want 5)", nk(E_LAUNCH));
    {
        int a, d, k;
        static const int want[5] = { 3, 4, 7, 1, 26 };
        for (k = 1; k <= 5; k++)
            CHECK(nth(E_LAUNCH, k, &a, &d) && a == want[k - 1] && d == Q_BEAT,
                  "pc running status: launch %d = %d (want %d)", k, a, want[k - 1]);
    }
}

/* ---------------- clock ---------------- */

/* one interval in Q16 samples, exact enough for 60/120/200 bpm at 44100 */
static uint32_t period_q16(int bpm) { return (uint32_t)(((uint64_t)60 * FS << 16) / ((uint64_t)bpm * 24)); }
static uint32_t rng_s;
static uint32_t rng(void) { rng_s = rng_s * 1664525u + 1013904223u; return rng_s >> 8; }

static void test_clock_steady(void)
{
    static const int bpms[3] = { 60, 120, 200 };
    static const char *names[3] = { "60", "120", "200" };
    int k;
    for (k = 0; k < 3; k++) {
        int bpm = bpms[k];
        uint32_t p = period_q16(bpm);
        uint64_t t = 0;
        int i, warm = 72, total = 360;
        int32_t worst = 0;
        reset(0, 0, 0);
        rng_s = 12345u + (uint32_t)bpm;
        for (i = 0; i < total; i++) {
            int32_t jit = (int32_t)(rng() % (2u * (FS / 1000u) + 1u)) - (int32_t)(FS / 1000u);
            int32_t samp;
            t += p;
            samp = (int32_t)(t >> 16) + jit;
            if (samp < 0) samp = 0;
            now = (uint32_t)samp;
            midi_in_byte(&m, 0xF8, now);
            if (i >= warm) {
                int32_t e = (int32_t)midi_in_bpm_x10(&m) - bpm * 10;
                if (e < 0) e = -e;
                if (e > worst) worst = e;
            }
        }
        CHECK(worst <= 5, "clock %s bpm: worst error %d x0.1 bpm (want <= 5)", names[k], (int)worst);
    }
}

static void test_clock_step(void)
{
    static const int from[2] = { 120, 200 };
    static const int to[2]   = { 160, 90 };
    int k;
    for (k = 0; k < 2; k++) {
        uint32_t p1 = period_q16(from[k]), p2 = period_q16(to[k]);
        uint64_t t = 0;
        int i, lead = 96, tail = 96, first = -1;
        reset(0, 0, 0);
        for (i = 0; i < lead + tail; i++) {
            t += (i < lead) ? p1 : p2;
            now = (uint32_t)(t >> 16);
            midi_in_byte(&m, 0xF8, now);
            if (i >= lead && first < 0) {
                int32_t e = (int32_t)midi_in_bpm_x10(&m) - to[k] * 10;
                if (e < 0) e = -e;
                if (e <= 5) first = i - lead;      /* ticks after the change */
            }
        }
        CHECK(first >= 0 && first <= 24, "clock step %d->%d: settled after %d ticks (want <= 24)",
              from[k], to[k], first);
        {
            int32_t e = (int32_t)midi_in_bpm_x10(&m) - to[k] * 10;
            if (e < 0) e = -e;
            CHECK(e <= 5, "clock step %d->%d: final error %d", from[k], to[k], (int)e);
        }
    }
}

static void test_clock_ramp(void)
{
    /* 100 -> 200 bpm over 8 beats, then hold; the estimate must track up and settle */
    uint64_t t = 0;
    int i, n = 24 * 8, hold = 24 * 8;
    int32_t at_mid = 0;
    reset(0, 0, 0);
    for (i = 0; i < n + hold; i++) {
        int bpm = i < n ? 100 + 100 * i / n : 200;
        t += period_q16(bpm);
        now = (uint32_t)(t >> 16);
        midi_in_byte(&m, 0xF8, now);
        if (i == n / 2) at_mid = (int32_t)midi_in_bpm_x10(&m);
    }
    CHECK(at_mid > 1100 && at_mid < 1900, "clock ramp: mid estimate %d (want 1100..1900)", (int)at_mid);
    {
        int32_t e = (int32_t)midi_in_bpm_x10(&m) - 2000;
        if (e < 0) e = -e;
        CHECK(e <= 5, "clock ramp: final error %d (want <= 5)", (int)e);
    }
}

static void test_clock_lost(void)
{
    uint32_t p = period_q16(120);
    uint64_t t = 0;
    int i;
    reset(0, 0, 0);
    for (i = 0; i < 96; i++) { t += p; now = (uint32_t)(t >> 16); midi_in_byte(&m, 0xF8, now); }
    CHECK(midi_in_clock_ok(&m) == 1, "clock lost: following before");
    /* exactly one beat: not yet lost */
    midi_in_tick(&m, now + (uint32_t)(m.est_fp >> 10));
    CHECK(nk(E_LOST) == 0, "clock lost: fires at exactly one beat");
    /* past one beat: lost once */
    midi_in_tick(&m, now + (uint32_t)(m.est_fp >> 10) + 1u);
    CHECK(nk(E_LOST) == 1, "clock lost: fires after one beat");
    CHECK(midi_in_clock_ok(&m) == 0, "clock lost: flag cleared");
    midi_in_tick(&m, now + 10u * (uint32_t)(m.est_fp >> 10));
    CHECK(nk(E_LOST) == 1, "clock lost: fires only once");
    /* ticks resume, then it can fire again */
    for (i = 0; i < 96; i++) { t += p; now = (uint32_t)(t >> 16); midi_in_byte(&m, 0xF8, now); }
    CHECK(midi_in_clock_ok(&m) == 1, "clock lost: follows again");
    midi_in_tick(&m, now + (uint32_t)(m.est_fp >> 10) + 1u);
    CHECK(nk(E_LOST) == 2, "clock lost: fires again after losing it twice");

    /* above 240 bpm one beat is shorter than fs/4: the check must still be one beat */
    reset(0, 0, 0);
    p = period_q16(250);
    t = 0;
    for (i = 0; i < 96; i++) { t += p; now = (uint32_t)(t >> 16); midi_in_byte(&m, 0xF8, now); }
    midi_in_tick(&m, now + (uint32_t)(m.est_fp >> 10) + 1u);
    CHECK(nk(E_LOST) == 1, "clock lost above 240 bpm fires after one beat");
}

/* ticks at `bpm`, dropping drop_n F8s at tick drop_at; returns the worst |error| x0.1 bpm
 * over the 30 ticks after the drop */
static int32_t drop_run(int bpm, int drop_at, int drop_n)
{
    uint32_t p = period_q16(bpm);
    uint64_t t = 0;
    int32_t worst = 0;
    int i;
    reset(0, 0, 0);
    for (i = 0; i < 24 * 12; i++) {
        t += p;                                    /* the clock's time always advances */
        now = (uint32_t)(t >> 16);
        if (i >= drop_at && i < drop_at + drop_n)
            continue;                              /* the F8 never arrives */
        midi_in_byte(&m, 0xF8, now);
        midi_in_tick(&m, now);
        if (i > drop_at + drop_n && i < drop_at + drop_n + 30) {
            int32_t e = (int32_t)midi_in_bpm_x10(&m) - bpm * 10;
            if (e < 0) e = -e;
            if (e > worst) worst = e;
        }
    }
    return worst;
}

static void test_clock_drops(void)
{
    struct { int bpm, at, n; } c[3] = { { 120, 100, 2 }, { 120, 100, 4 }, { 250, 120, 5 } };
    int k;
    for (k = 0; k < 3; k++) {
        int32_t e = drop_run(c[k].bpm, c[k].at, c[k].n);
        CHECK(e <= 10, "clock drops %d bpm x%d: worst %d x0.1 bpm (want <= 10)",
              c[k].bpm, c[k].n, (int)e);
    }
}

static void test_clock_lost_no_tempo(void)
{
    reset(0, 0, 0);
    midi_in_byte(&m, 0xF8, 1000u);       /* a single tick: no interval, so no tempo yet */
    CHECK(midi_in_bpm_x10(&m) == 0, "no tempo: bpm_x10 = %u (want 0)", midi_in_bpm_x10(&m));
    CHECK(midi_in_clock_ok(&m) == 1, "no tempo: still following");
    midi_in_tick(&m, 1000u + FS);        /* exactly one second: not yet */
    CHECK(nk(E_LOST) == 0, "no tempo: must not fire at exactly one second");
    midi_in_tick(&m, 1000u + FS + 1u);   /* past it: fires */
    CHECK(nk(E_LOST) == 1, "no tempo: fires past one second");
}

static void test_tick_wrap(void)
{
    uint32_t t = 0xFFFFFFFFu - 918u * 24u * 3u;
    int i;
    reset(0, 0, 0);
    for (i = 0; i < 24 * 6; i++) {
        midi_in_byte(&m, 0xF8, t);
        t += 918u;
        midi_in_tick(&m, t - 10u);
    }
    {
        int32_t e = (int32_t)midi_in_bpm_x10(&m) - 1200;
        if (e < 0) e = -e;
        CHECK(e <= 5, "tick wrap: bpm_x10 error %d (want <= 5)", (int)e);
    }
    CHECK(nk(E_LOST) == 0, "tick wrap: no false clock_lost");
}

static void test_fuzz(void)
{
    int i;
    reset(0, 1, Q_BEAT);
    rng_s = 0xC0FFEEu;
    for (i = 0; i < 1000000; i++) {
        uint32_t r = rng();
        now += 1u + (r & 0x3Fu);
        if ((r & 3u) == 0u)                    /* a quarter of them as USB packets */
            pkt((uint8_t)((r >> 8) & 1u), (uint8_t)(r & 0xFu),
                (uint8_t)(r >> 12), (uint8_t)(r >> 20), (uint8_t)(r >> 26));
        else
            midi_in_byte(&m, (uint8_t)r, now);
    }
    /* the parser must recover on the next status byte */
    nev = 0;
    b(0x90); b(60); b(100);
    CHECK(nk(E_ON) == 1, "fuzz: recovered on next status (on=%d)", nk(E_ON));
    {
        int a, d;
        CHECK(nth(E_ON, 1, &a, &d) && a == 60 && d == 100, "fuzz: note %d %d", a, d);
    }
    /* and running status works again */
    b(62); b(90);
    CHECK(nk(E_ON) == 2, "fuzz: running status after recovery (on=%d)", nk(E_ON));
}

int main(void)
{
    test_running_status();
    test_realtime_inside();
    test_sysex();
    test_vel0_off();
    test_usb_packets();
    test_channel_filter();
    test_pc_launch();
    test_cc();
    test_spp();
    test_transport();
    test_stray_f7();
    test_pc_running_status();
    test_clock_steady();
    test_clock_step();
    test_clock_ramp();
    test_clock_drops();
    test_clock_lost();
    test_clock_lost_no_tempo();
    test_tick_wrap();
    test_fuzz();
    printf(fails ? "midi_in: %d FAILED\n" : "midi_in: ok (20 cases)\n", fails);
    return fails != 0;
}
