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
#include "song_templates.h"             /* T8i: the Empty/Starter templates */
/* T8i: the songs_ops_t / song_row_t seam (songs_seam.h, pulled by song_io.h) */
#include "song_io.h"

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
/* TRAM: song_io's open / clip-count path shares the store's one song_t scratch
 * (song_store_scratch_acquire) instead of holding a second 14,352 B buffer. */
static uint8_t ready, dirty, loaded, failed;
static int cur_slot = -1;                /* T8i: the slot the current song lives in, -1 = none yet */
static uint32_t saves;
/* T9i: the take's deferred store state (see the take block below) */
enum { TAKE_NONE, TAKE_SAVE, TAKE_CLEAR };
static uint8_t take_pending;
static uint8_t take_ram_ok;
static uint32_t take_pending_ser;       /* S5: undo_stack_serial() when the pending write was queued */

static song_t *song(void) { return (song_t *)live_bridge_song(); }

/* T8i: the "first free slot" policy: a slot that has never held a song first, then a
 * deleted one. -1 when all are in use. (song_store.h's has_song / deleted.) */
static int first_free_slot(void)
{
    int s;
    for (s = 0; s < (int)SONG_STORE_SLOTS; s++)
        if (!store.slot[s].has_song)
            return s;
    for (s = 0; s < (int)SONG_STORE_SLOTS; s++)
        if (store.slot[s].has_song && store.slot[s].deleted)
            return s;
    return -1;
}

void song_io_boot(uint16_t bpm_x10, int preferred)
{
    int slot;
    song_store_init(&store, &FB);
    undo_stack_init(&undo);
    ready = 0;
    loaded = 0;
    dirty = 0;
    failed = 0;
    cur_slot = -1;
    /* T8i: the last current slot first, then slot 0, then the project */
    slot = (preferred >= 0 && preferred < (int)SONG_STORE_SLOTS) ? preferred : 0;
    if (song_store_load(&store, slot, song()) == 0) {
        loaded = 1;                                     /* T7a: the song is the edited object; use it
                                                         * as it is. The T6i pattern-hash remap is gone. */
        cur_slot = slot;
        live_bridge_song_baseline(0);                   /* nothing to re-derive from the project */
    } else if (slot != 0 && song_store_load(&store, 0, song()) == 0) {
        loaded = 1;                                     /* the preferred slot was deleted: use slot 0 */
        cur_slot = 0;
        live_bridge_song_baseline(0);
    } else {
        /* TCLS: no stored song and no project mapping any more — the first boot (or an empty store)
         * starts from the Starter template, which clamps the tempo itself. */
        song_template_starter(song(), bpm_x10, SONG_TMPL_DEFAULT_KEY);
        cur_slot = -1;
        dirty = 1;                                      /* it must be written back */
    }
    ready = 1;
    live_bridge_song_reload();                          /* the clip engine follows the new song */
    take_pending = TAKE_NONE;                           /* T9i: the take is the song's own */
    take_pending_ser = 0;
    song_io_take_load_ram();
}

int song_io_loaded(void) { return loaded != 0; }
int song_io_dirty(void) { return dirty != 0; }
int song_io_failed(void) { return failed != 0; }
int song_io_wants_save(void) { return dirty && !failed; }
int song_io_saves(void) { return (int)saves; }
void song_io_mark_dirty(void) { dirty = 1; failed = 0; }   /* a new edit re-enables saving */

