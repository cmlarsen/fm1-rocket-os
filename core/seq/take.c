/* SPDX-License-Identifier: GPL-3.0-only */
/* T9c take recorder and player; see take.h for the model.
 *
 * The take is a flat, append-only log. Recording:
 *
 *   take_arm()      resets the log, writes TAKE_MARK start at offset 0, runs the caller's
 *                   prelude (setup events, also at offset 0) and records the absolute sample
 *                   the first live event will land on (`start`, the next bar boundary).
 *   take_feed()     in the ISR, once per drained engine command: advances the arm/stop state
 *                   machine and appends `{sample - start, cmd}`. Constant time, no heap.
 *   take_stop()     records the boundary to end on; take_tick()/take_feed() close the take
 *                   there with TAKE_MARK end and set hdr.length_samples.
 *
 * The offsets are unsigned 32-bit `sample - start`, so a take recorded across the audio
 * counter's wrap is contiguous. The final slot is reserved for TAKE_MARK end: when the buffer
 * fills, recording stops with that mark and `full` is set for the UI.
 *
 * Playback mirrors clip_engine_render(): take_render() advances the replay clock by nframes and
 * hands the sink each event with its exact offset into that window. Because the log is sorted
 * by offset, any window size produces the same absolute firing times.
 */
#include "take.h"

/* `full` is a byte in the struct; keep the model header honest about the array it sizes. */
_Static_assert(sizeof(take_event_t) == 8, "take_event_t");
_Static_assert(TAKE_MAX_EVENTS <= 0xFFFFu, "take_hdr_t.nevents is uint16_t");

/* publish we have written the data / read the flag before the data */
#define TAKE_BARRIER() __asm__ volatile("" ::: "memory")

/* ---- helpers ------------------------------------------------------------- */

static uint32_t bar_of(const take_t *t, uint32_t sample)
{
    return t->bar ? t->bar(t->bar_ctx, sample) : sample;
}

/* Close the take at absolute offset `off`. Writes TAKE_MARK end if a slot is free. The ISR's
 * take_tick() is the normal caller; the `ended` guard makes it idempotent (the full-buffer path
 * may also reach it from the arm's build). */
static void take_end(take_t *t, uint32_t off, int full)
{
    if (t->ended)
        return;
    t->ended = 1;
    if (t->n < TAKE_MAX_EVENTS) {
        t->ev[t->n].sample = off;
        t->ev[t->n].cmd = TAKE_MARK_END;
        t->n++;
    }
    t->hdr.nevents = t->n;
    t->hdr.length_samples = off;
    t->full = (uint8_t)(full ? 1u : 0u);
    t->state = TAKE_IDLE;
}

/* Append one event. The last slot is reserved for the end mark, so a full buffer closes
 * itself cleanly instead of overflowing. Returns 0 once the take has closed. */
static int take_put(take_t *t, uint32_t off, uint32_t cmd)
{
    if (t->n + 1u >= TAKE_MAX_EVENTS) {
        take_end(t, off, 1);
        return 0;
    }
    t->ev[t->n].sample = off;
    t->ev[t->n].cmd = cmd;
    t->n++;
    t->hdr.nevents = t->n;
    return 1;
}

static void take_clear(take_t *t)
{
    t->n = 0;
    t->state = TAKE_IDLE;
    t->full = 0;
    t->stop_set = 0;
    t->arming = 0;
    t->ended = 0;
    t->hdr.nevents = 0;
    t->hdr.length_samples = 0;
}

/* ---- setup --------------------------------------------------------------- */

void take_init(take_t *t)
{
    if (!t)
        return;
    take_clear(t);
    t->hdr.bpm_x10 = 1200;
    t->start = 0;
    t->stop_at = 0;
    t->play_pos = 0;
    t->play_i = 0;
    t->bar = 0;
    t->bar_ctx = 0;
    t->pre = 0;
    t->pre_ctx = 0;
}

void take_set_bar_fn(take_t *t, take_bar_fn fn, void *ctx)
{
    if (!t)
        return;
    t->bar = fn;
    t->bar_ctx = ctx;
}

void take_set_prelude(take_t *t, take_prelude_fn fn, void *ctx)
{
    if (!t)
        return;
    t->pre = fn;
    t->pre_ctx = ctx;
}

void take_set_bpm(take_t *t, uint16_t bpm_x10)
{
    if (t)
        t->hdr.bpm_x10 = bpm_x10;
}

/* Only the arm's prelude builds offset-0 events (`arming`); a stray call cannot touch a log. */
void take_setup_event(take_t *t, uint32_t cmd)
{
    if (t && t->arming)
        take_put(t, 0, cmd);
}

/* ---- recorder ------------------------------------------------------------ */

