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
static uint8_t g_eng_ready;                           /* T7a: clip_engine_init has been called on g_eng */
static uint32_t g_pat_hash;                           /* the project pattern hash the clips match */
static uint8_t g_stop_q[NROLES];                      /* a stop on the role is pending (for the LED) */

/* ---- T9i: the take recorder/player -------------------------------------- */
static take_t g_take LIVE_POOL;                       /* 16,436 B: the song's one take */
static uint8_t g_take_arm_at_start;                   /* REC while stopped: start at the next PLAY */
static uint8_t g_bar_identity;                        /* the arm-to-start bar function (see below) */
static uint32_t g_take_block_end;                     /* the block's end: the T2a tap's sample for
                                                       * a command the ISR applies at the block's
                                                       * first frame (drain logs it there too) */
static uint8_t g_take_live;                           /* T9i: LIVE is active: the take may record */
static volatile uint8_t g_take_end_req;               /* the ISR's natural take end, polled by the main loop */
static uint8_t g_take_reload_req;                     /* an arm was cancelled: reload the song's take (NIT) */
static uint16_t g_take_stage_i[NROLES];               /* T9i r4: the next take event index to stage per role */
#ifdef X0X_HOST
/* T9i r1: the host test traps a main-loop ring push from the audio-ISR side (the take sink). */
volatile int live_bridge_isr;
/* T9i r2: a host test's preemption injector. It is called at race points inside the main-loop
 * take functions (take_play / take_stage) to model the audio ISR firing there; the test's handler
 * runs an ISR block. It skips itself while the host critical section is held (a real ISR cannot
 * preempt a masked section), so the fix is exercised, not the model. Never set on the device. */
void (*live_bridge_test_hook)(void);
/* T9i r2/r3: a test's preemption injector, run just after the take close's critical section (so a
 * real IRQ could run there) to prove the closed take ignores a later ISR feed. Never set on the
 * device. */
void (*take_test_hook)(void);
#endif

/* The main loop's launch / mute / FX intents, stamped by the ISR at the block boundary where
 * the clip engine will process them, so a recorded stream replays sample-exactly. SPSC ring. */
enum { TP_LAUNCH, TP_STOP, TP_LAUNCH_NOW, TP_MUTE, TP_FX, TP_LAUNCH_Q, TP_BPM };
#define TP_RING 32u
static struct { uint8_t kind, a, b; } tp[TP_RING];
static volatile uint8_t tp_h, tp_t;

static void tp_post(uint8_t kind, uint8_t a, uint8_t b)
{
    uint8_t next = (uint8_t)((tp_h + 1u) % TP_RING);
#ifdef X0X_HOST
    if (live_bridge_isr)               /* T9i r1: the ISR-side take sink must never post here */
        __builtin_trap();
#endif
    if (next == tp_t)
        return;                                       /* full: drop (a stray bump) */
    tp[tp_h].kind = kind;
    tp[tp_h].a = a;
    tp[tp_h].b = b;
    __asm__ volatile("" ::: "memory");                /* publish the slot before the head */
    tp_h = next;
}

/* The take's bar grid: the first bar boundary at or after `sample` (absolute samples). While the
 * transport is stopped it is the identity, so an arm stamps its own start (REC before PLAY).
 * T9i/S4: the clip engine's position words are 32-bit and the audio ISR writes them every block, so
 * the read is masked (a few instructions) rather than left torn. Only the main loop calls this
 * (take_arm / take_stop). */
