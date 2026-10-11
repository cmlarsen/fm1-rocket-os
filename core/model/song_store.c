/* SPDX-License-Identifier: GPL-3.0-only */
/* Song and take storage (T6c). See core/model/song_store.h for the format and
 * the region. Integer only, no heap, bounded time per call.
 *
 * lz.c is X0X's compressor, included here so the static lz_* entry points are
 * visible. This file must therefore be its own translation unit (as the clip
 * engine is), not unity-included beside firmware/src/app/project.c, which
 * already includes lz.c. */
#include <stdint.h>
#include "song_store.h"
#include "../../firmware/src/app/lz.c"

#define SST_MAGIC 0x31545353u                 /* "SST1" */
#define SST_HDR ((uint32_t)sizeof(sst_hdr_t))
#define SST_SONG_EXT_BYTES (SONG_STORE_SONG_SECTORS * SONG_STORE_SECTOR)
#define SST_TAKE_EXT_BYTES (SONG_STORE_TAKE_SECTORS * SONG_STORE_SECTOR)

enum { SST_KIND_SONG = 1, SST_KIND_TAKE = 2 };

/* Header written last: its CRC (hcrc) commits the record. */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t kind;
    uint8_t slot;
    uint8_t flags;
    uint16_t rawlen;                          /* uncompressed payload bytes */
    uint32_t seq;
    uint32_t len;                             /* stored (song: compressed) payload bytes */
    uint32_t crc;                             /* CRC-32 of the stored payload */
    char name[SONG_NAME_LEN];                 /* song name, so list() need not decompress */
    uint32_t hcrc;                            /* CRC-32 of this header up to hcrc */
} sst_hdr_t;

_Static_assert(sizeof(sst_hdr_t) == 40, "sst_hdr_t");
_Static_assert(SONG_STORE_BASE >= 0x9F000u + SONG_STORE_SECTOR, "clear of the FM6 patch sector");
_Static_assert(SONG_STORE_BASE + SONG_STORE_SECTORS * SONG_STORE_SECTOR <= 0xDC000u, "clear of PANEL/MOTION");
_Static_assert((SONG_STORE_SONG_EXTENTS * SONG_STORE_SONG_SECTORS +
                SONG_STORE_TAKE_EXTENTS * SONG_STORE_TAKE_SECTORS) <= SONG_STORE_SECTORS, "extents fit");
_Static_assert(SST_SONG_EXT_BYTES - 40u >= 14465u, "worst-case song (incompressible) fits in a song extent");
_Static_assert(SST_TAKE_EXT_BYTES - 40u >= 8u + TAKE_MAX_EVENTS * 8u, "worst-case take fits in a take extent");

/* ---- CRC-32 (zlib) ------------------------------------------------------ */

static const uint32_t SST_CRC_T[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu};

static uint32_t sst_crc(uint32_t c, const uint8_t *p, uint32_t n)
{
    while (n--) {
        c ^= *p++;
        c = (c >> 4) ^ SST_CRC_T[c & 15u];
        c = (c >> 4) ^ SST_CRC_T[c & 15u];
    }
    return c;
}

static uint32_t sst_crc_of(const void *p, uint32_t n)
{
    return ~sst_crc(0xFFFFFFFFu, (const uint8_t *)p, n);
}

/* ---- flash: region checks and byte access ------------------------------ */

static int sst_in_region(uint32_t off, uint32_t n)
{
    uint32_t rel;
    if (off < SONG_STORE_BASE)
        return 0;
    rel = off - SONG_STORE_BASE;
    if (rel > SONG_STORE_SECTORS * SONG_STORE_SECTOR)
        return 0;
    return n <= SONG_STORE_SECTORS * SONG_STORE_SECTOR - rel;
}

static int sst_read(const song_store_t *st, uint32_t off, void *dst, uint32_t n)
{
    if (!sst_in_region(off, n))
        return -1;
    return st->fb.read(st->fb.ctx, off, dst, n) ? -1 : 0;
}

static int sst_erase(const song_store_t *st, uint32_t off)
{
    if (!sst_in_region(off, SONG_STORE_SECTOR) || ((off - SONG_STORE_BASE) & (SONG_STORE_SECTOR - 1u)))
        return -1;
    return st->fb.erase(st->fb.ctx, off) ? -1 : 0;
}

