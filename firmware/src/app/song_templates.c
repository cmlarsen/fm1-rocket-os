/* SPDX-License-Identifier: GPL-3.0-only */
/* T8: the Empty and Starter songs (see song_templates.h).
 *
 * song_store_empty lays down the valid skeleton (magic, version, five fixed role
 * ranges, the engines, the identity key map); this file sets the name, tempo, key and
 * scale and, for Starter, writes the clips. Every note and hit is written by hand, so
 * the result is deterministic and its golden never moves. */
#include "song_templates.h"
#include "core/model/song_store.h"

/* the 909's voice order, as engine.c's N909 spells it: 0 BD, 1 SD, 2 LT, 3 MT, 4 HT,
 * 5 RS, 6 CP, 7 CH, 8 OH, 9 CR, 10 RD. A drum clip's lane is this index. */
enum { TM_BD = 0, TM_SD = 1, TM_LT = 2, TM_MT = 3, TM_HT = 4, TM_RS = 5,
       TM_CP = 6, TM_CH = 7, TM_OH = 8, TM_CR = 9, TM_RD = 10 };

static const char *const TM_KEYN[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

/* a chord in a progression: a semitone offset from the key root and its quality */
typedef struct {
    int8_t  deg;
    uint8_t minor;
} tmpl_deg_t;

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

uint8_t song_tmpl_key_scale(int key)
{
    return (uint8_t)(song_tmpl_key_minor(key) ? SCALE_MINOR : SCALE_MAJOR);
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

/* a per-step accent (the drum grid's accent row), as X0X's dpart_t carries it */
static void tmpl_acc(clip_t *c, int step)
{
    if (step < 0 || step >= (int)c->len)
        return;
    c->d.drum.accent[step >> 5] |= 1u << (step & 31);
}

/* a triad at `step` (held `hold` steps): root position (inv 0) is root/third/fifth,
 * second inversion (inv 1) is fifth/octave/third-up, for a different voicing */
static void tmpl_chord(clip_t *c, int step, int base, int quality_minor, int inv, int hold)
{
    int third = quality_minor ? 3 : 4;
    if (inv) {
        tmpl_note(c, step, base + 7, 70, hold);
        tmpl_note(c, step, base + 12, 70, hold);
        tmpl_note(c, step, base + third + 12, 70, hold);
    } else {
        tmpl_note(c, step, base, 70, hold);
        tmpl_note(c, step, base + third, 70, hold);
        tmpl_note(c, step, base + 7, 70, hold);
    }
}

/* one chord per bar (16 steps), four bars: len 64, at steps 0/16/32/48 */
static void tmpl_progression(clip_t *c, const tmpl_deg_t *p, int base, int inv)
{
    int bar;
    tmpl_clip(c, 64);
    for (bar = 0; bar < 4; bar++)
        tmpl_chord(c, bar * 16, base + p[bar].deg, p[bar].minor, inv, 16);
}

void song_template_empty(song_t *out, uint16_t bpm_x10, int key)
{
    song_store_empty(out);
    out->bpm_x10 = song_tmpl_bpm(bpm_x10);
    out->key = (uint8_t)song_tmpl_key_root(key);
    out->scale = song_tmpl_key_scale(key);
    tmpl_name(out, "NEW SONG");
    tmpl_no_motion(out);
}

void song_template_starter(song_t *out, uint16_t bpm_x10, int key)
{
    /* real progressions in the chosen key and scale, one chord per bar */
    static const tmpl_deg_t MAJ_A[4] = {{0, 0}, {7, 0}, {9, 1}, {5, 0}};    /* I  V  vi IV  */
    static const tmpl_deg_t MAJ_B[4] = {{9, 1}, {5, 0}, {0, 0}, {7, 0}};    /* vi IV I  V   */
    static const tmpl_deg_t MIN_A[4] = {{0, 1}, {8, 0}, {3, 0}, {10, 0}};   /* i  VI III VII */
    static const tmpl_deg_t MIN_B[4] = {{8, 0}, {10, 0}, {0, 1}, {10, 0}};  /* VI VII i  VII */

    int root = song_tmpl_key_root(key);
    int minor = song_tmpl_key_minor(key);
    int third = minor ? 3 : 4;
    int seventh = minor ? 10 : 11;
    int b = 36 + root;             /* BASS around MIDI 36 (C2) */
    int l = 72 + root;             /* LEAD around MIDI 72 (C5) */
    int c = 60 + root;             /* CHORDS around MIDI 60 (C4) */
    clip_t *cl;

    song_store_empty(out);
    out->bpm_x10 = song_tmpl_bpm(bpm_x10);
    out->key = (uint8_t)root;
    out->scale = song_tmpl_key_scale(key);
    tmpl_name(out, "STARTER");

    /* --- DRUMS A-D: clips 0..3, one bar each, accented, with ghost hats --- */
    cl = &out->clip[0];               /* A: a straight beat */
    tmpl_clip(cl, 16);
    tmpl_hit(cl, TM_BD, 0); tmpl_hit(cl, TM_BD, 8);
    tmpl_hit(cl, TM_SD, 4); tmpl_hit(cl, TM_SD, 12);
    tmpl_hit(cl, TM_CH, 2); tmpl_hit(cl, TM_CH, 6); tmpl_hit(cl, TM_CH, 10); tmpl_hit(cl, TM_CH, 14);
    tmpl_hit(cl, TM_CH, 3); tmpl_hit(cl, TM_CH, 7); tmpl_hit(cl, TM_CH, 11); tmpl_hit(cl, TM_CH, 15);  /* ghost hats */
    tmpl_acc(cl, 0); tmpl_acc(cl, 4); tmpl_acc(cl, 8); tmpl_acc(cl, 12);

    cl = &out->clip[1];               /* B: the same beat, busier, with a clap */
    tmpl_clip(cl, 16);
    tmpl_hit(cl, TM_BD, 0); tmpl_hit(cl, TM_BD, 8);
    tmpl_hit(cl, TM_SD, 4); tmpl_hit(cl, TM_SD, 12);
    tmpl_hit(cl, TM_CP, 12);
    tmpl_hit(cl, TM_CH, 0); tmpl_hit(cl, TM_CH, 2); tmpl_hit(cl, TM_CH, 4); tmpl_hit(cl, TM_CH, 6);
    tmpl_hit(cl, TM_CH, 8); tmpl_hit(cl, TM_CH, 10); tmpl_hit(cl, TM_CH, 12);
    tmpl_hit(cl, TM_CH, 5); tmpl_hit(cl, TM_CH, 9); tmpl_hit(cl, TM_CH, 13);   /* ghost hats */
    tmpl_hit(cl, TM_OH, 14);
    tmpl_acc(cl, 0); tmpl_acc(cl, 4); tmpl_acc(cl, 8); tmpl_acc(cl, 12);

    cl = &out->clip[2];               /* C: hats and a rim */
    tmpl_clip(cl, 16);
    tmpl_hit(cl, TM_BD, 0); tmpl_hit(cl, TM_BD, 8);
    tmpl_hit(cl, TM_RS, 4); tmpl_hit(cl, TM_RS, 12);
    tmpl_hit(cl, TM_CH, 0); tmpl_hit(cl, TM_CH, 2); tmpl_hit(cl, TM_CH, 4); tmpl_hit(cl, TM_CH, 6);
    tmpl_hit(cl, TM_CH, 8); tmpl_hit(cl, TM_CH, 10); tmpl_hit(cl, TM_CH, 12); tmpl_hit(cl, TM_CH, 14);
    tmpl_hit(cl, TM_CH, 1); tmpl_hit(cl, TM_CH, 5); tmpl_hit(cl, TM_CH, 9); tmpl_hit(cl, TM_CH, 13);  /* ghost hats */
    tmpl_acc(cl, 0); tmpl_acc(cl, 8);

    cl = &out->clip[3];               /* D: the fill, its crash on the downbeat */
    tmpl_clip(cl, 16);
    tmpl_hit(cl, TM_CR, 0); tmpl_hit(cl, TM_BD, 0); tmpl_hit(cl, TM_BD, 8);
    tmpl_hit(cl, TM_SD, 4); tmpl_hit(cl, TM_SD, 12);
    tmpl_hit(cl, TM_LT, 13); tmpl_hit(cl, TM_MT, 14); tmpl_hit(cl, TM_HT, 15);
    tmpl_hit(cl, TM_RD, 8);
    tmpl_acc(cl, 0); tmpl_acc(cl, 4); tmpl_acc(cl, 8); tmpl_acc(cl, 12); tmpl_acc(cl, 15);

    /* --- BASS A-C: clips 7..9 --- */
    cl = &out->clip[7];               /* A: root, a fifth, the octave */
    tmpl_clip(cl, 16);
    tmpl_note(cl, 0, b, 110, 4);
    tmpl_note(cl, 4, b, 90, 2);
    tmpl_note(cl, 6, b + 7, 90, 2);
    tmpl_note(cl, 8, b, 110, 4);
    tmpl_note(cl, 12, b + 12, 90, 2);
    tmpl_note(cl, 14, b + third, 90, 2);

    cl = &out->clip[8];               /* B: syncopated */
    tmpl_clip(cl, 16);
    tmpl_note(cl, 0, b, 110, 3);
    tmpl_note(cl, 3, b, 70, 2);
    tmpl_note(cl, 6, b + 12, 90, 2);
    tmpl_note(cl, 9, b + 7, 90, 2);
    tmpl_note(cl, 12, b, 110, 3);
    tmpl_note(cl, 15, b + third, 90, 2);

    cl = &out->clip[9];               /* C: driving eighths */
    tmpl_clip(cl, 16);
    tmpl_note(cl, 0, b, 110, 2);
    tmpl_note(cl, 2, b + 12, 70, 2);
    tmpl_note(cl, 4, b + 7, 90, 2);
    tmpl_note(cl, 8, b, 110, 4);
    tmpl_note(cl, 12, b + seventh, 90, 2);
    tmpl_note(cl, 14, b + 12, 90, 2);

    /* --- LEAD A-C: clips 12..14 --- */
    cl = &out->clip[12];              /* A: a simple call */
    tmpl_clip(cl, 16);
    tmpl_note(cl, 0, l, 100, 4);
    tmpl_note(cl, 4, l + third, 90, 2);
    tmpl_note(cl, 6, l + 7, 90, 2);
    tmpl_note(cl, 8, l + 12, 100, 4);
    tmpl_note(cl, 12, l + 7, 90, 2);
    tmpl_note(cl, 14, l + third, 90, 2);

    cl = &out->clip[13];              /* B: an answer an octave up */
    tmpl_clip(cl, 16);
    tmpl_note(cl, 2, l + 12, 90, 2);
    tmpl_note(cl, 6, l + third + 12, 90, 2);
    tmpl_note(cl, 8, l + 7 + 12, 100, 2);
    tmpl_note(cl, 12, l + 12, 100, 4);

    cl = &out->clip[14];              /* C: held notes */
    tmpl_clip(cl, 16);
    tmpl_note(cl, 0, l, 90, 2);
    tmpl_note(cl, 3, l + third, 90, 2);
    tmpl_note(cl, 4, l + 7, 90, 2);
    tmpl_note(cl, 7, l + 12, 90, 2);
    tmpl_note(cl, 8, l + 7, 100, 4);
    tmpl_note(cl, 14, l, 90, 2);

    /* --- CHORDS A-B: clips 19..20, one chord per bar --- */
    if (minor) {
        tmpl_progression(&out->clip[19], MIN_A, c, 0);
        tmpl_progression(&out->clip[20], MIN_B, c, 1);
    } else {
        tmpl_progression(&out->clip[19], MAJ_A, c, 0);
        tmpl_progression(&out->clip[20], MAJ_B, c, 1);
    }

    /* --- FX A: clip 24 (noise accents) --- */
    cl = &out->clip[24];
    tmpl_clip(cl, 16);
    tmpl_note(cl, 0, l + 12, 100, 2);
    tmpl_note(cl, 8, l + 7, 80, 4);

    tmpl_no_motion(out);
}
