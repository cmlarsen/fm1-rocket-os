/* SPDX-License-Identifier: GPL-3.0-only */
/* T6i host test of firmware/src/app/song_io.c against a simulated song flash.
 *
 * The simulator is the same shape as tests/host/song_store_test.c's: erase -> 0xFF, program that
 * only clears bits and never wraps a 256-byte page, a write region of 0xA0000..0xDBFFF, a cut
 * that can kill the Nth flash op (the "power cut during an autosave" case) and a one-shot program
 * failure (the "failed save retries on a different extent" case). live_bridge's song is a RAM
 * song_t (the real one lives in live_bridge.c); its hooks are stubbed here.
 *
 * Build: cc -std=c99 -I. -DX0X_HOST tests/host/song_io_test.c firmware/src/app/song_io.c \
 *        core/model/song_store.c core/model/undo_stack.c */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "core/model/model.h"
#include "core/model/song_store.h"
#include "core/model/undo_stack.h"
#include "firmware/src/app/live_bridge.h"
#include "firmware/src/app/song_io.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* ---- the simulated song flash (0xA0000..0xDBFFF) ---- */
#define NOR_BASE SONG_STORE_BASE
#define NOR_SECTORS SONG_STORE_SECTORS
#define NOR_SIZE (NOR_SECTORS * SONG_STORE_SECTOR)
static uint8_t nor[NOR_SIZE];
static long cut_at = -1, ops;                  /* the op index that dies (-1 none) */
static int fail_prog_once;                     /* the next program fails (item 3) */
static uint32_t wrote_lo = 0xFFFFFFFFu, wrote_hi;

static uint32_t rel(uint32_t off) { return off - NOR_BASE; }
static int in_region(uint32_t off, uint32_t n)
{
    return off >= NOR_BASE && rel(off) <= NOR_SIZE && n <= NOR_SIZE - rel(off);
}
static void note(uint32_t off, uint32_t n)
{
    if (off < wrote_lo) wrote_lo = off;
    if (off + n > wrote_hi) wrote_hi = off + n;
}
int plat_song_read(uint32_t off, void *dst, uint32_t n)
{
    if (!in_region(off, n))
        return -1;
    memcpy(dst, nor + rel(off), n);
    return 0;
}
int plat_song_erase(uint32_t off)
{
    if (!in_region(off, SONG_STORE_SECTOR) || (off & (SONG_STORE_SECTOR - 1u)))
        return -1;
    if (cut_at >= 0 && ops >= cut_at) {         /* the power died mid-erase */
        cut_at = -1;
        return -1;
    }
    ops++;
    memset(nor + rel(off), 0xFF, SONG_STORE_SECTOR);
    note(off, SONG_STORE_SECTOR);
    return 0;
}
int plat_song_program(uint32_t off, const void *src, uint32_t n)
{
    const uint8_t *s = (const uint8_t *)src;
    uint32_t i;
    if (!in_region(off, n) || n == 0 || n > 256u || ((off & 0xFFu) + n) > 256u)
        return -1;
    if (fail_prog_once) {                        /* a bad block: the write is refused */
        fail_prog_once = 0;
        return -9;
    }
    if (cut_at >= 0 && ops >= cut_at) {          /* the power died inside the page */
        for (i = 0; i < n / 2u; i++)
            nor[rel(off) + i] &= s[i];
        cut_at = -1;
        return -9;
    }
    ops++;
    for (i = 0; i < n; i++)
        nor[rel(off) + i] &= s[i];
    note(off, n);
    return 0;
}

/* the extent holding slot 0's newest song record (highest seq), -1 if none */
static int slot0_extent(void)
{
    int i, best = -1;
    uint32_t best_seq = 0;
    for (i = 0; i < (int)SONG_STORE_SONG_EXTENTS; i++) {
        uint32_t o = rel(SONG_STORE_BASE + (uint32_t)i * SONG_STORE_SONG_SECTORS * SONG_STORE_SECTOR);
        uint32_t m = (uint32_t)nor[o] | (uint32_t)nor[o + 1u] << 8 | (uint32_t)nor[o + 2u] << 16 |
                     (uint32_t)nor[o + 3u] << 24;
        uint32_t seq;
        if (m != 0x31545353u)
            continue;
        seq = (uint32_t)nor[o + 12u] | (uint32_t)nor[o + 13u] << 8 | (uint32_t)nor[o + 14u] << 16 |
              (uint32_t)nor[o + 15u] << 24;
        if (best < 0 || seq > best_seq) {
            best = i;
            best_seq = seq;
        }
    }
    return best;
}

