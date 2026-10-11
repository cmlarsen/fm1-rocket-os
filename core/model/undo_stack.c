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
static void undo_push(undo_stack_t *u, uint8_t type, uint8_t index, uint8_t aux, const clip_t *clip)
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
        e->clip = *clip;
    else {
        uint8_t *z = (uint8_t *)&e->clip;
        uint32_t k;
        for (k = 0; k < sizeof e->clip; k++)
            z[k] = 0;
    }
    u->n++;
    u->cur = u->n;
}

void undo_stack_push_clear(undo_stack_t *u, uint8_t clip, const clip_t *old)
{
    undo_push(u, UNDO_CLEAR_CLIP, clip, 0u, old);
}

void undo_stack_push_record(undo_stack_t *u, uint8_t clip, const clip_t *old)
{
    undo_push(u, UNDO_RECORD_CLIP, clip, 0u, old);
}

void undo_stack_push_copy(undo_stack_t *u, uint8_t dst, uint8_t src, const clip_t *old_dst)
{
    undo_push(u, UNDO_COPY_CLIP, dst, src, old_dst);
}

void undo_stack_push_take(undo_stack_t *u, uint8_t slot)
{
    undo_push(u, UNDO_REPLACE_TAKE, slot, 0u, 0);
}

void undo_stack_push_delete(undo_stack_t *u, uint8_t slot)
{
    undo_push(u, UNDO_DELETE_SONG, slot, 0u, 0);
}

static int undo_swap_clip(song_t *song, undo_entry_t *e)
{
    clip_t t;
    if (!song || e->index >= NCLIPS)
        return 0;
    t = song->clip[e->index];
    song->clip[e->index] = e->clip;
    e->clip = t;
    return 1;
}

int undo_stack_undo(undo_stack_t *u, song_t *song, song_store_t *store)
{
    undo_entry_t *e;
    if (!u || u->cur == 0u)
        return 0;
    e = &u->e[u->cur - 1u];
    switch (e->type) {
    case UNDO_CLEAR_CLIP:
    case UNDO_COPY_CLIP:
    case UNDO_RECORD_CLIP:
        if (!undo_swap_clip(song, e))
            return 0;
        break;
    case UNDO_REPLACE_TAKE:
        if (!song_store_take_swap(store, e->index))
            return 0;
        break;
    case UNDO_DELETE_SONG:
        if (song_store_undelete(store, e->index))
            return 0;
        break;
    default:
        return 0;
    }
    u->cur--;
    return 1;
}

int undo_stack_redo(undo_stack_t *u, song_t *song, song_store_t *store)
{
    undo_entry_t *e;
    if (!u || u->cur >= u->n)
        return 0;
    e = &u->e[u->cur];
    switch (e->type) {
    case UNDO_CLEAR_CLIP:
    case UNDO_COPY_CLIP:
    case UNDO_RECORD_CLIP:
        if (!undo_swap_clip(song, e))
            return 0;
        break;
    case UNDO_REPLACE_TAKE:
        if (!song_store_take_swap(store, e->index))
            return 0;
        break;
    case UNDO_DELETE_SONG:
        if (song_store_delete(store, e->index))
            return 0;
        break;
    default:
        return 0;
    }
    u->cur++;
    return 1;
}

uint8_t undo_stack_undo_type(const undo_stack_t *u)
{
    return (u && u->cur) ? u->e[u->cur - 1u].type : (uint8_t)UNDO_NONE;
}

uint8_t undo_stack_redo_type(const undo_stack_t *u)
{
    return (u && u->cur < u->n) ? u->e[u->cur].type : (uint8_t)UNDO_NONE;
}