static int sst_program(const song_store_t *st, uint32_t off, const void *src, uint32_t n)
{
    if (!sst_in_region(off, n) || n == 0 || n > 256u || ((off & 0xFFu) + n) > 256u)
        return -1;
    return st->fb.program(st->fb.ctx, off, src, n) ? -1 : 0;
}

static uint32_t sst_song_ext_off(uint32_t i)
{
    return SONG_STORE_BASE + i * SONG_STORE_SONG_SECTORS * SONG_STORE_SECTOR;
}

static uint32_t sst_take_ext_off(uint32_t j)
{
    return SONG_STORE_BASE +
           (SONG_STORE_SONG_EXTENTS * SONG_STORE_SONG_SECTORS + j * SONG_STORE_TAKE_SECTORS) * SONG_STORE_SECTOR;
}

/* ---- header ------------------------------------------------------------- */

static uint32_t sst_put_hdr(sst_hdr_t *h, uint16_t kind, uint8_t slot, uint32_t seq, uint32_t rawlen,
                            uint32_t len, uint32_t crc, const char *name)
{
    int i;
    h->magic = SST_MAGIC;
    h->version = SONG_STORE_VERSION;
    h->kind = kind;
    h->slot = slot;
    h->flags = 0;
    h->rawlen = (uint16_t)rawlen;
    h->seq = seq;
    h->len = len;
    h->crc = crc;
    for (i = 0; i < (int)SONG_NAME_LEN; i++)
        h->name[i] = name ? name[i] : 0;
    h->hcrc = sst_crc_of(h, (uint32_t)((const uint8_t *)&h->hcrc - (const uint8_t *)h));
    return h->hcrc;
}

/* header fields consistent with its kind and range: 0 = ok */
static int sst_hdr_ok(const sst_hdr_t *h, uint32_t cap)
{
    if (h->magic != SST_MAGIC || h->version != SONG_STORE_VERSION)
        return 0;
    if (h->slot >= SONG_STORE_SLOTS)
        return 0;
    if (h->kind != SST_KIND_SONG && h->kind != SST_KIND_TAKE)
        return 0;
    if (h->len == 0 || h->len > cap - SST_HDR || h->rawlen == 0)
        return 0;
    if (h->hcrc != sst_crc_of(h, (uint32_t)((const uint8_t *)&h->hcrc - (const uint8_t *)h)))
        return 0;
    if (h->kind == SST_KIND_SONG) {
        if (h->rawlen != sizeof(song_t))
            return 0;
    } else {
        if (h->rawlen != h->len || h->rawlen < 8u || h->rawlen > 8u + TAKE_MAX_EVENTS * 8u ||
            ((h->rawlen - 8u) % 8u) != 0u)
            return 0;
    }
    return 1;
}

static int sst_read_hdr(const song_store_t *st, uint32_t off, sst_hdr_t *h)
{
    return sst_read(st, off, h, SST_HDR);
}

static int sst_payload_crc(const song_store_t *st, uint32_t off, uint32_t len, uint32_t want)
{
    uint8_t buf[256];
    uint32_t c = 0xFFFFFFFFu, left = len;
    while (left) {
        uint32_t k = left > (uint32_t)sizeof buf ? (uint32_t)sizeof buf : left;
        if (sst_read(st, off, buf, k))
            return -1;
        c = sst_crc(c, buf, k);
        off += k;
        left -= k;
    }
    return ~c == want ? 0 : -1;
}

/* ---- write: compression sink straight to flash ------------------------- */

typedef struct {
    song_store_t *st;
    uint32_t abs;                             /* absolute offset of the next byte */
    uint32_t page_abs;                        /* absolute offset of buf[0] */
    uint32_t nbuf;
    uint32_t len;
    uint32_t limit;
    uint32_t crc;
    int err;
    uint8_t buf[256];
} sst_wr_t;

