/* SPDX-License-Identifier: GPL-3.0-only */
/* T11i: the audio sample clock a MIDI producer stamps packets with.
 *
 * One writer (the audio ISR) publishes a (sample, timer) anchor at each half buffer; readers are
 * the TIMER5 MIDI producer (which can preempt the writer) and the main loop (which the writer can
 * preempt). The publish saves the previous complete pair, then flips a sequence counter odd, writes
 * the two words and flips it even. The read is bounded and wait-free — it retries the seqlock a few
 * times and, if the writer could not finish because this reader preempted it, uses the writer's
 * previous complete pair. It holds no reader-local state, so every context shares one clock safely.
 * No path in an ISR ever waits on another ISR.
 *
 * Pure C plus compiler barriers. tests/host/sample_clock_test.c defines SAMPLE_CLOCK_STEP to run a
 * writer store between the reader's own loads, so it steps through every interleaving. */
#pragma once
#include <stdint.h>

typedef struct {
    volatile uint32_t sample, tick;             /* the pair (mid-publish while seq is odd) */
    volatile uint32_t prev_sample, prev_tick;   /* the last complete pair, for a preempted writer */
    volatile uint8_t  seq;                      /* odd while a publish is in progress */
} sample_clock_t;

#ifndef SAMPLE_CLOCK_STEP
#define SAMPLE_CLOCK_STEP() ((void)0)           /* the test hook; nothing on the device */
#endif

/* One writer: save the previous pair, then publish the new one under the sequence counter. */
static inline void sample_clock_publish(sample_clock_t *c, uint32_t sample, uint32_t tick)
{
    c->prev_sample = c->sample;
    c->prev_tick = c->tick;
    __asm__ volatile("" ::: "memory");
    c->seq++;                                  /* odd: a reader must not trust the pair */
    __asm__ volatile("" ::: "memory");
    c->sample = sample;
    c->tick = tick;
    __asm__ volatile("" ::: "memory");
    c->seq++;                                  /* even: consistent */
    SAMPLE_CLOCK_STEP();
}

/* One consistent (sample, tick) pair; *ok is 1 when it is. May be preempted by the writer, so the
 * caller retries; the sequence is compared exactly, so two publishes inside one read are seen. */
static inline void sample_clock_read(const sample_clock_t *c, uint32_t *sample, uint32_t *tick, int *ok)
{
    uint8_t q1 = c->seq, q2;
    SAMPLE_CLOCK_STEP();
    *sample = c->sample;
    SAMPLE_CLOCK_STEP();
    *tick = c->tick;
    SAMPLE_CLOCK_STEP();
    q2 = c->seq;
    *ok = (q1 == q2) && !(q1 & 1u);
}

/* The sample counter at now_ticks, extrapolated from the last anchor. Bounded retries; if the
 * writer is stuck mid-publish (this reader preempted it), its previous complete pair is used. No
 * reader-local state, so it is safe from any context. Never waits on the writer. */
static inline uint32_t sample_clock_now(const sample_clock_t *c, uint32_t now_ticks)
{
    uint32_t s = 0, t = 0;
    int ok = 0, tries = 8;
    while (tries-- > 0 && !ok) {
        sample_clock_read(c, &s, &t, &ok);
        SAMPLE_CLOCK_STEP();
    }
    if (!ok) {
        s = c->prev_sample;
        t = c->prev_tick;
    }
    /* 44100 samples per second over a 24 MHz timer: samples = ticks * 147 / 80000 */
    return s + (uint32_t)(((now_ticks - t) * 147u) / 80000u);
}
