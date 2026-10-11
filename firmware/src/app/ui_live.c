/* SPDX-License-Identifier: GPL-3.0-only */
/* T3: the LIVE view. Included by ui.c (like undo.c) so it can draw with the same helpers; it
 * is the PADS layer (a role x clip grid on the keys) and the PLAY layer (the keys play the
 * focused role), toggled by SCL. The state itself is plain bytes; the song and the clip engine
 * belong to live_bridge.c. Screens follow design/project/Live-Pads-*.dc.html, Live-Play.dc.html,
 * Header.dc.html and PadGrid.dc.html, at 1:1. */
#include "live_bridge.h"
#include "song_io.h"                      /* T9i: the take's store calls (model-free) */

#define LIVE_HOLD_MS 600u                 /* the interaction model's ring: ~600 ms */
#define LIVE_TAKE_CLEAR_MS 1500u          /* hold REC this long to clear the take (design LIVE 8) */
#define LIVE_REC_TAP_MS 300u              /* a REC tap (arm/stop) vs a hold (clear) */

/* ---- T9i: the take, in LIVE --------------------------------------------- */
static uint8_t take_open;                 /* the take card (EDIT in LIVE) is showing */
static uint8_t rec_pressed, rec_cleared;  /* REC is down; the hold-to-clear already fired */
static uint32_t rec_t0;
static uint8_t take_prev_state;           /* the take state last frame (a finished recording) */
static uint8_t take_full_toast;           /* "take full" shown for the current recording */
static uint8_t take_knob_moved;           /* K1-K4 whose moves the take has recorded (the red dots) */

static const uint16_t LV_COL[LIVE_NROLES] = {TOK_DRUM, TOK_BASS, TOK_LEAD, TOK_CHORD, TOK_FX};
static const char *const LV_ROLE[LIVE_NROLES] = {"DR", "BA", "LE", "CH", "FX"};   /* 2 letters: clears the first cell */
static const char *const LV_ROLE_L[LIVE_NROLES] = {"DRUMS", "BASS", "LEAD", "CHORDS", "FX"};
static const char *const LV_SOUND[LIVE_NROLES] = {"909", "303 A", "303 B", "FM6", "NOISE"};

typedef struct {
    uint8_t layer;                        /* LIVE_PADS / LIVE_PLAY */
    uint8_t focus;                        /* the focused role (LIVE_*) */
    uint8_t play_down;                    /* PLAY is held */
    uint8_t holding, to_live;             /* the ring is filling; 1 = entering LIVE */
    uint8_t long_done;                    /* the 600 ms point was reached */
    uint8_t back;                         /* the view to come back to */
    uint8_t back_classic;                 /* T7a: LIVE was entered from the classic pages */
    uint32_t t0;
    int8_t octv;                          /* the PLAY layer's octave */
} live_ui_t;
static live_ui_t live = {.layer = LIVE_PADS, .focus = LIVE_BASS, .back = V_PART};

static int live_mode(void) { return ui.view == V_LIVE; }
static int live_holding(void) { return live.holding != 0; }
int live_layer(void) { return live.layer; }
int live_focus(void) { return live.focus; }

/* ---- T10i: the FX layer ------------------------------------------------- *
 * FX held (or double-tapped) turns the first six lower keys into the six punch FX and the upper
 * keys into role mutes. A key tap latches an effect, a hold is momentary. The FX button released
 * returns to the layer underneath; latched effects stay on. HOME + FX resets every effect. The
 * perform state itself lives in engine.c (the pool); the lightest UI state — which keys are down,
 * and the double-tap latch — lives here. */
#define LIVE_FX_TAP_MS 250u          /* a tap (latch) vs a hold (momentary) */
#define LIVE_FX_DBL_MS 320u          /* two FX taps inside this latch the FX layer on */
static uint8_t fx_held;              /* the FX button is down */
static uint8_t fx_layer_latch;       /* double-tapped: the FX layer stays on */
static uint32_t fx_press_t, fx_last_tap;
static uint32_t fx_key_t[6];         /* each FX key's press time */
static uint8_t fx_key_dn[6];         /* the key is down (the effect is momentary) */
static uint8_t fx_key_used[6];       /* the key went down in the FX layer: its release is ours */

static int fx_layer_active(void) { return fx_held || fx_layer_latch; }
int live_fx_layer(void) { return fx_layer_active(); }
static void live_fx_reset_ui(void)
{
    int i;
    fx_held = fx_layer_latch = 0;
    fx_press_t = fx_last_tap = 0;
    for (i = 0; i < 6; i++)
        fx_key_dn[i] = fx_key_used[i] = 0;
}

/* T7a round 2: the held-note table lives in the shared key path (ui.c), so a key's note is tracked
 * per key whatever the layer/focus/octave does, and a release (or a view/mode change) always
 * releases what that key started. */

/* ---- T9i: the take gestures --------------------------------------------- */

static int live_recording(void) { return live_bridge_take_recording() || live_bridge_take_armed(); }
static void live_len_str(char *out, uint32_t samples);

