/* SPDX-License-Identifier: GPL-3.0-only */
/* T11i: MIDI input on the device; see midi_live.c.
 *
 * One interface for the app: poll it from the main loop (the ring is drained there, in the same
 * context as the clock-lost check), and it turns the incoming TRS/USB MIDI into the focused
 * role's notes, quantized clip launches, a followed clock and the transport. This header stays
 * free of model.h and live_bridge.h so engine.c (or any other unit) can include it without the
 * two `song_t`s colliding. */
#pragma once
#include <stdint.h>

/* The main loop: drain the shared TRS/USB ring, feed the parser, follow the clock. `sample` is the
 * audio sample counter's value now. Returns the packets taken. */
int midi_live_poll(uint32_t sample);

/* One ring packet at its sample timestamp (also what the runner's synthetic MIDI uses). */
void midi_live_feed(uint32_t pkt, uint32_t sample);

/* The clock-lost check and the tempo handoff, called by midi_live_poll and, for sample-accurate
 * tests, directly after a batch of feed()s. */
void midi_live_tick(uint32_t sample);

/* The LIVE SETUP rows (and the host runner): write the current song's live channel / PC launch. */
void midi_live_set_song_cfg(uint8_t live_channel, uint8_t pc_launch);

/* The GLOBAL clock source: 1 = Internal (ignore the incoming clock). */
void midi_live_set_clock(int internal);

/* Release every note external MIDI is holding; a view/mode change and the panic path call it. */
void midi_live_note_all_off(void);

/* Drop held notes without sounding them (a mode/view change that must not leave a note held). */
void midi_live_reset(void);

/* 1 while an external clock is being followed (the header's EXT chip). */
int midi_live_ext(void);

/* The followed tempo, bpm * 10 (0 until one is known). */
uint16_t midi_live_bpm_x10(void);

/* How many notes external MIDI is holding (the "no hung notes" check for the tests). */
int midi_live_held(void);
