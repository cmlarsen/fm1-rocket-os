/* SPDX-License-Identifier: GPL-3.0-only */
/* T3: the LIVE view. Included by ui.c (like undo.c) so it can draw with the same helpers; it
 * is the PADS layer (a role x clip grid on the keys) and the PLAY layer (the keys play the
 * focused role), toggled by SCL. The state itself is plain bytes; the song and the clip engine
 * belong to live_bridge.c. Screens follow design/project/Live-Pads-*.dc.html, Live-Play.dc.html,
 * Header.dc.html and PadGrid.dc.html, at 1:1. */
#include "live_bridge.h"

#define LIVE_HOLD_MS 600u                 /* the interaction model's ring: ~600 ms */

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

/* T3 review: a key's note is tracked per key, whatever the layer/focus/octave does while it is
 * held. A release always releases the note that key started (never the current focus's). */
static uint8_t held_role[NKEYS], held_note[NKEYS], held_on[NKEYS];
static void live_clear_held(void)
{
    int k;
    for (k = 0; k < NKEYS; k++)
        held_on[k] = 0;
}

/* ---- mode in and out ---- */

static void live_enter(void)
{
    live.back = ui.view;
    live.back_classic = ui.classic;       /* T7a: remember whether we came from the classic pages */
    live.layer = LIVE_PADS;               /* the PADS layer is the default on entry */
    live.focus = LIVE_BASS;               /* ... or whichever role launched most recently */
    live.octv = 0;
    live_clear_held();
    ui.classic = 0;                       /* LIVE is the clip engine, wherever it was entered from */
    live_bridge_enter(&proj.pat[0], 0);   /* T7a: make the clip engine the source; it does not
                                           * rebuild and does not reset the transport, so clips keep
                                           * playing across the mode change */
    set_view(V_LIVE);
    say("LIVE", 0);
}

static void live_leave(void)
{
    live_clear_held();
    if (live.back_classic) {              /* back to the classic pages: X0X's sequencer resumes */
        ui.classic = 1;
        live_bridge_exit();
        engine_all_off();
        set_view(live.back == V_LIVE ? V_HOME : live.back);
    } else {                              /* back to the song: the clips keep playing (T7a dec. 7) */
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

static int live_handle_key(int k, int down)
{
    if (!down) {                          /* a release always releases the note this key started */
        if (held_on[k]) {
            live_bridge_play_key(held_role[k], held_note[k], 0, 0);
            held_on[k] = 0;
        }
        return 1;
    }
    if (live.layer == LIVE_PLAY) {        /* the keys play the focused role, chromatically */
        int note = 41 + k + 12 * live.octv;
        if (note < 0)
            note = 0;
        if (note > 127)
            note = 127;
        held_role[k] = live.focus;        /* the layer/focus/octave may change while it is held */
        held_note[k] = (uint8_t)note;
        held_on[k] = 1;
        live_bridge_play_key(held_role[k], held_note[k], 1, 88);
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
    switch (b) {
    case B_PLAY:
        if (live_bridge_transport_playing()) {
            live_bridge_transport_stop();
            say("STOP", 0);
        } else {
            live_bridge_transport_start();
            say("PLAY", 0);
        }
        break;
    case B_SEL:                                            /* SCL toggles the layers */
        live.layer = (uint8_t)(live.layer == LIVE_PADS ? LIVE_PLAY : LIVE_PADS);
        say(live.layer == LIVE_PADS ? "PADS" : "PLAY", 0);
        break;
    case B_OCTDN:
    case B_OCTUP: {
        int o = live.octv + (b == B_OCTUP ? 1 : -1);
        live.octv = (int8_t)(o < -3 ? -3 : o > 4 ? 4 : o);
        break;
    }
    default:
        break;                                             /* SEL, HOME, REC, FX ...: inert in T3 */
    }
    return 1;
}

/* ---- the encoders: ALGORITHM steps the focused role; K1-K4 are the LIVE performance knobs ---- */

static void live_input(void)
{
    int i, e, pgi = cur_page();
    for (i = 0; i < NE; i++) {
        e = (int)enc(i);
        if (i == EN_ALGO && e) {
            int f = live.focus + (e > 0 ? 1 : -1);
            live.focus = (uint8_t)((f + LIVE_NROLES) % LIVE_NROLES);
        } else if (i >= EN_K1 && i <= EN_K4 && e) {
            turn_ref(pg.r[pgi][i - EN_K1], e, i);     /* FILTER / SPACE / ECHO / DRIVE */
            ui.touched = (int8_t)(i - EN_K1);
            ui.touch_until = plat_ms() + 900u;
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
    cv_text(4 + w + 6, vc(&AF_S, 3, 14), &AF_S, live.layer == LIVE_PADS ? "PADS" : "PLAY", TOK_MID);
    x = 104;
    if (playing)
        play_icon(x, 4, C_WHITE);
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
    if (ui.dirty)
        dot(233, 9, 1, C_GRAY);
    cv_commit(0, 0, 0);
}

/* ---- the footer ---- */

static uint16_t live_footer(const char **a, const char **b, const char **c)
{
    *a = *b = *c = 0;
    if (live.layer == LIVE_PLAY) {
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

static void live_draw(int band)
{
    if (live.layer == LIVE_PLAY)
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
