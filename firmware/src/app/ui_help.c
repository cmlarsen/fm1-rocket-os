/* SPDX-License-Identifier: GPL-3.0-only */
/* T11h: the cheat sheet (HOME held) and the GLO+button help cards. See ui_help.h.
 *
 * This file holds the two text tables (the reason it is one file: docs and tools/check_help.py
 * can read the card table) and the drawing. It is compiled into ui.c by inclusion, after the
 * drawing helpers, so it uses box()/frame()/cv_text()/tw()/vc()/text_r() directly.
 *
 * The card table is HELP_CARD[context][button]; a button's card is its tap, hold and combos in
 * 1..HELP_CARD_LINES lines of at most 224 px at AF_S (help_cards_fit() enforces both). The
 * cheat sheet is SHEET_<context>: the HOME combos from design/project/Sys-Shift.dc.html. */
#include "ui_help.h"

/* ============================================================ the text tables === */

typedef struct { const char *l[HELP_CARD_LINES]; } help_card_t;

/* One entry for every button in STUDIO, LIVE and SONGS. Written from the interaction model
 * (docs/product/interaction-model.md) and the handlers in ui.c/ui_studio.c/ui_live.c/ui_songs.c;
 * the report lists where the two disagree (the text follows the model, the code is unchanged). */
static const help_card_t HELP_CARD[HELP_NCTX][NB] = {
[HELP_CTX_STUDIO] = {
    [B_FX]   = {{"the FX page", "tap again: next page", "HOME+FX: reset all FX"}},
    [B_SEL]  = {{"launch the cursor clip", "hold 1.5 s: clear it", "in a list: run the row"}},
    [B_ENV]  = {{"no action here", "the sound lives on EDIT"}},
    [B_LFO]  = {{"the mixer and master", "tap again: next page"}},
    [B_EDIT] = {{"the cursor role's sound", "drums 909, bass 303,", "chords FM6, FX NOISE"}},
    [B_GLO]  = {{"settings; tap again:", "close", "GLO + a button: its card"}},
    [B_HOME] = {{"the song home (root)", "HOME + tap: launch now", "HOME + hold: clear the clip", "hold: the cheat sheet"}},
    [B_SAVE] = {{"save now", "hold 0.6 s: the songs", "stopped: it autosaves"}},
    [B_ARP]  = {{"no action here", "TB-3PO is gone"}},
    [B_SEQ]  = {{"new clip: REC, or step it", "the cursor clip's settings", "tap again: next page"}},
    [B_PLAY] = {{"start / stop the clips", "hold 0.6 s: enter LIVE", "HOME+PLAY: redo"}},
    [B_REC]  = {{"records the cursor clip", "from the next bar", "HOME+REC: undo"}},
    [B_OCTDN] = {{"no action here yet", "clip banks come later"}},
    [B_OCTUP] = {{"no action here yet", "clip banks come later"}}
},
[HELP_CTX_LIVE] = {
    [B_FX]   = {{"punch FX on the keys", "tap latches, hold is momentary", "HOME+FX: reset all FX"}},
    [B_SEL]  = {{"SEL toggles the layers", "off: PADS, lit: PLAY", "in SETUP: open the row"}},
    [B_ENV]  = {{"no action in LIVE"}},
    [B_LFO]  = {{"no action in LIVE"}},
    [B_EDIT] = {{"EDIT: setup / EDIT again: done", "hold key + PRESETS: assign", "ALGO: change row value", "MIDI, clock, lock, take"}},
    [B_GLO]  = {{"GLO + a button: its card", "nothing else in LIVE"}},
    [B_HOME] = {{"HOME + tap a pad:", "launch it now", "hold: the cheat sheet", "HOME never exits LIVE"}},
    [B_SAVE] = {{"no action in LIVE", "the song saves when stopped"}},
    [B_ARP]  = {{"no action in LIVE"}},
    [B_SEQ]  = {{"no action in LIVE", "GLO + SEQ shows this card"}},
    [B_PLAY] = {{"start / stop the clips", "hold 0.6 s: leave LIVE", "the ring fills to leave"}},
    [B_REC]  = {{"record the take", "from the next bar; tap again", "stops and keeps it", "hold 1.5 s: clear the take"}},
    [B_OCTDN] = {{"octave down in PLAY", "the keys play the role"}},
    [B_OCTUP] = {{"octave up in PLAY", "the keys play the role"}}
},
[HELP_CTX_SONGS] = {
    [B_FX]   = {{"no action in SONGS", "SONGS is a stopped view"}},
    [B_SEL]  = {{"open the song", "row 0: new song", "in a menu: do the row"}},
    [B_ENV]  = {{"no action in SONGS", "SONGS is a stopped view"}},
    [B_LFO]  = {{"no action in SONGS", "SONGS is a stopped view"}},
    [B_EDIT] = {{"the song's menu", "open, copy, rename,", "delete (hold)"}},
    [B_GLO]  = {{"GLO + a button: its card", "SONGS is a stopped view"}},
    [B_HOME] = {{"back out of SONGS", "HOME+REC: undo a delete"}},
    [B_SAVE] = {{"no action in SONGS", "SONGS is a stopped view"}},
    [B_ARP]  = {{"no action in SONGS", "SONGS is a stopped view"}},
    [B_SEQ]  = {{"no action in SONGS", "SONGS is a stopped view"}},
    [B_PLAY] = {{"no action in SONGS", "SONGS is a stopped view"}},
    [B_REC]  = {{"HOME+REC: undo the", "last song delete"}},
    [B_OCTDN] = {{"no action in SONGS", "SONGS is a stopped view"}},
    [B_OCTUP] = {{"no action in SONGS", "SONGS is a stopped view"}}
}
};