static int sst_wr_flush(sst_wr_t *w)
{
    if (!w->nbuf)
        return 0;
    if (sst_program(w->st, w->page_abs, w->buf, w->nbuf)) {
        w->err = 1;
        return -1;
    }
    w->page_abs += w->nbuf;
    w->nbuf = 0;
    return 0;
}

static int sst_wr_put(void *ctx, const uint8_t *p, uint32_t n)
{
    sst_wr_t *w = (sst_wr_t *)ctx;
    uint32_t i;
    if (w->err || w->len + n > w->limit) {
        w->err = 1;
        return -1;
    }
    w->crc = sst_crc(w->crc, p, n);
    for (i = 0; i < n; i++) {
        if (w->nbuf == 0)
            w->page_abs = w->abs;
        if ((w->page_abs & 0xFFu) + w->nbuf == 256u) {
            if (sst_wr_flush(w))
                return -1;
            w->page_abs = w->abs;
        }
        w->buf[w->nbuf++] = p[i];
        w->abs++;
        w->len++;
    }
    return 0;
}

/* raw payload (takes); `crc` carries the running (non-finalised) value */
static int sst_write_raw(const song_store_t *st, uint32_t off, const uint8_t *p, uint32_t n, uint32_t *crc)
{
    uint32_t c = *crc;
    while (n) {
        uint32_t room = 256u - (off & 0xFFu);
        uint32_t k = n < room ? n : room;
        if (sst_program(st, off, p, k))
            return -1;
        c = sst_crc(c, p, k);
        off += k;
        p += k;
        n -= k;
    }
    *crc = c;
    return 0;
}

/* ---- write a whole record (erase, payload, header last) ---------------- */

static int sst_erase_extent(const song_store_t *st, uint32_t off, uint32_t sectors)
{
    uint32_t i;
    for (i = 0; i < sectors; i++)
        if (sst_erase(st, off + i * SONG_STORE_SECTOR))
            return -1;
    return 0;
}

static int sst_commit(const song_store_t *st, uint32_t off, const sst_hdr_t *h)
{
    sst_hdr_t chk;
    uint8_t buf[256];
    uint32_t left = h->len, c = 0xFFFFFFFFu, at = off + SST_HDR;
    if (sst_program(st, off, h, SST_HDR))       /* the commit record, last */
        return -1;
    if (sst_read_hdr(st, off, &chk) || chk.magic != SST_MAGIC || chk.seq != h->seq || chk.crc != h->crc ||
        chk.hcrc != h->hcrc)
        return -1;
    while (left) {
        uint32_t k = left > (uint32_t)sizeof buf ? (uint32_t)sizeof buf : left;
        if (sst_read(st, at, buf, k))
            return -1;
        c = sst_crc(c, buf, k);
        at += k;
        left -= k;
    }
    return ~c == h->crc ? 0 : -1;
}

static int sst_write_song(song_store_t *st, uint32_t off, uint8_t slot, uint32_t seq, const song_t *song)
{
    sst_wr_t w;
    lz_sink_t sink;
    sst_hdr_t h;
    uint32_t crc;
    if (sst_erase_extent(st, off, SONG_STORE_SONG_SECTORS))
        return -1;
    w.st = st;
    w.abs = off + SST_HDR;
    w.page_abs = w.abs;
    w.nbuf = 0u;
    w.len = 0u;
    w.limit = SST_SONG_EXT_BYTES - SST_HDR;
    w.crc = 0xFFFFFFFFu;
    w.err = 0;
    sink.put = sst_wr_put;
    sink.ctx = &w;
    if (lz_compress((const uint8_t *)song, sizeof(song_t), &sink) || sst_wr_flush(&w) || w.err)
        return -1;
    crc = ~w.crc;
    sst_put_hdr(&h, SST_KIND_SONG, slot, seq, sizeof(song_t), w.len, crc, song->name);
    return sst_commit(st, off, &h);
}

