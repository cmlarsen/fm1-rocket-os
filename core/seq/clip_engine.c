/* SPDX-License-Identifier: GPL-3.0-only */
/* T4c clip engine; see clip_engine.h for the model.
 *
 * Timing, exactly and with no 64-bit division. A 16th lasts
 *     STEP_NUM / bpm_x10   samples,   STEP_NUM = FS * 150   (= FS * 60 / 4 * 10)
 * (FS * 60 / (bpm*4) with bpm = bpm_x10/10). STEP_NUM is 7,200,000 at 48 kHz, so the divide
 * is 32-bit. Its integer part L = STEP_NUM / bpm_x10 and remainder f = STEP_NUM % bpm_x10
 * drive an exact Bresenham accumulator in units of 1/bpm_x10 sample:
 *
 *     step_sample(k) = floor(k * STEP_NUM / bpm_x10)      (never drifts; proven by induction)
 *
 * so bar N starts on exactly the sample that multiplication says, hour after hour, and the
 * fractional part stays under bpm_x10 (a 32-bit divide). A tempo change keeps the current
 * step's phase (the elapsed fraction of the step is rescaled to the new step length), so no
 * step fires twice, however big the jump. Swing is X0X's: an odd 16th is delayed by
 * L * swing / 200 samples (100 = the next even 16th shifts by half a step = 75 %). A bar or
 * beat lands on an even 16th, so quantize points are never swung.
 *
 * A clip's kind is decided by its role: ROLE_DRUMS uses d.drum, everything else uses
 * d.notes[0..nnotes). An unused clip cannot be launched.
 *
 * The render loop walks one step at a time and emits each event at its exact sample offset.
 * At the head of a step it applies any queued launch whose target step has arrived (releasing
 * the previous clip's notes), the roles advance and fire, and note-offs run on the sample
 * counter: a note still sounding when a clip stops, switches or loops is released there.
 */
#include "clip_engine.h"

#ifndef CLIP_ENGINE_FS
#define CLIP_ENGINE_FS 48000u
#endif
#define CLIP_STEP_NUM ((uint32_t)CLIP_ENGINE_FS * 150u)
#define NEVER32 0xFFFFFFFFu
/* publish we have written the slot / read the head before the slot */
#define CLIP_BARRIER() __asm__ volatile("" ::: "memory")

#ifdef X0X_HOST
/* T9i r1: the host test traps a command-ring push from the audio-ISR side (the take player). Set by
 * the test around the ISR-side call; never set on the device (the guard compiles out). */
volatile int clip_engine_isr;
#endif

/* ---- tempo --------------------------------------------------------------- */

static uint16_t clamp_bpm(uint16_t b)
{
    if (b < CLIP_ENGINE_BPM_MIN) return CLIP_ENGINE_BPM_MIN;
    if (b > CLIP_ENGINE_BPM_MAX) return CLIP_ENGINE_BPM_MAX;
    return b;
}

/* Change the tempo, keeping the elapsed fraction of the current step (ISR context only: the
 * UI sends a tempo through clip_engine_queue_bpm). */
static void set_bpm(clip_engine_t *e, uint16_t bpm_x10)
{
    uint16_t b = clamp_bpm(bpm_x10);
    uint32_t new_l = CLIP_STEP_NUM / b;
    uint32_t new_f = CLIP_STEP_NUM % b;
    if (e->playing && e->step_L && new_l) {
        uint32_t phase = e->elapsed - e->step_sample;   /* samples into the current step */
        uint32_t scaled;
        if ((int32_t)phase < 0)
            phase = 0;
        if (phase > e->step_L)
            phase = e->step_L;
        /* keep the same fraction of the step, so the next boundary is exactly one step away */
        scaled = (uint32_t)((uint64_t)phase * new_l / e->step_L);
        e->step_sample = e->elapsed - scaled;
        e->step_frac = 0;
    } else {
        e->step_frac = 0;
    }
    e->bpm_x10 = b;
    e->bpm_src = bpm_x10;
    e->step_L = new_l;
    e->step_f = new_f;
}

/* swing shift of step k, in samples */
static uint32_t swing_of(const clip_engine_t *e, uint32_t k)
{
    uint32_t sw = e->song ? e->song->swing : 0;
    if (!(k & 1u))
        return 0;
    if (sw > 100u) sw = 100u;
    return (e->step_L * sw) / 200u;
}

/* absolute sample the next step begins on (unswung) */
static uint32_t next_step_sample(const clip_engine_t *e)
{
    return e->step_sample + e->step_L + (e->step_frac + e->step_f) / e->bpm_x10;
}

static uint32_t next_step_time(const clip_engine_t *e)
{
    return next_step_sample(e) + swing_of(e, e->global_step + 1u);
}

static void step_advance(clip_engine_t *e)
{
    uint32_t carry = e->step_frac + e->step_f;
    e->step_sample += e->step_L + carry / e->bpm_x10;
    e->step_frac = carry % e->bpm_x10;
    e->global_step++;
}

/* ---- clips --------------------------------------------------------------- */

