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
#include "core/seq/take.h"                 /* T9i: take_t (model.h's song_t is already renamed) */
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

/* ---- T9i: the take recorder/player -------------------------------------- */
static take_t g_take LIVE_POOL;                       /* 16,436 B: the song's one take */
static uint8_t g_take_arm_at_start;                   /* REC while stopped: start at the next PLAY */
static uint8_t g_bar_identity;                        /* the arm-to-start bar function (see below) */
static uint32_t g_take_block_base;                    /* the block base the sink's offsets add to */

/* The main loop's launch / mute / FX intents, stamped by the ISR at the block boundary where
 * the clip engine will process them, so a recorded stream replays sample-exactly. SPSC ring. */
enum { TP_LAUNCH, TP_STOP, TP_LAUNCH_NOW, TP_MUTE, TP_FX };
#define TP_RING 32u
static struct { uint8_t kind, a, b; } tp[TP_RING];
static volatile uint8_t tp_h, tp_t;

static void tp_post(uint8_t kind, uint8_t a, uint8_t b)
{
    uint8_t next = (uint8_t)((tp_h + 1u) % TP_RING);
    if (next == tp_t)
        return;                                       /* full: drop (a stray bump) */
    tp[tp_h].kind = kind;
    tp[tp_h].a = a;
    tp[tp_h].b = b;
    __asm__ volatile("" ::: "memory");                /* publish the slot before the head */
    tp_h = next;
}

/* The take's bar grid: the first bar boundary at or after `sample` (absolute samples). While the
 * transport is stopped it is the identity, so an arm stamps its own start (REC before PLAY). */
static uint32_t bridge_bar_fn(void *ctx, uint32_t sample)
{
    uint32_t m, target;
    (void)ctx;
    if (g_bar_identity || !g_eng.playing || g_eng.bpm_x10 == 0u)
        return sample;
    m = STEPS_PER_BAR - (g_eng.global_step & (STEPS_PER_BAR - 1u));   /* steps to the next bar */
    target = g_eng.step_sample + m * g_eng.step_L +
             (g_eng.step_frac + m * g_eng.step_f) / g_eng.bpm_x10;
    return sample + (target - g_eng.elapsed);
}

/* The prelude: the state a replay starts from. The playing clip per role (or a stop), the mutes,
 * the tempo and every parameter's current value (the engine's sound mirror). */
static void bridge_prelude(void *ctx, struct take *t)
{
    const sound_t *snd = engine_sound_mirror();
    int r, tgt, v, i;
    (void)ctx;
    for (r = 0; r < (int)NROLES; r++) {
        uint8_t c = clip_engine_playing_clip(&g_eng, (uint8_t)r);
        if (c == CLIP_NONE)
            take_setup_event(t, TAKE_LAUNCH(0u, TAKE_LAUNCH_STOP, (uint8_t)r));
        else
            take_setup_event(t, TAKE_LAUNCH(c, TAKE_LAUNCH_NOW, (uint8_t)r));
        take_setup_event(t, TAKE_MUTE((uint8_t)r, clip_engine_muted(&g_eng, (uint8_t)r) ? 1u : 0u));
    }
    take_setup_event(t, TAKE_MARK_TEMPO(g_eng.bpm_x10));
    if (!snd)
        return;
    for (tgt = 0; tgt < NTARGETS; tgt++)
        for (v = 0; v < engine_nvoices(tgt) && v < NVOICES_MAX; v++)
            for (i = 0; i < engine_nparams(tgt, v) && i < NPARAMS_MAX; i++)
                take_setup_event(t, TAKE_CMD((uint32_t)tgt, (uint32_t)v, (uint32_t)i, snd->v[tgt][v][i]));
}

/* The playback sink: one recorded command, at its exact window offset. Parameters and notes go
 * straight to the engines (ISR); launches, mutes, FX and tempo drive the clip engine and perform. */
