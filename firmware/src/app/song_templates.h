/* SPDX-License-Identifier: GPL-3.0-only */
/* T8: the built-in song templates for STUDIO 8, New song.
 *
 * Two songs a user can start from, both plain `song_t` values with no pointers:
 *   - Empty: five roles with the engine defaults and sensible ranges, no clips; the
 *     same shape song_store_empty builds (a valid song that validates and round-trips);
 *   - Starter: thirteen clips (4 drums, 3 bass, 3 lead, 2 chords, 1 FX) written by
 *     hand and transposed into the chosen key, so a new song has something to play.
 *
 * Deterministic on purpose: fixed patterns and a fixed key map, so a repeated host run
 * is byte-identical and the goldens stay stable. The content is the same for a given
 * (key, tempo); tempo only sets bpm_x10, the notes do not move with it.
 *
 * `song_template_starter` needs `song_store_empty` (core/model/song_store.c) to lay
 * down the role ranges and engines, then fills the clips. Nothing here reads flash,
 * draws, or touches the audio engine. */
#pragma once
#include <stdint.h>
#include "core/model/model.h"

/* The New-song key picker offers 24 keys: 12 major (0..11) then 12 minor (12..23),
 * each a pitch class. There is no key field in model v1's song_t (a gap, reported in
 * reports/T8.md): the chosen key shapes the Starter's notes and is shown on the New
 * screen, but it is not stored with the song. */
#define SONG_TMPL_NKEYS 24u
#define SONG_TMPL_DEFAULT_KEY 21u              /* A MIN: 12 minors + A (9) */
#define SONG_TMPL_BPM_DEFAULT 1250u            /* 125.0 BPM, song_store_empty's default */
#define SONG_TMPL_BPM_MIN 200u                 /* 20.0 BPM (song_validate's floor) */
#define SONG_TMPL_BPM_MAX 3000u                /* 300.0 BPM (song_validate's ceiling) */

int         song_tmpl_key_root(int key);       /* 0..23 -> pitch class 0..11 */
int         song_tmpl_key_minor(int key);      /* 0..23 -> 0 major / 1 minor */
const char *song_tmpl_key_name(int key);       /* "C MAJ" .. "B MIN" (static) */

/* Clamp a tempo to what a song may hold (song_validate's 20.0 .. 300.0 BPM). */
uint16_t song_tmpl_bpm(uint16_t bpm_x10);

/* Fill `out` with a valid Empty song at `bpm_x10`: five roles, no clips. */
void song_template_empty(song_t *out, uint16_t bpm_x10);

/* Fill `out` with the Starter song at `bpm_x10`, transposed into `key` (0..23). */
void song_template_starter(song_t *out, uint16_t bpm_x10, int key);
