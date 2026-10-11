/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Ported from Felucca firmware/src/ui_name.c (GPL-3.0-only).
 * fm1-rocket-os: the NAME screen's state machine and drawing (T8c).
 *
 * Felucca's name entry, adapted to X0X's input events and gfx:
 *   - the model is a name of up to NAME_MAX characters plus a cursor; allowed characters
 *     are A-Z 0-9 space - _ . ! (Felucca allowed / # + too; the song name does not);
 *   - the white keys type (phone-style multi-tap: a tap types the group's first character,
 *     another tap of the same key within NAME_TAP_MS the next, cycling; another key or the
 *     timeout keeps it); the black keys by name are F# cursor left, G# SPACE, A# cursor
 *     right, C# DELETE (held: repeats), D# ABC / 123;
 *   - ALGORITHM is Felucca's KNOB 2 (the character at the cursor), SELECT turns its KNOB 1
 *     (the cursor), K3 its PROJECT random name;
 *   - SEL confirms (Felucca's OCT+), HOME goes back (Felucca's OCT-); a long HOME hold
 *     clears the name. OCT+ / OCT- are kept as aliases so the port stays a port;
 *   - drawing follows design/project/Studio-Rename.dc.html at 1:1, in the T2b Inter Tight
 *     faces (AF_S / AF_M / AF_L) and the GREY tokens (ui_tokens.h).
 *
 * This file is #included into the translation unit that holds gfx.c (X0X's ui.c), so the
 * cv_* drawing calls and the fonts are the ones gfx.c defines. It defines nothing global
 * but the name_edit_* API. */
#include "ui_name.h"

/* ---- the character set and the key tables (Fel/ X0X: F3 .. G5) -------------------- */
/* the black keys, by name: F# cursor left, G# SPACE, A# cursor right, C# DELETE, D# ABC/123 */
enum { NB_LEFT, NB_SPACE, NB_RIGHT, NB_DEL, NB_MODE, NB_NONE };
static const char NM_SET[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._!";
/* white key place p in ABC mode: Felucca's groups */
static const char *const NM_ABC[16] = {"AB", "CD", "EF", "GH", "IJK", "LM", "NO", "PQ", "RS", "TU", "VW", "XYZ",
                                       "123", "456", "789", "0-."};
/* white key place p in 123 mode: one character each (allowed set only, 16 slots) */
static const char NM_NUM[16] = {'1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '.', '_', '!', '-', '.'};
/* key k's white place, -1 = black. F3 G3 A3 B3 C4 D4 E4 F4 G4 A4 B4 C5 D5 E5 F5 G5 */
static const int8_t NM_PLACE[NKEYS] = {0, -1, 1, -1, 2, -1, 3, 4, -1, 5, -1, 6, 7, -1,
                                       8, -1, 9, -1, 10, 11, -1, 12, -1, 13, 14, -1, 15};

/* tiny string helpers (no libc string calls on the device path) */
static uint32_t nm_len(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }
static int nm_eq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void nm_cpy(char *dst, const char *src)             /* truncating copy into a NAME_BUF */
{
    uint32_t i = 0;
    while (i < NAME_MAX && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}
static void nm_app(char *dst, const char *add)             /* append if it fits in a NAME_BUF */
{
    uint32_t n = nm_len(dst), i = 0;
    while (n + i < NAME_MAX && add[i]) { dst[n + i] = add[i]; i++; }
    dst[n + i] = 0;
}
static void nm_cat(char *dst, uint32_t cap, const char *add)   /* append under an explicit cap */
{
    uint32_t n = nm_len(dst), i = 0;
    while (n + i + 1u < cap && add[i]) { dst[n + i] = add[i]; i++; }
    dst[n + i] = 0;
}

int name_char_allowed(char c)
{
    return c == ' ' || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
           c == '!';
}

/* ---- the state machine (Fel/ name_* ) --------------------------------------------- */
void name_edit_begin(name_edit_t *e, const char *initial)
{
    uint32_t i;
    e->s[0] = e->orig[0] = 0;
    e->len = e->cur = 0;
    e->mode = e->key = e->tap = e->rep = 0;
    e->t = e->rep_t = e->home_t = 0;
    e->home_held = e->home_cleared = 0;
    e->rnd = 0;
    e->prev_btn = e->prev_keys = 0;
    if (initial)
        for (i = 0; initial[i] && e->len < NAME_MAX; i++)
            if (name_char_allowed(initial[i]))
                e->s[e->len++] = initial[i];
    e->s[e->len] = 0;
    e->cur = e->len;
    nm_cpy(e->orig, e->s);
}

static void nm_commit(name_edit_t *e)                        /* the cycled letter is kept: cursor past it */
{
    if (e->key) {
        e->key = 0;
        if (e->cur < e->len)
            e->cur++;
    }
}
static int nm_insert(name_edit_t *e, char c)                 /* at the cursor (it stays on it); 0 = full */
{
    uint32_t i;
    if (!name_char_allowed(c))
        return 0;
    if (e->len >= NAME_MAX)
        return 0;
    for (i = e->len; i > e->cur; i--)
        e->s[i] = e->s[i - 1u];
    e->s[e->cur] = c;
    e->s[++e->len] = 0;
    return 1;
}
static const char *nm_group(const name_edit_t *e, uint32_t p)
{
    static char one[2];
    if (!e->mode)
        return NM_ABC[p & 15u];
    one[0] = NM_NUM[p & 15u];
    return one;
}
static uint32_t nm_black(uint32_t k)                         /* black key k's function, by its name */
{
    switch ((k + 5u) % 12u) {
    case 6: return NB_LEFT;
    case 8: return NB_SPACE;
    case 10: return NB_RIGHT;
    case 1: return NB_DEL;
    case 3: return NB_MODE;
    default: return NB_NONE;
    }
}
static void nm_white(name_edit_t *e, uint32_t p, uint32_t ms)
{
    const char *g = nm_group(e, p);
    uint32_t n = nm_len(g);
    if (e->key == p + 1u && (uint32_t)(ms - e->t) < NAME_TAP_MS) {   /* the same key again: its next letter */
        e->tap = (uint8_t)((e->tap + 1u) % n);
        e->s[e->cur] = g[e->tap];
        e->t = ms;
        return;
    }
    nm_commit(e);
    if (!nm_insert(e, g[0]))
        return;
    if (n > 1u) {
        e->key = (uint8_t)(p + 1u);
        e->tap = 0;
        e->t = ms;
    } else {
        e->cur++;
    }
}
static void nm_do(name_edit_t *e, uint32_t f)                /* a black key's function */
{
    uint32_t i;
    nm_commit(e);
    switch (f) {
    case NB_LEFT:
        if (e->cur)
            e->cur--;
        break;
    case NB_RIGHT:
        if (e->cur < e->len)
            e->cur++;
        break;
    case NB_SPACE:
        if (nm_insert(e, ' '))
            e->cur++;
        break;
    case NB_DEL:
        if (!e->cur)
            break;
        for (i = --e->cur; i < e->len; i++)
            e->s[i] = e->s[i + 1u];
        e->len--;
        break;
    case NB_MODE:
        e->mode ^= 1u;                                       /* ABC <-> 123 */
        break;
    default:
        break;
    }
}
static void nm_move(name_edit_t *e, int32_t s)               /* SELECT: the cursor */
{
    int32_t c;
    nm_commit(e);
    c = (int32_t)e->cur + s;
    if (c < 0)
        c = 0;
    if (c > (int32_t)e->len)
        c = (int32_t)e->len;
    e->cur = (uint8_t)c;
}
static void nm_char(name_edit_t *e, int32_t s)               /* ALGORITHM: the character at the cursor */
{
    int32_t i, n = (int32_t)sizeof NM_SET - 1;
    nm_commit(e);
    if (e->cur == e->len && !nm_insert(e, ' '))              /* at the end: a new character (from SPACE) */
        return;
    for (i = 0; i < n && NM_SET[i] != e->s[e->cur]; i++)
        ;
    i = i < n ? i : 0;
    e->s[e->cur] = NM_SET[((i + s) % n + n) % n];
}
/* K3: a random name, an adjective and a noun (Fel/ NM_RA / NM_RB), never the one in the field */
static const char *const NM_RA[16] = {"NEON", "GLASS", "AMBER", "SOLAR", "LUNAR", "SILK", "IRON", "PALE",
                                      "DEEP", "WILD", "QUIET", "SLOW", "GOLD", "BLUE", "DUSTY", "HAZY"};
static const char *const NM_RB[16] = {"DRIFT", "PULSE", "ORBIT", "RIVER", "HARBOR", "TIDE", "ECHO", "GARDEN",
                                      "SIGNAL", "MIRROR", "CANAL", "FIELD", "COAST", "CLOUD", "BLOOM", "SAIL"};
static void nm_random(name_edit_t *e)
{
    char b[NAME_BUF];
    uint32_t x, tries = 0;
    do {
        x = e->rnd ? e->rnd : 2463534242u;                   /* a fixed seed: host runs stay bit-identical */
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        e->rnd = x;
        nm_cpy(b, NM_RA[x & 15u]);
        nm_app(b, " ");
        nm_app(b, NM_RB[(x >> 4) & 15u]);
    } while (nm_eq(b, e->s) && ++tries < 64u);
    nm_cpy(e->s, b);
    e->len = e->cur = (uint8_t)nm_len(e->s);
    e->key = e->tap = e->rep = 0;
}
static void nm_clear(name_edit_t *e)                         /* HOME held: the name goes */
{
    e->s[0] = 0;
    e->len = e->cur = 0;
    e->key = e->tap = e->rep = 0;
}
static void nm_restore(name_edit_t *e)                       /* HOME tapped: the original comes back */
{
    nm_cpy(e->s, e->orig);
    e->len = e->cur = (uint8_t)nm_len(e->s);
    e->key = e->tap = e->rep = 0;
}

int name_edit_input(name_edit_t *e, const name_event_t *event)
{
    uint32_t pb = e->prev_btn, pk = e->prev_keys;
    uint32_t down = event->btn & ~pb, up = pb & ~event->btn;
    uint32_t kdown = event->keys & ~pk;
    uint32_t k;
    int32_t s;

    e->prev_btn = event->btn;
    e->prev_keys = event->keys;

    if (down & (1u << B_HOME)) {                             /* HOME down: hold clears, tap cancels */
        e->home_held = 1;
        e->home_t = event->ms;
        e->home_cleared = 0;
    }
    if (e->key && (uint32_t)(event->ms - e->t) >= NAME_TAP_MS)   /* no tap in time: the letter is kept */
        nm_commit(e);

    for (k = 0; k < NKEYS; k++) {
        uint32_t f;
        if (!((kdown >> k) & 1u))
            continue;
        if (NM_PLACE[k] >= 0) {
            nm_white(e, (uint32_t)NM_PLACE[k], event->ms);
            continue;
        }
        f = nm_black(k);
        nm_do(e, f);
        if (f == NB_LEFT || f == NB_RIGHT || f == NB_DEL) {  /* held: repeats */
            e->rep = (uint8_t)(k + 1u);
            e->rep_t = event->ms + NAME_REP_MS;
        }
    }
    if (e->rep && !((event->keys >> (e->rep - 1u)) & 1u))
        e->rep = 0;
    else if (e->rep && (int32_t)(event->ms - e->rep_t) >= 0) {
        nm_do(e, nm_black(e->rep - 1u));
        e->rep_t = event->ms + NAME_RATE_MS;
    }

    if ((s = event->enc[EN_SELECT]) != 0)
        nm_move(e, s);
    if ((s = event->enc[EN_ALGO]) != 0)
        nm_char(e, s);
    if (event->enc[EN_K3] != 0)
        nm_random(e);

    if (e->home_held && !e->home_cleared && (uint32_t)(event->ms - e->home_t) >= NAME_CLEAR_MS) {
        nm_clear(e);
        e->home_cleared = 1;
    }
    if (up & (1u << B_HOME)) {
        e->home_held = 0;
        if (!e->home_cleared) {
            nm_restore(e);
            return NAME_CANCELLED;
        }
        e->home_cleared = 0;
    }
    if (down & (1u << B_SEL)) {                              /* SEL confirms */
        nm_commit(e);
        return NAME_DONE;
    }
    if (down & (1u << B_OCTUP)) {                            /* Felucca's OCT+: save */
        nm_commit(e);
        return NAME_DONE;
    }
    if (down & (1u << B_OCTDN)) {                            /* Felucca's OCT-: cancel */
        nm_restore(e);
        return NAME_CANCELLED;
    }
    return NAME_EDITING;
}

uint32_t name_edit_result(const name_edit_t *e, char *out)   /* spaces at either end are dropped */
{
    uint32_t a = 0, z = e->len, i;
    while (z && e->s[z - 1u] == ' ')
        z--;
    while (a < z && e->s[a] == ' ')
        a++;
    for (i = 0; a + i < z; i++)
        out[i] = e->s[a + i];
    out[i] = 0;
    return i;
}

/* ---- drawing (design/project/Studio-Rename.dc.html at 1:1, on gfx.c's canvas) ----- */
static int nm_isqrt(int v)
{
    int s = 0;
    while ((s + 1) * (s + 1) <= v)
        s++;
    return s;
}
static void nm_rrect(int x, int y, int w, int h, int r, uint16_t f)
{
    int i;
    if (r <= 0) {
        cv_rect(x, y, w, h, f);
        return;
    }
    if (2 * r > h)
        r = h / 2;
    if (2 * r > w)
        r = w / 2;
    for (i = 0; i < h; i++) {
        int dy = i < r ? r - i : i >= h - r ? i - (h - r) + 1 : 0;
        int inset = dy ? r - nm_isqrt(r * r - dy * dy) : 0;
        cv_rect(x + inset, y + i, w - 2 * inset, 1, f);
    }
}
static void nm_utoa(int v, char *b)                          /* 0 .. 99 */
{
    int i = 0;
    if (v >= 10)
        b[i++] = (char)('0' + v / 10);
    b[i++] = (char)('0' + v % 10);
    b[i] = 0;
}
static int nm_chip(int x, int y, const char *t)              /* a keycap chip, as in fm1.css */
{
    int w = text_w(&AF_S, t) + 8;
    nm_rrect(x, y, w, 16, 4, TOK_KEY);
    cv_text(x + 4, y, &AF_S, t, TOK_INK);
    return w;
}
static void nm_battery(int x, int y)
{
    cv_rect(x, y, 16, 1, TOK_MID);
    cv_rect(x, y + 9, 16, 1, TOK_MID);
    cv_rect(x, y, 1, 10, TOK_MID);
    cv_rect(x + 15, y, 1, 10, TOK_MID);
    cv_rect(x + 16, y + 3, 2, 4, TOK_MID);
    cv_rect(x + 2, y + 2, 10, 6, TOK_THEME);
}
static void nm_draw_head(void)
{
    int x = 5;
    x += nm_chip(x, 4, "STUDIO");
    cv_text(x + 4, 5, &AF_S, "NAME", TOK_MID);
    nm_battery(240 - 5 - 18, 7);
}
static void nm_draw_card(const name_edit_t *e, uint32_t ms)
{
    char b[24], one[2], left[NAME_BUF], right[NAME_BUF], n[4];
    int x = 14, y = 60, curx, cw = 0, i, on = ((ms / NAME_BLINK_MS) & 1u) == 0u;

    nm_rrect(4, 30, 232, 76, 6, TOK_SURF);
    b[0] = 0;
    nm_cat(b, sizeof b, "RENAME SONG ");
    nm_utoa((int)e->len, n);
    nm_cat(b, sizeof b, n);
    nm_cat(b, sizeof b, " OF 12");
    cv_text(x, 38, &AF_S, b, TOK_MID);

    if (e->cur < e->len) {                                    /* the character being edited */
        for (i = 0; i < (int)e->cur; i++)
            left[i] = e->s[i];
        left[i] = 0;
        one[0] = e->s[e->cur];
        one[1] = 0;
        curx = x + text_w(&AF_L, left);
        cw = text_w(&AF_L, one);
        nm_rrect(curx - 1, y - 2, cw + 2, AF_L.h, 3, TOK_RAISE);
        for (i = 0; i < (int)e->len - (int)e->cur - 1; i++)
            right[i] = e->s[e->cur + 1 + i];
        right[i] = 0;
        cv_text(x, y, &AF_L, left, TOK_THEME);
        cv_text(curx, y, &AF_L, one, TOK_ACCENT);
        cv_text(curx + cw, y, &AF_L, right, TOK_THEME);
        curx += cw;
    } else {                                                  /* the cursor is past the end */
        cv_text(x, y, &AF_L, e->s, TOK_THEME);
        curx = x + text_w(&AF_L, e->s);
    }
    if (on)                                                   /* the blinking cursor bar */
        cv_rect(curx + 2, y + 1, 2, AF_L.h - 2, TOK_ACCENT);
}
static void nm_hint(int x, int y, const char *lbl, const char *val, const aafont_t *vf)
{
    nm_rrect(x, y, 114, 44, 6, TOK_SURF);
    cv_text(x + 7, y + 5, &AF_S, lbl, TOK_MID);
    cv_text(x + 7, y + 20, vf, val, TOK_TEXT);
}
static void nm_draw_hints(const name_edit_t *e, int top)
{
    nm_hint(4, top, "white keys", e->mode ? "type 123" : "type ABC", &AF_M);
    nm_hint(122, top, "tap again", "next letter", &AF_M);
    nm_hint(4, top + 48, "black keys", "left space right del", &AF_S);
    nm_hint(122, top + 48, "k3", "random name", &AF_M);
}
static void nm_draw_foot(int y)
{
    const char *l = "HOME", *r = "cancel";
    int x = 5, w, cw;
    cv_rect(0, y, 240, 22, TOK_BG);
    x += nm_chip(x, y + 3, "SEL");
    cv_text(x + 3, y + 3, &AF_S, "save", TOK_MID);
    w = text_w(&AF_S, l) + 8 + 3 + text_w(&AF_S, r);
    x = 235 - w;
    cw = nm_chip(x, y + 3, l);
    cv_text(x + cw + 3, y + 3, &AF_S, r, TOK_MID);
}
void name_edit_draw(const name_edit_t *e, uint32_t ms)
{
    cv_begin(240, 110, TOK_BG);                              /* header + the name card */
    nm_draw_head();
    nm_draw_card(e, ms);
    cv_blit(0, 0);
    cv_begin(240, 130, TOK_BG);                              /* the hints + the footer, to 240 */
    nm_draw_hints(e, 2);
    nm_draw_foot(108);
    cv_blit(0, 110);
}