static int sst_write_take(song_store_t *st, uint32_t off, uint8_t slot, uint32_t seq,
                          const take_hdr_t *hdr, const take_event_t *ev)
{
    uint8_t tmp[8];
    sst_hdr_t h;
    uint32_t c = 0xFFFFFFFFu, i;
    if (sst_erase_extent(st, off, SONG_STORE_TAKE_SECTORS))
        return -1;
    for (i = 0; i < 8u; i++)
        tmp[i] = ((const uint8_t *)hdr)[i];
    if (sst_write_raw(st, off + SST_HDR, tmp, 8u, &c))
        return -1;
    if (hdr->nevents)
        if (sst_write_raw(st, off + SST_HDR + 8u, (const uint8_t *)ev, hdr->nevents * 8u, &c))
            return -1;
    sst_put_hdr(&h, SST_KIND_TAKE, slot, seq, 8u + hdr->nevents * 8u, 8u + hdr->nevents * 8u, ~c, 0);
    return sst_commit(st, off, &h);
}

/* ---- directory helpers -------------------------------------------------- */

static void sst_zero(void *p, uint32_t n)
{
    uint8_t *b = (uint8_t *)p;
    uint32_t i;
    for (i = 0; i < n; i++)
        b[i] = 0;
}

static int sst_song_ext_used(const song_store_t *st, int ext)
{
    int s;
    for (s = 0; s < (int)SONG_STORE_SLOTS; s++)
        if (st->slot[s].song_ext == (int8_t)ext)
            return 1;
    return 0;
}

static int sst_take_ext_used(const song_store_t *st, int ext)
{
    int s;
    for (s = 0; s < (int)SONG_STORE_SLOTS; s++)
        if (st->slot[s].take_ext == (int8_t)ext || st->slot[s].prev_take_ext == (int8_t)ext)
            return 1;
    return 0;
}

/* the first extent of the kind no slot keeps; there is always one (one spare) */
static int sst_pick_song_ext(const song_store_t *st)
{
    int i;
    for (i = 0; i < (int)SONG_STORE_SONG_EXTENTS; i++)
        if (!sst_song_ext_used(st, i))
            return i;
    return -1;
}

static int sst_pick_take_ext(song_store_t *st)
{
    int i, s, best = -1;
    uint32_t best_seq = 0;
    for (i = 0; i < (int)SONG_STORE_TAKE_EXTENTS; i++)
        if (!sst_take_ext_used(st, i))
            return i;
    for (s = 0; s < (int)SONG_STORE_SLOTS; s++) {   /* full: reuse the oldest kept previous take */
        if (st->slot[s].prev_take_ext >= 0 &&
            (best < 0 || st->slot[s].prev_take_seq < best_seq)) {
            best = s;
            best_seq = st->slot[s].prev_take_seq;
        }
    }
    if (best < 0)
        return -1;
    i = st->slot[best].prev_take_ext;
    st->slot[best].prev_take_ext = -1;
    st->slot[best].prev_take_seq = 0u;
    return i;
}

/* ---- fill a valid empty song ------------------------------------------- */

void song_store_empty(song_t *out)
{
    static const uint8_t FIRST[NROLES] = {0u, 7u, 12u, 19u, 24u};
    static const uint8_t NC[NROLES] = {ROLE_CLIPS_DRUMS, ROLE_CLIPS_BASS, ROLE_CLIPS_LEAD, ROLE_CLIPS_CHORDS, ROLE_CLIPS_FX};
    static const uint8_t ENG[NROLES] = {ENG_909, ENG_303, ENG_FM6, ENG_FM6, ENG_NOISE};
    int i;
    sst_zero(out, (uint32_t)sizeof *out);
    out->magic = SONG_MAGIC;
    out->version = MODEL_VERSION;
    out->bpm_x10 = 1250u;
    out->swing = 0u;
    out->quant_default = Q_BAR;
    for (i = 0; i < NROLES; i++) {
        out->role[i].engine = ENG[i];
        out->role[i].preset = 0u;
        out->role[i].volume = 100u;
        out->role[i].muted = 0u;
        out->role[i].first_clip = FIRST[i];
        out->role[i].nclips = NC[i];
    }
    for (i = 0; i < (int)NKEYS; i++)
        out->key_clip[i] = (uint8_t)i;
}

/* ---- validation --------------------------------------------------------- */