/* ---- the live_bridge hooks song_io uses ---- */
static song_t g_song;
static uint32_t g_proj_hash = 0x1111u;          /* the test's "project": change it to simulate an edit */
static uint32_t g_song_hash, g_rebuilds, g_baselines, g_reloads, g_changed;
void *live_bridge_song(void) { return &g_song; }
void live_bridge_song_reload(void) { g_reloads++; }
uint32_t live_bridge_pats_hash(const pattern_t *pats) { (void)pats; return g_proj_hash; }
uint32_t live_bridge_song_hash(void) { return g_song_hash; }
uint32_t live_bridge_rebuild(const pattern_t *pats)
{
    song_t keep = g_song;                       /* the real one keeps the song's own fields */
    (void)pats;
    g_rebuilds++;
    song_store_empty(&g_song);
    if (keep.magic == SONG_MAGIC)
        memcpy(g_song.key_clip, keep.key_clip, NKEYS);
    if (keep.name[0])
        memcpy(g_song.name, keep.name, SONG_NAME_LEN);
    else {
        g_song.name[0] = 'P';                   /* "built from the project" marker */
        g_song.name[1] = 0;
    }
    g_song_hash = g_proj_hash;
    return g_song_hash;
}
void live_bridge_song_baseline(uint32_t h) { g_baselines++; g_song_hash = h; }
void live_bridge_clip_changed(uint8_t clip) { (void)clip; g_changed++; }   /* T7b: no engine in this test */
uint8_t live_bridge_playing_clip(uint8_t role) { (void)role; return 0xFFu; }
void live_bridge_stop(uint8_t role) { (void)role; }
int live_bridge_stop_queued(int role) { (void)role; return 0; }
int live_bridge_transport_playing(void) { return 0; }   /* T8i: the test is never playing */

/* ---- T9i: the take buffer song_io reads/writes (a plain in-RAM stub) ---- */
static take_hdr_t g_take_hdr;
static take_event_t g_take_ev[TAKE_MAX_EVENTS];
static int g_take_n;
const void *live_bridge_take_hdr(void) { return &g_take_hdr; }
const void *live_bridge_take_events(void) { return g_take_ev; }
void *live_bridge_take_hdr_mut(void) { return &g_take_hdr; }
void *live_bridge_take_events_mut(void) { return g_take_ev; }
int live_bridge_take_nevents(void) { return g_take_n; }
void live_bridge_take_loaded(int nevents, uint32_t length_samples, uint16_t bpm_x10)
{
    g_take_n = nevents;
    g_take_hdr.nevents = (uint16_t)nevents;
    g_take_hdr.length_samples = length_samples;
    g_take_hdr.bpm_x10 = bpm_x10;
}
void live_bridge_take_clear_ram(void) { g_take_n = 0; g_take_hdr.nevents = 0; g_take_hdr.length_samples = 0; }
int live_bridge_take_recording(void) { return 0; }       /* T9i r1: never live in this test */
int live_bridge_take_armed(void) { return 0; }
int live_bridge_take_playing(void) { return 0; }

static void reset_flash(void)
{
    memset(nor, 0xFF, sizeof nor);
    cut_at = -1;
    ops = 0;
    fail_prog_once = 0;
    wrote_lo = 0xFFFFFFFFu;
    wrote_hi = 0;
}
static void boot(void)
{
    song_io_boot(0, 1250u, 0);                  /* pats is unused by the stub; T8i: no preferred slot */
}

/* ---- tests ---- */