static uint32_t bridge_bar_fn(void *ctx, uint32_t sample)
{
    uint32_t f, m, target, ret;
    (void)ctx;
    f = engine_crit_enter();
    if (g_bar_identity || !g_eng.playing || g_eng.bpm_x10 == 0u) {
        engine_crit_exit(f);
        return sample;
    }
    m = STEPS_PER_BAR - (g_eng.global_step & (STEPS_PER_BAR - 1u));   /* steps to the next bar */
    target = g_eng.step_sample + m * g_eng.step_L +
             (g_eng.step_frac + m * g_eng.step_f) / g_eng.bpm_x10;
    ret = sample + (target - g_eng.elapsed);
    engine_crit_exit(f);
    return ret;
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
    /* T9i: the punch FX state a replay starts from (S2). All six are written: a latched effect as
     * LATCH, a momentary one as ON, the rest as OFF, so the replay reconstructs the exact state
     * (a momentary cannot be held across the arm, but a latched one can). */
    {
        int fx;
        for (fx = 0; fx < 6; fx++) {
            uint32_t how = engine_perform_latched(fx) ? TAKE_FX_LATCH
                         : engine_perform_active(fx) ? TAKE_FX_ON : TAKE_FX_OFF;
            take_setup_event(t, TAKE_FX((uint32_t)fx, how));
        }
    }
    if (!snd)
        return;
    for (tgt = 0; tgt < NTARGETS; tgt++)
        for (v = 0; v < engine_nvoices(tgt) && v < NVOICES_MAX; v++)
            for (i = 0; i < engine_nparams(tgt, v) && i < NPARAMS_MAX; i++)
                take_setup_event(t, TAKE_CMD((uint32_t)tgt, (uint32_t)v, (uint32_t)i, snd->v[tgt][v][i]));
}

/* The playback sink: one recorded command, at its exact window offset. Parameters and notes go
 * straight to the engines (ISR); launches, mutes, FX and tempo drive the clip engine and perform
 * through their ISR-side apply functions. Nothing here pushes onto a main-loop ring or reads
 * song->clip[]: the main loop stages the launch copies in next[] ahead of the event time, and a
 * launch whose copy is not staged is dropped by clip_engine_take_launch (the invariant). */
static void take_sink_ev(void *ctx, uint32_t sample, uint32_t cmd)
{
    uint32_t type = TAKE_CMD_TYPE(cmd), a = TAKE_CMD_A(cmd), b = TAKE_CMD_B(cmd), c = TAKE_CMD_C(cmd);
    (void)ctx;
    (void)sample;                                     /* the tap uses the block end, not the offset */
    if (type < 12u) {
        engine_take_cmd(cmd);
    } else if (type == TAKE_T_LAUNCH) {
        if (b == TAKE_LAUNCH_STOP)
            clip_engine_take_stop(&g_eng, (uint8_t)(c & 0xFFu));
        else if (b == TAKE_LAUNCH_NOW)
            clip_engine_take_launch(&g_eng, (uint8_t)a, 1u, 0u, g_take.play_i);
        else if (b == TAKE_LAUNCH_QUANT)             /* S1: a Program Change launch's quantize */
            clip_engine_take_launch(&g_eng, (uint8_t)a, 0u, (uint8_t)((c >> 8) & 3u), g_take.play_i);
        else
            clip_engine_take_launch(&g_eng, (uint8_t)a, 0u, 0xFFu, g_take.play_i);   /* the staged copy's own quant */
    } else if (type == TAKE_T_MUTE) {
        clip_engine_take_mute(&g_eng, (uint8_t)a, (int)b);
    } else if (type == TAKE_T_FX) {
        engine_take_perform((int)a, (int)b);         /* ISR-side: no ring, no re-record */
    } else if (type == TAKE_T_MARK && a == 2u) {
        clip_engine_take_bpm(&g_eng, (uint16_t)TAKE_MARK_TEMPO_GET(b, c));
    }
    if (engine_tap)
        engine_tap(g_take_block_end, cmd);            /* T2a logs a drained command at the block's
                                                       * end; the replay must report the same sample
                                                       * (the note is applied at the block's first
                                                       * frame either way), or eventscmp sees the
                                                       * recorded and replayed notes one block apart */
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
        g_eng.next_set[r] = 0;                        /* T9i: no stale staged launch survives a reset */
        g_eng.take_mute[r] = 0;
    }
    clip_engine_take_stage_clear(&g_eng);             /* T9i r4: drop every staged take copy */
    for (r = 0; r < (int)NROLES; r++)
        g_take_stage_i[r] = 0;                        /* T9i r4: restage from the first event */
    g_eng.take_mute_set = 0;
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
    g_active = 1;                                     /* TCLS: the clip engine is the audio source in
                                                       * every view, from power-on; there is no second
                                                       * owner to hand it to. */
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
    g_take_live = 0;
    g_take_end_req = 0;
    g_take_reload_req = 0;
    tp_h = tp_t = 0;
    {
        int r;
        for (r = 0; r < (int)NROLES; r++)
            g_take_stage_i[r] = 0;
    }
}

