/* SPDX-License-Identifier: GPL-3.0-only */
/* T6c host test of core/model/song_store.c against a simulated NOR flash.
 *
 * The simulator models erase -> 0xFF and program that can only clear bits, does
 * not wrap a 256-byte page, and can be told to stop after N programmed bytes: a
 * power cut at any byte of a save. Every write is range-checked against the
 * store's region. Nothing here touches hardware.
 *
 * Build: cc -std=c99 -I. tests/host/song_store_test.c core/model/song_store.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "core/model/model.h"
#include "core/model/song_store.h"

#define NOR_SIZE 0x100000u
static uint8_t nor[NOR_SIZE];

static long prog_budget = -1;              /* -1 = unlimited; else bytes left before the cut */
static uint32_t wrote_lo, wrote_hi;        /* programming/erase range seen since the last reset */

static int sim_read(void *ctx, uint32_t off, void *dst, uint32_t n)
{
    (void)ctx;
    if (off > NOR_SIZE || n > NOR_SIZE - off)
        return -1;
    memcpy(dst, nor + off, n);
    return 0;
}

static int sim_erase(void *ctx, uint32_t off)
{
    (void)ctx;
    if (off > NOR_SIZE || 4096u > NOR_SIZE - off)
        return -1;
    memset(nor + off, 0xFF, 4096);
    if (off < wrote_lo)
        wrote_lo = off;
    if (off + 4096u > wrote_hi)
        wrote_hi = off + 4096u;
    return 0;
}

static int sim_program(void *ctx, uint32_t off, const void *src, uint32_t n)
{
    const uint8_t *s = (const uint8_t *)src;
    uint32_t i;
    (void)ctx;
    if ((off & 0xFFu) + n > 256u) {
        printf("page wrap at %#x (%u)\n", off, n);
        exit(2);
    }
    if (off > NOR_SIZE || n > NOR_SIZE - off)
        return -1;
    if (prog_budget >= 0) {
        if (prog_budget == 0)
            return -9;
        if (n > (uint32_t)prog_budget) {
            for (i = 0; i < (uint32_t)prog_budget; i++)
                nor[off + i] &= s[i];
            prog_budget = 0;
            return -9;                     /* the power went off inside the page */
        }
        prog_budget -= (long)n;
    }
    for (i = 0; i < n; i++)
        nor[off + i] &= s[i];
    if (off < wrote_lo)
        wrote_lo = off;
    if (off + n > wrote_hi)
        wrote_hi = off + n;
    return 0;
}

static const song_flash_t FLASH = { sim_read, sim_erase, sim_program, 0 };
static song_store_t store;

/* a view of the record header, for the test only */
typedef struct {
    uint32_t magic;
    uint16_t version, kind;
    uint8_t slot, flags;
    uint16_t rawlen;
    uint32_t seq, len, crc;
    char name[SONG_NAME_LEN];
    uint32_t hcrc;
} hdrv_t;

static uint32_t song_ext_off(int i)
{
    return SONG_STORE_BASE + (uint32_t)i * SONG_STORE_SONG_SECTORS * SONG_STORE_SECTOR;
}
static uint32_t take_ext_off(int j)
{
    return SONG_STORE_BASE + (SONG_STORE_SONG_EXTENTS * SONG_STORE_SONG_SECTORS +
                              (uint32_t)j * SONG_STORE_TAKE_SECTORS) * SONG_STORE_SECTOR;
}

static int newest_song_ext(int slot)
{
    int i, best = -1;
    uint32_t best_seq = 0;
    for (i = 0; i < (int)SONG_STORE_SONG_EXTENTS; i++) {
        hdrv_t h;
        memcpy(&h, nor + song_ext_off(i), sizeof h);
        if (h.magic != 0x31545353u || h.kind != 1u || h.slot != (uint8_t)slot)
            continue;
        if (best < 0 || h.seq > best_seq) {
            best = i;
            best_seq = h.seq;
        }
    }
    return best;
}

