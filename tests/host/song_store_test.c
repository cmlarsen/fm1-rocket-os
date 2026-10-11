/* SPDX-License-Identifier: GPL-3.0-only */
/* T6c host test of core/model/song_store.c against a simulated NOR flash.
 *
 * The simulator models erase -> 0xFF and program that can only clear bits, does
 * not wrap a 256-byte page, and can be cut at the Nth programmed byte (a power
 * cut) or the Nth flash operation (a torn erase or program). Every write is
 * range-checked against the store's region. Nothing here touches hardware.
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

static long prog_budget = -1;              /* byte cut: -1 = unlimited, else bytes left */
static long op_at = -1, op_count;          /* op cut: the operation index that dies */
static int op_dead;
static int fail_next_prog;                 /* T6i: the next program fails (a bad block) */
static uint32_t wrote_lo, wrote_hi;

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
    if (op_dead)
        return -1;
    if (op_at >= 0 && op_count == op_at) {         /* the power died mid-erase */
        memset(nor + off, 0xFF, 1500);
        op_dead = 1;
        return -1;
    }
    op_count++;
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
    if (fail_next_prog) {                          /* T6i: a bad block: refuse this program */
        fail_next_prog = 0;
        return -9;
    }
    if (op_dead)
        return -1;
    if (op_at >= 0 && op_count == op_at) {         /* the power died mid-program */
        for (i = 0; i < n / 2u; i++)
            nor[off + i] &= s[i];
        op_dead = 1;
        return -1;
    }
    op_count++;
    if (prog_budget >= 0) {
        if (prog_budget == 0)
            return -9;
        if (n > (uint32_t)prog_budget) {
            for (i = 0; i < (uint32_t)prog_budget; i++)
                nor[off + i] &= s[i];
            prog_budget = 0;
            return -9;                         /* the power went off inside the page */
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
    uint32_t phash;                            /* T6i */
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

static int newest_song_ext(int slot, int want_deleted_ok)
{
    int i, best = -1;
    uint32_t best_seq = 0;
    for (i = 0; i < (int)SONG_STORE_SONG_EXTENTS; i++) {
        hdrv_t h;
        memcpy(&h, nor + song_ext_off(i), sizeof h);
        if (h.magic != 0x31545353u || h.kind != 1u || h.slot != (uint8_t)slot)
            continue;
        if (!want_deleted_ok && (h.flags & 1u))
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
    s->clip[lead[0]].motion[0].param = 0x0123u;    /* a T5i-range param (< 0x0800) */
    for (j = 0; j < (int)CLIP_MAX_STEPS; j++)
        s->clip[lead[0]].motion[0].v[j] = (uint8_t)(j * 2);      /* 0..126 */
    s->clip[chords[0]].motion[1].param = 0x00ABu;
    for (j = 0; j < (int)CLIP_MAX_STEPS; j += 2)
        s->clip[chords[0]].motion[1].v[j] = (uint8_t)(127 - (j / 2));
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
                cl->d.notes[k].vel = (uint8_t)(1u + rnd() % 127u);
                cl->d.notes[k].len = (uint8_t)(1u + rnd() % CLIP_MAX_STEPS);
            }
        }
        for (j = 0; j < (int)CLIP_MOTION_LANES; j++) {
            int n;
            cl->motion[j].param = (uint16_t)(rnd() % 0x800u);       /* valid T5i range */
            for (n = 0; n < (int)CLIP_MAX_STEPS; n++)
                cl->motion[j].v[n] = (uint8_t)((rnd() & 1u) ? MOTION_NONE : (rnd() & 0x7Fu));
        }
    }
}

static int songs_equal(const song_t *a, const song_t *b)
{
    return memcmp(a, b, sizeof *a) == 0;
}

static void roundtrip(const char *what, song_t *s, int slot)
{
    song_t got;
    int rc = song_store_save(&store, slot, s);
    memset(&got, 0xA5, sizeof got);
    rc |= song_store_load(&store, slot, &got);
    check(what, rc == 0 && songs_equal(s, &got));
}

/* ---- power cut at every byte of a save --------------------------------- */

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
    record_bytes = song_store_measure(new_s) + 44;   /* the record: payload + the 44-byte header */
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
            n = record_bytes - stride;
    }
    printf("  %-10s byte cut: %d cuts over 0..%d bytes of a %d-byte record\n", name, cuts, record_bytes, record_bytes);
    {
        char l[96];
        snprintf(l, sizeof l, "%s byte cut: old or new, never garbage", name);
        check(l, ok);
        snprintf(l, sizeof l, "%s byte cut: saw the old copy", name);
        check(l, saw_old);
        snprintf(l, sizeof l, "%s byte cut: saw the new copy", name);
        check(l, saw_new);
    }
}