/* T9i: record only user actions, and coalesce parameter changes to the same parameter within one
 * 16th step (the last value wins), so a long knob sweep costs at most one event per step. The
 * clip engine's own notes never reach here: live_apply applies them directly and only taps the
 * host log (engine_tap), so a take holds launches, mutes, punch FX, parameter moves and notes
 * played live; replay regenerates the clip notes from the launches. */
void live_bridge_take_feed(uint32_t sample, uint32_t cmd)
{
    if (!g_take_live)                                 /* B2: never record outside LIVE */
        return;
    if (g_take.state == TAKE_REC && TAKE_CMD_TYPE(cmd) < 8u && g_take.n > 0 &&
        !(g_take.stop_set && (int32_t)(sample - g_take.stop_at) >= 0)) {   /* NIT: never past stop_at */
        uint32_t bpm = g_take.hdr.bpm_x10 ? g_take.hdr.bpm_x10 : 1200u;
        uint32_t step = (CLIP_ENGINE_FS * 150u) / bpm;
        int32_t off = (int32_t)(sample - g_take.start);
        take_event_t *last = &g_take.ev[g_take.n - 1u];
        if (off >= 0 && step &&
            TAKE_CMD_TYPE(last->cmd) == TAKE_CMD_TYPE(cmd) &&
            TAKE_CMD_A(last->cmd) == TAKE_CMD_A(cmd) &&
            TAKE_CMD_B(last->cmd) == TAKE_CMD_B(cmd) &&
            (uint32_t)off / step == (uint32_t)last->sample / step) {
            last->cmd = cmd;                        /* same parameter, same 16th: keep the last value */
            return;
        }
    }
    take_feed(&g_take, sample, cmd);
}

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
        case TP_LAUNCH_Q:   cmd = TAKE_LAUNCH_Q(a, b, clip_role(a)); break;   /* b = quant (S1) */
        case TP_BPM:        cmd = TAKE_MARK_TEMPO((uint16_t)(((uint16_t)a << 8) | b)); break;
        default: break;
        }
        if (cmd)
            take_feed(&g_take, base, cmd);
    }
    take_tick(&g_take, base);
    if (!take_playing(&g_take))
        return;
    g_take_block_end = base + nframes;
    sink.ev = take_sink_ev;
    sink.ctx = 0;
    take_render(&g_take, nframes, &sink);
    if ((int32_t)(g_take.play_pos - g_take.hdr.length_samples) >= 0) {   /* the take ran out */
        /* B1: the natural end must not push onto a main-loop ring. Stop the transport directly
         * (the next clip render releases every role), clear what the take set, and let the main
         * loop notice through g_take_end_req. */
        take_stop_play(&g_take);
        clip_engine_take_bpm_clear(&g_eng);              /* B2: the song's tempo comes back */
        clip_engine_take_stage_clear(&g_eng);            /* T9i r4: drop the staged take copies */
        g_eng.playing = 0;
        clip_engine_take_mute_clear(&g_eng);
        engine_take_perform_reset();
        engine_all_off();
        g_take_end_req = 1;
    }
}

