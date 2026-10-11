/* SPDX-License-Identifier: GPL-3.0-only */
/* T8: the SONGS view (STUDIO 7, 8, 11, 12 and 13).
 *
 * An include-style view like ui_live.c / ui_name.c: it draws through gfx.c's canvas
 * (cv_* and the AF_ faces) and drives its state machine from X0X input events, so it
 * is compiled into the translation unit that holds gfx.c. It talks to the songs
 * themselves through a small ops table (`songs_ops_t`) instead of reaching into
 * song_io, which keeps the view testable against a RAM fake and lets the integration
 * pass (T8i) bind it to the store. Nothing here reads flash or touches the engine.
 *
 * Screens:
 *   SO_LIST    STUDIO 7  - "+ New song" then the SONG_SLOTS songs, a current marker
 *                          and the storage meter; SELECT moves, SEL opens, EDIT menu,
 *                          HOME back.
 *   SO_NEW     STUDIO 8  - Empty / Starter / Copy current; K1 tempo, K2 key; SEL
 *                          creates in the first free slot ("5 SONGS MAX: DELETE ONE"
 *                          when none is free).
 *   SO_MENU    STUDIO 11 - Open, Copy, Rename, Delete for one song.
 *   SO_RENAME  STUDIO 12 - T8c's name_edit_* over the song's name.
 *   SO_DELETE  STUDIO 13 - hold SEL 1.5 s; a ring names the song and its contents;
 *                          HOME+REC undoes a delete.
 *
 * The shell (ui.c) enters with ui_songs_begin, feeds one song_event_t per frame to
 * ui_songs_input, draws with ui_songs_draw, and leaves on SONG_ACTION_CLOSE /
 * SONG_ACTION_OPENED. Layout follows design/project/Studio-Songs.dc.html,
 * Studio-NewSong, Studio-SongMenu, Studio-Rename and Studio-DeleteSong at 1:1. */
#pragma once
#include <stdint.h>
#include "ui_name.h"                   /* name_edit_t, name_event_t (and plat.h's B_* / NE / NKEYS) */

/* model.h's SONG_SLOTS and SONG_NAME_LEN, kept local so this header need not include
 * model.h beside plat.h (both define NKEYS). The host test asserts them equal. */
#define SONGS_SLOTS 5u
#define SONGS_ROWS (SONGS_SLOTS + 1u)  /* the list adds the "+ New song" row */
#define SONGS_NAME (NAME_MAX + 1u)     /* SONG_NAME_LEN: 12 */

/* the delete hold-to-confirm ring and the toast the screens can show */
#define SONGS_DELETE_MS 1500u
#define SONGS_TOAST_MS 1600u

/* screens */
enum { SO_LIST = 0, SO_NEW, SO_MENU, SO_RENAME, SO_DELETE };

/* what ui_songs_input reports to the shell */
enum { SONG_ACTION_NONE = 0, SONG_ACTION_CLOSE, SONG_ACTION_OPENED };

/* one row of the songs list, as the ops table reports it */
typedef struct {
    char    name[SONGS_NAME];
    uint8_t used;                      /* a live (undeleted) song exists */
    uint8_t has_take;                  /* the song has one take */
    uint8_t current;                   /* the song being edited / heard */
} song_row_t;

/* The songs the view needs. The integration (T8i) implements these over song_io /
 * song_store; tests/host/ui_songs_test.c implements a RAM fake. Every pointer is
 * required except clip_count, which only feeds the menu and the delete ring. */
typedef struct {
    void *ctx;

    /* Fill `out` for slot 0 .. SONGS_SLOTS-1. A deleted slot reports used = 0 and an
     * empty name; `current` marks the song the app is editing. Always a valid row. */
    void (*list)(void *ctx, int slot, song_row_t *out);

    /* The slot being edited, or -1. */
    int  (*current_slot)(void *ctx);

    /* Make `slot` the current song: stop the transport, flush the current song if it
     * is dirty (the autosave safety rules), load it and reload the clip engine.
     * 0 = opened. */
    int  (*open)(void *ctx, int slot);

    /* Write a song (a `const song_t *`) into the first free slot, without opening it.
     * Returns the new slot (>= 0), or -2 when all SONGS_SLOTS are taken. */
    int  (*create_from)(void *ctx, const void *song);

    /* Copy slot `src` into the first free slot. Returns the new slot (>= 0) or -2. */
    int  (*copy)(void *ctx, int src);

    /* Rename slot `slot`, durably. 0 = ok. */
    int  (*rename)(void *ctx, int slot, const char *name);

    /* Delete slot `slot`: the store's durable delete, plus an undo entry. 0 = ok. If it
     * was the current song the op opens another, or a new Empty when none is left. */
    int  (*del)(void *ctx, int slot);

    /* Undo the most recent delete. UNDO_DONE / UNDO_EMPTY / UNDO_CANNOT
     * (core/model/undo_stack.h). */
    int  (*undelete)(void *ctx);

    /* How many clips the slot's song holds, for the menu and the delete ring;
     * -1 when unknown. */
    int  (*clip_count)(void *ctx, int slot);
} songs_ops_t;

/* X0X's `input()` frame (btn / keys / enc / ms), the same shape ui_name uses */
typedef name_event_t song_event_t;

typedef struct {
    const songs_ops_t *ops;
    uint8_t  open;                     /* the view is showing */
    uint8_t  screen;                   /* SO_* */
    uint8_t  row;                      /* the cursor in the current screen */
    uint8_t  slot;                     /* the song the menu / rename / delete acts on */
    song_row_t rows[SONGS_SLOTS];
    int      nused;                    /* songs in use (the meter's numerator) */

    /* SO_NEW */
    uint8_t  tmpl;                     /* 0 Empty, 1 Starter, 2 Copy current */
    uint16_t bpm_x10;
    uint8_t  key;                      /* 0..23 (song_templates.h) */

    /* SO_RENAME */
    name_edit_t name;

    /* SO_DELETE */
    uint8_t  del_held;                 /* SEL is down and armed */
    uint8_t  del_done;                 /* the delete fired */
    uint8_t  del_up_seen;              /* SEL was released since entering the screen */
    uint32_t del_t0;

    /* button/key edges (the shell feeds held state) */
    uint32_t prev_btn, prev_keys;

    /* a transient message, shown as a pill and readable by the shell */
    char     msg[30];
    uint32_t msg_until;
} songs_ui_t;

/* Enter / leave. begin refreshes the rows and puts the cursor on the current song. */
void ui_songs_begin(songs_ui_t *u, const songs_ops_t *ops);
void ui_songs_end(songs_ui_t *u);
int  ui_songs_active(const songs_ui_t *u);

/* Re-read the rows from the ops (after an edit or an external undo). */
void ui_songs_refresh(songs_ui_t *u);

/* One frame of input. Returns SONG_ACTION_NONE, SONG_ACTION_CLOSE (HOME) or
 * SONG_ACTION_OPENED (a song was opened or created). */
int  ui_songs_input(songs_ui_t *u, const song_event_t *ev);

/* Draw the whole screen (two bands, blitted). `ms` drives the delete ring's blink and
 * the toast. The rename screen delegates to name_edit_draw. */
void ui_songs_draw(const songs_ui_t *u, uint32_t ms);

/* The key/button LEDs the screen wants; the shell ORs them in. */
void ui_songs_leds(const songs_ui_t *u, uint32_t *btn, uint32_t *key, uint32_t *dim);

/* The current message, or NULL. */
const char *ui_songs_toast(const songs_ui_t *u);