static uint32_t rng = 0xC0FFEEu;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static int bad;
static void check(const char *what, int ok)
{
    printf("  %-52s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        bad = 1;
}

/* ---- sample songs ------------------------------------------------------- */

static void make_starter(song_t *s)
{
    song_store_empty(s);
    memcpy(s->name, "STARTER", 7);
    s->bpm_x10 = 1240u;
    s->swing = 8u;
    s->quant_default = Q_BAR;
    /* 4 drum, 3 bass, 3 lead, 2 chords, 1 fx */
    static const int drums[4] = {0, 1, 2, 3};
    static const int bass[3] = {7, 8, 9};
    static const int lead[3] = {12, 13, 14};
    static const int chords[2] = {19, 20};
    static const int fx[1] = {24};
    int i, k, j;
    for (i = 0; i < 4; i++) {
        clip_t *c = &s->clip[drums[i]];
        c->used = 1;
        c->len = 16;
        c->mode = CLIP_LOOP;
        c->quant = Q_BAR;
        for (k = 0; k < (int)DRUM_LANES; k += 2)
            drum_set(&c->d.drum, (unsigned)k, (unsigned)(i * 3 + k), 1);
        for (k = 0; k < (int)STEPS_PER_BAR; k += 4)
            c->d.drum.accent[0] |= 1u << k;
    }
    for (i = 0; i < 3; i++) {
        clip_t *c = &s->clip[bass[i]];
        c->used = 1;
        c->len = 16;
        c->nnotes = 6;
        for (k = 0; k < 6; k++) {
            c->d.notes[k].step = (uint8_t)(k * 2);
            c->d.notes[k].note = (uint8_t)(36 + i * 2 + (k % 3));
            c->d.notes[k].vel = 100;
            c->d.notes[k].len = 2;
        }
    }
    for (i = 0; i < 3; i++) {
        clip_t *c = &s->clip[lead[i]];
        c->used = 1;
        c->len = 32;
        c->nnotes = 10;
        for (k = 0; k < 10; k++) {
            c->d.notes[k].step = (uint8_t)(k * 3);
            c->d.notes[k].note = (uint8_t)(60 + i * 3 + (k % 5));
            c->d.notes[k].vel = (uint8_t)(70 + k);
            c->d.notes[k].len = 3;
        }
    }
    for (i = 0; i < 2; i++) {
        clip_t *c = &s->clip[chords[i]];
        c->used = 1;
        c->len = 16;
        c->nnotes = 8;
        for (k = 0; k < 8; k++) {
            c->d.notes[k].step = (uint8_t)(k * 2);
            c->d.notes[k].note = (uint8_t)(48 + (k / 3) * 4 + (k % 3));
            c->d.notes[k].vel = 90;
            c->d.notes[k].len = 4;
        }
    }
    {
        clip_t *c = &s->clip[fx[0]];
        c->used = 1;
        c->len = 16;
        c->nnotes = 2;
        for (k = 0; k < 2; k++) {
            c->d.notes[k].step = (uint8_t)(k * 8);
            c->d.notes[k].note = 70;
            c->d.notes[k].vel = 110;
            c->d.notes[k].len = 1;
        }
    }
    /* 2 motion lanes used (on lead A and chords A) */
    s->clip[lead[0]].motion[0].param = 0x1234u;
    for (j = 0; j < (int)CLIP_MAX_STEPS; j++)
        s->clip[lead[0]].motion[0].v[j] = (uint8_t)(j * 3);
    s->clip[chords[0]].motion[1].param = 0x00ABu;
    for (j = 0; j < (int)CLIP_MAX_STEPS; j += 2)
        s->clip[chords[0]].motion[1].v[j] = (uint8_t)(255 - j);
}

/* every clip full of random, high-entropy but still valid data */
static void make_worst(song_t *s)
{
    int c, k, j;
    song_store_empty(s);
    memcpy(s->name, "WORSTCASE", 9);
    s->bpm_x10 = 2010u;
    s->swing = 63u;
    for (c = 0; c < (int)NCLIPS; c++) {
        clip_t *cl = &s->clip[c];
        cl->used = 1;
        cl->len = CLIP_MAX_STEPS;
        cl->mode = (uint8_t)(rnd() & 1u);
        cl->quant = (uint8_t)(rnd() % NQUANT);
        if (clip_role((uint8_t)c) == ROLE_DRUMS) {
            int lane, w;
            for (lane = 0; lane < (int)DRUM_LANES; lane++)
                for (w = 0; w < (int)CLIP_SW; w++)
                    cl->d.drum.hit[lane][w] = rnd();
            for (w = 0; w < (int)CLIP_SW; w++)
                cl->d.drum.accent[w] = rnd();
        } else {
            cl->nnotes = CLIP_MAX_NOTES;
            for (k = 0; k < (int)CLIP_MAX_NOTES; k++) {
                cl->d.notes[k].step = (uint8_t)(rnd() % CLIP_MAX_STEPS);
                cl->d.notes[k].note = (uint8_t)(rnd() % 128u);
                cl->d.notes[k].vel = (uint8_t)(rnd() % 128u);
                cl->d.notes[k].len = (uint8_t)(1u + rnd() % CLIP_MAX_STEPS);
            }
        }
        for (j = 0; j < (int)CLIP_MOTION_LANES; j++) {
            int n;
            cl->motion[j].param = (uint16_t)rnd();
            for (n = 0; n < (int)CLIP_MAX_STEPS; n++)
                cl->motion[j].v[n] = (uint8_t)rnd();
        }
    }
}

static int songs_equal(const song_t *a, const song_t *b)
{
    return memcmp(a, b, sizeof *a) == 0;
}

/* ---- tests -------------------------------------------------------------- */

static void roundtrip(const char *what, song_t *s, int slot)
{
    song_t got;
    int rc = song_store_save(&store, slot, s);
    memset(&got, 0xA5, sizeof got);
    rc |= song_store_load(&store, slot, &got);
    check(what, rc == 0 && songs_equal(s, &got));
}

static void test_power_cut(const char *name, song_t *old_s, song_t *new_s, int stride)
{
    static uint8_t snap[SONG_STORE_SONG_EXTENTS * SONG_STORE_SONG_SECTORS * SONG_STORE_SECTOR];
    const uint32_t snap_len = (uint32_t)sizeof snap;
    song_t got;
    int n, record_bytes, rc, ok = 1, saw_old = 0, saw_new = 0, cuts = 0;
    memset(nor, 0xFF, sizeof nor);
    song_store_init(&store, &FLASH);
    song_store_save(&store, 0, old_s);
    memcpy(snap, nor + SONG_STORE_BASE, snap_len);
    record_bytes = song_store_measure(new_s) + 40;
    for (n = 0;; n += stride) {
        memcpy(nor + SONG_STORE_BASE, snap, snap_len);
        song_store_init(&store, &FLASH);
        prog_budget = n;
        song_store_save(&store, 0, new_s);
        prog_budget = -1;
        cuts++;
        song_store_init(&store, &FLASH);      /* a power cut, then a reboot: the scan decides */
        memset(&got, 0xA5, sizeof got);
        rc = song_store_load(&store, 0, &got);
        if (songs_equal(&got, old_s))
            saw_old = 1;
        else if (songs_equal(&got, new_s))
            saw_new = 1;
        else {
            printf("  %s power cut at byte %d: bad load (rc=%d)\n", name, n, rc);
            ok = 0;
            break;
        }
        if (rc != 0 || song_validate(&got)) {
            printf("  %s power cut at byte %d: rc=%d\n", name, n, rc);
            ok = 0;
            break;
        }
        if (n >= record_bytes)
            break;
        if (n + stride > record_bytes)
            n = record_bytes - stride;   /* next step lands exactly on record_bytes */
    }
    printf("  %-10s power cut: %d cuts over 0..%d bytes of a %d-byte record\n", name, cuts, record_bytes, record_bytes);
    {
        char l[96];
        snprintf(l, sizeof l, "%s power cut: old or new, never garbage", name);
        check(l, ok);
        snprintf(l, sizeof l, "%s power cut: saw the old copy", name);
        check(l, saw_old);
        snprintf(l, sizeof l, "%s power cut: saw the new copy", name);
        check(l, saw_new);
    }
}

int main(void)
{
    song_t empty, starter, worst, got, raw;
    song_slot_t info;
    int i;

    printf("T6c song store\n");
    printf("  sizeof(song_t)   = %u\n", (unsigned)sizeof(song_t));
    printf("  sizeof(song_store_t) = %u\n", (unsigned)sizeof(song_store_t));
    printf("  region 0x%X..0x%X, %u sectors\n", SONG_STORE_BASE,
           SONG_STORE_BASE + SONG_STORE_SECTORS * SONG_STORE_SECTOR, SONG_STORE_SECTORS);

    song_store_empty(&empty);
    make_starter(&starter);
    make_worst(&worst);
    for (i = 0; i < (int)sizeof raw; i++)
        ((uint8_t *)&raw)[i] = (uint8_t)rnd();

    printf("  compressed: empty %d B, starter %d B, worst(valid) %d B, worst(random) %d B\n",
           song_store_measure(&empty), song_store_measure(&starter),
           song_store_measure(&worst), song_store_measure(&raw));
    check("empty song compresses to a few hundred bytes", song_store_measure(&empty) < 512);

    memset(nor, 0xFF, sizeof nor);
    song_store_init(&store, &FLASH);
    wrote_lo = 0xFFFFFFFFu;
    wrote_hi = 0u;

    roundtrip("round trip: empty", &empty, 0);
    roundtrip("round trip: starter", &starter, 1);
    roundtrip("round trip: worst", &worst, 2);

    /* list */
    song_store_list(&store, 1, &info);
    check("list: used", info.used == 1u);
    check("list: name", memcmp(info.name, "STARTER", 7) == 0);
    check("list: no take yet", info.has_take == 0u);
    song_store_list(&store, 4, &info);
    check("list: empty slot is unused", info.used == 0u);

    /* newest valid wins, and a corrupt newest falls back to the older copy */
    {
        int ext;
        song_store_save(&store, 3, &empty);
        song_store_save(&store, 3, &starter);
        ext = newest_song_ext(3);
        check("newest copy wins", ext >= 0 && song_store_load(&store, 3, &got) == 0 && songs_equal(&got, &starter));
        nor[song_ext_off(ext) + 40 + 100] ^= 0x40;       /* bit rot in the newest payload */
        check("corrupt newest -> the older copy loads",
              song_store_load(&store, 3, &got) == 0 && songs_equal(&got, &empty));
    }

    /* a completely unreadable slot falls back to an empty song, never garbage */
    {
        int j;
        for (j = 0; j < (int)SONG_STORE_SONG_EXTENTS; j++)
            nor[song_ext_off(j)] = 0x00;                  /* break every magic */
        memset(&got, 0xA5, sizeof got);
        check("all copies broken -> empty song, error",
              song_store_load(&store, 3, &got) == -1 && songs_equal(&got, &empty));
    }

    /* delete / undelete keep the data */
    {
        song_store_save(&store, 0, &starter);
        check("delete", song_store_delete(&store, 0) == 0 && song_store_is_deleted(&store, 0) == 1);
        song_store_list(&store, 0, &info);
        check("list: deleted, still used", info.used == 1u && info.deleted == 1u);
        check("undelete", song_store_undelete(&store, 0) == 0 && song_store_is_deleted(&store, 0) == 0);
        check("data survived delete", song_store_load(&store, 0, &got) == 0 && songs_equal(&got, &starter));
    }

    /* takes: save, replace, the previous stays loadable, swap (undo) */
    {
        take_hdr_t h1, h2, hg;
        take_event_t e1[3], e2[5], eg[TAKE_MAX_EVENTS];
        int k;
        memset(&h1, 0, sizeof h1);
        memset(&h2, 0, sizeof h2);
        h1.nevents = 3;
        h1.bpm_x10 = 1200;
        h1.length_samples = 1000;
        for (k = 0; k < 3; k++) {
            e1[k].sample = (uint32_t)k * 100u;
            e1[k].cmd = 0x100u + (uint32_t)k;
        }
        h2.nevents = 5;
        h2.bpm_x10 = 1210;
        h2.length_samples = 2000;
        for (k = 0; k < 5; k++) {
            e2[k].sample = (uint32_t)k * 90u;
            e2[k].cmd = 0x200u + (uint32_t)k;
        }
        check("take save 1", song_store_take_save(&store, 1, &h1, e1) == 0);
        song_store_list(&store, 1, &info);
        check("list: has_take", info.has_take == 1u);
        check("take load 1", song_store_take_load(&store, 1, &hg, eg, TAKE_MAX_EVENTS) == 0 &&
              hg.nevents == 3 && eg[2].cmd == 0x102u);
        check("take replace 2", song_store_take_save(&store, 1, &h2, e2) == 0);
        check("take load 2", song_store_take_load(&store, 1, &hg, eg, TAKE_MAX_EVENTS) == 0 && hg.nevents == 5);
        check("take swap (undo)", song_store_take_swap(&store, 1) == 1);
        check("take load 1 after undo", song_store_take_load(&store, 1, &hg, eg, TAKE_MAX_EVENTS) == 0 &&
              hg.nevents == 3 && eg[2].cmd == 0x102u);
        check("take swap (redo)", song_store_take_swap(&store, 1) == 1);
        check("take load 2 after redo", song_store_take_load(&store, 1, &hg, eg, TAKE_MAX_EVENTS) == 0 &&
              hg.nevents == 5);
        check("take load with too small a buffer is refused",
              song_store_take_load(&store, 1, &hg, eg, 2) == -1);
        {
            int j, found = -1;
            for (j = 0; j < (int)SONG_STORE_TAKE_EXTENTS; j++) {
                hdrv_t hv;
                memcpy(&hv, nor + take_ext_off(j), sizeof hv);
                if (hv.magic == 0x31545353u && hv.kind == 2u && hv.slot == 1u) {
                    found = j;
                    break;
                }
            }
            check("take record sits in the take extent area", found >= 0);
        }
    }

    /* all writes stayed inside the region */
    check("every erase/program inside 0xA0000..0xDC000",
          wrote_lo >= SONG_STORE_BASE && wrote_hi <= SONG_STORE_BASE + SONG_STORE_SECTORS * SONG_STORE_SECTOR);

    /* validation rejects malformed fields, and save refuses them */
    {
        song_t v = starter;
        check("valid starter accepted", song_validate(&v) == 0);
        v.clip[7].len = 200;
        check("len > 64 rejected", song_validate(&v) != 0);
        v = starter;
        v.clip[12].nnotes = 100;
        check("nnotes > 96 rejected", song_validate(&v) != 0);
        v = starter;
        v.role[0].nclips = 3;
        check("bad role range rejected", song_validate(&v) != 0);
        v = starter;
        v.magic = 0;
        check("bad song magic rejected", song_validate(&v) != 0);
        v = starter;
        v.clip[13].d.notes[0].note = 250;
        check("note > 127 rejected", song_validate(&v) != 0);
        v = starter;
        v.clip[7].len = 200;
        check("save refuses a malformed song", song_store_save(&store, 4, &v) == -2);
    }

    /* power cut at every byte of a save, for a small record and the large
     * worst-case record */
    test_power_cut("a:small", &empty, &starter, 1);
    test_power_cut("b:large", &starter, &worst, 1);

    printf("%s\n", bad ? "SONG STORE TEST FAILED" : "song store test passed");
    return bad ? 1 : 0;
}