/* the clear gesture: the take is gone in RAM and queued for the next flush (stopped and idle) */
static void live_clear_take(void)
{
    song_io_take_request_clear();
    take_open = 0;
    say("TAKE CLEARED", 0);
}

/* a REC tap: arm at the next bar, or stop the recording at the next bar */
static void live_rec_tap(void)
{
    if (live_bridge_take_recording() || live_bridge_take_armed()) {
        live_bridge_take_stop();
        say("TAKE STOP", 0);
    } else {
        live_bridge_take_arm();
        say("REC", 0);
    }
}

/* REC's edges, once per frame (the press is consumed by live_handle_button; the release is ours).
 * A short tap arms/stops; holding it 1.5 s clears the take (nothing recording). */
static void live_rec_tick(uint32_t btn)
{
    uint32_t now = plat_ms();
    int down = (int)((btn >> B_REC) & 1u);
    if (!live_mode()) {
        rec_pressed = 0;
        rec_cleared = 0;
        return;
    }
    if (down && !rec_pressed) {
        if (ui.btn & (1u << B_HOME)) {          /* HOME + REC: undo, handled by button_tap */
            rec_pressed = 0;
            rec_cleared = 0;
            return;
        }
        rec_pressed = 1;
        rec_cleared = 0;
        rec_t0 = now;
    } else if (down && rec_pressed && !rec_cleared && !live_recording() &&
               (now - rec_t0) >= LIVE_TAKE_CLEAR_MS) {
        if (live_bridge_take_has()) {
            rec_cleared = 1;                    /* the ring completed: clear on the spot */
            live_clear_take();
        }
    } else if (!down && rec_pressed) {
        rec_pressed = 0;
        if (!rec_cleared && (now - rec_t0) < LIVE_TAKE_CLEAR_MS)
            live_rec_tap();
        rec_cleared = 0;
    }
}

/* once per frame: notice a recording that just finished and queue its store write; keep the
 * "take full" toast. Also reload the take card's source when nothing is pending. */
static void live_take_tick(void)
{
    int st = live_bridge_take_state();
    if (st == 2 && take_prev_state != 2)
        take_knob_moved = 0;                    /* a fresh recording: no knob marked yet */
    if ((take_prev_state == 2 /*REC*/ || take_prev_state == 1 /*ARMED*/) && st == 0 /*IDLE*/ &&
        live_bridge_take_has()) {
        song_io_take_request_save();            /* the take is the song's: written on the next flush */
        if (live_bridge_take_full())
            take_full_toast = 1;
        say(live_bridge_take_full() ? "TAKE FULL" : "TAKE SAVED", 0);
    }
    take_prev_state = (uint8_t)st;
    /* T9i/B3: no in-LIVE flash write. A finished take's store write stays pending and rides the
     * normal autosave path (stopped and idle, after leaving LIVE), so autosave's own rules hold. */
    live_bridge_take_end_pending();             /* B1: clear the ISR's natural-end flag */
    if (live_bridge_take_reload_pending() && !song_io_take_pending())
        song_io_take_load_ram();                /* NIT: an arm-then-cancel restores the song's take */
    live_bridge_take_stage();                   /* B1: stage the queued launches ahead of the event time */
}

/* how much of the take each role covers, for the card's timeline (first/last event offsets) */
static int live_take_role_span(int role, uint32_t *lo, uint32_t *hi)
{
    const uint32_t *ev = (const uint32_t *)live_bridge_take_events();   /* {sample, cmd} pairs */
    int i, n = live_bridge_take_nevents(), seen = 0;
    uint32_t l = 0xFFFFFFFFu, h = 0;
    for (i = 0; i < n; i++) {
        uint32_t off = ev[i * 2u], cmd = ev[i * 2u + 1u];
        uint32_t type = (cmd >> 28) & 0xFu, a = (cmd >> 20) & 0xFFu;
        int r = -1;
        if (type == 12u) r = (int)(cmd & 0xFFFu);            /* TAKE_LAUNCH: c = role */
        else if (type == 13u) r = (int)a;                    /* TAKE_MUTE: a = role */
        if (r != role)
            continue;
        seen = 1;
        if (off < l)
            l = off;
        if (off > h)
            h = off;
    }
    if (lo)
        *lo = l;
    if (hi)
        *hi = h;
    return seen;
}

/* ---- mode in and out ---- */

