/* SPDX-License-Identifier: GPL-3.0-only */
/* T6i: the song on real flash — the glue between core/model's store and undo, the LIVE song the
 * bridge owns, and X0X's project save. One current song (v1; the songs list is T8).
 *
 * Plain types only: ui.c is one compilation unit that already has X0X's own `song_t` in scope, so
 * the model's `song_t` (and `clip_t`, `take_hdr_t`, `take_event_t`) stay inside song_io.c and are
 * passed through `void *` here. `pattern_t` comes from X0X's pattern.h, as live_bridge.h does.
 *
 * The store and the undo stack are caller-owned big static buffers (song_io.c; .pool on device):
 * `song_store_t` (144 B) and `undo_stack_t` (~12 KiB).
 *
 * Boot: song_store scans 0xA0000..0xDBFFF. If slot 0 holds a valid, undeleted song it becomes the
 * current song (and LIVE stops rebuilding from the project); otherwise the current song is mapped
 * from the X0X project with live_bridge_map and marked dirty. Autosave (ui.c's gate, X0X's rule:
 * stopped, quiet, not in LIVE) saves the project and then the song, one after the other. */
#pragma once
#include <stdint.h>
#include "firmware/src/seq/pattern.h"   /* pattern_t (root-relative: -I. is on every build) */
#include "songs_seam.h"                 /* T8i: song_row_t / songs_ops_t (model-free) */

/* Scan the store and adopt slot 0, or build the Starter song from song_templates.c and mark dirty
 * (TCLS: the X0X project is no longer mapped). Call once from ui_init(), after
 * engine_init()/project_load() (the LIVE song is live_bridge's g_song).
 * T8i: `preferred` is the last current slot (the project's reserved byte); it is adopted
 * first, then slot 0, then the template. */
void song_io_boot(uint16_t bpm_x10, int preferred);

int  song_io_loaded(void);          /* 1: the current song came from slot 0 (not mapped from the project) */
int  song_io_dirty(void);           /* the current song changed since the last save */
int  song_io_failed(void);          /* the last save failed; no retry until the next edit (T6i item 3) */
int  song_io_wants_save(void);      /* dirty and not failed: what the autosave gate acts on */
void song_io_mark_dirty(void);
int  song_io_saves(void);           /* successful saves since boot (tests/scenarios) */

/* Save the current song to slot 0. 0 = saved; < 0 = the store refused it (kept in RAM, dirty).
 * `flush` is the autosave path: it saves only when dirty. */
int  song_io_save(void);
int  song_io_flush(void);

/* T6i behaviour 2: an edit to the current song marks it dirty. T10's assign-keys UI calls this. */
void song_io_set_key_clip(int key, int clip);

/* LIVE SETUP: model-free song settings; edits use the normal autosave gate. */
enum { SONG_SETUP_QUANT, SONG_SETUP_LOCK, SONG_SETUP_CHANNEL, SONG_SETUP_PC };
enum { LIVE_ASSIGN_STOP = 27, LIVE_ASSIGN_FX = 32, LIVE_ASSIGN_NOTE = 38, LIVE_ASSIGN_COUNT = 39, LIVE_ASSIGN_NONE = 255 };
int song_io_setup_get(int setting);
void song_io_setup_set(int setting, int value);

/* T7a: the clip under the cursor. Indices are the model's clip order (0..26); a role index is
 * 0..4 (DRUMS..FX). The clip-settings edits mark the song dirty; the copy/clear edits push an undo
 * entry of their own. Creating a clip only fills an unused slot. */
int  song_io_clip_used(int clip);
int  song_io_clip_len(int clip);          /* 0 when unused */
int  song_io_clip_mode(int clip);         /* 0 Loop, 1 Once */
int  song_io_clip_quant(int clip);        /* 0 Bar, 1 Beat, 2 End */
int  song_io_clip_nnotes(int clip);
int  song_io_clip_motion(int clip);       /* the used motion lanes */
int  song_io_clip_role(int clip);
void song_io_clip_create(int clip, int len, int mode, int quant);
void song_io_clip_create_undoable(int clip, int len, int mode, int quant);   /* T7b: + undo entry */
void song_io_clip_clear(int clip);
void song_io_clip_copy(int dst, int src);
void song_io_clip_set_len(int clip, int len);
void song_io_clip_set_mode(int clip, int mode);
void song_io_clip_set_quant(int clip, int quant);
int  song_io_swing(void);
void song_io_set_swing(int v);            /* 0..100 */
void song_io_set_bpm(int bpm_x10);
void song_io_name(char *out, int n);      /* the song's name, NUL-terminated */
int  song_io_key(void);                   /* 0..11 (C..B) */
int  song_io_scale(void);                 /* model.h's SCALE_* */

/* ---- T7b: the clip editor's edits ---------------------------------------- */
/* Each editor session and each record pass is one undo entry: edit_begin when the editor opens,
 * edit_end when it closes, and a mutator calls edit_push() just before it writes the clip (the
 * first push stores the clip as it was before the session, so undo reverts the whole session). */
void song_io_edit_begin(int clip);
void song_io_edit_end(void);
void song_io_edit_push(void);

int  song_io_clip_is_drum(int clip);      /* 1 when the clip's role is DRUMS */

/* Drums: 11 lanes x the clip length, plus a per-step accent. */
int  song_io_drum_get(int clip, int lane, int step);     /* 0 / 1 */
int  song_io_drum_accent(int clip, int step);            /* 0 / 1 */
void song_io_drum_set(int clip, int lane, int step, int on);
void song_io_drum_set_accent(int clip, int step, int on);