int song_io_save(void)
{
    int rc, slot;
    if (!ready)
        return -1;
    slot = cur_slot;
    if (slot < 0)
        slot = first_free_slot();                       /* T8i: the project mapping gets a slot */
    if (slot < 0) {
        failed = 1;
        return -1;
    }
    rc = song_store_save_tag(&store, slot, song(), live_bridge_song_hash());
    if (rc == 0) {
        cur_slot = slot;
        loaded = 1;
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
    int rc = 0;
    if (dirty && !failed)
        rc = song_io_save();
    if (rc == 0)
        rc = song_io_take_flush();          /* T9i: the take rides the same stopped-and-idle window */
    return rc;
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
    int type = redo ? (int)undo_stack_redo_type(&undo) : (int)undo_stack_undo_type(&undo);
    int clip = redo ? (undo.cur < undo.n ? (int)undo.e[undo.cur].index : -1)
                    : (undo.cur ? (int)undo.e[undo.cur - 1u].index : -1);
    undo_take_scratch_t ts;
    int rc;
    /* B2: never memcpy the take buffer while an armed/recording/playing take owns it. */
    if (type == (int)UNDO_REPLACE_TAKE &&
        (live_bridge_take_recording() || live_bridge_take_armed() || live_bridge_take_playing()))
        return UNDO_CANNOT;
    ts.hdr = (take_hdr_t *)live_bridge_take_hdr_mut();   /* T9i: the in-RAM take backs the take undo (T6c) */
    ts.ev = (take_event_t *)live_bridge_take_events_mut();
    ts.max_events = TAKE_MAX_EVENTS;
    rc = redo ? undo_stack_redo(&undo, song(), &store, &ts) : undo_stack_undo(&undo, song(), &store, &ts);
    if (rc == UNDO_DONE) {
        if (type == (int)UNDO_REPLACE_TAKE) {           /* keep has_take in step with the restored take */
            song()->has_take = (uint8_t)(ts.hdr->nevents > 0 ? 1 : 0);
            live_bridge_take_loaded((int)ts.hdr->nevents, ts.hdr->length_samples, ts.hdr->bpm_x10);
        }
        dirty = 1;                                      /* the song changed: write it back on the next save */
        failed = 0;
        if (type == UNDO_CLEAR_CLIP || type == UNDO_COPY_CLIP || type == UNDO_RECORD_CLIP) {
            /* T7b re-review: stage the restored clip through the main-loop path; do NOT re-init the
             * clip engine (that would stop the music while it plays). Only song open / load (stopped)
             * re-inits. */
            live_bridge_clip_changed((uint8_t)clip);
            if (!song_io_clip_used(clip))
                live_bridge_stop((uint8_t)clip_role((uint8_t)clip));   /* the clip vanished: stop it */
        }
    }
    return rc;
}

int song_io_undo(void)
{
    if (take_pending != TAKE_NONE && undo_stack_serial(&undo) <= take_pending_ser) {
        if (live_bridge_take_recording() || live_bridge_take_armed() || live_bridge_take_playing())
            return UNDO_CANNOT;                         /* B2: the live take owns the buffer */
        /* T9i: a record/clear not yet flushed, and no newer edit sits above it. The store still
         * holds the previous take, so undo is "cancel the pending write and reload it". S5: the
         * stamp (not undo.cur, which saturates on a full ring) tells whether a newer clip edit
         * exists; if it does, undo that first — true order. */
        take_pending = TAKE_NONE;
        song()->has_take = 1;                           /* a clear had cleared it; the load corrects it */
        if (song_io_take_load_ram() != 0)
            song()->has_take = 0;
        dirty = 1;
        failed = 0;
        return UNDO_DONE;
    }
    return undo_run(0);
}
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

/* T9i: the take's deferred store write. A clear is stored as an empty take (0 events), which
 * supersedes the old record at boot and keeps the old one recoverable for one undo (prev_take). */
static void take_store_empty(int slot)
{
    take_hdr_t h;
    h.nevents = 0;
    h.bpm_x10 = (uint16_t)(song()->bpm_x10 ? song()->bpm_x10 : 1200u);
    h.length_samples = 0;
    song_store_take_save(&store, slot, &h, 0);
}

void song_io_take_request_save(void)
{
    take_pending = TAKE_SAVE;
    take_ram_ok = 1;
    take_pending_ser = undo_stack_serial(&undo);        /* S5: where this op sits in undo order */
    song()->has_take = (uint8_t)(live_bridge_take_nevents() > 0 ? 1 : 0);
    dirty = 1;
    failed = 0;
}
void song_io_take_request_clear(void)
{
    live_bridge_take_clear_ram();
    take_pending = TAKE_CLEAR;
    take_ram_ok = 1;
    take_pending_ser = undo_stack_serial(&undo);        /* S5 */
    song()->has_take = 0;
    dirty = 1;
    failed = 0;
}
int song_io_take_pending(void) { return take_pending != TAKE_NONE; }
int song_io_take_loaded_ram(void) { return take_ram_ok; }

int song_io_take_flush(void)
{
    int slot, rc = 0;
    if (!ready || take_pending == TAKE_NONE)
        return 0;
    slot = cur_slot;
    if (slot < 0)
        slot = first_free_slot();
    if (slot < 0) {
        failed = 1;
        return -1;
    }
    if (take_pending == TAKE_CLEAR || live_bridge_take_nevents() == 0) {
        take_store_empty(slot);
    } else {
        rc = song_store_take_save(&store, slot, (const take_hdr_t *)live_bridge_take_hdr(),
                                  (const take_event_t *)live_bridge_take_events());
        if (rc != 0) {
            failed = 1;
            return -1;
        }
    }
    /* T9i r3/NIT: the durable take undo goes at the position it had when the take ended, not at
     * this deferred flush, so a clip edit made after the take is undone first. */
    undo_stack_push_take_at(&undo, &store, (uint8_t)slot, take_pending_ser);
    take_pending = TAKE_NONE;
    return 0;
}

/* Load the song's stored take into live_bridge's buffer (or clear it when there is none). */
int song_io_take_load_ram(void)
{
    take_hdr_t *hdr;
    take_event_t *ev;
    int slot = cur_slot;
    if (!ready)
        return -1;
    if (live_bridge_take_recording() || live_bridge_take_armed() || live_bridge_take_playing())
        return -1;                                      /* B2: never memcpy over a live take */
    hdr = (take_hdr_t *)live_bridge_take_hdr_mut();
    ev = (take_event_t *)live_bridge_take_events_mut();
    take_ram_ok = 0;
    if (!song()->has_take || slot < 0) {
        live_bridge_take_clear_ram();
        return -1;
    }
    if (song_store_take_load(&store, slot, hdr, ev, TAKE_MAX_EVENTS) != 0 || hdr->nevents == 0) {
        live_bridge_take_clear_ram();
        song()->has_take = 0;
        return -1;
    }
    live_bridge_take_loaded((int)hdr->nevents, hdr->length_samples, hdr->bpm_x10);
    take_ram_ok = 1;
    return 0;
}

/* ==================================================================== T8i === */
/* The songs list and CRUD over the store. Kept in one block, separate from T7a/T7b's
 * clip API, so a parallel change to song_io merges cleanly. */
_Static_assert(SONGS_SLOTS == SONG_SLOTS, "SONGS_SLOTS must track model.h's SONG_SLOTS");
_Static_assert(SONGS_NAME == SONG_NAME_LEN, "SONGS_NAME must track model.h's SONG_NAME_LEN");

void song_io_songs_list(void *ctx, int slot, song_row_t *out)
{
    song_slot_t r;
    int i;
    (void)ctx;
    for (i = 0; i < (int)SONGS_NAME; i++)
        out->name[i] = 0;
    out->used = out->has_take = out->current = 0;
    if (song_store_list(&store, slot, &r) != 0)
        return;
    for (i = 0; i < (int)SONGS_NAME - 1 && r.name[i]; i++)
        out->name[i] = r.name[i];
    out->used = r.used;
    out->has_take = r.has_take;
    out->current = (uint8_t)(slot == cur_slot);
}

int song_io_songs_current_slot(void *ctx) { (void)ctx; return cur_slot; }

/* T8i: the one-level delete undo (SONGS only). It is separate from the clip undo stack,
 * so opening/creating/copying a song can clear the clip undo without losing it. */
static int8_t del_slot = -1;

/* T8i: SONGS is a stopped, quiet view. Every store write refuses while the clip transport
 * runs (the caller shows "SAVE FAILED"). live_bridge's transport is the app's. */
static int store_quiet(void) { return !live_bridge_transport_playing(); }

/* T8i: a song change clears the clip undo (it belongs to one song). */
static void clip_undo_clear(void) { undo_stack_init(&undo); }

int song_io_songs_open(void *ctx, int slot)
{
    (void)ctx;
    if (!ready || slot < 0 || slot >= (int)SONG_STORE_SLOTS)
        return -1;
    if (!store_quiet())
        return -1;
    {
        song_t *s = song_store_scratch_acquire();       /* TRAM: the store's shared scratch */
        if (song_store_load(&store, slot, s) != 0) {    /* deleted or unreadable: g_song is untouched */
            song_store_scratch_release();
            return -1;
        }
        *song() = *s;                                   /* only now change the live song */
        song_store_scratch_release();
    }
    cur_slot = slot;
    loaded = 1;
    dirty = 0;
    failed = 0;
    clip_undo_clear();
    live_bridge_song_reload();
    live_bridge_song_baseline(song_store_tag(&store, slot));
    take_pending = TAKE_NONE;                            /* T9i: the new song's own take */
    take_pending_ser = 0;
    song_io_take_load_ram();
    return 0;
}

int song_io_songs_create(void *ctx, int kind, uint16_t bpm_x10, int key)
{
    song_t s;
    int slot;
    (void)ctx;
    if (!store_quiet())
        return -1;
    slot = first_free_slot();
    if (slot < 0)
        return -2;                                      /* all slots taken */
    if (kind == 1)
        song_template_starter(&s, bpm_x10, key);
    else
        song_template_empty(&s, bpm_x10, key);
    if (song_store_save_tag(&store, slot, &s, 0u) != 0)
        return -1;
    clip_undo_clear();                                  /* creating a song clears the clip undo */
    return slot;
}

int song_io_songs_copy(void *ctx, int src)
{
    song_t s;
    int slot, i;
    char nm[SONG_NAME_LEN];
    (void)ctx;
    if (!store_quiet())
        return -1;
    if (src < 0 || src >= (int)SONG_STORE_SLOTS)
        return -1;
    if (song_store_load(&store, src, &s) != 0)
        return -1;
    slot = first_free_slot();
    if (slot < 0)
        return -2;
    /* a copy gets a new name: the source's, truncated to leave room for " 2" */
    for (i = 0; i < (int)SONG_NAME_LEN; i++)
        nm[i] = 0;
    for (i = 0; i < (int)SONG_NAME_LEN - 3 && s.name[i]; i++)
        nm[i] = s.name[i];
    nm[i] = ' ';
    nm[i + 1] = '2';
    for (i = 0; i < (int)SONG_NAME_LEN; i++)
        s.name[i] = nm[i];
    s.has_take = 0;                                     /* the take is not copied with the song record */
    if (song_store_save_tag(&store, slot, &s, 0u) != 0)
        return -1;
    clip_undo_clear();                                  /* copying a song clears the clip undo */
    return slot;
}

int song_io_songs_rename(void *ctx, int slot, const char *name)
{
    song_t *cur;
    song_t s;
    int i;
    (void)ctx;
    if (!store_quiet())
        return -1;
    if (slot < 0 || slot >= (int)SONG_STORE_SLOTS)
        return -1;
    if (slot == cur_slot) {                             /* the live song: rename in RAM, then save it */
        cur = song();
        for (i = 0; i < (int)SONG_NAME_LEN; i++)
            cur->name[i] = 0;
        for (i = 0; i < (int)SONG_NAME_LEN - 1 && name[i]; i++)
            cur->name[i] = name[i];
        dirty = 1;
        failed = 0;
        return song_io_save();
    }
    if (song_store_load(&store, slot, &s) != 0)
        return -1;
    for (i = 0; i < (int)SONG_NAME_LEN; i++)
        s.name[i] = 0;
    for (i = 0; i < (int)SONG_NAME_LEN - 1 && name[i]; i++)
        s.name[i] = name[i];
    return song_store_save_tag(&store, slot, &s, song_store_tag(&store, slot));
}

int song_io_songs_delete(void *ctx, int slot)
{
    (void)ctx;
    if (!store_quiet())
        return -1;
    if (slot < 0 || slot >= (int)SONG_STORE_SLOTS)
        return -1;
    if (song_store_delete(&store, slot) != 0)
        return -1;
    del_slot = (int8_t)slot;                            /* the one-level delete undo, SONGS only */
    return 0;
}

int song_io_songs_undelete(void *ctx)
{
    (void)ctx;
    if (!store_quiet())
        return -1;                                      /* UNDO_CANNOT */
    if (del_slot < 0)
        return 0;                                       /* UNDO_EMPTY */
    if (!store.slot[del_slot].has_song || !store.slot[del_slot].deleted)
        return -1;                                      /* the slot was reused: can't undo */
    if (song_store_undelete(&store, (uint8_t)del_slot) != 0)
        return -1;
    del_slot = -1;
    return 1;                                           /* UNDO_DONE */
}

int song_io_songs_first_free(void *ctx) { (void)ctx; return first_free_slot(); }

int song_io_songs_can_undelete(void *ctx)
{
    (void)ctx;
    return (del_slot >= 0 && store.slot[del_slot].has_song && store.slot[del_slot].deleted) ? 1 : 0;
}

int song_io_songs_clip_count(void *ctx, int slot)
{
    int i, n = 0;
    song_t *s;
    (void)ctx;
    s = song_store_scratch_acquire();                   /* TRAM: the store's shared scratch */
    if (song_store_load(&store, slot, s) != 0) {
        song_store_scratch_release();
        return -1;
    }
    for (i = 0; i < (int)NCLIPS; i++)
        if (s->clip[i].used)
            n++;
    song_store_scratch_release();
    return n;
}

static const songs_ops_t SONG_OPS = {
    0,
    song_io_songs_list,
    song_io_songs_current_slot,
    song_io_songs_open,
    song_io_songs_create,
    song_io_songs_copy,
    song_io_songs_rename,
    song_io_songs_delete,
    song_io_songs_undelete,
    song_io_songs_first_free,
    song_io_songs_can_undelete,
    song_io_songs_clip_count
};

const songs_ops_t *song_io_songs_ops(void) { return &SONG_OPS; }
