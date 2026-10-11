/* SPDX-License-Identifier: GPL-3.0-only */
/* T6c host test of core/model/undo_stack.c.
 *
 * Clip entries carry the old clip_t and swap with the song; take and delete
 * entries name a slot whose recoverable bytes live in the store. A new edit
 * clears the redo tail, and the ring drops the oldest when full. The store is
 * the same RAM simulator as song_store_test.c.
 *
 * Build: cc -std=c99 -I. tests/host/undo_stack_test.c core/model/undo_stack.c core/model/song_store.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "core/model/model.h"
#include "core/model/song_store.h"
#include "core/model/undo_stack.h"

#define NOR_SIZE 0x100000u
static uint8_t nor[NOR_SIZE];
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
    if ((off & 0xFFu) + n > 256u)
        return -1;
    for (i = 0; i < n; i++)
        nor[off + i] &= s[i];
    return 0;
}
static const song_flash_t FLASH = { sim_read, sim_erase, sim_program, 0 };

static int bad;
static void check(const char *what, int ok)
{
    printf("  %-52s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        bad = 1;
}

static void fill_clip(clip_t *c, uint8_t seed)
{
    int i;
    memset(c, 0, sizeof *c);
    c->used = 1;
    c->len = 16;
    c->mode = CLIP_LOOP;
    c->quant = Q_BAR;
    c->nnotes = 4;
    for (i = 0; i < 4; i++) {
        c->d.notes[i].step = (uint8_t)i;
        c->d.notes[i].note = (uint8_t)(seed + i);
        c->d.notes[i].vel = 100;
        c->d.notes[i].len = 2;
    }
}

int main(void)
{
    song_t song;
    undo_stack_t u;
    song_store_t store;

    printf("T6c undo stack\n");
    printf("  sizeof(undo_entry_t) = %u\n", (unsigned)sizeof(undo_entry_t));
    printf("  sizeof(undo_stack_t) = %u (depth %u)\n", (unsigned)sizeof(undo_stack_t), (unsigned)UNDO_STACK_DEPTH);

    song_store_empty(&song);
    undo_stack_init(&u);
    check("empty stack: nothing to undo", undo_stack_undo(&u, &song, 0) == 0);
    check("empty stack: nothing to redo", undo_stack_redo(&u, &song, 0) == 0);
    check("types are NONE", undo_stack_undo_type(&u) == UNDO_NONE && undo_stack_redo_type(&u) == UNDO_NONE);

    /* clear: the old clip is restored on undo, removed again on redo */
    {
        clip_t oldc, cleared;
        fill_clip(&oldc, 40);
        song.clip[7] = oldc;
        undo_stack_push_clear(&u, 7, &oldc);
        memset(&cleared, 0, sizeof cleared);
        song.clip[7] = cleared;                        /* the edit: clear */
        check("undo type is CLEAR", undo_stack_undo_type(&u) == UNDO_CLEAR_CLIP);
        check("undo clear restores the clip", undo_stack_undo(&u, &song, 0) == 1 &&
              memcmp(&song.clip[7], &oldc, sizeof oldc) == 0);
        check("redo type is CLEAR", undo_stack_redo_type(&u) == UNDO_CLEAR_CLIP);
        check("redo clears again", undo_stack_redo(&u, &song, 0) == 1 &&
              memcmp(&song.clip[7], &cleared, sizeof cleared) == 0);
    }

    /* copy over */
    {
        clip_t dst0, src, dst_after;
        undo_stack_init(&u);
        fill_clip(&dst0, 10);
        fill_clip(&src, 90);
        song.clip[12] = dst0;
        song.clip[13] = src;
        undo_stack_push_copy(&u, 12, 13, &dst0);
        dst_after = src;
        song.clip[12] = dst_after;                     /* the edit: copy 13 -> 12 */
        check("undo copy restores the destination", undo_stack_undo(&u, &song, 0) == 1 &&
              memcmp(&song.clip[12], &dst0, sizeof dst0) == 0);
        check("redo copy re-copies", undo_stack_redo(&u, &song, 0) == 1 &&
              memcmp(&song.clip[12], &dst_after, sizeof dst_after) == 0);
    }

    /* record pass into a clip */
    {
        clip_t oldc, rec;
        undo_stack_init(&u);
        fill_clip(&oldc, 1);
        fill_clip(&rec, 200);
        song.clip[0] = oldc;
        undo_stack_push_record(&u, 0, &oldc);
        song.clip[0] = rec;                            /* the edit: record over */
        check("undo record restores the clip", undo_stack_undo(&u, &song, 0) == 1 &&
              memcmp(&song.clip[0], &oldc, sizeof oldc) == 0);
        check("redo record re-applies", undo_stack_redo(&u, &song, 0) == 1 &&
              memcmp(&song.clip[0], &rec, sizeof rec) == 0);
    }

    /* a new edit clears the redo tail */
    {
        clip_t a, b, c;
        undo_stack_init(&u);
        fill_clip(&a, 3);
        fill_clip(&b, 4);
        fill_clip(&c, 5);
        song.clip[1] = a;
        undo_stack_push_clear(&u, 1, &a);
        song.clip[1] = b;
        undo_stack_undo(&u, &song, 0);                 /* back to a, one redo available */
        check("redo available after undo", undo_stack_redo_type(&u) != UNDO_NONE);
        song.clip[1] = c;
        undo_stack_push_clear(&u, 1, &c);              /* a new edit */
        check("new edit clears redo", undo_stack_redo_type(&u) == UNDO_NONE);
    }

    /* the ring drops the oldest when full */
    {
        clip_t cl;
        int k;
        undo_stack_init(&u);
        for (k = 0; k < (int)UNDO_STACK_DEPTH + 5; k++) {
            fill_clip(&cl, (uint8_t)k);
            undo_stack_push_clear(&u, 2, &cl);
        }
        check("ring capped at the depth", u.n == UNDO_STACK_DEPTH && u.cur == UNDO_STACK_DEPTH);
        for (k = 0; k < (int)UNDO_STACK_DEPTH; k++)
            undo_stack_undo(&u, &song, 0);
        check("all kept entries can be undone", u.cur == 0);
    }

    /* take replace and delete, against the store */
    memset(nor, 0xFF, sizeof nor);
    song_store_init(&store, &FLASH);
    undo_stack_init(&u);
    song_store_save(&store, 0, &song);
    {
        take_hdr_t ha, hb, hg;
        take_event_t ea[2], eb[4], eg[TAKE_MAX_EVENTS];
        int k;
        memset(&ha, 0, sizeof ha);
        memset(&hb, 0, sizeof hb);
        ha.nevents = 2;
        ha.length_samples = 500;
        for (k = 0; k < 2; k++) {
            ea[k].sample = (uint32_t)k * 120u;
            ea[k].cmd = 0xA0u + (uint32_t)k;
        }
        hb.nevents = 4;
        hb.length_samples = 900;
        for (k = 0; k < 4; k++) {
            eb[k].sample = (uint32_t)k * 110u;
            eb[k].cmd = 0xB0u + (uint32_t)k;
        }
        song_store_take_save(&store, 0, &ha, ea);
        undo_stack_push_take(&u, 0);
        song_store_take_save(&store, 0, &hb, eb);      /* the edit: replace the take */
        check("undo type is REPLACE_TAKE", undo_stack_undo_type(&u) == UNDO_REPLACE_TAKE);
        check("undo take brings the first take back", undo_stack_undo(&u, &song, &store) == 1 &&
              song_store_take_load(&store, 0, &hg, eg, TAKE_MAX_EVENTS) == 0 && hg.nevents == 2 && eg[1].cmd == 0xA1u);
        check("redo take brings the second take back", undo_stack_redo(&u, &song, &store) == 1 &&
              song_store_take_load(&store, 0, &hg, eg, TAKE_MAX_EVENTS) == 0 && hg.nevents == 4);
    }
    {
        song_t got;
        undo_stack_init(&u);
        song_store_save(&store, 1, &song);
        undo_stack_push_delete(&u, 1);
        song_store_delete(&store, 1);                  /* the edit: delete */
        check("undo type is DELETE_SONG", undo_stack_undo_type(&u) == UNDO_DELETE_SONG);
        check("deleted is visible", song_store_is_deleted(&store, 1) == 1);
        check("undo delete undeletes", undo_stack_undo(&u, &song, &store) == 1 &&
              song_store_is_deleted(&store, 1) == 0);
        check("data survived the undo", song_store_load(&store, 1, &got) == 0 &&
              memcmp(&got, &song, sizeof song) == 0);
        check("redo delete deletes again", undo_stack_redo(&u, &song, &store) == 1 &&
              song_store_is_deleted(&store, 1) == 1);
    }

    printf("%s\n", bad ? "UNDO STACK TEST FAILED" : "undo stack test passed");
    return bad ? 1 : 0;
}
