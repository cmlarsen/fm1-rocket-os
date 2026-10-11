/* SPDX-License-Identifier: GPL-3.0-only */
/* T7b: the clip editors — STUDIO 3 (notes), 4 (the drum grid) and 15 (motion) — plus recording
 * into a clip. Included by ui.c (like ui_studio.c / ui_live.c), after the drawing helpers, so it
 * can use them and reach ui/proj/live_bridge/song_io. It edits the clip under the song home's
 * cursor through song_io (plain void *), so it never needs model.h; the clip engine is told to
 * re-take its playing copy with live_bridge_clip_reload() after an edit.
 *
 * SEQ cycles NOTES (or the DRUM grid) -> MOTION -> SETTINGS -> the song home; HOME goes back to
 * the song home from any page. Every editing session is one song_io undo entry, and every record
 * pass is its own. Screens follow design/project/Studio-ClipSteps.dc.html,
 * Studio-ClipDrums.dc.html and Studio-ClipMotion.dc.html, at 1:1. */

/* the editor's pages (the view decides which: V_CLIPEDIT is notes or drums, V_CLIPMOTION is
 * motion); the clip's role decides notes vs drums. The knob kinds CLK_* come from ui.c. */
enum { CLI_NOTES = 1, CLI_DRUMS, CLI_MOTION };

#define CL_COL_LANE 22                     /* the drum grid's lane-label column */
#define CL_GRID_X 26
#define CL_GRID_Y 2
#define CL_DRUM_ROW 12
#define CL_DRUM_STEP 13
#define CL_NOTE_ROW 9
#define CL_NOTE_X 24
#define CL_NOTE_STEP 13
#define CL_MOTION_CARD_H 42
#define CL_TOAST_MS 1800u

static const uint16_t CL_COL[5] = {TOK_DRUM, TOK_BASS, TOK_LEAD, TOK_CHORD, TOK_FX};

typedef struct {
    int clip;                              /* the clip being edited, -1 none */
    int role;
    int step;                              /* the step cursor / the drum step the keys show */
    int lane;                              /* the drum grid's selected lane */
    int page;                              /* the visible 16-step page */
    int oct;                               /* the keyboard octave */
    char tag[8], msg[26];                  /* the edit toast (UNDO / MOTION ...) */
    uint32_t toast_until;
} cl_ui_t;
static cl_ui_t cl = {.clip = -1};

/* ---- the recorder -------------------------------------------------------- */
/* One pass at a time. Notes land in the clip as they are played (so the editor shows them), and
 * the length is set when the pass closes at a bar boundary. The pass is one undo entry. */
static struct {
    int active, overdub;                   /* a pass is running; it adds to an existing clip */
    int clip;
    uint32_t start;                        /* the global step the pass begins on */
    int closing;                           /* REC pressed again: close at the next bar */
    uint32_t close;
    int n;                                 /* notes recorded this pass (for the toast) */
    uint16_t touched;                      /* the params this pass recorded (for the toast) */
} rec;

/* ================================================================ the mode === */

static int clip_view(void) { return ui.view == V_CLIPEDIT || ui.view == V_CLIPMOTION; }
static int clip_is_drum(void) { return cl.clip >= 0 && song_io_clip_is_drum(cl.clip); }
int clip_edit_is_drum(void) { return clip_is_drum(); }
int clip_editing(void) { return clip_view() && cl.clip >= 0; }
int clip_rec_active(void) { return rec.active; }

static int cl_len(void) { return cl.clip >= 0 ? song_io_clip_len(cl.clip) : 16; }
static int cl_role_col(void) { return cl.role >= 0 && cl.role < 5 ? CL_COL[cl.role] : TOK_MID; }
static int cl_step_cap(void)
{
    int n = cl_len();
    return n < 1 ? 1 : n > (int)LIVE_MAX_STEPS ? (int)LIVE_MAX_STEPS : n;
}
static void cl_toast(const char *tag, const char *msg)
{
    put_s(cl.tag, tag);
    put_s(cl.msg, msg);
    cl.toast_until = plat_ms() + CL_TOAST_MS;
}
static void cl_reload(void)
{
    if (cl.clip >= 0)
        live_bridge_clip_reload((uint8_t)cl.clip);     /* T7b: re-take the playing copy */
}