/* the cheat sheet: the HOME combos of a context (key, tap/hold/touch, what it does). STUDIO is
 * Sys-Shift's six rows; LIVE is the same minus the editing rows (pad hold, knob-lane touch). */
typedef struct { const char *key, *mod, *does; } help_row_t;
static const help_row_t SHEET_STUDIO[] = {
    {"REC",    "tap",   "undo"},
    {"PLAY",   "tap",   "redo"},
    {"PAD",    "tap",   "launch now"},
    {"PAD",    "hold",  "clear clip"},
    {"K1-K4",  "touch", "clear knob lane"},
    {"FX",     "tap",   "reset all FX"}
};
static const help_row_t SHEET_LIVE[] = {
    {"REC",  "tap", "undo"},
    {"PLAY", "tap", "redo"},
    {"PAD",  "tap", "launch now"},
    {"FX",   "tap", "reset all FX"}
};

static const help_card_t *help_card_for(int ctx, int b)
{
    if (ctx < 0 || ctx >= HELP_NCTX || b < 0 || b >= NB)
        return 0;
    return &HELP_CARD[ctx][b];
}
static const char *help_ctx_name(int ctx)
{
    return ctx == HELP_CTX_LIVE ? "in LIVE" : ctx == HELP_CTX_SONGS ? "in SONGS" : "in STUDIO";
}

/* =================================================================== state === */

typedef struct {
    uint8_t sheet;                         /* the cheat sheet is up (HOME held) */
    uint8_t card;                          /* the GLO+button card: its button + 1, 0 none */
} help_ui_t;
static help_ui_t hlp;

int help_ctx(void)
{
    if (ui.overlay == O_ASK)
        return -1;                         /* a question: no overlay */
    if (ui.view == V_SONGS)
        return HELP_CTX_SONGS;
    if (ui.view == V_LIVE)
        return HELP_CTX_LIVE;
    return HELP_CTX_STUDIO;
}

int help_shift_active(void) { return hlp.sheet != 0; }
int help_card_active(void) { return hlp.card != 0; }
int help_card_button(void) { return hlp.card ? hlp.card - 1 : -1; }

/* SYSTEM 1 + closing the card: called once a frame from ui.c's input(), after help_tick. */
void help_overlay_tick(uint32_t btn, uint32_t keys)
{
    uint32_t home = 1u << B_HOME;
    int ctx = help_ctx();
    if (ctx == -1) {                       /* a question: no overlay */
        hlp.sheet = 0;
        hlp.card = 0;
        return;
    }
    if (hlp.card) {                        /* GLO + a button: a release of either closes it */
        uint32_t m = 1u << (hlp.card - 1);
        if (!(btn & (1u << B_GLO)) || !(btn & m))
            hlp.card = 0;
    }
    if (!(btn & home))
        hlp.sheet = 0;
    else if (!hlp.sheet && ctx != HELP_CTX_SONGS && btn == home && !keys && ui.overlay == O_NONE &&
             !(ui.btn_used & home) && (int32_t)(ui.turn_t - ui.down_t[B_HOME]) <= 0 &&
             (int32_t)(plat_ms() - ui.down_t[B_HOME]) >= (int32_t)HELP_HOLD_MS) {
        hlp.sheet = 1;                     /* held cleanly: the sheet; the release just closes it */
        ui.btn_used |= home;
    }
}