void live_bridge_take_arm(void)
{
    if (!g_take_live)
        return;
    if (take_recording(&g_take) || take_armed(&g_take)) {
        live_bridge_take_stop();                          /* the second tap ends it at the next bar */
        return;
    }
    if (take_playing(&g_take))
        return;
    {
        uint32_t f = engine_crit_enter();
        uint16_t bpm = g_eng.bpm_x10;
        uint8_t playing = g_eng.playing;
        engine_crit_exit(f);
        take_set_bpm(&g_take, bpm);
        if (playing) {
            g_take_arm_at_start = 0;
            take_arm(&g_take, engine_sample_now());       /* the next bar */
        } else {
            g_take_arm_at_start = 1;                      /* wait for PLAY: bar 0 */
        }
    }
}

void live_bridge_take_stop(void)
{
    if (g_take_arm_at_start) {                            /* armed for the next PLAY: cancel it */
        g_take_arm_at_start = 0;
        g_take_reload_req = 1;                            /* NIT: restore the song's take */
        return;
    }
    if (take_recording(&g_take) || take_armed(&g_take)) {
        int was_armed = take_armed(&g_take) && !take_recording(&g_take);
        take_stop(&g_take, engine_sample_now());
        if (was_armed && !take_has(&g_take))
            g_take_reload_req = 1;                        /* NIT: an arm that never started emptied the RAM */
    }
}

/* NIT: the UI polls this after an arm-then-cancel and reloads the song's stored take, so the
 * in-RAM take is not left empty. Clears the request. */
int live_bridge_take_reload_pending(void)
{
    int r = g_take_reload_req != 0;
    g_take_reload_req = 0;
    return r;
}

/* B2: leaving LIVE closes the take at once (the store write is queued by the UI's tick), so the
 * recording is neither lost nor carried into STUDIO. S4: take_finish writes the END mark and the
 * length and moves the take to IDLE; an ISR take_feed in between (take_tick -> take_put) would
 * clobber the END mark or n, so the whole close runs with the audio IRQs masked. */
void live_bridge_take_finish(void)
{
    uint32_t f;
    g_take_arm_at_start = 0;
    f = engine_crit_enter();
    if (take_recording(&g_take) || take_armed(&g_take))
        take_finish(&g_take, engine_sample_now());
    engine_crit_exit(f);
#ifdef X0X_HOST
    if (take_test_hook)                    /* T9i r3/NIT: a test's ISR just after the close window */
        take_test_hook();
#endif
}

#ifdef X0X_HOST
void (*live_bridge_test_scan_hook)(void); /* observe scan work and its interrupt mask */
#endif
/* Immutable take-log search: main loop only, with interrupts enabled. */
static uint16_t take_next_launch(uint8_t role, uint16_t from)
{
    uint16_t i;
    for (i = from; i < g_take.n; i++) {
        uint32_t cmd = g_take.ev[i].cmd;
#ifdef X0X_HOST
        if (live_bridge_test_scan_hook)
            live_bridge_test_scan_hook();
#endif
        if (TAKE_CMD_TYPE(cmd) == TAKE_T_LAUNCH &&
            TAKE_CMD_B(cmd) != TAKE_LAUNCH_STOP &&
            (uint8_t)(TAKE_CMD_C(cmd) & 0xFFu) == role && TAKE_CMD_A(cmd) < NCLIPS)
            break;
    }
    return i;
}

