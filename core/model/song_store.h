/* SPDX-License-Identifier: GPL-3.0-only */
/* Song and take storage (T6c).
 *
 * Everything here is plain C with no heap and no hardware: the caller gives a
 * flash region as three byte-level functions (read / erase one sector /
 * program up to one page), so the same code runs against the device driver and
 * a RAM simulator. The region and sector size come from X0X's flash map; the
 * abstract interface is `song_flash_t` below.
 *
 * Records are power-safe. A record is a header written last (magic, version,
 * slot, kind, sequence number, raw and stored lengths, CRC-32 of the payload)
 * followed by the payload. Two records never share a flash extent, and the
 * store keeps one spare extent of each kind, so a save writes into a free
 * extent and only then drops the old one: a power cut at any byte leaves the
 * previous record intact. At init every extent header is scanned and the
 * newest valid record per slot wins, which replaces A/B without needing a
 * directory on flash. The previous take of a slot is kept until its extent is
 * reused, so a replace-take is undoable once.
 *
 * Extents (sectors of 4096 B), all inside the region:
 *   song: SONG_STORE_SONG_EXTENTS x SONG_STORE_SONG_SECTORS (4)   first
 *   take: SONG_STORE_TAKE_EXTENTS x SONG_STORE_TAKE_SECTORS (5)   after
 * A song extent holds any song: the worst case (a song_t of incompressible
 * bytes) compresses to 14,465 B and the record header is 40 B, both inside
 * 4 sectors. A take extent holds one take (header + events), 16,392 B raw.
 *
 * Flavour: integer only, no allocation, bounded time per call. `song_store_save`
 * and `song_store_take_save` erase and program; the save policy (stopped and
 * idle, autosave) lives in the caller. */
#pragma once
#include <stdint.h>
#include "model.h"

/* ---- the region and the geometry (X0X flash map) ----------------------- */
/* Felucca's data area is 0x97000..0xDFFFF. X0X itself keeps 0x97000-0x9EFFF
 * (SOUND/PAT0/PAT1/SONG), 0x9F000 (FM6 patches) and 0xDC000-0xDFFFF
 * (PANEL/MOTION). The 0xA0000..0xDBFFF user-sample area is unused after T1
 * (the product drops BREAK samples), and is the region T6 owns. */

#define SONG_STORE_SECTOR 4096u
#define SONG_STORE_BASE 0xA0000u
#define SONG_STORE_SECTORS 60u                /* 0xA0000..0xDBFFF, 240 KiB */

/* Extent sizes. A song record needs 40 + 14,465 = 14,505 B <= 4 sectors; a take
 * record needs 40 + 16,392 = 16,432 B <= 5 sectors. */
#define SONG_STORE_SONG_SECTORS 4u
#define SONG_STORE_TAKE_SECTORS 5u
/* N slots + one spare extent of each kind. (5+1)*4 + (5+1)*5 = 54 <= 60. */
#define SONG_STORE_SONG_EXTENTS 6u
#define SONG_STORE_TAKE_EXTENTS 6u
#define SONG_STORE_SLOTS 5u                   /* what fits; model.h still says 8 */
#define SONG_STORE_EXTENTS (SONG_STORE_SONG_EXTENTS + SONG_STORE_TAKE_EXTENTS)

#define SONG_STORE_VERSION 1u

/* ---- flash interface --------------------------------------------------- */
/* Offsets are absolute flash sectors (not relative to the region). `program`
 * writes at most one 256-byte page and must not wrap a page. Each function
 * returns 0 on success, non-zero on failure. */
typedef struct {
    int (*read)(void *ctx, uint32_t off, void *dst, uint32_t n);
    int (*erase)(void *ctx, uint32_t off);                          /* one 4096 B sector */
    int (*program)(void *ctx, uint32_t off, const void *src, uint32_t n);   /* <= 256 B, no page wrap */
    void *ctx;
} song_flash_t;

/* ---- store state (caller owns it; no heap) ----------------------------- */
typedef struct {
    int8_t song_ext;             /* extent holding this slot's song, -1 = none */
    int8_t take_ext;             /* extent holding the current take, -1 = none */
    int8_t prev_take_ext;        /* superseded take kept for undo, -1 = none */
    uint32_t song_seq;           /* sequence number of the slot's song record */
    uint32_t take_seq;
    uint32_t prev_take_seq;
    uint8_t has_song;
    uint8_t deleted;             /* RAM-only: keeps the data until the slot is reused */
} song_store_slot_t;

typedef struct {
    song_flash_t fb;
    song_store_slot_t slot[SONG_STORE_SLOTS];
    uint32_t next_seq;
    uint8_t ready;
} song_store_t;

/* what song_store_list reports: enough for the songs screen */
typedef struct {
    char name[SONG_NAME_LEN];
    uint8_t used;                /* a song record exists */
    uint8_t deleted;             /* marked deleted, data still there */
    uint8_t has_take;
} song_slot_t;

/* ---- API --------------------------------------------------------------- */
/* Scan the region and rebuild the directory. 0 = ok. Cheap enough for boot. */
int song_store_init(song_store_t *st, const song_flash_t *fb);

int song_store_save(song_store_t *st, int slot, const song_t *song);
int song_store_load(song_store_t *st, int slot, song_t *out);      /* 0 = loaded */
int song_store_delete(song_store_t *st, int slot);                 /* recoverable */
int song_store_undelete(song_store_t *st, int slot);
int song_store_is_deleted(const song_store_t *st, int slot);
int song_store_list(const song_store_t *st, int slot, song_slot_t *out);

/* One take per song. `hdr->nevents` events are stored raw (no compression). */
int song_store_take_save(song_store_t *st, int slot, const take_hdr_t *hdr, const take_event_t *ev);
int song_store_take_load(song_store_t *st, int slot, take_hdr_t *hdr, take_event_t *ev, uint32_t max_events);
int song_store_take_swap(song_store_t *st, int slot);              /* undo/redo of a replace */

/* A valid empty song, for the load fallback and New > Empty. */
void song_store_empty(song_t *out);

/* Reject a song whose fields are out of range: 0 = ok, -1 = malformed. Used by
 * both save and load so nothing malformed is ever stored or handed out. */
int song_validate(const song_t *song);

/* Compressed size of a song (for the slot budget); -1 if lz refuses. */
int song_store_measure(const song_t *song);
