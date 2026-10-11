/* SPDX-License-Identifier: GPL-3.0-only */
/* T6c host test of core/model/undo_stack.c.
 *
 * Clip entries carry the old clip_t and swap with the song; take and delete
 * entries name a slot plus the sequence number of the record they restore, and
 * the store re-commits it so the undo survives a reboot. A new edit clears the
 * redo tail; a new replace-take for a slot invalidates older take entries for
 * that slot; a stale entry returns UNDO_CANNOT.
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
    if ((off & 0xFFu) + n > 256u || off > NOR_SIZE || n > NOR_SIZE - off)
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

static song_store_t store;
static take_hdr_t th;
static take_event_t tev[TAKE_MAX_EVENTS];
static undo_take_scratch_t TS;

static void take(int slot, uint32_t tag)
{
    take_hdr_t h;
    take_event_t e[1];
    memset(&h, 0, sizeof h);
    h.nevents = 1;
    h.length_samples = tag;
    e[0].sample = tag;
    e[0].cmd = tag;
    song_store_take_save(&store, slot, &h, e);
}

static uint32_t cur(int slot)
{
    take_hdr_t h;
    take_event_t e[4];
    if (song_store_take_load(&store, slot, &h, e, 4))
        return 0;
    return h.length_samples;
}

int main(void)
{
    song_t song;
    undo_stack_t u;

    printf("T6c undo stack\n");
    printf("  sizeof(undo_entry_t) = %u\n", (unsigned)sizeof(undo_entry_t));
    printf("  sizeof(undo_stack_t) = %u (depth %u)\n", (unsigned)sizeof(undo_stack_t), (unsigned)UNDO_STACK_DEPTH);

    song_store_empty(&song);
    TS.hdr = &th;
    TS.ev = tev;
    TS.max_events = TAKE_MAX_EVENTS;

    undo_stack_init(&u);
    check("empty stack: nothing to undo", undo_stack_undo(&u, &song, 0, 0) == UNDO_EMPTY);
    check("empty stack: nothing to redo", undo_stack_redo(&u, &song, 0, 0) == UNDO_EMPTY);
    check("types are NONE", undo_stack_undo_type(&u) == UNDO_NONE && undo_stack_redo_type(&u) == UNDO_NONE);

    /* clear: the old clip is restored on undo, removed again on redo */
    {
        clip_t oldc, cleared;
        fill_clip(&oldc, 40);
        song.clip[7] = oldc;
        undo_stack_push_clear(&u, 7, &oldc);
        memset(&cleared, 0, sizeof cleared);
        song.clip[7] = cleared;
        check("undo type is CLEAR", undo_stack_undo_type(&u) == UNDO_CLEAR_CLIP);
        check("undo clear restores the clip", undo_stack_undo(&u, &song, 0, 0) == UNDO_DONE &&
              memcmp(&song.clip[7], &oldc, sizeof oldc) == 0);
        check("redo type is CLEAR", undo_stack_redo_type(&u) == UNDO_CLEAR_CLIP);
        check("redo clears again", undo_stack_redo(&u, &song, 0, 0) == UNDO_DONE &&
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
        song.clip[12] = dst_after;
        check("undo copy restores the destination", undo_stack_undo(&u, &song, 0, 0) == UNDO_DONE &&
              memcmp(&song.clip[12], &dst0, sizeof dst0) == 0);
        check("redo copy re-copies", undo_stack_redo(&u, &song, 0, 0) == UNDO_DONE &&
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
        song.clip[0] = rec;
        check("undo record restores the clip", undo_stack_undo(&u, &song, 0, 0) == UNDO_DONE &&
              memcmp(&song.clip[0], &oldc, sizeof oldc) == 0);
        check("redo record re-applies", undo_stack_redo(&u, &song, 0, 0) == UNDO_DONE &&
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
        undo_stack_undo(&u, &song, 0, 0);
        check("redo available after undo", undo_stack_redo_type(&u) != UNDO_NONE);
        song.clip[1] = c;
        undo_stack_push_clear(&u, 1, &c);
        check("new edit clears redo", undo_stack_redo_type(&u) == UNDO_NONE);
    }

    /* T6c fix (r3): a new edit after an undo discards the redo tail. The old code kept the entry
     * above `cur`, so edit v0->v1, undo, edit v0->v2, undo, undo wrongly restored v1. */
    {
        clip_t v0, v1, v2, v1b;
        undo_stack_init(&u);
        fill_clip(&v0, 10);
        fill_clip(&v1, 20);
        fill_clip(&v2, 30);
        fill_clip(&v1b, 21);                            /* a detectable "branch" clip */
        song.clip[0] = v0;
        undo_stack_push_record(&u, 0, &v0);             /* the clip is about to become v1 */
        song.clip[0] = v1;
        check("branch: the first edit undoes", undo_stack_undo(&u, &song, 0, 0) == UNDO_DONE &&
              memcmp(&song.clip[0], &v0, sizeof v0) == 0);
        song.clip[0] = v1b;                             /* (the branch value lives only in the song) */
        undo_stack_push_record(&u, 0, &v0);             /* a new edit from v0, so the clip becomes v2 */
        song.clip[0] = v2;
        check("branch: the new edit undoes", undo_stack_undo(&u, &song, 0, 0) == UNDO_DONE &&
              memcmp(&song.clip[0], &v0, sizeof v0) == 0);
        check("branch: the discarded redo is not undone again",
              undo_stack_undo(&u, &song, 0, 0) == UNDO_EMPTY &&
              memcmp(&song.clip[0], &v0, sizeof v0) == 0);
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
            undo_stack_undo(&u, &song, 0, 0);
        check("all kept entries can be undone", u.cur == 0);
    }

    /* takes: a replace survives a reboot, and only the newest is undoable */
    memset(nor, 0xFF, sizeof nor);
    song_store_init(&store, &FLASH);
    undo_stack_init(&u);
    take(1, 111);
    take(1, 222);
    undo_stack_push_take(&u, &store, 1);
    check("undo type is REPLACE_TAKE", undo_stack_undo_type(&u) == UNDO_REPLACE_TAKE);
    check("undo take restores the first take", undo_stack_undo(&u, &song, &store, &TS) == UNDO_DONE &&
          cur(1) == 111u);
    check("redo take restores the second take", undo_stack_redo(&u, &song, &store, &TS) == UNDO_DONE &&
          cur(1) == 222u);
    check("undo again gives the first", undo_stack_undo(&u, &song, &store, &TS) == UNDO_DONE && cur(1) == 111u);
    song_store_init(&store, &FLASH);                   /* reboot: the commit is on flash */
    check("take undo survives a reboot", cur(1) == 111u);

    undo_stack_init(&u);
    take(2, 1);
    take(2, 2);
    undo_stack_push_take(&u, &store, 2);
    take(2, 3);
    undo_stack_push_take(&u, &store, 2);
    check("first undo gives the second take", undo_stack_undo(&u, &song, &store, &TS) == UNDO_DONE && cur(2) == 2u);
    check("second undo cannot (only the newest take entry is live)",
          undo_stack_undo(&u, &song, &store, &TS) == UNDO_CANNOT && cur(2) == 2u);

    /* delete: persists, undoes and redoes with the sequence-number check */
    {
        song_t other;
        undo_stack_init(&u);
        song_store_save(&store, 0, &song);
        song_store_delete(&store, 0);                  /* the edit first */
        undo_stack_push_delete(&u, &store, 0);         /* then the entry that names it */
        check("undo type is DELETE_SONG", undo_stack_undo_type(&u) == UNDO_DELETE_SONG);
        check("deleted is visible", song_store_is_deleted(&store, 0) == 1);
        song_store_init(&store, &FLASH);
        check("delete survives a reboot", song_store_is_deleted(&store, 0) == 1);
        check("undo delete undeletes", undo_stack_undo(&u, &song, &store, &TS) == UNDO_DONE &&
              song_store_is_deleted(&store, 0) == 0);
        song_store_init(&store, &FLASH);
        check("undelete survives a reboot", song_store_is_deleted(&store, 0) == 0);
        check("data survived the undo", song_store_load(&store, 0, &other) == 0 &&
              memcmp(&other, &song, sizeof song) == 0);
        check("redo delete deletes again", undo_stack_redo(&u, &song, &store, &TS) == UNDO_DONE &&
              song_store_is_deleted(&store, 0) == 1);
    }
    {
        song_t other;
        undo_stack_init(&u);
        song_store_save(&store, 1, &song);             /* a live song to delete */
        song_store_delete(&store, 1);
        undo_stack_push_delete(&u, &store, 1);         /* records the deleted record's seq */
        song_store_empty(&other);
        memcpy(other.name, "NEW ", 4);
        song_store_save(&store, 1, &other);            /* the slot was reused */
        check("undo delete after reuse cannot", undo_stack_undo(&u, &song, &store, &TS) == UNDO_CANNOT);
    }

    printf("%s\n", bad ? "UNDO STACK TEST FAILED" : "undo stack test passed");
    return bad ? 1 : 0;
}
