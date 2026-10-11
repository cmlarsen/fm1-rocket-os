/* SPDX-License-Identifier: GPL-3.0-only */
/* fm1-rocket-os UI colour tokens (T2b).
 *
 * The GREY palette and its derived tokens, taken from the design system in
 * design/project/fm1.css (the `.scr` variables) as RGB565, the panel's format.
 * gfx.c maps X0X's five-step palette (C_LINE .. C_HI) and C_BLACK / C_WHITE onto
 * these; the five role colours are the clip/part identities. Anything that did
 * not map is listed in reports/T2b.md. */
#pragma once
#include <stdint.h>

#define TOK_RGB(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

/* --bg:#0e0e0e --surf:#242424 --text:#cccccc --theme:#e8e8e8 --accent:#ffffff
 * --mid:#939393 --dim:#5e5e5e --line:#303030 --sel:#bcbcbc --raise:#383838
 * --key:#a2a2a2 --ink:#0e0e0e --rec:#ff4848 */
#define TOK_BG     TOK_RGB(14, 14, 14)
#define TOK_SURF   TOK_RGB(36, 36, 36)
#define TOK_TEXT   TOK_RGB(204, 204, 204)
#define TOK_THEME  TOK_RGB(232, 232, 232)
#define TOK_ACCENT TOK_RGB(255, 255, 255)
#define TOK_MID    TOK_RGB(147, 147, 147)
#define TOK_DIM    TOK_RGB(94, 94, 94)
#define TOK_LINE   TOK_RGB(48, 48, 48)
#define TOK_SEL    TOK_RGB(188, 188, 188)
#define TOK_RAISE  TOK_RGB(56, 56, 56)
#define TOK_KEY    TOK_RGB(162, 162, 162)
#define TOK_INK    TOK_RGB(14, 14, 14)
#define TOK_REC    TOK_RGB(255, 72, 72)

/* --drum:#ff9a3c --bass:#4ea8ff --lead:#5fd36a --chord:#b98cff --fx:#ff6f9c */
#define TOK_DRUM   TOK_RGB(255, 154, 60)
#define TOK_BASS   TOK_RGB(78, 168, 255)
#define TOK_LEAD   TOK_RGB(95, 211, 106)
#define TOK_CHORD  TOK_RGB(185, 140, 255)
#define TOK_FX     TOK_RGB(255, 111, 156)