static void live_enter(void)
{
    live.back = ui.view;
    live.back_classic = ui.classic;       /* T7a: remember whether we came from the classic pages */
    live.layer = LIVE_PADS;               /* the PADS layer is the default on entry */
    live.focus = LIVE_BASS;               /* ... or whichever role launched most recently */
    live.octv = 0;
    note_release_all();                   /* T7a r2: changing mode releases every held note */
    live_fx_reset_ui();                   /* T10i: a fresh LIVE starts with no FX layer open */
    engine_perform_reset();               /* T10i: and no effect left from a previous visit */
    live_bridge_mute_all(0);              /* T10i: and no mute */
    take_open = 0;                        /* T9i: the take card is closed on entry */
    rec_pressed = rec_cleared = take_full_toast = 0;
    take_prev_state = (uint8_t)live_bridge_take_state();
    if (!song_io_take_pending())          /* an unsaved recording is the in-RAM take; keep it */
        song_io_take_load_ram();
    ui.classic = 0;                       /* LIVE is the clip engine, wherever it was entered from */
    live_bridge_enter(&proj.pat[0], 0);   /* T7a: make the clip engine the source; it does not
                                           * rebuild and does not reset the transport, so clips keep
                                           * playing across the mode change */
    set_view(V_LIVE);
    say("LIVE", 0);
}

static void live_leave(void)
{
    note_release_all();                   /* T7a D1/r2: leaving LIVE releases every held note */
    live_fx_reset_ui();                   /* T10i: the FX layer closes */
    /* T9i B2/S3: finalize the take at once and stop any playback first, then reset the perform FX
     * and mutes the take may have set (after the stop, as the review asks). */
    live_bridge_take_live(0);
    engine_perform_reset();               /* T10i: leaving LIVE releases every effect */
    live_bridge_mute_all(0);              /* T10i: and clears every mute */
    take_open = 0;
    rec_pressed = rec_cleared = 0;
    if (live.back_classic) {              /* back to the classic pages: X0X's sequencer resumes */
        ui.classic = 1;
        live_bridge_exit();
        engine_all_off();
        set_view(live.back == V_LIVE ? V_HOME : live.back);
    } else {                              /* back to the song: the clips keep playing (T7a dec. 7) */
        if (!live_bridge_transport_playing())
            engine_all_off();             /* safety: release any other held voice when stopped */
        set_view(live.back == V_LIVE ? V_STUDIO : live.back);
        if (ui.view == V_STUDIO)
            studio_enter();
    }
    say("STUDIO", 0);
}

/* ---- the PLAY button: short press on release (< 600 ms) = transport; the long press switches
 * the mode and never starts or stops either transport. ---- */

static void live_play_tick(uint32_t btn)
{
    uint32_t now = plat_ms();
    int down = (int)((btn >> B_PLAY) & 1u);
    if (down && !live.play_down) {
        live.play_down = 1;
        live.long_done = 0;
        live.holding = 1;
        live.t0 = now;
        live.to_live = (uint8_t)!live_mode();
        any_button = 1;
    } else if (down && live.holding && !live.long_done && (now - live.t0) >= LIVE_HOLD_MS) {
        live.long_done = 1;               /* the hold reached 600 ms: in or out of LIVE */
        live.holding = 0;
        ui.btn_used |= 1u << B_PLAY;
        if (live.to_live)
            live_enter();
        else
            live_leave();
    } else if (!down && live.play_down) {
        live.play_down = 0;
        live.holding = 0;
        if (!live.long_done) {            /* a short press: the transport, only on release */
            if (live_mode())
                live_handle_button(B_PLAY);
            else
                button_tap(B_PLAY);       /* transport, or HOME+PLAY redo */
        }
        live.long_done = 0;
    }
}

/* ---- the keys ---- */

/* ---- the FX layer's keys: the six effects and the role mutes ---- */

static const char *const LV_FX_NAME[6] = {"REPEAT 1/8", "REPEAT 1/16", "TAPE STOP",
                                          "FILTER DIVE", "ECHO THROW", "FREEZE"};

static void live_fx_key(int k, int down)
{
    if (down) {
        fx_key_t[k] = plat_ms();
        fx_key_dn[k] = 1;
        fx_key_used[k] = 1;
        if (!engine_perform_latched(k))
            engine_perform_engage(k, 0);          /* hold: momentary (a tap latches on release) */
    } else {
        fx_key_used[k] = 0;
        if (fx_key_dn[k]) {
            uint32_t dt = plat_ms() - fx_key_t[k];
            fx_key_dn[k] = 0;
            if (dt < LIVE_FX_TAP_MS)
                engine_perform_engage(k, 1);      /* a tap: latch it, or unlatch a latched one */
            else if (!engine_perform_latched(k))
                engine_perform_release(k);        /* a hold that ended: back off */
        }
    }
}

static void live_fx_mute(int role)
{
    int on = !live_bridge_muted((uint8_t)role);
    live_bridge_mute((uint8_t)role, on);
    say(on ? "MUTE " : "UNMUTE ", LV_ROLE_L[role]);
}

