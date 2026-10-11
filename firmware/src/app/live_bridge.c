/* SPDX-License-Identifier: GPL-3.0-only */
/* T3 live bridge; see live_bridge.h.
 *
 * This is the one file that sees both worlds. X0X's sequencer.h and core/model/model.h both
 * name a type `song_t`, so when the model comes in we rename it to `live_song_t` for this
 * translation unit (the clip engine's prototypes follow the macro). engine.h comes first, so
 * `song_t` below the `#undef` is X0X's again. */
#include "engine.h"

#define song_t live_song_t
#include "core/seq/clip_engine.h"
#undef song_t

#include "live_bridge.h"

/* The model's `NKEYS` and plat.h's collide (27u vs 27), so the pool attribute is spelled out
 * rather than pulled in through plat.h; engine.c's POOL is the same .pool region. */
#if defined(X0X_HOST)
#define LIVE_POOL
#else
#define LIVE_POOL __attribute__((section(".pool")))
#endif

static live_song_t g_song LIVE_POOL;
static clip_engine_t g_eng LIVE_POOL;
static uint8_t g_active;
static uint8_t g_song_ready;                          /* T6i: g_song holds a loaded song or a mapped project */
static uint8_t g_eng_ready;                           /* T7a: clip_engine_init has been called on g_eng */
static uint32_t g_pat_hash;                           /* the project pattern hash the clips match */
static uint8_t g_stop_q[NROLES];                      /* a stop on the role is pending (for the LED) */

/* T10: the clip engine's step length is fixed at compile time, so it must be built at the same
 * rate as the audio sample counter (sequencer.h's FS). Both are visible here. */
#ifndef CLIP_ENGINE_FS
#define CLIP_ENGINE_FS 48000u
#endif
_Static_assert(CLIP_ENGINE_FS == FS, "CLIP_ENGINE_FS must match the sample counter's FS");

/* ---------------------------------------------------------------- mapping --- */

/* a drum pattern plays if any of its 11 lanes has a hit */
static int drum_used(const dpart_t *d)
{
    int lane;
    for (lane = 0; lane < (int)DRUM_LANES; lane++)
        if (sm_any(d->hit[lane]))
            return 1;
    return 0;
}

/* a 303 line is a clip if it has at least one NOTE (a line of only ties is empty) */
static int bass_used(const bpart_t *b)
{
    int s, n = b->len < 1 ? 1 : (b->len > CLIP_MAX_STEPS ? (int)CLIP_MAX_STEPS : b->len);
    for (s = 0; s < n; s++)
        if (bstep_gate(&b->step[s]) == G_NOTE)
            return 1;
    return 0;
}

/* one 303 line as a clip: NOTE steps become notes, a run of TIEs extends the note before them.
 * A note's vel carries the slide in bit 7 (7 bits of velocity), matching `bass303_note_on`'s
 * slide flag: a note slides when the step before it sounded and was marked BS_SLIDE (X0X's rule). */
static void map_line(const bpart_t *bp, clip_t *c)
{
    int s, nn = 0, tr = (int)bp->transpose - 24;
    int len = bp->len < 1 ? 1 : (bp->len > CLIP_MAX_STEPS ? (int)CLIP_MAX_STEPS : bp->len);
    int prev_sounded = 0, prev_slide = 0;
    c->used = 1;
    c->len = (uint8_t)len;
    c->mode = CLIP_LOOP;
    c->quant = Q_BAR;
    for (s = 0; s < len && nn < (int)CLIP_MAX_NOTES; s++) {
        note_t *nt;
        int j, note, gate = bstep_gate(&bp->step[s]);
        if (gate == G_NOTE) {
            j = s + 1;
            while (j < len && bstep_gate(&bp->step[j]) == G_TIE)
                j++;
            note = (int)bp->step[s].note + tr;
            note = note < 0 ? 0 : note > 127 ? 127 : note;
            nt = &c->d.notes[nn++];
            nt->step = (uint8_t)s;
            nt->note = (uint8_t)note;
            nt->vel = (uint8_t)(((bp->step[s].flags & BS_ACCENT) ? 127u : 88u) |
                                ((prev_sounded && prev_slide) ? 0x80u : 0u));
            nt->len = (uint8_t)(j - s);
        }
        prev_slide = (bp->step[s].flags & BS_SLIDE) != 0;
        prev_sounded = gate != G_REST;
    }
    c->nnotes = (uint8_t)nn;
    if (!nn)
        c->used = 0;
}

