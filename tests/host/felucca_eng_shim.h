/* SPDX-License-Identifier: GPL-3.0-only
 * fm1-rocket-os T5c: a slice of Felucca's core.h, enough for Felucca's ORIGINAL eng_noise.c to compile
 * as a host reference (tests/host/noise_ref_tu.c). The types are the engine's own; the reference driver
 * fills them by name, so only the fields the engine reads need to match. Include felucca_tables.h first. */
#pragma once
#include <stdint.h>

#define NELEM(a) (sizeof(a) / sizeof((a)[0]))

enum {
    F_INT, F_PCT, F_BIPCT, F_TIME, F_LFOHZ, F_CUTOFF, F_DB, F_SEMI, F_ENUM, F_BPM, F_NOTE,
    F_ONOFF, F_OCT, F_STEPS
};

typedef struct {
    const char *label;
    uint8_t fmt;
    int16_t min, max, def;
    const char *const *names;
    const char *unit;
} param_desc_t;

enum { P_E0 = 0, P_E1, P_E2, P_E3, P_E4, P_E5, P_E6, P_E7, P_REL, P_COUNT };

typedef struct { int16_t p[P_COUNT]; } track_t;

typedef struct {
    uint8_t note;
    int32_t fine;
    int32_t s[8];
    uint32_t ph[3];
} voice_t;

typedef struct {
    uint32_t inc;
    int32_t pitch16, amp0, amp1, cutoff, shape;
} vmod_t;

typedef struct {
    const char *name;
    int8_t e[8];
    uint8_t env[4];
    int8_t fenv;
    uint8_t mono;
    uint8_t fx[4];
    uint8_t pat;
} preset_t;
#define FX(d, c, dl, r) .fx = {(d) + 1, (c) + 1, (dl) + 1, (r) + 1}
#define PAT(n) .pat = (n)

typedef struct {
    const char *name;
    const char *page_title[2];
    param_desc_t edit[8];
    const preset_t *presets;
    uint8_t npresets;
    uint8_t knob[4];
    uint8_t poly, sampled, keep, oneshot;
    void (*note_on)(track_t *t, voice_t *v);
    void (*render)(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m);
    const param_desc_t *(*desc)(const track_t *t, uint32_t k);
} engine_t;