/* SYSTEM 2: a button pressed while GLO is held shows that button's card (either order). */
int help_glo_press(uint32_t btn, int b)
{
    uint32_t glo = 1u << B_GLO;
    int other;
    if (help_ctx() == -1)
        return 0;
    if (b != B_GLO) {
        if (!(ui.btn & glo))               /* GLO must already be held */
            return 0;
        other = b;
    } else {
        uint32_t d = (btn & ~glo) & ui.btn;   /* GLO pressed while another button is already held */
        if (!d || (d & (d - 1u)))
            return 0;
        other = 0;
        while (!(d & (1u << other)))
            other++;
    }
    hlp.card = (uint8_t)(other + 1);
    ui.btn_used |= glo | (1u << other);    /* neither release acts */
    return 1;
}

/* SONGS runs its own event loop, so its GLO+button edge is detected here. Returns 1 while a card
 * is up: the shell blanks the event's buttons so the card is modal. */
int help_songs_tick(uint32_t btn, uint32_t keys)
{
    uint32_t glo = 1u << B_GLO, pressed = btn & ~ui.btn;
    int b;
    (void)keys;
    if (help_ctx() != HELP_CTX_SONGS) {
        hlp.card = 0;
        return 0;
    }
    if (hlp.card) {
        uint32_t m = 1u << (hlp.card - 1);
        if (!(btn & glo) || !(btn & m))
            hlp.card = 0;
    }
    for (b = 0; b < NB; b++)
        if ((pressed & (1u << b)) && b != B_GLO && (ui.btn & glo)) {
            hlp.card = (uint8_t)(b + 1);
            ui.btn_used |= glo | (1u << b);
            break;
        }
    if ((pressed & glo) && !hlp.card) {
        uint32_t d = btn & ~glo;
        if (d && !(d & (d - 1u))) {
            int other = 0;
            while (!(d & (1u << other)))
                other++;
            hlp.card = (uint8_t)(other + 1);
            ui.btn_used |= glo | (1u << other);
        }
    }
    return hlp.card != 0;
}

/* ================================================================= drawing === */

#define HELP_SHEET_Y0 3
#define HELP_SHEET_H 20
#define HELP_SHEET_STEP 22
#define HELP_SHEET_N(ctx) ((ctx) == HELP_CTX_LIVE ? (int)(sizeof SHEET_LIVE / sizeof SHEET_LIVE[0]) : (int)(sizeof SHEET_STUDIO / sizeof SHEET_STUDIO[0]))
#define HELP_SHEET_ROWS(ctx) ((ctx) == HELP_CTX_LIVE ? SHEET_LIVE : SHEET_STUDIO)

static void help_draw_sheet(int band)
{
    int ctx = help_ctx();
    const help_row_t *rows = HELP_SHEET_ROWS(ctx);
    int n = HELP_SHEET_N(ctx), i;
    for (i = 0; i < n; i++) {
        int32_t abs = HELP_SHEET_Y0 + i * HELP_SHEET_STEP, y = abs - band * BAND_H, cw, x;
        if (y <= -HELP_SHEET_H || y >= BAND_H)
            continue;
        box(4, y, 232, HELP_SHEET_H, TOK_SURF);
        cw = tw(&AF_S, rows[i].key) + 8;
        box(8, y + 3, cw, 14, TOK_KEY);
        cv_text(12, vc(&AF_S, y + 3, 14), &AF_S, rows[i].key, TOK_INK);
        x = 8 + cw + 6;
        x = cv_text(x, vc(&AF_S, y, HELP_SHEET_H), &AF_S, rows[i].mod, TOK_MID) + 6;
        (void)x;
        text_r(230, vc(&AF_S, y, HELP_SHEET_H), &AF_S, rows[i].does, TOK_TEXT);
    }
}

