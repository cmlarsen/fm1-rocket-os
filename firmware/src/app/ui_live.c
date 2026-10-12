/* SPDX-License-Identifier: GPL-3.0-only */
/* T3: the LIVE view. Included by ui.c (like undo.c) so it can draw with the same helpers; it
 * is the PADS layer (a role x clip grid on the keys) and the PLAY layer (the keys play the
 * focused role), toggled by SEL. The state itself is plain bytes; the song and the clip engine
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
static uint8_t fx_held;              /* the FX layer is showing because FX is down */
static uint8_t fx_press;             /* the FX button is physically down (even when not ours) */
static uint8_t fx_consumed;          /* TFIXU 2/5: this press belongs to help or a reset: its release is inert */
static uint8_t fx_layer_latch;       /* double-tapped: the FX layer stays on */
static uint32_t fx_press_t, fx_last_tap;
static uint32_t fx_key_t[6];         /* each FX key's press time */
static uint8_t fx_key_dn[6];         /* the key is down (the effect is momentary) */
static uint8_t fx_key_used[6];       /* the key went down in the FX layer: its release is ours */
static uint8_t fx_owner_key[6];      /* the key that engaged each effect, plus one (0 = none) */

static int fx_layer_active(void) { return fx_held || fx_layer_latch; }
int live_fx_layer(void) { return fx_layer_active(); }

/* TFIXU 4: the six punch FX are on the first six LOWER keys and the role mutes on the first five
 * UPPER keys. Raw key indexes are chromatic, so translate by physical position (WHITE_KEY/
 * BLACK_KEY, ui.c). This is the one mapping the input and the LED paths share. */
static int live_key_white(int k) { int i; for (i = 0; i < 16; i++) if (WHITE_KEY[i] == k) return i; return -1; }
static int live_key_black(int k) { int i; for (i = 0; i < 11; i++) if (BLACK_KEY[i] == k) return i; return -1; }
static int live_fx_slot(int k) { int p = live_key_white(k); return (p >= 0 && p < 6) ? p : -1; }
static int live_fx_role(int k) { int p = live_key_black(k); return (p >= 0 && p < LIVE_NROLES) ? p : -1; }

/* TFIXU 2: while GLO is held (or its card is up) a button belongs to help, not the instrument:
 * its edges and timers must not act, and its release is consumed. btn is the frame's buttons.
 * (The GLO-held test also covers the frame help_glo_press claims the button: that runs after the
 * timers, but GLO is already down.) */
static int live_help_owns(uint32_t btn)
{
    return (btn & (1u << B_GLO)) != 0 || help_card_active();
}
static void live_fx_release_held(void)
{
    int i;
    for (i = 0; i < 6; i++)
        if (fx_key_dn[i]) {
            if (!engine_perform_latched(i))
                engine_perform_release(i);
            fx_key_dn[i] = 0;
        }
}

static void live_fx_reset_ui(void)
{
    int i;
    live_fx_release_held();
    fx_held = fx_layer_latch = fx_press = fx_consumed = 0;
    fx_press_t = fx_last_tap = 0;
    for (i = 0; i < 6; i++)
        fx_key_dn[i] = fx_key_used[i] = fx_owner_key[i] = 0;
}

/* T7a round 2: the held-note table lives in the shared key path (ui.c), so a key's note is tracked
 * per key whatever the layer/focus/octave does, and a release (or a view/mode change) always
 * releases what that key started. */

/* ---- T9i: the take gestures --------------------------------------------- */