static void test_boot_empty_builds(void)
{
    reset_flash();
    g_rebuilds = g_baselines = 0;
    g_proj_hash = 0x1111u;
    boot();
    CHECK(song_io_loaded() == 0);
    CHECK(song_io_dirty() == 1);                /* built from the project: write it back */
    CHECK(g_rebuilds == 1);
    CHECK(((song_t *)live_bridge_song())->name[0] == 'P');
}

static void test_save_reload(void)
{
    reset_flash();
    g_proj_hash = 0x1111u;
    boot();
    song_io_set_key_clip(0, 5);
    CHECK(song_io_save() == 0);
    CHECK(song_io_dirty() == 0);
    CHECK(song_io_saves() == 1);
    CHECK(wrote_lo >= NOR_BASE && wrote_hi <= NOR_BASE + NOR_SIZE);

    g_rebuilds = g_baselines = 0;
    song_io_set_key_clip(0, 9);                 /* a further edit, not saved */
    boot();                                     /* reboot: slot 0 wins, the hash matches */
    CHECK(song_io_loaded() == 1);
    CHECK(song_io_dirty() == 0);
    CHECK(g_baselines == 1 && g_rebuilds == 0); /* no rebuild: the clips still match the project */
    CHECK(((song_t *)live_bridge_song())->key_clip[0] == 5);
}

/* T7a: the song is the edited object. A change to the project's patterns no longer rebuilds a
 * stored song; slot 0 is adopted as it is, key map and all (the T6i pattern-hash remap is gone). */
static void test_project_change_kept(void)
{
    reset_flash();
    g_proj_hash = 0x1111u;
    boot();
    song_io_set_key_clip(0, 5);
    song_io_set_key_clip(1, 6);
    CHECK(song_io_save() == 0);                 /* the record carries tag 0x1111 */
    g_proj_hash = 0x2222u;                      /* the project's patterns changed in STUDIO */
    g_rebuilds = g_baselines = 0;
    boot();
    CHECK(song_io_loaded() == 1);
    CHECK(song_io_dirty() == 0);                /* used as it is: nothing to write back */
    CHECK(g_rebuilds == 0 && g_baselines == 1); /* no rebuild: the song is the object */
    CHECK(((song_t *)live_bridge_song())->key_clip[0] == 5);   /* the assignments survive */
    CHECK(((song_t *)live_bridge_song())->key_clip[1] == 6);
}

/* T7a: the clip edits the song home makes (create, length/mode/quant, clear, copy) and undo/redo */
static void test_clip_edits(void)
{
    clip_t before;
    reset_flash();
    g_proj_hash = 0x1111u;
    boot();
    CHECK(song_io_clip_used(0) == 0);
    song_io_clip_create(0, 16, 0, 0);
    CHECK(song_io_clip_used(0) == 1);
    CHECK(song_io_clip_len(0) == 16 && song_io_clip_mode(0) == 0 && song_io_clip_quant(0) == 0);
    song_io_clip_set_len(0, 32);
    song_io_clip_set_mode(0, 1);
    song_io_clip_set_quant(0, 2);
    CHECK(song_io_clip_len(0) == 32 && song_io_clip_mode(0) == 1 && song_io_clip_quant(0) == 2);
    CHECK(song_io_dirty() == 1);
    /* clear pushes an undo entry; undo restores the clip, redo clears it again */
    song_io_clip_clear(0);
    CHECK(song_io_clip_used(0) == 0);
    CHECK(song_io_undo_type() == 1 /* UNDO_CLEAR_CLIP */ && song_io_undo_clip() == 0);
    CHECK(song_io_undo() == 1);
    CHECK(song_io_clip_used(0) == 1 && song_io_clip_len(0) == 32);
    CHECK(song_io_redo() == 1 && song_io_clip_used(0) == 0);
    CHECK(song_io_undo() == 1 && song_io_clip_used(0) == 1);
    /* copy: create a second clip, copy 0 over 1, undo restores 1's old (empty) clip */
    before = ((song_t *)live_bridge_song())->clip[1];
    song_io_clip_create(1, 16, 0, 0);
    song_io_clip_copy(1, 0);
    CHECK(song_io_clip_len(1) == 32);
    CHECK(song_io_undo_type() == 2 /* UNDO_COPY_CLIP */ && song_io_undo_clip() == 1);
    CHECK(song_io_undo() == 1);
    CHECK(song_io_clip_used(1) == 1 && song_io_clip_len(1) == 16);   /* the copied-in clip it replaced */
    (void)before;
}

