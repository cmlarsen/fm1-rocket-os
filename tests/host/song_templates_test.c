/* SPDX-License-Identifier: GPL-3.0-only */
/* T8: the Empty and Starter song templates (firmware/src/app/song_templates.c).
 *
 * Plain C, no flash: it builds each template, checks it validates, that the clips
 * land in the right roles, that the key and scale are stored, that the Starter's
 * chords follow the progression for its key, and that a repeated call is
 * byte-identical (determinism). Run by `make unit-songs`, also under ASan/UBSan. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../../firmware/src/app/song_templates.h"
#include "../../core/model/song_store.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("  FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* the 909 lane order (engine.c's N909): BD = 0, CH = 7, CR = 9 */
enum { TM_BD = 0, TM_CH = 7, TM_CR = 9 };

static int count_role(const song_t *s, int role)
{
    int i, n = 0;
    for (i = 0; i < (int)NCLIPS; i++)
        if (s->clip[i].used && clip_role((uint8_t)i) == role)
            n++;
    return n;
}
static int count_used(const song_t *s)
{
    int i, n = 0;
    for (i = 0; i < (int)NCLIPS; i++)
        if (s->clip[i].used)
            n++;
    return n;
}
/* the lowest note at `step` in a clip (the chord's root for a root-position triad) */
static int lowest_at(const clip_t *c, int step)
{
    int i, lo = 128, seen = 0;
    for (i = 0; i < (int)c->nnotes; i++)
        if (c->d.notes[i].step == (uint8_t)step) {
            seen = 1;
            if (c->d.notes[i].note < lo)
                lo = c->d.notes[i].note;
        }
    return seen ? lo : -1;
}

static void test_empty(void)
{
    song_t s;
    int i, m, st;
    song_template_empty(&s, 1400, SONG_TMPL_DEFAULT_KEY);
    CHECK(song_validate(&s) == 0);
    CHECK(s.bpm_x10 == 1400);
    CHECK(s.key == 9 && s.scale == SCALE_MINOR);          /* A MIN */
    CHECK(strcmp(s.name, "NEW SONG") == 0);
    CHECK(count_used(&s) == 0);
    for (i = 0; i < (int)NROLES; i++) {
        CHECK(s.role[i].nclips == (uint8_t)(i == ROLE_DRUMS ? ROLE_CLIPS_DRUMS
                                          : i == ROLE_BASS ? ROLE_CLIPS_BASS
                                          : i == ROLE_LEAD ? ROLE_CLIPS_LEAD
                                          : i == ROLE_CHORDS ? ROLE_CLIPS_CHORDS
                                          : ROLE_CLIPS_FX));
    }
    for (i = 0; i < (int)NCLIPS; i++)
        for (m = 0; m < (int)CLIP_MOTION_LANES; m++) {
            CHECK(s.clip[i].motion[m].param == PARAM_NONE);
            for (st = 0; st < (int)CLIP_MAX_STEPS; st++)
                CHECK(s.clip[i].motion[m].v[st] == MOTION_NONE);
        }
    /* an Empty in C major stores C major */
    song_template_empty(&s, 1250, 0);
    CHECK(song_validate(&s) == 0);
    CHECK(s.key == 0 && s.scale == SCALE_MAJOR);
    /* the tempo clamps to what a song may hold */
    song_template_empty(&s, 10, 0);
    CHECK(s.bpm_x10 == SONG_TMPL_BPM_MIN);
    song_template_empty(&s, 60000, 0);
    CHECK(s.bpm_x10 == SONG_TMPL_BPM_MAX);
}

static void test_starter_shape(void)
{
    song_t s;
    int i, m, st;
    song_template_starter(&s, 1250, SONG_TMPL_DEFAULT_KEY);
    CHECK(song_validate(&s) == 0);
    CHECK(s.bpm_x10 == 1250);
    CHECK(strcmp(s.name, "STARTER") == 0);
    CHECK(count_role(&s, ROLE_DRUMS) == 4);
    CHECK(count_role(&s, ROLE_BASS) == 3);
    CHECK(count_role(&s, ROLE_LEAD) == 3);
    CHECK(count_role(&s, ROLE_CHORDS) == 2);
    CHECK(count_role(&s, ROLE_FX) == 1);
    CHECK(count_used(&s) == 13);
    for (i = 0; i < (int)NCLIPS; i++) {
        if (!s.clip[i].used)
            continue;
        CHECK(s.clip[i].mode == CLIP_LOOP);
        CHECK(s.clip[i].quant == Q_BAR);
        if (clip_role((uint8_t)i) == ROLE_CHORDS)
            CHECK(s.clip[i].len == 64);           /* four bars, one chord per bar */
        else
            CHECK(s.clip[i].len == 16);
    }
    for (i = 0; i < (int)NCLIPS; i++)
        for (m = 0; m < (int)CLIP_MOTION_LANES; m++)
            for (st = 0; st < (int)CLIP_MAX_STEPS; st++)
                if (s.clip[i].motion[m].v[st] != MOTION_NONE)
                    CHECK(0);
    CHECK(s.clip[0].nnotes == 0);         /* drums use the step grid, not notes */
    CHECK(s.clip[7].nnotes > 0);          /* a melodic clip has notes */
    /* the beat is accented and has ghost hats on the off-beats */
    CHECK(drum_get(&s.clip[0].d.drum, TM_BD, 0) == 1);
    CHECK(drum_get(&s.clip[0].d.drum, TM_CH, 3) == 1);
    CHECK((s.clip[0].d.drum.accent[0] & 1u) != 0u);       /* step 0 accented */
    CHECK((s.clip[0].d.drum.accent[0] & (1u << 3)) == 0u);/* step 3 (a ghost) not */
    /* the fill's crash is on step 0, so the downbeat is marked */
    CHECK(drum_get(&s.clip[3].d.drum, TM_CR, 0) == 1);
    CHECK(drum_get(&s.clip[3].d.drum, TM_CR, 15) == 0);
}

