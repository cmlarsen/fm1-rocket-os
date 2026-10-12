/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* fm1-rocket-os: NAME entry (T8c).
 *
 * Ported from Felucca firmware/src/ui_name.c (GPL-3.0-only), which names a preset or a
 * project on the device. Here it names a song, and it is driven by X0X's input events
 * (plat_buttons / plat_keys / plat_enc) instead of Felucca's ui_input.c.
 *
 * The model: a name of up to NAME_MAX characters from the set A-Z 0-9 space - _ . !,
 * plus a cursor. NAME_MAX is SONG_NAME_LEN - 1 (core/model/model.h), i.e. 11.
 *
 * The one grammar (docs/product/interaction-model.md): ALGORITHM changes the character
 * under the cursor, SELECT turns move the cursor, SEL (its push) confirms, HOME goes
 * back (cancel restores the original; a long hold clears the name). The white keys type
 * letters phone-style, the black keys are <- SPACE -> DEL and ABC/123, and K3 drops in
 * a random name (design/project/Studio-Rename.dc.html).
 *
 * This is an include-style view, like Felucca's ui_name.c (which ui_input.c included):
 * it draws through X0X's gfx.c, so it is #included into the same translation unit that
 * includes gfx.c (X0X's ui.c, once T8i wires it in). It is spelled out as a header so
 * the API is visible to the caller and to the host test. */
#pragma once
#include <stdint.h>
#include "plat.h"                 /* B_*, NE: X0X's input events */

/* SONG_NAME_LEN - 1 (core/model/model.h). Kept local so this header need not include
 * model.h beside plat.h (both define NKEYS). T8i asserts them equal at integration. */
#define NAME_MAX 11u
#define NAME_BUF (NAME_MAX + 1u)
/* TFIXU 13: the name field's usable pixel width inside the 240-wide card (text starts at x=14,
 * the card ends at 236), so a maximum-length name can be drawn without running off screen. */
#define NAME_FIELD_W 216

/* multi-tap, held repeat, and the HOME hold that clears the name */
#define NAME_TAP_MS   800u
#define NAME_REP_MS   450u
#define NAME_RATE_MS   90u
#define NAME_CLEAR_MS 800u
#define NAME_BLINK_MS 400u

/* name_edit_input's verdict */
enum { NAME_EDITING = 0, NAME_DONE = 1, NAME_CANCELLED = 2 };

typedef struct {
    char s[NAME_BUF];                 /* the working name, NUL-terminated */
    char orig[NAME_BUF];              /* what name_edit_begin was given (HOME restores it) */
    uint8_t len;                      /* characters in s, 0 .. NAME_MAX */
    uint8_t cur;                      /* the cursor, 0 .. len */
    uint8_t mode;                     /* 0 = ABC (multi-tap letters), 1 = 123 (one tap each) */
    uint8_t key;                      /* the white key (place + 1) whose group is being cycled, 0 none */
    uint8_t tap;                      /* .. that group's character index */
    uint8_t rep;                      /* the held <- -> DEL key (+ 1), 0 none */
    uint32_t t;                       /* the last multi-tap, plat_ms */
    uint32_t rep_t;                   /* the next held repeat, plat_ms */
    uint32_t home_t;                  /* HOME went down at, plat_ms */
    uint8_t home_held;                /* HOME is down */
    uint8_t home_cleared;             /* the long hold already cleared the name */
    uint32_t rnd;                     /* the xorshift seed for K3's random names (0 = not started) */
    uint32_t prev_btn;                /* the previous frame's held buttons (edge detection) */
    uint32_t prev_keys;               /* the previous frame's held keys (edge detection) */
} name_edit_t;

/* One UI frame of X0X's input, exactly as input() reads it */
typedef struct {
    uint32_t btn;                     /* held buttons, bit B_* */
    uint32_t keys;                    /* held keys, bit k = key k (0 = F3) */
    int32_t enc[NE];                  /* detents since the last frame (plat_enc), EN_* */
    uint32_t ms;                      /* plat_ms() */
} name_event_t;

/* 1 = c is one of A-Z 0-9 space - _ . ! (the model's character set) */
int name_char_allowed(char c);

void name_edit_begin(name_edit_t *e, const char *initial);
int  name_edit_input(name_edit_t *e, const name_event_t *event);
uint32_t name_edit_result(const name_edit_t *e, char *out);   /* out must hold NAME_BUF */
void name_edit_draw(const name_edit_t *e, uint32_t ms);
/* TFIXU 13: the pixel width the field draws the whole name at (AF_L, or a smaller face when the
 * full name would overflow NAME_FIELD_W). Exposed so the host test can assert the field fits. */
int name_edit_width(const name_edit_t *e);