int song_validate(const song_t *s)
{
    static const uint8_t FIRST[NROLES] = {0u, 7u, 12u, 19u, 24u};
    static const uint8_t NC[NROLES] = {ROLE_CLIPS_DRUMS, ROLE_CLIPS_BASS, ROLE_CLIPS_LEAD, ROLE_CLIPS_CHORDS, ROLE_CLIPS_FX};
    int i, k;
    if (!s || s->magic != SONG_MAGIC)
        return -1;
    if (s->version != MODEL_VERSION)
        return -1;
    if (s->bpm_x10 == 0)
        return -1;
    if (s->swing > 100 || s->quant_default >= NQUANT || s->live_channel > 16)
        return -1;
    if (s->pc_launch > 1 || s->stage_lock > 1 || s->has_take > 1)
        return -1;
    for (i = 0; i < NROLES; i++) {
        if (s->role[i].engine >= NENGINES || s->role[i].muted > 1 || s->role[i].volume > 127)
            return -1;
        if (s->role[i].first_clip != FIRST[i] || s->role[i].nclips != NC[i])
            return -1;                        /* the role ranges are fixed in model v1 */
    }
    for (i = 0; i < (int)NKEYS; i++)
        if (s->key_clip[i] != CLIP_NONE && s->key_clip[i] >= NCLIPS)
            return -1;
    for (i = 0; i < (int)NCLIPS; i++) {
        const clip_t *c = &s->clip[i];
        if (c->used > 1 || c->mode >= 2 || c->quant >= NQUANT)
            return -1;
        if (c->nnotes > (int)CLIP_MAX_NOTES)
            return -1;
        if ((c->used || c->nnotes || c->len) && (c->len < 1u || c->len > (int)CLIP_MAX_STEPS))
            return -1;
        if (clip_role((uint8_t)i) != ROLE_DRUMS) {
            for (k = 0; k < (int)c->nnotes; k++) {
                const note_t *n = &c->d.notes[k];
                if (n->step >= (int)CLIP_MAX_STEPS || n->len < 1u || n->len > (int)CLIP_MAX_STEPS)
                    return -1;
                if (n->note > 127 || n->vel > 127)
                    return -1;
            }
        }
    }
    return 0;
}

/* ---- init (scan all extents) ------------------------------------------- */

static void sst_slot_reset(song_store_t *st, int s)
{
    st->slot[s].song_ext = -1;
    st->slot[s].take_ext = -1;
    st->slot[s].prev_take_ext = -1;
    st->slot[s].song_seq = 0u;
    st->slot[s].take_seq = 0u;
    st->slot[s].prev_take_seq = 0u;
    st->slot[s].has_song = 0u;
    st->slot[s].deleted = 0u;
}

static void sst_take_record(song_store_t *st, uint8_t slot, int ext, uint32_t seq)
{
    if (!st->slot[slot].take_seq || seq > st->slot[slot].take_seq) {
        st->slot[slot].take_ext = (int8_t)ext;
        st->slot[slot].take_seq = seq;
        st->slot[slot].prev_take_ext = -1;      /* the previous copy is only session state */
        st->slot[slot].prev_take_seq = 0u;
    }
}

int song_store_init(song_store_t *st, const song_flash_t *fb)
{
    uint32_t max_seq = 0;
    int i, s;
    if (!st || !fb)
        return -1;
    for (s = 0; s < (int)SONG_STORE_SLOTS; s++)
        sst_slot_reset(st, s);
    st->next_seq = 1u;
    st->ready = 0u;
    st->fb = *fb;
    for (i = 0; i < (int)SONG_STORE_SONG_EXTENTS; i++) {
        uint32_t off = sst_song_ext_off((uint32_t)i);
        sst_hdr_t h;
        if (sst_read_hdr(st, off, &h) || !sst_hdr_ok(&h, SST_SONG_EXT_BYTES) || h.kind != SST_KIND_SONG)
            continue;
        if (sst_payload_crc(st, off + SST_HDR, h.len, h.crc))
            continue;
        if (h.seq > max_seq)
            max_seq = h.seq;
        if (!st->slot[h.slot].has_song || h.seq > st->slot[h.slot].song_seq) {
            st->slot[h.slot].song_ext = (int8_t)i;
            st->slot[h.slot].song_seq = h.seq;
            st->slot[h.slot].has_song = 1u;
        }
    }
    for (i = 0; i < (int)SONG_STORE_TAKE_EXTENTS; i++) {
        uint32_t off = sst_take_ext_off((uint32_t)i);
        sst_hdr_t h;
        if (sst_read_hdr(st, off, &h) || !sst_hdr_ok(&h, SST_TAKE_EXT_BYTES) || h.kind != SST_KIND_TAKE)
            continue;
        if (sst_payload_crc(st, off + SST_HDR, h.len, h.crc))
            continue;
        if (h.seq > max_seq)
            max_seq = h.seq;
        sst_take_record(st, h.slot, i, h.seq);
    }
    st->next_seq = max_seq + 1u;
    if (!st->next_seq)
        st->next_seq = 1u;
    st->ready = 1u;
    return 0;
}

