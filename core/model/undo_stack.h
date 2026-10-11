/* SPDX-License-Identifier: GPL-3.0-only */
/* Undo/redo core (T6c).
 *
 * A fixed RAM ring of entries, no heap. Clip edits (clear, copy over, a record
 * pass) store the old clip_t and swap it with the song's clip on undo/redo, so
 * one entry serves both directions. Take and delete entries only name the slot:
 * the recoverable bytes live in the store (the previous take extent, or the
 * untouched song data of a deleted slot) and the store is asked to swap or
 * toggle them. A new edit clears the redo tail.
 *
 * The ring holds UNDO_STACK_DEPTH entries of a fixed 4 + clip_t bytes. clip_t
 * dominates, so the depth is 12 KiB / (4 + 528) ~= 23; the ring, with its head
 * and cursor, is 12,240 B. */
#pragma once
#include <stdint.h>
#include "model.h"
#include "song_store.h"

enum {
    UNDO_NONE = 0,
    UNDO_CLEAR_CLIP,       /* cleared a clip (HOME + key) */
    UNDO_COPY_CLIP,        /* copied a clip over another (hold + tap) */
    UNDO_RECORD_CLIP,      /* a record pass overwrote a clip */
    UNDO_REPLACE_TAKE,     /* recording again replaced the song's take */
    UNDO_DELETE_SONG       /* deleted a song */
};

#define UNDO_STACK_DEPTH 23u

typedef struct {
    uint8_t type;
    uint8_t index;         /* clip index, or slot for take/delete */
    uint8_t aux;           /* source clip for a copy */
    uint8_t pad;
    clip_t clip;           /* the old clip; after an undo it holds the new one */
} undo_entry_t;

typedef struct {
    undo_entry_t e[UNDO_STACK_DEPTH];
    uint8_t n;             /* entries stored (<= depth) */
    uint8_t cur;           /* entries applied (0..n) */
    uint8_t pad[2];
} undo_stack_t;

void undo_stack_init(undo_stack_t *u);
void undo_stack_push_clear(undo_stack_t *u, uint8_t clip, const clip_t *old);
void undo_stack_push_record(undo_stack_t *u, uint8_t clip, const clip_t *old);
void undo_stack_push_copy(undo_stack_t *u, uint8_t dst, uint8_t src, const clip_t *old_dst);
void undo_stack_push_take(undo_stack_t *u, uint8_t slot);
void undo_stack_push_delete(undo_stack_t *u, uint8_t slot);

int undo_stack_undo(undo_stack_t *u, song_t *song, song_store_t *store);   /* 1 = undone */
int undo_stack_redo(undo_stack_t *u, song_t *song, song_store_t *store);   /* 1 = redone */

uint8_t undo_stack_undo_type(const undo_stack_t *u);   /* type of the next undo, or UNDO_NONE */
uint8_t undo_stack_redo_type(const undo_stack_t *u);
