/* SPDX-License-Identifier: GPL-3.0-only */
/* T11i: the sample clock's lock-free publish, with the writer stepping between the reader's own
 * loads (SAMPLE_CLOCK_STEP), so every interleaving is exercised.
 *
 * The audio-ISR writer publishes a (sample, timer) pair under a sequence counter; the TIMER5 MIDI
 * producer (which can preempt the write) and the main loop read it. The reader must never wait on
 * the writer: a read either sees a complete publish or falls back to the previous one. This drives
 * the writer one store at a time and runs the reader (which steps the writer again at each of its
 * own loads), checks that a successful read is always the last complete publish, and that a
 * preempted writer never makes the reader spin.
 *
 * Build: cc -std=c99 -O2 -I. tests/host/sample_clock_test.c
 */
#include <stdio.h>
#include <stdint.h>

/* The writer, one store per call, interleaved with the reader's loads. The hook macro must exist
 * before the header so its inline reader calls test_writer_step at each of its own loads. */
static void test_writer_step(void);
static void reader_hook(void);
#define SAMPLE_CLOCK_STEP() reader_hook()
#include "core/midi/sample_clock.h"

static sample_clock_t c;
static uint32_t ps, pt;              /* the pair being published */
static uint32_t done_s, done_t;      /* the last complete published pair */
static uint32_t hist_s[4], hist_t[4];/* the last few published pairs (a read must return one) */
static int hist_n;
static int wphase;                   /* the next store; >= PUB_STORES when idle */
static int wrunning;
static int hook_on = 1;              /* 0 freezes the writer for the reader's own hooks */
#define PUB_STORES 6

static int in_hist(uint32_t s, uint32_t t)
{
    int k, n = hist_n < 4 ? hist_n : 4;
    for (k = 0; k < n; k++)
        if (hist_s[k] == s && hist_t[k] == t)
            return 1;
    return 0;
}

/* The reader's hook: run a writer store between the reader's loads, unless frozen. */
static void reader_hook(void)
{
    if (hook_on)
        test_writer_step();
}

static void test_writer_step(void)
{
    if (!wrunning)
        return;
    switch (wphase++) {
    case 0: c.prev_sample = c.sample; break;
    case 1: c.prev_tick = c.tick; break;
    case 2: c.seq++; break;                     /* odd */
    case 3: c.sample = ps; break;
    case 4: c.tick = pt; break;
    case 5:
        c.seq++;                                /* even */
        done_s = ps;
        done_t = pt;
        hist_s[hist_n & 3] = ps;
        hist_t[hist_n & 3] = pt;
        hist_n++;
        wrunning = 0;
        break;
    default: wrunning = 0; break;
    }
}

static int fails;

static void start_publish(uint32_t s, uint32_t t) { ps = s; pt = t; wphase = 0; wrunning = 1; }
static void finish_now(void) { while (wrunning) test_writer_step(); }

static void want_u32(const char *what, uint32_t got, uint32_t want)
{
    if (got != want) {
        printf("  FAIL %s: got %u, want %u\n", what, got, want);
        fails++;
    } else {
        printf("  ok   %s == %u\n", what, got);
    }
}

int main(void)
{
    int i, start;

    printf("sample_clock: round trip\n");
    sample_clock_publish(&c, 1000, 24000);       /* 1 ms on the 24 MHz timer */
    hist_s[hist_n & 3] = 1000; hist_t[hist_n & 3] = 24000; hist_n++;
    want_u32("now at the anchor", sample_clock_now(&c, 24000), 1000);
    want_u32("now 1 ms later", sample_clock_now(&c, 48000), 1000 + 44);

    printf("sample_clock: the reader runs the writer between its own loads\n");
    for (start = 0; start <= PUB_STORES; start++) {
        uint32_t s, t;
        int ok;
        start_publish(2000 + (uint32_t)start, 72000 + (uint32_t)start);
        for (i = 0; i < start; i++)
            test_writer_step();
        sample_clock_read(&c, &s, &t, &ok);      /* its own hooks keep stepping the writer */
        if (ok && !in_hist(s, t)) {              /* a successful read must be a published pair */
            printf("  FAIL start=%d: read %u,%u was never published\n", start, s, t);
            fails++;
        }
        sample_clock_now(&c, 100000);            /* must always return */
        finish_now();
    }
    printf("  ok   every interleaving of a publish with a read\n");

    printf("sample_clock: a stuck writer never makes the reader wait\n");
    {
        uint32_t prev;
        start_publish(3000, 96000);
        finish_now();
        prev = sample_clock_now(&c, 96000);
        start_publish(9999, 1);
        for (i = 0; i < 5; i++)                  /* every store but the final seq++: the pair is
                                                  * written but not published */
            test_writer_step();
        hook_on = 0;                             /* the writer cannot run: the hook must not advance it */
        for (i = 0; i < 1000000; i++)            /* a spin would hang here; bounded reads only */
            (void)sample_clock_now(&c, 96000);
        want_u32("stuck writer: the previous complete value", sample_clock_now(&c, 96000), prev);
        hook_on = 1;
        finish_now();
    }

    printf("sample_clock: interleaving fuzz (a successful read is a complete publish)\n");
    {
        uint32_t rng = 12345u;
        int bad = 0;
        hook_on = 0;                             /* the writer runs only where the fuzz says, so a
                                                  * read can land mid-publish and a broken reader
                                                  * would hand back a half-written pair */
        for (i = 0; i < 200000; i++) {
            uint32_t s, t;
            int ok;
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            if (wrunning)
                test_writer_step();              /* one writer store */
            else
                start_publish(rng, rng ^ 0x5A5Au);
            sample_clock_read(&c, &s, &t, &ok);
            if (ok && !in_hist(s, t))            /* a broken reader would pass a half-written pair */
                bad++;
            sample_clock_now(&c, rng);           /* must always return */
        }
        hook_on = 1;
        if (bad) {
            printf("  FAIL fuzz: %d read(s) returned a pair that was never published\n", bad);
            fails++;
        } else {
            printf("  ok   fuzz: 200000 reads at every writer phase, every ok read was published\n");
        }
    }

    if (fails) {
        printf("sample_clock: %d case(s) failed\n", fails);
        return 1;
    }
    printf("sample_clock: all passed\n");
    return 0;
}