static int clip_len_raw(const clip_t *c)
{
    int n = c->len ? (int)c->len : 1;
    return n > (int)CLIP_MAX_STEPS ? (int)CLIP_MAX_STEPS : n;
}

/* T7b: the length of a role's playing copy (the ISR reads play[], never song->clip[]). */
static int play_len(const clip_engine_t *e, uint8_t role)
{
    return clip_len_raw(&e->play[role]);
}

static int clip_notes(const clip_t *c)
{
    int n = (int)c->nnotes;
    return n > (int)CLIP_MAX_NOTES ? (int)CLIP_MAX_NOTES : n;
}

static int clip_valid(const clip_engine_t *e, uint8_t clip)
{
    uint8_t role, lo, hi;
    if (!e->song || clip >= NCLIPS || !e->song->clip[clip].used)
        return 0;
    role = clip_role(clip);
    lo = e->song->role[role].first_clip;
    hi = (uint8_t)(lo + e->song->role[role].nclips);
    if (e->song->role[role].nclips == 0)
        return 1;                     /* an un-filled song: clip_role decides the role */
    return clip >= lo && clip < hi;
}

/* ---- events -------------------------------------------------------------- */

static void ev(const clip_sink_t *sink, uint32_t sample, uint8_t kind, uint8_t role,
               uint8_t a, uint8_t b, uint8_t c)
{
    clip_event_t e;
    if (!sink || !sink->ev)
        return;
    e.sample = sample;
    e.kind = kind;
    e.role = role;
    e.a = a;
    e.b = b;
    e.c = c;
    sink->ev(sink->ctx, &e);
}

/* ---- note pool ----------------------------------------------------------- */

static int note_set(clip_engine_t *e, uint8_t role, uint8_t note, uint32_t end)
{
    uint32_t i;
    for (i = 0; i < CLIP_ENGINE_MAX_VOICES; i++)
        if (!e->note[i].used) {
            e->note[i].used = 1;
            e->note[i].role = role;
            e->note[i].note = note;
            e->note[i].end = end;
            return 1;
        }
    return 0;                          /* full: drop the note rather than leave it stuck on */
}

static uint32_t note_next(const clip_engine_t *e)
{
    uint32_t i, m = NEVER32;
    for (i = 0; i < CLIP_ENGINE_MAX_VOICES; i++)
        if (e->note[i].used && (m == NEVER32 || (int32_t)(e->note[i].end - m) < 0))
            m = e->note[i].end;
    return m;
}

/* release every note whose end is on or before upto; emit at window offset off */
static void note_due(clip_engine_t *e, uint32_t upto, uint32_t off, const clip_sink_t *sink)
{
    uint32_t i;
    for (i = 0; i < CLIP_ENGINE_MAX_VOICES; i++) {
        if (e->note[i].used && (int32_t)(e->note[i].end - upto) <= 0) {
            uint8_t role = e->note[i].role, note = e->note[i].note;
            e->note[i].used = 0;
            ev(sink, off, CLIPEV_NOTE_OFF, role, note, 0, 0);
        }
    }
}

/* release everything a role holds (clip stop / switch / transport stop) */
static void release_role(clip_engine_t *e, uint8_t role, uint32_t off, const clip_sink_t *sink)
{
    uint32_t i;
    for (i = 0; i < CLIP_ENGINE_MAX_VOICES; i++) {
        if (e->note[i].used && e->note[i].role == role) {
            uint8_t note = e->note[i].note;
            e->note[i].used = 0;
            ev(sink, off, CLIPEV_NOTE_OFF, role, note, 0, 0);
        }
    }
}

/* ---- firing one step ----------------------------------------------------- */

/* release every sounding note of one role+pitch (a retrigger) */
static void release_pitch(clip_engine_t *e, uint8_t role, uint8_t note, uint32_t off,
                          const clip_sink_t *sink)
{
    uint32_t i;
    for (i = 0; i < CLIP_ENGINE_MAX_VOICES; i++) {
        if (e->note[i].used && e->note[i].role == role && e->note[i].note == note) {
            e->note[i].used = 0;
            ev(sink, off, CLIPEV_NOTE_OFF, role, note, 0, 0);
        }
    }
}