#define CHORDS_FIRST (ROLE_CLIPS_DRUMS + ROLE_CLIPS_BASS + ROLE_CLIPS_LEAD)
#define FX_FIRST (CHORDS_FIRST + ROLE_CLIPS_CHORDS)

/* T5i interim content (until T6i/T8's starter songs): the CHORDS clips are derived from the BASS
 * clips, each bass note a chord a fifth and two octaves up at vel 90, and the three FX clips are a
 * generated NOISE pattern: off-beat eighths, a 1-bar riser and a sparse 2-bar texture. */
static void map_chords_fx(live_song_t *sg)
{
    static const int COFF[3] = {12, 19, 24};
    int i, j, k, nn;
    for (i = 0; i < (int)ROLE_CLIPS_CHORDS; i++) {
        const clip_t *b = &sg->clip[ROLE_CLIPS_DRUMS + i];   /* the BASS clips */
        clip_t *c = &sg->clip[CHORDS_FIRST + i];
        if (!b->used)
            continue;
        c->used = 1;
        c->len = b->len;
        c->mode = b->mode;
        c->quant = b->quant;
        nn = 0;
        for (j = 0; j < (int)b->nnotes && nn + 3 <= (int)CLIP_MAX_NOTES; j++)
            for (k = 0; k < 3; k++) {                  /* root + 12, +19, +24 */
                int n = (int)b->d.notes[j].note + COFF[k];
                note_t *nt = &c->d.notes[nn++];
                nt->step = b->d.notes[j].step;
                nt->note = (uint8_t)(n < 0 ? 0 : n > 127 ? 127 : n);
                nt->vel = 90;
                nt->len = b->d.notes[j].len;
            }
        c->nnotes = (uint8_t)nn;
        if (!nn)
            c->used = 0;
    }
    for (i = 0; i < (int)ROLE_CLIPS_FX; i++) {
        clip_t *c = &sg->clip[FX_FIRST + i];
        c->used = 1;
        c->mode = CLIP_LOOP;
        c->quant = Q_BAR;
        nn = 0;
        if (i == 0) {                                  /* off-beat eighths, 1 bar */
            c->len = 16;
            for (j = 2; j < 16; j += 4) {
                note_t *nt = &c->d.notes[nn++];
                nt->step = (uint8_t)j;
                nt->note = 60;
                nt->vel = 100;
                nt->len = 1;
            }
        } else if (i == 1) {                           /* a 1-bar riser: 16 steps, vel 20 -> 127 */
            c->len = 16;
            for (j = 0; j < 16; j++) {
                note_t *nt = &c->d.notes[nn++];
                nt->step = (uint8_t)j;
                nt->note = 60;
                nt->vel = (uint8_t)(20 + 107 * j / 15);
                nt->len = 1;
            }
        } else {                                       /* a sparse 2-bar texture */
            static const uint8_t SP[3] = {0, 11, 22};
            c->len = 32;
            for (j = 0; j < 3; j++) {
                note_t *nt = &c->d.notes[nn++];
                nt->step = SP[j];
                nt->note = 48;
                nt->vel = 80;
                nt->len = 4;
            }
        }
        c->nnotes = (uint8_t)nn;
    }
}