static int live_handle_key(int k, int down)
{
    /* an FX key's release is ours even if the layer closed while it was held */
    if (k >= 0 && k < 6 && fx_key_used[k] && !down) {
        live_fx_key(k, 0);
        return 1;
    }
    if (live_bridge_take_playing())
        return 1;                          /* T9i: LIVE input is ignored while the take plays */
    if (fx_layer_active()) {              /* T10i: FX held: keys 0-5 punch, upper keys mute */
        if (k >= 0 && k < 6) {
            if (down)
                live_fx_key(k, 1);
        } else if (k >= 16 && k < 16 + LIVE_NROLES) {
            if (down)
                live_fx_mute(k - 16);
        }
        return 1;                          /* the other keys are inert while FX is showing */
    }
    if (!down)
        return 1;                         /* the release is done by the shared table (ui.c) */
    if (live.layer == LIVE_PLAY) {        /* the keys play the focused role, chromatically */
        int note = 41 + k + 12 * live.octv;
        if (note < 0)
            note = 0;
        if (note > 127)
            note = 127;
        note_key_down(k, live.focus, (uint8_t)note);   /* the layer/focus may change while it is held */
        return 1;
    }
    {                                     /* PADS: each key is a clip in its zone (press only) */
        int clip = live_bridge_key_clip(k);
        int role = live_bridge_role_of(clip);
        if (clip < 0 || role < 0)
            return 1;
        live.focus = (uint8_t)role;        /* the focused role is the last one launched */
        if (live_bridge_playing_clip((uint8_t)role) == (uint8_t)clip)
            live_bridge_stop((uint8_t)role);              /* tapping the playing clip queues a stop */
        else if (ui.btn & (1u << B_HOME))
            live_bridge_launch_now((uint8_t)clip);        /* HOME + key launches now */
        else
            live_bridge_launch((uint8_t)clip);            /* at the clip's own quantize */
    }
    return 1;
}

/* ---- the buttons (LIVE owns all of them; SPEC and SELECT are inert) ---- */

static int live_handle_button(int b)
{
    if (live_bridge_take_playing()) {                      /* T9i: only STOP is live during playback */
        if (b == B_PLAY || b == B_SEL) {                   /* NIT: SEL stops the take too, as the spec */
            live_bridge_take_stop_play();
            say("TAKE STOP", 0);
        }
        return 1;
    }
    switch (b) {
    case B_PLAY:
        if (ui.btn & (1u << B_HOME)) {                     /* HOME + PLAY: redo (T9i) */
            ui.btn_used |= 1u << B_HOME;
            studio_redo();
            break;
        }
        if (live_bridge_transport_playing()) {
            live_bridge_transport_stop();
            say("STOP", 0);
        } else {
            live_bridge_transport_start();
            say("PLAY", 0);
        }
        break;
    case B_SEL:                                            /* SCL toggles the layers; on the card: play */
        if (take_open) {
            if (live_bridge_take_has()) {
                live_bridge_take_play();
                say("TAKE PLAY", 0);
            }
            break;
        }
        live.layer = (uint8_t)(live.layer == LIVE_PADS ? LIVE_PLAY : LIVE_PADS);
        say(live.layer == LIVE_PADS ? "PADS" : "PLAY", 0);
        break;
    case B_EDIT:                                           /* T9i: EDIT in LIVE opens the take card */
        take_open = (uint8_t)!take_open;
        if (take_open && !song_io_take_pending())
            song_io_take_load_ram();
        say(take_open ? "THE TAKE" : "LIVE", 0);
        break;
    case B_HOME:                                           /* close the card; LIVE has no back */
        if (take_open) {
            take_open = 0;
            say("LIVE", 0);
        }
        break;
    case B_REC:
        if (ui.btn & (1u << B_HOME)) {                     /* HOME + REC: undo the last take op */
            ui.btn_used |= 1u << B_HOME;
            studio_undo();
        }
        break;                                             /* else live_rec_tick arms/stops/clears */
    case B_OCTDN:
    case B_OCTUP: {
        int o = live.octv + (b == B_OCTUP ? 1 : -1);
        live.octv = (int8_t)(o < -3 ? -3 : o > 4 ? 4 : o);
        break;
    }
    default:
        break;                                             /* HOME, FX ...: inert in T3 */
    }
    return 1;
}

/* ---- the encoders: ALGORITHM steps the focused role; K1-K4 are the LIVE performance knobs ---- */

/* T10i: the FX button's edges. Held: the FX layer. Double-tapped: the layer latches on. A tap on
 * a held effect key latches the effect; releasing FX ends any momentary effect and returns to the
 * layer underneath (latched effects stay on, in perform's own state). HOME + FX resets. */
static void live_fx_button(void)
{
    int down = (ui.btn & (1u << B_FX)) != 0;
    uint32_t now = plat_ms();
    if (down && !fx_held) {
        fx_held = 1;
        ui.btn_used |= 1u << B_FX;                 /* the release must not also act */
        if (ui.btn & (1u << B_HOME)) {
            ui.btn_used |= 1u << B_HOME;
            engine_perform_reset();                /* HOME + FX: every effect off */
            fx_held = 0;                           /* the reset press does not open the layer */
            say("FX RESET", 0);
        } else {
            fx_press_t = now;                      /* the header chip shows the layer; no toast (T10i review) */
        }
    } else if (!down && fx_held) {
        uint32_t dt = now - fx_press_t;
        int i;
        fx_held = 0;
        for (i = 0; i < 6; i++)                    /* a held (momentary) effect ends with the layer */
            if (fx_key_dn[i]) {
                if (!engine_perform_latched(i))
                    engine_perform_release(i);
                fx_key_dn[i] = 0;
            }
        if (dt < LIVE_FX_TAP_MS) {                 /* a tap */
            if (now - fx_last_tap < LIVE_FX_DBL_MS) {
                fx_layer_latch = (uint8_t)!fx_layer_latch;   /* a double tap latches the layer */
                fx_last_tap = 0;                   /* the header chip shows it; no toast */
            } else {
                fx_last_tap = now;
            }
        }
    }
}