/* the chord roots, bar by bar. Clip A (root position) is I-V-vi-IV in major and
 * i-VI-III-VII in minor; clip B is a different progression in a second inversion. */
static void test_chords(void)
{
    static const int MAJ_A[4] = {0, 7, 9, 5};    /* I  V  vi IV  */
    static const int MAJ_B[4] = {9, 5, 0, 7};    /* vi IV I  V   */
    static const int MIN_A[4] = {0, 8, 3, 10};   /* i  VI III VII */
    static const int MIN_B[4] = {8, 10, 0, 10};  /* VI VII i  VII */
    int key, b;
    for (key = 0; key < (int)SONG_TMPL_NKEYS; key++) {
        song_t s;
        int root = song_tmpl_key_root(key);
        int minor = song_tmpl_key_minor(key);
        const int *prog_a = minor ? MIN_A : MAJ_A;
        const int *prog_b = minor ? MIN_B : MAJ_B;
        song_template_starter(&s, 1250, key);
        CHECK(s.key == (uint8_t)root);
        CHECK(s.scale == song_tmpl_key_scale(key));
        CHECK(s.clip[19].len == 64 && s.clip[20].len == 64);
        for (b = 0; b < 4; b++) {
            int want_a = 60 + root + prog_a[b];
            int want_b = 60 + root + prog_b[b];
            int i, found = 0;
            CHECK(lowest_at(&s.clip[19], b * 16) == want_a);   /* root position */
            /* the answer clip is a second inversion: its root is an octave up */
            for (i = 0; i < (int)s.clip[20].nnotes; i++)
                if (s.clip[20].d.notes[i].step == (uint8_t)(b * 16) &&
                    s.clip[20].d.notes[i].note == want_b + 12)
                    found = 1;
            CHECK(found);
        }
    }
}

static void test_starter_keys(void)
{
    int key;
    for (key = 0; key < (int)SONG_TMPL_NKEYS; key++) {
        song_t s;
        int root = song_tmpl_key_root(key);
        song_template_starter(&s, 1250, key);
        CHECK(song_validate(&s) == 0);
        /* the bass line opens on the key's root at MIDI 36 + root */
        CHECK(s.clip[7].nnotes > 0);
        CHECK(s.clip[7].d.notes[0].note == (uint8_t)(36 + root));
        /* its last note is a third in the key's quality */
        CHECK(s.clip[7].d.notes[5].note - (36 + root) == (song_tmpl_key_minor(key) ? 3 : 4));
    }
    CHECK(strcmp(song_tmpl_key_name(0), "C MAJ") == 0);
    CHECK(strcmp(song_tmpl_key_name(SONG_TMPL_DEFAULT_KEY), "A MIN") == 0);
    CHECK(song_tmpl_key_minor(0) == 0 && song_tmpl_key_minor(12) == 1);
    CHECK(song_tmpl_key_root(21) == 9);
    CHECK(song_tmpl_key_scale(0) == SCALE_MAJOR && song_tmpl_key_scale(21) == SCALE_MINOR);
}

static void test_determinism(void)
{
    song_t a, b;
    int k;
    for (k = 0; k < (int)SONG_TMPL_NKEYS; k++) {
        song_template_starter(&a, 1250, k);
        song_template_starter(&b, 1250, k);
        CHECK(memcmp(&a, &b, sizeof a) == 0);
    }
    song_template_empty(&a, 900, 3);
    song_template_empty(&b, 900, 3);
    CHECK(memcmp(&a, &b, sizeof a) == 0);
}

int main(void)
{
    printf("song_templates: empty\n");
    test_empty();
    printf("song_templates: starter shape\n");
    test_starter_shape();
    printf("song_templates: chords\n");
    test_chords();
    printf("song_templates: starter keys\n");
    test_starter_keys();
    printf("song_templates: determinism\n");
    test_determinism();
    if (fails) {
        printf("song_templates: %d FAILURES\n", fails);
        return 1;
    }
    printf("song_templates: all passed\n");
    return 0;
}