/* the note/drum/motion events of one step, at window offset off (sample abs) */
static void step_events(clip_engine_t *e, uint8_t role, uint8_t clip, uint8_t step,
                        uint32_t off, uint32_t abs, const clip_sink_t *sink)
{
    const clip_t *c = &e->play[role];        /* T7b: the playing copy, not the editable song */
    int i;

    if (e->muted[role])                 /* T10i: a muted role sounds nothing, but its position */
        return;                         /* keeps advancing (the transport does not stop) */

    /* motion first: a drum or a note hears this step's parameters as it fires */
    for (i = 0; i < (int)CLIP_MOTION_LANES; i++) {
        const motion_lane_t *m = &c->motion[i];
        if (m->param == PARAM_NONE || m->v[step] == MOTION_NONE)
            continue;
        ev(sink, off, CLIPEV_MOTION, role, (uint8_t)(m->param & 0xFFu),
           (uint8_t)(m->param >> 8), m->v[step]);
    }

    if (role == ROLE_DRUMS) {
        uint8_t acc = (uint8_t)((c->d.drum.accent[step >> 5] >> (step & 31u)) & 1u);
        int lane;
        for (lane = 0; lane < (int)DRUM_LANES; lane++) {
            if (!drum_get(&c->d.drum, (unsigned)lane, step))
                continue;
            ev(sink, off, CLIPEV_DRUM, role, (uint8_t)lane, acc, clip);
        }
    } else {
        int nn = clip_notes(c);
        for (i = 0; i < nn; i++) {
            const note_t *nt = &c->d.notes[i];
            uint32_t dur, end;
            if (nt->len == 0 || nt->step != step)
                continue;
            release_pitch(e, role, nt->note, off, sink);   /* a retrigger cuts the old one */
            dur = (uint32_t)((uint64_t)nt->len * e->step_L);
            if (dur == 0)
                dur = 1;
            end = abs + dur;
            if ((int32_t)(end - abs) <= 0)
                end = abs + 1u;
            if (note_set(e, role, nt->note, end))
                ev(sink, off, CLIPEV_NOTE_ON, role, nt->note, nt->vel, clip);
        }
    }
}

/* ---- queue --------------------------------------------------------------- */

/* global steps from now until the action takes effect (>= 1) */
static uint32_t action_lead(const clip_engine_t *e, uint8_t role, uint8_t quant)
{
    uint32_t r;
    if (quant == Q_END && e->cur[role] != CLIP_NONE) {
        int len = play_len(e, role);
        int off = len - (int)e->pos[role];
        return off < 1 ? 1u : (uint32_t)off;
    }
    if (quant == Q_BEAT) {
        r = (4u - (e->global_step & 3u)) & 3u;
        return r ? r : 4u;
    }
    /* Q_BAR, or Q_END with nothing playing */
    r = (STEPS_PER_BAR - (e->global_step & (STEPS_PER_BAR - 1u))) & (STEPS_PER_BAR - 1u);
    return r ? r : STEPS_PER_BAR;
}

/* ---- transport ----------------------------------------------------------- */

static int push_cmd(clip_engine_t *e, uint8_t cmd, uint8_t clip, uint8_t quant, uint16_t bpm, uint32_t at);

void clip_engine_init(clip_engine_t *e, song_t *song)
{
    uint32_t i;
    uint8_t j;
    for (i = 0; i < sizeof *e; i++)
        ((uint8_t *)e)[i] = 0;
    e->song = song;
    for (j = 0; j < NROLES; j++) {
        e->cur[j] = CLIP_NONE;
        e->que[j] = CLIP_NONE;
        e->s_cur[j] = CLIP_NONE;              /* T11i: nothing to relaunch until a clip plays */
    }
    e->bpm_x10 = (song && song->bpm_x10) ? song->bpm_x10 : 1200u;
    e->bpm_src = e->bpm_x10;
    e->song_bpm = e->bpm_x10;
    e->bpm_x10 = clamp_bpm(e->bpm_x10);
    e->step_L = CLIP_STEP_NUM / e->bpm_x10;
    e->step_f = CLIP_STEP_NUM % e->bpm_x10;
}

/* T11i: the PLAY button's start. `playing` is set here for the UI and the tests, and a ring
 * command is pushed after it, so a transport Stop queued earlier (e.g. leaving LIVE) cannot stop
 * this start: the ISR applies Stop then Start, in order. */
void clip_engine_start(clip_engine_t *e)
{
    e->playing = 1;
    e->resume = 0;
    push_cmd(e, CLIP_CMD_START, 0, 0, 0, 0);
}

/* T11i: stage the copy a MIDI Start/Continue will relaunch, the same main-loop staging a launch
 * uses (launch_cmd), so the ISR swaps in the current song bytes: an edit made while the transport
 * was stopped is picked up. The caller masks the audio IRQs (engine_crit), as for a launch. */
static void relaunch_stage(clip_engine_t *e)
{
    uint8_t r;
    if (!e->song)
        return;
    for (r = 0; r < NROLES; r++) {
        uint8_t clip;
        if (e->que_set[r])
            continue;                 /* a launch is already staged for this role: it wins */
        clip = e->cur[r] != CLIP_NONE ? e->cur[r] : e->s_cur[r];
        if (clip != CLIP_NONE && clip < NCLIPS) {
            e->next[r] = e->song->clip[clip];
            e->next_clip[r] = clip;
            e->next_set[r] = 1;
        }
    }
}

void clip_engine_restart(clip_engine_t *e) { relaunch_stage(e); push_cmd(e, CLIP_CMD_RESTART, 0, 0, 0, 0); }
void clip_engine_continue(clip_engine_t *e) { relaunch_stage(e); push_cmd(e, CLIP_CMD_CONTINUE, 0, 0, 0, 0); }

