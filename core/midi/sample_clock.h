/* SPDX-License-Identifier: GPL-3.0-only */
/* T11i: the audio sample clock a MIDI producer stamps packets with.
 *
 * One writer (the audio ISR) publishes a (sample, timer) anchor at each half buffer; readers are
 * the TIMER5 MIDI producer (which can preempt the writer) and the main loop (which the writer can
 * preempt). The publish is a seqlock: the sequence is odd while the two words are being written.
 * The read is bounded and wait-free — it retries the seqlock a few times and, if the writer could
 * not finish because the reader preempted it, returns the caller's previous good value. No path in
 * an ISR ever waits on another ISR.
 *
 * Pure C plus a compiler barrier, so tests/host/sample_clock_test.c can drive it: it interleaves a
 * reader at every store of the writer and checks the reader returns a consistent pair or the
 * previous snapshot, never spinning. */
#pragma once
#include <stdint.h>

typedef struct {
    volatile uint32_t sample;   /* the audio sample counter at the anchor */
    volatile uint32_t tick;     /* the free-running timer then */
    volatile uint8_t  seq;      /* odd while a publish is in progress */
} sample_clock_t;

/* One writer: the three stores plus the sequence counter. */
static inline void sample_clock_publish(sample_clock_t *c, uint32_t sample, uint32_t tick)
{
    c->seq++;                                  /* odd: a reader must not trust the pair */
    __asm__ volatile("" ::: "memory");
    c->sample = sample;
    c->tick = tick;
    __asm__ volatile("" ::: "memory");
    c->seq++;                                  /* even: the pair is consistent */
}

/* One consistent (sample, tick) pair; *ok is 1 when it is. May be preempted by the writer, so the
 * caller retries. */
static inline void sample_clock_read(const sample_clock_t *c, uint32_t *sample, uint32_t *tick, int *ok)
{
    uint8_t q1 = c->seq, q2;
    *sample = c->sample;
    *tick = c->tick;
    q2 = c->seq;
    *ok = (q1 == q2) && !(q1 & 1u);
}

/* The sample counter at now_ticks, extrapolated from the last anchor. Bounded retries; if the
 * writer is stuck mid-publish (this reader preempted it), the previous good value, *last, is
 * returned. Never waits on the writer. */
static inline uint32_t sample_clock_now(const sample_clock_t *c, uint32_t now_ticks, uint32_t *last)
{
    uint32_t s = 0, t = 0;
    int ok = 0, tries = 8;
    while (tries-- > 0 && !ok)
        sample_clock_read(c, &s, &t, &ok);
    if (!ok)
        return *last;                          /* the writer could not finish: the previous snapshot */
    /* 44100 samples per second over a 24 MHz timer: samples = ticks * 147 / 80000 */
    *last = s + (uint32_t)(((now_ticks - t) * 147u) / 80000u);
    return *last;
}