/* the keyboard note a key plays on the notes page */
static int cl_note_of(int k)
{
    int n = 53 + k + 12 * cl.oct;
    return n < 0 ? 0 : n > 127 ? 127 : n;
}

/* the step the pass writes to now, quantized to the 16th (0..cap-1); -1 before it starts */
static int cl_rec_step(void)
{
    uint32_t g = live_bridge_global_step();
    int cap, s;
    if (!rec.active || g < rec.start)
        return -1;
    s = (int)(g - rec.start);
    cap = rec.overdub ? cl_len() : (int)LIVE_MAX_STEPS;
    if (cap < 1)
        cap = 1;
    return s % cap;
}
static int cl_rec_playing(void) { return rec.active && live_bridge_global_step() >= rec.start; }

/* the step within a role's playing clip, for the playhead (-1 when it is not playing this clip) */
static int cl_playhead(void)
{
    if (cl.clip < 0 || cl.role < 0)
        return -1;
    if (live_bridge_playing_clip((uint8_t)cl.role) != (uint8_t)cl.clip)
        return -1;
    return (int)live_bridge_clip_step((uint8_t)cl.role);
}

/* ============================================================= opening === */

/* end an editing session (song_io pushes one undo entry per session) */
static void cl_session_end(void)
{
    song_io_edit_end();
    rec.active = 0;
    rec.closing = 0;
    ui.rec = 0;
}

void clip_open(int clip)
{
    int role = live_bridge_role_of(clip);
    if (clip < 0 || role < 0)
        return;
    cl_session_end();
    cl.clip = clip;
    cl.role = role;
    cl.step = 0;
    cl.lane = 0;
    cl.page = 0;
    cl.oct = 0;
    cl.toast_until = 0;
    song_io_edit_begin(clip);                          /* the editor session's one undo entry */
    set_view(V_CLIPEDIT);
}

/* SEQ's cycle: EDIT -> MOTION -> SETTINGS -> (the song home) */
static void clip_next_page(void)
{
    if (ui.view == V_CLIPEDIT) {
        set_view(V_CLIPMOTION);
    } else if (ui.view == V_CLIPMOTION) {
        cl_session_end();
        set_view(V_CLIPSET);
    } else {
        set_view(V_STUDIO);
    }
    build_pages();
}

static void clip_home(void)
{
    cl_session_end();
    set_view(V_STUDIO);
    studio_enter();
}

