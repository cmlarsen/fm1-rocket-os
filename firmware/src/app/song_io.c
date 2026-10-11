/* SPDX-License-Identifier: GPL-3.0-only */
/* T6i: songs on real flash — see song_io.h.
 *
 * This is a separate translation unit (song_store.c includes X0X's lz.c, and the device build's
 * x0x.c already includes it through project.c, so the two must not share one). It talks to the
 * model (song_store.h / undo_stack.h) and to the LIVE song, which lives in live_bridge.c, and
 * nothing else. The flash is the platform's plat_song_* backend (plat_fm1.c on the device,
 * host/x0x_host.c in the simulator), pinned to 0xA0000..0xDBFFF.
 *
 * The store and the undo ring are static here: song_store_t (144 B) and undo_stack_t (~12 KiB,
 * UNDO_STACK_DEPTH entries). On the device both go in .pool (X0X_POOL), like the clip engine's. */
#include <stdint.h>
#include "song_flash.h"                 /* the plat_song_* backend (no plat.h: its NKEYS clashes) */
#include "live_bridge.h"
#include "core/model/song_store.h"
#include "core/model/undo_stack.h"

/* the device's .pool section, as live_bridge.c/engine.c spell it (plat.h's X0X_POOL without the
 * NKEYS collision) */
#if defined(X0X_HOST)
#define SONG_POOL
#else
#define SONG_POOL __attribute__((section(".pool")))
#endif

/* the platform's three byte-level functions as the store's interface */
static int fb_read(void *ctx, uint32_t off, void *dst, uint32_t n) { (void)ctx; return plat_song_read(off, dst, n); }
static int fb_erase(void *ctx, uint32_t off) { (void)ctx; return plat_song_erase(off); }
static int fb_program(void *ctx, uint32_t off, const void *src, uint32_t n) { (void)ctx; return plat_song_program(off, src, n); }
static const song_flash_t FB = { fb_read, fb_erase, fb_program, 0 };

static song_store_t store SONG_POOL;
static undo_stack_t undo SONG_POOL;
static uint8_t ready, dirty, loaded, failed;
static uint32_t saves;

static song_t *song(void) { return (song_t *)live_bridge_song(); }

void song_io_boot(const pattern_t *pats, uint16_t bpm_x10)
{
    song_store_init(&store, &FB);
    undo_stack_init(&undo);
    ready = 0;
    loaded = 0;
    dirty = 0;
    failed = 0;
    if (song_store_load(&store, 0, song()) == 0) {      /* slot 0 holds a valid, undeleted song */
        loaded = 1;                                     /* T7a: the song is the edited object; use it
                                                         * as it is. The T6i pattern-hash remap is gone. */
        live_bridge_song_baseline(0);                   /* nothing to re-derive from the project */
    } else {
        live_bridge_rebuild(pats);                      /* one-time seed: the project, when no song */
        if (bpm_x10 < 200u)
            bpm_x10 = 200u;
        else if (bpm_x10 > 3000u)
            bpm_x10 = 3000u;
        song()->bpm_x10 = bpm_x10;
        dirty = 1;                                      /* it must be written back */
    }
    ready = 1;
    live_bridge_song_reload();                          /* the clip engine follows the new song */
}

int song_io_loaded(void) { return loaded != 0; }
int song_io_dirty(void) { return dirty != 0; }
int song_io_failed(void) { return failed != 0; }
int song_io_wants_save(void) { return dirty && !failed; }
int song_io_saves(void) { return (int)saves; }
void song_io_mark_dirty(void) { dirty = 1; failed = 0; }   /* a new edit re-enables saving */

int song_io_save(void)
{
    int rc;
    if (!ready)
        return -1;
    rc = song_store_save_tag(&store, 0, song(), live_bridge_song_hash());
    if (rc == 0) {
        dirty = 0;
        failed = 0;
        saves++;
    } else {
        failed = 1;                                     /* no retry until the next edit (item 3) */
    }
    return rc;
}

int song_io_flush(void)
{
    if (!dirty || failed)
        return 0;
    return song_io_save();
}