/* T11i: the transport stop is a ring command too, so a Start and a Stop in the same main-loop poll
 * apply in order (the Stop wins). `playing` is also cleared here, so leaving LIVE — where the clip
 * engine stops rendering — still reads as stopped. */
void clip_engine_transport_stop(clip_engine_t *e)
{
    e->playing = 0;
    push_cmd(e, CLIP_CMD_TRANSPORT_STOP, 0, 0, 0, 0);
}

/* ---- UI entry points ----------------------------------------------------- */

static int push_cmd(clip_engine_t *e, uint8_t cmd, uint8_t clip, uint8_t quant, uint16_t bpm, uint32_t at)
{
    uint8_t head = e->cmd_head, next = (uint8_t)((head + 1u) % CLIP_CMD_RING);
#ifdef X0X_HOST
    if (clip_engine_isr)               /* T9i r1: an ISR-side path must never push onto this ring */
        __builtin_trap();
#endif
    if (next == e->cmd_tail)
        return -1;                        /* full: the UI may retry */
    e->cmd[head].cmd = cmd;
    e->cmd[head].clip = clip;
    e->cmd[head].quant = quant;
    e->cmd[head].bpm_x10 = bpm;
    e->cmd[head].at = at;
    CLIP_BARRIER();                       /* publish the slot before the head */
    e->cmd_head = next;
    return 0;
}

/* T7b: stage a launch target's copy in the main loop, so the ISR only ever swaps play[]/next[].
 * The command is pushed FIRST: if the ring is full the launch is refused (return -1) and nothing is
 * staged, so a valid launch already queued keeps its copy. Staging then happens in the same
 * main-loop iteration, well before the ISR applies the launch at the boundary. A clip that does not
 * validate is refused and touches no staging. */
static int launch_cmd(clip_engine_t *e, uint8_t clip, uint8_t cmd, uint8_t quant, uint32_t at)
{
    uint8_t role;
    if (clip >= NCLIPS || !e->song)
        return -1;
    if (!clip_valid(e, clip))
        return -1;                        /* invalid: do not clear a queued valid launch's staging */
    if (push_cmd(e, cmd, clip, quant, 0, at) != 0)
        return -1;                        /* ring full: refused, nothing staged */
    role = clip_role(clip);
    e->next[role] = e->song->clip[clip];
    e->next_clip[role] = clip;
    e->next_set[role] = 1;
    return 0;
}

void clip_engine_launch_quant(clip_engine_t *e, uint8_t clip, uint8_t quant)
{
    launch_cmd(e, clip, CLIP_CMD_LAUNCH, (uint8_t)(quant & 3u), 0);
}

void clip_engine_launch(clip_engine_t *e, uint8_t clip)
{
    if (clip >= NCLIPS || !e->song)
        return;
    launch_cmd(e, clip, CLIP_CMD_LAUNCH, (uint8_t)(e->song->clip[clip].quant & 3u), 0);
}

void clip_engine_launch_now(clip_engine_t *e, uint8_t clip)
{
    launch_cmd(e, clip, CLIP_CMD_LAUNCH_NOW, 0, 0);
}

void clip_engine_launch_at(clip_engine_t *e, uint8_t clip, uint32_t at_step)
{
    launch_cmd(e, clip, CLIP_CMD_LAUNCH_AT, 0, at_step);
}

void clip_engine_stop(clip_engine_t *e, uint8_t role)
{
    if (role >= NROLES)
        return;
    push_cmd(e, CLIP_CMD_STOP, role, Q_END, 0, 0);
}

void clip_engine_queue_bpm(clip_engine_t *e, uint16_t bpm_x10)
{
    push_cmd(e, CLIP_CMD_SET_BPM, 0, 0, bpm_x10, 0);
}

void clip_engine_clip_changed(clip_engine_t *e, uint8_t clip)
{
    uint8_t role;
    if (clip >= NCLIPS || !e->song)
        return;
    role = clip_role(clip);
    if (e->cur[role] == clip)                   /* the role is playing it: refresh the copy */
        e->play[role] = e->song->clip[clip];
    if (e->next_set[role] && e->next_clip[role] == clip)   /* a launch is staged: refresh that too */
        e->next[role] = e->song->clip[clip];
}

/* T11i: the external clock's tempo: the running tempo only, never the song's */
void clip_engine_queue_bpm_soft(clip_engine_t *e, uint16_t bpm_x10)
{
    push_cmd(e, CLIP_CMD_SET_BPM_SOFT, 0, 0, bpm_x10, 0);
}

/* T10i: mute / unmute a role, through the ring. */
void clip_engine_mute(clip_engine_t *e, uint8_t role, int on)
{
    if (role >= NROLES)
        return;
    push_cmd(e, CLIP_CMD_MUTE, role, (uint8_t)(on ? 1 : 0), 0, 0);
}

int clip_engine_muted(const clip_engine_t *e, uint8_t role)
{
    return role < NROLES ? (int)e->muted[role] : 0;
}

