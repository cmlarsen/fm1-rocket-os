/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Small-canvas renderer (no full framebuffer): draw into an off-screen strip, then blit it in
 * one DMA transfer (X0X's layout; pixels are byte-swapped: the panel takes RGB565 big-endian).
 *
 * T2b: the text is Felucca's anti-aliased Inter Tight (tools/gen_aa_font.py -> ui_fonts.h),
 * ported to X0X's canvas. X0X's API names (cv_begin / cv_band / cv_text / cv_rect / ... and the
 * C_* colours) are kept so ui.c changes stay small; the colours now come from the GREY token
 * palette in ui_tokens.h. The strip-canvas design and blit-only-on-change behaviour are X0X's.
 *
 * A glyph is 4-bit alpha, trimmed to its ink box, advances in 1/16 px; a glyph is rasterised at
 * 1 << psh horizontal phases (S M 4, L 2) and cv_text takes the phase nearest the pen, so the
 * space between neighbours stays true. Coverage is blended over what is already on the canvas
 * (an anti-aliased face on a coloured box gets no dark fringe). No heap, no double. */
#include <stdint.h>
#include "ui_tokens.h"

typedef struct { uint16_t off, adv; int8_t bx; uint8_t by, bw, bh; } aag_t;
typedef struct {
    uint8_t h, asc, px, first, last, nex, psh;   /* line height, baseline, face size, ASCII range, extras, phases log2 */
    uint16_t nk;                                 /* kerning pairs */
    const aag_t *g;
    const uint8_t *data;
    const uint16_t *kkey;                        /* a << 8 | b, sorted */
    const int8_t *kd;                            /* 1/16 px */
    const uint8_t *ex;                           /* codes of the extra glyphs */
    const uint8_t *hc;                           /* reserved (0: plain 2 px per byte) */
} aafont_t;
#include "ui_fonts.h"

#define ELLIPSIS '\x85'                          /* the ellipsis glyph of AF_S and AF_M */

#define CV_MAX (240u * 144u)                     /* X0X: the main area, both bands, is 240 x 144 */
static uint16_t cv_px[CV_MAX] __attribute__((section(".pool")));
static uint32_t cv_w, cv_h;
static int32_t cv_oy;                            /* y offset for graph drawing */
static int32_t cv_y0, cv_y1;                     /* X0X: the rows drawing may touch (cv_band) */

/* T2b layout lint: the host build (X0X_LINT) records every text draw and every canvas begin to a
 * log; tests/lint/check_layout.py reads it. Nothing of this is compiled into the firmware. */
#ifdef X0X_LINT
#include <stdio.h>
#include <stdlib.h>
static FILE *lint_fp;
static void lint_open(void)
{
    if (!lint_fp) {
        const char *p = getenv("X0X_LINT_LOG");
        lint_fp = fopen(p ? p : "build/screens/lint.log", "w");
    }
}
#define LINT_BEGIN(w, h) do { lint_open(); if (lint_fp) fprintf(lint_fp, "B %u %u\n", (unsigned)(w), (unsigned)(h)); } while (0)
#define LINT_TEXT(x0, y0, x1, y1, px, col, s) \
    do { if (lint_fp) fprintf(lint_fp, "T %d %d %d %d %u %u %s\n", (int)(x0), (int)(y0), (int)(x1), (int)(y1), \
                             (unsigned)(px), (unsigned)(col), (s)); } while (0)
#define LINT_RECT(x, y, w, h, col) \
    do { if (lint_fp) fprintf(lint_fp, "R %d %d %d %d %u\n", (int)(x), (int)(y), (int)(w), (int)(h), (unsigned)(col)); } while (0)
#else
#define LINT_BEGIN(w, h) ((void)0)
#define LINT_TEXT(x0, y0, x1, y1, px, col, s) ((void)0)
#define LINT_RECT(x, y, w, h, col) ((void)0)
#endif

#define RGB(r, g, b) TOK_RGB(r, g, b)
#define C_BLACK TOK_BG                           /* the canvas / ink on a light fill */
#define C_WHITE TOK_ACCENT                       /* accent only: what is being touched / where we are */
/* The screen's five steps, darkest to brightest. T2b: the GREY palette of design/project/fm1.css
 * is the default; the other four carry a hue on the two brightest steps. */
typedef struct {
    const char *name;
    uint16_t c[5];
} palette_t;
#define NEUTRALS TOK_LINE, TOK_DIM, TOK_MID
static const palette_t PALETTES[] = {
    {"GREY", {NEUTRALS, TOK_THEME, TOK_TEXT}},   /* T2b: values=THEME, normal text=TEXT (design roles) */
    {"AMBER", {NEUTRALS, RGB(232, 136, 30), RGB(255, 184, 70)}},
    {"CYAN", {NEUTRALS, RGB(60, 180, 235), RGB(150, 225, 255)}},
    {"RED", {NEUTRALS, RGB(232, 76, 60), RGB(255, 132, 112)}},
    {"MONO", {NEUTRALS, RGB(196, 196, 202), RGB(236, 236, 240)}},
};
#define NPALETTES (sizeof(PALETTES) / sizeof(PALETTES[0]))
static uint16_t pal[5];
#define C_LINE pal[0]                /* 1 rules, separators */
#define C_DIM pal[1]                 /* 2 inactive, empty steps, units */
#define C_GRAY pal[2]                /* 3 labels */
#define C_AMB pal[3]                 /* 4 secondary text */
#define C_HI pal[4]                  /* 5 values, curves */

static void palette_set(uint32_t i)
{
    uint32_t k;
    for (k = 0; k < 5u; k++)
        pal[k] = PALETTES[i % NPALETTES].c[k];
}

static inline uint16_t swap16(uint32_t c) { return (uint16_t)(((c >> 8) & 0xFFu) | ((c & 0xFFu) << 8)); }

static void cv_begin(uint32_t w, uint32_t h, uint16_t bg)
{
    uint32_t i, n;
    if (w * h > CV_MAX)
        h = CV_MAX / w;
    lcd_sync();                     /* the last blit may still read cv_px */
    cv_w = w;
    cv_h = h;
    cv_oy = cv_y0 = 0;
    cv_y1 = (int32_t)h;
    LINT_BEGIN(cv_w, cv_h);
    n = w * h;
    for (i = 0; i < n; i++)
        cv_px[i] = swap16(bg);
}

/* X0X: draw the next things as if the canvas were rows y0 .. y0+h-1 alone (one band of a
 * canvas that holds several, sent in one transfer so the bands change on screen together) */
static void cv_band(int32_t y0, int32_t h)
{
    cv_oy = cv_y0 = y0;
    cv_y1 = y0 + h;
}

static void cv_blit(uint32_t x, uint32_t y) { lcd_blit(x, y, cv_w, cv_h, cv_px); }

/* canvas rows r0 .. cv_h-1 only, to screen row y + r0 */
static void cv_blit_from(uint32_t x, uint32_t y, uint32_t r0)
{
    if (r0 < cv_h)
        lcd_blit(x, y + r0, cv_w, cv_h - r0, cv_px + r0 * cv_w);
}

static inline void cv_pset(int32_t x, int32_t y, uint16_t c)
{
    y += cv_oy;
    if ((uint32_t)x < cv_w && y >= cv_y0 && y < cv_y1)
        cv_px[(uint32_t)y * cv_w + (uint32_t)x] = swap16(c);
}

static void cv_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    int32_t x1 = x + w, y1 = y + h + cv_oy, i;
    uint16_t sc = swap16(c);
    y += cv_oy;                             /* clipped once, then filled row by row */
    if (x < 0)
        x = 0;
    if (y < cv_y0)
        y = cv_y0;
    if (x1 > (int32_t)cv_w)
        x1 = (int32_t)cv_w;
    if (y1 > cv_y1)
        y1 = cv_y1;
    LINT_RECT(x, y, x1 - x, y1 - y, c);
    for (; y < y1; y++)
        for (i = x; i < x1; i++)
            cv_px[(uint32_t)y * cv_w + (uint32_t)i] = sc;
}

