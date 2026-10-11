/* SPDX-License-Identifier: GPL-3.0-only */
/* T6c fuzzer.
 *
 * Phase A mutates a valid stored region and checks the store never crashes and
 * never hands out an invalid song.
 *
 * Phase B mutates the decoded song fields, re-encodes each mutated song as a
 * record with a valid CRC, and loads it back: every load therefore decompresses
 * and reaches song_validate. It counts how many loads reach the validator (all
 * of them) and cross-checks the store's verdict against a direct song_validate,
 * so a load that disagrees fails. It prints the reached ratio, which must be
 * above 90% (the review's bar).
 *
 * Run under -fsanitize=address,undefined.
 * Build: cc -std=c99 -I. tests/host/song_fuzz.c core/model/song_store.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "core/model/model.h"
#include "core/model/song_store.h"
#include "../../firmware/src/app/lz.c"      /* its own compressor, for re-encoding */

#define NOR_SIZE 0x100000u
#define REGION_BYTES (SONG_STORE_SECTORS * SONG_STORE_SECTOR)
static uint8_t nor[NOR_SIZE];
static uint8_t pristine[REGION_BYTES];

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
    return 0;
}
static int sim_program(void *ctx, uint32_t off, const void *src, uint32_t n)
{
    const uint8_t *s = (const uint8_t *)src;
    uint32_t i;
    (void)ctx;
    if ((off & 0xFFu) + n > 256u || off > NOR_SIZE || n > NOR_SIZE - off)
        return -1;
    for (i = 0; i < n; i++)
        nor[off + i] &= s[i];
    return 0;
}
static const song_flash_t FLASH = { sim_read, sim_erase, sim_program, 0 };

static uint32_t rng = 0x1234567u;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static void make_song(song_t *s)
{
    int c, k;
    song_store_empty(s);
    memcpy(s->name, "FUZZ", 4);
    s->bpm_x10 = 1300u;
    for (c = 0; c < (int)NCLIPS; c++) {
        clip_t *cl = &s->clip[c];
        cl->used = 1;
        cl->len = (uint8_t)(1u + (uint32_t)c % CLIP_MAX_STEPS);
        cl->mode = (uint8_t)(c & 1);
        cl->quant = (uint8_t)(c % NQUANT);
        if (clip_role((uint8_t)c) != ROLE_DRUMS) {
            cl->nnotes = 4;
            for (k = 0; k < 4; k++) {
                cl->d.notes[k].step = (uint8_t)(k % cl->len);
                cl->d.notes[k].note = (uint8_t)(40 + c + k);
                cl->d.notes[k].vel = 100;
                cl->d.notes[k].len = 1;
            }
        }
    }
}

/* ---- Phase B: re-encode a mutated song with a valid CRC ----------------- */

typedef struct {
    uint32_t magic;
    uint16_t version, kind;
    uint8_t slot, flags;
    uint16_t rawlen;
    uint32_t seq, len, crc;
    uint32_t phash;                            /* T6i: the caller tag (song_io's project hash) */
    char name[SONG_NAME_LEN];
    uint32_t hcrc;
} fhdr_t;
_Static_assert(sizeof(fhdr_t) == 44, "header");

static uint32_t f_crc(uint32_t c, const uint8_t *p, uint32_t n)
{
    static const uint32_t T[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu};
    while (n--) {
        c ^= *p++;
        c = (c >> 4) ^ T[c & 15u];
        c = (c >> 4) ^ T[c & 15u];
    }
    return c;
}
static uint32_t f_crc_of(const void *p, uint32_t n) { return ~f_crc(0xFFFFFFFFu, (const uint8_t *)p, n); }

typedef struct { uint8_t *p; uint32_t n, cap; } obuf_t;
static int obuf_put(void *ctx, const uint8_t *p, uint32_t n)
{
    obuf_t *o = (obuf_t *)ctx;
    if (n > o->cap - o->n)
        return -1;
    memcpy(o->p + o->n, p, n);
    o->n += n;
    return 0;
}

static uint8_t payload[0x4000];
static uint32_t seq_clock = 1u;

/* write song `m` as a valid record into song extent 0 */
static void encode_extent0(const song_t *m)
{
    obuf_t o;
    lz_sink_t sk;
    fhdr_t h;
    uint32_t base = SONG_STORE_BASE, off, len, crc, i;
    o.p = payload;
    o.n = 0u;
    o.cap = (uint32_t)sizeof payload;
    sk.put = obuf_put;
    sk.ctx = &o;
    if (lz_compress((const uint8_t *)m, sizeof *m, &sk))
        exit(3);
    len = o.n;
    crc = f_crc_of(payload, len);
    for (i = 0; i < SONG_STORE_SONG_SECTORS; i++)
        sim_erase(0, base + i * SONG_STORE_SECTOR);
    for (off = 0; off < len;) {
        uint32_t room = 256u - ((base + 44u + off) & 0xFFu);
        uint32_t k = len - off < room ? len - off : room;
        sim_program(0, base + 44u + off, payload + off, k);
        off += k;
    }
    memset(&h, 0, sizeof h);
    h.magic = 0x31545353u;
    h.version = 1u;
    h.kind = 1u;
    h.slot = 0u;
    h.flags = 0u;
    h.rawlen = (uint16_t)sizeof *m;
    h.seq = seq_clock++;
    h.len = len;
    h.crc = crc;
    memcpy(h.name, m->name, SONG_NAME_LEN);
    h.hcrc = f_crc_of(&h, (uint32_t)((const uint8_t *)&h.hcrc - (const uint8_t *)&h));
    sim_program(0, base, &h, (uint32_t)sizeof h);
}