void clip_engine_mute_clear(clip_engine_t *e)
{
    uint8_t r;
    for (r = 0; r < NROLES; r++)
        e->muted[r] = 0;                       /* immediate: the engine may stop rendering (classic) */
    /* ... and a ring command ordered AFTER any pending mute, so a queued CLIP_CMD_MUTE that drains
     * later cannot re-apply (T10i review). */
    push_cmd(e, CLIP_CMD_MUTE_CLEAR, 0, 0, 0, 0);
}

/* ---- T9i: the take player's ISR-side entry points ------------------------ *
 * These run from the audio ISR (the take sink), so they never push onto the command ring and never
 * read song->clip[]. The main loop stages the launch's copy in next[role]; apply_role swaps it in at
 * the boundary, so the sound is the staged snapshot. */

/* how: 0 = queue at `quant` (0xFF = the staged copy's own quantize), 1 = next step. */
int clip_engine_take_launch(clip_engine_t *e, uint8_t clip, uint8_t how, uint8_t quant)
{
    uint8_t role;
    if (clip >= NCLIPS)
        return 0;
    role = clip_role(clip);
    /* T9i r2/B1: the hard invariant. The ISR may only queue a launch whose staged copy is in
     * next[role]; apply_role() will swap it into play[] at the boundary. If the main loop did not
     * stage it (a missed lookahead window), DROP the launch: cur stays, play stays, and the drop is
     * counted. Never queue against a stale play[] copy. */
    if (!e->next_set[role] || e->next_clip[role] != clip) {
        e->take_drop++;
        return 0;
    }
    /* a relay launch of the already-queued clip cancels it, as consume_cmds does (not for NOW) */
    if (how == 0 && e->que_set[role] && e->que[role] == clip) {
        e->que_set[role] = 0;
        e->que[role] = CLIP_NONE;
        return 1;
    }
    e->que[role] = clip;
    e->que_set[role] = 1;
    if (how == 1)
        e->que_at[role] = e->global_step + 1u;
    else
        e->que_at[role] = e->global_step + action_lead(e, role, quant == 0xFFu ? e->next[role].quant : quant);
    return 1;
}

void clip_engine_take_stop(clip_engine_t *e, uint8_t role)
{
    uint8_t q;
    if (role >= NROLES)
        return;
    if (e->que_set[role] && e->que[role] == CLIP_NONE) {   /* a second stop cancels, as consume_cmds */
        e->que_set[role] = 0;
        return;
    }
    e->que[role] = CLIP_NONE;
    e->que_set[role] = 1;
    q = (e->cur[role] != CLIP_NONE) ? e->play[role].quant : Q_BAR;
    e->que_at[role] = e->global_step + action_lead(e, role, q);
}

/* Latch a take mute; clip_engine_render() applies it at the window start (release_role needs the
 * sink, which only the render has). */
void clip_engine_take_mute(clip_engine_t *e, uint8_t role, int on)
{
    if (role >= NROLES)
        return;
    e->take_mute[role] = (uint8_t)(on ? 1 : 0);
    e->take_mute_set = 1;
}

void clip_engine_take_mute_clear(clip_engine_t *e)
{
    uint8_t r;
    for (r = 0; r < NROLES; r++) {
        e->muted[r] = 0;
        e->take_mute[r] = 0;
    }
    e->take_mute_set = 0;
}

void clip_engine_take_bpm(clip_engine_t *e, uint16_t bpm_x10)
{
    e->bpm_take_on = 1;                                /* B2: hold the take's tempo, not the song's */
    set_bpm(e, bpm_x10);
}

/* B2: the take playback begins: hold whatever tempo is running (the prelude's tempo mark follows at
 * offset 0) so the song's own tempo cannot clobber it in the meantime. */
void clip_engine_take_bpm_hold(clip_engine_t *e) { e->bpm_take_on = 1; }

/* B2: the take ended/stopped: release the hold and go back to the song's own tempo. Pure
 * arithmetic (no ring), so the take's natural ISR end may call it. */
void clip_engine_take_bpm_clear(clip_engine_t *e)
{
    e->bpm_take_on = 0;
    if (e->song && e->song->bpm_x10) {
        e->bpm_ext = 0;
        set_bpm(e, e->song->bpm_x10);
    }
}

int clip_engine_take_drops(const clip_engine_t *e) { return e ? e->take_drop : 0; }

/* ---- the render loop ----------------------------------------------------- */

/* apply a queued action (off = the boundary's window offset) */
static void apply_role(clip_engine_t *e, uint8_t role, uint32_t off, const clip_sink_t *sink)
{
    uint8_t target = e->que[role];
    e->que_set[role] = 0;
    release_role(e, role, off, sink);
    if (target == CLIP_NONE) {
        /* T7b: do not clear next_set here: a valid launch may still be queued behind this stop. */
        if (e->cur[role] != CLIP_NONE) {
            ev(sink, off, CLIPEV_CLIP_STOPPED, role, 0, 0, e->cur[role]);
            e->cur[role] = CLIP_NONE;
        }
        e->pos[role] = 0;
    } else {
        /* T9i r2/B1: the hard invariant. Only a target whose staged copy is present may become
         * cur[role]; the ISR never sets cur without swapping the matching play[] copy in. If the
         * staging is missing (a missed window), drop the launch and keep cur/play consistent. */
        if (e->next_set[role] && e->next_clip[role] == target) {   /* T7b: next -> play swap */
            e->play[role] = e->next[role];
            e->next_set[role] = 0;
            e->cur[role] = target;
            e->pos[role] = 0;
            ev(sink, off, CLIPEV_CLIP_STARTED, role, 0, 0, target);
        }
    }
}