static void cv_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t c)
{
    int32_t dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int32_t dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int32_t err = dx + dy, guard = 2000;
    while (guard--) {                       /* bounded: a line is never longer than 480 px */
        int32_t e2 = 2 * err;               /* both tests use the same error value */
        cv_pset(x0, y0, c);
        if (x0 == x1 && y0 == y1)
            break;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

/* ------------------------------------------------------------- text --- */
/* the glyph number of ch in f, -1 = not in the face */
static int32_t glyph_at(const aafont_t *f, uint32_t ch)
{
    uint32_t i;
    if (ch >= f->first && ch <= f->last)
        return (int32_t)(ch - f->first);
    for (i = 0; i < f->nex; i++)
        if (f->ex[i] == ch)
            return (int32_t)(f->last - f->first + 1u + i);
    return -1;
}

/* the table entry of ch at phase 0 (its other phases follow it). Not in the face: the ellipsis as '.',
 * else '?', else the face's first glyph (L is sparse: its space) */
static uint32_t glyph(const aafont_t *f, uint32_t ch)
{
    int32_t k;
    if (ch >= 'a' && ch <= 'z' && f->last < 'a')
        ch -= 32u;                                   /* L has capitals only */
    if ((k = glyph_at(f, ch)) < 0 && (ch != (uint8_t)ELLIPSIS || (k = glyph_at(f, '.')) < 0) &&
        (k = glyph_at(f, '?')) < 0)
        k = 0;
    return (uint32_t)k << f->psh;
}

static int32_t kern(const aafont_t *f, uint32_t a, uint32_t b)   /* 1/16 px */
{
    uint32_t key = (a << 8) | b, lo = 0, hi = f->nk;
    while (lo < hi) {
        uint32_t m = (lo + hi) / 2u;
        if (f->kkey[m] == key)
            return f->kd[m];
        if (f->kkey[m] < key)
            lo = m + 1u;
        else
            hi = m;
    }
    return 0;
}

static uint32_t fold(const aafont_t *f, uint32_t c) { return c >= 'a' && c <= 'z' && f->last < 'a' ? c - 32u : c; }

static int32_t text_w(const aafont_t *f, const char *s)
{
    int32_t pen = 0;
    uint32_t prev = 0;
    for (; *s; s++) {
        uint32_t c = fold(f, (uint8_t)*s);
        if (prev)
            pen += kern(f, prev, c);
        pen += f->g[glyph(f, c)].adv;
        prev = c;
    }
    return (pen + 8) >> 4;
}

/* a w x h nibble bitmap (rows back to back, high nibble first) at canvas (x, y), colour fg, blended
 * over what is already on the canvas */
static void cv_nib(int32_t x, int32_t y, uint32_t w, uint32_t h, const uint8_t *d, uint16_t fg)
{
    uint32_t k = 0, gx, gy, r = fg >> 11, g = (fg >> 5) & 63u, b = fg & 31u;
    y += cv_oy;
    for (gy = 0; gy < h; gy++) {
        int32_t py = y + (int32_t)gy;
        if (py < cv_y0 || py >= cv_y1) {
            k += w;
            continue;
        }
        for (gx = 0; gx < w; gx++, k++) {
            uint32_t a = (d[k >> 1] >> ((k & 1u) ? 0 : 4)) & 15u;
            int32_t px = x + (int32_t)gx;
            if (!a || (uint32_t)px >= cv_w)
                continue;
            if (a == 15u) {
                cv_px[(uint32_t)py * cv_w + (uint32_t)px] = swap16(fg);
            } else {
                uint32_t bg = swap16(cv_px[(uint32_t)py * cv_w + (uint32_t)px]);
                uint32_t br = bg >> 11, bgg = (bg >> 5) & 63u, bb = bg & 31u;
                uint32_t nr = (r * a + br * (15u - a)) / 15u, ng = (g * a + bgg * (15u - a)) / 15u,
                         nb = (b * a + bb * (15u - a)) / 15u;
                cv_px[(uint32_t)py * cv_w + (uint32_t)px] = swap16((nr << 11) | (ng << 5) | nb);
            }
        }
    }
}

/* draw s in f at (x, y = top of the line box) in colour fg; returns the pen x at the end (px).
 * The pen (1/16 px) lies between two phase positions; the glyph takes the one whose offset from the
 * pen is nearer the previous glyph's, so the space between neighbours stays truest. A glyph with one
 * phase (figures and .) sits at the rounded pen. */
static int32_t text_run(int32_t x, int32_t y, const aafont_t *f, const char *s, uint16_t fg, int draw)
{
    int32_t pen = 0, err = 0;
    int32_t x0 = 0x7FFFFFFF, y0 = 0x7FFFFFFF, x1 = -0x7FFFFFFF, y1 = -0x7FFFFFFF;
#ifdef X0X_LINT
    const char *s0 = s;                          /* LINT_TEXT reads the whole string after the walk */
#endif
    const int32_t step = 16 >> f->psh;
    uint32_t prev = 0;
    for (; *s; s++) {
        uint32_t c = fold(f, (uint8_t)*s);
        const aag_t *g;
        int32_t pos, d0, d1;
        if (prev)
            pen += kern(f, prev, c);
        g = &f->g[glyph(f, c)];
        if (f->psh && g[1].off != g->off) {
            pos = pen - (pen & (step - 1));          /* the phase position at or left of the pen */
            d0 = pos - pen - err;                    /* .. and the one right of it: which keeps the gap truer */
            d1 = d0 + step;
            if ((d0 < 0 ? -d0 : d0) > (d1 < 0 ? -d1 : d1))
                pos += step;
            g += ((uint32_t)pos >> (4u - f->psh)) & ((1u << f->psh) - 1u);
        } else {
            pos = (pen + 8) & ~15;                   /* one phase (figures, ., blanks): the rounded pen */
        }
        err = pos - pen;
        if (g->bw) {
            int32_t gx = x + (pos >> 4) + g->bx, gy = y + g->by + cv_oy;
            if (draw)
                cv_nib(gx, gy - cv_oy, g->bw, g->bh, f->data + g->off, fg);
            if (gx < x0) x0 = gx;
            if (gx + (int32_t)g->bw > x1) x1 = gx + (int32_t)g->bw;
            if (gy < y0) y0 = gy;
            if (gy + (int32_t)g->bh > y1) y1 = gy + (int32_t)g->bh;
        }
        pen += g->adv;
        prev = c;
    }
    if (x1 > x0)
        LINT_TEXT(x0, y0, x1, y1, f->px, fg, s0);
    return x + ((pen + 8) >> 4);
}

/* text in colour c, each pixel's coverage blended over what is already on the canvas; returns the end x */
static int32_t cv_text(int32_t x, int32_t y, const aafont_t *f, const char *s, uint16_t c)
{
    return text_run(x, y, f, s, c, 1);
}

/* one-shot: text in a box, cleared to C_BLACK, blitted (align: 0 left, 1 centre, 2 right) */
static void draw_text_box(uint32_t x, uint32_t y, uint32_t w, const aafont_t *f, const char *s,
                          uint16_t c, int align)
{
    int32_t tw = text_w(f, s), tx = 0;
    cv_begin(w, f->h, C_BLACK);
    if (align == 1)
        tx = ((int32_t)w - tw) / 2;
    else if (align == 2)
        tx = (int32_t)w - tw;
    cv_text(tx, 0, f, s, c);
    cv_blit(x, y);
    lcd_sync();                     /* one-shots (boot, crash, UBOOT, update) finish here */
}