/* ---- power cut at every flash operation across a burst of saves --------- */

static void mkfill(song_t *s, char c)
{
    song_store_empty(s);
    memset(s->name, c, 4);
}

static void test_power_cut_ops(void)
{
    static uint8_t snap[0x100000];
    song_t a1, a2, b1, b2, o;
    long total, c;
    int bad_cuts = 0, s;
    mkfill(&a1, 'A');
    a1.name[1] = '1';
    mkfill(&a2, 'A');
    a2.name[1] = '2';
    mkfill(&b1, 'B');
    b1.name[1] = '1';
    mkfill(&b2, 'B');
    b2.name[1] = '2';

    memset(nor, 0xFF, sizeof nor);
    song_store_init(&store, &FLASH);
    for (s = 0; s < (int)SONG_STORE_SLOTS; s++) {      /* fill every slot: only the spare is free */
        song_t x;
        mkfill(&x, 'F');
        x.name[3] = (char)('0' + s);
        song_store_save(&store, s, &x);
    }
    song_store_save(&store, 0, &a1);
    song_store_save(&store, 1, &b1);
    memcpy(snap, nor, sizeof nor);

    op_at = -1;
    op_count = 0;
    op_dead = 0;
    song_store_init(&store, &FLASH);
    song_store_save(&store, 0, &a2);
    song_store_save(&store, 0, &a1);
    song_store_save(&store, 1, &b2);
    total = op_count;

    for (c = 0; c < total; c++) {
        memcpy(nor, snap, sizeof nor);
        op_count = 0;
        op_dead = 0;
        op_at = c;
        song_store_init(&store, &FLASH);
        song_store_save(&store, 0, &a2);
        song_store_save(&store, 0, &a1);
        song_store_save(&store, 1, &b2);
        op_at = -1;
        op_dead = 0;
        song_store_init(&store, &FLASH);
        if (song_store_load(&store, 0, &o) || (memcmp(o.name, "A1", 2) && memcmp(o.name, "A2", 2)))
            bad_cuts++;
        if (song_store_load(&store, 1, &o) || (memcmp(o.name, "B1", 2) && memcmp(o.name, "B2", 2)))
            bad_cuts++;
        for (s = 2; s < (int)SONG_STORE_SLOTS; s++)
            if (song_store_load(&store, s, &o) || memcmp(o.name, "F", 1))
                bad_cuts++;
    }
    printf("  op cut: %ld flash operations across three saves\n", total);
    {
        char l[96];
        snprintf(l, sizeof l, "op cut at every flash op: all slots old or new (%ld cuts)", total);
        check(l, bad_cuts == 0);
    }
    op_at = -1;
    op_dead = 0;
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
    check("empty song validates", song_validate(&empty) == 0);
    check("starter song validates", song_validate(&starter) == 0);
    check("worst-case song validates", song_validate(&worst) == 0);

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
        ext = newest_song_ext(3, 1);
        check("newest copy wins", ext >= 0 && song_store_load(&store, 3, &got) == 0 && songs_equal(&got, &starter));
        nor[song_ext_off(ext) + 44 + 100] ^= 0x40;       /* bit rot in the newest payload */
        check("corrupt newest -> the older copy loads",
              song_store_load(&store, 3, &got) == 0 && songs_equal(&got, &empty));
        for (i = 0; i < (int)SONG_STORE_SONG_EXTENTS; i++)
            nor[song_ext_off(i)] = 0x00;                  /* break every magic */
        memset(&got, 0xA5, sizeof got);
        check("all copies broken -> empty song, error",
              song_store_load(&store, 3, &got) == -1 && songs_equal(&got, &empty));
    }

    /* T6i: the record's tag survives a reboot, and a failed save switches extent (item 3) */
    {
        int ext0, ext1;
        song_t bad;
        memset(nor, 0xFF, sizeof nor);
        song_store_init(&store, &FLASH);
        song_store_empty(&empty);
        bad = empty;
        bad.magic = 0u;
        check("save_tag refuses a malformed song", song_store_save_tag(&store, 0, &bad, 0x55u) == -2);
        check("save_tag commits", song_store_save_tag(&store, 0, &empty, 0xDEADBEEFu) == 0);
        check("tag round trip", song_store_tag(&store, 0) == 0xDEADBEEFu);
        song_store_init(&store, &FLASH);                  /* a reboot re-scans */
        check("tag survives a reboot", song_store_tag(&store, 0) == 0xDEADBEEFu);
        ext0 = newest_song_ext(0, 1);
        fail_next_prog = 1;                               /* the next program fails */
        check("a failed save errors", song_store_save_tag(&store, 0, &starter, 0x1234u) != 0);
        check("the failed save left the old song",
              song_store_load(&store, 0, &got) == 0 && songs_equal(&got, &empty));
        check("the failed save did not take the new tag", song_store_tag(&store, 0) == 0xDEADBEEFu);
        check("the next save succeeds on another extent",
              song_store_save_tag(&store, 0, &starter, 0x1234u) == 0 &&
              (ext1 = newest_song_ext(0, 1)) >= 0 && ext1 != ext0);
        check("the new tag is in charge", song_store_tag(&store, 0) == 0x1234u);
        memset(nor, 0xFF, sizeof nor);                    /* leave the region clean for the next block */
        song_store_init(&store, &FLASH);
    }

    /* delete survives a reboot, keeps the data, and undelete brings it back */
    {
        song_store_save(&store, 0, &starter);
        check("delete", song_store_delete(&store, 0) == 0 && song_store_is_deleted(&store, 0) == 1);
        song_store_init(&store, &FLASH);                  /* reboot */
        check("delete survives reboot", song_store_is_deleted(&store, 0) == 1);
        song_store_list(&store, 0, &info);
        check("list reports a deleted slot as free",
              info.used == 0u && info.deleted == 1u && info.name[0] == 0 && info.has_take == 0u);
        check("load of a deleted slot is refused (free)", song_store_load(&store, 0, &got) == -1);
        check("undelete", song_store_undelete(&store, 0) == 0 && song_store_is_deleted(&store, 0) == 0);
        song_store_init(&store, &FLASH);                  /* reboot */
        song_store_list(&store, 0, &info);
        check("undelete survives reboot", song_store_is_deleted(&store, 0) == 0 && info.used == 1u);
        check("data survived delete+undelete",
              song_store_load(&store, 0, &got) == 0 && songs_equal(&got, &starter));
        song_store_delete(&store, 0);
        song_store_save(&store, 0, &empty);               /* a save into a deleted slot supersedes it */
        song_store_init(&store, &FLASH);
        check("save into a deleted slot supersedes it",
              song_store_is_deleted(&store, 0) == 0 && song_store_load(&store, 0, &got) == 0 &&
              songs_equal(&got, &empty));
    }

    /* takes: save, replace, the previous stays loadable, restore survives reboot */
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
        check("take restore (undo)", song_store_take_restore(&store, 1, &hg, eg, TAKE_MAX_EVENTS) == 0);
        song_store_init(&store, &FLASH);                  /* reboot: the restored take is newest */
        check("restored take survives reboot",
              song_store_take_load(&store, 1, &hg, eg, TAKE_MAX_EVENTS) == 0 && hg.nevents == 3 &&
              eg[2].cmd == 0x102u);
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

    /* validator (the review's items 5a-5e) */
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

        v = empty;
        memset(v.name, 'A', SONG_NAME_LEN);
        check("unterminated name rejected", song_validate(&v) != 0);
        v = empty;
        v.bpm_x10 = 65535;
        check("bpm 6553.5 rejected", song_validate(&v) != 0);
        v = empty;
        v.bpm_x10 = 199;
        check("bpm 19.9 rejected", song_validate(&v) != 0);
        v = empty;
        v.bpm_x10 = 200;
        check("bpm 20.0 accepted", song_validate(&v) == 0);
        v = empty;
        v.bpm_x10 = 3000;
        check("bpm 300.0 accepted", song_validate(&v) == 0);
        v = empty;
        v.bpm_x10 = 3001;
        check("bpm 300.1 rejected", song_validate(&v) != 0);
        v = empty;
        v.clip[7].used = 1;
        v.clip[7].len = 4;
        v.clip[7].nnotes = 1;
        v.clip[7].d.notes[0].step = 63;
        v.clip[7].d.notes[0].len = 4;
        v.clip[7].d.notes[0].note = 60;
        v.clip[7].d.notes[0].vel = 100;
        check("note step >= clip.len rejected", song_validate(&v) != 0);
        v = empty;
        v.clip[7].used = 1;
        v.clip[7].len = 4;
        v.clip[7].nnotes = 1;
        v.clip[7].d.notes[0].step = 0;
        v.clip[7].d.notes[0].len = 0;
        v.clip[7].d.notes[0].note = 60;
        v.clip[7].d.notes[0].vel = 100;
        check("note len 0 rejected", song_validate(&v) != 0);
        v = empty;
        v.clip[7].used = 1;
        v.clip[7].len = 4;
        v.clip[7].nnotes = 1;
        v.clip[7].d.notes[0].step = 0;
        v.clip[7].d.notes[0].len = 4;
        v.clip[7].d.notes[0].note = 60;
        v.clip[7].d.notes[0].vel = 0;
        check("note vel 0 rejected", song_validate(&v) != 0);
        v = empty;
        v.clip[0].motion[0].param = 0x0123u;
        v.clip[0].motion[0].v[0] = 200;
        check("motion value 200 rejected", song_validate(&v) != 0);
        v = empty;
        v.clip[0].motion[0].param = 0x1234u;
        check("motion param 0x1234 rejected", song_validate(&v) != 0);
        v = empty;
        v.clip[0].motion[0].param = 0x0800u;
        check("motion param 0x0800 rejected", song_validate(&v) != 0);
        v = empty;
        v.clip[0].motion[0].param = 0x07FFu;
        check("motion param 0x07FF accepted", song_validate(&v) == 0);
        v = empty;
        v.clip[0].motion[0].param = PARAM_NONE;
        v.clip[0].motion[0].v[5] = MOTION_NONE;
        check("MOTION_NONE and PARAM_NONE accepted", song_validate(&v) == 0);
        v = empty;
        v.live_channel = 17;
        check("live_channel 17 rejected", song_validate(&v) != 0);
        v = empty;
        v.live_channel = 16;
        check("live_channel 16 accepted", song_validate(&v) == 0);
    }

    /* TLSET: an actual compressed v1 record made with pre-TLSET store/model code. */
    {
        song_t old, got;
        FILE *f = fopen("tests/fixtures/tlset-v1-song.bin", "rb");
        check("v1 fixture exists", f != 0);
        memset(nor, 255, sizeof nor);
        if (f) {
            check("v1 fixture extent read", fread(nor + SONG_STORE_BASE, 1, 16384, f) == 16384);
            fclose(f);
        }
        song_store_init(&store, &FLASH);
        check("v1 compressed song loads and migrates", song_store_load(&store, 0, &old) == 0);
        check("v1 mapping/options preserved", old.version == MODEL_VERSION && old.key_clip[0] == 20 &&
              old.key_clip[26] == CLIP_NONE && old.live_channel == 3 && old.pc_launch == 1 && old.stage_lock == 1);
        check("migrated v1 saves as v2", song_store_save(&store, 0, &old) == 0);
        song_store_init(&store, &FLASH);
        check("migrated v1 round trip", song_store_load(&store, 0, &got) == 0 && !memcmp(&got, &old, sizeof got));
        old.key_clip[0] = KEY_STOP_FIRST;
        old.key_clip[1] = KEY_FX_FIRST + 5;
        old.key_clip[2] = KEY_LIVE_NOTE;
        check("v2 all target kinds save", song_store_save(&store, 0, &old) == 0);
        song_store_init(&store, &FLASH);
        check("v2 assignments round trip", song_store_load(&store, 0, &got) == 0 && !memcmp(&got, &old, sizeof got));
        old.key_clip[0] = KEY_TARGET_COUNT;
        check("v2 reserved target rejected", song_validate(&old) != 0);
        {
            song_t v1 = got;
            unsigned t;
            int bad = 0, k;
            for (k = 0; k < (int)NKEYS; k++)
                got.key_clip[k] = (uint8_t)k;
            v1 = got;
            v1.version = 1u;
            v1.key_clip[0] = 26u;
            check("v1 clip target migrates", song_migrate(&v1) == 0 && v1.version == MODEL_VERSION);
            for (t = NCLIPS; t < KEY_TARGET_COUNT; t++) {
                v1 = got;
                v1.version = 1u;
                v1.key_clip[0] = (uint8_t)t;
                if (song_migrate(&v1) == 0)
                    bad++;
            }
            check("v1 rejects non-clip bytes 27-38", bad == 0);
            v1 = got;
            v1.version = 1u;
            v1.key_clip[0] = CLIP_NONE;
            check("v1 unassigned key migrates", song_migrate(&v1) == 0);
        }
    }

    /* power cut at every byte of a save, for a small record and the large
     * worst-case record */
    test_power_cut("a:small", &empty, &starter, 1);
    test_power_cut("b:large", &starter, &worst, 1);
    test_power_cut_ops();

    printf("%s\n", bad ? "SONG STORE TEST FAILED" : "song store test passed");
    return bad ? 1 : 0;
}
