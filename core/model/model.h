/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>

#define MODEL_VERSION 2u

enum { ROLE_DRUMS, ROLE_BASS, ROLE_LEAD, ROLE_CHORDS, ROLE_FX, NROLES };
enum { ENG_909, ENG_808, ENG_303, ENG_FM6, ENG_NOISE, NENGINES };
enum { CLIP_LOOP, CLIP_ONCE };
enum { Q_BAR, Q_BEAT, Q_END, NQUANT };
enum { SCALE_MAJOR, SCALE_MINOR, SCALE_DORIAN, SCALE_PENTA, SCALE_CHROMATIC, NSCALES };

#define ROLE_CLIPS_DRUMS 7u
#define ROLE_CLIPS_BASS 5u
#define ROLE_CLIPS_LEAD 7u
#define ROLE_CLIPS_CHORDS 5u
#define ROLE_CLIPS_FX 3u
#define NCLIPS 27u
#define NKEYS 27u
#define CLIP_NONE 0xFFu

/* TLSET v2: no struct growth; v1 maps migrate byte-for-byte. */
#define KEY_STOP_FIRST 27u
#define KEY_FX_FIRST 32u
#define KEY_LIVE_NOTE 38u
#define KEY_TARGET_COUNT 39u
static inline int key_target_valid(unsigned v) { return v < KEY_TARGET_COUNT || v == CLIP_NONE; }

#define CLIP_MAX_STEPS 64u
#define STEPS_PER_BAR 16u
#define CLIP_SW (CLIP_MAX_STEPS / 32u)
#define DRUM_LANES 11u
#define CLIP_MAX_NOTES 96u
#define CLIP_MOTION_LANES 2u
#define MOTION_NONE 0xFFu
#define PARAM_NONE 0xFFFFu

#define SONG_SLOTS 5u
#define SONG_NAME_LEN 12u
#define TAKE_MAX_EVENTS 1024u

typedef struct {
    uint8_t step;
    uint8_t note;
    uint8_t vel;
    uint8_t len;
} note_t;

typedef struct {
    uint32_t hit[DRUM_LANES][CLIP_SW];
    uint32_t accent[CLIP_SW];
} drum_steps_t;

typedef struct {
    uint16_t param;
    uint8_t rsv[2];
    uint8_t v[CLIP_MAX_STEPS];
} motion_lane_t;

typedef struct {
    uint8_t used;
    uint8_t len;
    uint8_t mode;
    uint8_t quant;
    uint8_t nnotes;
    uint8_t rsv[3];
    union {
        drum_steps_t drum;
        note_t notes[CLIP_MAX_NOTES];
    } d;
    motion_lane_t motion[CLIP_MOTION_LANES];
} clip_t;

typedef struct {
    uint8_t engine;
    uint8_t preset;
    uint8_t volume;
    uint8_t muted;
    uint8_t first_clip;
    uint8_t nclips;
    uint8_t rsv[2];
} role_t;

typedef struct {
    uint32_t sample;
    uint32_t cmd;
} take_event_t;

typedef struct {
    uint16_t nevents;
    uint16_t bpm_x10;
    uint32_t length_samples;
} take_hdr_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t bpm_x10;
    char name[SONG_NAME_LEN];
    uint8_t swing;
    uint8_t quant_default;
    uint8_t live_channel;
    uint8_t pc_launch;
    uint8_t stage_lock;
    uint8_t has_take;
    uint8_t key;
    uint8_t scale;
    role_t role[NROLES];
    uint8_t key_clip[NKEYS];
    uint8_t rsv2[1];
    clip_t clip[NCLIPS];
} song_t;

#define SONG_MAGIC 0x474E4F53u

static inline uint8_t clip_role(uint8_t c)
{
    if (c < 7u) return ROLE_DRUMS;
    if (c < 12u) return ROLE_BASS;
    if (c < 19u) return ROLE_LEAD;
    if (c < 24u) return ROLE_CHORDS;
    return ROLE_FX;
}

static inline int drum_get(const drum_steps_t *d, unsigned lane, unsigned s)
{
    return (int)((d->hit[lane][s >> 5] >> (s & 31u)) & 1u);
}

static inline void drum_set(drum_steps_t *d, unsigned lane, unsigned s, int on)
{
    uint32_t m = 1u << (s & 31u);
    if (on) d->hit[lane][s >> 5] |= m;
    else d->hit[lane][s >> 5] &= ~m;
}

_Static_assert(sizeof(note_t) == 4, "note_t");
_Static_assert(sizeof(clip_t) == 8 + CLIP_MAX_NOTES * 4 + CLIP_MOTION_LANES * (4 + CLIP_MAX_STEPS), "clip_t");
_Static_assert(ROLE_CLIPS_DRUMS + ROLE_CLIPS_BASS + ROLE_CLIPS_LEAD + ROLE_CLIPS_CHORDS + ROLE_CLIPS_FX == NCLIPS, "clips");
_Static_assert(sizeof(song_t) < 16384, "song_t");