/* ============================================================ the knob cards === */
/* ui.c's pref_get/pref_set route the R_CLIP_* refs here. `which` is CLK_*. */
static int cl_cur_note(void)
{
    int step = cl.step, note = cl_note_of(7);          /* the cursor's note, or middle-ish */
    int i, n = song_io_note_count(cl.clip);
    for (i = 0; i < n; i++) {
        int s, v;
        if (song_io_note_get(cl.clip, i, &s, &v, 0, 0) && s == step)
            return v;
    }
    return note;
}
static int cl_note_index_at_cursor(void)
{
    int i, n = song_io_note_count(cl.clip);
    for (i = 0; i < n; i++) {
        int s;
        if (song_io_note_get(cl.clip, i, &s, 0, 0, 0) && s == cl.step)
            return i;
    }
    return -1;
}
int clip_edit_get(int which)
{
    switch (which) {
    case CLK_STEP: return song_io_clip_is_drum(cl.clip) ? cl.step : cl_step_cap() ? cl.step : 0;
    case CLK_LANE: return cl.lane;
    case CLK_NOTE: return cl_cur_note();
    case CLK_VEL: {
        int i = cl_note_index_at_cursor(), v = 100;
        if (i >= 0)
            song_io_note_get(cl.clip, i, 0, 0, &v, 0);
        return v;
    }
    case CLK_LEN: {
        int i = cl_note_index_at_cursor(), v = 1;
        if (i >= 0)
            song_io_note_get(cl.clip, i, 0, 0, 0, &v);
        return v;
    }
    default: return 0;
    }
}
void clip_edit_set(int which, int v)
{
    int i;
    if (cl.clip < 0)
        return;
    switch (which) {
    case CLK_STEP: {
        int cap = cl_step_cap();
        cl.step = v < 0 ? 0 : v >= cap ? cap - 1 : v;
        break;
    }
    case CLK_LANE:
        cl.lane = v < 0 ? 0 : v >= (int)LIVE_DRUM_LANES ? (int)LIVE_DRUM_LANES - 1 : v;
        break;
    case CLK_NOTE:
        i = cl_note_index_at_cursor();
        if (i >= 0)
            song_io_note_set(cl.clip, i, -1, v, -1, -1);
        break;
    case CLK_VEL:
        i = cl_note_index_at_cursor();
        if (i >= 0)
            song_io_note_set(cl.clip, i, -1, -1, v, -1);
        break;
    case CLK_LEN:
        i = cl_note_index_at_cursor();
        if (i >= 0)
            song_io_note_set(cl.clip, i, -1, -1, -1, v);
        break;
    default: break;
    }
    cl_reload();
}

/* ============================================================ recording === */

/* the bar boundary at or after step g (a pass starts / closes on it) */
static uint32_t cl_next_bar(uint32_t g) { return ((g / LIVE_STEPS_PER_BAR) + 1u) * LIVE_STEPS_PER_BAR; }

void clip_rec_button(void)
{
    if (rec.active) {
        if (!rec.closing) {
            rec.closing = 1;
            rec.close = cl_next_bar(live_bridge_global_step());
            say("REC: CLOSING", 0);
        }
        return;
    }
    {   /* arm a pass on the current clip */
        int clip = cl.clip;
        if (clip < 0)
            return;
        cl_session_end();
        if (!live_bridge_transport_playing()) {
            live_bridge_transport_start();
            rec.start = 0;                             /* the transport starts on bar 1 */
        } else {
            rec.start = cl_next_bar(live_bridge_global_step());
        }
        rec.active = 1;
        rec.overdub = song_io_clip_is_drum(clip) ? 0 : (song_io_note_count(clip) > 0);
        rec.clip = clip;
        rec.closing = 0;
        rec.n = 0;
        rec.touched = 0;
        ui.rec = 1;
        song_io_edit_begin(clip);                      /* the pass is its own undo entry */
        say("REC", 0);
    }
}

/* a note played while a pass runs: quantized to the step under the playhead */
static void cl_rec_note(int note, int vel)
{
    int step = cl_rec_step();
    if (step < 0)
        return;
    if (song_io_note_add(rec.clip, step, note, vel, 1) >= 0)
        rec.n++;
    cl_reload();
}

static void cl_rec_commit(void)
{
    int bars, len, step_1 = 0;
    uint32_t recorded;
    if (!rec.active)
        return;
    if (rec.closing)
        step_1 = 1;
    else
        step_1 = 0;
    recorded = live_bridge_global_step() > rec.start ? live_bridge_global_step() - rec.start : 0;
    bars = (int)((recorded + LIVE_STEPS_PER_BAR - 1u) / LIVE_STEPS_PER_BAR);
    if (bars < 1)
        bars = 1;
    if (bars > 2 && bars <= 4)
        bars = 4;
    else if (bars > 1)
        bars = 2;
    if (bars > 4)
        bars = 4;
    len = bars * (int)LIVE_STEPS_PER_BAR;
    if (!rec.overdub && !song_io_clip_is_drum(rec.clip))
        song_io_clip_set_len(rec.clip, len);           /* a melodic take sets its bar count */
    song_io_edit_end();
    cl_reload();
    if (rec.n > 0)
        cl_toast(step_1 ? "REC" : "REC", step_1 ? "closed" : "recorded");
    live_bridge_launch((uint8_t)rec.clip);             /* the loop starts looping */
    rec.active = 0;
    rec.closing = 0;
    ui.rec = 0;
    song_io_edit_begin(cl.clip);                       /* back to the editor's session */
    say("RECORDED", 0);
}