/* advance a playing role one step */
static void advance_role(clip_engine_t *e, uint8_t role, uint32_t off, uint32_t abs,
                         const clip_sink_t *sink)
{
    uint8_t clip = e->cur[role];
    int len, next;
    if (clip == CLIP_NONE) {
        e->pos[role] = 0;
        return;
    }
    len = play_len(e, role);
    next = (int)e->pos[role] + 1;
    if (next >= len) {
        if (e->play[role].mode == CLIP_ONCE) {
            release_role(e, role, off, sink);
            ev(sink, off, CLIPEV_CLIP_STOPPED, role, 0, 0, clip);
            e->cur[role] = CLIP_NONE;
            e->pos[role] = 0;
            return;
        }
        /* loop: clamp notes still sounding to the loop point, then restart at step 0. T7b: an edit
         * to the playing clip was already copied into play[] by the main loop, so the loop uses it
         * as it stands (no song read in the ISR). */
        {
            uint32_t i;
            for (i = 0; i < CLIP_ENGINE_MAX_VOICES; i++)
                if (e->note[i].used && e->note[i].role == role &&
                    (int32_t)(e->note[i].end - abs) > 0)
                    e->note[i].end = abs;
        }
        next = 0;
    }
    e->pos[role] = (uint8_t)next;
}

static void consume_cmds(clip_engine_t *e, const clip_sink_t *sink)
{
    for (;;) {
        uint8_t head = e->cmd_head;
        clip_cmd_t c;
        uint8_t role, target;
        CLIP_BARRIER();                       /* read the head before the slot */
        if (e->cmd_tail == head)
            break;
        c = e->cmd[e->cmd_tail];
        e->cmd_tail = (uint8_t)((e->cmd_tail + 1u) % CLIP_CMD_RING);

        if (c.cmd == CLIP_CMD_SET_BPM) {
            if (e->song)
                e->song->bpm_x10 = c.bpm_x10;  /* one source of truth */
            e->bpm_ext = 0;                    /* T11i: the song's tempo wins again */
            set_bpm(e, c.bpm_x10);
            continue;
        }
        if (c.cmd == CLIP_CMD_SET_BPM_SOFT) {  /* T11i: the external clock's live tempo */
            e->bpm_ext = c.bpm_x10;
            set_bpm(e, c.bpm_x10);
            continue;
        }
        if (c.cmd == CLIP_CMD_RESTART) {       /* T11i: MIDI Start, applied here so only the ISR
                                                * changes the transport */
            e->playing = 1;
            e->resume = 0;
            e->restart = 1;
            continue;
        }
        if (c.cmd == CLIP_CMD_CONTINUE) {      /* T11i: MIDI Continue */
            e->playing = 1;
            e->resume = 1;
            e->restart = 0;
            continue;
        }
        if (c.cmd == CLIP_CMD_TRANSPORT_STOP) {   /* T11i: the transport stop, ring-ordered */
            e->playing = 0;
            continue;
        }
        if (c.cmd == CLIP_CMD_START) {            /* T11i: the PLAY button's start, ring-ordered */
            e->playing = 1;
            e->resume = 0;
            continue;
        }
        if (c.cmd == CLIP_CMD_MUTE) {                          /* T10i */
            role = c.clip;
            if (role >= NROLES)
                continue;
            if (c.quant) {
                if (!e->muted[role]) {
                    e->muted[role] = 1;
                    release_role(e, role, 0, sink);            /* the sounding notes go now */
                }
            } else {
                e->muted[role] = 0;                            /* unmute: rejoins in time */
            }
            continue;                                          /* live mutes are not song state (T10i review) */
        }
        if (c.cmd == CLIP_CMD_MUTE_CLEAR) {                    /* T10i: ordered after any queued mute */
            uint8_t r;
            for (r = 0; r < NROLES; r++)
                e->muted[r] = 0;
            continue;
        }
        if (c.cmd == CLIP_CMD_STOP) {
            role = c.clip;
            if (role >= NROLES)
                continue;
            if (e->que_set[role] && e->que[role] == CLIP_NONE) {   /* a second stop cancels */
                e->que_set[role] = 0;
                ev(sink, 0, CLIPEV_CLIP_CANCELLED, role, 0, 0, CLIP_NONE);
                continue;
            }
            e->que[role] = CLIP_NONE;
            e->que_set[role] = 1;
            {
                uint8_t q = (e->cur[role] != CLIP_NONE)
                            ? e->play[role].quant : Q_BAR;
                e->que_at[role] = e->global_step + action_lead(e, role, q);
            }
            ev(sink, 0, CLIPEV_CLIP_QUEUED, role, 0, 0, CLIP_NONE);
            continue;
        }
        /* CLIP_CMD_LAUNCH / CLIP_CMD_LAUNCH_NOW / CLIP_CMD_LAUNCH_AT: the main loop staged the
         * target's copy in next[] after a successful push, so the ISR queues the launch and never
         * reads song->clip[] here. The staged copy is swapped in at apply_role (the boundary), by
         * which time the main loop's staging is long done. */
        target = c.clip;
        if (target >= NCLIPS)
            continue;
        role = clip_role(target);
        if (c.cmd == CLIP_CMD_LAUNCH && e->que_set[role] && e->que[role] == target) {
            e->que_set[role] = 0;             /* relaunching the queued clip cancels */
            e->que[role] = CLIP_NONE;
            ev(sink, 0, CLIPEV_CLIP_CANCELLED, role, 0, 0, target);
            continue;
        }
        e->que[role] = target;
        e->que_set[role] = 1;
        if (c.cmd == CLIP_CMD_LAUNCH_AT)
            e->que_at[role] = c.at;           /* exactly the requested global step (T7b) */
        else
            e->que_at[role] = e->global_step +
                (c.cmd == CLIP_CMD_LAUNCH_NOW ? 1u : action_lead(e, role, c.quant));
        ev(sink, 0, CLIPEV_CLIP_QUEUED, role, 0, 0, target);
    }
}