static void live_input(void)
{
    int i, e, pgi = cur_page();
    if (live_bridge_take_playing() || take_open)
        return;                            /* T9i: the take card and playback take no encoders */
    live_fx_button();
    for (i = 0; i < NE; i++) {
        e = (int)enc(i);
        if (i == EN_ALGO && e) {
            int f = live.focus + (e > 0 ? 1 : -1);
            live.focus = (uint8_t)((f + LIVE_NROLES) % LIVE_NROLES);
        } else if (i >= EN_K1 && i <= EN_K4 && e) {
            turn_ref(pg.r[pgi][i - EN_K1], e, i);     /* FILTER / SPACE / ECHO / DRIVE */
            ui.touched = (int8_t)(i - EN_K1);
            ui.touch_until = plat_ms() + 900u;
            if (live_bridge_take_recording())
                take_knob_moved |= (uint8_t)(1u << (i - EN_K1));   /* the red "recorded" dot */
        }
    }
}

/* ---- the LEDs: off (empty), dim (loaded), on (playing), fast blink (queued to start),
 * slow blink (queued to stop). SCL is lit on the PLAY layer. ---- */

static void live_leds(uint32_t *btn, uint32_t *key, uint32_t *dim)
{
    int k;
    if (live_bridge_transport_playing())
        *btn |= 1u << B_PLAY;
    if (fx_layer_active()) {                           /* T10i: the six punches and the role mutes */
        int i;
        for (i = 0; i < 6; i++) {
            if (engine_perform_latched(i) || fx_key_dn[i])
                *key |= 1u << i;                       /* active: latched or held */
            else
                *dim |= 1u << i;
        }
        for (i = 0; i < LIVE_NROLES; i++) {
            if (live_bridge_muted((uint8_t)i))
                *key |= 1u << (16 + i);                /* the upper keys: a muted role lit */
            else
                *dim |= 1u << (16 + i);
        }
        return;
    }
    if (live.layer == LIVE_PLAY)
        *btn |= 1u << B_SEL;                               /* the SCL LED: off = PADS, lit = PLAY */
    for (k = 0; k < NKEYS; k++) {
        int clip, role, play, q, qstart, qstop;
        if (live.layer == LIVE_PLAY) {                     /* the keyboard: its Cs dim, to find your place */
            if (k % 12 == 7)
                *dim |= 1u << k;
            continue;
        }
        clip = live_bridge_key_clip(k);
        role = live_bridge_role_of(clip);
        if (role < 0 || !live_bridge_clip_used(clip))
            continue;                                      /* empty slot: off */
        play = live_bridge_playing_clip((uint8_t)role) == clip;
        q = live_bridge_queued_clip((uint8_t)role);
        qstart = (q == clip);
        qstop = play && live_bridge_stop_queued(role);     /* a stop on the playing clip is pending */
        if (qstop) {
            if (ui.frame & 32u)                            /* slow blink */
                *key |= 1u << k;
            else
                *dim |= 1u << k;
        } else if (qstart) {
            if (ui.frame & 4u)                             /* fast blink */
                *key |= 1u << k;
        } else if (play) {
            *key |= 1u << k;                               /* on */
        } else {
            *dim |= 1u << k;                               /* loaded, stopped */
        }
    }
}

/* ---- the header ---- */

static void live_draw_header(void)
{
    char t[16];
    int32_t x, w;
    int playing = live_bridge_transport_playing();
    w = tw(&AF_S, "LIVE") + 10;
    box(4, 3, w, 14, TOK_THEME);
    cv_text(4 + 5, vc(&AF_S, 3, 14), &AF_S, "LIVE", C_BLACK);
    if (live_bridge_take_recording()) {                /* T9i: a red REC chip while recording */
        int32_t rw = tw(&AF_S, "REC") + 10;
        box(4 + w + 6, 3, rw, 14, TOK_REC);
        cv_text(4 + w + 11, vc(&AF_S, 3, 14), &AF_S, "REC", C_BLACK);
    } else {
        cv_text(4 + w + 6, vc(&AF_S, 3, 14), &AF_S,
                help_shift_active() ? "SHIFT" : take_open ? "TAKE" : fx_layer_active() ? "FX"
                                   : (live.layer == LIVE_PADS ? "PADS" : "PLAY"),
                (take_open || fx_layer_active()) && !help_shift_active() ? TOK_ACCENT : TOK_MID);
    }
    x = 104;
    if (playing)
        play_icon(x, 4, C_WHITE);
    else
        box(x + 1, 5, 8, 8, TOK_MID);
    put_i(t, (int)(live_bridge_bpm_x10() / 10u));
    cv_text(x + 14, vc(&AF_M, 1, 17), &AF_M, t, C_WHITE);
    if (live_bridge_take_recording()) {                /* T9i: the elapsed bars while recording */
        char u[10], *q = put_s(u, "+");
        q = put_i(q, live_bridge_take_rec_bars());
        put_s(q, "bar");
        text_r(226, vc(&AF_S, 1, 17), &AF_S, u, TOK_REC);
    } else {
        uint8_t bar = 0, beat = 0;
        char u[8], *q = u;
        live_bridge_pos(&bar, &beat, 0);
        q = put_i(q, bar + 1);
        *q++ = '.';
        put_i(q, beat + 1);
        text_r(226, vc(&AF_S, 1, 17), &AF_S, u, playing ? C_HI : C_GRAY);
    }
    if (ui.dirty)
        dot(233, 9, 1, C_GRAY);
    cv_commit(0, 0, 0);
}