void clip_tick(void)
{
    uint32_t now = plat_ms();
    if (rec.active && !clip_view()) {                  /* left the editor: end the pass */
        song_io_edit_end();
        rec.active = rec.closing = 0;
        ui.rec = 0;
    }
    if (rec.active) {
        if (!live_bridge_transport_playing()) {        /* the transport went away: end the pass */
            song_io_edit_end();
            rec.active = rec.closing = 0;
            ui.rec = 0;
        } else if (rec.closing && live_bridge_global_step() >= rec.close) {
            cl_rec_commit();
        }
    }
    if (cl.toast_until && (int32_t)(cl.toast_until - now) <= 0)
        cl.toast_until = 0;
}

/* ============================================================ the keys === */

void clip_handle_key(int k, int down)
{
    int w = KEY_WHITE[k], bl = KEY_BLACK[k];
    if (!down) {
        /* the release is done by ui.c's shared table */
        return;
    }
    if (ui.view == V_CLIPMOTION)
        return;                                        /* the keys are inert on the motion page */
    if (clip_is_drum()) {
        if (bl >= 0) {                                 /* a black key picks the lane */
            cl.lane = bl < (int)LIVE_DRUM_LANES ? bl : (int)LIVE_DRUM_LANES - 1;
            return;
        }
        if (w >= 0) {                                  /* a white key toggles the visible step */
            int step = cl.page * 16 + w;
            if (step >= cl_len())
                return;
            if (ui.btn & (1u << B_HOME)) {              /* HOME + a key toggles the accent */
                ui.btn_used |= 1u << B_HOME;
                song_io_drum_set_accent(cl.clip, step, !song_io_drum_accent(cl.clip, step));
            } else {
                song_io_drum_set(cl.clip, cl.lane, step, !song_io_drum_get(cl.clip, cl.lane, step));
            }
            cl_reload();
            cl.step = step;
        }
        return;
    }
    {   /* the notes page: the keys enter notes at the cursor */
        int note = cl_note_of(k);
        if (ui.btn & (1u << B_HOME)) {                  /* HOME + a key removes that note */
            ui.btn_used |= 1u << B_HOME;
            if (song_io_note_remove(cl.clip, cl.step, note))
                cl_reload();
            return;
        }
        if (rec.active && cl_rec_playing()) {           /* recording: write it at the playhead */
            note_key_down(k, (uint8_t)cl.role, (uint8_t)note);
            cl_rec_note(note, 100);
        } else {
            note_key_down(k, (uint8_t)cl.role, (uint8_t)note);
            song_io_note_add(cl.clip, cl.step, note, 100, 1);
            cl_reload();
        }
    }
}

/* ============================================================ the buttons === */

