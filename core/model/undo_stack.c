/* SPDX-License-Identifier: GPL-3.0-only */
/* Undo/redo core (T6c). See core/model/undo_stack.h. */
#include "undo_stack.h"

_Static_assert(sizeof(undo_stack_t) <= 6144u, "undo ring fits in about 6 KiB (T9i)");

void undo_stack_init(undo_stack_t *u)
{
    u->n = 0u;
    u->cur = 0u;
    u->base_ser = 0u;
}

/* Store one entry; only a deferred take insertion keeps the redo tail. Drop the oldest when full. */
static void undo_push_at(undo_stack_t *u, uint32_t pos, uint8_t type, uint8_t index, uint8_t aux,
                         const clip_t *clip, uint32_t seq, int keep_tail)
{
    undo_entry_t *e;
    undo_entry_t tmp;
    uint32_t i;
    int behind = keep_tail && pos <= u->cur;
    /* T6c fix: a new edit at the top discards the redo tail. Without it, an entry above `cur` (a
     * state undone before this edit) would be restored by a later undo, replaying a branch that no
     * longer exists (edit v0->v1, undo, edit v0->v2, undo, undo restored v1).
     * T9i r5: the deferred take flush keeps the tail even at pos == cur, so an edit made after the
     * take and undone stays redoable. Ordinary new edits still discard their redo branch. */
    if (!keep_tail)
        u->n = u->cur;
    if (pos > u->n)
        pos = u->n;
    if (u->n >= UNDO_STACK_DEPTH) {
        for (i = 1u; i < UNDO_STACK_DEPTH; i++)
            u->e[i - 1u] = u->e[i];
        u->n = UNDO_STACK_DEPTH - 1u;
        u->base_ser++;                    /* S5: a push still advances the position stamp when the
                                           * full ring drops the oldest entry (cur alone saturates) */
        if (u->cur)
            u->cur--;
        if (pos)
            pos--;
    }
    tmp.type = type;
    tmp.index = index;
    tmp.aux = aux;
    tmp.pad = 0u;
    if (clip)
        tmp.u.clip = *clip;
    else
        tmp.u.seq = seq;
    for (i = u->n; i > pos; i--)          /* make room: entries above `pos` keep their order */
        u->e[i] = u->e[i - 1u];
    e = &u->e[pos];
    *e = tmp;
    u->n++;
    /* T9i r4: the entries above `pos` shifted up by one, so the current position follows them; a
     * top push lands at the new top instead. */
    u->cur = behind ? u->cur + 1u : u->n;
}

static void undo_push(undo_stack_t *u, uint8_t type, uint8_t index, uint8_t aux, const clip_t *clip, uint32_t seq)
{
    undo_push_at(u, u->cur, type, index, aux, clip, seq, 0);
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

/* T9i r3/NIT: push the take entry at the position `ser` implies, so a clip edit made after the take
 * ended sits above it and is undone first. `ser` is undo_stack_serial() from when the take ended, not
 * from this deferred flush. */
void undo_stack_push_take_at(undo_stack_t *u, const song_store_t *store, uint8_t slot, uint32_t ser)
{
    uint32_t seq = 0u;
    uint32_t i, pos;
    if (!u)
        return;
    /* only the newest replace-take per slot is undoable: the previous take it
     * names is the one an extent reuse would destroy first */
    for (i = 0u; i < u->n; i++)
        if (u->e[i].type == UNDO_REPLACE_TAKE && u->e[i].index == slot)
            u->e[i].u.seq = 0u;
    if (store && slot < (int)SONG_STORE_SLOTS && store->slot[slot].prev_take_ext >= 0)
        seq = store->slot[slot].prev_take_seq;
    pos = ser > u->base_ser ? ser - u->base_ser : 0u;
    undo_push_at(u, pos, UNDO_REPLACE_TAKE, slot, 0u, 0, seq, 1);
}

void undo_stack_push_take(undo_stack_t *u, const song_store_t *store, uint8_t slot)
{
    undo_stack_push_take_at(u, store, slot, undo_stack_serial(u));
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

uint32_t undo_stack_serial(const undo_stack_t *u) { return u ? u->base_ser + u->cur : 0u; }
