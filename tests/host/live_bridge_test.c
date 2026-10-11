/* SPDX-License-Identifier: GPL-3.0-only */
/* T3 unit test: the pure X0X-project -> LIVE-song mapping in live_bridge.c.
 *
 * The bridge sees both X0X's sequencer.h and core/model/model.h, which both name `song_t`;
 * compile this the same way live_bridge.c does: engine.h first, then the model under a rename.
 * engine.h's engine_* commands and the sequencer `seq` are stubbed below. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "firmware/src/app/engine.h"

#define song_t live_song_t
#include "core/model/model.h"
#undef song_t

#include "firmware/src/app/live_bridge.h"

/* ---- stubs the bridge links against ---- */
seq_t seq;
void engine_drum(int kit, int v, float vel) { (void)kit; (void)v; (void)vel; }
void engine_bass_on(int p, int n, int a, int s) { (void)p; (void)n; (void)a; (void)s; }
void engine_bass_off(int p) { (void)p; }
void engine_set(int t, int v, int i, int val) { (void)t; (void)v; (void)i; (void)val; }

static int fails;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

static pattern_t pats[NPAT];
static live_song_t sg;

static int count_used(int lo, int n)
{
    int i, k = 0;
    for (i = 0; i < n; i++)
        if (sg.clip[lo + i].used)
            k++;
    return k;
}

int main(void)
{
    memset(pats, 0, sizeof pats);

    /* P0: a 909 kick on steps 0 and 4 (accented on 0), a snare on step 8; a 303 A line
     * (accented note, a tie, a plain note, a rest) with a +2 transpose; a 303 B note. */
    sm_set(pats[0].drum[0].hit[0], 0);
    sm_set(pats[0].drum[0].hit[0], 4);
    sm_set(pats[0].drum[0].hit[3], 8);
    pats[0].drum[0].accent[0] = 1u << 0;
    pats[0].drum[0].len = 16;

    pats[0].bass[0].len = 8;
    pats[0].bass[0].transpose = 24 + 2;
    pats[0].bass[0].step[0] = (bstep_t){36, (uint8_t)(G_NOTE | BS_ACCENT)};
    pats[0].bass[0].step[1] = (bstep_t){0, G_TIE};
    pats[0].bass[0].step[2] = (bstep_t){38, G_NOTE};
    pats[0].bass[0].step[3] = (bstep_t){0, G_REST};

    pats[0].bass[1].len = 4;
    pats[0].bass[1].transpose = 24;
    pats[0].bass[1].step[0] = (bstep_t){48, G_NOTE};

    /* P1: a 909 snare/clap on step 2 only (no drums before it); no 303 lines */
    sm_set(pats[1].drum[0].hit[1], 2);
    pats[1].drum[0].len = 16;

    /* P2: no 909 at all; a 303 A note (a second BASS clip) */
    pats[2].bass[0].len = 4;
    pats[2].bass[0].transpose = 24;
    pats[2].bass[0].step[0] = (bstep_t){40, G_NOTE};

    live_bridge_map(pats, &sg);

    /* roles and their clip ranges */
    CHECK(sg.role[ROLE_DRUMS].engine == ENG_909);
    CHECK(sg.role[ROLE_DRUMS].first_clip == 0 && sg.role[ROLE_DRUMS].nclips == ROLE_CLIPS_DRUMS);
    CHECK(sg.role[ROLE_BASS].engine == ENG_303);
    CHECK(sg.role[ROLE_BASS].first_clip == 7 && sg.role[ROLE_BASS].nclips == ROLE_CLIPS_BASS);
    CHECK(sg.role[ROLE_LEAD].engine == ENG_303);
    CHECK(sg.role[ROLE_LEAD].first_clip == 12 && sg.role[ROLE_LEAD].nclips == ROLE_CLIPS_LEAD);
    CHECK(sg.role[ROLE_CHORDS].nclips == ROLE_CLIPS_CHORDS);
    CHECK(sg.role[ROLE_FX].nclips == ROLE_CLIPS_FX);

    /* drums: P0 -> clip A, P1 -> clip B, the rest empty; a direct dpart -> drum_steps_t map */
    CHECK(count_used(0, ROLE_CLIPS_DRUMS) == 2);
    CHECK(sg.clip[0].used && sg.clip[1].used && !sg.clip[2].used);
    CHECK(sg.clip[0].len == 16 && sg.clip[0].mode == CLIP_LOOP && sg.clip[0].quant == Q_BAR);
    CHECK(drum_get(&sg.clip[0].d.drum, 0, 0) && drum_get(&sg.clip[0].d.drum, 0, 4));
    CHECK(!drum_get(&sg.clip[0].d.drum, 0, 1));
    CHECK(drum_get(&sg.clip[0].d.drum, 3, 8));
    CHECK((sg.clip[0].d.drum.accent[0] & 1u) == 1u);          /* step 0 accented */
    CHECK(drum_get(&sg.clip[1].d.drum, 1, 2) && !drum_get(&sg.clip[1].d.drum, 0, 0));

    /* bass: P0 -> clip 7 (first non-empty 303 A), P2 -> clip 8; the tie extends the note and
     * the transpose is applied */
    CHECK(count_used(7, ROLE_CLIPS_BASS) == 2);
    CHECK(sg.clip[7].used && sg.clip[8].used && !sg.clip[9].used);
    CHECK(sg.clip[7].nnotes == 2);
    CHECK(sg.clip[7].d.notes[0].step == 0 && sg.clip[7].d.notes[0].note == 38 /* 36 + 2 */);
    CHECK(sg.clip[7].d.notes[0].vel == 127 && sg.clip[7].d.notes[0].len == 2);   /* accented, tie */
    CHECK(sg.clip[7].d.notes[1].step == 2 && sg.clip[7].d.notes[1].note == 40 /* 38 + 2 */);
    CHECK(sg.clip[7].d.notes[1].vel == 88 && sg.clip[7].d.notes[1].len == 1);
    CHECK(sg.clip[8].d.notes[0].note == 40 && sg.clip[8].nnotes == 1);

    /* lead: P0's 303 B -> clip 12 */
    CHECK(count_used(12, ROLE_CLIPS_LEAD) == 1);
    CHECK(sg.clip[12].used && sg.clip[12].d.notes[0].note == 48);

    /* CHORDS and FX stay empty */
    CHECK(count_used(19, ROLE_CLIPS_CHORDS) == 0);
    CHECK(count_used(24, ROLE_CLIPS_FX) == 0);

    /* every clip's motion lanes are empty (marked MOTION_NONE, not zero) */
    CHECK(sg.clip[0].motion[0].param == PARAM_NONE && sg.clip[0].motion[1].param == PARAM_NONE);
    CHECK(sg.clip[0].motion[0].v[0] == MOTION_NONE && sg.clip[7].motion[1].v[15] == MOTION_NONE);

    /* the key map defaults to identity */
    {
        int i, id = 1;
        for (i = 0; i < (int)NKEYS; i++)
            if (sg.key_clip[i] != i)
                id = 0;
        CHECK(id);
    }

    if (fails) {
        printf("live_bridge: %d check(s) failed\n", fails);
        return 1;
    }
    printf("live_bridge: mapping ok\n");
    return 0;
}