static void live_setup_close(void);
static void live_mapped_release_all(void);
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
    if (live_help_owns(btn)) {               /* TFIXU 2: GLO + REC shows a card; the take is not
                                              * armed, stopped or cleared, and the press is consumed */
        rec_pressed = 1;                     /* keep the release inert too */
        rec_cleared = 1;
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
        if (type == 12u) r = (int)(cmd & 0xFFu);             /* TAKE_LAUNCH: c = role | quant<<8 */
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
    live.layer = LIVE_PADS;               /* the PADS layer is the default on entry */
    live.focus = LIVE_BASS;               /* ... or whichever role launched most recently */
    live.octv = 0;
    note_release_all();                   /* T7a r2: changing mode releases every held note */
    live_fx_reset_ui();                   /* T10i: a fresh LIVE starts with no FX layer open */
    engine_perform_reset();               /* T10i: and no effect left from a previous visit */
    live_bridge_mute_all(0);              /* T10i: and no mute */
    live_setup_close();                  /* TLSET: a fresh LIVE has no setup open */
    rec_pressed = rec_cleared = take_full_toast = 0;
    take_prev_state = (uint8_t)live_bridge_take_state();
    if (!song_io_take_pending())          /* an unsaved recording is the in-RAM take; keep it */
        song_io_take_load_ram();
    live_bridge_enter();                  /* the clip engine is already the source; it does not
                                           * reset the transport, so clips keep playing */
    set_view(V_LIVE);
    say("LIVE", 0);
}

static void live_leave(void)
{
    live_mapped_release_all();
    live_setup_close();
    note_release_all();                   /* T7a D1/r2: leaving LIVE releases every held note */
    live_fx_reset_ui();                   /* T10i: the FX layer closes */
    /* T9i B2/S3: finalize the take at once and stop any playback first, then reset the perform FX
     * and mutes the take may have set (after the stop, as the review asks). */
    live_bridge_take_live(0);
    engine_perform_reset();               /* T10i: leaving LIVE releases every effect */
    live_bridge_mute_all(0);              /* T10i: and clears every mute */
    take_open = 0;
    rec_pressed = rec_cleared = 0;
    if (!live_bridge_transport_playing())
        engine_all_off();                 /* safety: release any other held voice when stopped */
    set_view(live.back == V_LIVE ? V_STUDIO : live.back);
    if (ui.view == V_STUDIO)
        studio_enter();
    say("STUDIO", 0);
}

/* ---- the PLAY button: short press on release (< 600 ms) = transport; the long press switches
 * the mode and never starts or stops either transport. ---- */