void live_bridge_take_play(void)
{
    uint32_t f;
    uint16_t first[NROLES][2];
    int r, slot;
    if (take_playing(&g_take)) {
        live_bridge_take_stop_play();
        return;
    }
    if (!take_has(&g_take))
        return;
    /* Find the initial FIFO events before masking interrupts. PLAY and the copies must still
     * publish atomically so an offset-0 launch can never replay before its copy is staged. */
    for (r = 0; r < NROLES; r++) {
        first[r][0] = take_next_launch((uint8_t)r, 0u);
        first[r][1] = first[r][0] < g_take.n ?
            take_next_launch((uint8_t)r, (uint16_t)(first[r][0] + 1u)) : g_take.n;
    }
    /* S4/B1: the reset, the PLAY publish and the offset-0 staging are ONE critical section. The
     * prelude writes a TAKE_LAUNCH(now) for every role that is playing, and the first ISR block
     * would fire them with next_set[role] still 0; staging them here, before playing=1, is what
     * keeps the invariant (the ISR can never see PLAY without the matching staged copy). */
    f = engine_crit_enter();
    eng_reset_transport();                                /* bar 0 for the pass */
    take_play(&g_take, engine_sample_now());
    clip_engine_take_bpm_hold(&g_eng);                    /* B2: the take owns the tempo from here */
    for (r = 0; r < NROLES; r++) {
        for (slot = 0; slot < 2; slot++) {
            uint16_t i = first[r][slot];
            g_take_stage_i[r] = i;
            if (i == g_take.n)
                break;
            {
                uint8_t clip = (uint8_t)TAKE_CMD_A(g_take.ev[i].cmd);
                clip_engine_take_stage(&g_eng, (uint8_t)r, clip, &g_song.clip[clip], i);
            }
            g_take_stage_i[r] = (uint16_t)(i + 1u);
        }
    }
    g_eng.playing = 1;
    engine_crit_exit(f);
#ifdef X0X_HOST
    if (live_bridge_test_hook)                            /* T9i r3/NIT: a test's ISR just after the PLAY
                                                           * point, where a real IRQ can run */
        live_bridge_test_hook();
#endif
    engine_all_off();
}

void live_bridge_take_stop_play(void)
{
    take_stop_play(&g_take);
    clip_engine_transport_stop(&g_eng);                   /* main loop: the ring is fine here */
    {
        /* NIT: take_bpm_clear rewrites the transport words the audio ISR reads; mask it. */
        uint32_t f = engine_crit_enter();
        clip_engine_take_bpm_clear(&g_eng);               /* B2: hand the tempo back to the song */
        clip_engine_take_stage_clear(&g_eng);             /* T9i r4: drop the staged take copies */
        engine_crit_exit(f);
    }
    engine_all_off();
    clip_engine_mute_clear(&g_eng);                       /* S3: the take's mutes go ... */
    engine_perform_reset();                               /* ... and its FX (after the stop) */
}

/* T9i r4: the main loop stages the take's launch copies into the clip engine's two-deep FIFO. It
 * fills each role's free slots with the next un-staged launch events (in take order), as far ahead
 * as the FIFO allows, so a launch that follows a queued one within a single UI frame (or across a
 * stalled frame) still has its copy in place when its event replays. A copy is only ever written
 * into an empty slot; the ISR pops slot 0 into play[] at the boundary and shifts slot 1 down. */

void live_bridge_take_stage(void)
{
    int r;
    if (!take_playing(&g_take))
        return;
    for (r = 0; r < NROLES; r++) {
        while (clip_engine_take_stage_free(&g_eng, (uint8_t)r) > 0) {
            uint16_t i = g_take_stage_i[r];
            uint16_t play_i = g_take.play_i;
            uint8_t clip = CLIP_NONE;
            uint32_t f;
            if (i < play_i)
                i = play_i;
            /* The immutable take log is scanned with interrupts enabled. Cache exhaustion so
             * a role with no later launch does not rescan the tail on every UI frame. */
            i = take_next_launch((uint8_t)r, i);
            g_take_stage_i[r] = i;
            if (i == g_take.n)
                break;
            clip = (uint8_t)TAKE_CMD_A(g_take.ev[i].cmd);
            f = engine_crit_enter();
            /* Playback may have advanced during the scan: never publish an already-replayed
             * event. Recheck the play cursor and state while publishing the copy. */
            if (!take_playing(&g_take)) {
                engine_crit_exit(f);
                return;
            }
            if (i < g_take.play_i) {
                g_take_stage_i[r] = g_take.play_i;
                engine_crit_exit(f);
                continue;
            }
            if (!clip_engine_take_stage(&g_eng, (uint8_t)r, clip, &g_song.clip[clip], i)) {
                engine_crit_exit(f);
                break;
            }
            g_take_stage_i[r] = (uint16_t)(i + 1u);
            engine_crit_exit(f);
        }
#ifdef X0X_HOST
        if (live_bridge_test_hook)                       /* T9i r2: a test's ISR between roles */
            live_bridge_test_hook();
#endif
    }
}

