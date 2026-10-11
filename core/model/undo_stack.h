/* SPDX-License-Identifier: GPL-3.0-only */
/* Undo/redo core (T6c).
 *
 * A fixed RAM ring of entries, no heap. Clip edits (clear, copy over, a record
 * pass) store the old clip_t and swap it with the song's clip on undo/redo, so
 * one entry serves both directions. Take and delete entries only name the slot
 * plus the sequence number of the record they expect to restore: the
 * recoverable bytes live in the store, and the store re-commits them as new
 * records so the undo survives a reboot. A new edit clears the redo tail.
 *
 * Depth is set by 6 KiB / (4 + max(clip_t, u32)) = 6 KiB / (4 + 528) ~= 11 (T9i cut it from
 * 23 to free pool for the take; the per-song clip undo and the separate song-delete undo are
 * unchanged). */
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

/* return values of undo_stack_undo / _redo */
enum { UNDO_CANNOT = -1, UNDO_EMPTY = 0, UNDO_DONE = 1 };

#define UNDO_STACK_DEPTH 11u

typedef struct {
    uint8_t type;
    uint8_t index;         /* clip index, or slot for take/delete */
    uint8_t aux;           /* source clip for a copy */
    uint8_t pad;
    union {
        clip_t clip;       /* the old clip; after an undo it holds the new one */
        uint32_t seq;      /* expected record sequence number for take/delete */
    } u;
} undo_entry_t;

typedef struct {
    undo_entry_t e[UNDO_STACK_DEPTH];
    uint8_t n;             /* entries stored (<= depth) */
    uint8_t cur;           /* entries applied (0..n) */
    uint8_t pad[2];
} undo_stack_t;

/* The RAM a take undo/redo loads the previous take into and re-commits from. The
 * caller owns it (the recorder already keeps one); it never overlaps the store. */
typedef struct {
    take_hdr_t *hdr;
    take_event_t *ev;
    uint32_t max_events;
} undo_take_scratch_t;

void undo_stack_init(undo_stack_t *u);
void undo_stack_push_clear(undo_stack_t *u, uint8_t clip, const clip_t *old);
void undo_stack_push_record(undo_stack_t *u, uint8_t clip, const clip_t *old);
void undo_stack_push_copy(undo_stack_t *u, uint8_t dst, uint8_t src, const clip_t *old_dst);
/* Call after the store edit being undone: the take/delete entry records the
 * sequence number of the record it will restore. A new take entry for a slot
 * invalidates older take entries for that slot, so only the newest is undoable. */
void undo_stack_push_take(undo_stack_t *u, const song_store_t *store, uint8_t slot);
void undo_stack_push_delete(undo_stack_t *u, const song_store_t *store, uint8_t slot);

int undo_stack_undo(undo_stack_t *u, song_t *song, song_store_t *store, const undo_take_scratch_t *ts);
int undo_stack_redo(undo_stack_t *u, song_t *song, song_store_t *store, const undo_take_scratch_t *ts);

uint8_t undo_stack_undo_type(const undo_stack_t *u);   /* type of the next undo, or UNDO_NONE */
uint8_t undo_stack_redo_type(const undo_stack_t *u);