/* process the step that has just begun (off/abs = its window offset and sample) */
static void do_step(clip_engine_t *e, uint32_t off, uint32_t abs, const clip_sink_t *sink)
{
    uint8_t r, done = 0;
    for (r = 0; r < NROLES; r++) {
        if (e->que_set[r] && (int32_t)(e->que_at[r] - e->global_step) <= 0) {
            apply_role(e, r, off, sink);
            done |= (uint8_t)(1u << r);
        }
    }
    for (r = 0; r < NROLES; r++) {
        if ((done >> r) & 1u)
            continue;
        if (e->cur[r] != CLIP_NONE)
            advance_role(e, r, off, abs, sink);
    }
    for (r = 0; r < NROLES; r++) {
        if (e->cur[r] != CLIP_NONE)
            step_events(e, r, e->cur[r], e->pos[r], off, abs, sink);
    }
}

void clip_engine_render(clip_engine_t *e, uint32_t nframes, const clip_sink_t *sink)
{
    uint32_t base, end, r;

    if (!e->song) {
        if (e->playing) {
            e->playing = 0;
            for (r = 0; r < NROLES; r++)
                release_role(e, r, 0, sink);
            ev(sink, 0, CLIPEV_STOPPED, 0, 0, 0, 0);
        }
        return;
    }
    if (!nframes)
        return;

    /* T9i: a take mute latched by the sink is applied here, at the window start, so its release's
     * note-offs reach the sink (the sink is only available during the render). */
    if (e->take_mute_set) {
        uint8_t r;
        e->take_mute_set = 0;
        for (r = 0; r < NROLES; r++) {
            if (e->take_mute[r]) {
                if (!e->muted[r]) {
                    e->muted[r] = 1;
                    release_role(e, r, 0, sink);
                }
            } else {
                e->muted[r] = 0;
            }
        }
    }

    /* the song is the tempo source of truth: the UI writes song->bpm_x10 directly, or a
     * ring tempo command writes it (in consume_cmds). T9i r2/B2: while a take playback owns the
     * tempo (bpm_take_on), the song's tempo (and an edited/stored value) must NOT clobber it. */
    if (e->song->bpm_x10 && e->song->bpm_x10 != e->song_bpm) {
        e->song_bpm = e->song->bpm_x10;     /* T11i: a loaded or edited song's tempo wins over a
                                             * clock that is no longer present */
        if (!e->bpm_take_on)
            e->bpm_ext = 0;
    }
    if (!e->bpm_take_on && e->song->bpm_x10 && e->song->bpm_x10 != e->bpm_src && !e->bpm_ext)
        set_bpm(e, e->song->bpm_x10);

    consume_cmds(e, sink);

    if (e->playing && (e->restart || !e->was_playing)) {
        uint8_t restart = e->restart;
        e->restart = 0;
        for (r = 0; r < NROLES; r++)               /* T11i: a restart/continue leaves nothing sounding */
            release_role(e, r, 0, sink);
        if (e->resume) {
            /* T11i: Continue resumes the stop point (time paused during the stop) */
            e->resume = 0;
            e->elapsed = e->s_elapsed;
            e->step_sample = e->s_step_sample;
            e->step_frac = e->s_step_frac;
            e->global_step = e->s_global;
            for (r = 0; r < NROLES; r++) {
                e->cur[r] = e->s_cur[r];
                e->pos[r] = e->s_pos[r];
                if (e->next_set[r] && e->next_clip[r] == e->s_cur[r]) {   /* T11i: an edit while stopped is picked up */
                    e->play[r] = e->next[r];
                    e->next_set[r] = 0;
                }
            }
            ev(sink, 0, CLIPEV_PLAYING, 1, 0, 0, 0);
            base = e->elapsed;
        } else {
            for (r = 0; r < NROLES; r++) {
                if (restart && !e->que_set[r]) {      /* T11i: MIDI Start relaunches what was playing
                                                       * (or what a stop saved) from step 0, through the
                                                       * launch path so step 0 fires, not step 1 */
                    uint8_t clip = e->cur[r] != CLIP_NONE ? e->cur[r] : e->s_cur[r];
                    if (clip != CLIP_NONE) {
                        e->que[r] = clip;
                        e->que_set[r] = 1;
                    }
                }
                e->pos[r] = 0;
                if (e->que_set[r])
                    e->que_at[r] = 0;
            }
            e->global_step = 0;
            e->step_sample = 0;
            e->step_frac = 0;
            e->elapsed = 0;
            ev(sink, 0, CLIPEV_PLAYING, 1, 0, 0, 0);
            do_step(e, 0, 0, sink);               /* step 0 fires at sample 0 */
            base = 0;
        }
    } else if (!e->playing) {
        if (e->was_playing) {
            for (r = 0; r < NROLES; r++) {
                release_role(e, r, 0, sink);
                e->s_cur[r] = e->cur[r];           /* T11i: what a Continue will resume */
                e->s_pos[r] = e->pos[r];
                e->cur[r] = CLIP_NONE;
                e->pos[r] = 0;
            }
            e->s_elapsed = e->elapsed;
            e->s_step_sample = e->step_sample;
            e->s_step_frac = e->step_frac;
            e->s_global = e->global_step;
            ev(sink, 0, CLIPEV_STOPPED, 0, 0, 0, 0);
        }
        e->was_playing = 0;
        return;
    } else {
        e->resume = 0;                         /* a Continue while already playing is a no-op */
        base = e->elapsed;
    }

    end = base + nframes;
    while ((int32_t)(e->elapsed - end) < 0) {
        uint32_t Tn;
        if (note_next(e) != NEVER32 && (int32_t)(note_next(e) - e->elapsed) <= 0) {
            note_due(e, e->elapsed, e->elapsed - base, sink);
            continue;
        }
        Tn = next_step_time(e);
        if ((int32_t)(Tn - e->elapsed) <= 0) {
            step_advance(e);
            do_step(e, e->elapsed - base, e->elapsed, sink);
            continue;
        }
        {
            uint32_t next = (int32_t)(Tn - end) < 0 ? Tn : end;
            uint32_t no = note_next(e);
            if (no != NEVER32 && (int32_t)(no - next) < 0 && (int32_t)(no - e->elapsed) > 0)
                next = no;
            if ((int32_t)(next - e->elapsed) <= 0)
                next = end;
            e->elapsed = next;
        }
    }
    e->was_playing = e->playing;
}