static void live_play_tick(uint32_t btn)
{
    uint32_t now = plat_ms();
    int down = (int)((btn >> B_PLAY) & 1u);
    if (live_help_owns(btn)) {           /* TFIXU 2: GLO + PLAY shows a card, never the transport */
        live.play_down = (uint8_t)down;
        live.holding = 0;
        live.long_done = 1;              /* consume the release: no short press, no mode switch */
        return;
    }
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

static void live_fx_key(int k, int down, int owner)
{
    if (down) {
        fx_key_t[k] = plat_ms();
        fx_key_dn[k] = 1;
        fx_key_used[k] = 1;
        fx_owner_key[k] = (uint8_t)(owner + 1);
        if (!engine_perform_latched(k))
            engine_perform_engage(k, 0);          /* hold: momentary (a tap latches on release) */
    } else {
        fx_key_used[k] = 0;
        fx_owner_key[k] = 0;
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

/* Mapped FX retain the originating key across layer and overlay changes. */
static uint8_t mapped_fx[27];             /* effect + 1, zero = none */
static void live_mapped_release_all(void)
{
    int k;
    for (k = 0; k < 27; k++) {
        if (mapped_fx[k]) {
            int f = mapped_fx[k] - 1;
            if (!engine_perform_latched(f)) engine_perform_release(f);
            fx_key_dn[f] = fx_key_used[f] = fx_owner_key[f] = 0;
            mapped_fx[k] = 0;
        }
    }
}
#include "ui_live_setup.c"

/* TFIXU 5 / ui 9 (owner decision, 2026-10-11): the PLAY keyboard is a scale-degree keyboard in
 * the song's key and scale. Key k steps up the scale (wrapping into the next octave when it passes
 * the last degree); the tonic sits at 12*(3+octv) + the song key, i.e. C2 for C major. The mapped
 * Live-note keys in PADS use the same mapping. */
static const char *const LV_KEY[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static const char *const LV_SCALE[5] = {"major", "minor", "dorian", "penta", "chromatic"};
static int live_scale_step(int k, int scale)
{
    static const int8_t IV[5][12] = {
        {0, 2, 4, 5, 7, 9, 11, 0, 0, 0, 0, 0},    /* major */
        {0, 2, 3, 5, 7, 8, 10, 0, 0, 0, 0, 0},    /* minor */
        {0, 2, 3, 5, 7, 9, 10, 0, 0, 0, 0, 0},    /* dorian */
        {0, 2, 4, 7, 9, 0, 0, 0, 0, 0, 0, 0},     /* penta */
        {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11},   /* chromatic */
    };
    static const int8_t LEN[5] = {7, 7, 7, 5, 12};
    int s = scale < 0 ? 0 : scale > 4 ? 4 : scale;
    return 12 * (k / LEN[s]) + IV[s][k % LEN[s]];
}
static int live_song_scale(void)
{
    int s = song_io_scale();
    return s < 0 ? 0 : s > 4 ? 4 : s;
}
/* key k's note at octave octv in `key`/`scale` (live_play_note_for's core; the host test reads
 * it too). r1: a note above 127 (or below 0) folds by whole octaves, so a high octave keeps
 * every key on its own in-range pitch instead of piling the top keys onto 127. */
int live_play_note_scale(int k, int octv, int key, int scale)
{
    int note = 12 * (3 + octv) + key + live_scale_step(k, scale);
    while (note > 127)
        note -= 12;
    while (note < 0)
        note += 12;
    return note;
}
/* key k at the current octave, in the song's key/scale (the host test reads this too) */
int live_play_note_for(int k, int octv)
{
    return live_play_note_scale(k, octv, song_io_key(), live_song_scale());
}

static int live_handle_key(int k, int down)
{
    if (!down && mapped_fx[k]) {
        int f = mapped_fx[k] - 1, j, held = 0;
        mapped_fx[k] = 0;
        for (j = 0; j < 27; j++) if (mapped_fx[j] == f + 1) held = 1;
        if (!held) live_fx_key(f, 0, k);
        return 1;
    }
    /* an FX key's release is ours even if the layer closed while it was held */
    if (!down) {
        int f = live_fx_slot(k);
        if (f >= 0 && fx_key_used[f] && fx_owner_key[f] == (uint8_t)(k + 1)) {
            live_fx_key(f, 0, k);
            return 1;
        }
    }
    if (live_setup_key(k, down)) return 1;
    if (live_bridge_take_playing())
        return 1;                          /* T9i: LIVE input is ignored while the take plays */
    if (fx_layer_active()) {              /* T10i: FX held: the first six lower keys punch, the
                                           * first five upper keys mute (TFIXU 4: by position) */
        int f = live_fx_slot(k);
        if (f >= 0) {
            if (down)
                live_fx_key(f, 1, k);
        } else {
            int r = live_fx_role(k);
            if (r >= 0 && down)
                live_fx_mute(r);
        }
        return 1;                          /* the other keys are inert while FX is showing */
    }
    if (!down)
        return 1;                         /* the release is done by the shared table (ui.c) */
    if (live.layer == LIVE_PLAY) {        /* the keys play the focused role, in the song's scale */
        note_key_down(k, live.focus, (uint8_t)live_play_note_for(k, live.octv));
        return 1;                          /* the layer/focus may change while it is held */
    }
    {                                     /* PADS: each key is a clip in its zone (press only) */
        int clip = live_bridge_key_clip(k);
        int role;
        if (clip >= LIVE_ASSIGN_STOP && clip < LIVE_ASSIGN_FX) {
            live_bridge_stop((uint8_t)(clip - LIVE_ASSIGN_STOP));
            return 1;
        }
        if (clip >= LIVE_ASSIGN_FX && clip < LIVE_ASSIGN_NOTE) {
            int f = clip - LIVE_ASSIGN_FX, j, held = 0;
            for (j = 0; j < 27; j++) if (mapped_fx[j] == f + 1) held = 1;
            mapped_fx[k] = (uint8_t)(f + 1);
            if (!held) live_fx_key(f, 1, k);
            return 1;
        }
        if (clip == LIVE_ASSIGN_NOTE) {
            note_key_down(k, live.focus, (uint8_t)live_play_note_for(k, live.octv));
            return 1;                      /* a mapped Live note: the same key/scale mapping */
        }
        role = live_bridge_role_of(clip);
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
    if (!live_bridge_take_playing() && live_setup_button(b)) return 1;
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
            engine_all_off();                      /* TCLS: STOP releases every voice, as X0X's did */
            say("STOP", 0);
        } else {
            live_bridge_transport_start();
            say("PLAY", 0);
        }
        break;
    case B_SEL:                                            /* SEL toggles the layers; on the card: play */
        if (take_open) {
            if (live_bridge_take_has()) {
                live_bridge_take_play();
                say("TAKE PLAY", 0);
            }
            break;
        }
        live_mapped_release_all();
        note_release_all();
        live.layer = (uint8_t)(live.layer == LIVE_PADS ? LIVE_PLAY : LIVE_PADS);
        say(live.layer == LIVE_PADS ? "PADS" : "PLAY", 0);
        break;
    case B_EDIT:
        live_mapped_release_all();
        live_setup_enter();
        say("LIVE SETUP", 0);
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
    if (!down) {
        if (fx_press) {                            /* the FX release */
            uint32_t dt = now - fx_press_t;
            int consumed = fx_consumed;
            fx_press = fx_consumed = 0;                /* r1: clear the static too, not just the local */
            if (fx_held) {                         /* a press that opened the layer */
                fx_held = 0;
                live_fx_release_held();            /* a held (momentary) effect ends with the layer */
                if (!consumed && dt < LIVE_FX_TAP_MS) {      /* a tap */
                    if (now - fx_last_tap < LIVE_FX_DBL_MS) {
                        fx_layer_latch = (uint8_t)!fx_layer_latch;   /* a double tap latches the layer */
                        fx_last_tap = 0;       /* the header chip shows it; no toast */
                        if (fx_layer_latch)
                            note_release_all();          /* TFIXU 6: latching the layer releases notes */
                    } else {
                        fx_last_tap = now;
                    }
                }
            }
        }
        return;
    }
    if (fx_press)
        return;                                    /* only the down edge acts */
    fx_press = 1;
    fx_press_t = now;
    if (live_help_owns(ui.btn)) {                  /* TFIXU 2: GLO + FX shows a card, no layer */
        fx_consumed = 1;                           /* the release must not act */
        return;
    }
    if (ui.btn & (1u << B_HOME)) {                 /* HOME + FX: every effect and every mute off */
        int i;
        ui.btn_used |= 1u << B_FX;
        ui.btn_used |= 1u << B_HOME;
        /* r1: the reset must be recorded in the take. Release every active effect and unmute
         * every muted role through the recorded paths (engine_perform_release ->
         * live_bridge_take_fx, live_bridge_mute), so a replay ends as silent as the reset left
         * it. engine_perform_reset() then sweeps any state a release does not cover (a filter
         * still sweeping back, the echo base). */
        for (i = 0; i < 6; i++)
            if (engine_perform_latched(i) || engine_perform_active(i))
                engine_perform_release(i);
        for (i = 0; i < LIVE_NROLES; i++)
            if (live_bridge_muted((uint8_t)i))
                live_bridge_mute((uint8_t)i, 0);
        engine_perform_reset();
        fx_consumed = 1;                           /* the reset press is not a layer press */
        say("FX RESET", 0);
        return;
    }
    fx_consumed = 0;
    fx_held = 1;                                   /* the FX layer opens */
    ui.btn_used |= 1u << B_FX;                     /* the release must not also act */
    note_release_all();                            /* TFIXU 6: opening the layer releases PLAY notes */
}

static void live_input(void)
{
    int i, e, pgi = cur_page();
    if (setup_open && !live_bridge_take_playing()) live_setup_input();
    if (live_bridge_take_playing() || take_open)
        return;                            /* T9i: the take card and playback take no encoders */
    if (!setup_open) live_fx_button();
    for (i = 0; i < NE; i++) {
        e = (int)enc(i);
        if (i == EN_ALGO && e && !setup_open) {
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
 * slow blink (queued to stop). SEL is lit on the PLAY layer. ---- */

static void live_leds(uint32_t *btn, uint32_t *key, uint32_t *dim)
{
    int k;
    if (live_bridge_transport_playing())
        *btn |= 1u << B_PLAY;
    if (fx_layer_active()) {                           /* T10i: the six punches and the role mutes */
        int i;
        for (i = 0; i < 6; i++) {                      /* TFIXU 4: the lower keys, by position */
            if (engine_perform_latched(i) || fx_key_dn[i])
                *key |= 1u << WHITE_KEY[i];            /* active: latched or held */
            else
                *dim |= 1u << WHITE_KEY[i];
        }
        for (i = 0; i < LIVE_NROLES; i++) {            /* the upper keys, by position */
            if (live_bridge_muted((uint8_t)i))
                *key |= 1u << BLACK_KEY[i];            /* a muted role lit */
            else
                *dim |= 1u << BLACK_KEY[i];
        }
        return;
    }
    if (live.layer == LIVE_PLAY)
        *btn |= 1u << B_SEL;                               /* the SEL LED: off = PADS, lit = PLAY */
    for (k = 0; k < NKEYS; k++) {
        int clip, role, play, q, qstart, qstop;
        if (live.layer == LIVE_PLAY) {                     /* the keyboard: its Cs dim, to find your place */
            if (k % 12 == 7)
                *dim |= 1u << k;
            continue;
        }
        clip = live_bridge_key_clip(k);
        if (clip >= LIVE_ASSIGN_STOP && clip < LIVE_ASSIGN_COUNT) {   /* an assigned stop, FX or live note */
            int f = clip - LIVE_ASSIGN_FX;
            if (clip >= LIVE_ASSIGN_FX && clip < LIVE_ASSIGN_NOTE && (engine_perform_latched(f) || fx_key_dn[f]))
                *key |= 1u << k;
            else
                *dim |= 1u << k;
            continue;
        }
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
                help_shift_active() ? "SHIFT" : take_open ? "TAKE" : setup_open ? "SETUP" : fx_layer_active() ? "FX"
                                   : (live.layer == LIVE_PADS ? "PADS" : "PLAY"),
                (take_open || fx_layer_active()) && !help_shift_active() ? TOK_ACCENT : TOK_MID);
    }
    x = 104;
    if (playing)
        play_icon(x, 4, C_WHITE);
    else
        box(x + 1, 5, 8, 8, TOK_MID);
    if (midi_live_ext()) {
        box(169, 3, 25, 14, TOK_ACCENT);
        cv_text(172, vc(&AF_S, 3, 14), &AF_S, "EXT", TOK_INK);
    }
    put_i(t, (int)((midi_live_ext() ? midi_live_bpm_x10() : live_bridge_bpm_x10()) / 10u));
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
    if (ui.dirty || song_io_dirty())
        dot(233, 9, 1, C_GRAY);
    cv_commit(0, 0, 0);
}

/* ---- the footer ---- */

static uint16_t live_footer(const char **a, const char **b, const char **c)
{
    /* the design's lowercase; FOOT_Y leaves room for the descenders (ui.c) */
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
    if (setup_open) {
        *a = assign_open ? "PRESETS: PICK" : "SELECT: ROW";
        *b = assign_open ? "RELEASE: KEEP" : "ALGO: VALUE";
        *c = assign_open ? 0 : "EDIT: DONE";
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
        *b = "SEL pads";
    } else {
        *a = "SEL play";
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
        q = put_i(q, live.octv);
        q = put_s(q, " - ");
        q = put_s(q, LV_KEY[song_io_key() % 12]);
        *q++ = ' ';
        put_s(q, LV_SCALE[live_song_scale()]);              /* TFIXU 9: the song's key and scale */
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
    else if (setup_open)
        live_setup_draw(band);
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