static void map_impl(const pattern_t *pats, int npats, live_song_t *sg)
{
    int i, nd = 0, nb = 0, nl = 0;
    uint8_t k;

    for (i = 0; i < (int)sizeof *sg; i++)
        ((uint8_t *)sg)[i] = 0;
    sg->magic = SONG_MAGIC;
    sg->version = MODEL_VERSION;
    sg->quant_default = Q_BAR;

    sg->role[ROLE_DRUMS].engine = ENG_909;
    sg->role[ROLE_DRUMS].volume = 100;
    sg->role[ROLE_DRUMS].first_clip = 0;
    sg->role[ROLE_DRUMS].nclips = ROLE_CLIPS_DRUMS;
    sg->role[ROLE_BASS].engine = ENG_303;
    sg->role[ROLE_BASS].volume = 100;
    sg->role[ROLE_BASS].first_clip = ROLE_CLIPS_DRUMS;
    sg->role[ROLE_BASS].nclips = ROLE_CLIPS_BASS;
    sg->role[ROLE_LEAD].engine = ENG_303;             /* 303 B plays LEAD in v1 (T3) */
    sg->role[ROLE_LEAD].volume = 100;
    sg->role[ROLE_LEAD].first_clip = ROLE_CLIPS_DRUMS + ROLE_CLIPS_BASS;
    sg->role[ROLE_LEAD].nclips = ROLE_CLIPS_LEAD;
    sg->role[ROLE_CHORDS].engine = ENG_FM6;           /* T5i: FM6 chords derived from the BASS clips */
    sg->role[ROLE_CHORDS].volume = 100;
    sg->role[ROLE_CHORDS].first_clip = ROLE_CLIPS_DRUMS + ROLE_CLIPS_BASS + ROLE_CLIPS_LEAD;
    sg->role[ROLE_CHORDS].nclips = ROLE_CLIPS_CHORDS;
    sg->role[ROLE_FX].engine = ENG_NOISE;             /* T5i: generated NOISE FX patterns */
    sg->role[ROLE_FX].volume = 100;
    sg->role[ROLE_FX].first_clip = ROLE_CLIPS_DRUMS + ROLE_CLIPS_BASS + ROLE_CLIPS_LEAD + ROLE_CLIPS_CHORDS;
    sg->role[ROLE_FX].nclips = ROLE_CLIPS_FX;

    for (k = 0; k < NKEYS; k++)
        sg->key_clip[k] = k;                          /* the default: identity */

    /* the 909: the first 7 non-empty patterns become DRUMS A-G */
    for (i = 0; i < npats && nd < (int)ROLE_CLIPS_DRUMS; i++) {
        const dpart_t *d = &pats[i].drum[0];
        clip_t *c;
        int lane, w, len;
        if (!drum_used(d))
            continue;
        c = &sg->clip[nd];
        len = d->len < 1 ? 1 : (d->len > CLIP_MAX_STEPS ? (int)CLIP_MAX_STEPS : d->len);
        c->used = 1;
        c->len = (uint8_t)len;
        c->mode = CLIP_LOOP;
        c->quant = Q_BAR;
        for (lane = 0; lane < (int)DRUM_LANES; lane++)
            for (w = 0; w < (int)CLIP_SW; w++)
                c->d.drum.hit[lane][w] = d->hit[lane][w];
        for (w = 0; w < (int)CLIP_SW; w++)
            c->d.drum.accent[w] = d->accent[w];
        nd++;
    }

    /* 303 A: the first 5 non-empty patterns become BASS clips; 303 B: the first 7 become LEAD */
    for (i = 0; i < npats; i++) {
        int first = ROLE_CLIPS_DRUMS;
        if (nb < (int)ROLE_CLIPS_BASS && bass_used(&pats[i].bass[0])) {
            map_line(&pats[i].bass[0], &sg->clip[first + nb]);
            nb++;
        }
        if (nl < (int)ROLE_CLIPS_LEAD && bass_used(&pats[i].bass[1])) {
            map_line(&pats[i].bass[1], &sg->clip[first + ROLE_CLIPS_BASS + nl]);
            nl++;
        }
    }

    map_chords_fx(sg);

    /* T3 leaves every clip's motion lanes empty: a zeroed lane would look like a real parameter
     * (param 0) and fire on every step, so mark them MOTION_NONE. */
    for (i = 0; i < (int)NCLIPS; i++) {
        int m, s;
        for (m = 0; m < (int)CLIP_MOTION_LANES; m++) {
            sg->clip[i].motion[m].param = PARAM_NONE;
            for (s = 0; s < (int)CLIP_MAX_STEPS; s++)
                sg->clip[i].motion[m].v[s] = MOTION_NONE;
        }
    }
}