/* ---- queries ------------------------------------------------------------- */

uint8_t clip_engine_playing_clip(const clip_engine_t *e, uint8_t role)
{
    return role < NROLES ? e->cur[role] : CLIP_NONE;
}

uint8_t clip_engine_queued_clip(const clip_engine_t *e, uint8_t role)
{
    uint8_t q, s;
    if (role >= NROLES)
        return CLIP_NONE;
    CLIP_BARRIER();                        /* a consistent snapshot of the pair */
    s = e->que_set[role];
    q = e->que[role];
    CLIP_BARRIER();
    return s ? q : CLIP_NONE;
}

uint8_t clip_engine_progress(const clip_engine_t *e, uint8_t role)
{
    int len;
    uint32_t p;
    if (role >= NROLES || e->cur[role] == CLIP_NONE)
        return 0;
    len = play_len(e, role);
    p = ((uint32_t)e->pos[role] * 256u) / (uint32_t)len;
    return p > 255u ? 255u : (uint8_t)p;
}

void clip_engine_pos(const clip_engine_t *e, uint8_t *bar, uint8_t *beat, uint8_t *step)
{
    uint32_t s = e->global_step, inbar = s & (STEPS_PER_BAR - 1u);
    if (bar)  *bar  = (uint8_t)((s / STEPS_PER_BAR) & 0xFFu);
    if (beat) *beat = (uint8_t)(inbar / 4u);
    if (step) *step = (uint8_t)inbar;
}

uint32_t clip_engine_global_step(const clip_engine_t *e) { return e->global_step; }

uint8_t clip_engine_step_phase(const clip_engine_t *e)
{
    uint32_t into;
    if (!e->playing || !e->step_L)
        return 0;
    into = e->elapsed - e->step_sample;          /* samples into the current step */
    if (into >= e->step_L)
        return 255;
    into = (into * 256u) / e->step_L;
    return into > 255u ? 255u : (uint8_t)into;
}

uint8_t clip_engine_clip_step(const clip_engine_t *e, uint8_t role)
{
    return role < NROLES ? e->pos[role] : 0;
}
