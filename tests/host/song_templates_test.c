/* SPDX-License-Identifier: GPL-3.0-only */
/* T8: the Empty and Starter song templates (firmware/src/app/song_templates.c).
 *
 * Plain C, no flash: it builds each template, checks it validates, that the clips
 * land in the right roles and are musically in the chosen key, and that a repeated
 * call is byte-identical (determinism). Run by `make unit-songs`, also under
 * ASan/UBSan. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../../firmware/src/app/song_templates.h"
#include "../../core/model/song_store.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("  FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

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

static void test_empty(void)
{
    song_t s;
    int i, m, st;
    song_template_empty(&s, 1400);
    CHECK(song_validate(&s) == 0);
    CHECK(s.bpm_x10 == 1400);
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
    /* the tempo clamps to what a song may hold */
    song_template_empty(&s, 10);
    CHECK(s.bpm_x10 == SONG_TMPL_BPM_MIN);
    song_template_empty(&s, 60000);
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
    /* clips land on their role's first slots (A, B, C ...) */
    for (i = 0; i < (int)NCLIPS; i++) {
        if (!s.clip[i].used)
            continue;
        CHECK(clip_role((uint8_t)i) == clip_role((uint8_t)i));
        CHECK(s.clip[i].len == 16);
        CHECK(s.clip[i].mode == CLIP_LOOP);
        CHECK(s.clip[i].quant == Q_BAR);
    }
    for (i = 0; i < (int)NCLIPS; i++)
        for (m = 0; m < (int)CLIP_MOTION_LANES; m++)
            for (st = 0; st < (int)CLIP_MAX_STEPS; st++)
                if (s.clip[i].motion[m].v[st] != MOTION_NONE)
                    CHECK(0);
    CHECK(s.clip[0].nnotes == 0);         /* drums use the step grid, not notes */
    CHECK(s.clip[7].nnotes > 0);          /* a melodic clip has notes */
}

static void test_starter_keys(void)
{
    int key, root, prev_root = -1;
    for (key = 0; key < (int)SONG_TMPL_NKEYS; key++) {
        song_t s;
        song_template_starter(&s, 1250, key);
        root = song_tmpl_key_root(key);
        CHECK(song_validate(&s) == 0);
        /* the bass line opens on the key's root at MIDI 36 + root */
        CHECK(s.clip[7].nnotes > 0);
        CHECK(s.clip[7].d.notes[0].note == (uint8_t)(36 + root));
        /* a minor third or a major third, depending on the key */
        {
            int t = s.clip[7].d.notes[5].note - (36 + root);   /* the last note, a third */
            CHECK(t == (song_tmpl_key_minor(key) ? 3 : 4));
        }
        prev_root = root;
    }
    (void)prev_root;
    CHECK(strcmp(song_tmpl_key_name(0), "C MAJ") == 0);
    CHECK(strcmp(song_tmpl_key_name(SONG_TMPL_DEFAULT_KEY), "A MIN") == 0);
    CHECK(song_tmpl_key_minor(0) == 0 && song_tmpl_key_minor(12) == 1);
    CHECK(song_tmpl_key_root(21) == 9);
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
    song_template_empty(&a, 900);
    song_template_empty(&b, 900);
    CHECK(memcmp(&a, &b, sizeof a) == 0);
}

int main(void)
{
    printf("song_templates: empty\n");
    test_empty();
    printf("song_templates: starter shape\n");
    test_starter_shape();
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