int clip_handle_button(int b)
{
    switch (b) {
    case B_HOME:
        clip_home();
        return 1;
    case B_SEQ:
        clip_next_page();
        return 1;
    case B_REC:
        if (ui.btn & (1u << B_HOME))
            return 0;                                  /* HOME + REC: undo, handled by ui.c */
        clip_rec_button();
        return 1;
    case B_OCTUP:
    case B_OCTDN: {
        int up = b == B_OCTUP;
        if (ui.btn & (1u << B_HOME))
            return 0;                                  /* HOME + OCT: the song home's tempo etc. */
        if (clip_is_drum()) {                          /* page the steps */
            int pages = (cl_len() + 15) / 16;
            int p = cl.page + (up ? 1 : -1);
            if (p < 0)
                p = 0;
            if (pages < 1)
                pages = 1;
            if (p > pages - 1)
                p = pages - 1;
            cl.page = p;
        } else if (!clip_is_drum()) {
            int o = cl.oct + (up ? 1 : -1);
            cl.oct = o < -3 ? -3 : o > 4 ? 4 : o;
        }
        return 1;
    }
    case B_SEL:
        if (clip_is_drum())
            cl.step = cl.page * 16 + 0;
        return 1;
    default:
        return 0;                                      /* FX / LFO / GLO / PLAY / SAVE still work */
    }
}

/* ============================================================ the encoders === */

void clip_input(void)
{
    int e, i;
    int pgi = cur_page();
    if ((e = enc(EN_ALGO)) != 0 && ui.view == V_CLIPEDIT) {   /* ALGORITHM moves the step cursor */
        int cap = cl_step_cap();
        int s = cl.step + (e > 0 ? 1 : -1);
        cl.step = s < 0 ? 0 : s >= cap ? cap - 1 : s;
        cl.page = cl.step / 16;
    }
    (void)pgi;
    for (i = 0; i < 4; i++) {
        if ((e = enc(EN_K1 + i)) == 0)
            continue;
        if (ui.view == V_CLIPMOTION) {
            clip_motion_knob(i, e);
            continue;
        }
        turn_ref(pg.r[cur_page()][i], e, EN_K1 + i);
        ui.touched = (int8_t)i;
        ui.touch_until = plat_ms() + 900u;
    }
}

/* ================================================================ motion === */

/* the four performance params the motion page's knobs write, as live_bridge.h packs them */
static uint16_t cl_motion_param(int i)
{
    switch (i) {
    case 0: return (uint16_t)LIVE_PARAM(T_MST, 0, MST_DJF);
    case 1: return (uint16_t)LIVE_PARAM(T_FX, 0, FX_RV_LEVEL);
    case 2: return (uint16_t)LIVE_PARAM(T_FX, 0, FX_DL_LEVEL);
    default: return (uint16_t)LIVE_PARAM(T_FX, 0, FX_DRIVE);
    }
}
static const char *const CL_MOTION_N[4] = {"FILTER", "SPACE", "ECHO", "DRIVE"};

void clip_motion_knob(int i, int e)
{
    int param, lane, step, val;
    pref_t r;
    if (cl.clip < 0)
        return;
    param = (int)cl_motion_param(i);
    r = PR(R_ENG, (param >> 12) & 15, (param >> 8) & 15, param & 0xFF);
    if (ui.btn & (1u << B_HOME)) {                      /* HOME + a knob clears its lane */
        ui.btn_used |= 1u << B_HOME;
        lane = song_io_motion_lane(cl.clip, param, 0);
        if (lane >= 0) {
            song_io_motion_clear(cl.clip, lane);
            cl_toast("MOTION", "cleared");
        } else {
            cl_toast("MOTION", "nothing");
        }
        cl_reload();
        ui.touched = (int8_t)i;
        ui.touch_until = plat_ms() + 900u;
        return;
    }
    turn_ref(r, e, EN_K1 + i);                          /* change the live value first */
    val = proj.sound.v[(param >> 12) & 15][(param >> 8) & 15][param & 0xFF];
    if (rec.active || ui.rec) {                         /* write motion at the current step */
        int ph = cl_playhead();
        lane = song_io_motion_lane(cl.clip, param, 1);
        if (lane < 0) {
            cl_toast("MOTION", "2 lanes max");
        } else {
            step = ph >= 0 ? ph : cl.step;
            song_io_motion_set(cl.clip, lane, step, val);
            rec.touched |= (uint16_t)(1u << lane);
        }
        cl_reload();
    }
    ui.touched = (int8_t)i;
    ui.touch_until = plat_ms() + 900u;
}