/* ---- load --------------------------------------------------------------- */

typedef struct {
    const song_store_t *st;
    uint32_t abs, left;
    uint8_t buf[256];
    uint32_t nbuf, pos;
    int err;
} sst_rd_t;

static int sst_rd_get(void *ctx)
{
    sst_rd_t *r = (sst_rd_t *)ctx;
    if (r->pos >= r->nbuf) {
        uint32_t want = r->left > (uint32_t)sizeof r->buf ? (uint32_t)sizeof r->buf : r->left;
        if (!want)
            return -1;
        if (sst_read(r->st, r->abs, r->buf, want)) {
            r->err = 1;
            return -1;
        }
        r->abs += want;
        r->left -= want;
        r->nbuf = want;
        r->pos = 0;
    }
    return r->buf[r->pos++];
}

/* one candidate: header, payload CRC, decompress, validate; malformed is skipped */
static int sst_try_song(const song_store_t *st, int ext, uint8_t slot, song_t *out)
{
    uint32_t off = sst_song_ext_off((uint32_t)ext);
    sst_hdr_t h;
    sst_rd_t rd;
    lz_src_t src;
    if (sst_read_hdr(st, off, &h) || !sst_hdr_ok(&h, SST_SONG_EXT_BYTES) || h.kind != SST_KIND_SONG ||
        h.slot != slot)
        return -1;
    if (sst_payload_crc(st, off + SST_HDR, h.len, h.crc))
        return -1;
    rd.st = st;
    rd.abs = off + SST_HDR;
    rd.left = h.len;
    rd.nbuf = 0u;
    rd.pos = 0u;
    rd.err = 0;
    src.get = sst_rd_get;
    src.ctx = &rd;
    if (lz_decompress(&src, (uint8_t *)out, sizeof(song_t)) || rd.err)
        return -1;
    return song_validate(out);
}

int song_store_load(song_store_t *st, int slot, song_t *out)
{
    int order[SONG_STORE_SONG_EXTENTS], n = 0, i, j, excl = -1;
    uint32_t seq[SONG_STORE_SONG_EXTENTS];
    if (!st || !st->ready || !out || slot < 0 || slot >= (int)SONG_STORE_SLOTS) {
        if (out)
            song_store_empty(out);
        return -1;
    }
    /* the directory's copy first (after a take swap the directory is the truth) */
    if (st->slot[slot].song_ext >= 0 && sst_try_song(st, st->slot[slot].song_ext, (uint8_t)slot, out) == 0)
        return 0;
    excl = st->slot[slot].song_ext;
    for (i = 0; i < (int)SONG_STORE_SONG_EXTENTS; i++) {
        sst_hdr_t h;
        if (i == excl)
            continue;
        if (sst_read_hdr(st, sst_song_ext_off((uint32_t)i), &h) || !sst_hdr_ok(&h, SST_SONG_EXT_BYTES) ||
            h.kind != SST_KIND_SONG || h.slot != slot)
            continue;
        order[n] = i;
        seq[n] = h.seq;
        n++;
    }
    for (i = 0; i < n; i++) {                 /* newest first */
        int best = -1;
        for (j = 0; j < n; j++) {
            if (order[j] < 0)
                continue;
            if (best < 0 || seq[j] > seq[(unsigned)best])
                best = j;
        }
        if (best < 0)
            break;
        if (sst_try_song(st, order[best], (uint8_t)slot, out) == 0)
            return 0;
        order[best] = -1;
    }
    song_store_empty(out);
    return -1;
}

