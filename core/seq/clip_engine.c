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
    }
    e->bpm_x10 = (song && song->bpm_x10) ? song->bpm_x10 : 1200u;
    e->bpm_src = e->bpm_x10;
    e->bpm_x10 = clamp_bpm(e->bpm_x10);
    e->step_L = CLIP_STEP_NUM / e->bpm_x10;
    e->step_f = CLIP_STEP_NUM % e->bpm_x10;
}

void clip_engine_start(clip_engine_t *e) { e->playing = 1; }
void clip_engine_transport_stop(clip_engine_t *e) { e->playing = 0; }

/* ---- UI entry points ----------------------------------------------------- */

static int push_cmd(clip_engine_t *e, uint8_t cmd, uint8_t clip, uint8_t quant, uint16_t bpm)
{
    uint8_t head = e->cmd_head, next = (uint8_t)((head + 1u) % CLIP_CMD_RING);
    if (next == e->cmd_tail)
        return -1;                        /* full: the UI may retry */
    e->cmd[head].cmd = cmd;
    e->cmd[head].clip = clip;
    e->cmd[head].quant = quant;
    e->cmd[head].bpm_x10 = bpm;
    CLIP_BARRIER();                       /* publish the slot before the head */
    e->cmd_head = next;
    return 0;
}

void clip_engine_launch_quant(clip_engine_t *e, uint8_t clip, uint8_t quant)
{
    if (clip >= NCLIPS)
        return;
    push_cmd(e, CLIP_CMD_LAUNCH, clip, (uint8_t)(quant & 3u), 0);
}

void clip_engine_launch(clip_engine_t *e, uint8_t clip)
{
    if (clip >= NCLIPS || !e->song)
        return;
    push_cmd(e, CLIP_CMD_LAUNCH, clip, (uint8_t)(e->song->clip[clip].quant & 3u), 0);
}

void clip_engine_launch_now(clip_engine_t *e, uint8_t clip)
{
    if (clip >= NCLIPS)
        return;
    push_cmd(e, CLIP_CMD_LAUNCH_NOW, clip, 0, 0);
}

void clip_engine_stop(clip_engine_t *e, uint8_t role)
{
    if (role >= NROLES)
        return;
    push_cmd(e, CLIP_CMD_STOP, role, Q_END, 0);
}

void clip_engine_queue_bpm(clip_engine_t *e, uint16_t bpm_x10)
{
    push_cmd(e, CLIP_CMD_SET_BPM, 0, 0, bpm_x10);
}

void clip_engine_reload(clip_engine_t *e, uint8_t clip)
{
    if (clip >= NCLIPS)
        return;
    push_cmd(e, CLIP_CMD_RELOAD, clip, 0, 0);
}

/* ---- the render loop ----------------------------------------------------- */

/* apply a queued action (off = the boundary's window offset) */
static void apply_role(clip_engine_t *e, uint8_t role, uint32_t off, const clip_sink_t *sink)
{
    uint8_t target = e->que[role];
    e->que_set[role] = 0;
    release_role(e, role, off, sink);
    if (target == CLIP_NONE) {
        if (e->cur[role] != CLIP_NONE) {
            ev(sink, off, CLIPEV_CLIP_STOPPED, role, 0, 0, e->cur[role]);
            e->cur[role] = CLIP_NONE;
        }
        e->pos[role] = 0;
    } else {
        e->cur[role] = target;
        e->pos[role] = 0;
        e->play[role] = e->song->clip[target];    /* T7b: the ISR reads this private copy */
        ev(sink, off, CLIPEV_CLIP_STARTED, role, 0, 0, target);
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
        /* loop: clamp notes still sounding to the loop point, then restart at step 0. T7b: the
         * loop point is also where an edit to the playing clip lands (a fresh copy). */
        {
            uint32_t i;
            for (i = 0; i < CLIP_ENGINE_MAX_VOICES; i++)
                if (e->note[i].used && e->note[i].role == role &&
                    (int32_t)(e->note[i].end - abs) > 0)
                    e->note[i].end = abs;
        }
        e->play[role] = e->song->clip[clip];
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
            set_bpm(e, c.bpm_x10);
            continue;
        }
        if (c.cmd == CLIP_CMD_RELOAD) {        /* T7b: an edit landed; re-take the playing copy */
            uint8_t r = clip_role(c.clip);
            if (e->cur[r] == c.clip)
                e->play[r] = e->song->clip[c.clip];
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
        /* CLIP_CMD_LAUNCH / CLIP_CMD_LAUNCH_NOW */
        target = c.clip;
        if (!clip_valid(e, target))
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

    /* the song is the tempo source of truth: the UI writes song->bpm_x10 directly, or a
     * ring tempo command writes it (in consume_cmds) */
    if (e->song->bpm_x10 && e->song->bpm_x10 != e->bpm_src)
        set_bpm(e, e->song->bpm_x10);

    consume_cmds(e, sink);

    if (e->playing && !e->was_playing) {
        e->global_step = 0;
        e->step_sample = 0;
        e->step_frac = 0;
        e->elapsed = 0;
        for (r = 0; r < NROLES; r++) {
            e->pos[r] = 0;
            if (e->que_set[r])
                e->que_at[r] = 0;
        }
        ev(sink, 0, CLIPEV_PLAYING, 1, 0, 0, 0);
        do_step(e, 0, 0, sink);               /* step 0 fires at sample 0 */
        base = 0;
    } else if (!e->playing) {
        if (e->was_playing) {
            for (r = 0; r < NROLES; r++) {
                release_role(e, r, 0, sink);
                e->cur[r] = CLIP_NONE;
                e->pos[r] = 0;
            }
            ev(sink, 0, CLIPEV_STOPPED, 0, 0, 0, 0);
        }
        e->was_playing = 0;
        return;
    } else {
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

uint8_t clip_engine_clip_step(const clip_engine_t *e, uint8_t role)
{
    return role < NROLES ? e->pos[role] : 0;
}