/* ================================================================ drawing === */

static int cl_vis(int y, int h, int band) { int ly = y - band * BAND_H; return ly < BAND_H && ly + h > 0; }

static void cl_draw_toast(int band)
{
    if (!cl.toast_until || band != 1)
        return;
    {
        int32_t y = 100 - band * BAND_H;
        int32_t w = tw(&AF_S, cl.tag) + 8 + tw(&AF_S, cl.msg) + 16;
        int32_t x = 120 - w / 2;
        box(x, y, w, 22, TOK_THEME);
        cv_text(x + 8, vc(&AF_S, y, 22), &AF_S, cl.tag, TOK_MID);
        cv_text(x + 8 + tw(&AF_S, cl.tag) + 8, vc(&AF_S, y, 22), &AF_S, cl.msg, TOK_INK);
    }
}

/* the notes page: a 12-row piano roll with a step cursor and a playhead */
static void cl_draw_notes(int band)
{
    int off = band * BAND_H;
    int base = 60 + 12 * cl.oct, r, s;
    int ph = cl_playhead();
    int n = song_io_note_count(cl.clip);
    for (s = 0; s < 16; s++) {                          /* the beat grid */
        int x = CL_NOTE_X + s * CL_NOTE_STEP;
        if (s % 4 == 0)
            cv_line(x - 1, -off, x - 1, 2 * BAND_H - off, TOK_RAISE);
    }
    for (r = 0; r < 12; r++) {                          /* the rows */
        int y = CL_GRID_Y + r * CL_NOTE_ROW;
        int note = base + 11 - r;
        if (!(note % 12 == 0 || note % 12 == 5))
            continue;
        if (cl_vis(y, CL_NOTE_ROW, band))
            cv_text(2, vc(&AF_S, y - off, CL_NOTE_ROW), &AF_S, NOTE_N[note % 12], TOK_MID);
    }
    for (s = 0; s < 16; s++) {                          /* the step cursor */
        int step = cl.page * 16 + s, x = CL_NOTE_X + s * CL_NOTE_STEP;
        if (step == cl.step)
            box(x - 1, -off, CL_NOTE_STEP, 2 * BAND_H, TOK_SURF);
    }
    if (ph >= 0) {                                      /* the playhead */
        int pstep = ph - cl.page * 16;
        if (pstep >= 0 && pstep < 16) {
            int x = CL_NOTE_X + pstep * CL_NOTE_STEP;
            cv_rect(x - 1, -off, 2, 2 * BAND_H, CL_COL[cl.role]);
        }
    }
    {
        int i;
        for (i = 0; i < n; i++) {                       /* the notes */
            int st, note, len;
            int x, y, w, rr;
            if (!song_io_note_get(cl.clip, i, &st, &note, 0, &len))
                continue;
            if (note < base || note >= base + 12)
                continue;
            if (st < cl.page * 16 || st >= cl.page * 16 + 16)
                continue;
            rr = base + 11 - note;
            x = CL_NOTE_X + (st - cl.page * 16) * CL_NOTE_STEP;
            y = CL_GRID_Y + rr * CL_NOTE_ROW;
            w = len * CL_NOTE_STEP - 2;
            if (w < 3)
                w = 3;
            if (w > (CL_NOTE_X + 16 * CL_NOTE_STEP) - x - 1)
                w = (CL_NOTE_X + 16 * CL_NOTE_STEP) - x - 1;
            if (cl_vis(y, CL_NOTE_ROW - 1, band))
                rbox(x, y - off, w, CL_NOTE_ROW - 1, 2, cl_role_col());
        }
    }
}