/* Notes: step, note (MIDI), vel (1..127), len (steps). */
int  song_io_note_count(int clip);
int  song_io_note_get(int clip, int i, int *step, int *note, int *vel, int *len);
int  song_io_note_index(int clip, int step, int note);   /* -1 when absent */
int  song_io_note_add(int clip, int step, int note, int vel, int len);  /* the index, or -1 */
int  song_io_note_remove(int clip, int step, int note);
void song_io_note_set(int clip, int i, int step, int note, int vel, int len);

/* Motion: two lanes a clip; a lane is one packed param (live_bridge.h's LIVE_PARAM). */
int  song_io_motion_param(int clip, int lane);           /* -1 when the lane is empty */
int  song_io_motion_lane(int clip, int param, int create); /* the lane for a param, or -1 */
int  song_io_motion_get(int clip, int lane, int step);   /* -1 when unset, else 0..127 */
void song_io_motion_set(int clip, int lane, int step, int v);   /* v < 0 clears the cell */
void song_io_motion_clear(int clip, int lane);

/* ---- undo entry points (T7 wires the UI; no UI yet). The clip args are the model's `clip_t *`, passed
 * as void here. `push_*` are called after the edit, with the value it replaced. `undo`/`redo`
 * return model's UNDO_DONE (1) / UNDO_EMPTY (0) / UNDO_CANNOT (-1) and mark the song dirty when
 * they act. `undo_clip`/`redo_clip` name the clip the next undo/redo touches, -1 when none. */
void song_io_push_clear(int clip, const void *old_clip);
void song_io_push_record(int clip, const void *old_clip);
void song_io_push_copy(int dst, int src, const void *old_dst);
int  song_io_undo(void);
int  song_io_redo(void);
int  song_io_undo_type(void);
int  song_io_redo_type(void);
int  song_io_undo_clip(void);           /* the clip the next undo touches, -1 none */
int  song_io_undo_depth(void);
int  song_io_undo_pos(void);
int  song_io_redo_clip(void);           /* the clip the next redo touches, -1 none */

/* Takes (T9i): the model's `take_hdr_t *` / `take_event_t *`, passed as void here. The in-RAM
 * take lives in live_bridge (`live_bridge_take_hdr()` and friends); song_io reads it to store it
 * and writes it back on load/undo. `song_io_take_save`/`_load` are the raw store calls. */
int song_io_take_save(int slot, const void *hdr, const void *ev);
int song_io_take_load(int slot, void *hdr, void *ev, uint32_t max_events);

/* T9i: a finished recording (or a clear) marks the take for the next flush; the store write is
 * deferred until the transport is stopped and quiet, like the song (project_save -> song_io_flush). */
void song_io_take_request_save(void);          /* a recording finished: the in-RAM take is the song's */
void song_io_take_request_clear(void);         /* the clear gesture: on the next flush, store the empty take */
int  song_io_take_pending(void);               /* a take store write is queued */
int  song_io_take_flush(void);                 /* the deferred store write (called by song_io_flush) */
int  song_io_take_load_ram(void);              /* the song's stored take into live_bridge's buffer */
int  song_io_take_loaded_ram(void);            /* 1 while the in-RAM take holds the song's take */

/* ---- T8i: the songs list and its CRUD (a block separate from T7a/T7b's clip API) ------- */
/* The SONGS view reaches the store only through these, via the songs_ops_t table in
 * songs_seam.h. The `ctx` args are unused (the store is static); they exist so the
 * functions are directly assignable to the ops pointers. `cur_slot` is the slot the
 * current song lives in, or -1 when the current song is still the project mapping. */

/* Fill one row (see song_row_t); `current` marks the slot being edited. */
void song_io_songs_list(void *ctx, int slot, song_row_t *out);
int  song_io_songs_current_slot(void *ctx);          /* cur_slot, or -1 */

/* Load `slot` as the current song and reload the clip engine. 0 = ok. The caller stops
 * the transport and silences the voices first. */
int  song_io_songs_open(void *ctx, int slot);

/* Build a template (0 Empty, 1 Starter) and save it into the first free slot. Returns
 * the new slot, or -2 when all are taken, or -1 on a save failure. */
int  song_io_songs_create(void *ctx, int kind, uint16_t bpm_x10, int key);

/* Copy `src` into the first free slot. Returns the new slot or -2. */
int  song_io_songs_copy(void *ctx, int src);

/* Rename `slot` durably (and, if it is current, in RAM). 0 = ok. */
int  song_io_songs_rename(void *ctx, int slot, const char *name);

/* Durable delete + an undo entry. 0 = ok. */
int  song_io_songs_delete(void *ctx, int slot);

/* Undo the most recent delete (UNDO_DONE / UNDO_EMPTY / UNDO_CANNOT). */
int  song_io_songs_undelete(void *ctx);

/* The first free slot: never-used first, then a deleted one, or -2. */
int  song_io_songs_first_free(void *ctx);

/* 1 while the most recent delete can still be undone. */
int  song_io_songs_can_undelete(void *ctx);

/* Used clips in `slot`'s stored song; -1 when unknown. */
int  song_io_songs_clip_count(void *ctx, int slot);

/* The ops table above, for ui_songs_begin. */
const songs_ops_t *song_io_songs_ops(void);
