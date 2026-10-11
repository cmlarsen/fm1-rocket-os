/* SPDX-License-Identifier: GPL-3.0-only */
/* T6i host test of firmware/src/app/song_io.c against a simulated song flash.
 *
 * The simulator is the same shape as tests/host/song_store_test.c's: erase -> 0xFF, program that
 * only clears bits and never wraps a 256-byte page, a write region of 0xA0000..0xDBFFF, and a cut
 * that can kill the Nth flash op (the "power cut during an autosave" case). live_bridge's song is
 * a RAM song_t (the real one lives in live_bridge.c); its map/reload/lock hooks are stubbed here.
 *
 * Build: cc -std=c99 -I. tests/host/song_io_test.c firmware/src/app/song_io.c \
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
    if (cut_at >= 0 && ops >= cut_at) {         /* the power died inside the page */
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

/* ---- the live_bridge hooks song_io uses ---- */
static song_t g_song;
static int g_locked, g_reloads, g_maps;
void *live_bridge_song(void) { return &g_song; }
void live_bridge_song_reload(void) { g_reloads++; }
void live_bridge_lock_song(int on) { g_locked = on; }
void live_bridge_map(const pattern_t *pats, void *songp)
{
    (void)pats;
    g_maps++;
    song_store_empty((song_t *)songp);
    ((song_t *)songp)->name[0] = 'P';           /* "built from the project" marker */
    ((song_t *)songp)->name[1] = 0;
}

static void reset_flash(void)
{
    memset(nor, 0xFF, sizeof nor);
    cut_at = -1;
    ops = 0;
    wrote_lo = 0xFFFFFFFFu;
    wrote_hi = 0;
}
static int boot(void)
{
    song_io_boot(0, 1250u);                     /* pats is unused by the stub */
    return 0;
}

/* ---- tests ---- */

static void test_boot_empty_builds(void)
{
    reset_flash();
    g_maps = 0;
    boot();
    CHECK(song_io_loaded() == 0);
    CHECK(song_io_dirty() == 1);                /* built from the project: write it back */
    CHECK(g_locked == 0);                       /* LIVE may still re-derive from the project */
    CHECK(g_maps == 1);
    CHECK(((song_t *)live_bridge_song())->name[0] == 'P');
}

static void test_save_reload(void)
{
    int loaded_first;
    reset_flash();
    boot();
    song_io_set_key_clip(0, 5);
    CHECK(song_io_save() == 0);
    CHECK(song_io_dirty() == 0);
    CHECK(song_io_saves() == 1);
    CHECK(wrote_lo >= NOR_BASE && wrote_hi <= NOR_BASE + NOR_SIZE);

    song_io_set_key_clip(0, 9);                 /* a further edit, not saved */
    boot();                                     /* reboot: slot 0 wins */
    loaded_first = song_io_loaded();
    CHECK(loaded_first == 1);
    CHECK(song_io_dirty() == 0);
    CHECK(g_locked == 1);                       /* LIVE must not re-derive over a stored song */
    CHECK(((song_t *)live_bridge_song())->key_clip[0] == 5);
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

static void test_corrupt_record_falls_back(void)
{
    reset_flash();
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
    g_maps = 0;
    boot();
    CHECK(song_io_loaded() == 0);               /* nothing valid: the project mapping */
    CHECK(song_io_dirty() == 1);
    CHECK(g_maps == 1);
}

int main(void)
{
    test_boot_empty_builds();
    test_save_reload();
    test_backend_refuses_out_of_region();
    test_power_cut_keeps_previous();
    test_corrupt_record_falls_back();
    if (fails) {
        printf("song_io: %d FAILED\n", fails);
        return 1;
    }
    printf("song_io: boot, save/reboot, region guard, power cut, corrupt fallback all pass\n");
    return 0;
}
