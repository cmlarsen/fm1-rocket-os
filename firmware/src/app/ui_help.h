/* SPDX-License-Identifier: GPL-3.0-only */
/* T11h: the two help systems, in one new file with small hooks in ui.c.
 *
 * SYSTEM 1 - the cheat sheet. Holding HOME for HELP_HOLD_MS with no other input shows the
 * context's HOME combinations (design/project/Sys-Shift.dc.html). Releasing HOME closes it.
 * A short HOME tap still means back, and the combos keep working while the sheet is up.
 *
 * SYSTEM 2 - the help cards. GLO + any button shows that button's card for the current
 * context: what tap, hold and combos do (design/project/Sys-Help.dc.html). Releasing closes
 * it. Every card's text lives in the table in ui_help.c, so docs and check_help.py can read it.
 *
 * TCLS: the inherited X0X "hold a button 1 s" help (the Classic help) is gone; ui.c's help_tick
 * no longer sets it. These two systems are the only help.
 *
 * ui_help.c is included by ui.c after the drawing helpers, like ui_live.c, so it draws with the
 * same code and reaches ui/proj. These declarations are what ui.c needs earlier in the file. */
#pragma once
#include <stdint.h>

#define HELP_HOLD_MS 400u              /* HOME held this long shows the cheat sheet */
#define HELP_CARD_LINES 4              /* a card is at most this many text lines */

/* the contexts a card and a cheat sheet are written for */
enum { HELP_CTX_STUDIO, HELP_CTX_LIVE, HELP_CTX_SONGS, HELP_NCTX };

/* the current context, or -1 when there is none (a question or the perf test) */
int  help_ctx(void);

/* SYSTEM 1 */
int  help_shift_active(void);          /* the cheat sheet is on screen */
void help_overlay_tick(uint32_t btn, uint32_t keys);   /* every frame, before the mode input */

/* SYSTEM 2: called on each button press in ui.c's edge loop. Returns 1 when it showed a card
 * (the press must do nothing else); btn is the whole button mask at that moment. */
int  help_glo_press(uint32_t btn, int b);
int  help_card_active(void);
int  help_card_button(void);           /* the card's button, -1 when none (host probe) */
int  help_songs_tick(uint32_t btn, uint32_t keys);     /* SONGS has its own event loop */

/* drawing: one band of the main area, or the card over the SONGS screen */
void help_draw(int band);
void help_draw_songs(void);

/* the footer override: returns 1 when it wrote the line (the sheet or a card is up) */
int  help_footer(const char **a, const char **b, const char **c);

/* host-only: every card and every cheat-sheet row fits the screen (X0X_HOST) */
int  help_cards_fit(void);
