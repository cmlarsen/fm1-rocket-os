/* SPDX-License-Identifier: GPL-3.0-only */
/* T11c MIDI input core; see midi_in.h for the contract.
 *
 * The parser is a small state machine over one byte stream. Running status: a channel status
 * byte (0x80..0xEF) is remembered and its data bytes keep arriving without a repeat of it. A
 * System Common status (0xF1..0xF6) and SysEx (0xF0) cancel running status, as the spec says.
 * Real-time bytes (0xF8..0xFF) are handled the moment they arrive and never touch the parser
 * state, so they may fall inside another message or inside SysEx. Inside SysEx every non
 * real-time byte is dropped until 0xF7.
 *
 * Tempo. The window is one beat: the 24 tick intervals of a MIDI beat. Their sum is the beat
 * length, and the estimate is a one-pole average of that window:
 *     est_fp += (window_fp - est_fp) / 8
 * in Q10 samples per beat, with 600*fs as the numerator when it turns into bpm_x10
 * (bpm = 60*fs/beat, so bpm_x10 = 600*fs/beat). A jitter-free beat is exact; the average kills
 * the jitter. When the window jumps by more than 1/64 of the estimate the tempo really changed,
 * so the filter snaps to the new window and a step is adopted within one beat.
 *
 * Dropped ticks. An interval over 1.5 expected ticks is a missed tick, not a tempo: it is held
 * and neither it nor the following interval enters the window, so a dropped F8 cannot inflate
 * the sum. Two such intervals in a row are a real tempo change (a step is sustained, a drop is
 * not), so the window is rebuilt from the new interval. An interval of a beat or more, or one
 * slower than 20 BPM (a tick over fs/8), resyncs the window.
 *
 * Every product here stays in 32 bits: an accepted interval is at most fs/8, so d<<10 * 24 and
 * sum<<10 both fit, and every divide is 32-bit.
 */
#include "midi_in.h"

#define MIDI_QSHIFT      10u   /* Q10 fixed point for est_fp (samples per beat) */
#define MIDI_SLOW_DEN     8u   /* one-pole: est_fp += (window - est_fp) / 8 */
#define MIDI_DETECT_DEN  64u   /* a jump over est_fp/64 is a tempo change */
#define MIDI_GAP_NUM     16u   /* d*MIDI_GAP_NUM > beat means d > 1.5 expected ticks */
#define MIDI_FLOOR_DIV    8u   /* a tick over fs/8 is under 20 BPM: resync */

static uint16_t bpm_from(const midi_in_t *m)
{
    /* round the Q10 estimate to whole samples, then bpm_x10 = 600*fs/beat */
    uint32_t beat = (m->est_fp + (1u << (MIDI_QSHIFT - 1u))) >> MIDI_QSHIFT;
    uint32_t b;
    if (beat == 0u)
        return 0u;
    b = (600u * m->fs) / beat;                     /* bpm * 10 */
    if (b < MIDI_BPM_MIN) b = MIDI_BPM_MIN;
    if (b > MIDI_BPM_MAX) b = MIDI_BPM_MAX;
    return (uint16_t)b;
}

/* ------------------------------------------------------------------ clock --- */