/* ---- the footer ---- */

static uint16_t live_footer(const char **a, const char **b, const char **c)
{
    *a = *b = *c = 0;
    if (live_bridge_take_recording() || live_bridge_take_armed()) {
        *a = "REC STOP";
        *b = "hold REC: CLEAR";
        return TOK_REC;
    }
    if (live_bridge_take_playing()) {
        *a = "PLAY: STOP";
        return TOK_REC;
    }
    if (take_open) {
        *a = "SEL: PLAY";
        *b = "HOLD REC: CLEAR";
        *c = "EDIT: BACK";
        return C_DIM;
    }
    if (fx_layer_active()) {
        *a = "tap latch hold moment";
        *b = "HOME+FX reset";
    } else if (live.layer == LIVE_PLAY) {
        static char t[24];
        char *q;
        q = put_s(t, "keys play ");
        put_s(q, LV_ROLE_L[live.focus]);
        *a = t;
        *b = "SCL pads";
    } else {
        *a = "SCL play";
        *b = "hold PLAY exit";
    }
    return C_DIM;
}

/* ---- the PADS layer: a role x clip grid ---- */

#define LV_ROW_Y(i) (2 + (i) * 27)
#define LV_CELL_X(j) (48 + (j) * 26)
#define LV_CELL 24

static void live_draw_pads(int band)
{
    int r, j;
    for (r = 0; r < LIVE_NROLES; r++) {
        int first = live_bridge_role_first(r), n = live_bridge_role_nclips(r);
        int gy = LV_ROW_Y(r), ly = gy - band * BAND_H;
        if (first < 0 || ly >= BAND_H || ly + 25 <= 0)
            continue;
        if (r == live.focus) {
            box(2, ly, 236, 25, TOK_SURF);
            play_icon(6, ly + 8, LV_COL[r]);               /* the focus arrow */
        }
        cv_text(18, vc(&AF_S, ly, 25), &AF_S, LV_ROLE[r], LV_COL[r]);
        for (j = 0; j < n && j < 7; j++) {
            int clip = first + j, cx = LV_CELL_X(j);
            int used, playing, q, qstart, qstop;
            char L[2];
            uint16_t bg, fg;
            int outline = 0;
            if (cx + LV_CELL > 238)
                break;
            used = live_bridge_clip_used(clip);
            L[0] = (char)('A' + j);
            L[1] = 0;
            if (!used) {                                   /* empty: hollow */
                rframe(cx, ly, LV_CELL, LV_CELL, 3, C_DIM);
                continue;
            }
            playing = live_bridge_playing_clip((uint8_t)r) == clip;
            q = live_bridge_queued_clip((uint8_t)r);
            qstart = (q == clip);
            qstop = playing && live_bridge_stop_queued(r);
            if (qstart && (ui.frame & 4u)) {               /* queued to start: blinking outline */
                outline = 1;
                bg = C_BLACK;
                fg = LV_COL[r];
            } else if (qstop && (ui.frame & 32u)) {        /* queued to stop: slow blink */
                bg = dim(LV_COL[r], 8);
                fg = LV_COL[r];
            } else if (playing) {
                bg = LV_COL[r];
                fg = C_BLACK;
            } else {
                bg = dim(LV_COL[r], 4);
                fg = LV_COL[r];
            }
            if (outline)
                frame(cx, ly, LV_CELL, LV_CELL, LV_COL[r]);
            else
                box(cx, ly, LV_CELL, LV_CELL, bg);
            text_c(cx + LV_CELL / 2, vc(&AF_S, ly, LV_CELL), &AF_S, L, fg);
            if (playing) {                                 /* the clip's progress */
                int p = live_bridge_progress((uint8_t)r);
                rbox(cx + 3, ly + LV_CELL - 4, (LV_CELL - 6) * p / 255, 2, 1, C_BLACK);
            }
        }
    }
}

