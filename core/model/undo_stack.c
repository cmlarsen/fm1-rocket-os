/* SPDX-License-Identifier: GPL-3.0-only */
/* Undo/redo core (T6c). See core/model/undo_stack.h. */
#include "undo_stack.h"

_Static_assert(sizeof(undo_stack_t) <= 12288u, "undo ring fits in about 12 KiB");

void undo_stack_init(undo_stack_t *u)
{
    u->n = 0u;
    u->cur = 0u;
}

/* store one entry and clear the redo tail; drop the oldest when full */
static void undo_push(undo_stack_t *u, uint8_t type, uint8_t index, uint8_t aux, const clip_t *clip, uint32_t seq)
{
    undo_entry_t *e;
    uint32_t i;
    if (u->n >= UNDO_STACK_DEPTH) {
        for (i = 1u; i < UNDO_STACK_DEPTH; i++)
            u->e[i - 1u] = u->e[i];
        u->n = UNDO_STACK_DEPTH - 1u;
        if (u->cur)
            u->cur--;
    }
    e = &u->e[u->n];
    e->type = type;
    e->index = index;
    e->aux = aux;
    e->pad = 0u;
    if (clip)
        e->u.clip = *clip;
    else
        e->u.seq = seq;
    u->n++;
    u->cur = u->n;
}

void undo_stack_push_clear(undo_stack_t *u, uint8_t clip, const clip_t *old)
{
    undo_push(u, UNDO_CLEAR_CLIP, clip, 0u, old, 0u);
}

void undo_stack_push_record(undo_stack_t *u, uint8_t clip, const clip_t *old)
{
    undo_push(u, UNDO_RECORD_CLIP, clip, 0u, old, 0u);
}

void undo_stack_push_copy(undo_stack_t *u, uint8_t dst, uint8_t src, const clip_t *old_dst)
{
    undo_push(u, UNDO_COPY_CLIP, dst, src, old_dst, 0u);
}

void undo_stack_push_take(undo_stack_t *u, const song_store_t *store, uint8_t slot)
{
    uint32_t seq = 0u;
    uint32_t i;
    /* only the newest replace-take per slot is undoable: the previous take it
     * names is the one an extent reuse would destroy first */
    for (i = 0u; i < u->n; i++)
        if (u->e[i].type == UNDO_REPLACE_TAKE && u->e[i].index == slot)
            u->e[i].u.seq = 0u;
    if (store && slot < (int)SONG_STORE_SLOTS && store->slot[slot].prev_take_ext >= 0)
        seq = store->slot[slot].prev_take_seq;
    undo_push(u, UNDO_REPLACE_TAKE, slot, 0u, 0, seq);
}

void undo_stack_push_delete(undo_stack_t *u, const song_store_t *store, uint8_t slot)
{
    uint32_t seq = 0u;
    if (store && slot < (int)SONG_STORE_SLOTS)
        seq = store->slot[slot].song_seq;
    undo_push(u, UNDO_DELETE_SONG, slot, 0u, 0, seq);
}

static int undo_swap_clip(song_t *song, undo_entry_t *e)
{
    clip_t t;
    if (!song || e->index >= NCLIPS)
        return 0;
    t = song->clip[e->index];
    song->clip[e->index] = e->u.clip;
    e->u.clip = t;
    return 1;
}

/* 1 = the take entry still points at a restorable record; 0 = it is stale */
static int undo_take_live(const undo_entry_t *e, const song_store_t *store, const undo_take_scratch_t *ts)
{
    if (e->u.seq == 0u || e->index >= (int)SONG_STORE_SLOTS || !store || !ts || !ts->hdr)
        return 0;
    return store->slot[e->index].prev_take_ext >= 0 && store->slot[e->index].prev_take_seq == e->u.seq;
}

int undo_stack_undo(undo_stack_t *u, song_t *song, song_store_t *store, const undo_take_scratch_t *ts)
{
    undo_entry_t *e;
    if (!u || u->cur == 0u)
        return UNDO_EMPTY;
    e = &u->e[u->cur - 1u];
    switch (e->type) {
    case UNDO_CLEAR_CLIP:
    case UNDO_COPY_CLIP:
    case UNDO_RECORD_CLIP:
        if (!undo_swap_clip(song, e))
            return UNDO_EMPTY;
        break;
    case UNDO_REPLACE_TAKE:
        if (!undo_take_live(e, store, ts)) {
            u->cur--;
            return UNDO_CANNOT;
        }
        if (song_store_take_restore(store, e->index, ts->hdr, ts->ev, ts->max_events)) {
            u->cur--;
            return UNDO_CANNOT;
        }
        e->u.seq = store->slot[e->index].prev_take_seq;   /* the replaced take, for redo */
        break;
    case UNDO_DELETE_SONG:
        if (!store || e->index >= (int)SONG_STORE_SLOTS || store->slot[e->index].song_seq != e->u.seq ||
            !store->slot[e->index].deleted) {
            u->cur--;
            return UNDO_CANNOT;                            /* the slot was reused */
        }
        if (song_store_undelete(store, e->index)) {
            u->cur--;
            return UNDO_CANNOT;
        }
        e->u.seq = store->slot[e->index].song_seq;         /* the restored record, for redo */
        break;
    default:
        return UNDO_EMPTY;
    }
    u->cur--;
    return UNDO_DONE;
}

int undo_stack_redo(undo_stack_t *u, song_t *song, song_store_t *store, const undo_take_scratch_t *ts)
{
    undo_entry_t *e;
    if (!u || u->cur >= u->n)
        return UNDO_EMPTY;
    e = &u->e[u->cur];
    switch (e->type) {
    case UNDO_CLEAR_CLIP:
    case UNDO_COPY_CLIP:
    case UNDO_RECORD_CLIP:
        if (!undo_swap_clip(song, e))
            return UNDO_EMPTY;
        break;
    case UNDO_REPLACE_TAKE:
        if (!undo_take_live(e, store, ts)) {
            u->cur++;
            return UNDO_CANNOT;
        }
        if (song_store_take_restore(store, e->index, ts->hdr, ts->ev, ts->max_events)) {
            u->cur++;
            return UNDO_CANNOT;
        }
        e->u.seq = store->slot[e->index].prev_take_seq;
        break;
    case UNDO_DELETE_SONG:
        if (!store || e->index >= (int)SONG_STORE_SLOTS || store->slot[e->index].song_seq != e->u.seq ||
            store->slot[e->index].deleted) {
            u->cur++;
            return UNDO_CANNOT;
        }
        if (song_store_delete(store, e->index)) {
            u->cur++;
            return UNDO_CANNOT;
        }
        e->u.seq = store->slot[e->index].song_seq;
        break;
    default:
        return UNDO_EMPTY;
    }
    u->cur++;
    return UNDO_DONE;
}

uint8_t undo_stack_undo_type(const undo_stack_t *u)
{
    return (u && u->cur) ? u->e[u->cur - 1u].type : (uint8_t)UNDO_NONE;
}

uint8_t undo_stack_redo_type(const undo_stack_t *u)
{
    return (u && u->cur < u->n) ? u->e[u->cur].type : (uint8_t)UNDO_NONE;
}
