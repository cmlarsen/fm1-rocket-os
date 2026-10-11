/* SPDX-License-Identifier: GPL-3.0-only */
/* T8: the SONGS view's state machine (firmware/src/app/ui_songs.c) against a RAM
 * fake, plus a 1:1 render of every screen to PNG for the reviewer.
 *
 * The fake implements songs_ops_t over an array of song_t (no flash), so the whole
 * flow runs here: list -> new -> create, the 5-slot limit, menu -> open/copy/rename/
 * delete, delete's hold ring, and HOME+REC undo. ui_songs.c draws through gfx.c's
 * statics, so gfx.c, ui_name.c and ui_songs.c share this translation unit, as X0X's
 * x0x.c does. Run by `make unit-songs`, also under ASan/UBSan. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define __attribute__(x)                 /* as host/x0x_host.c does: .pool is a device thing */

/* ---- the framebuffer gfx.c blits into ---- */
static uint16_t fb[240 * 240];
static void lcd_sync(void) {}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    uint32_t i, j;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            if (x + i < 240u && y + j < 240u)
                fb[(y + j) * 240u + x + i] = px[j * w + i];
}

#include "../../firmware/src/gfx.c"
#include "../../firmware/src/app/ui_songs.h"
#undef NKEYS
#include "../../core/model/model.h"
#include "../../firmware/src/app/ui_name.c"
#include "../../firmware/src/app/ui_songs.c"

_Static_assert(SONGS_SLOTS == SONG_SLOTS, "SONGS_SLOTS must track SONG_SLOTS");
_Static_assert(SONGS_NAME == SONG_NAME_LEN, "SONGS_NAME must track SONG_NAME_LEN");

