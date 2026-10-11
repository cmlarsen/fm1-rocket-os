/* SPDX-License-Identifier: GPL-3.0-only */
/* T3: the bridge between X0X's project and the LIVE clip engine.
 *
 * This is the only place where X0X's project (pattern_t / the four parts) is turned into a
 * `core/model/model.h` song and where the clip engine's events are mapped onto X0X's engine
 * commands. It owns the LIVE `song_t` and one `clip_engine_t`.
 *
 * The header stays free of model.h and engine.h on purpose: ui.c is one compilation unit that
 * already has X0X's own `song_t` (sequencer.h) in scope, so it can only include a header that
 * speaks plain types. live_bridge.c includes the two worlds and reconciles the one name they
 * share (`song_t`) with a local rename; see the top of that file.
 *
 * X0X's sequencer and the clip engine are never both driving the transport: while LIVE is
 * active engine.c calls live_bridge_render() instead of seq_advance(). */
#pragma once
#include <stdint.h>
#include "firmware/src/seq/pattern.h"   /* root-relative: -I. is on every build that includes this */

/* Role indices, matching core/model/model.h (frozen): DRUMS, BASS, LEAD, CHORDS, FX. */
enum { LIVE_DRUMS, LIVE_BASS, LIVE_LEAD, LIVE_CHORDS, LIVE_FX, LIVE_NROLES };

/* The two LIVE layers, toggled by SCL. */
enum { LIVE_PADS, LIVE_PLAY };

#define LIVE_NCLIPS 27u          /* core/model/model.h's NCLIPS (asserted in live_bridge.c) */
#define LIVE_CLIP_NONE 0xFFu     /* core/model/model.h's CLIP_NONE */

/* Behavior of a clip's motion lane's `param` field, which model.h leaves free: it is the
 * engine.h (target, voice, index) triple, packed so the low byte is the index and the high
 * byte is (target << 4) | voice. The bridge unpacks it back into engine_set(). */
#define LIVE_PARAM(t, v, i) ((uint16_t)(((uint16_t)(t) << 12) | ((uint16_t)(v) << 8) | (uint8_t)(i)))

/* Set up the static song and clip engine. Call once from engine_init()/ui_init(). */
void live_bridge_init(void);

/* Build the song from the project's patterns and make the bridge the audio source. The clip
 * transport starts stopped. `pats` is proj.pat (NPAT patterns), `bpm_x10` the project tempo. */
void live_bridge_enter(const pattern_t *pats, uint16_t bpm_x10);

/* Stop the clip transport, release every sounding note and hand the audio back to X0X. */
void live_bridge_exit(void);

/* 1 while LIVE owns the audio path (between enter and exit). */
int live_bridge_active(void);

/* One audio block: advance the clip engine and map its events onto X0X's engine commands.
 * Called from engine_render() in place of seq_advance() while active. */
void live_bridge_render(uint32_t nframes);

/* Transport (the PLAY button in LIVE). */
void live_bridge_transport_start(void);
void live_bridge_transport_stop(void);
int  live_bridge_transport_playing(void);

/* Clip launching, straight through to the clip engine's ring (main-loop safe). */
void live_bridge_launch(uint8_t clip);      /* at the clip's own quantize */
void live_bridge_launch_now(uint8_t clip);  /* HOME + key: the next step */
void live_bridge_stop(uint8_t role);        /* stop the role at its quantize */

/* PLAY layer: a key down/up on the focused role, through X0X's engine commands. `note` is the
 * final MIDI note (the UI has applied its octave); `vel` is 0..255 (>=100 accents the 303). */
void live_bridge_play_key(uint8_t role, uint8_t note, int down, uint8_t vel);

/* Tempo, in the clip engine's single source of truth. */
void live_bridge_set_bpm(uint16_t bpm_x10);
uint16_t live_bridge_bpm_x10(void);

/* UI queries. */
uint8_t live_bridge_playing_clip(uint8_t role);   /* CLIP_NONE when stopped */
uint8_t live_bridge_queued_clip(uint8_t role);    /* CLIP_NONE when nothing is queued */
uint8_t live_bridge_progress(uint8_t role);       /* 0..255 through the playing clip */
void    live_bridge_pos(uint8_t *bar, uint8_t *beat, uint8_t *step);
int     live_bridge_stop_queued(int role);        /* 1 while a stop on the role is pending */

/* The clip map and the role layout (model.h's, kept here so UI code needs no model.h). */
int live_bridge_role_first(int role);             /* the role's first clip index, -1 if no role */
int live_bridge_role_nclips(int role);            /* the role's clip count, 0 if no role */
int live_bridge_role_of(int clip);                /* the role a clip belongs to */
int live_bridge_clip_used(int clip);              /* 1 if the clip slot holds a clip */
int live_bridge_key_clip(int key);                /* song.key_clip[key] */

/* The pure pattern -> song mapping (T3 has a unit test for it). `song` is a model.h `song_t *`;
 * it is `void *` here only so this header can be included from ui.c. Fills all 27 clips and the
 * roles; CHORDS and FX stay empty. */
void live_bridge_map(const pattern_t *pats, void *song);