/* ---- the PLAY layer: the focused role and the octave ---- */

static void live_draw_play(int band)
{
    int r = live.focus;
    if (band == 0) {
        char t[24], *q;
        cv_text(8, vc(&AF_L, 2, 32), &AF_L, LV_ROLE_L[r], LV_COL[r]);
        text_r(232, vc(&AF_S, 8, 14), &AF_S, "ALGO: role", TOK_MID);
        q = put_s(t, "ENGINE ");
        put_s(q, LV_SOUND[r]);
        cv_text(8, vc(&AF_M, 36, 18), &AF_M, t, TOK_TEXT);
        q = put_s(t, "octave ");
        put_i(q, live.octv);
        cv_text(8, vc(&AF_S, 54, 14), &AF_S, t, TOK_MID);   /* fully inside the band */
    } else {
        int i;
        for (i = 0; i < LIVE_NROLES; i++) {                /* the clip-status strip */
            int first = live_bridge_role_first(i);
            int play = live_bridge_playing_clip((uint8_t)i);
            int x = 4 + i * 46, w = 44, ly = 96 - band * BAND_H;
            uint16_t bg, fg;
            char L[2];
            if (play != 0xFF && first >= 0) {
                bg = LV_COL[i];
                fg = C_BLACK;
                L[0] = (char)('A' + (play - first));
                L[1] = 0;
            } else {
                bg = TOK_RAISE;
                fg = TOK_MID;
                L[0] = '-';
                L[1] = 0;
            }
            box(x, ly, w, 30, bg);
            text_c(x + w / 2, vc(&AF_S, ly + 1, 12), &AF_S, LV_ROLE[i], fg);
            text_c(x + w / 2, vc(&AF_M, ly + 13, 16), &AF_M, L, fg);
        }
    }
}

/* ---- the FX layer: the K1-K4 strip above, the six punch cells below (LIVE 6) ---- */

/* The design puts the performance knobs above the grid; the app's own strip is the bottom band, so
 * the FX view draws its own compact K1-K4 row at the top of the main area (the generic cards are
 * suppressed for this layer, see draw_knobs). */
static void live_fx_knobs(void)
{
    int i, pgi = cur_page();
    for (i = 0; i < 4; i++) {
        pref_t r = pg.r[pgi][i];
        const x0x_param_t *d = pref_desc(r);
        char num[16], unit[8];
        int cx = 3 + i * 59, val;
        if (!d)
            continue;
        box(cx, 3, 57, 44, TOK_SURF);
        text_c(cx + 28, vc(&AF_S, 6, 10), &AF_S, caps_of(pref_name(r)), TOK_MID);
        val = pref_get(r);
        pref_value_of(r, val, num, unit);
        cell_value(cx + 28, vc(&AF_M, 19, 17), num, unit, C_HI);
        box(cx + 4, 42, 49, 3, C_LINE);
        if (d->max && val > 0)
            box(cx + 4, 42, val * 49 / d->max, 3, TOK_THEME);
    }
}

static void live_draw_fx(int band)
{
    int i;
    if (band == 0) {                        /* the knob strip, above the grid, as the design */
        live_fx_knobs();
        return;
    }
    for (i = 0; i < 6; i++) {                /* three columns, two rows */
        int lat = engine_perform_latched(i);
        int hold = fx_key_dn[i];
        int act = lat || hold;
        int cx = 3 + (i % 3) * 79, cy = 1 + (i / 3) * 34, w = 76, h = 32;
        uint16_t bg = !act ? TOK_SURF : lat ? TOK_THEME : TOK_ACCENT;   /* solid fills; ink text */
        uint16_t fg = act ? C_BLACK : TOK_TEXT;
        box(cx, cy, w, h, bg);
        frame(cx, cy, w, h, act ? bg : TOK_LINE);
        text_c(cx + w / 2, vc(&AF_S, cy + 4, 12), &AF_S, LV_FX_NAME[i], fg);
        text_c(cx + w / 2, vc(&AF_S, cy + 18, 11), &AF_S,
               lat ? "LATCH" : hold ? "HOLD" : "OFF", act ? C_BLACK : TOK_MID);
    }
}

/* ---- T9i: the take card (LIVE 10) and the recording border ---- */

static void live_len_str(char *out, uint32_t samples)
{
    uint32_t sec = samples / (uint32_t)FS, s = sec % 60u;
    char *q;
    q = put_i(out, (int)(sec / 60u));
    *q++ = ':';
    *q++ = (char)('0' + (s / 10u));
    put_i(q, (int)(s % 10u));
}