static void clock_tick(midi_in_t *m, uint32_t sample)
{
    uint32_t d, beat, dfp;

    m->clock_ok = 1u;
    if (!m->have_tick) {                            /* first F8: just anchor */
        m->have_tick = 1u;
        m->last_tick = sample;
        m->gap = 0u;
        return;
    }
    d = sample - m->last_tick;
    m->last_tick = sample;
    if (d == 0u)                                    /* a duplicate */
        return;
    if (d > m->fs / MIDI_FLOOR_DIV) {               /* under 20 BPM, or a long gap: resync */
        m->ring_i = 0u;
        m->ring_n = 0u;
        m->sum = 0u;
        m->gap = 0u;
        return;
    }

    if (m->est_fp != 0u) {
        beat = m->est_fp >> MIDI_QSHIFT;            /* samples per beat */
        if (d > beat) {                             /* a beat or more: the clock stopped */
            m->ring_i = 0u;
            m->ring_n = 0u;
            m->sum = 0u;
            m->gap = 0u;
            return;
        }
        if (d * MIDI_GAP_NUM > beat) {              /* over 1.5 ticks: a dropped tick */
            if (m->gap) {                           /* two in a row: a real tempo change */
                m->ring_i = 0u;
                m->ring_n = 0u;
                m->sum = 0u;
                m->est_fp = (d << MIDI_QSHIFT) * MIDI_WINDOW;   /* reseed from this tick */
                m->gap = 0u;                        /* fall through and add it */
            } else {
                m->gap = 1u;                        /* hold it: it and the next do not enter */
                return;
            }
        } else if (m->gap) {                        /* the interval after an isolated gap */
            m->gap = 0u;
            return;
        }
    }

    if (m->ring_n < MIDI_WINDOW) {                  /* fill the one-beat window */
        m->ring[m->ring_i] = d;
        m->sum += d;
        m->ring_i = (uint8_t)((m->ring_i + 1u) % MIDI_WINDOW);
        m->ring_n++;
    } else {
        m->sum += d - m->ring[m->ring_i];
        m->ring[m->ring_i] = d;
        m->ring_i = (uint8_t)((m->ring_i + 1u) % MIDI_WINDOW);
    }

    dfp = (d << MIDI_QSHIFT) * MIDI_WINDOW;         /* one interval, scaled to a beat */
    if (m->ring_n < MIDI_WINDOW) {                  /* warm-up: average single intervals */
        if (m->est_fp == 0u)
            m->est_fp = dfp;
        else
            m->est_fp = (uint32_t)((int32_t)m->est_fp +
                        ((int32_t)(dfp - m->est_fp) / (int32_t)MIDI_SLOW_DEN));
    } else {
        uint32_t win = m->sum << MIDI_QSHIFT;       /* Q10 samples per beat */
        if (m->est_fp == 0u) {
            m->est_fp = win;
        } else {
            int32_t diff = (int32_t)(win - m->est_fp);
            uint32_t ad = diff < 0 ? (uint32_t)(-diff) : (uint32_t)diff;
            if (ad > m->est_fp / MIDI_DETECT_DEN)   /* tempo step: adopt it */
                m->est_fp = win;
            else
                m->est_fp = (uint32_t)((int32_t)m->est_fp + diff / (int32_t)MIDI_SLOW_DEN);
        }
    }
    m->bpm_x10 = bpm_from(m);
}

static void real_time(midi_in_t *m, uint8_t b, uint32_t sample)
{
    switch (b) {
    case 0xF8u: clock_tick(m, sample); break;
    case 0xFAu: if (m->sink.start) m->sink.start(m->sink.ctx); break;
    case 0xFBu: if (m->sink.cont)  m->sink.cont(m->sink.ctx);  break;
    case 0xFCu: if (m->sink.stop)  m->sink.stop(m->sink.ctx);  break;
    default: break;                                 /* F9, FD, FE, FF: ignored */
    }
}

/* --------------------------------------------------------------- routing --- */

static uint8_t ch_match(const midi_in_t *m, uint8_t status)
{
    uint8_t ch = (uint8_t)((status & 0x0Fu) + 1u);  /* 1..16 */
    return (uint8_t)(m->cfg.live_channel == 0u || m->cfg.live_channel == ch);
}

static void channel_message(midi_in_t *m, uint8_t status, uint8_t d1, uint8_t d2)
{
    switch (status & 0xF0u) {
    case 0x80u:                                     /* Note Off */
        if (ch_match(m, status) && m->sink.note_off)
            m->sink.note_off(m->sink.ctx, d1);
        break;
    case 0x90u:                                     /* Note On (vel 0 = off) */
        if (!ch_match(m, status))
            break;
        if (d2 == 0u) {
            if (m->sink.note_off) m->sink.note_off(m->sink.ctx, d1);
        } else if (m->sink.note_on) {
            m->sink.note_on(m->sink.ctx, d1, d2);
        }
        break;
    case 0xB0u:                                     /* Control Change */
        if ((d1 == 120u || d1 == 123u) && ch_match(m, status) && m->sink.all_off)
            m->sink.all_off(m->sink.ctx);
        break;
    case 0xC0u:                                     /* Program Change */
        if (m->cfg.pc_launch && ch_match(m, status) && d1 < NCLIPS && m->sink.launch)
            m->sink.launch(m->sink.ctx, d1, m->cfg.pc_quant);
        break;
    default:                                        /* aftertouch, pitch bend: not ours */
        break;
    }
}

static void system_common(midi_in_t *m, uint8_t status, uint8_t d1, uint8_t d2)
{
    if (status == 0xF2u && m->sink.spp)             /* Song Position Pointer, 16th notes */
        m->sink.spp(m->sink.ctx, (uint16_t)(d1 | ((uint16_t)d2 << 7)));
}

/* ------------------------------------------------------------------ parser -- */

