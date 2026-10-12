/* SPDX-License-Identifier: GPL-3.0-only */
/* T8: the seam between the SONGS view (ui_songs.c) and the store (song_io.c).
 *
 * Model-free and platform-free on purpose: `song_io.c` includes model.h (so it must not
 * also pull plat.h's NKEYS) and `ui.c` is one TU that already has X0X's own `song_t`
 * (sequencer.h), so neither can include model.h beside the other. The view builds and
 * saves songs through these ops; nothing here names a model or X0X type. */
#pragma once
#include <stdint.h>

/* model.h's SONG_SLOTS and SONG_NAME_LEN, kept here so no model.h is needed.
 * song_io.c static-asserts them equal. */
#define SONGS_SLOTS 5u
#define SONGS_NAME 12u

/* one row of the songs list, as the ops report it */
typedef struct {
    char    name[SONGS_NAME];
    uint8_t used;                      /* a live (undeleted) song exists */
    uint8_t has_take;
    uint8_t current;                   /* the song being edited / heard */
} song_row_t;

/* What the view needs from the store. song_io.c provides the real table
 * (song_io_songs_ops); tests/host/ui_songs_test.c provides a RAM fake. Each pointer is
 * required; a call that can fail returns < 0 (or -2 for "no free slot"). */
typedef struct {
    void *ctx;

    /* Fill `out` for slot 0 .. SONGS_SLOTS-1. A deleted slot reports used = 0 and an
     * empty name; `current` marks the song the app is editing. Always a valid row. */
    void (*list)(void *ctx, int slot, song_row_t *out);

    /* The slot being edited, or -1. */
    int  (*current_slot)(void *ctx);

    /* Make `slot` the current song. The caller (the view's shell, ui.c) stops the clip
     * engine and silences everything first; this loads the song and reloads the clip
     * engine. 0 = opened. */
    int  (*open)(void *ctx, int slot);

    /* Write a new song (kind 0 = Empty, 1 = Starter) from the templates into the first
     * free slot. Returns the new slot (>= 0), or -2 when all SONGS_SLOTS are taken. */
    int  (*create_new)(void *ctx, int kind, uint16_t bpm_x10, int key);

    /* Copy slot `src` into the first free slot. Returns the new slot (>= 0) or -2. */
    int  (*copy)(void *ctx, int src);

    /* Rename slot `slot`, durably. 0 = ok. */
    int  (*rename)(void *ctx, int slot, const char *name);

    /* Delete slot `slot`: the store's durable delete, plus an undo entry. 0 = ok. If it
     * was the current song the shell opens another, or a new Empty when none is left. */
    int  (*del)(void *ctx, int slot);

    /* Undo the most recent delete. UNDO_DONE / UNDO_EMPTY / UNDO_CANNOT
     * (core/model/undo_stack.h). */
    int  (*undelete)(void *ctx);

    /* The slot the next create_new / copy would use: a slot that has never held a song
     * first, then a deleted one, or -2 when all are in use. */
    int  (*first_free)(void *ctx);

    /* 1 while the most recent delete can still be undone (its slot has not been reused). */
    int  (*can_undelete)(void *ctx);

    /* How many clips the slot's song holds, for the menu and the delete ring;
     * -1 when unknown. */
    int  (*clip_count)(void *ctx, int slot);

    /* TFIXS r1/B1: drop the current song's unsaved edits and pending take, so a refused
     * open may proceed. The edits cannot be undone. */
    void (*discard)(void *ctx);
} songs_ops_t;