static void help_draw_card(int band)
{
    int b = hlp.card - 1;
    const help_card_t *c = help_card_for(help_ctx(), b);
    int32_t y0 = 2 - band * BAND_H;
    int i;
    uint16_t chip = (b == B_REC) ? TOK_REC : TOK_THEME;
    box(4, y0, 232, 134, TOK_SURF);
    frame(4, y0, 232, 134, TOK_LINE);
    if (band == 0) {
        int32_t w = tw(&AF_S, BTN_LABEL[b]) + 10;
        box(10, 8, w, 16, chip);
        cv_text(15, vc(&AF_S, 8, 16), &AF_S, BTN_LABEL[b], C_BLACK);
        cv_text(10 + w + 6, vc(&AF_S, 8, 16), &AF_S, help_ctx_name(help_ctx()), TOK_MID);
    }
    if (!c)
        return;
    for (i = 0; i < HELP_CARD_LINES && c->l[i]; i++) {
        int32_t abs = 36 + i * 24, y = abs - band * BAND_H;
        if (y <= -16 || y >= BAND_H)
            continue;
        cv_text(12, vc(&AF_S, abs, 14) - band * BAND_H, &AF_S, c->l[i], TOK_TEXT);
    }
}

void help_draw(int band)
{
    if (hlp.card)
        help_draw_card(band);
    else if (hlp.sheet)
        help_draw_sheet(band);
}

void help_draw_songs(void)
{
    cv_begin(240, 2 * BAND_H, C_BLACK);
    cv_band(0, BAND_H);
    help_draw(0);
    cv_band(BAND_H, BAND_H);
    help_draw(1);
    cv_blit(0, MAIN_Y);
}

/* ================================================================== footer === */

int help_footer(const char **a, const char **b, const char **c)
{
    /* uppercase: a lowercase descender would reach past the 82 px knob strip (screens-lint) */
    if (hlp.sheet) {
        *a = "HOLDING HOME";
        *b = "TAP HOME: BACK";
        *c = 0;
        return 1;
    }
    if (hlp.card) {
        *a = "RELEASE: CLOSE";
        *b = 0;
        *c = 0;
        return 1;
    }
    return 0;
}

/* ==================================================== the host fit check === */

#ifdef X0X_HOST
#include <stdio.h>
/* every card is 1..HELP_CARD_LINES lines of at most 224 px, and every cheat-sheet row fits: a
 * backstop for hand-written text, like ui.c's help_fits() is for the inherited cards. */
int help_cards_fit(void)
{
    int ctx, b, i, n, ok = 1;
    for (ctx = 0; ctx < HELP_NCTX; ctx++)
        for (b = 0; b < NB; b++) {
            const help_card_t *c = help_card_for(ctx, b);
            if (!c || !c->l[0]) {
                printf("help card missing: %s %s\n", help_ctx_name(ctx), BTN_LABEL[b]);
                ok = 0;
                continue;
            }
            for (n = 0; n < HELP_CARD_LINES && c->l[n]; n++) {
                if (tw(&AF_S, c->l[n]) > 224) {
                    printf("help card too wide: %s %s: \"%s\" (%d px)\n", help_ctx_name(ctx), BTN_LABEL[b],
                           c->l[n], (int)tw(&AF_S, c->l[n]));
                    ok = 0;
                }
            }
            if (n < 1 || n > HELP_CARD_LINES) {
                printf("help card %s %s: %d lines\n", help_ctx_name(ctx), BTN_LABEL[b], n);
                ok = 0;
            }
        }
    for (ctx = HELP_CTX_STUDIO; ctx <= HELP_CTX_LIVE; ctx++) {
        const help_row_t *rows = HELP_SHEET_ROWS(ctx);
        for (i = 0; i < HELP_SHEET_N(ctx); i++) {
            int32_t cw = tw(&AF_S, rows[i].key) + 8, mx = 8 + cw + 6 + tw(&AF_S, rows[i].mod) + 8,
                    dx = 230 - tw(&AF_S, rows[i].does);
            if (mx > dx || tw(&AF_S, rows[i].does) > 224) {
                printf("cheat sheet row too wide: %s %s %s (mod ends %d, text starts %d)\n",
                       rows[i].key, rows[i].mod, rows[i].does, (int)mx, (int)dx);
                ok = 0;
            }
        }
    }
    return ok;
}
#endif