/* T7b: the clip editor's edits — drums, notes and motion, each editor session one undo entry. */
static void test_clip_editor_ops(void)
{
    reset_flash();
    g_proj_hash = 0x1111u;
    boot();
    /* a DRUMS clip: the session (three edits) is one undo entry */
    song_io_clip_create(0, 16, 0, 0);
    CHECK(song_io_clip_is_drum(0) == 1);
    song_io_edit_begin(0);
    song_io_drum_set(0, 0, 0, 1);
    song_io_drum_set(0, 1, 4, 1);
    song_io_drum_set_accent(0, 0, 1);
    song_io_edit_end();
    CHECK(song_io_drum_get(0, 0, 0) == 1 && song_io_drum_get(0, 0, 1) == 0);
    CHECK(song_io_drum_get(0, 1, 4) == 1 && song_io_drum_accent(0, 0) == 1);
    CHECK(song_io_undo_depth() == 1);
    CHECK(song_io_undo() == 1);
    CHECK(song_io_drum_get(0, 0, 0) == 0 && song_io_drum_accent(0, 0) == 0);
    CHECK(song_io_undo_pos() == 0);

    /* a melodic clip: notes, a chord on one step, and the same note not added twice */
    song_io_clip_create(7, 16, 0, 0);
    CHECK(song_io_clip_is_drum(7) == 0);
    song_io_edit_begin(7);
    CHECK(song_io_note_add(7, 0, 60, 100, 1) == 0);
    CHECK(song_io_note_add(7, 0, 64, 100, 1) == 1);
    CHECK(song_io_note_add(7, 0, 60, 100, 1) == 0);
    CHECK(song_io_note_count(7) == 2);
    CHECK(song_io_note_index(7, 0, 64) == 1);
    {
        int st, no, ve, le;
        CHECK(song_io_note_get(7, 1, &st, &no, &ve, &le) == 1);
        CHECK(st == 0 && no == 64 && ve == 100 && le == 1);
    }
    song_io_note_remove(7, 0, 64);
    CHECK(song_io_note_count(7) == 1);
    song_io_edit_end();
    CHECK(song_io_undo() == 1);                 /* the session: back to the empty clip */
    CHECK(song_io_note_count(7) == 0);

    /* motion: two lanes at most; the same param reuses its lane */
    song_io_clip_create(7, 16, 0, 0);
    song_io_edit_begin(7);
    CHECK(song_io_motion_lane(7, 0x6001, 1) == 0);
    CHECK(song_io_motion_lane(7, 0x6001, 1) == 0);
    CHECK(song_io_motion_lane(7, 0x4002, 1) == 1);
    CHECK(song_io_motion_lane(7, 0x5003, 1) == -1);        /* two lanes at most */
    song_io_motion_set(7, 0, 3, 99);
    CHECK(song_io_motion_get(7, 0, 3) == 99 && song_io_motion_get(7, 0, 2) == -1);
    song_io_motion_clear(7, 0);
    CHECK(song_io_motion_param(7, 0) == -1);
    song_io_edit_end();
}

/* T7b: undo of REC on an empty slot removes the clip it created (the creation is in the session). */
static void test_clip_create_undo(void)
{
    reset_flash();
    g_proj_hash = 0x1111u;
    boot();
    CHECK(song_io_clip_used(7) == 0);
    song_io_edit_begin(7);
    song_io_clip_create_undoable(7, 16, 0, 0);
    CHECK(song_io_clip_used(7) == 1);
    CHECK(song_io_note_add(7, 0, 60, 100, 1) == 0);     /* the pass continues in the same entry */
    song_io_edit_end();
    CHECK(song_io_undo_depth() == 1);
    CHECK(song_io_undo() == 1);
    CHECK(song_io_clip_used(7) == 0);                   /* the create is undone too */
    CHECK(song_io_redo() == 1);
    CHECK(song_io_clip_used(7) == 1 && song_io_note_count(7) == 1);
}