/* ---- save / delete / list ---------------------------------------------- */

int song_store_save(song_store_t *st, int slot, const song_t *song)
{
    int ext;
    uint32_t seq, off;
    if (!st || !st->ready || slot < 0 || slot >= (int)SONG_STORE_SLOTS)
        return -1;
    if (song_validate(song))
        return -2;
    ext = sst_pick_song_ext(st);
    if (ext < 0)
        return -3;
    seq = st->next_seq;
    off = sst_song_ext_off((uint32_t)ext);
    if (sst_write_song(st, off, (uint8_t)slot, seq, song))
        return -4;
    st->next_seq = seq + 1u;
    st->slot[slot].song_ext = (int8_t)ext;    /* the old extent is now unowned, its bytes kept */
    st->slot[slot].song_seq = seq;
    st->slot[slot].has_song = 1u;
    st->slot[slot].deleted = 0u;
    return 0;
}

int song_store_delete(song_store_t *st, int slot)
{
    if (!st || slot < 0 || slot >= (int)SONG_STORE_SLOTS || !st->slot[slot].has_song)
        return -1;
    st->slot[slot].deleted = 1u;               /* data stays until the slot is reused */
    return 0;
}

int song_store_undelete(song_store_t *st, int slot)
{
    if (!st || slot < 0 || slot >= (int)SONG_STORE_SLOTS || !st->slot[slot].has_song)
        return -1;
    st->slot[slot].deleted = 0u;
    return 0;
}

int song_store_is_deleted(const song_store_t *st, int slot)
{
    if (!st || slot < 0 || slot >= (int)SONG_STORE_SLOTS)
        return 0;
    return st->slot[slot].deleted ? 1 : 0;
}

int song_store_list(const song_store_t *st, int slot, song_slot_t *out)
{
    int i;
    if (!out)
        return -1;
    for (i = 0; i < (int)SONG_NAME_LEN; i++)
        out->name[i] = 0;
    out->used = 0u;
    out->deleted = 0u;
    out->has_take = 0u;
    if (!st || slot < 0 || slot >= (int)SONG_STORE_SLOTS)
        return -1;
    out->used = st->slot[slot].has_song;
    out->deleted = st->slot[slot].deleted;
    out->has_take = st->slot[slot].take_ext >= 0 ? 1u : 0u;
    if (st->slot[slot].has_song && st->slot[slot].song_ext >= 0) {
        sst_hdr_t h;
        if (sst_read_hdr(st, sst_song_ext_off((uint32_t)st->slot[slot].song_ext), &h) == 0 &&
            sst_hdr_ok(&h, SST_SONG_EXT_BYTES))
            for (i = 0; i < (int)SONG_NAME_LEN; i++)
                out->name[i] = h.name[i];
    }
    return 0;
}

/* ---- takes -------------------------------------------------------------- */

static int sst_try_take(const song_store_t *st, int ext, uint8_t slot, take_hdr_t *hdr, take_event_t *ev,
                        uint32_t max_events)
{
    uint32_t off = sst_take_ext_off((uint32_t)ext);
    sst_hdr_t h;
    uint32_t nevents, c = 0xFFFFFFFFu;
    uint8_t b[8];
    if (sst_read_hdr(st, off, &h) || !sst_hdr_ok(&h, SST_TAKE_EXT_BYTES) || h.kind != SST_KIND_TAKE ||
        h.slot != slot)
        return -1;
    nevents = (h.rawlen - 8u) / 8u;
    if (nevents > max_events)
        return -2;
    if (sst_read(st, off + SST_HDR, b, 8u))
        return -1;
    {
        int i;
        for (i = 0; i < 8; i++)
            ((uint8_t *)hdr)[i] = b[i];
    }
    c = sst_crc(c, b, 8u);
    if (nevents) {
        if (sst_read(st, off + SST_HDR + 8u, ev, nevents * 8u))
            return -1;
        c = sst_crc(c, (const uint8_t *)ev, nevents * 8u);
    }
    return ~c == h.crc ? 0 : -1;
}