/* the drum grid: 11 lanes x the visible 16 steps */
static void cl_draw_drums(int band)
{
    int off = band * BAND_H;
    int lane, s;
    int ph = cl_playhead();
    for (lane = 0; lane < (int)LIVE_DRUM_LANES; lane++) {
        int y = CL_GRID_Y + lane * CL_DRUM_ROW;
        if (!cl_vis(y, CL_DRUM_ROW, band))
            continue;
        if (lane == cl.lane)
            box(0, y - off, 240, CL_DRUM_ROW - 1, TOK_SURF);
        cv_text(2, vc(&AF_S, y - off, CL_DRUM_ROW - 1), &AF_S,
                engine_voice_name(T_909, lane), lane == cl.lane ? CL_COL[0] : TOK_MID);
        for (s = 0; s < 16; s++) {
            int step = cl.page * 16 + s, x = CL_GRID_X + s * CL_DRUM_STEP;
            int on, acc, y0, w, h;
            uint16_t bg;
            if (step >= cl_len())
                continue;
            on = song_io_drum_get(cl.clip, lane, step);
            acc = song_io_drum_accent(cl.clip, step);
            y0 = y + 1 - off;
            w = 10;
            h = CL_DRUM_ROW - 3;
            bg = on ? CL_COL[0] : (s % 4 == 0 ? TOK_LINE : TOK_RAISE);
            rbox(x, y0, w, h, 2, bg);
            if (on && acc)
                rframe(x - 1, y0 - 1, w + 2, h + 2, 1, TOK_ACCENT);
            if (step == cl.step)
                rframe(x - 1, y0 - 1, w + 2, h + 2, 1, TOK_ACCENT);
            if (step == ph)
                box(x, y0, w, h, TOK_ACCENT);
        }
    }
}

/* the motion page: a chip row and up to two lane cards with a curve */
static void cl_motion_curve(int lane, int x, int y, int w, int h, uint16_t c)
{
    int i, n = cl_len(), px = 0, py = 0, first = 1;
    if (n < 1)
        n = 1;
    for (i = 0; i < n; i++) {
        int v = song_io_motion_get(cl.clip, lane, i);
        int cx, cy;
        if (v < 0)
            continue;
        cx = x + (i * w) / (n > 1 ? n - 1 : 1);
        cy = y + h - 1 - (v * (h - 2)) / 127;
        if (!first)
            cv_line(px, py, cx, cy, c);
        px = cx;
        py = cy;
        first = 0;
    }
}
static void cl_draw_motion(int band)
{
    int off = band * BAND_H;
    int i;
    if (cl_vis(2, 14, band))
        cv_text(6, vc(&AF_S, 2 - off, 14), &AF_S, "MOTION", TOK_MID);
    for (i = 0; i < 2; i++) {
        int param = song_io_motion_param(cl.clip, i);
        int y = 20 + i * 52;
        if (!cl_vis(y, 48, band))
            continue;
        box(4, y - off, 232, 48, param >= 0 ? TOK_RAISE : TOK_SURF);
        if (param >= 0) {
            const char *nm = "?";
            int j;
            for (j = 0; j < 4; j++)
                if ((int)cl_motion_param(j) == param)
                    nm = CL_MOTION_N[j];
            cv_text(10, vc(&AF_M, y + 3 - off, 16), &AF_M, nm, TOK_TEXT);
            cv_text(10, vc(&AF_S, y + 24 - off, 12), &AF_S, i ? "lane 2" : "lane 1", TOK_MID);
            cl_motion_curve(i, 76, y + 8 - off, 152, 32, i ? TOK_BASS : TOK_ACCENT);
        } else {
            cv_text(10, vc(&AF_S, y + 8 - off, 32), &AF_S, "+ turn a knob while REC", TOK_MID);
        }
    }
}

static void cl_draw_notes(int band);
static void cl_draw_drums(int band);
static void cl_draw_motion(int band);

void clip_draw(int band)
{
    if (ui.view == V_CLIPMOTION)
        cl_draw_motion(band);
    else if (clip_is_drum())
        cl_draw_drums(band);
    else
        cl_draw_notes(band);
    cl_draw_toast(band);
}