/* ---- PNG writer (stored deflate blocks, no zlib: from ui_name_test.c) ---- */
static void png_write(const char *path)
{
    static uint8_t raw[240 * (1 + 240 * 3)];
    FILE *f = fopen(path, "wb");
    uint32_t crc_t[256], i, j, k, n = 0, a = 1, b = 0, pos, len = sizeof raw;
    uint8_t hdr[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (!f) {
        fprintf(stderr, "cannot write %s\n", path);
        return;
    }
    for (i = 0; i < 256; i++) {
        uint32_t c = i;
        for (k = 0; k < 8; k++)
            c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_t[i] = c;
    }
    for (j = 0; j < 240; j++) {
        raw[n++] = 0;
        for (i = 0; i < 240; i++) {
            uint16_t p = fb[j * 240 + i];
            p = (uint16_t)((p >> 8) | (p << 8));
            raw[n++] = (uint8_t)(((p >> 11) & 31) * 255 / 31);
            raw[n++] = (uint8_t)(((p >> 5) & 63) * 255 / 63);
            raw[n++] = (uint8_t)((p & 31) * 255 / 31);
        }
    }
    for (i = 0; i < len; i++) {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
#define PUT32(v) do { uint8_t q_[4] = {(uint8_t)((v) >> 24), (uint8_t)((v) >> 16), (uint8_t)((v) >> 8), (uint8_t)(v)}; fwrite(q_, 1, 4, f); } while (0)
    fwrite(hdr, 1, 8, f);
    {
        uint8_t ih[17] = {'I', 'H', 'D', 'R', 0, 0, 0, 240, 0, 0, 0, 240, 8, 2, 0, 0, 0};
        uint32_t c = 0xFFFFFFFFu;
        PUT32(13u);
        fwrite(ih, 1, 17, f);
        for (i = 0; i < 17; i++)
            c = crc_t[(c ^ ih[i]) & 255] ^ (c >> 8);
        PUT32(~c);
    }
    {
        uint32_t nblk = (len + 65534) / 65535, zlen = 2 + len + nblk * 5 + 4, c = 0xFFFFFFFFu;
        uint8_t *z = malloc(zlen + 4), *q = z;
        memcpy(q, "IDAT", 4);
        q += 4;
        *q++ = 0x78;
        *q++ = 0x01;
        for (pos = 0; pos < len;) {
            uint32_t m = len - pos > 65535 ? 65535 : len - pos;
            *q++ = pos + m >= len ? 1 : 0;
            *q++ = (uint8_t)m;
            *q++ = (uint8_t)(m >> 8);
            *q++ = (uint8_t)~m;
            *q++ = (uint8_t)(~m >> 8);
            memcpy(q, raw + pos, m);
            q += m;
            pos += m;
        }
        *q++ = (uint8_t)(b >> 8);
        *q++ = (uint8_t)b;
        *q++ = (uint8_t)(a >> 8);
        *q++ = (uint8_t)a;
        PUT32(zlen);
        fwrite(z, 1, zlen + 4, f);
        for (i = 0; i < zlen + 4; i++)
            c = crc_t[(c ^ z[i]) & 255] ^ (c >> 8);
        PUT32(~c);
        free(z);
    }
    {
        uint8_t ie[4] = {'I', 'E', 'N', 'D'};
        uint32_t c = 0xFFFFFFFFu;
        PUT32(0u);
        fwrite(ie, 1, 4, f);
        for (i = 0; i < 4; i++)
            c = crc_t[(c ^ ie[i]) & 255] ^ (c >> 8);
        PUT32(~c);
    }
    fclose(f);
#undef PUT32
}

/* ---- the RAM fake store -------------------------------------------------- */
typedef struct {
    song_t  s[SONGS_SLOTS];
    uint8_t used[SONGS_SLOTS];
    uint8_t deleted[SONGS_SLOTS];
    uint8_t take[SONGS_SLOTS];
    int     clips[SONGS_SLOTS];
    int     current;
} fake_t;

static int fake_clip_count(const song_t *s)
{
    int i, n = 0;
    for (i = 0; i < (int)NCLIPS; i++)
        if (s->clip[i].used)
            n++;
    return n;
}
static int fake_free(fake_t *f)
{
    int i;
    for (i = 0; i < (int)SONGS_SLOTS; i++)
        if (!f->used[i])
            return i;
    return -2;
}
static void fake_list(void *ctx, int slot, song_row_t *out)
{
    fake_t *f = (fake_t *)ctx;
    memset(out, 0, sizeof *out);
    if (f->used[slot] && !f->deleted[slot]) {
        out->used = 1;
        strncpy(out->name, f->s[slot].name, SONGS_NAME - 1);
        out->has_take = f->take[slot];
        out->current = (slot == f->current);
    }
}
static int fake_current(void *ctx) { return ((fake_t *)ctx)->current; }
static int fake_open(void *ctx, int slot)
{
    fake_t *f = (fake_t *)ctx;
    if (!f->used[slot])
        return -1;
    f->current = slot;
    return 0;
}
static int fake_create(void *ctx, const void *song)
{
    fake_t *f = (fake_t *)ctx;
    int slot = fake_free(f);
    if (slot < 0)
        return -2;
    f->s[slot] = *(const song_t *)song;
    f->used[slot] = 1;
    f->deleted[slot] = 0;
    f->clips[slot] = fake_clip_count(&f->s[slot]);
    return slot;
}
static int fake_copy(void *ctx, int src)
{
    fake_t *f = (fake_t *)ctx;
    int slot = fake_free(f);
    char n[SONGS_NAME];
    if (slot < 0)
        return -2;
    f->s[slot] = f->s[src];
    strncpy(n, f->s[src].name, SONGS_NAME - 3);
    n[SONGS_NAME - 3] = 0;
    strncat(n, " 2", SONGS_NAME - 1 - strlen(n));
    memcpy(f->s[slot].name, n, SONGS_NAME);
    f->used[slot] = 1;
    f->deleted[slot] = 0;
    f->take[slot] = f->take[src];
    f->clips[slot] = f->clips[src];
    return slot;
}
static int fake_rename(void *ctx, int slot, const char *name)
{
    fake_t *f = (fake_t *)ctx;
    if (!f->used[slot])
        return -1;
    memset(f->s[slot].name, 0, SONG_NAME_LEN);
    strncpy(f->s[slot].name, name, SONG_NAME_LEN - 1);
    return 0;
}
static int fake_del(void *ctx, int slot)
{
    fake_t *f = (fake_t *)ctx;
    int i;
    if (!f->used[slot])
        return -1;
    f->deleted[slot] = 1;
    if (f->current == slot) {                 /* the op opens another, or nothing is left */
        f->current = -1;
        for (i = 0; i < (int)SONGS_SLOTS; i++)
            if (f->used[i] && !f->deleted[i]) {
                f->current = i;
                break;
            }
    }
    return 0;
}
static int fake_undelete(void *ctx)
{
    fake_t *f = (fake_t *)ctx;
    int i;
    for (i = 0; i < (int)SONGS_SLOTS; i++)
        if (f->used[i] && f->deleted[i]) {
            f->deleted[i] = 0;
            if (f->current < 0)
                f->current = i;
            return 1;                          /* UNDO_DONE */
        }
    return 0;                                  /* UNDO_EMPTY */
}
static int fake_clips(void *ctx, int slot) { return ((fake_t *)ctx)->clips[slot]; }

static songs_ops_t FAKE_OPS = { 0, fake_list, fake_current, fake_open, fake_create,
                                fake_copy, fake_rename, fake_del, fake_undelete, fake_clips };

/* ---- small helpers ------------------------------------------------------- */
static int fails;
#define CHECK(c) do { if (!(c)) { printf("  FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static song_event_t sev(uint32_t btn, uint32_t keys, uint32_t ms)
{
    song_event_t e;
    memset(&e, 0, sizeof e);
    e.btn = btn;
    e.keys = keys;
    e.ms = ms;
    return e;
}
static song_event_t sev_enc(uint32_t btn, uint32_t keys, uint32_t ms, int role, int32_t v)
{
    song_event_t e = sev(btn, keys, ms);
    e.enc[role] = v;
    return e;
}
/* a tap of a button: down then up, ten ms apart */
static int tap_btn(songs_ui_t *u, int b, uint32_t *t)
{
    int a;
    song_event_t e = sev(1u << b, 0, *t);
    a = ui_songs_input(u, &e);            /* the press carries the action */
    *t += 10;
    e = sev(0, 0, *t);
    ui_songs_input(u, &e);
    *t += 10;
    return a;
}
static void turn(songs_ui_t *u, int role, int32_t v, uint32_t *t)
{
    song_event_t e = sev_enc(0, 0, *t, role, v);
    ui_songs_input(u, &e);
    *t += 10;
}
/* SELECT moves one row per detent: feed `n` single-detent frames */
static void turns(songs_ui_t *u, int n, uint32_t *t)
{
    int i, step = n < 0 ? -1 : 1;
    for (i = 0; i < (n < 0 ? -n : n); i++)
        turn(u, EN_SELECT, step, t);
}

static void seed_song(song_t *s, const char *name, int nclips, uint16_t bpm)
{
    int i;
    song_template_empty(s, bpm);
    memset(s->name, 0, SONG_NAME_LEN);
    strncpy(s->name, name, SONG_NAME_LEN - 1);
    for (i = 0; i < nclips && i < (int)NCLIPS; i++)
        s->clip[i].used = 1;
}
static void seed(fake_t *f, const char *const *names, int n)
{
    int i;
    memset(f, 0, sizeof *f);
    f->current = n ? 0 : -1;
    for (i = 0; i < n; i++) {
        seed_song(&f->s[i], names[i], 4 + i, 1250);
        f->used[i] = 1;
        f->clips[i] = 4 + i;
        f->take[i] = (uint8_t)(i == 1);
    }
}

static void render(songs_ui_t *u, const char *file, uint32_t ms)
{
    memset(fb, 0, sizeof fb);
    ui_songs_draw(u, ms);
    png_write(file);
    printf("  shot %s\n", file);
}

/* ================================================================ tests === */
static void test_list(void)
{
    static const char *names[] = {"NIGHT DRIVE", "SUNDAY DUB", "ACID TEST"};
    fake_t f;
    songs_ui_t u;
    uint32_t t = 0;
    seed(&f, names, 3);
    FAKE_OPS.ctx = &f;
    ui_songs_begin(&u, &FAKE_OPS);
    CHECK(ui_songs_active(&u));
    CHECK(u.screen == SO_LIST);
    CHECK(u.nused == 3);
    CHECK(u.row == 1);                        /* the cursor lands on the current song */
    CHECK(u.rows[0].current && u.rows[1].has_take);
    CHECK(strcmp(u.rows[0].name, "NIGHT DRIVE") == 0);
    render(&u, "build/host/ui_songs/list.png", 0);
    /* SELECT moves through the rows and stops at the ends */
    turns(&u, 3, &t);
    CHECK(u.row == 4);
    turns(&u, 9, &t);
    CHECK(u.row == 5);
    turns(&u, -9, &t);
    CHECK(u.row == 0);
    /* SEL on the current song opens it */
    u.row = 1;
    CHECK(tap_btn(&u, B_SEL, &t) == SONG_ACTION_OPENED);
    /* SEL on an empty slot says so and stays */
    u.rows[3].used = 0;
    u.row = 4;
    CHECK(tap_btn(&u, B_SEL, &t) == SONG_ACTION_NONE);
    CHECK(strcmp(ui_songs_toast(&u), "EMPTY SLOT") == 0);
    /* HOME closes */
    u.row = 0;
    CHECK(tap_btn(&u, B_HOME, &t) == SONG_ACTION_CLOSE);
}

static void test_list_empty(void)
{
    fake_t f;
    songs_ui_t u;
    seed(&f, 0, 0);
    FAKE_OPS.ctx = &f;
    ui_songs_begin(&u, &FAKE_OPS);
    CHECK(u.nused == 0);
    CHECK(u.row == 0);
    render(&u, "build/host/ui_songs/list_empty.png", 0);
}
static void test_list_full(void)
{
    static const char *names[] = {"ONE", "TWO", "THREE", "FOUR", "FIVE"};
    fake_t f;
    songs_ui_t u;
    seed(&f, names, 5);
    FAKE_OPS.ctx = &f;
    ui_songs_begin(&u, &FAKE_OPS);
    CHECK(u.nused == 5);
    render(&u, "build/host/ui_songs/list_full.png", 0);
}

static void test_new_and_create(void)
{
    fake_t f;
    songs_ui_t u;
    uint32_t t = 0;
    seed(&f, 0, 0);
    FAKE_OPS.ctx = &f;
    ui_songs_begin(&u, &FAKE_OPS);
    tap_btn(&u, B_SEL, &t);                   /* "+ New song" */
    CHECK(u.screen == SO_NEW);
    render(&u, "build/host/ui_songs/new_starter.png", 0);   /* Starter is the default */
    turns(&u, -1, &t);
    CHECK(u.row == 0);
    render(&u, "build/host/ui_songs/new_empty.png", 0);
    turns(&u, 2, &t);
    CHECK(u.row == 2);
    render(&u, "build/host/ui_songs/new_copy.png", 0);
    /* K1 tempo and K2 key */
    u.row = 1;
    turn(&u, EN_K1, 5, &t);
    CHECK(u.bpm_x10 == 1250 + 50);
    turn(&u, EN_K1, -50, &t);
    CHECK(u.bpm_x10 == 800);
    turn(&u, EN_K1, -200, &t);
    CHECK(u.bpm_x10 == SONG_TMPL_BPM_MIN);
    turn(&u, EN_K2, 1, &t);
    CHECK(u.key == SONG_TMPL_DEFAULT_KEY + 1);
    turn(&u, EN_K2, -1, &t);
    CHECK(u.key == SONG_TMPL_DEFAULT_KEY);
    /* SEL creates the Starter in the first free slot and opens it */
    CHECK(tap_btn(&u, B_SEL, &t) == SONG_ACTION_OPENED);
    CHECK(f.used[0] && f.current == 0);
    CHECK(strcmp(f.s[0].name, "STARTER") == 0);
    CHECK(f.clips[0] == 13);
    /* and an Empty */
    ui_songs_begin(&u, &FAKE_OPS);
    u.row = 0;
    tap_btn(&u, B_EDIT, &t);                  /* EDIT on + New song opens NEW */
    CHECK(u.screen == SO_NEW);
    u.row = 0;
    CHECK(tap_btn(&u, B_SEL, &t) == SONG_ACTION_OPENED);
    CHECK(f.used[1] && strcmp(f.s[1].name, "NEW SONG") == 0);
    CHECK(f.clips[1] == 0);
}

static void test_five_max(void)
{
    static const char *names[] = {"ONE", "TWO", "THREE", "FOUR", "FIVE"};
    fake_t f;
    songs_ui_t u;
    uint32_t t = 0;
    seed(&f, names, 5);
    FAKE_OPS.ctx = &f;
    ui_songs_begin(&u, &FAKE_OPS);
    u.row = 0;
    tap_btn(&u, B_SEL, &t);                   /* + New song */
    CHECK(u.screen == SO_NEW);
    u.row = 1;
    CHECK(tap_btn(&u, B_SEL, &t) == SONG_ACTION_NONE);
    CHECK(strcmp(ui_songs_toast(&u), "5 SONGS MAX: DELETE ONE") == 0);
    CHECK(u.screen == SO_NEW);
    CHECK(f.used[4]);
    render(&u, "build/host/ui_songs/five_max.png", 200);
}

static void test_menu_open_copy(void)
{
    static const char *names[] = {"NIGHT DRIVE", "SUNDAY DUB"};
    fake_t f;
    songs_ui_t u;
    uint32_t t = 0;
    seed(&f, names, 2);
    FAKE_OPS.ctx = &f;
    ui_songs_begin(&u, &FAKE_OPS);
    u.row = 2;
    tap_btn(&u, B_EDIT, &t);                  /* the menu on SUNDAY DUB */
    CHECK(u.screen == SO_MENU && u.slot == 1);
    render(&u, "build/host/ui_songs/menu.png", 0);
    turns(&u, 1, &t);               /* Copy */
    CHECK(u.row == 1);
    CHECK(tap_btn(&u, B_SEL, &t) == SONG_ACTION_NONE);
    CHECK(u.screen == SO_LIST);
    CHECK(f.used[2] && strcmp(f.s[2].name, "SUNDAY DU 2") == 0);
    /* Select cursor sits on the copy */
    CHECK(u.row == 3);
    /* Open from the menu */
    u.row = 1;
    tap_btn(&u, B_EDIT, &t);
    CHECK(u.screen == SO_MENU && u.slot == 0);
    CHECK(tap_btn(&u, B_SEL, &t) == SONG_ACTION_OPENED);   /* Open */
    CHECK(f.current == 0);
}

static void test_rename(void)
{
    static const char *names[] = {"NIGHT DRIVE"};
    fake_t f;
    songs_ui_t u;
    uint32_t t = 0;
    seed(&f, names, 1);
    FAKE_OPS.ctx = &f;
    ui_songs_begin(&u, &FAKE_OPS);
    u.row = 1;
    tap_btn(&u, B_EDIT, &t);
    CHECK(u.screen == SO_MENU);
    turns(&u, 2, &t);               /* Rename */
    CHECK(u.row == 2);
    tap_btn(&u, B_SEL, &t);
    CHECK(u.screen == SO_RENAME);
    render(&u, "build/host/ui_songs/rename.png", 0);
    /* move the cursor to the last character and step the letter: E -> F */
    turns(&u, -1, &t);
    turn(&u, EN_ALGO, 1, &t);
    CHECK(tap_btn(&u, B_SEL, &t) == SONG_ACTION_NONE);
    CHECK(u.screen == SO_MENU);
    CHECK(strcmp(f.s[0].name, "NIGHT DRIVF") == 0);
    /* a rename that is cancelled keeps the old name */
    u.row = 2;                      /* Rename */
    tap_btn(&u, B_SEL, &t);
    CHECK(u.screen == SO_RENAME);
    turns(&u, -1, &t);
    turn(&u, EN_ALGO, 1, &t);
    tap_btn(&u, B_HOME, &t);                  /* HOME tap cancels */
    CHECK(u.screen == SO_MENU);
    CHECK(strcmp(f.s[0].name, "NIGHT DRIVF") == 0);
}

static void test_delete_undo(void)
{
    static const char *names[] = {"NIGHT DRIVE", "SUNDAY DUB"};
    fake_t f;
    songs_ui_t u;
    uint32_t t = 0;
    seed(&f, names, 2);
    FAKE_OPS.ctx = &f;
    ui_songs_begin(&u, &FAKE_OPS);
    u.row = 2;
    tap_btn(&u, B_EDIT, &t);                  /* the menu on SUNDAY DUB */
    turns(&u, 3, &t);               /* Delete */
    tap_btn(&u, B_SEL, &t);                   /* choose Delete, then release */
    CHECK(u.screen == SO_DELETE);
    /* arm the hold, then render the ring at ~50% without letting it fire */
    {
        song_event_t e = sev(1u << B_SEL, 0, 1000);
        ui_songs_input(&u, &e);
        CHECK(u.del_held);
    }
    render(&u, "build/host/ui_songs/delete_ring.png", 1750);   /* 750/1500 = 50% */
    /* hold past 1.5 s: it fires */
    {
        song_event_t e = sev(1u << B_SEL, 0, 2600);
        ui_songs_input(&u, &e);
    }
    CHECK(u.del_done);
    CHECK(u.screen == SO_LIST);
    CHECK(f.deleted[1]);
    CHECK(f.current == 0);                    /* another song opened */
    CHECK(u.nused == 1);
    /* release, then HOME+REC undoes it */
    {
        song_event_t e = sev(0, 0, 2610);
        ui_songs_input(&u, &e);
        e = sev((1u << B_HOME) | (1u << B_REC), 0, 2620);   /* HOME held + REC press */
        ui_songs_input(&u, &e);
    }
    CHECK(!f.deleted[1]);
    CHECK(u.nused == 2);
    CHECK(strcmp(ui_songs_toast(&u), "RESTORED") == 0);
    /* releasing the SEL early cancels */
    u.row = 2;
    tap_btn(&u, B_EDIT, &t);
    turns(&u, 3, &t);
    tap_btn(&u, B_SEL, &t);
    CHECK(u.screen == SO_DELETE);
    {
        song_event_t e = sev(1u << B_SEL, 0, 3000);
        ui_songs_input(&u, &e);
        e = sev(0, 0, 3200);                  /* released before 1.5 s */
        ui_songs_input(&u, &e);
    }
    CHECK(!u.del_done);
    CHECK(u.screen == SO_MENU);
}

static void test_deterministic_render(void)
{
    /* the same state renders the same bytes twice (no wall clock, no random) */
    static const char *names[] = {"NIGHT DRIVE", "SUNDAY DUB"};
    fake_t f;
    songs_ui_t u;
    seed(&f, names, 2);
    FAKE_OPS.ctx = &f;
    ui_songs_begin(&u, &FAKE_OPS);
    memset(fb, 0, sizeof fb);
    ui_songs_draw(&u, 0);
    {
        uint16_t a[240 * 240];
        memcpy(a, fb, sizeof fb);
        memset(fb, 0, sizeof fb);
        ui_songs_draw(&u, 0);
        CHECK(memcmp(a, fb, sizeof fb) == 0);
    }
}

int main(void)
{
    printf("ui_songs: state machine\n");
    test_list();
    test_list_empty();
    test_list_full();
    test_new_and_create();
    test_five_max();
    test_menu_open_copy();
    test_rename();
    test_delete_undo();
    test_deterministic_render();
    if (fails) {
        printf("ui_songs: %d FAILURES\n", fails);
        return 1;
    }
    printf("ui_songs: state machine ok\n");
    printf("ui_songs: renders\n");
    {
        static const char *names[] = {"NIGHT DRIVE", "SUNDAY DUB", "ACID TEST", "WARMUP LOOPS", "UNTITLED"};
        fake_t f;
        songs_ui_t u;
        seed(&f, names, 5);
        FAKE_OPS.ctx = &f;
        ui_songs_begin(&u, &FAKE_OPS);
        render(&u, "build/host/ui_songs/list.png", 0);
        u.row = 0;
        render(&u, "build/host/ui_songs/list_new_cursor.png", 0);
    }
    printf("ui_songs: all passed\n");
    return 0;
}