/* B2: the take subsystem is LIVE-scoped. Entering LIVE arms it; leaving it finalizes/stops every
 * take action and stops the feed, so nothing records or plays outside LIVE. */
void live_bridge_take_live(int on)
{
    if (on) {
        g_take_live = 1;
        return;
    }
    if (take_recording(&g_take) || take_armed(&g_take))
        live_bridge_take_finish();
    if (take_playing(&g_take))
        live_bridge_take_stop_play();
    g_take_live = 0;
    g_take_arm_at_start = 0;
}

/* B1: the ISR's natural take end signals here; the main loop polls and clears it. */
int live_bridge_take_end_pending(void)
{
    int r = g_take_end_req != 0;
    g_take_end_req = 0;
    return r;
}

void live_bridge_take_clear_ram(void)
{
    int r;
    take_init(&g_take);
    take_set_bar_fn(&g_take, bridge_bar_fn, 0);
    take_set_prelude(&g_take, bridge_prelude, 0);
    g_take_arm_at_start = 0;
    for (r = 0; r < (int)NROLES; r++)
        g_take_stage_i[r] = 0;
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
int live_bridge_take_drops(void) { return clip_engine_take_drops(&g_eng); }

#ifdef X0X_HOST
/* T9i r2: the host tests' window on the hard invariant. For every role, cur[role] must be CLIP_NONE
 * or play[] must be the byte-for-byte copy of song->clip[cur[role]]. 1 = consistent. */
int live_bridge_test_consistent(void)
{
    int r;
    for (r = 0; r < (int)NROLES; r++) {
        uint8_t c = g_eng.cur[r];
        const uint8_t *a, *b;
        uint32_t k;
        if (c == CLIP_NONE)
            continue;
        if (c >= NCLIPS)
            return 0;
        a = (const uint8_t *)&g_eng.play[r];
        b = (const uint8_t *)&g_song.clip[c];
        for (k = 0; k < (uint32_t)sizeof(clip_t); k++)
            if (a[k] != b[k])
                return 0;
    }
    return 1;
}
#endif
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
    return g_pat_hash;
}

void live_bridge_song_baseline(uint32_t pat_hash)
{
    g_pat_hash = pat_hash;                            /* a stored song's clips match this project */
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
    g_take_live = 1;                                   /* T9i: LIVE owns the take again */
    return 0;
}