static void take_sink_ev(void *ctx, uint32_t sample, uint32_t cmd)
{
    uint32_t type = TAKE_CMD_TYPE(cmd), a = TAKE_CMD_A(cmd), b = TAKE_CMD_B(cmd), c = TAKE_CMD_C(cmd);
    (void)ctx;
    if (type < 12u) {
        engine_take_cmd(cmd);
    } else if (type == TAKE_T_LAUNCH) {
        if (b == TAKE_LAUNCH_STOP)
            live_bridge_stop((uint8_t)c);
        else if (b == TAKE_LAUNCH_NOW)
            live_bridge_launch_now((uint8_t)a);
        else
            live_bridge_launch((uint8_t)a);
    } else if (type == TAKE_T_MUTE) {
        live_bridge_mute((uint8_t)a, (int)b);
    } else if (type == TAKE_T_FX) {
        if (b == TAKE_FX_OFF)
            engine_perform_release((int)a);
        else
            engine_perform_engage((int)a, b == TAKE_FX_LATCH);
    } else if (type == TAKE_T_MARK && a == 2u) {
        live_bridge_set_bpm((uint16_t)TAKE_MARK_TEMPO_GET(b, c));
    }
    if (engine_tap)
        engine_tap(g_take_block_base + sample, cmd);
}

/* Reset the clip transport to bar 0 before a playback pass, without a render in between. */
static void eng_reset_transport(void)
{
    int r, i;
    clip_engine_transport_stop(&g_eng);
    g_eng.playing = 0;
    g_eng.was_playing = 0;
    g_eng.global_step = 0;
    g_eng.step_sample = 0;
    g_eng.step_frac = 0;
    g_eng.elapsed = 0;
    for (r = 0; r < (int)NROLES; r++) {
        g_eng.cur[r] = CLIP_NONE;
        g_eng.pos[r] = 0;
        g_eng.que[r] = CLIP_NONE;
        g_eng.que_set[r] = 0;
        g_eng.muted[r] = 0;
    }
    for (i = 0; i < (int)CLIP_ENGINE_MAX_VOICES; i++)
        g_eng.note[i].used = 0;
    g_eng.cmd_tail = g_eng.cmd_head;                  /* drop any stale UI ring entry */
}

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
    g_active = 1;                                     /* T7a: the clip engine is the audio source in
                                                       * every normal view, from power-on. Classic
                                                       * mode (live_bridge_exit) hands it to X0X. */
    g_song_ready = 0;                                 /* T6i: nothing mapped or loaded yet */
    clip_engine_init(&g_eng, &g_song);
    g_eng_ready = 1;
    take_init(&g_take);                               /* T9i: the song's take */
    take_set_bar_fn(&g_take, bridge_bar_fn, 0);
    take_set_prelude(&g_take, bridge_prelude, 0);
}

/* T9i: the take recorder/player's public surface. */
void live_bridge_take_init(void)
{
    take_init(&g_take);
    take_set_bar_fn(&g_take, bridge_bar_fn, 0);
    take_set_prelude(&g_take, bridge_prelude, 0);
    g_take_arm_at_start = 0;
    tp_h = tp_t = 0;
}

void live_bridge_take_feed(uint32_t sample, uint32_t cmd) { take_feed(&g_take, sample, cmd); }

void live_bridge_take_fx(int id, int how)
{
    tp_post(TP_FX, (uint8_t)(id & 0xFF), (uint8_t)(how & 0xFF));
}

/* The ISR pump, once per audio block: flush the post ring, advance the arm/stop state machine and
 * replay the take. Runs before the clip engine renders so a replayed launch lands in this window. */
void live_bridge_take_block(uint32_t base, uint32_t nframes)
{
    take_sink_t sink;
    while (tp_t != tp_h) {
        uint8_t kind = tp[tp_t].kind, a = tp[tp_t].a, b = tp[tp_t].b;
        uint32_t cmd = 0;
        tp_t = (uint8_t)((tp_t + 1u) % TP_RING);
        switch (kind) {
        case TP_LAUNCH:     cmd = TAKE_LAUNCH(a, TAKE_LAUNCH_QUEUE, b); break;
        case TP_LAUNCH_NOW: cmd = TAKE_LAUNCH(a, TAKE_LAUNCH_NOW, b); break;
        case TP_STOP:       cmd = TAKE_LAUNCH(0u, TAKE_LAUNCH_STOP, a); break;
        case TP_MUTE:       cmd = TAKE_MUTE(a, b); break;
        case TP_FX:         cmd = TAKE_FX(a, b); break;
        default: break;
        }
        if (cmd)
            take_feed(&g_take, base, cmd);
    }
    take_tick(&g_take, base);
    if (!take_playing(&g_take))
        return;
    g_take_block_base = base;
    sink.ev = take_sink_ev;
    sink.ctx = 0;
    take_render(&g_take, nframes, &sink);
    if ((int32_t)(g_take.play_pos - g_take.hdr.length_samples) >= 0) {   /* the take ran out */
        take_stop_play(&g_take);
        clip_engine_transport_stop(&g_eng);
        engine_all_off();
    }
}

