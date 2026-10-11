/* SPDX-License-Identifier: GPL-3.0-only */
/* T11i: the sample clock's lock-free publish, interleaving a reader at every store of the writer.
 *
 * The audio-ISR writer publishes a (sample, timer) pair under a seqlock; the TIMER5 MIDI producer
 * (which can preempt the write) and the main loop read it. The reader must never wait on the writer:
 * a torn read returns the previous good value. This drives the writer one store at a time and calls
 * the reader after each, plus a stuck-writer case and a random-interleaving fuzz.
 *
 * Build: cc -std=c99 -O2 -I. tests/host/sample_clock_test.c
 */
#include <stdio.h>
#include <stdint.h>
#include "core/midi/sample_clock.h"

static int fails;

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
    sample_clock_t c = {0, 0, 0};
    uint32_t last = 0;

    printf("sample_clock: round trip\n");
    sample_clock_publish(&c, 1000, 24000);                 /* 1 ms on the 24 MHz timer */
    want_u32("now at the anchor", sample_clock_now(&c, 24000, &last), 1000);
    want_u32("now 1 ms later", sample_clock_now(&c, 48000, &last), 1000 + 44);

    printf("sample_clock: the reader at every store of the writer\n");
    {
        uint32_t prev = last;                              /* the last good value */
        c.seq++;                                           /* odd: a publish begins */
        want_u32("after seq++ (previous)", sample_clock_now(&c, 48000, &last), prev);
        c.sample = 2000;                                   /* half written */
        want_u32("after sample= (previous)", sample_clock_now(&c, 48000, &last), prev);
        c.tick = 72000;                                    /* still odd */
        want_u32("after tick= (previous)", sample_clock_now(&c, 48000, &last), prev);
        c.seq++;                                           /* even: done */
        want_u32("after the final seq++ (new)", sample_clock_now(&c, 72000, &last), 2000);
    }

    printf("sample_clock: a stuck writer never makes the reader wait\n");
    {
        uint32_t prev = last;
        int i;
        c.seq++;                                           /* the writer is preempted mid-publish */
        c.sample = 9999;
        c.tick = 1;
        for (i = 0; i < 1000000; i++)                      /* a spin would hang here */
            if (sample_clock_now(&c, 100000, &last) != prev) {
                printf("  FAIL stuck writer: a read changed\n");
                fails++;
                break;
            }
        if (i == 1000000)
            printf("  ok   stuck writer: 1e6 bounded reads, still %u\n", prev);
        c.seq++;                                           /* finish it */
    }

    printf("sample_clock: random interleaving fuzz\n");
    {
        uint32_t rng = 12345u, good_s = 0, good_t = 0, bad = 0;
        int i;
        for (i = 0; i < 200000; i++) {
            uint32_t s, t;
            int ok;
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            switch (rng % 4u) {
            case 0: c.seq++; break;                        /* start a write */
            case 1: c.sample = rng; break;                 /* a store */
            case 2: c.tick = rng; break;
            default:                                       /* complete a write */
                if (c.seq & 1u) {
                    c.seq++;
                    good_s = c.sample;
                    good_t = c.tick;
                }
                break;
            }
            sample_clock_read(&c, &s, &t, &ok);
            /* a read is only ever ok on a fully published pair */
            if (ok && ((c.seq & 1u) || s != c.sample || t != c.tick))
                bad++;
            sample_clock_now(&c, rng, &last);              /* must always return */
        }
        if (bad) {
            printf("  FAIL interleaving: %u bad reads\n", bad);
            fails++;
        } else {
            printf("  ok   interleaving: 200000 interleaved stores, no bad read\n");
        }
        (void)good_s; (void)good_t;
    }

    if (fails) {
        printf("sample_clock: %d case(s) failed\n", fails);
        return 1;
    }
    printf("sample_clock: all passed\n");
    return 0;
}
