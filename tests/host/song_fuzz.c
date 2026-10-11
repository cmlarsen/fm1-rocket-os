/* SPDX-License-Identifier: GPL-3.0-only */
/* T6c fuzzer: 100,000 random mutations of a valid stored region must never crash
 * and must never load an invalid song. Run under -fsanitize=address,undefined.
 *
 * Build: cc -std=c99 -I. tests/host/song_fuzz.c core/model/song_store.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "core/model/model.h"
#include "core/model/song_store.h"

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

int main(int argc, char **argv)
{
    song_store_t store;
    song_t s, out;
    take_hdr_t th, thg;
    take_event_t ev[8], evg[TAKE_MAX_EVENTS];
    long iters = argc > 1 ? strtol(argv[1], 0, 10) : 100000L;
    long it;
    int k;

    printf("T6c fuzz: %ld mutations\n", iters);
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

    for (it = 0; it < iters; it++) {
        uint32_t pos[6];
        uint8_t oldb[6];
        int n = (int)(rnd() % 5u) + 1, i;
        for (i = 0; i < n; i++) {
            pos[i] = (uint32_t)(rnd() % REGION_BYTES);
            oldb[i] = nor[SONG_STORE_BASE + pos[i]];
            nor[SONG_STORE_BASE + pos[i]] = (uint8_t)rnd();
        }
        if (song_store_load(&store, 0, &out)) {
            /* rejected: the fallback must still be a valid (empty) song */
        }
        if (song_validate(&out)) {
            printf("FAIL: invalid song after mutation at iteration %ld\n", it);
            return 1;
        }
        if (song_store_take_load(&store, 0, &thg, evg, TAKE_MAX_EVENTS) == 0) {
            if (thg.nevents > TAKE_MAX_EVENTS) {
                printf("FAIL: take nevents out of range at iteration %ld\n", it);
                return 1;
            }
        }
        for (i = n - 1; i >= 0; i--)
            nor[SONG_STORE_BASE + pos[i]] = oldb[i];   /* undo the mutation, keep the region valid */
    }

    /* the region is still exactly the pristine bytes */
    if (memcmp(nor + SONG_STORE_BASE, pristine, REGION_BYTES) != 0) {
        printf("FAIL: the fuzzer changed the region\n");
        return 1;
    }
    printf("fuzz: %ld mutations, no crash, no invalid song\n", iters);
    return 0;
}