void live_bridge_take_arm(void)
{
    if (take_recording(&g_take) || take_armed(&g_take)) {
        take_stop(&g_take, engine_sample_now());          /* the second tap ends it at the next bar */
        return;
    }
    if (take_playing(&g_take))
        return;
    take_set_bpm(&g_take, g_eng.bpm_x10);
    if (g_eng.playing) {
        g_take_arm_at_start = 0;
        take_arm(&g_take, engine_sample_now());           /* the next bar */
    } else {
        g_take_arm_at_start = 1;                          /* wait for PLAY: bar 0 */
    }
}

void live_bridge_take_stop(void)
{
    if (g_take_arm_at_start) {                            /* armed for the next PLAY: cancel it */
        g_take_arm_at_start = 0;
        return;
    }
    if (take_recording(&g_take) || take_armed(&g_take))
        take_stop(&g_take, engine_sample_now());
}

void live_bridge_take_play(void)
{
    if (take_playing(&g_take)) {
        live_bridge_take_stop_play();
        return;
    }
    if (!take_has(&g_take))
        return;
    eng_reset_transport();                                /* bar 0 for the pass */
    engine_all_off();
    take_play(&g_take, engine_sample_now());
    g_eng.playing = 1;
}

void live_bridge_take_stop_play(void)
{
    take_stop_play(&g_take);
    clip_engine_transport_stop(&g_eng);
    engine_all_off();
}

void live_bridge_take_clear_ram(void)
{
    take_init(&g_take);
    take_set_bar_fn(&g_take, bridge_bar_fn, 0);
    take_set_prelude(&g_take, bridge_prelude, 0);
    g_take_arm_at_start = 0;
}

void live_bridge_take_loaded(int nevents, uint32_t length_samples, uint16_t bpm_x10)
{
    if (nevents < 0)
        nevents = 0;
    if (nevents > (int)TAKE_MAX_EVENTS)
        nevents = (int)TAKE_MAX_EVENTS;
    g_take.n = (uint16_t)nevents;
    g_take.hdr.nevents = (uint16_t)nevents;
    g_take.hdr.length_samples = length_samples;
    g_take.hdr.bpm_x10 = bpm_x10;
    g_take.state = TAKE_IDLE;
    g_take.full = 0;
    g_take.ended = 1;                                     /* the log is a stored one, already ended */
}

int live_bridge_take_state(void) { return take_state(&g_take); }
int live_bridge_take_recording(void) { return take_recording(&g_take); }
int live_bridge_take_armed(void) { return take_armed(&g_take) || g_take_arm_at_start; }
int live_bridge_take_playing(void) { return take_playing(&g_take); }
int live_bridge_take_full(void) { return take_full(&g_take); }
int live_bridge_take_has(void) { return take_has(&g_take); }
int live_bridge_take_nevents(void) { return (int)g_take.n; }
uint16_t live_bridge_take_bpm_x10(void) { return g_take.hdr.bpm_x10 ? g_take.hdr.bpm_x10 : g_eng.bpm_x10; }
uint32_t live_bridge_take_length_samples(void) { return g_take.hdr.length_samples; }
int live_bridge_take_bars(void)
{
    uint32_t per_bar = (CLIP_ENGINE_FS * 150u * STEPS_PER_BAR) / (g_take.hdr.bpm_x10 ? g_take.hdr.bpm_x10 : 1200u);
    return per_bar ? (int)(g_take.hdr.length_samples / per_bar) : 0;
}
static uint32_t take_per_bar(void)
{
    return (CLIP_ENGINE_FS * 150u * STEPS_PER_BAR) /
           (g_take.hdr.bpm_x10 ? g_take.hdr.bpm_x10 : 1200u);
}
int live_bridge_take_rec_elapsed_ms(void)
{
    uint32_t d;
    if (!take_recording(&g_take))
        return 0;
    d = engine_sample_now() - g_take.start;
    return (int)(d / (CLIP_ENGINE_FS / 1000u));           /* 32-bit: ~1/44 sample -> ms */
}
int live_bridge_take_rec_bars(void)
{
    uint32_t d, per_bar;
    if (!take_recording(&g_take))
        return 0;
    d = engine_sample_now() - g_take.start;
    per_bar = take_per_bar();
    return per_bar ? (int)(d / per_bar) : 0;
}