/* ---- the header ---- */
static int cl_name(char *out)                          /* "BASS A" */
{
    int role = cl.role >= 0 ? cl.role : 0;
    int first = live_bridge_role_first(role);
    const char *const RN[5] = {"DRUM", "BASS", "LEAD", "CHRD", "FX"};
    char *q = put_s(out, RN[role]);
    if (first >= 0 && cl.clip >= first) {
        *q++ = ' ';
        *q++ = (char)('A' + (cl.clip - first));
    }
    *q = 0;
    return role;
}
void clip_draw_header(void)
{
    char nm[16], t[16];
    int32_t x, w;
    int playing = live_bridge_transport_playing();
    w = tw(&AF_S, "EDIT") + 10;
    box(4, 3, w, 14, cl_role_col());
    cv_text(9, vc(&AF_S, 3, 14), &AF_S, "EDIT", TOK_INK);
    cl_name(nm);
    cv_text(4 + w + 6, vc(&AF_S, 3, 14), &AF_S, nm, TOK_MID);
    if (rec.active)
        cv_text(4 + w + 6 + tw(&AF_S, nm) + 5, vc(&AF_S, 3, 14), &AF_S, "REC", TOK_REC);
    x = 112;
    if (playing)
        play_icon(x, 4, rec.active ? TOK_REC : C_WHITE);
    else
        box(x + 1, 5, 8, 8, TOK_MID);
    put_i(t, (int)(live_bridge_bpm_x10() / 10u));
    cv_text(x + 14, vc(&AF_M, 1, 17), &AF_M, t, C_WHITE);
    {
        uint8_t bar = 0, beat = 0;
        char u[8], *q = u;
        live_bridge_pos(&bar, &beat, 0);
        q = put_i(q, bar + 1);
        *q++ = '.';
        put_i(q, beat + 1);
        text_r(226, vc(&AF_S, 1, 17), &AF_S, u, playing ? C_HI : C_GRAY);
    }
    if (ui.dirty || song_io_dirty())
        dot(233, 9, 1, C_GRAY);
    cv_commit(0, 0, 0);
}

/* ---- the footer ---- */
uint16_t clip_footer(const char **a, const char **b, const char **c)
{
    *a = *b = *c = 0;
    if (ui.view == V_CLIPMOTION) {
        *a = "HOME+KNOB CLEAR";
        *b = "SEQ: SETTINGS";
    } else if (clip_is_drum()) {
        *a = "BLK: LANE";
        *b = "WHT: STEP";
        *c = "HOME+WHT ACC";
    } else {
        *a = "KEYS: NOTE";
        *b = "HOME+KEY DEL";
        *c = "SEQ:MOTION";
    }
    return C_DIM;
}

/* ---- the key lights ---- */
void clip_leds(uint32_t *btn, uint32_t *key, uint32_t *dim)
{
    int k;
    (void)btn;
    if (clip_is_drum()) {
        for (k = 0; k < 11 && k < (int)LIVE_DRUM_LANES; k++)
            if (k == cl.lane)
                *key |= 1u << BLACK_KEY[k];
        for (k = 0; k < 16; k++) {
            int step = cl.page * 16 + k;
            if (step < cl_len() && song_io_drum_get(cl.clip, cl.lane, step))
                *dim |= 1u << WHITE_KEY[k];
        }
    } else if (ui.view == V_CLIPEDIT) {
        int i, n = song_io_note_count(cl.clip);
        for (i = 0; i < n; i++) {
            int st, note;
            if (!song_io_note_get(cl.clip, i, &st, &note, 0, 0))
                continue;
            if (st == cl.step) {
                int kk = note - 53 - 12 * cl.oct;
                if (kk >= 0 && kk < NKEYS)
                    *dim |= 1u << kk;
            }
        }
    }
    *key |= ui.keys;
}

/* ---- small accessors for ui.c's knob cards / the runner ---- */
int clip_cur_clip(void) { return cl.clip; }
