/* SPDX-License-Identifier: GPL-3.0-only */
/* T8c: the NAME state machine and a render of the screen to PNG.
 *
 * The state machine runs against hand-built X0X input frames (btn / keys / enc / ms),
 * with no gfx involved. Then the screen is drawn through the real gfx.c (the same T2b
 * fonts and tokens the firmware uses) into a framebuffer and written as PNG so the
 * Opus reviewer can look at it at 1:1. Run by `make unit-name`, also under ASan/UBSan.
 *
 * ui_name.c is an include-style view (it draws with gfx.c's statics), so gfx.c and
 * ui_name.c are compiled into this one translation unit, as X0X's x0x.c does. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define __attribute__(x)                     /* as host/x0x_host.c does: the .pool section is a device thing */

/* ---- the framebuffer gfx.c blits into (as host/x0x_host.c) ---- */
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
#include "../../core/model/model.h"
#undef NKEYS                                 /* plat.h (via ui_name.h) defines it too, as a bare 27 */
#include "../../firmware/src/app/ui_name.c"

_Static_assert(NAME_MAX == SONG_NAME_LEN - 1u, "NAME_MAX must track SONG_NAME_LEN");

/* ---- a PNG writer (stored deflate blocks, no zlib: from host/x0x_host.c) ---- */
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

/* ---- the tests ---- */
static int fails;
#define CHECK(cond) do { if (!(cond)) { printf("  FAIL line %d: %s\n", __LINE__, #cond); fails++; } } while (0)
#define OK(cond) do { if (!(cond)) { printf("  FAIL line %d: %s\n", __LINE__, #cond); fails++; } } while (0)

static name_event_t nev(uint32_t btn, uint32_t keys, uint32_t ms)
{
    name_event_t e;
    memset(&e, 0, sizeof e);
    e.btn = btn;
    e.keys = keys;
    e.ms = ms;
    return e;
}
static name_event_t nevx(uint32_t btn, uint32_t keys, uint32_t ms, int role, int32_t v)
{
    name_event_t e = nev(btn, keys, ms);
    e.enc[role] = v;
    return e;
}
/* one tap of key k: press then release, ten milliseconds apart */
static void tap(name_edit_t *n, int k, uint32_t *t)
{
    name_event_t e = nev(0, 1u << k, *t);
    name_edit_input(n, &e);
    *t += 10;
    e = nev(0, 0, *t);
    name_edit_input(n, &e);
    *t += 10;
}
static int all_allowed(const char *s)
{
    for (; *s; s++)
        if (!name_char_allowed(*s))
            return 0;
    return 1;
}

static void test_begin_result(void)
{
    name_edit_t n;
    char out[NAME_BUF];
    name_edit_begin(&n, "SUNDAY");
    CHECK(n.len == 6);
    CHECK(n.cur == 6);
    CHECK(strcmp(n.s, "SUNDAY") == 0);
    CHECK(name_edit_result(&n, out) == 6);
    CHECK(strcmp(out, "SUNDAY") == 0);

    name_edit_begin(&n, "  AB  ");          /* spaces are in the set; the result trims the ends */
    CHECK(n.len == 6);
    CHECK(name_edit_result(&n, out) == 2);
    CHECK(strcmp(out, "AB") == 0);

    name_edit_begin(&n, "A/B#C+");          /* disallowed characters never enter the model */
    CHECK(strcmp(n.s, "ABC") == 0);
    CHECK(n.len == 3 && n.cur == 3);
}

