/* SPDX-License-Identifier: GPL-3.0-only */
/* T8: the Empty and Starter songs (see song_templates.h).
 *
 * song_store_empty lays down the valid skeleton (magic, version, five fixed role
 * ranges, the engines, the identity key map); this file only changes the name and
 * tempo and, for Starter, writes the clips. Every note and hit is written by hand, so
 * the result is deterministic and its golden never moves. */
#include "song_templates.h"
#include "core/model/song_store.h"

/* the 909's voice order, as engine.c's N909 spells it: 0 BD, 1 SD, 2 LT, 3 MT, 4 HT,
 * 5 RS, 6 CP, 7 CH, 8 OH, 9 CR, 10 RD. A drum clip's lane is this index. */
enum { TM_BD = 0, TM_SD = 1, TM_LT = 2, TM_MT = 3, TM_HT = 4, TM_RS = 5,
       TM_CP = 6, TM_CH = 7, TM_OH = 8, TM_CR = 9, TM_RD = 10 };

static const char *const TM_KEYN[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

int song_tmpl_key_root(int key)
{
    int k = key % (int)SONG_TMPL_NKEYS;
    if (k < 0)
        k += (int)SONG_TMPL_NKEYS;
    return k % 12;
}

int song_tmpl_key_minor(int key)
{
    int k = key % (int)SONG_TMPL_NKEYS;
    if (k < 0)
        k += (int)SONG_TMPL_NKEYS;
    return k / 12;
}

const char *song_tmpl_key_name(int key)
{
    static char b[8];
    const char *n = TM_KEYN[song_tmpl_key_root(key)];
    int i = 0, j;
    for (j = 0; n[j] && i < 3; j++)
        b[i++] = n[j];
    b[i++] = ' ';
    b[i++] = 'M';
    if (song_tmpl_key_minor(key)) {
        b[i++] = 'I';
        b[i++] = 'N';
    } else {
        b[i++] = 'A';
        b[i++] = 'J';
    }
    b[i] = 0;
    return b;
}

uint16_t song_tmpl_bpm(uint16_t bpm_x10)
{
    if (bpm_x10 < SONG_TMPL_BPM_MIN)
        return SONG_TMPL_BPM_MIN;
    if (bpm_x10 > SONG_TMPL_BPM_MAX)
        return SONG_TMPL_BPM_MAX;
    return bpm_x10;
}

static void tmpl_name(song_t *s, const char *name)
{
    int i = 0;
    while (i < (int)SONG_NAME_LEN - 1 && name[i]) {
        s->name[i] = name[i];
        i++;
    }
    while (i < (int)SONG_NAME_LEN)
        s->name[i++] = 0;
}

/* A zeroed motion lane looks like a real parameter (param 0) and would fire on every
 * step, so a template marks its lanes empty, as live_bridge.c does when it maps. */
static void tmpl_no_motion(song_t *s)
{
    int i, m, st;
    for (i = 0; i < (int)NCLIPS; i++)
        for (m = 0; m < (int)CLIP_MOTION_LANES; m++) {
            s->clip[i].motion[m].param = PARAM_NONE;
            for (st = 0; st < (int)CLIP_MAX_STEPS; st++)
                s->clip[i].motion[m].v[st] = MOTION_NONE;
        }
}

static void tmpl_clip(clip_t *c, int len)
{
    c->used = 1;
    c->len = (uint8_t)len;
    c->mode = CLIP_LOOP;
    c->quant = Q_BAR;
    c->nnotes = 0;
}

static void tmpl_note(clip_t *c, int step, int note, int vel, int len)
{
    note_t *n;
    if (c->nnotes >= CLIP_MAX_NOTES)
        return;
    if (step < 0 || step >= (int)c->len || step >= (int)CLIP_MAX_STEPS)
        return;
    if (note < 0 || note > 127 || vel < 1 || vel > 127 || len < 1 || len > (int)CLIP_MAX_STEPS)
        return;
    n = &c->d.notes[c->nnotes++];
    n->step = (uint8_t)step;
    n->note = (uint8_t)note;
    n->vel = (uint8_t)vel;
    n->len = (uint8_t)len;
}

static void tmpl_hit(clip_t *c, int lane, int step)
{
    if (lane < 0 || lane >= (int)DRUM_LANES)
        return;
    if (step < 0 || step >= (int)c->len)
        return;
    drum_set(&c->d.drum, (unsigned)lane, (unsigned)step, 1);
}

void song_template_empty(song_t *out, uint16_t bpm_x10)
{
    song_t s;
    song_store_empty(&s);
    s.bpm_x10 = song_tmpl_bpm(bpm_x10);
    tmpl_name(&s, "NEW SONG");
    tmpl_no_motion(&s);
    *out = s;
}

void song_template_starter(song_t *out, uint16_t bpm_x10, int key)
{
    song_t s;
    int root, third, fifth, seventh;
    int b, l, c;
    clip_t *cl;

    song_store_empty(&s);
    s.bpm_x10 = song_tmpl_bpm(bpm_x10);
    tmpl_name(&s, "STARTER");

    root = song_tmpl_key_root(key);
    third = song_tmpl_key_minor(key) ? 3 : 4;
    fifth = 7;
    seventh = song_tmpl_key_minor(key) ? 10 : 11;
    b = 36 + root;                 /* BASS around MIDI 36 (C2) */
    l = 72 + root;                 /* LEAD around MIDI 72 (C5) */
    c = 60 + root;                 /* CHORDS around MIDI 60 (C4) */

    /* --- DRUMS A-D: clips 0..3, one bar each --- */
    cl = &s.clip[0];               /* A: a straight beat */
    tmpl_clip(cl, 16);
    tmpl_hit(cl, TM_BD, 0); tmpl_hit(cl, TM_BD, 8);
    tmpl_hit(cl, TM_SD, 4); tmpl_hit(cl, TM_SD, 12);
    tmpl_hit(cl, TM_CH, 2); tmpl_hit(cl, TM_CH, 6); tmpl_hit(cl, TM_CH, 10); tmpl_hit(cl, TM_CH, 14);

    cl = &s.clip[1];               /* B: the same beat, busier, with a clap */
    tmpl_clip(cl, 16);
    tmpl_hit(cl, TM_BD, 0); tmpl_hit(cl, TM_BD, 8);
    tmpl_hit(cl, TM_SD, 4); tmpl_hit(cl, TM_SD, 12);
    tmpl_hit(cl, TM_CP, 12);
    tmpl_hit(cl, TM_CH, 0); tmpl_hit(cl, TM_CH, 2); tmpl_hit(cl, TM_CH, 4); tmpl_hit(cl, TM_CH, 6);
    tmpl_hit(cl, TM_CH, 8); tmpl_hit(cl, TM_CH, 10); tmpl_hit(cl, TM_CH, 12); tmpl_hit(cl, TM_OH, 14);

    cl = &s.clip[2];               /* C: hats and a rim */
    tmpl_clip(cl, 16);
    tmpl_hit(cl, TM_BD, 0); tmpl_hit(cl, TM_BD, 8);
    tmpl_hit(cl, TM_RS, 4); tmpl_hit(cl, TM_RS, 12);
    tmpl_hit(cl, TM_CH, 0); tmpl_hit(cl, TM_CH, 2); tmpl_hit(cl, TM_CH, 4); tmpl_hit(cl, TM_CH, 6);
    tmpl_hit(cl, TM_CH, 8); tmpl_hit(cl, TM_CH, 10); tmpl_hit(cl, TM_CH, 12); tmpl_hit(cl, TM_CH, 14);

    cl = &s.clip[3];               /* D: the fill */
    tmpl_clip(cl, 16);
    tmpl_hit(cl, TM_BD, 0); tmpl_hit(cl, TM_BD, 8);
    tmpl_hit(cl, TM_SD, 4); tmpl_hit(cl, TM_SD, 12);
    tmpl_hit(cl, TM_LT, 13); tmpl_hit(cl, TM_MT, 14); tmpl_hit(cl, TM_HT, 15);
    tmpl_hit(cl, TM_CR, 15); tmpl_hit(cl, TM_RD, 0); tmpl_hit(cl, TM_RD, 8);

    /* --- BASS A-C: clips 7..9 --- */
    cl = &s.clip[7];               /* A: root, a fifth, the octave */
    tmpl_clip(cl, 16);
    tmpl_note(cl, 0, b, 110, 4);
    tmpl_note(cl, 4, b, 90, 2);
    tmpl_note(cl, 6, b + fifth, 90, 2);
    tmpl_note(cl, 8, b, 110, 4);
    tmpl_note(cl, 12, b + 12, 90, 2);
    tmpl_note(cl, 14, b + third, 90, 2);

    cl = &s.clip[8];               /* B: syncopated */
    tmpl_clip(cl, 16);
    tmpl_note(cl, 0, b, 110, 3);
    tmpl_note(cl, 3, b, 70, 2);
    tmpl_note(cl, 6, b + 12, 90, 2);
    tmpl_note(cl, 9, b + fifth, 90, 2);
    tmpl_note(cl, 12, b, 110, 3);
    tmpl_note(cl, 15, b + third, 90, 2);

    cl = &s.clip[9];               /* C: driving eighths */
    tmpl_clip(cl, 16);
    tmpl_note(cl, 0, b, 110, 2);
    tmpl_note(cl, 2, b + 12, 70, 2);
    tmpl_note(cl, 4, b + fifth, 90, 2);
    tmpl_note(cl, 8, b, 110, 4);
    tmpl_note(cl, 12, b + seventh, 90, 2);
    tmpl_note(cl, 14, b + 12, 90, 2);

    /* --- LEAD A-C: clips 12..14 --- */
    cl = &s.clip[12];              /* A: a simple call */
    tmpl_clip(cl, 16);
    tmpl_note(cl, 0, l, 100, 4);
    tmpl_note(cl, 4, l + third, 90, 2);
    tmpl_note(cl, 6, l + fifth, 90, 2);
    tmpl_note(cl, 8, l + 12, 100, 4);
    tmpl_note(cl, 12, l + fifth, 90, 2);
    tmpl_note(cl, 14, l + third, 90, 2);

    cl = &s.clip[13];              /* B: an answer an octave up */
    tmpl_clip(cl, 16);
    tmpl_note(cl, 2, l + 12, 90, 2);
    tmpl_note(cl, 6, l + third + 12, 90, 2);
    tmpl_note(cl, 8, l + fifth + 12, 100, 2);
    tmpl_note(cl, 12, l + 12, 100, 4);

    cl = &s.clip[14];              /* C: held notes */
    tmpl_clip(cl, 16);
    tmpl_note(cl, 0, l, 90, 2);
    tmpl_note(cl, 3, l + third, 90, 2);
    tmpl_note(cl, 4, l + fifth, 90, 2);
    tmpl_note(cl, 7, l + 12, 90, 2);
    tmpl_note(cl, 8, l + fifth, 100, 4);
    tmpl_note(cl, 14, l, 90, 2);

    /* --- CHORDS A-B: clips 19..20 (triads) --- */
    cl = &s.clip[19];              /* A: root position */
    tmpl_clip(cl, 16);
    tmpl_note(cl, 0, c, 80, 8);
    tmpl_note(cl, 0, c + third, 80, 8);
    tmpl_note(cl, 0, c + fifth, 80, 8);
    tmpl_note(cl, 8, c, 80, 8);
    tmpl_note(cl, 8, c + third, 80, 8);
    tmpl_note(cl, 8, c + fifth, 80, 8);

    cl = &s.clip[20];              /* B: second inversion, an octave spread */
    tmpl_clip(cl, 16);
    tmpl_note(cl, 0, c + fifth, 70, 8);
    tmpl_note(cl, 0, c + 12, 70, 8);
    tmpl_note(cl, 0, c + third + 12, 70, 8);
    tmpl_note(cl, 8, c, 70, 8);
    tmpl_note(cl, 8, c + third, 70, 8);
    tmpl_note(cl, 8, c + fifth, 70, 8);

    /* --- FX A: clip 24 (noise accents) --- */
    cl = &s.clip[24];
    tmpl_clip(cl, 16);
    tmpl_note(cl, 0, l + 12, 100, 2);
    tmpl_note(cl, 8, l + fifth, 80, 4);

    tmpl_no_motion(&s);
    *out = s;
}
