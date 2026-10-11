/* SPDX-License-Identifier: GPL-3.0-only */
/* T7b: the clip editors — the interface ui.c needs. ui_clip.c is an include-style view (like
 * ui_live.c / ui_studio.c): it is compiled as part of ui.c, after the drawing helpers, so it can
 * use them and reach ui / proj / live_bridge / song_io. ui.c includes this header near the top so
 * build_pages and the input/draw/led dispatch can name the entry points before the include.
 *
 * The editors own STUDIO 3 (notes), STUDIO 4 (drums) and STUDIO 15 (motion) plus recording into a
 * clip, all through song_io. They edit the clip the song home's cursor points at; the clip engine
 * is told to re-take its playing copy with live_bridge_clip_reload() after an edit. */
#pragma once
#include <stdint.h>

/* Open the editor for a clip (SEQ on the song home, or REC on an empty / loaded slot). Sets the
 * view to V_CLIPEDIT and begins the editor's one-undo session. */
void clip_open(int clip);

int  clip_editing(void);                      /* the view is one of the editor's pages */
int  clip_edit_is_drum(void);                 /* the clip being edited is a DRUMS clip */

/* REC on the song home: open the editor and arm a pass (a fresh slot's create shares its entry). */
void clip_record_open(int clip);

/* Recording: REC arms a pass, a second REC closes it at the next bar, and the clip loops. */
int  clip_rec_active(void);                   /* a pass is running */
void clip_rec_button(void);                   /* arm / close (from the editor's REC) */
void clip_rec_arm(int keep_session);          /* arm; keep_session: a fresh slot's create is the entry */
void clip_tick(void);                         /* the record-close timer and the edit toast */

/* Input and drawing (ui.c dispatches to these when clip_editing()). */
void clip_handle_key(int k, int down);
int  clip_handle_button(int b);               /* 1 = consumed */
void clip_input(void);                        /* the encoders */
void clip_draw_header(void);
void clip_draw(int band);
uint16_t clip_footer(const char **a, const char **b, const char **c);
void clip_leds(uint32_t *btn, uint32_t *key, uint32_t *dim);

/* The K1-K4 cards (ui.c's R_ED_STEP.. prefs route pref_get/pref_set here). `which` is a CLK_*. */
int  clip_edit_get(int which);
void clip_edit_set(int which, int v);

/* The motion page's K1-K4 (R_ENG refs; a turn writes a motion lane while a pass records). */
void clip_motion_knob(int i, int e);

/* The clip the editor holds (-1 none), for the host runner. */
int  clip_cur_clip(void);

enum { CLK_STEP, CLK_NOTE, CLK_VEL, CLK_LEN, CLK_LANE };
