/* SPDX-License-Identifier: GPL-3.0-only */
/* T3 unit test: the pure X0X-project -> LIVE-song mapping in live_bridge.c, the BS_SLIDE carry,
 * the 64-step (second hit word) and 7-pattern cap, and the bridge's event stream.
 *
 * The bridge sees both X0X's sequencer.h and core/model/model.h, which both name `song_t`;
 * compile this the same way live_bridge.c does: engine.h first, then the model under a rename.
 * engine.h's main-loop command functions are stubbed below (the bridge only uses them for the
 * PLAY layer and for enter/exit; the clip render now emits plain events, captured here). */
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
void engine_note_on(int part, int note, int vel) { (void)part; (void)note; (void)vel; }
void engine_note_off(int part, int note) { (void)part; (void)note; }
void engine_set(int t, int v, int i, int val) { (void)t; (void)v; (void)i; (void)val; }
void engine_all_off(void) {}
uint32_t engine_crit_enter(void) { return 0; }        /* T7b: the test has no ISR */
void engine_crit_exit(uint32_t s) { (void)s; }

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

/* ---- the bridge's event stream, captured (non-stub) ---- */
static live_ev_t cap[512];
static int ncap;
static void cap_ev(void *ctx, const live_ev_t *e)
{
    (void)ctx;
    if (ncap < (int)(sizeof cap / sizeof cap[0]))
        cap[ncap++] = *e;
}