void live_bridge_map(const pattern_t *pats, void *songp) { map_impl(pats, NPAT, (live_song_t *)songp); }
void live_bridge_map_n(const pattern_t *pats, int npats, void *songp)
{
    map_impl(pats, npats, (live_song_t *)songp);
}

/* ---------------------------------------------------------------- runtime --- */

void live_bridge_init(void)
{
    g_active = 1;                                     /* TCLS: the clip engine is the audio source in
                                                       * every view, from power-on; there is no second
                                                       * owner to hand it to. */
    g_song_ready = 0;                                 /* T6i: nothing mapped or loaded yet */
    clip_engine_init(&g_eng, &g_song);
    g_eng_ready = 1;
}

/* T6i: the current song, owned here. song_io reads and edits it and tracks the project pattern hash
 * the clips were derived from, so LIVE rebuilds them when the project changes. */
void *live_bridge_song(void) { return &g_song; }
/* T7a: the song's tempo is authoritative in both modes; X0X's sequencer follows it. */
static void sync_seq_tempo(void) { seq.bpm = (float)g_song.bpm_x10 / 10.0f; }
void live_bridge_song_reload(void)
{
    uint32_t f = engine_crit_enter();                  /* T7b: re-init never tears a render */
    int r;
    clip_engine_init(&g_eng, &g_song);                 /* T10i: init zeroes the engine's mute bits */
    engine_crit_exit(f);
    g_eng_ready = 1;
    for (r = 0; r < NROLES; r++)                        /* a live mute is performance state: not kept */
        g_song.role[r].muted = 0;                       /* in the song, and cleared on load (T10i review) */
    sync_seq_tempo();
}
void live_bridge_sync_tempo(void) { sync_seq_tempo(); }
uint32_t live_bridge_pats_hash(const pattern_t *pats)
{
    const uint8_t *p = (const uint8_t *)pats;
    uint32_t h = 2166136261u, n;
    for (n = 0; n < (uint32_t)sizeof(pattern_t) * NPAT; n++)
        h = (h ^ p[n]) * 16777619u;
    return h;
}
uint32_t live_bridge_song_hash(void) { return g_pat_hash; }

/* Re-derive the clips from the project. The song's own fields (the key map, the LIVE SETUP options,
 * the tempo and the name) are the performer's and survive a rebuild; the clips and roles are the
 * project's. */
static void rebuild_clips(const pattern_t *pats, int npats)
{
    uint8_t keys[NKEYS];
    char name[SONG_NAME_LEN];
    uint16_t bpm = g_song.bpm_x10;
    uint8_t q = g_song.quant_default, lc = g_song.live_channel, pc = g_song.pc_launch;
    uint8_t sl = g_song.stage_lock, ht = g_song.has_take;
    int k;
    for (k = 0; k < (int)NKEYS; k++)
        keys[k] = g_song.key_clip[k];
    for (k = 0; k < (int)SONG_NAME_LEN; k++)
        name[k] = g_song.name[k];
    live_bridge_map_n(pats, npats, &g_song);
    for (k = 0; k < (int)NKEYS; k++)
        g_song.key_clip[k] = keys[k];
    for (k = 0; k < (int)SONG_NAME_LEN; k++)
        g_song.name[k] = name[k];
    g_song.bpm_x10 = bpm;
    g_song.quant_default = q;
    g_song.live_channel = lc;
    g_song.pc_launch = pc;
    g_song.stage_lock = sl;
    g_song.has_take = ht;
    g_song.swing = pats[0].swing ? pats[0].swing : 0;  /* the 909's shuffle is the song's */
}

uint32_t live_bridge_rebuild(const pattern_t *pats)
{
    rebuild_clips(pats, NPAT);
    g_pat_hash = live_bridge_pats_hash(pats);
    g_song_ready = 1;
    return g_pat_hash;
}

/* T10i: rebuild from the first npats patterns (rc_debug's stress content is one pattern, so it
 * does not carry a full NPAT array in the pool). */
