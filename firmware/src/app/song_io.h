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

/* Scan the store and adopt slot 0, or build from the project and mark dirty. Call once from
 * ui_init(), after engine_init()/project_load() (the LIVE song is live_bridge's g_song). */
void song_io_boot(const pattern_t *pats, uint16_t bpm_x10);

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
void song_io_clip_clear(int clip);
void song_io_clip_copy(int dst, int src);
void song_io_clip_set_len(int clip, int len);
void song_io_clip_set_mode(int clip, int mode);
void song_io_clip_set_quant(int clip, int quant);
int  song_io_swing(void);
void song_io_set_swing(int v);            /* 0..100 */
void song_io_set_bpm(int bpm_x10);
void song_io_name(char *out, int n);      /* the song's name, NUL-terminated */

/* Undo entry points (T7 wires the UI; no UI yet). The clip args are the model's `clip_t *`, passed
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

/* Takes (T9i): the model's `take_hdr_t *` / `take_event_t *`, passed as void here. Nothing calls
 * them yet. */
int song_io_take_save(int slot, const void *hdr, const void *ev);
int song_io_take_load(int slot, void *hdr, void *ev, uint32_t max_events);