int main(void)
{
    int i;
    memset(pats, 0, sizeof pats);

    /* P0: a 909 kick on steps 0 and 4 (accented on 0), a snare on step 8; a 303 A line
     * (accented note, a SLIDE tie, a plain note, a rest) with a +2 transpose; a 303 B note. */
    sm_set(pats[0].drum[0].hit[0], 0);
    sm_set(pats[0].drum[0].hit[0], 4);
    sm_set(pats[0].drum[0].hit[3], 8);
    pats[0].drum[0].accent[0] = 1u << 0;
    pats[0].drum[0].len = 16;

    pats[0].bass[0].len = 8;
    pats[0].bass[0].transpose = 24 + 2;
    pats[0].bass[0].step[0] = (bstep_t){36, (uint8_t)(G_NOTE | BS_ACCENT)};
    pats[0].bass[0].step[1] = (bstep_t){0, (uint8_t)(G_TIE | BS_SLIDE)};   /* the next note slides */
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

    /* P4: a 64-step 909 pattern with a hit in the second word (step 40) — clip C */
    sm_set(pats[4].drum[0].hit[0], 40);               /* word 1, bit 8 */
    pats[4].drum[0].len = 64;

    /* P5..P9: 5 more drum patterns (so 8 non-empty in total; the 7th slot caps the map) */
    for (i = 5; i <= 9; i++) {
        sm_set(pats[i].drum[0].hit[2], i);
        pats[i].drum[0].len = 16;
    }

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

    /* drums: the first 7 non-empty patterns fill all 7 slots, the 8th is dropped; a direct
     * dpart -> drum_steps_t map (including the second 32-step word and a 64-step length) */
    CHECK(count_used(0, ROLE_CLIPS_DRUMS) == 7);
    CHECK(sg.clip[0].used && sg.clip[1].used && sg.clip[2].used && sg.clip[6].used);
    CHECK(sg.clip[0].len == 16 && sg.clip[0].mode == CLIP_LOOP && sg.clip[0].quant == Q_BAR);
    CHECK(drum_get(&sg.clip[0].d.drum, 0, 0) && drum_get(&sg.clip[0].d.drum, 0, 4));
    CHECK(!drum_get(&sg.clip[0].d.drum, 0, 1));
    CHECK(drum_get(&sg.clip[0].d.drum, 3, 8));
    CHECK((sg.clip[0].d.drum.accent[0] & 1u) == 1u);          /* step 0 accented */
    CHECK(drum_get(&sg.clip[1].d.drum, 1, 2) && !drum_get(&sg.clip[1].d.drum, 0, 0));
    CHECK(sg.clip[2].len == 64 && drum_get(&sg.clip[2].d.drum, 0, 40) &&
          !drum_get(&sg.clip[2].d.drum, 0, 39));

    /* bass: P0 -> clip 7 (first non-empty 303 A), P2 -> clip 8; the tie extends the note, the
     * transpose is applied, and the note after a BS_SLIDE step carries the slide in vel bit 7 */
    CHECK(count_used(7, ROLE_CLIPS_BASS) == 2);
    CHECK(sg.clip[7].used && sg.clip[8].used && !sg.clip[9].used);
    CHECK(sg.clip[7].nnotes == 2);
    CHECK(sg.clip[7].d.notes[0].step == 0 && sg.clip[7].d.notes[0].note == 38 /* 36 + 2 */);
    CHECK(sg.clip[7].d.notes[0].vel == 127 && sg.clip[7].d.notes[0].len == 2);   /* accented, tie */
    CHECK(sg.clip[7].d.notes[1].step == 2 && sg.clip[7].d.notes[1].note == 40 /* 38 + 2 */);
    CHECK(sg.clip[7].d.notes[1].vel == (88u | 0x80u));                           /* slid into, not accented */
    CHECK(sg.clip[7].d.notes[1].len == 1);
    CHECK(sg.clip[8].d.notes[0].note == 40 && sg.clip[8].nnotes == 1);

    /* lead: P0's 303 B -> clip 12 */
    CHECK(count_used(12, ROLE_CLIPS_LEAD) == 1);
    CHECK(sg.clip[12].used && sg.clip[12].d.notes[0].note == 48);

    /* T5i: the CHORDS clips are derived from the BASS clips (each note a chord at root+12/+19/+24,
     * vel 90, the same step and length), and the three FX clips are generated */
    CHECK(count_used(19, ROLE_CLIPS_CHORDS) == 2);
    CHECK(sg.clip[19].used && sg.clip[20].used && !sg.clip[21].used);
    CHECK(sg.clip[19].nnotes == 6);
    CHECK(sg.clip[19].d.notes[0].step == 0 && sg.clip[19].d.notes[0].note == 50 &&
          sg.clip[19].d.notes[0].vel == 90 && sg.clip[19].d.notes[0].len == 2);
    CHECK(sg.clip[19].d.notes[1].note == 57 && sg.clip[19].d.notes[2].note == 62);
    CHECK(sg.clip[19].d.notes[3].step == 2 && sg.clip[19].d.notes[3].note == 52 &&
          sg.clip[19].d.notes[3].len == 1);
    CHECK(sg.clip[19].d.notes[4].note == 59 && sg.clip[19].d.notes[5].note == 64);
    CHECK(sg.clip[20].nnotes == 3 && sg.clip[20].d.notes[0].note == 52 &&
          sg.clip[20].d.notes[1].note == 59 && sg.clip[20].d.notes[2].note == 64);
    CHECK(count_used(24, ROLE_CLIPS_FX) == 3);
    CHECK(sg.clip[24].len == 16 && sg.clip[24].nnotes == 4 && sg.clip[24].d.notes[0].step == 2 &&
          sg.clip[24].d.notes[0].note == 60 && sg.clip[24].d.notes[0].vel == 100 &&
          sg.clip[24].d.notes[3].step == 14);
    CHECK(sg.clip[25].len == 16 && sg.clip[25].nnotes == 16 && sg.clip[25].d.notes[0].vel == 20 &&
          sg.clip[25].d.notes[0].step == 0 && sg.clip[25].d.notes[15].vel == 127 &&
          sg.clip[25].d.notes[15].step == 15);
    CHECK(sg.clip[26].len == 32 && sg.clip[26].nnotes == 3 && sg.clip[26].d.notes[0].step == 0 &&
          sg.clip[26].d.notes[1].step == 11 && sg.clip[26].d.notes[2].step == 22 &&
          sg.clip[26].d.notes[0].note == 48 && sg.clip[26].d.notes[0].vel == 80 &&
          sg.clip[26].d.notes[0].len == 4);

    /* every clip's motion lanes are empty (marked MOTION_NONE, not zero) */
    CHECK(sg.clip[0].motion[0].param == PARAM_NONE && sg.clip[0].motion[1].param == PARAM_NONE);
    CHECK(sg.clip[0].motion[0].v[0] == MOTION_NONE && sg.clip[7].motion[1].v[15] == MOTION_NONE);

    /* the key map defaults to identity */
    {
        int id = 1;
        for (i = 0; i < (int)NKEYS; i++)
            if (sg.key_clip[i] != i)
                id = 0;
        CHECK(id);
    }

    /* the bridge's event stream: launch a drum clip and a 303 clip "now" and render; the sink
     * (not a stub) sees the DRUM with its lane and the NOTE_ON/NOTE_OFF of the bass. */
    live_bridge_enter(pats, 1200u);
    live_bridge_transport_start();
    live_bridge_launch_now(0);                        /* drum clip A: a kick on step 0 */
    live_bridge_launch_now(7);                        /* bass clip A: a note on step 0 */
    ncap = 0;
    {
        live_sink_t sink = {cap_ev, 0};
        live_bridge_render(20000u, &sink);            /* ~0.45 s at 44.1 kHz: step 0 and its off */
    }
    {
        int drum = 0, non = 0, noff = 0;
        for (i = 0; i < ncap; i++) {
            if (cap[i].kind == LIVE_EV_DRUM && cap[i].role == LIVE_DRUMS && cap[i].a == 0)
                drum = 1;
            if (cap[i].kind == LIVE_EV_NOTE_ON && cap[i].role == LIVE_BASS && cap[i].a == 38)
                non = 1;
            if (cap[i].kind == LIVE_EV_NOTE_OFF && cap[i].role == LIVE_BASS)
                noff = 1;
        }
        CHECK(drum && non && noff);
    }
    live_bridge_exit();

    if (fails) {
        printf("live_bridge: %d check(s) failed\n", fails);
        return 1;
    }
    printf("live_bridge: mapping and events ok\n");
    return 0;
}