static void test_typing(void)
{
    name_edit_t n;
    uint32_t t = 0;
    name_edit_begin(&n, "");
    tap(&n, 0, &t);                          /* F3: "AB" -> A */
    CHECK(n.len == 1 && strcmp(n.s, "A") == 0);
    tap(&n, 0, &t);                          /* same key inside the window: B */
    CHECK(n.len == 1 && strcmp(n.s, "B") == 0);
    tap(&n, 7, &t);                          /* C4: "IJK" -> I (the cycled B is kept, cursor moves on) */
    CHECK(n.len == 2 && strcmp(n.s, "BI") == 0);
    tap(&n, 7, &t);
    CHECK(strcmp(n.s, "BJ") == 0);
    tap(&n, 7, &t);
    CHECK(strcmp(n.s, "BK") == 0);
    tap(&n, 7, &t);                          /* cycles back to I */
    CHECK(strcmp(n.s, "BI") == 0);

    name_edit_begin(&n, "");                 /* the timeout keeps the letter and starts a new group */
    t = 0;
    tap(&n, 0, &t);
    { name_event_t e = nev(0, 0, t + NAME_TAP_MS + 20u); name_edit_input(&n, &e); }
    t += NAME_TAP_MS + 20u;
    tap(&n, 0, &t);
    CHECK(n.len == 2 && strcmp(n.s, "AA") == 0);

    name_edit_begin(&n, "");                 /* space and the black-key edits */
    t = 0;
    tap(&n, 6, &t);                          /* B3: "GH" -> G */
    tap(&n, 3, &t);                          /* G#3: SPACE */
    tap(&n, 14, &t);                         /* G4: "RS" -> R */
    CHECK(strcmp(n.s, "G R") == 0);
    tap(&n, 8, &t);                          /* C#4: DEL the character before the cursor */
    CHECK(strcmp(n.s, "G ") == 0);
    tap(&n, 10, &t);                         /* D#4: ABC -> 123 */
    CHECK(n.mode == 1);
    tap(&n, 0, &t);                          /* one tap, one character: '1' */
    CHECK(n.s[2] == '1' && n.len == 3);
}

static void test_cursor_and_char(void)
{
    name_edit_t n;
    char out[NAME_BUF];
    name_edit_begin(&n, "ABCD");
    { name_event_t e = nevx(0, 0, 0, EN_SELECT, -1); name_edit_input(&n, &e); }
    CHECK(n.cur == 3);
    { name_event_t e = nevx(0, 0, 0, EN_SELECT, -9); name_edit_input(&n, &e); }
    CHECK(n.cur == 0);
    { name_event_t e = nevx(0, 0, 0, EN_SELECT, 9); name_edit_input(&n, &e); }
    CHECK(n.cur == 4);
    { name_event_t e = nevx(0, 0, 0, EN_SELECT, -1); name_edit_input(&n, &e); }
    CHECK(n.cur == 3);
    { name_event_t e = nevx(0, 0, 0, EN_ALGO, 1); name_edit_input(&n, &e); }
    CHECK(strcmp(n.s, "ABCE") == 0);         /* 'D' -> the next in NM_SET, 'E' */

    name_edit_begin(&n, "A");
    { name_event_t e = nevx(0, 0, 0, EN_SELECT, -1); name_edit_input(&n, &e); }
    { name_event_t e = nevx(0, 0, 0, EN_ALGO, -1); name_edit_input(&n, &e); }
    CHECK(n.s[0] == ' ');                    /* 'A' steps back to SPACE */
    { name_event_t e = nevx(0, 0, 0, EN_ALGO, -1); name_edit_input(&n, &e); }
    CHECK(n.s[0] == '!');                    /* SPACE wraps to the set's last character */
    { name_event_t e = nevx(0, 0, 0, EN_ALGO, 1); name_edit_input(&n, &e); }
    CHECK(n.s[0] == ' ');

    name_edit_begin(&n, "AB");               /* every step of ALGORITHM stays in the allowed set */
    { name_event_t e = nevx(0, 0, 0, EN_SELECT, -2); name_edit_input(&n, &e); }
    for (int i = 0; i < 90; i++) {
        name_event_t e = nevx(0, 0, 0, EN_ALGO, 1);
        name_edit_input(&n, &e);
        CHECK(name_char_allowed(n.s[0]));
    }
    (void)out;
}

