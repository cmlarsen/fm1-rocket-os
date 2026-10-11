/* SPDX-License-Identifier: GPL-3.0-only */
/* T11c MIDI input core: one byte-stream parser plus a clock follower.
 *
 * One midi_in_t is one input stream; TRS and USB each get their own instance. The caller feeds
 * every byte (midi_in_byte) or every USB-MIDI 4-byte event packet (midi_in_packet) with the
 * sample-counter value of its arrival, and calls midi_in_tick() from the main loop so a stalled
 * clock can be reported. Everything is integer math: no heap, no double, no 64-bit division,
 * constant time per byte. The state is caller-allocated and only includes core/model/model.h.
 *
 * The parser keeps running status, passes SysEx over without corrupting state, treats Note On
 * with velocity 0 as Note Off, and accepts real-time bytes interleaved anywhere (inside another
 * message, inside SysEx). ROUTING is driven by the config (live_channel / pc_launch / pc_quant,
 * taken from the song; see midi_cfg_from_song):
 *   - notes on the live channel (1..16, or 0 = omni) reach sink.note_on / sink.note_off;
 *   - Program Change on the live channel launches clip = program (0..NCLIPS-1) through
 *     sink.launch, quantized by pc_quant, when pc_launch is set; out-of-range programs are
 *     ignored;
 *   - CC 120/123 reach sink.all_off.
 *
 * Clock follow: 0xF8 ticks give a smoothed tempo (bpm_x10) over a one-beat window, steady within
 * +/-0.5 bpm of clean input and reacting to a tempo change within one beat; 0xFA/0xFB/0xFC reach
 * the transport callbacks; F2 Song Position Pointer reaches sink.spp. When ticks stop for more
 * than one beat at the last tempo, sink.clock_lost fires once (until ticks resume).
 *
 * This module is standalone. T11i wires it to the TRS/USB inputs, the clip engine and the LIVE
 * play layer; see reports/T11c.md.
 */
#pragma once
#include <stdint.h>
#include "core/model/model.h"

/* Config taken from the song (midi_cfg_from_song fills it). */
typedef struct {
    uint8_t live_channel;  /* 1..16, or 0 = omni   (song_t.live_channel) */
    uint8_t pc_launch;     /* nonzero: PC launches  (song_t.pc_launch)     */
    uint8_t pc_quant;      /* Q_BAR / Q_BEAT / Q_END for a PC launch       */
    uint8_t cable;         /* USB cable index to accept (0 for the FM-1)   */
} midi_cfg_t;

/* What the parser reports. Every field may be NULL; ctx is passed back unchanged. */
typedef struct {
    void *ctx;
    void (*note_on)(void *ctx, uint8_t note, uint8_t vel);   /* live channel, vel 1..127 */
    void (*note_off)(void *ctx, uint8_t note);               /* live channel             */
    void (*all_off)(void *ctx);                              /* CC 120 or CC 123         */
    void (*launch)(void *ctx, uint8_t clip, uint8_t quant);  /* Program Change           */
    void (*start)(void *ctx);                                /* 0xFA                     */
    void (*cont)(void *ctx);                                 /* 0xFB                     */
    void (*stop)(void *ctx);                                 /* 0xFC                     */
    void (*spp)(void *ctx, uint16_t pos16);                  /* F2, in 16th notes        */
    void (*clock_lost)(void *ctx);                           /* no ticks for > 1 beat    */
} midi_sink_t;

#define MIDI_WINDOW 24u   /* one beat of MIDI clock (24 ppqn) is the tempo window */

typedef struct {
    midi_cfg_t  cfg;
    midi_sink_t sink;
    uint32_t    fs;          /* sample-counter rate, for bpm_x10 */

    /* running-status parser */
    uint8_t st;              /* channel running status, 0 = none */
    uint8_t syst;            /* system-common status being collected, 0 = none */
    uint8_t need;            /* data bytes the current status wants (0..2) */
    uint8_t got;             /* data bytes received for it */
    uint8_t d0;              /* first data byte */
    uint8_t sys;             /* 1 inside SysEx */

    /* clock follow */
    uint8_t  have_tick;      /* at least one F8 seen */
    uint8_t  clock_ok;       /* following the clock (cleared by clock_lost) */
    uint8_t  ring_i;         /* next slot in the tick ring */
    uint8_t  ring_n;         /* slots filled, capped at MIDI_WINDOW */
    uint32_t ring[MIDI_WINDOW];  /* the last MIDI_WINDOW tick intervals, in samples */
    uint32_t sum;            /* their sum: samples per beat once ring_n == MIDI_WINDOW */
    uint32_t est_fp;         /* smoothed estimate, Q10 samples per beat */
    uint32_t last_tick;      /* sample of the last F8 */
    uint16_t bpm_x10;        /* the estimate as bpm*10, 0 until the first tick arrives */
} midi_in_t;

#ifndef MIDI_FS
#define MIDI_FS 44100u   /* X0X's sample counter rate; override or midi_in_set_fs() */
#endif
#define MIDI_BPM_MIN 200u    /* 20.0 BPM: below the clock is treated as a gap */
#define MIDI_BPM_MAX 3000u   /* 300.0 BPM */

void midi_in_init(midi_in_t *m, const midi_cfg_t *cfg, const midi_sink_t *sink);
void midi_in_set_cfg(midi_in_t *m, const midi_cfg_t *cfg);
void midi_in_set_fs(midi_in_t *m, uint32_t fs);

/* Feed one parsed byte (TRS) at its sample timestamp. */
void midi_in_byte(midi_in_t *m, uint8_t b, uint32_t sample);

/* Feed one USB-MIDI 4-byte event packet at its sample timestamp. The packet is the low byte
 * first: byte0 = cable<<4 | CIN, byte1 = status, byte2 = d1, byte3 = d2 (exactly what X0X's
 * plat_midi_in returns and what midi_uart.c packs). The cable and code index are decoded. */
void midi_in_packet(midi_in_t *m, uint32_t pkt, uint32_t sample);

/* Call from the main loop with the current sample value; fires clock_lost once when ticks have
 * been silent for longer than one beat at the last tempo. */
void midi_in_tick(midi_in_t *m, uint32_t sample);

uint16_t midi_in_bpm_x10(const midi_in_t *m);   /* 0 until a tempo is known */
uint8_t  midi_in_clock_ok(const midi_in_t *m);  /* 0 = clock lost, 1 = following */

static inline void midi_cfg_from_song(midi_cfg_t *c, const song_t *s)
{
    c->live_channel = s->live_channel;
    c->pc_launch    = s->pc_launch;
    c->pc_quant     = s->quant_default;
    c->cable        = 0u;
}
