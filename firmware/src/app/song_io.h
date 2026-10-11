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
void song_io_mark_dirty(void);
int  song_io_saves(void);           /* successful saves since boot (tests/scenarios) */

/* Save the current song to slot 0. 0 = saved; < 0 = the store refused it (kept in RAM, dirty).
 * `flush` is the autosave path: it saves only when dirty. */
int  song_io_save(void);
int  song_io_flush(void);

/* T6i behaviour 2: an edit to the current song marks it dirty. T10's assign-keys UI calls this. */
void song_io_set_key_clip(int key, int clip);

/* Undo entry points (T7 wires the UI; no UI yet). The clip args are the model's `clip_t *`, passed
 * as void here. `push_*` are called after the edit, with the value it replaced. `undo`/`redo`
 * return model's UNDO_DONE / UNDO_EMPTY / UNDO_CANNOT and mark the song dirty when they act. */
void song_io_push_clear(int clip, const void *old_clip);
void song_io_push_record(int clip, const void *old_clip);
void song_io_push_copy(int dst, int src, const void *old_dst);
int  song_io_undo(void);
int  song_io_redo(void);
int  song_io_undo_type(void);
int  song_io_redo_type(void);

/* Takes (T9i): the model's `take_hdr_t *` / `take_event_t *`, passed as void here. Nothing calls
 * them yet. */
int song_io_take_save(int slot, const void *hdr, const void *ev);
int song_io_take_load(int slot, void *hdr, void *ev, uint32_t max_events);