uint32_t live_bridge_rebuild_n(const pattern_t *pats, int npats)
{
    rebuild_clips(pats, npats);
    {
        const uint8_t *p = (const uint8_t *)pats;
        uint32_t h = 2166136261u, n;
        for (n = 0; n < (uint32_t)sizeof(pattern_t) * (uint32_t)npats; n++)
            h = (h ^ p[n]) * 16777619u;
        g_pat_hash = h;
    }
    g_song_ready = 1;
    return g_pat_hash;
}

void live_bridge_song_baseline(uint32_t pat_hash)
{
    g_pat_hash = pat_hash;                            /* a stored song's clips match this project */
    g_song_ready = 1;
}

/* TCLS: make the clip engine the audio source (the song is the edited object; there is no X0X
 * pattern-hash remap and no project seed any more — song_io_boot builds the song from
 * song_templates.c). The clip engine is not re-initialised, so clips keep playing across the
 * STUDIO <-> LIVE switch. rc_debug keeps the exit/enter pair for its stress teardown. */
int live_bridge_enter(void)
{
    if (!g_eng_ready)
        live_bridge_song_reload();                     /* a caller (a unit test) that skipped init */
    if (!g_active) {
        g_active = 1;
        engine_all_off();                              /* release every other sound before the clip engine */
    }
    sync_seq_tempo();
    return 0;
}

void live_bridge_exit(void)
{
    int i;
    if (!g_active)
        return;
    clip_engine_transport_stop(&g_eng);
    clip_engine_mute_clear(&g_eng);                    /* T10i: leaving LIVE clears every mute */
    engine_bass_off(0);                                /* release any held 303 A/B note (and log it) */
    engine_bass_off(1);
    engine_all_off();                                  /* T5i: release FM6/NOISE (and stop X0X) too */
    for (i = 0; i < NROLES; i++)
        g_stop_q[i] = 0;
    g_active = 0;
}

int live_bridge_active(void) { return g_active; }

/* ----- audio: the clip engine's events, handed to engine.c in plain terms ----- */

static void bridge_ev(void *ctx, const clip_event_t *e)
{
    const live_sink_t *s = (const live_sink_t *)ctx;
    live_ev_t o;
    o.sample = e->sample;
    o.role = e->role;
    o.a = e->a;
    o.b = e->b;
    o.c = e->c;
    switch (e->kind) {
    case CLIPEV_DRUM:     o.kind = LIVE_EV_DRUM; break;
    case CLIPEV_NOTE_ON:  o.kind = LIVE_EV_NOTE_ON; break;
    case CLIPEV_NOTE_OFF: o.kind = LIVE_EV_NOTE_OFF; break;
    case CLIPEV_MOTION:   o.kind = LIVE_EV_MOTION; break;
    default:              return;                      /* clip started/stopped/queued: not audio */
    }
    if (s && s->ev)
        s->ev(s->ctx, &o);
}

void live_bridge_render(uint32_t nframes, const live_sink_t *sink)
{
    clip_sink_t cs = {bridge_ev, (void *)sink};
    clip_engine_render(&g_eng, nframes, &cs);
}

/* ----- transport and launching (main loop) ----- */

void live_bridge_transport_start(void) { clip_engine_start(&g_eng); }
void live_bridge_transport_restart(void)                 /* T11i: MIDI Start */
{
    uint32_t f = engine_crit_enter();
    clip_engine_restart(&g_eng);                         /* stages next[] under the mask */
    engine_crit_exit(f);
}
void live_bridge_transport_continue(void)                /* T11i: MIDI Continue resumes */
{
    uint32_t f = engine_crit_enter();
    clip_engine_continue(&g_eng);
    engine_crit_exit(f);
}
void live_bridge_transport_stop(void) { clip_engine_transport_stop(&g_eng); }
int live_bridge_transport_playing(void) { return g_eng.playing != 0; }

void live_bridge_launch(uint8_t clip)
{
    uint32_t f = engine_crit_enter();
    g_stop_q[clip_role(clip) % NROLES] = 0;
    clip_engine_launch(&g_eng, clip);                  /* stages next[] under the mask */
    engine_crit_exit(f);
}

