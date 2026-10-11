/* SPDX-License-Identifier: GPL-3.0-only */
/* T6i: songs on real flash — see song_io.h.
 *
 * This is a separate translation unit (song_store.c includes X0X's lz.c, and the device build's
 * x0x.c already includes it through project.c, so the two must not share one). It talks to the
 * model (song_store.h / undo_stack.h) and to the LIVE song, which lives in live_bridge.c, and
 * nothing else. The flash is the platform's plat_song_* backend (plat_fm1.c on the device,
 * host/x0x_host.c in the simulator), pinned to 0xA0000..0xDBFFF.
 *
 * The store and the undo ring are static here: song_store_t (144 B) and undo_stack_t (~12 KiB,
 * UNDO_STACK_DEPTH entries). On the device both go in .pool (X0X_POOL), like the clip engine's. */
#include <stdint.h>
#include "song_flash.h"                 /* the plat_song_* backend (no plat.h: its NKEYS clashes) */
#include "live_bridge.h"
#include "core/model/song_store.h"
#include "core/model/undo_stack.h"

/* the device's .pool section, as live_bridge.c/engine.c spell it (plat.h's X0X_POOL without the
 * NKEYS collision) */
#if defined(X0X_HOST)
#define SONG_POOL
#else
#define SONG_POOL __attribute__((section(".pool")))
#endif

/* the platform's three byte-level functions as the store's interface */
static int fb_read(void *ctx, uint32_t off, void *dst, uint32_t n) { (void)ctx; return plat_song_read(off, dst, n); }
static int fb_erase(void *ctx, uint32_t off) { (void)ctx; return plat_song_erase(off); }
static int fb_program(void *ctx, uint32_t off, const void *src, uint32_t n) { (void)ctx; return plat_song_program(off, src, n); }
static const song_flash_t FB = { fb_read, fb_erase, fb_program, 0 };

static song_store_t store SONG_POOL;
static undo_stack_t undo SONG_POOL;
static uint8_t ready, dirty, loaded;
static uint32_t saves;

static song_t *song(void) { return (song_t *)live_bridge_song(); }

void song_io_boot(const pattern_t *pats, uint16_t bpm_x10)
{
    song_store_init(&store, &FB);
    undo_stack_init(&undo);
    ready = 0;
    loaded = 0;
    dirty = 0;
    if (song_store_load(&store, 0, song()) == 0) {      /* slot 0 holds a valid, undeleted song */
        loaded = 1;
        live_bridge_lock_song(1);                       /* LIVE uses it as it is; no fresh mapping */
    } else {
        live_bridge_lock_song(0);
        live_bridge_map(pats, song());
        if (bpm_x10 < 200u)
            bpm_x10 = 200u;
        else if (bpm_x10 > 3000u)
            bpm_x10 = 3000u;
        song()->bpm_x10 = bpm_x10;
        dirty = 1;                                      /* built from the project: write it back */
    }
    ready = 1;
    live_bridge_song_reload();                          /* the clip engine follows the new song */
}

int song_io_loaded(void) { return loaded != 0; }
int song_io_dirty(void) { return dirty != 0; }
int song_io_saves(void) { return (int)saves; }
void song_io_mark_dirty(void) { dirty = 1; }

int song_io_save(void)
{
    int rc;
    if (!ready)
        return -1;
    rc = song_store_save(&store, 0, song());
    if (rc == 0) {
        dirty = 0;
        saves++;
    }
    return rc;
}

int song_io_flush(void)
{
    if (!dirty)
        return 0;
    return song_io_save();
}

void song_io_set_key_clip(int key, int clip)
{
    if (key < 0 || key >= (int)NKEYS)
        return;
    if (clip != (int)CLIP_NONE && (clip < 0 || clip > (int)NCLIPS))
        return;
    song()->key_clip[key] = (uint8_t)clip;
    dirty = 1;
}

/* ---- undo ---------------------------------------------------------------- */

void song_io_push_clear(int clip, const void *old_clip)
{
    undo_stack_push_clear(&undo, (uint8_t)clip, (const clip_t *)old_clip);
}
void song_io_push_record(int clip, const void *old_clip)
{
    undo_stack_push_record(&undo, (uint8_t)clip, (const clip_t *)old_clip);
}
void song_io_push_copy(int dst, int src, const void *old_dst)
{
    undo_stack_push_copy(&undo, (uint8_t)dst, (uint8_t)src, (const clip_t *)old_dst);
}

static int undo_run(int redo)
{
    int rc = redo ? undo_stack_redo(&undo, song(), &store, 0) : undo_stack_undo(&undo, song(), &store, 0);
    if (rc == UNDO_DONE) {
        dirty = 1;                                      /* the song changed: write it back on the next save */
        live_bridge_song_reload();
    }
    return rc;
}

int song_io_undo(void) { return undo_run(0); }
int song_io_redo(void) { return undo_run(1); }
int song_io_undo_type(void) { return (int)undo_stack_undo_type(&undo); }
int song_io_redo_type(void) { return (int)undo_stack_redo_type(&undo); }

/* ---- takes (T9i) --------------------------------------------------------- */

int song_io_take_save(int slot, const void *hdr, const void *ev)
{
    return song_store_take_save(&store, slot, (const take_hdr_t *)hdr, (const take_event_t *)ev);
}
int song_io_take_load(int slot, void *hdr, void *ev, uint32_t max_events)
{
    return song_store_take_load(&store, slot, (take_hdr_t *)hdr, (take_event_t *)ev, max_events);
}