/* T7b re-review 4: undo stages the restored clip through the main-loop path; it does NOT re-init
 * the clip engine (which would stop the music while it plays). Only song open / load re-inits. */
static void test_undo_stages_not_reloads(void)
{
    reset_flash();
    g_proj_hash = 0x1111u;
    boot();
    song_io_clip_create(7, 16, 0, 0);
    song_io_edit_begin(7);
    song_io_note_add(7, 0, 60, 100, 1);
    song_io_edit_end();
    CHECK(g_changed > 0);                               /* the edit staged the clip */
    g_changed = 0;
    g_reloads = 0;
    CHECK(song_io_undo() == 1);
    CHECK(g_changed > 0);                               /* undo staged the restored clip */
    CHECK(g_reloads == 0);                              /* ...and did not re-init the engine */
    CHECK(song_io_note_count(7) == 0);
}

static void test_backend_refuses_out_of_region(void)
{
    uint8_t b[8];
    CHECK(plat_song_read(NOR_BASE - 1u, b, 1) != 0);
    CHECK(plat_song_read(NOR_BASE + NOR_SIZE, b, 1) != 0);
    CHECK(plat_song_read(NOR_BASE + NOR_SIZE - 4u, b, 8) != 0);       /* a range that runs past the end */
    CHECK(plat_song_erase(NOR_BASE - SONG_STORE_SECTOR) != 0);
    CHECK(plat_song_erase(NOR_BASE + NOR_SIZE) != 0);
    CHECK(plat_song_erase(NOR_BASE + 1u) != 0);                       /* not sector-aligned */
    CHECK(plat_song_program(NOR_BASE - 4u, b, 4) != 0);
    CHECK(plat_song_program(NOR_BASE + NOR_SIZE - 4u, b, 8) != 0);
    CHECK(plat_song_program(NOR_BASE, b, 300) != 0);                  /* more than a page */
    CHECK(plat_song_erase(NOR_BASE) == 0);                            /* the real region is accepted */
    CHECK(plat_song_program(NOR_BASE, b, 4) == 0);
}

static void test_power_cut_keeps_previous(void)
{
    reset_flash();
    g_proj_hash = 0x1111u;
    boot();
    song_io_set_key_clip(0, 5);
    CHECK(song_io_save() == 0);                 /* v1 */
    song_io_set_key_clip(0, 9);
    cut_at = ops;                               /* the power dies on the next flash op of the save */
    CHECK(song_io_save() < 0);                  /* the save fails... */
    CHECK(song_io_dirty() == 1);                /* ...and the song stays in RAM, dirty */
    boot();                                     /* a reboot scans what committed */
    CHECK(song_io_loaded() == 1);
    CHECK(((song_t *)live_bridge_song())->key_clip[0] == 5);   /* the previous song */
}

/* item 3: a failed save shows an error, does not retry on a timer, and the next edit saves on
 * another extent */
static void test_failed_save_no_retry(void)
{
    int ext0, ext1;
    reset_flash();
    g_proj_hash = 0x3333u;
    boot();
    song_io_set_key_clip(0, 5);
    CHECK(song_io_save() == 0);
    ext0 = slot0_extent();
    CHECK(ext0 >= 0);

    song_io_set_key_clip(0, 9);
    fail_prog_once = 1;                         /* the next program op fails */
    CHECK(song_io_save() < 0);
    CHECK(song_io_failed() == 1);
    CHECK(song_io_dirty() == 1);
    CHECK(song_io_wants_save() == 0);           /* nothing to do until an edit */
    {
        long o = ops;
        CHECK(song_io_flush() == 0);            /* the autosave path: a no-op, not a retry */
        CHECK(song_io_flush() == 0);
        CHECK(ops == o);                        /* no further erase/program happened */
    }
    song_io_set_key_clip(0, 11);                /* a new edit re-enables saving */
    CHECK(song_io_failed() == 0);
    CHECK(song_io_wants_save() == 1);
    CHECK(song_io_save() == 0);                 /* succeeds, on a different extent */
    ext1 = slot0_extent();
    CHECK(ext1 >= 0 && ext1 != ext0);
    boot();
    CHECK(((song_t *)live_bridge_song())->key_clip[0] == 11);
}