/* T11i: Program Change launches a clip at the song's PC quantize (not the clip's own). Kept in its
 * own block; it takes the same critical section as the other launches, because
 * clip_engine_launch_quant also stages next[] from the main loop. */
void live_bridge_launch_quant(uint8_t clip, uint8_t quant)
{
    uint32_t f;
    if (clip >= NCLIPS)
        return;
    f = engine_crit_enter();
    g_stop_q[clip_role(clip) % NROLES] = 0;
    clip_engine_launch_quant(&g_eng, clip, quant);
    engine_crit_exit(f);
}
void live_bridge_launch_now(uint8_t clip)
{
    uint32_t f = engine_crit_enter();
    g_stop_q[clip_role(clip) % NROLES] = 0;
    clip_engine_launch_now(&g_eng, clip);
    engine_crit_exit(f);
}
void live_bridge_launch_at(uint8_t clip, uint32_t at_step)
{
    uint32_t f = engine_crit_enter();
    g_stop_q[clip_role(clip) % NROLES] = 0;
    clip_engine_launch_at(&g_eng, clip, at_step);
    engine_crit_exit(f);
}
void live_bridge_stop(uint8_t role)
{
    if (role >= NROLES)
        return;
    g_stop_q[role] ^= 1;                               /* a second stop cancels, as the engine does */
    clip_engine_stop(&g_eng, role);
}

void live_bridge_play_key(uint8_t role, uint8_t note, int down, uint8_t vel)
{
    if (role == ROLE_DRUMS) {
        if (down)
            engine_drum(0, note % DRUM_LANES, (float)vel / 127.0f);
        return;
    }
    if (role == ROLE_BASS || role == ROLE_LEAD) {
        int part = role == ROLE_BASS ? 0 : 1;
        if (down)
            engine_bass_on(part, note, vel >= 100, 0);
        else
            engine_bass_off(part);
        return;
    }
    if (role == ROLE_CHORDS) {                         /* T5i: FM6 */
        if (down)
            engine_note_on(PART_FM6, note, vel);
        else
            engine_note_off(PART_FM6, note);
    } else if (role == ROLE_FX) {                      /* T5i: NOISE */
        if (down)
            engine_note_on(PART_NOISE, note, vel);
        else
            engine_note_off(PART_NOISE, note);
    }
}

void live_bridge_set_bpm(uint16_t bpm_x10) { clip_engine_queue_bpm(&g_eng, bpm_x10); }
void live_bridge_set_bpm_soft(uint16_t bpm_x10) { clip_engine_queue_bpm_soft(&g_eng, bpm_x10); }   /* T11i: the clock */
uint16_t live_bridge_bpm_x10(void) { return g_eng.bpm_x10; }

/* T7b: the main loop owns the snapshot. Every edit writes the clip into the engine's copies with
 * the audio IRQs masked; a launch stages its copy the same way before the command is posted, so the
 * audio ISR only ever reads play[]/next[], never song->clip[]. */
void live_bridge_clip_changed(uint8_t clip)
{
    uint32_t f = engine_crit_enter();
    clip_engine_clip_changed(&g_eng, clip);
    engine_crit_exit(f);
}
uint32_t live_bridge_global_step(void) { return clip_engine_global_step(&g_eng); }
uint8_t live_bridge_step_phase(void) { return clip_engine_step_phase(&g_eng); }
uint8_t live_bridge_clip_step(uint8_t role) { return clip_engine_clip_step(&g_eng, role); }

/* T10i: the next 1/8 and 1/16 grid distances. The clip engine's step math is
 *     step_sample(k + m) = step_sample + m*step_L + (step_frac + m*step_f) / bpm_x10
 * (exact: the Bresenham remainder is removed then re-added; see clip_engine.c). The 1/16 grid is
 * the next step; the 1/8 grid is the next even step (a bar or a beat always lands on an even 16th,
 * so a swung odd step never moves one). */