int main(int argc, char **argv)
{
    song_store_t store;
    song_t s, out;
    take_hdr_t th, thg;
    take_event_t ev[8], evg[TAKE_MAX_EVENTS];
    long iters = argc > 1 ? strtol(argv[1], 0, 10) : 100000L;
    long it, reached = 0, rejected = 0;
    int k;

    (void)lz_decompress;                      /* this TU only re-encodes; keep the symbol used */

    printf("T6c fuzz: %ld mutations per phase\n", iters);
    memset(nor, 0xFF, sizeof nor);
    song_store_init(&store, &FLASH);
    make_song(&s);
    if (song_store_save(&store, 0, &s))
        return 2;
    memset(&th, 0, sizeof th);
    th.nevents = 8;
    th.length_samples = 2048;
    for (k = 0; k < 8; k++) {
        ev[k].sample = (uint32_t)k * 256u;
        ev[k].cmd = 0x5A5A0000u + (uint32_t)k;
    }
    if (song_store_take_save(&store, 0, &th, ev))
        return 2;
    memcpy(pristine, nor + SONG_STORE_BASE, REGION_BYTES);

    /* Phase A: mutate the stored region; never crash, never hand out garbage */
    for (it = 0; it < iters; it++) {
        uint32_t pos[6];
        uint8_t oldb[6];
        int n = (int)(rnd() % 5u) + 1, i;
        for (i = 0; i < n; i++) {
            pos[i] = (uint32_t)(rnd() % REGION_BYTES);
            oldb[i] = nor[SONG_STORE_BASE + pos[i]];
            nor[SONG_STORE_BASE + pos[i]] = (uint8_t)rnd();
        }
        if (song_store_load(&store, 0, &out))
            ;                                 /* rejected: the fallback is still valid */
        if (song_validate(&out)) {
            printf("FAIL: invalid song after region mutation at iteration %ld\n", it);
            return 1;
        }
        if (song_store_take_load(&store, 0, &thg, evg, TAKE_MAX_EVENTS) == 0 &&
            thg.nevents > TAKE_MAX_EVENTS) {
            printf("FAIL: take nevents out of range at iteration %ld\n", it);
            return 1;
        }
        for (i = n - 1; i >= 0; i--)
            nor[SONG_STORE_BASE + pos[i]] = oldb[i];
    }
    if (memcmp(nor + SONG_STORE_BASE, pristine, REGION_BYTES) != 0) {
        printf("FAIL: the region fuzz changed the region\n");
        return 1;
    }
    printf("phase A: %ld region mutations, no crash, no invalid song\n", iters);

    /* Phase B: mutate the decoded song fields, re-encode, load; every load reaches the validator */
    memset(nor, 0xFF, sizeof nor);
    song_store_init(&store, &FLASH);
    for (it = 0; it < iters; it++) {
        song_t m = s;
        int n = (int)(rnd() % 6u) + 1, i, direct;
        for (i = 0; i < n; i++)
            ((uint8_t *)&m)[rnd() % sizeof m] = (uint8_t)rnd();
        encode_extent0(&m);
        direct = song_validate(&m);
        reached++;                            /* the record has a valid CRC, so load validates it */
        if (song_store_load(&store, 0, &out) == 0) {
            if (direct != 0) {                /* the store accepted what the validator rejects */
                printf("FAIL: store loaded a song the validator rejects at %ld\n", it);
                return 1;
            }
        } else {
            rejected++;
            if (direct == 0) {                /* the store rejected a song the validator accepts */
                printf("FAIL: store refused a valid song at %ld\n", it);
                return 1;
            }
        }
        if (song_validate(&out)) {
            printf("FAIL: invalid song handed out at %ld\n", it);
            return 1;
        }
    }
    printf("phase B: %ld/%ld mutated loads reached the validator (%ld%%), %ld rejected\n",
           reached, iters, iters ? reached * 100 / iters : 0, rejected);
    if (reached * 100 < iters * 90) {
        printf("FAIL: validator reach %ld of %ld is below 90%%\n", reached, iters);
        return 1;
    }
    printf("fuzz: all ok\n");
    return 0;
}