static void test_corrupt_record_falls_back(void)
{
    reset_flash();
    g_proj_hash = 0x1111u;
    boot();
    song_io_set_key_clip(0, 7);
    CHECK(song_io_save() == 0);
    {
        uint32_t o;
        for (o = 0; o + 4u <= NOR_SIZE; o += SONG_STORE_SECTOR) {   /* flip every record's magic */
            uint32_t m = (uint32_t)nor[o] | (uint32_t)nor[o + 1u] << 8 |
                         (uint32_t)nor[o + 2u] << 16 | (uint32_t)nor[o + 3u] << 24;
            if (m == 0x31545353u)
                nor[o] = 0;
        }
    }
    g_rebuilds = g_baselines = 0;
    boot();
    CHECK(song_io_loaded() == 0);               /* nothing valid: the project mapping */
    CHECK(song_io_dirty() == 1);
    CHECK(g_rebuilds == 1);
}

/* T9i r2/S5: with a full undo ring, `undo.cur` saturates, so ordering a pending take by cur index
 * wrongly cancelled it. A newer clip edit must be undone first; the take goes when nothing newer is
 * above it. */
static void test_take_undo_order_full_ring(void)
{
    clip_t old;
    int i, rc;
    reset_flash();
    boot();
    song_io_clip_create(0, 16, 0, 0);           /* a clip to clear (undoably) */
    memset(&old, 0, sizeof old);
    for (i = 0; i < 12; i++)
        song_io_push_clear(0, &old);            /* overfill the 11-deep ring */
    CHECK(song_io_undo_depth() == 11);
    song_io_take_request_save();
    CHECK(song_io_take_pending());
    song_io_push_clear(0, &old);                /* a newer edit while the ring is full */
    rc = song_io_undo();
    CHECK(rc == 1 /* UNDO_DONE */);
    CHECK(song_io_take_pending());              /* S5: the newer edit went first, not the take */
    rc = song_io_undo();
    CHECK(rc == 1);
    CHECK(!song_io_take_pending());             /* now nothing newer: the take is cancelled */
}

/* T9i r3/NIT: the durable take undo is stamped when the take ended, not at the deferred flush, so a
 * clip edit made after the take is undone first. */
static void test_take_undo_order_flush(void)
{
    clip_t old;
    reset_flash();
    g_proj_hash = 0x1111u;
    boot();
    live_bridge_take_loaded(3, 1234u, 1200u);   /* the take buffer holds a finished recording */
    song_io_take_request_save();                /* the recording ended: stamp the undo position */
    CHECK(song_io_take_pending());
    memset(&old, 0, sizeof old);
    song_io_push_clear(0, &old);                /* a clip edit made after the take ended */
    CHECK(song_io_take_flush() == 0);           /* the deferred write happens now */
    CHECK(!song_io_take_pending());
    CHECK(song_io_undo_type() == 1 /* UNDO_CLEAR_CLIP */);      /* the newer edit is on top ... */
    CHECK(song_io_undo() == 1);
    CHECK(song_io_undo_type() == 4 /* UNDO_REPLACE_TAKE */);    /* ... then the take */
}

int main(void)
{
    test_boot_empty_builds();
    test_save_reload();
    test_project_change_kept();
    test_clip_edits();
    test_clip_editor_ops();
    test_clip_create_undo();
    test_undo_stages_not_reloads();
    test_backend_refuses_out_of_region();
    test_power_cut_keeps_previous();
    test_failed_save_no_retry();
    test_corrupt_record_falls_back();
    test_take_undo_order_full_ring();
    test_take_undo_order_flush();
    if (fails) {
        printf("song_io: %d FAILED\n", fails);
        return 1;
    }
    printf("song_io: boot, save/reboot, project rebuild, clip edits, the editor ops, region guard, power cut, failed save, corrupt fallback all pass\n");
    return 0;
}
