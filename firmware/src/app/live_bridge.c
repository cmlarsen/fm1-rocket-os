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
static uint8_t g_stop_q[NROLES];                      /* a stop on the role is pending (for the LED) */

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

/* one 303 line as a clip: NOTE steps become notes, a run of TIEs extends the note before them */
static void map_line(const bpart_t *bp, clip_t *c)
{
    int s, nn = 0, tr = (int)bp->transpose - 24;
    int len = bp->len < 1 ? 1 : (bp->len > CLIP_MAX_STEPS ? (int)CLIP_MAX_STEPS : bp->len);
    c->used = 1;
    c->len = (uint8_t)len;
    c->mode = CLIP_LOOP;
    c->quant = Q_BAR;
    for (s = 0; s < len && nn < (int)CLIP_MAX_NOTES; s++) {
        note_t *nt;
        int j, note;
        if (bstep_gate(&bp->step[s]) != G_NOTE)
            continue;
        j = s + 1;
        while (j < len && bstep_gate(&bp->step[j]) == G_TIE)
            j++;
        note = (int)bp->step[s].note + tr;
        note = note < 0 ? 0 : note > 127 ? 127 : note;
        nt = &c->d.notes[nn++];
        nt->step = (uint8_t)s;
        nt->note = (uint8_t)note;
        nt->vel = (bp->step[s].flags & BS_ACCENT) ? 127u : 88u;
        nt->len = (uint8_t)(j - s);
    }
    c->nnotes = (uint8_t)nn;
    if (!nn)
        c->used = 0;
}

void live_bridge_map(const pattern_t *pats, void *songp)
{
    live_song_t *sg = (live_song_t *)songp;
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
    sg->role[ROLE_CHORDS].engine = ENG_FM6;           /* empty until T5i; CHORDS and FX are silent */
    sg->role[ROLE_CHORDS].first_clip = ROLE_CLIPS_DRUMS + ROLE_CLIPS_BASS + ROLE_CLIPS_LEAD;
    sg->role[ROLE_CHORDS].nclips = ROLE_CLIPS_CHORDS;
    sg->role[ROLE_FX].engine = ENG_NOISE;
    sg->role[ROLE_FX].first_clip = ROLE_CLIPS_DRUMS + ROLE_CLIPS_BASS + ROLE_CLIPS_LEAD + ROLE_CLIPS_CHORDS;
    sg->role[ROLE_FX].nclips = ROLE_CLIPS_FX;

    for (k = 0; k < NKEYS; k++)
        sg->key_clip[k] = k;                          /* the default: identity */

    /* the 909: the first 7 non-empty patterns become DRUMS A-G */
    for (i = 0; i < NPAT && nd < (int)ROLE_CLIPS_DRUMS; i++) {
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
    for (i = 0; i < NPAT; i++) {
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

/* ---------------------------------------------------------------- runtime --- */

void live_bridge_init(void)
{
    g_active = 0;
    clip_engine_init(&g_eng, &g_song);
}

static uint16_t clamp_bpm(uint16_t b)
{
    if (b < CLIP_ENGINE_BPM_MIN) return CLIP_ENGINE_BPM_MIN;
    if (b > CLIP_ENGINE_BPM_MAX) return CLIP_ENGINE_BPM_MAX;
    return b;
}

void live_bridge_enter(const pattern_t *pats, uint16_t bpm_x10)
{
    live_bridge_map(pats, &g_song);
    g_song.bpm_x10 = clamp_bpm(bpm_x10 ? bpm_x10 : 1200u);
    g_song.swing = pats[0].swing ? pats[0].swing : 0;  /* the 909's shuffle is the song's */
    clip_engine_init(&g_eng, &g_song);
    g_active = 1;
    {
        int i;
        for (i = 0; i < NROLES; i++)
            g_stop_q[i] = 0;
    }
}

void live_bridge_exit(void)
{
    int i;
    if (!g_active)
        return;
    clip_engine_transport_stop(&g_eng);
    engine_bass_off(0);                                /* release any held 303 A/B note */
    engine_bass_off(1);
    for (i = 0; i < NROLES; i++)
        g_stop_q[i] = 0;
    g_active = 0;
}

int live_bridge_active(void) { return g_active; }

/* ----- audio: clip events -> X0X's engine commands ----- */

static void live_ev(void *ctx, const clip_event_t *e)
{
    (void)ctx;
    switch (e->kind) {
    case CLIPEV_DRUM:
        if (e->role == ROLE_DRUMS) {
            float v = e->b ? 1.0f : (float)seq.accent_q7 / 127.0f;
            engine_drum(0, e->a, v);                       /* kit 0 = 909; lane -> voice; accent -> velocity */
        }
        break;
    case CLIPEV_NOTE_ON:
        if (e->role == ROLE_BASS)
            engine_bass_on(0, e->a, e->b >= 100, 0);       /* 303 A */
        else if (e->role == ROLE_LEAD)
            engine_bass_on(1, e->a, e->b >= 100, 0);       /* 303 B */
        break;                                             /* CHORDS / FX: no sound until T5i */
    case CLIPEV_NOTE_OFF:
        if (e->role == ROLE_BASS)
            engine_bass_off(0);
        else if (e->role == ROLE_LEAD)
            engine_bass_off(1);
        break;
    case CLIPEV_MOTION:
        engine_set((int)(e->b >> 4), (int)(e->b & 15u), (int)e->a, (int)e->c);
        break;
    default:
        break;                                             /* clip started/stopped/queued: not audio */
    }
}

void live_bridge_render(uint32_t nframes)
{
    clip_sink_t sink = {live_ev, 0};
    clip_engine_render(&g_eng, nframes, &sink);
}

/* ----- transport and launching (main loop) ----- */

void live_bridge_transport_start(void) { clip_engine_start(&g_eng); }
void live_bridge_transport_stop(void) { clip_engine_transport_stop(&g_eng); }
int live_bridge_transport_playing(void) { return g_eng.playing != 0; }

void live_bridge_launch(uint8_t clip) { g_stop_q[clip_role(clip) % NROLES] = 0; clip_engine_launch(&g_eng, clip); }
void live_bridge_launch_now(uint8_t clip) { g_stop_q[clip_role(clip) % NROLES] = 0; clip_engine_launch_now(&g_eng, clip); }
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
    }
    /* CHORDS / FX: silent until T5i */
}

void live_bridge_set_bpm(uint16_t bpm_x10) { clip_engine_queue_bpm(&g_eng, bpm_x10); }
uint16_t live_bridge_bpm_x10(void) { return g_eng.bpm_x10; }

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