static uint32_t grid_at(const clip_engine_t *e, uint32_t m)
{
    return e->step_sample + m * e->step_L + (e->step_frac + m * e->step_f) / e->bpm_x10;
}
void live_bridge_grid(uint32_t *next_8, uint32_t *next_16)
{
    uint32_t now = g_eng.elapsed, d16, d8, m8;
    if (!g_eng.playing || g_eng.bpm_x10 == 0u) {
        if (next_8)
            *next_8 = 0xFFFFFFFFu;
        if (next_16)
            *next_16 = 0xFFFFFFFFu;
        return;
    }
    d16 = grid_at(&g_eng, 1u) - now;
    m8 = ((g_eng.global_step + 1u) & 1u) ? 2u : 1u;   /* the next even step (unswung) */
    d8 = grid_at(&g_eng, m8) - now;
    if (next_8)
        *next_8 = d8;
    if (next_16)
        *next_16 = d16;
}

void live_bridge_mute(uint8_t role, int on)
{
    if (role < NROLES)
        clip_engine_mute(&g_eng, role, on);
}
int live_bridge_muted(uint8_t role) { return clip_engine_muted(&g_eng, role); }
int live_bridge_song_role_muted(uint8_t role)
{
    return role < NROLES ? (int)g_song.role[role].muted : 0;
}
void live_bridge_mute_all(int on)                    /* leaving LIVE clears every mute */
{
    if (on) {
        uint8_t r;
        for (r = 0; r < NROLES; r++)
            clip_engine_mute(&g_eng, r, 1);
    } else {
        clip_engine_mute_clear(&g_eng);              /* synchronous: the engine may stop rendering */
    }
}

/* ----- queries ----- */

uint8_t live_bridge_playing_clip(uint8_t role) { return clip_engine_playing_clip(&g_eng, role); }
uint8_t live_bridge_queued_clip(uint8_t role) { return clip_engine_queued_clip(&g_eng, role); }
uint8_t live_bridge_progress(uint8_t role) { return clip_engine_progress(&g_eng, role); }
void live_bridge_pos(uint8_t *bar, uint8_t *beat, uint8_t *step) { clip_engine_pos(&g_eng, bar, beat, step); }

int live_bridge_stop_queued(int role)
{
    if (role < 0 || role >= NROLES || !g_stop_q[role])
        return 0;
    if (clip_engine_playing_clip(&g_eng, (uint8_t)role) == CLIP_NONE)
        g_stop_q[role] = 0;                           /* the stop landed: no longer pending */
    return g_stop_q[role];
}

/* ----- the role layout and the key map (model.h's, sliced for the UI) ----- */

/* the model's frozen role order must match live_bridge.h's LIVE_* */
_Static_assert(ROLE_DRUMS == LIVE_DRUMS && ROLE_BASS == LIVE_BASS && ROLE_LEAD == LIVE_LEAD &&
               ROLE_CHORDS == LIVE_CHORDS && ROLE_FX == LIVE_FX && NROLES == LIVE_NROLES, "role order");
_Static_assert(NCLIPS == (unsigned)LIVE_NCLIPS && CLIP_NONE == LIVE_CLIP_NONE, "clip constants");
_Static_assert(CLIP_MAX_STEPS == (unsigned)LIVE_MAX_STEPS && STEPS_PER_BAR == (unsigned)LIVE_STEPS_PER_BAR &&
               DRUM_LANES == (unsigned)LIVE_DRUM_LANES && CLIP_MAX_NOTES == (unsigned)LIVE_MAX_NOTES &&
               CLIP_MOTION_LANES == (unsigned)LIVE_MOTION_LANES && PARAM_NONE == LIVE_PARAM_NONE, "editor constants");

int live_bridge_role_first(int role)
{
    return (role >= 0 && role < NROLES) ? (int)g_song.role[role].first_clip : -1;
}
int live_bridge_role_nclips(int role)
{
    return (role >= 0 && role < NROLES) ? (int)g_song.role[role].nclips : 0;
}
int live_bridge_role_of(int clip) { return clip >= 0 && clip < (int)NCLIPS ? (int)clip_role((uint8_t)clip) : -1; }
int live_bridge_clip_used(int clip) { return clip >= 0 && clip < (int)NCLIPS ? g_song.clip[clip].used != 0 : 0; }
int live_bridge_key_clip(int key) { return key >= 0 && key < (int)NKEYS ? (int)g_song.key_clip[key] : -1; }