void song_io_set_key_clip(int key, int clip)
{
    if (key < 0 || key >= (int)NKEYS)
        return;
    if (clip != (int)CLIP_NONE && (clip < 0 || clip > (int)NCLIPS))
        return;
    song()->key_clip[key] = (uint8_t)clip;
    dirty = 1;
    failed = 0;
}

/* ---- T7a: the clip under the cursor -------------------------------------- */
/* Every edit marks the song dirty and re-enables saving (a new edit is what lifts the failed state). */
static clip_t *clip_at(int c) { return (c >= 0 && c < (int)NCLIPS) ? &song()->clip[c] : 0; }
static void edited(void) { dirty = 1; failed = 0; }
/* T7b: after an edit, hand the clip to the clip engine so it writes the playing copy (and a staged
 * launch copy) with the audio IRQs masked; the ISR never reads song->clip[]. */
static void synced(int c) { live_bridge_clip_changed((uint8_t)c); }
/* the undo push helpers are defined below */
void song_io_push_clear(int clip, const void *old_clip);
void song_io_push_record(int clip, const void *old_clip);
void song_io_push_copy(int dst, int src, const void *old_dst);
void song_io_edit_push(void);

int song_io_clip_used(int c) { clip_t *k = clip_at(c); return k ? k->used != 0 : 0; }
int song_io_clip_len(int c)
{
    clip_t *k = clip_at(c);
    return (k && k->used) ? (int)k->len : 0;
}
int song_io_clip_mode(int c) { clip_t *k = clip_at(c); return k ? (int)k->mode : 0; }
int song_io_clip_quant(int c) { clip_t *k = clip_at(c); return k ? (int)k->quant : 0; }
int song_io_clip_nnotes(int c)
{
    clip_t *k = clip_at(c);
    return (k && k->used && clip_role((uint8_t)c) != ROLE_DRUMS) ? (int)k->nnotes : 0;
}
int song_io_clip_motion(int c)
{
    clip_t *k = clip_at(c);
    int i, n = 0;
    if (!k)
        return 0;
    for (i = 0; i < (int)CLIP_MOTION_LANES; i++)
        if (k->motion[i].param != PARAM_NONE)
            n++;
    return n;
}
int song_io_clip_role(int c) { return (c >= 0 && c < (int)NCLIPS) ? (int)clip_role((uint8_t)c) : -1; }

void song_io_clip_create(int c, int len, int mode, int quant)
{
    clip_t *k = clip_at(c);
    int i, m, s;
    if (!k || k->used)
        return;
    for (i = 0; i < (int)sizeof *k; i++)
        ((uint8_t *)k)[i] = 0;
    k->used = 1;
    k->len = (uint8_t)(len < 1 ? 1 : len > (int)CLIP_MAX_STEPS ? (int)CLIP_MAX_STEPS : len);
    k->mode = (uint8_t)mode;
    k->quant = (uint8_t)quant;
    for (m = 0; m < (int)CLIP_MOTION_LANES; m++) {      /* an empty lane would look like parameter 0 */
        k->motion[m].param = PARAM_NONE;
        for (s = 0; s < (int)CLIP_MAX_STEPS; s++)
            k->motion[m].v[s] = MOTION_NONE;
    }
    edited();
    synced(c);
}

/* T7b: create the clip as part of the open editor session, so undo restores the empty slot (the
 * slot stops playing too). The caller has called song_io_edit_begin(clip). */
void song_io_clip_create_undoable(int c, int len, int mode, int quant)
{
    if (!clip_at(c) || song_io_clip_used(c))
        return;
    song_io_edit_push();                                /* the empty slot is the session's pre-state */
    song_io_clip_create(c, len, mode, quant);
}

void song_io_clip_clear(int c)
{
    clip_t *k = clip_at(c);
    int i, role = song_io_clip_role(c);
    if (!k || !k->used)
        return;
    song_io_push_clear(c, k);
    for (i = 0; i < (int)sizeof *k; i++)
        ((uint8_t *)k)[i] = 0;
    edited();
    synced(c);
    if (role >= 0 && live_bridge_playing_clip((uint8_t)role) == (uint8_t)c &&
        !live_bridge_stop_queued(role))
        live_bridge_stop((uint8_t)role);                /* the clip is gone: stop the role (once) */
}