void live_bridge_exit(void)
{
    int i;
    if (!g_active)
        return;
    live_bridge_take_live(0);                          /* T9i/B2: finalize the take at once, stop playback */
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

static void take_arm_if_pending(void)
{
    if (g_take_arm_at_start) {                        /* T9i: REC while stopped starts at PLAY (bar 0) */
        g_take_arm_at_start = 0;
        g_bar_identity = 1;
        take_arm(&g_take, engine_sample_now());
        g_bar_identity = 0;
    }
}
void live_bridge_transport_start(void)
{
    /* S4: arm and start in one critical section, so the ISR never sees PLAY before the take is
     * armed (and bar 0's prelude is built) — the arm's start and the transport's start are atomic. */
    uint32_t f = engine_crit_enter();
    take_arm_if_pending();
    clip_engine_start(&g_eng);
    engine_crit_exit(f);
}
void live_bridge_transport_restart(void)                 /* T11i: MIDI Start */
{
    uint32_t f;
    if (take_playing(&g_take))
        live_bridge_take_stop_play();                    /* NIT: a Start during playback stops it
                                                          * cleanly first (no transport desync) */
    if (take_recording(&g_take))
        live_bridge_take_finish();                       /* S2: a Start during a recording ends it cleanly */
    take_arm_if_pending();                               /* S2: honour a take armed while stopped */
    f = engine_crit_enter();
    clip_engine_restart(&g_eng);                         /* stages next[] under the mask */
    engine_crit_exit(f);
}
void live_bridge_transport_continue(void)                /* T11i: MIDI Continue resumes */
{
    uint32_t f;
    if (take_playing(&g_take))
        live_bridge_take_stop_play();                    /* NIT: as above */
    if (take_recording(&g_take))
        live_bridge_take_finish();                       /* S2: a Continue during a recording ends it cleanly */
    take_arm_if_pending();                               /* S2: honour a take armed while stopped */
    f = engine_crit_enter();
    clip_engine_continue(&g_eng);
    engine_crit_exit(f);
}
void live_bridge_transport_stop(void)
{
    clip_engine_transport_stop(&g_eng);
    if (take_playing(&g_take))
        live_bridge_take_stop_play();                    /* S7: MIDI Stop stops take playback too */
    else if (take_recording(&g_take) || take_armed(&g_take))
        live_bridge_take_stop();                         /* a hard stop ends the take (NIT: reload on cancel) */
}
int live_bridge_transport_playing(void) { return g_eng.playing != 0; }

void live_bridge_launch(uint8_t clip)
{
    uint32_t f;
    uint8_t q;
    if (clip >= NCLIPS)
        return;
    q = (uint8_t)(g_song.clip[clip].quant & 3u);       /* the clip's own quantize, recorded in the
                                                        * event (r2/S3): the replay never reads it
                                                        * back from a since-overwritten staged copy */
    f = engine_crit_enter();
    g_stop_q[clip_role(clip) % NROLES] = 0;
    clip_engine_launch(&g_eng, clip);                  /* stages next[] under the mask */
    engine_crit_exit(f);
    tp_post(TP_LAUNCH_Q, clip, q);                        /* T9i: record it from the ISR */
}

/* T11i: Program Change launches a clip at the song's PC quantize (not the clip's own). Kept in its
 * own block; it takes the same critical section as the other launches, because
 * clip_engine_launch_quant also stages next[] from the main loop. */
void live_bridge_launch_quant(uint8_t clip, uint8_t quant)
{
    uint32_t f;
    if (clip >= NCLIPS)
        return;
    if (take_playing(&g_take))
        return;                                       /* S7: a Program Change does not fight a take */
    f = engine_crit_enter();
    g_stop_q[clip_role(clip) % NROLES] = 0;
    clip_engine_launch_quant(&g_eng, clip, quant);
    engine_crit_exit(f);
    tp_post(TP_LAUNCH_Q, clip, (uint8_t)(quant & 3u));    /* S1: remember the quantize for replay */
}
void live_bridge_launch_now(uint8_t clip)
{
    uint32_t f = engine_crit_enter();
    g_stop_q[clip_role(clip) % NROLES] = 0;
    clip_engine_launch_now(&g_eng, clip);
    engine_crit_exit(f);
    tp_post(TP_LAUNCH_NOW, clip, (uint8_t)clip_role(clip));
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

void live_bridge_set_bpm(uint16_t bpm_x10)
{
    clip_engine_queue_bpm(&g_eng, bpm_x10);
    tp_post(TP_BPM, (uint8_t)(bpm_x10 >> 8), (uint8_t)bpm_x10);   /* S2: record the tempo change */
}
void live_bridge_set_bpm_soft(uint16_t bpm_x10)   /* T11i: the clock */
{
    clip_engine_queue_bpm_soft(&g_eng, bpm_x10);
    tp_post(TP_BPM, (uint8_t)(bpm_x10 >> 8), (uint8_t)bpm_x10);   /* S2: and the followed clock's */
}
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