void midi_in_byte(midi_in_t *m, uint8_t b, uint32_t sample)
{
    if (b >= 0xF8u) {                               /* real time: any time, keeps running status */
        real_time(m, b, sample);
        return;
    }
    if (b >= 0x80u) {                               /* a status byte */
        if (b == 0xF0u) {                           /* SysEx start: drop to EOX */
            m->sys = 1u;
            m->st = 0u;
            m->syst = 0u;
            m->need = m->got = 0u;
            return;
        }
        if (b == 0xF7u) {                           /* EOX: it cancels running status too */
            m->sys = 0u;
            m->st = 0u;
            m->syst = 0u;
            m->got = 0u;
            m->need = 0u;
            return;
        }
        m->sys = 0u;                                /* any other status byte ends SysEx */
        if (b < 0xF0u) {                            /* channel status: running status */
            m->st = b;
            m->syst = 0u;
            m->need = (uint8_t)((b & 0xE0u) == 0xC0u ? 1u : 2u);   /* Cx, Dx want one */
            m->got = 0u;
            return;
        }
        /* System common F1..F6: cancels running status */
        m->st = 0u;
        m->sys = 0u;
        m->got = 0u;
        m->syst = (b == 0xF1u || b == 0xF2u || b == 0xF3u) ? b : 0u;
        m->need = (uint8_t)(b == 0xF2u ? 2u : (b == 0xF1u || b == 0xF3u) ? 1u : 0u);
        return;
    }
    /* a data byte */
    if (m->sys)                                     /* inside SysEx: drop */
        return;
    if (m->syst) {                                  /* System Common data */
        if (m->need == 2u && m->got == 0u) {
            m->d0 = b;
            m->got = 1u;
            return;
        }
        {
            uint8_t d1 = m->need == 2u ? m->d0 : b;
            uint8_t d2 = m->need == 2u ? b : 0u;
            uint8_t st = m->syst;
            m->syst = 0u;
            m->got = 0u;
            system_common(m, st, d1, d2);
        }
        return;
    }
    if (!m->st)                                     /* no running status: ignore */
        return;
    if (m->need == 2u && m->got == 0u) {
        m->d0 = b;
        m->got = 1u;
        return;
    }
    {
        uint8_t d1 = m->need == 2u ? m->d0 : b;
        uint8_t d2 = m->need == 2u ? b : 0u;
        m->got = 0u;
        channel_message(m, m->st, (uint8_t)(d1 & 0x7Fu), (uint8_t)(d2 & 0x7Fu));
    }
}

/* CIN (code index number) -> how many of the packet's 3 data bytes carry MIDI. */
static const uint8_t CIN_LEN[16] = {
    0u, 0u, 2u, 3u, 3u, 1u, 2u, 3u, 3u, 3u, 3u, 3u, 2u, 2u, 3u, 1u
};

void midi_in_packet(midi_in_t *m, uint32_t pkt, uint32_t sample)
{
    uint8_t cable = (uint8_t)((pkt >> 4) & 0x0Fu);
    uint8_t cin = (uint8_t)(pkt & 0x0Fu);
    uint8_t n, b1, b2, b3;

    if (cable != m->cfg.cable)
        return;
    n = CIN_LEN[cin];
    b1 = (uint8_t)(pkt >> 8);
    b2 = (uint8_t)(pkt >> 16);
    b3 = (uint8_t)(pkt >> 24);
    if (n >= 1u) midi_in_byte(m, b1, sample);
    if (n >= 2u) midi_in_byte(m, b2, sample);
    if (n >= 3u) midi_in_byte(m, b3, sample);
}

void midi_in_tick(midi_in_t *m, uint32_t sample)
{
    uint32_t timeout;
    if (!m->have_tick || !m->clock_ok)
        return;
    if (m->est_fp == 0u)
        timeout = m->fs;                            /* no tempo yet: a fixed one second */
    else
        timeout = m->est_fp >> MIDI_QSHIFT;         /* one beat at the estimate */
    if ((int32_t)(sample - m->last_tick) > (int32_t)timeout) {
        m->clock_ok = 0u;
        if (m->sink.clock_lost)
            m->sink.clock_lost(m->sink.ctx);
    }
}

/* -------------------------------------------------------------------- api -- */

void midi_in_init(midi_in_t *m, const midi_cfg_t *cfg, const midi_sink_t *sink)
{
    uint8_t *p = (uint8_t *)m;
    unsigned i;
    for (i = 0; i < (unsigned)sizeof(*m); i++)
        p[i] = 0u;
    if (cfg)  m->cfg = *cfg;
    if (sink) m->sink = *sink;
    m->fs = MIDI_FS;
}

void midi_in_set_cfg(midi_in_t *m, const midi_cfg_t *cfg)
{
    if (cfg) m->cfg = *cfg;
}

void midi_in_set_fs(midi_in_t *m, uint32_t fs)
{
    if (fs) m->fs = fs;
}

uint16_t midi_in_bpm_x10(const midi_in_t *m) { return m->bpm_x10; }
uint8_t  midi_in_clock_ok(const midi_in_t *m) { return m->clock_ok; }