void song_io_clip_copy(int dst, int src)
{
    clip_t *d = clip_at(dst), *s = clip_at(src);
    if (!d || !s || !s->used || dst == src)
        return;
    song_io_push_copy(dst, src, d);                     /* the old destination, for undo */
    *d = *s;
    edited();
    synced(dst);
}

void song_io_clip_set_len(int c, int v)
{
    clip_t *k = clip_at(c);
    if (!k || !k->used)
        return;
    k->len = (uint8_t)(v < 1 ? 1 : v > (int)CLIP_MAX_STEPS ? (int)CLIP_MAX_STEPS : v);
    edited();
    synced(c);
}
void song_io_clip_set_mode(int c, int v)
{
    clip_t *k = clip_at(c);
    if (!k || !k->used)
        return;
    k->mode = (uint8_t)(v ? 1 : 0);
    edited();
    synced(c);
}
void song_io_clip_set_quant(int c, int v)
{
    clip_t *k = clip_at(c);
    if (!k || !k->used)
        return;
    if (v < 0) v = 0;
    if (v > (int)NQUANT - 1) v = (int)NQUANT - 1;
    k->quant = (uint8_t)v;
    edited();
    synced(c);
}
int song_io_swing(void) { return (int)song()->swing; }
void song_io_set_swing(int v)
{
    song()->swing = (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v);
    edited();
}
void song_io_set_bpm(int bpm_x10)
{
    if (bpm_x10 < 200)
        bpm_x10 = 200;
    else if (bpm_x10 > 3000)
        bpm_x10 = 3000;
    song()->bpm_x10 = (uint16_t)bpm_x10;
    edited();
}
void song_io_name(char *out, int n)
{
    int i;
    if (n < 1)
        return;
    for (i = 0; i < n - 1 && i < (int)SONG_NAME_LEN && song()->name[i]; i++)
        out[i] = song()->name[i];
    out[i] = 0;
}
int song_io_key(void) { return (int)song()->key; }
int song_io_scale(void) { return (int)song()->scale; }

/* ---- T7b: the clip editor's edits ---------------------------------------- */

static int edit_open = -1, edit_pushed;
void song_io_edit_begin(int clip) { edit_open = clip; edit_pushed = 0; }
void song_io_edit_end(void) { edit_open = -1; edit_pushed = 0; }
void song_io_edit_push(void)
{
    if (edit_open < 0 || edit_pushed)
        return;
    song_io_push_record(edit_open, &song()->clip[edit_open]);   /* the pre-session clip */
    edit_pushed = 1;
}

int song_io_clip_is_drum(int c) { return clip_role((uint8_t)c) == ROLE_DRUMS; }

int song_io_drum_get(int c, int lane, int step)
{
    clip_t *k = clip_at(c);
    if (!k || !k->used || lane < 0 || lane >= (int)DRUM_LANES || step < 0 || step >= (int)CLIP_MAX_STEPS)
        return 0;
    return drum_get(&k->d.drum, (unsigned)lane, (unsigned)step);
}
int song_io_drum_accent(int c, int step)
{
    clip_t *k = clip_at(c);
    if (!k || !k->used || step < 0 || step >= (int)CLIP_MAX_STEPS)
        return 0;
    return (int)((k->d.drum.accent[step >> 5] >> (step & 31u)) & 1u);
}
void song_io_drum_set(int c, int lane, int step, int on)
{
    clip_t *k = clip_at(c);
    if (!k || !k->used || clip_role((uint8_t)c) != ROLE_DRUMS)
        return;
    if (lane < 0 || lane >= (int)DRUM_LANES || step < 0 || step >= (int)CLIP_MAX_STEPS)
        return;
    song_io_edit_push();
    drum_set(&k->d.drum, (unsigned)lane, (unsigned)step, on ? 1 : 0);
    edited();
    synced(c);
}
void song_io_drum_set_accent(int c, int step, int on)
{
    clip_t *k = clip_at(c);
    uint32_t m;
    if (!k || !k->used || clip_role((uint8_t)c) != ROLE_DRUMS)
        return;
    if (step < 0 || step >= (int)CLIP_MAX_STEPS)
        return;
    song_io_edit_push();
    m = 1u << ((unsigned)step & 31u);
    if (on)
        k->d.drum.accent[step >> 5] |= m;
    else
        k->d.drum.accent[step >> 5] &= ~m;
    edited();
    synced(c);
}

