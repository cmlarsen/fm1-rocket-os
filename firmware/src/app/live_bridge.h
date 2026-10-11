/* SPDX-License-Identifier: GPL-3.0-only */
/* T3: the bridge between X0X's project and the LIVE clip engine.
 *
 * This is the only place where X0X's project (pattern_t / the four parts) is turned into a
 * `core/model/model.h` song, and where the clip engine is run for the LIVE view. It owns the
 * LIVE `song_t` and one `clip_engine_t`. The events it produces are handed to the caller
 * (engine.c) in plain terms, so engine.c applies them to the voices directly and
 * sample-accurately; this module never queues an engine command from the clip render.
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

/* A clip event, in plain terms, so engine.c can apply it without seeing model.h.
 *   DRUM:      a = lane, b = accent 0/1, c = clip
 *   NOTE_ON:   a = note, b = velocity (bit 7 = slide), c = clip, role = the part
 *   NOTE_OFF:  a = note, role = the part
 *   MOTION:    a = index, b = (target << 4) | voice, c = value */
enum { LIVE_EV_DRUM, LIVE_EV_NOTE_ON, LIVE_EV_NOTE_OFF, LIVE_EV_MOTION };
typedef struct {
    uint32_t sample;      /* offset into the window passed to live_bridge_render() */
    uint8_t kind;         /* LIVE_EV_* */
    uint8_t role;         /* LIVE_DRUMS / LIVE_BASS / LIVE_LEAD */
    uint8_t a, b, c;
} live_ev_t;

typedef struct {
    void (*ev)(void *ctx, const live_ev_t *e);
    void *ctx;
} live_sink_t;

/* Set up the static song and clip engine. Call once from engine_init()/ui_init(). */
void live_bridge_init(void);

/* T6i: the current song. `song_io` reads and edits it (a model.h `song_t *`, void here so this
 * header keeps X0X's own `song_t` out), re-inits the clip engine after a change, and keeps a hash
 * of the project's patterns the song's clips were derived from, so LIVE (and a boot) can rebuild
 * them when the project changed. */
void *live_bridge_song(void);
void  live_bridge_song_reload(void);
void  live_bridge_sync_tempo(void);                      /* T7a: X0X's sequencer follows the song's bpm */
uint32_t live_bridge_pats_hash(const pattern_t *pats);   /* FNV-1a over proj.pat */
uint32_t live_bridge_song_hash(void);                    /* the hash the current clips match */
/* Re-derive the clips from the project, keeping the song's own fields (key map, LIVE SETUP, tempo,
 * name); returns the new hash. `song_baseline` says a loaded song's clips match this hash. */
uint32_t live_bridge_rebuild(const pattern_t *pats);
uint32_t live_bridge_rebuild_n(const pattern_t *pats, int npats);   /* T10i: the first npats */
void     live_bridge_song_baseline(uint32_t pat_hash);

/* T7a: make the bridge the audio source (the clip engine). The clip transport is not reset, so
 * clips keep playing across a mode change. `pats` is proj.pat, used only to seed the song when
 * nothing has been built or loaded yet. `bpm_x10` is ignored: the song's own tempo is authoritative.
 * Returns 0 (the T6i pattern-hash rebuild is gone). */
int live_bridge_enter(const pattern_t *pats, uint16_t bpm_x10);

/* Stop the clip transport, release every sounding note and hand the audio back to X0X. */
void live_bridge_exit(void);

/* 1 while LIVE owns the audio path (between enter and exit). */
int live_bridge_active(void);

/* One audio block: advance the clip engine and hand each event to `sink` at its exact sample
 * offset. Called from engine_render() in place of seq_advance() while active; engine.c's sink
 * applies the events to the voices in the ISR. */
void live_bridge_render(uint32_t nframes, const live_sink_t *sink);

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

/* T10i: the sample distances from the transport's current position to its next 1/8 and 1/16 grid
 * boundaries (unswung, like the quantize points), for the punch FX. Only meaningful while the
 * transport is playing; engine.c passes PERFORM_NO_GRID when it is stopped. */
void live_bridge_grid(uint32_t *next_8, uint32_t *next_16);

/* T10i: role mutes, through the clip engine (T10i's CLIP_CMD_MUTE). Muting releases the role's
 * sounding notes and suppresses new ones; the transport keeps running. */
void live_bridge_mute(uint8_t role, int on);
int  live_bridge_muted(uint8_t role);
void live_bridge_mute_all(int on);                  /* leaving LIVE clears every mute */
/* T10i review: the song's stored role mute (always 0 for a live mute; kept separate from the
 * engine's runtime mute so a test can see that a live mute is not song state). */
int  live_bridge_song_role_muted(uint8_t role);

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
/* T10i: the same, over the first `npats` patterns. rc_debug's stress content is a single pattern,
 * so it need not carry a full NPAT array in the pool. */
void live_bridge_map_n(const pattern_t *pats, int npats, void *song);
