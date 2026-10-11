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
static uint32_t g_song_hash, g_rebuilds, g_baselines, g_reloads;
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
    song_io_boot(0, 1250u);                     /* pats is unused by the stub */
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

/* item 2: a STUDIO pattern edit (the project hash changed) rebuilds at boot, keeping the key map */
static void test_project_change_rebuilds(void)
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
    CHECK(song_io_dirty() == 1);                /* the rebuild marks the song dirty */
    CHECK(g_rebuilds == 1 && g_baselines == 0);
    CHECK(live_bridge_song_hash() == 0x2222u);
    CHECK(((song_t *)live_bridge_song())->key_clip[0] == 5);   /* the assignments survive */
    CHECK(((song_t *)live_bridge_song())->key_clip[1] == 6);
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

int main(void)
{
    test_boot_empty_builds();
    test_save_reload();
    test_project_change_rebuilds();
    test_backend_refuses_out_of_region();
    test_power_cut_keeps_previous();
    test_failed_save_no_retry();
    test_corrupt_record_falls_back();
    if (fails) {
        printf("song_io: %d FAILED\n", fails);
        return 1;
    }
    printf("song_io: boot, save/reboot, project rebuild, region guard, power cut, failed save, corrupt fallback all pass\n");
    return 0;
}