int song_io_note_count(int c)
{
    clip_t *k = clip_at(c);
    return (k && k->used && clip_role((uint8_t)c) != ROLE_DRUMS) ? (int)k->nnotes : 0;
}
int song_io_note_index(int c, int step, int note)
{
    clip_t *k = clip_at(c);
    int i, n;
    if (!k || !k->used || clip_role((uint8_t)c) == ROLE_DRUMS)
        return -1;
    n = (int)k->nnotes;
    for (i = 0; i < n; i++)
        if (k->d.notes[i].step == (uint8_t)step && k->d.notes[i].note == (uint8_t)note)
            return i;
    return -1;
}
int song_io_note_get(int c, int i, int *step, int *note, int *vel, int *len)
{
    clip_t *k = clip_at(c);
    if (!k || !k->used || i < 0 || i >= (int)k->nnotes)
        return 0;
    if (step) *step = (int)k->d.notes[i].step;
    if (note) *note = (int)k->d.notes[i].note;
    if (vel)  *vel  = (int)k->d.notes[i].vel;
    if (len)  *len  = (int)k->d.notes[i].len;
    return 1;
}
int song_io_note_add(int c, int step, int note, int vel, int len)
{
    clip_t *k = clip_at(c);
    note_t *nt;
    int i = song_io_note_index(c, step, note);
    if (i >= 0)
        return i;                                  /* the same note is already there */
    if (!k || !k->used || clip_role((uint8_t)c) == ROLE_DRUMS)
        return -1;
    if (k->nnotes >= (uint8_t)CLIP_MAX_NOTES || step < 0 || step >= (int)CLIP_MAX_STEPS)
        return -1;
    song_io_edit_push();
    nt = &k->d.notes[k->nnotes++];
    nt->step = (uint8_t)step;
    nt->note = (uint8_t)(note < 0 ? 0 : note > 127 ? 127 : note);
    nt->vel = (uint8_t)(vel < 1 ? 100 : vel > 127 ? 127 : vel);
    nt->len = (uint8_t)(len < 1 ? 1 : len > (int)CLIP_MAX_STEPS ? (int)CLIP_MAX_STEPS : len);
    edited();
    synced(c);
    return (int)k->nnotes - 1;
}
int song_io_note_remove(int c, int step, int note)
{
    clip_t *k = clip_at(c);
    int i = song_io_note_index(c, step, note), j;
    if (i < 0)
        return 0;
    song_io_edit_push();
    for (j = i + 1; j < (int)k->nnotes; j++)
        k->d.notes[j - 1] = k->d.notes[j];
    k->nnotes--;
    edited();
    synced(c);
    return 1;
}
void song_io_note_set(int c, int i, int step, int note, int vel, int len)
{
    clip_t *k = clip_at(c);
    note_t *nt;
    if (!k || !k->used || i < 0 || i >= (int)k->nnotes)
        return;
    song_io_edit_push();
    nt = &k->d.notes[i];
    if (step >= 0 && step < (int)CLIP_MAX_STEPS) nt->step = (uint8_t)step;
    if (note >= 0) nt->note = (uint8_t)(note > 127 ? 127 : note);
    if (vel >= 0)  nt->vel  = (uint8_t)(vel < 1 ? 1 : vel > 127 ? 127 : vel);
    if (len >= 0)  nt->len  = (uint8_t)(len < 1 ? 1 : len > (int)CLIP_MAX_STEPS ? (int)CLIP_MAX_STEPS : len);
    edited();
    synced(c);
}