const void *live_bridge_take_hdr(void) { return &g_take.hdr; }
const void *live_bridge_take_events(void) { return g_take.ev; }
void *live_bridge_take_hdr_mut(void) { return &g_take.hdr; }
void *live_bridge_take_events_mut(void) { return g_take.ev; }

/* T6i: the current song, owned here. song_io reads and edits it and tracks the project pattern hash
 * the clips were derived from, so LIVE rebuilds them when the project changes. */
void *live_bridge_song(void) { return &g_song; }
/* T7a: the song's tempo is authoritative in both modes; X0X's sequencer follows it. */
static void sync_seq_tempo(void) { seq.bpm = (float)g_song.bpm_x10 / 10.0f; }
void live_bridge_song_reload(void)
{
    int r;
    clip_engine_init(&g_eng, &g_song);                 /* T10i: init zeroes the engine's mute bits */
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

/* T7a: hand the audio to the clip engine (the song is the edited object; X0X's pattern-hash remap
 * is gone). The song's own tempo is kept — the argument is the project's tempo, which the song
 * overrides (decision 4). Only the first call with nothing built seeds from the project. The clip
 * engine is not re-initialised, so clips keep playing across the STUDIO <-> LIVE switch. */
int live_bridge_enter(const pattern_t *pats, uint16_t bpm_x10)
{
    (void)bpm_x10;
    if (!g_song_ready)
        live_bridge_rebuild(pats);                     /* one-time seed, when no stored song existed */
    if (!g_eng_ready)
        live_bridge_song_reload();                     /* a caller (a unit test) that skipped init */
    if (!g_active) {
        g_active = 1;
        engine_all_off();                              /* stop X0X's sequencer and release its voices */
    }
    sync_seq_tempo();
    return 0;
}

void live_bridge_exit(void)
{
    int i;
    if (!g_active)
        return;
    if (take_recording(&g_take) || take_armed(&g_take)) {   /* T9i: close the take as we leave */
        take_stop(&g_take, engine_sample_now());
        take_tick(&g_take, engine_sample_now() + 1u);
    }
    if (take_playing(&g_take))
        take_stop_play(&g_take);
    g_take_arm_at_start = 0;
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

void live_bridge_transport_start(void)
{
    if (g_take_arm_at_start) {                        /* T9i: REC while stopped starts at PLAY (bar 0) */
        g_take_arm_at_start = 0;
        g_bar_identity = 1;
        take_arm(&g_take, engine_sample_now());
        g_bar_identity = 0;
    }
    clip_engine_start(&g_eng);
}
void live_bridge_transport_stop(void)
{
    clip_engine_transport_stop(&g_eng);
    if (take_recording(&g_take) || take_armed(&g_take))
        take_stop(&g_take, engine_sample_now());      /* a hard stop ends the take at the next bar */
}
int live_bridge_transport_playing(void) { return g_eng.playing != 0; }

void live_bridge_launch(uint8_t clip)
{
    g_stop_q[clip_role(clip) % NROLES] = 0;
    clip_engine_launch(&g_eng, clip);
    tp_post(TP_LAUNCH, clip, (uint8_t)clip_role(clip));   /* T9i: record it from the ISR */
}
void live_bridge_launch_now(uint8_t clip)
{
    g_stop_q[clip_role(clip) % NROLES] = 0;
    clip_engine_launch_now(&g_eng, clip);
    tp_post(TP_LAUNCH_NOW, clip, (uint8_t)clip_role(clip));
}
void live_bridge_stop(uint8_t role)
{
    if (role >= NROLES)
        return;
    g_stop_q[role] ^= 1;                               /* a second stop cancels, as the engine does */
    clip_engine_stop(&g_eng, role);
    tp_post(TP_STOP, role, 0);
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
uint16_t live_bridge_bpm_x10(void) { return g_eng.bpm_x10; }

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
    if (role < NROLES) {
        clip_engine_mute(&g_eng, role, on);
        tp_post(TP_MUTE, role, (uint8_t)(on ? 1 : 0));    /* T9i */
    }
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