void take_arm(take_t *t, uint32_t now)
{
    if (!t)
        return;

    /* Build the log while state == IDLE: take_feed() ignores a feed in IDLE, so an ISR event
     * cannot interleave with the start mark or the prelude. Publish start, then the state. */
    t->state = TAKE_IDLE;
    t->arming = 1;
    t->ended = 0;
    t->full = 0;
    t->stop_set = 0;
    t->n = 0;
    t->hdr.nevents = 0;
    t->hdr.length_samples = 0;

    take_put(t, 0, TAKE_MARK_START);
    if (!t->ended && t->pre)
        t->pre(t->pre_ctx, t);

    /* if the prelude wrote a tempo MARK, the header follows it: one source of truth */
    {
        uint16_t i;
        for (i = 0; i < t->n; i++)
            if (TAKE_CMD_TYPE(t->ev[i].cmd) == TAKE_T_MARK && TAKE_CMD_A(t->ev[i].cmd) == 2u)
                t->hdr.bpm_x10 = (uint16_t)TAKE_MARK_TEMPO_GET(TAKE_CMD_B(t->ev[i].cmd),
                                                              TAKE_CMD_C(t->ev[i].cmd));
    }

    t->hdr.nevents = t->n;
    t->start = bar_of(t, now);
    t->arming = 0;
    if (t->ended)
        return;                          /* the prelude itself filled the buffer */
    TAKE_BARRIER();                      /* publish start before the state */
    t->state = (int32_t)(now - t->start) >= 0 ? TAKE_REC : TAKE_ARMED;
}

void take_tick(take_t *t, uint32_t now)
{
    if (!t)
        return;
    if (t->state == TAKE_ARMED && (int32_t)(now - t->start) >= 0) {
        TAKE_BARRIER();
        t->state = TAKE_REC;
    }
    if (t->state == TAKE_REC && t->stop_set && (int32_t)(now - t->stop_at) >= 0)
        take_end(t, t->stop_at - t->start, 0);
}

void take_feed(take_t *t, uint32_t sample, uint32_t cmd)
{
    if (!t)
        return;
    take_tick(t, sample);
    if (t->state != TAKE_REC)
        return;
    if ((int32_t)(sample - t->start) < 0)   /* before the take's zero: drop, do not clamp */
        return;
    take_put(t, sample - t->start, cmd);
}

void take_stop(take_t *t, uint32_t now)
{
    if (!t)
        return;
    if (t->state == TAKE_IDLE || t->state == TAKE_PLAY)
        return;
    if (t->state == TAKE_ARMED && (int32_t)(t->start - now) > 0) {
        take_clear(t);                   /* never started: cancel the arm */
        return;
    }
    if (t->state == TAKE_ARMED) {
        TAKE_BARRIER();
        t->state = TAKE_REC;
    }
    /* Request only: the ISR's take_tick() runs take_end(), so the END mark is written once. */
    t->stop_at = bar_of(t, now);
    TAKE_BARRIER();                      /* publish stop_at before stop_set */
    t->stop_set = 1;
}

/* T9i: end the take at once, on `now`, with no wait for a bar boundary. Leaving LIVE must close the
 * take immediately (the transport may never reach another bar), so the recording is not lost and no
 * further feed is appended. An arm that never started is cancelled (like take_stop). */
void take_finish(take_t *t, uint32_t now)
{
    if (!t)
        return;
    if (t->state == TAKE_IDLE || t->state == TAKE_PLAY)
        return;
    if (t->state == TAKE_ARMED && (int32_t)(t->start - now) > 0) {
        take_clear(t);                   /* never started: cancel the arm */
        return;
    }
    take_end(t, (uint32_t)(now - t->start), 0);
}

/* ---- player -------------------------------------------------------------- */

void take_play(take_t *t, uint32_t now)
{
    (void)now;
    if (!t || t->n == 0)
        return;
    t->play_i = 0;
    t->play_pos = 0;
    TAKE_BARRIER();                      /* publish the cursor before the state */
    t->state = TAKE_PLAY;
}

void take_render(take_t *t, uint32_t nframes, const take_sink_t *sink)
{
    uint32_t end;
    if (!t || t->state != TAKE_PLAY || nframes == 0)
        return;
    end = t->play_pos + nframes;
    while (t->play_i < t->n && (int32_t)(t->ev[t->play_i].sample - end) < 0) {
        if (sink && sink->ev)
            sink->ev(sink->ctx, t->ev[t->play_i].sample - t->play_pos, t->ev[t->play_i].cmd);
        t->play_i++;
    }
    t->play_pos = end;
}

void take_stop_play(take_t *t)
{
    if (t && t->state == TAKE_PLAY)
        t->state = TAKE_IDLE;
}

/* ---- queries ------------------------------------------------------------- */

int take_state(const take_t *t)      { return t ? t->state : TAKE_IDLE; }
int take_recording(const take_t *t)  { return t && t->state == TAKE_REC; }
int take_armed(const take_t *t)      { return t && t->state == TAKE_ARMED; }
int take_playing(const take_t *t)    { return t && t->state == TAKE_PLAY; }
int take_full(const take_t *t)       { return t && t->full; }
int take_has(const take_t *t)        { return t && t->n > 0; }
uint16_t take_nevents(const take_t *t) { return t ? t->n : 0; }
const take_hdr_t *take_header(const take_t *t) { return t ? &t->hdr : 0; }
const take_event_t *take_events(const take_t *t) { return t ? t->ev : 0; }