int song_io_motion_param(int c, int lane)
{
    clip_t *k = clip_at(c);
    if (!k || lane < 0 || lane >= (int)CLIP_MOTION_LANES || k->motion[lane].param == PARAM_NONE)
        return -1;
    return (int)k->motion[lane].param;
}
int song_io_motion_lane(int c, int param, int create)
{
    clip_t *k = clip_at(c);
    int i;
    if (!k || !k->used || param < 0 || param == (int)PARAM_NONE)
        return -1;
    for (i = 0; i < (int)CLIP_MOTION_LANES; i++)
        if (k->motion[i].param == (uint16_t)param)
            return i;
    if (!create)
        return -1;
    for (i = 0; i < (int)CLIP_MOTION_LANES; i++)
        if (k->motion[i].param == PARAM_NONE) {
            song_io_edit_push();
            k->motion[i].param = (uint16_t)param;
            edited();
            synced(c);
            return i;
        }
    return -1;                                         /* two lanes at most */
}
int song_io_motion_get(int c, int lane, int step)
{
    clip_t *k = clip_at(c);
    uint8_t v;
    if (!k || lane < 0 || lane >= (int)CLIP_MOTION_LANES || step < 0 || step >= (int)CLIP_MAX_STEPS)
        return -1;
    v = k->motion[lane].v[step];
    return v == MOTION_NONE ? -1 : (int)v;
}
void song_io_motion_set(int c, int lane, int step, int v)
{
    clip_t *k = clip_at(c);
    if (!k || !k->used || lane < 0 || lane >= (int)CLIP_MOTION_LANES ||
        step < 0 || step >= (int)CLIP_MAX_STEPS)
        return;
    if (k->motion[lane].param == PARAM_NONE)
        return;                                        /* the lane must be claimed first */
    song_io_edit_push();
    k->motion[lane].v[step] = (uint8_t)(v < 0 ? MOTION_NONE : v > 127 ? 127 : v);
    edited();
    synced(c);
}
void song_io_motion_clear(int c, int lane)
{
    clip_t *k = clip_at(c);
    int s;
    if (!k || !k->used || lane < 0 || lane >= (int)CLIP_MOTION_LANES)
        return;
    if (k->motion[lane].param == PARAM_NONE)
        return;
    song_io_edit_push();
    k->motion[lane].param = PARAM_NONE;
    for (s = 0; s < (int)CLIP_MAX_STEPS; s++)
        k->motion[lane].v[s] = MOTION_NONE;
    edited();
    synced(c);
}
int song_io_undo_clip(void) { return undo.cur ? (int)undo.e[undo.cur - 1u].index : -1; }
int song_io_redo_clip(void) { return undo.cur < undo.n ? (int)undo.e[undo.cur].index : -1; }
int song_io_undo_depth(void) { return (int)undo.n; }
int song_io_undo_pos(void) { return (int)undo.cur; }

/* ---- undo ---------------------------------------------------------------- */

void song_io_push_clear(int clip, const void *old_clip)
{
    undo_stack_push_clear(&undo, (uint8_t)clip, (const clip_t *)old_clip);
}
void song_io_push_record(int clip, const void *old_clip)
{
    undo_stack_push_record(&undo, (uint8_t)clip, (const clip_t *)old_clip);
}
void song_io_push_copy(int dst, int src, const void *old_dst)
{
    undo_stack_push_copy(&undo, (uint8_t)dst, (uint8_t)src, (const clip_t *)old_dst);
}

static int undo_run(int redo)
{
    int rc = redo ? undo_stack_redo(&undo, song(), &store, 0) : undo_stack_undo(&undo, song(), &store, 0);
    if (rc == UNDO_DONE) {
        dirty = 1;                                      /* the song changed: write it back on the next save */
        failed = 0;
        live_bridge_song_reload();
    }
    return rc;
}

int song_io_undo(void) { return undo_run(0); }
int song_io_redo(void) { return undo_run(1); }
int song_io_undo_type(void) { return (int)undo_stack_undo_type(&undo); }
int song_io_redo_type(void) { return (int)undo_stack_redo_type(&undo); }

/* ---- takes (T9i) --------------------------------------------------------- */

int song_io_take_save(int slot, const void *hdr, const void *ev)
{
    return song_store_take_save(&store, slot, (const take_hdr_t *)hdr, (const take_event_t *)ev);
}
int song_io_take_load(int slot, void *hdr, void *ev, uint32_t max_events)
{
    return song_store_take_load(&store, slot, (take_hdr_t *)hdr, (take_event_t *)ev, max_events);
}