static void live_draw_take(int band)
{
    int i;
    if (band == 0) {
        char len[10], u[20];
        box(4, 2, 232, 100, TOK_SURF);
        frame(4, 2, 232, 100, TOK_LINE);
        cv_text(12, vc(&AF_M, 5, 16), &AF_M, "This song's take", TOK_TEXT);
        if (!live_bridge_take_has()) {
            cv_text(12, vc(&AF_S, 30, 12), &AF_S, "no take yet", TOK_MID);
            cv_text(12, vc(&AF_S, 46, 12), &AF_S, "REC records one from the next bar", TOK_MID);
            return;
        }
        live_len_str(len, live_bridge_take_length_samples());
        {
            char *q = put_s(u, len);
            q = put_s(q, " - ");
            q = put_i(q, live_bridge_take_bars());
            put_s(q, " bars");
            text_r(228, vc(&AF_S, 5, 14), &AF_S, u, TOK_MID);
        }
        for (i = 0; i < LIVE_NROLES; i++) {                  /* role lanes (launches and mutes) */
            uint32_t lo = 0, hi = 0, len2 = live_bridge_take_length_samples();
            int y = 24 + i * 13, on = live_take_role_span(i, &lo, &hi);
            box(12, y, 216, 9, TOK_RAISE);
            if (on && len2) {
                int x0 = 12 + (int)(216u * lo / len2);
                int x1 = 12 + (int)(216u * hi / len2);
                if (x1 <= x0)
                    x1 = x0 + 2;
                if (x0 < 12) x0 = 12;
                if (x1 > 228) x1 = 228;
                box(x0, y, x1 - x0, 9, LV_COL[i]);
            }
        }
        cv_text(12, vc(&AF_S, 92, 10), &AF_S, "lanes: launches and mutes", TOK_MID);
    } else {
        box(4, 2, 232, 60, TOK_SURF);
        frame(4, 2, 232, 60, TOK_LINE);
        box(6, 6, 106, 18, TOK_THEME);
        cv_text(12, vc(&AF_M, 6, 18), &AF_M, "Play the take", TOK_INK);
        text_r(228, vc(&AF_S, 8, 14), &AF_S, live_bridge_take_playing() ? "playing" : "SEL", TOK_MID);
        cv_text(12, vc(&AF_M, 28, 15), &AF_M, "Clear", TOK_TEXT);
        text_r(228, vc(&AF_S, 28, 15), &AF_S, "hold REC", TOK_MID);
        cv_text(12, vc(&AF_S, 46, 11), &AF_S, "recording again replaces it - undo restores", TOK_MID);
    }
}

static void live_draw_rec_border(void)
{
    frame(0, 0, 240, 2 * BAND_H, TOK_REC);
    frame(1, 1, 238, 2 * BAND_H - 2, TOK_REC);
}

/* the clear-take ring (LIVE 8): filling while REC is held, nothing recording */
int live_clear_holding(void)
{
    return rec_pressed && !rec_cleared && !live_recording() && live_bridge_take_has() &&
           (plat_ms() - rec_t0) < LIVE_TAKE_CLEAR_MS;
}
int live_take_knob_moved(int i) { return i >= 0 && i < 4 && (take_knob_moved >> i & 1u); }
int live_take_open(void) { return take_open != 0; }

static void live_draw_clear_ring(void)
{
    uint32_t el = plat_ms() - rec_t0;
    float p = (float)el / (float)LIVE_TAKE_CLEAR_MS;
    char len[10], sum[20], *q;
    if (p > 1.0f)
        p = 1.0f;
    box(0, 0, 240, 2 * BAND_H, TOK_BG);
    box(40, 26, 160, 90, TOK_SURF);
    frame(40, 26, 160, 90, TOK_LINE);
    disc(74.0f, 71.0f, 27.0f, TOK_LINE);
    disc(74.0f, 71.0f, 22.0f, TOK_SURF);
    arc(74, 71, 24, p, TOK_LINE, TOK_REC, 0);
    cv_text(112, vc(&AF_S, 34, 12), &AF_S, "clear", TOK_REC);
    cv_text(112, vc(&AF_M, 50, 16), &AF_M, "This song's take", TOK_TEXT);
    live_len_str(len, live_bridge_take_length_samples());
    q = put_s(sum, len);
    put_s(q, " long");
    cv_text(112, vc(&AF_S, 70, 11), &AF_S, sum, TOK_MID);
    text_c(120, vc(&AF_S, 96, 11), &AF_S, "clips stay - music keeps playing", TOK_MID);
}

static void live_draw(int band)
{
    if (take_open)
        live_draw_take(band);
    else if (fx_layer_active())
        live_draw_fx(band);
    else if (live.layer == LIVE_PLAY)
        live_draw_play(band);
    else
        live_draw_pads(band);
}

/* ---- the PLAY hold ring (drawn over whatever view is showing) ---- */

static void live_draw_ring(void)
{
    uint32_t el = plat_ms() - live.t0;
    float p = (float)el / (float)LIVE_HOLD_MS;
    if (p > 1.0f)
        p = 1.0f;
    box(70, 32, 100, 74, TOK_SURF);
    frame(70, 32, 100, 74, C_LINE);
    arc(120, 60, 22, p, C_LINE, live.to_live ? TOK_THEME : TOK_ACCENT, 0);
    text_c(120, vc(&AF_S, 88, 12), &AF_S, live.to_live ? "LIVE" : "STUDIO", C_WHITE);
}