static void test_confirm_cancel_clear(void)
{
    name_edit_t n;
    char out[NAME_BUF];
    uint32_t t = 0;

    name_edit_begin(&n, "SONG");             /* SEL confirms */
    { name_event_t e = nev(1u << B_SEL, 0, 0); CHECK(name_edit_input(&n, &e) == NAME_DONE); }
    CHECK(name_edit_result(&n, out) == 4 && strcmp(out, "SONG") == 0);

    name_edit_begin(&n, "SONG");             /* HOME taps back, restoring the original */
    tap(&n, 0, &t);
    CHECK(n.len == 5);
    { name_event_t e = nev(1u << B_HOME, 0, 1000); CHECK(name_edit_input(&n, &e) == NAME_EDITING); }
    { name_event_t e = nev(0, 0, 1010); CHECK(name_edit_input(&n, &e) == NAME_CANCELLED); }
    CHECK(strcmp(n.s, "SONG") == 0 && n.len == 4);

    name_edit_begin(&n, "LONGNAME");         /* a long HOME hold clears it, and the release does not cancel */
    { name_event_t e = nev(1u << B_HOME, 0, 0); CHECK(name_edit_input(&n, &e) == NAME_EDITING); }
    { name_event_t e = nev(1u << B_HOME, 0, NAME_CLEAR_MS - 1u); CHECK(name_edit_input(&n, &e) == NAME_EDITING); }
    CHECK(n.len == 8);                       /* not yet */
    { name_event_t e = nev(1u << B_HOME, 0, NAME_CLEAR_MS); CHECK(name_edit_input(&n, &e) == NAME_EDITING); }
    CHECK(n.len == 0 && n.s[0] == 0);
    { name_event_t e = nev(0, 0, NAME_CLEAR_MS + 20u); CHECK(name_edit_input(&n, &e) == NAME_EDITING); }

    name_edit_begin(&n, "ABCDEFGHIJK");      /* the maximum length; one more is refused */
    CHECK(n.len == 11);
    t = 0;
    tap(&n, 0, &t);
    CHECK(n.len == 11);
}

static void test_random(void)
{
    name_edit_t n;
    int i;
    name_edit_begin(&n, "X");
    { name_event_t e = nevx(0, 0, 0, EN_K3, 1); name_edit_input(&n, &e); }
    CHECK(n.len > 0 && n.len <= NAME_MAX);
    CHECK(strcmp(n.s, "X") != 0);
    CHECK(all_allowed(n.s));
    CHECK(strstr(n.s, " ") != 0);            /* an adjective, a space, a noun */
    for (i = 0; i < 20; i++) {
        name_edit_t m;
        name_event_t e = nevx(0, 0, 0, EN_K3, 1);
        name_edit_begin(&m, "");
        name_edit_input(&m, &e);
        CHECK(m.len > 0 && m.len <= NAME_MAX && all_allowed(m.s));
    }
}

/* TFIXU 13: the maximum-length NAME must fit on screen. */
static void test_wide_field(void)
{
    name_edit_t n;
    char w[NAME_BUF];
    int i;
    for (i = 0; i < (int)NAME_MAX; i++)
        w[i] = 'W';
    w[NAME_MAX] = 0;
    name_edit_begin(&n, w);
    CHECK(n.len == NAME_MAX);
    CHECK(text_w(&AF_L, n.s) > NAME_FIELD_W);       /* the old face ran off the card */
    CHECK(name_edit_width(&n) <= NAME_FIELD_W);     /* the chosen face keeps the whole name in frame */
    name_edit_begin(&n, "SHORT");
    CHECK(name_edit_width(&n) <= NAME_FIELD_W);     /* ordinary names are unaffected */
}

static void render(const name_edit_t *n, const char *file, uint32_t ms)
{
    memset(fb, 0, sizeof fb);
    name_edit_draw(n, ms);
    png_write(file);
    printf("  shot %s\n", file);
}

int main(void)
{
    name_edit_t n;
    printf("ui_name: state machine\n");
    test_begin_result();
    test_typing();
    test_cursor_and_char();
    test_confirm_cancel_clear();
    test_random();
    test_wide_field();
    if (fails) {
        printf("ui_name: %d FAILURES\n", fails);
        return 1;
    }
    printf("ui_name: state machine ok\n");
    printf("ui_name: renders\n");
    name_edit_begin(&n, "SUNDAY DUB");
    { name_event_t e = nevx(0, 0, 0, EN_SELECT, -1); name_edit_input(&n, &e); }
    render(&n, "build/host/ui_name/editing_mid.png", 0);
    name_edit_begin(&n, "");
    render(&n, "build/host/ui_name/empty.png", 0);
    name_edit_begin(&n, "909 4 THE FLOOR");
    render(&n, "build/host/ui_name/full.png", 200);
    name_edit_begin(&n, "RANDOM");
    { name_event_t e = nevx(0, 0, 0, EN_K3, 1); name_edit_input(&n, &e); }
    render(&n, "build/host/ui_name/random.png", 0);
    name_edit_begin(&n, "WWWWWWWWWWW");              /* TFIXU 13: the widest valid name */
    render(&n, "build/host/ui_name/wide.png", 0);
    printf("ui_name: all passed\n");
    return 0;
}