int song_store_take_load(song_store_t *st, int slot, take_hdr_t *hdr, take_event_t *ev, uint32_t max_events)
{
    int order[SONG_STORE_TAKE_EXTENTS], n = 0, i, j, excl = -1;
    uint32_t seq[SONG_STORE_TAKE_EXTENTS];
    if (!st || !st->ready || !hdr || slot < 0 || slot >= (int)SONG_STORE_SLOTS)
        return -1;
    /* the directory's copy first: after song_store_take_swap (undo) the swap is
     * the truth, and the extent with the newest sequence number is the "other"
     * one, not what the caller wants. */
    if (st->slot[slot].take_ext >= 0 &&
        sst_try_take(st, st->slot[slot].take_ext, (uint8_t)slot, hdr, ev, max_events) == 0)
        return 0;
    excl = st->slot[slot].take_ext;
    for (i = 0; i < (int)SONG_STORE_TAKE_EXTENTS; i++) {
        sst_hdr_t h;
        if (i == excl)
            continue;
        if (sst_read_hdr(st, sst_take_ext_off((uint32_t)i), &h) || !sst_hdr_ok(&h, SST_TAKE_EXT_BYTES) ||
            h.kind != SST_KIND_TAKE || h.slot != slot)
            continue;
        order[n] = i;
        seq[n] = h.seq;
        n++;
    }
    for (i = 0; i < n; i++) {
        int best = -1;
        for (j = 0; j < n; j++) {
            if (order[j] < 0)
                continue;
            if (best < 0 || seq[j] > seq[(unsigned)best])
                best = j;
        }
        if (best < 0)
            break;
        if (sst_try_take(st, order[best], (uint8_t)slot, hdr, ev, max_events) == 0)
            return 0;
        order[best] = -1;
    }
    return -1;
}

int song_store_take_save(song_store_t *st, int slot, const take_hdr_t *hdr, const take_event_t *ev)
{
    int ext, old;
    uint32_t seq, off;
    if (!st || !st->ready || slot < 0 || slot >= (int)SONG_STORE_SLOTS || !hdr)
        return -1;
    if (hdr->nevents > TAKE_MAX_EVENTS || (hdr->nevents && !ev))
        return -2;
    ext = sst_pick_take_ext(st);
    if (ext < 0)
        return -3;
    seq = st->next_seq;
    off = sst_take_ext_off((uint32_t)ext);
    if (sst_write_take(st, off, (uint8_t)slot, seq, hdr, ev))
        return -4;
    st->next_seq = seq + 1u;
    old = st->slot[slot].take_ext;
    st->slot[slot].prev_take_ext = (int8_t)old;   /* keep the previous take for one undo */
    st->slot[slot].prev_take_seq = st->slot[slot].take_seq;
    st->slot[slot].take_ext = (int8_t)ext;
    st->slot[slot].take_seq = seq;
    return 0;
}

int song_store_take_swap(song_store_t *st, int slot)
{
    int8_t t;
    uint32_t q;
    if (!st || slot < 0 || slot >= (int)SONG_STORE_SLOTS || st->slot[slot].prev_take_ext < 0)
        return 0;
    t = st->slot[slot].take_ext;
    st->slot[slot].take_ext = st->slot[slot].prev_take_ext;
    st->slot[slot].prev_take_ext = t;
    q = st->slot[slot].take_seq;
    st->slot[slot].take_seq = st->slot[slot].prev_take_seq;
    st->slot[slot].prev_take_seq = q;
    return 1;
}

/* ---- measurement -------------------------------------------------------- */

typedef struct { uint32_t n; } sst_count_t;

static int sst_count_put(void *ctx, const uint8_t *p, uint32_t n)
{
    (void)p;
    ((sst_count_t *)ctx)->n += n;
    return 0;
}

int song_store_measure(const song_t *song)
{
    sst_count_t c;
    lz_sink_t sink;
    c.n = 0u;
    sink.put = sst_count_put;
    sink.ctx = &c;
    if (lz_compress((const uint8_t *)song, sizeof(song_t), &sink))
        return -1;
    return (int)c.n;
}
