/* SPDX-License-Identifier: GPL-3.0-only */
/* T7a: the STUDIO song home — the song is the edited object. Included by ui.c (like ui_live.c) so
 * it can draw with the same helpers and reach ui/proj/seq. It follows the design in
 * design/project/Studio-Song, Studio-EmptyClip, Studio-ClipSettings, Studio-ClearClip,
 * Studio-CopyClip and Studio-Undo, at 1:1.
 *
 * The cursor is (role, column). SELECT moves between roles, ALGORITHM along a row; SEL launches the
 * clip under the cursor. The keys mirror the grid: a loaded key selects and launches it (hold it to
 * copy onto an empty same-role target), HOME + a key holds to clear it, SEL held on the cursor also
 * clears. HOME+REC/PLAY undo and redo. SEQ creates an empty clip if the slot is empty and opens the
 * clip settings (STUDIO 5); T7b inserts the editor before it.
 *
 * All model access goes through song_io (plain void *), so this file never needs model.h. */
#include "song_io.h"

#define ST_ROLE_N 5

static const uint16_t ST_COL[ST_ROLE_N] = {TOK_DRUM, TOK_BASS, TOK_LEAD, TOK_CHORD, TOK_FX};
static const char *const ST_ROLE[ST_ROLE_N] = {"DRUM", "BASS", "LEAD", "CHRD", "FX"};

#define ST_CLEAR_MS 1500u                 /* hold-to-confirm, like the take (design: ~1.5 s) */
#define ST_TOAST_MS 1800u

typedef struct {
    uint8_t role, col;                    /* the cursor: a row (role) and a column (clip in it) */
    uint8_t copy_src;                     /* the clip being copied, 0xFF none */
    uint8_t clear_clip;                   /* the clip being cleared, 0xFF none */
    uint8_t clear_sel;                    /* the clear is SEL-on-cursor, not HOME+key */
    uint8_t clear_key;                    /* the key held for the clear, 0xFF for SEL */
    uint32_t clear_t0;
    char tag[8], msg[26];                 /* the toast (UNDO / REDO / CLEAR / COPY) */
    uint32_t toast_until;
} studio_t;

static studio_t st = {.copy_src = 0xFF, .clear_clip = 0xFF, .clear_key = 0xFF};

/* ---- helpers ---- */
static int st_role_n(int r) { return live_bridge_role_nclips(r); }
static int st_role_first(int r) { return live_bridge_role_first(r); }

static int st_cursor_clip(void)
{
    int first = st_role_first(st.role), n = st_role_n(st.role);
    if (first < 0 || n <= 0)
        return -1;
    if (st.col >= (uint8_t)n)
        st.col = (uint8_t)(n - 1);
    return first + st.col;
}
int studio_cursor(void) { return ((int)st.role << 8) | st.col; }
int studio_cursor_clip(void) { return st_cursor_clip(); }
int studio_cursor_role(void) { return st.role; }
void studio_set_cursor(int role, int col)
{
    if (role < 0) role = 0;
    if (role >= ST_ROLE_N) role = ST_ROLE_N - 1;
    st.role = (uint8_t)role;
    st.col = (uint8_t)(col < 0 ? 0 : col);
    if (st.col >= (uint8_t)st_role_n(st.role))
        st.col = 0;
    build_pages();
}

/* "LEAD B" for a clip index */
static void st_name(int clip, char *out)
{
    int role = live_bridge_role_of(clip), first = role >= 0 ? live_bridge_role_first(role) : -1;
    char *q = put_s(out, role >= 0 ? ST_ROLE[role] : "?");
    if (first >= 0 && clip >= first) {
        *q++ = ' ';
        *q++ = (char)('A' + (clip - first));
    }
    *q = 0;
}
static char st_letter(int clip)
{
    int role = live_bridge_role_of(clip), first = role >= 0 ? live_bridge_role_first(role) : -1;
    return (first >= 0 && clip >= first) ? (char)('A' + (clip - first)) : '?';
}
/* model.h's key/scale names (the enum is not included here; the values are frozen) */
static const char *const ST_KEY[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static const char *const ST_SCALE[5] = {"major", "minor", "dorian", "penta", "chromatic"};

/* a clip length, as the card and the ring read it: bars when it lands on a bar, else steps ("12
 * steps"), never "0 bars" (T7a D6). */
static char *st_len(char *q, int len)
{
    if (len > 0 && len % 16 == 0) {
        q = put_i(q, len / 16);
        return put_s(q, len == 16 ? " bar" : " bars");
    }
    q = put_i(q, len < 1 ? 1 : len);
    return put_s(q, len == 1 ? " step" : " steps");
}

static void st_toast(const char *tag, const char *msg)
{
    put_s(st.tag, tag);
    put_s(st.msg, msg);
    st.toast_until = plat_ms() + ST_TOAST_MS;
}
/* the sentence a toast shows for an undo/redo type */
static void st_action(int type, int clip, char *out)
{
    char nm[16];
    if (clip >= 0)
        st_name(clip, nm);
    else
        put_s(nm, "a clip");
    switch (type) {
    case 1: put_s(put_s(out, "cleared "), nm); break;    /* UNDO_CLEAR_CLIP */
    case 2: put_s(put_s(out, "copied to "), nm); break;  /* UNDO_COPY_CLIP */
    case 3: put_s(put_s(out, "recorded "), nm); break;   /* UNDO_RECORD_CLIP */
    default: put_s(out, "the last edit"); break;
    }
}

/* ================================================================ the mode === */

int studio_mode(void) { return ui.view == V_STUDIO; }
int studio_holding(void) { return st.clear_clip != 0xFF; }

void studio_enter(void)
{
    if (st.role >= ST_ROLE_N)
        st.role = 0;
    if (st_role_n(st.role) <= 0) {
        st.role = 0;
        st.col = 0;
    }
    if (st.col >= (uint8_t)st_role_n(st.role))
        st.col = 0;
    st.copy_src = 0xFF;
    build_pages();
}

/* ============================================================ the gestures === */

static void st_finish_clear(void)
{
    int c = st.clear_clip, role = live_bridge_role_of(c);
    char nm[16];
    st_name(c, nm);
    if (role >= 0 && live_bridge_playing_clip((uint8_t)role) == c)
        live_bridge_stop((uint8_t)role);      /* T7a D7: clearing the playing clip stops the role */
    song_io_clip_clear(c);
    st_toast("CLEAR", nm);
    if (st.clear_sel)
        ui.btn_used |= 1u << B_SEL;   /* suppress the SEL release */
    st.clear_clip = 0xFF;
}

static int st_clear_locked(void)
{
    if (!song_io_setup_get(SONG_SETUP_LOCK) || !live_bridge_transport_playing())
        return 0;
    st_toast("LOCKED", "stage lock");
    return 1;
}

/* Start a clear on a clip if it holds one (HOME + a pad, or SEL on the cursor). */
static void st_clear_start(int clip, int sel, int key)
{
    if (clip < 0 || !live_bridge_clip_used(clip) || st_clear_locked())
        return;
    st.clear_clip = (uint8_t)clip;
    st.clear_sel = (uint8_t)sel;
    st.clear_key = (uint8_t)key;
    st.clear_t0 = plat_ms();
}

void studio_tick(void)
{
    uint32_t now = plat_ms();
    /* SEL held on the cursor: a clear after a 250 ms hold (T7a D4: a tap is only a launch), like
     * HOME + a pad; the ring runs 1.5 s from the press. */
    if ((ui.view == V_STUDIO) && (ui.btn & (1u << B_SEL)) && !(ui.btn & (1u << B_HOME)) &&
        st.clear_clip == 0xFF && (now - ui.down_t[B_SEL]) >= 250u) {
        int c = st_cursor_clip();
        if (c >= 0 && live_bridge_clip_used(c) && !st_clear_locked()) {
            st.clear_clip = (uint8_t)c;
            st.clear_sel = 1;
            st.clear_key = 0xFF;
            st.clear_t0 = ui.down_t[B_SEL];
        }
    }
    if (st.clear_clip != 0xFF) {
        int held = st.clear_sel ? (ui.btn & (1u << B_SEL)) != 0 : ((ui.keys >> st.clear_key) & 1u) != 0;
        if (!held)
            st.clear_clip = 0xFF;                        /* released early: cancel */
        else if (now - st.clear_t0 >= ST_CLEAR_MS)
            st_finish_clear();
    }
    if (st.toast_until && (int32_t)(st.toast_until - now) <= 0)
        st.toast_until = 0;
}

static void studio_launch_cursor(void)
{
    int c = st_cursor_clip();
    if (c >= 0 && live_bridge_clip_used(c))
        live_bridge_launch((uint8_t)c);
}

static void studio_undo(void)
{
    int type = song_io_undo_type(), c = song_io_undo_clip(), rc = song_io_undo();
    char m[26];
    if (rc == 1) {
        st_action(type, c, m);
        st_toast("UNDO", m);
    } else {
        st_toast("UNDO", "can't undo");
    }
}
static void studio_redo(void)
{
    int type = song_io_redo_type(), c = song_io_redo_clip(), rc = song_io_redo();
    char m[26];
    if (rc == 1) {
        st_action(type, c, m);
        st_toast("REDO", m);
    } else {
        st_toast("REDO", "can't redo");
    }
}

/* a key down/up on the song home */
static void studio_handle_key(int k, int down)
{
    int clip = live_bridge_key_clip(k);
    int role = clip >= 0 ? live_bridge_role_of(clip) : -1;
    int used = clip >= 0 && live_bridge_clip_used(clip);
    if (!down) {
        if (st.copy_src == clip)
            st.copy_src = 0xFF;                          /* the source let go: the copy ends */
        if (st.clear_clip == clip && !st.clear_sel)
            st.clear_clip = 0xFF;                        /* HOME + a released pad: cancel */
        return;
    }
    if (clip < 0 || role < 0)
        return;
    if (ui.btn & (1u << B_HOME)) {                       /* HOME + a pad: hold to clear */
        ui.btn_used |= 1u << B_HOME;
        st_clear_start(clip, 0, k);
        return;
    }
    if (st.copy_src != 0xFF) {                           /* a target while the source is held */
        int s = st.copy_src;
        if (!used && live_bridge_role_of(s) == role) {
            char nm[16];
            song_io_clip_copy(clip, s);
            st_name(clip, nm);
            st_toast("COPY", nm);
        }
        return;
    }
    st.role = (uint8_t)role;                             /* select the cell */
    st.col = (uint8_t)(clip - st_role_first(role));
    build_pages();
    if (used) {
        live_bridge_launch((uint8_t)clip);               /* a key press selects and launches */
        st.copy_src = (uint8_t)clip;                     /* keep holding to copy it */
    } else {
        say("EMPTY SLOT", 0);
    }
}

/* T7a D2: EDIT opens the cursor role's sound page (never an X0X pattern editor):
 * DRUMS -> the 909, BASS -> 303A, LEAD -> 303B, CHORDS -> the FM6 card, FX -> the NOISE card. */
static void studio_open_sound(void)
{
    ui.sound_role = st.role;
    if (st.role == 0)
        ui.part = PART_909;
    else if (st.role == 1)
        ui.part = PART_303A;
    else if (st.role == 2)
        ui.part = PART_303B;
    else {
        set_view(V_SOUND);
        return;
    }
    set_view(V_PART);
}

static int studio_handle_button(int b)
{
    if (ui.view == V_SOUND) {                            /* the FM6/NOISE sound card */
        if (b == B_EDIT) {
            *page_of(V_SOUND) = (uint8_t)((cur_page() + 1) % pg.n);
            return 1;
        }
        return 0;                                        /* FX/MIX/GLO still open; HOME/SEQ handled */
    }
    if (ui.view == V_CLIPSET) {
        switch (b) {
        case B_HOME:
            set_view(V_STUDIO);
            studio_enter();
            return 1;
        case B_EDIT:
            studio_open_sound();
            return 1;
        case B_SEL:
            return 1;
        case B_SEQ:                                      /* T7b: the cycle ends back at the song home */
            set_view(V_STUDIO);
            studio_enter();
            return 1;
        case B_REC:
            return (ui.btn & (1u << B_HOME)) ? 0 : 1;    /* HOME + REC: undo, handled by ui.c */
        default:
            return 0;                                    /* FX / GLO ... still work */
        }
    }
    switch (b) {
    case B_SEL:
        studio_launch_cursor();
        return 1;
    case B_HOME:
        return 1;                                        /* the song home is the root */
    case B_EDIT:
        studio_open_sound();
        return 1;
    case B_SEQ: {                                        /* STUDIO 2/3/4: create the clip, open the editor */
        int c = st_cursor_clip();
        if (c >= 0) {
            clip_open(c);                                /* T7b: creates an empty slot (undoably) + NOTES */
            say("CLIP EDIT", 0);
        }
        return 1;
    }
    case B_REC:
        if (ui.btn & (1u << B_HOME))
            return 0;                                    /* HOME + REC: undo, handled by ui.c */
        {                                                /* T7b: record into the cursor clip */
            int c = st_cursor_clip();
            if (c >= 0) {
                clip_record_open(c);                     /* creates an empty slot: one undo entry */
                say("REC", 0);
            }
        }
        return 1;
    default:
        return 0;                                        /* FX, LFO, GLO, PLAY, SAVE ... */
    }
}

/* the encoders: SELECT between roles, ALGORITHM along the row, K1-K4 the clip settings */
static void studio_input(void)
{
    int e, i;
    if ((e = enc(EN_SELECT)) != 0) {
        int r = (int)st.role + (e > 0 ? 1 : -1);
        r = r < 0 ? 0 : r >= ST_ROLE_N ? ST_ROLE_N - 1 : r;
        if (r != (int)st.role) {
            st.role = (uint8_t)r;
            st.col = 0;
            build_pages();
        }
    }
    if ((e = enc(EN_ALGO)) != 0) {
        int n = st_role_n(st.role);
        int c = (int)st.col + (e > 0 ? 1 : -1);
        c = c < 0 ? 0 : c > n - 1 ? n - 1 : c;
        st.col = (uint8_t)c;
        build_pages();
    }
    for (i = 0; i < 4; i++) {
        if ((e = enc(EN_K1 + i)) == 0)
            continue;
        turn_ref(pg.r[cur_page()][i], e, EN_K1 + i);
        ui.touched = (int8_t)i;
        ui.touch_until = plat_ms() + 900u;
    }
    if ((e = enc(EN_PRESET)) != 0) {                     /* HOME + PRESETS: the song's tempo */
        if (ui.btn & (1u << B_HOME)) {
            int b = (int)live_bridge_bpm_x10() + e * 10;
            ui.btn_used |= 1u << B_HOME;
            if (b < 200)
                b = 200;
            if (b > 3000)
                b = 3000;
            song_io_set_bpm(b);
            live_bridge_set_bpm((uint16_t)b);
            live_bridge_sync_tempo();
            ui.touched = 4;
            ui.touch_until = plat_ms() + 900u;
        }
    }
}

/* the sound pages' keys (EDIT from the song home): audition the role, as LIVE's PLAY layer does,
 * never edit X0X steps */
static void studio_sound_key(int k, int down)
{
    int role = ui.view == V_SOUND ? (int)ui.sound_role
             : ui.part == PART_303A ? 1 : ui.part == PART_303B ? 2 : 0;   /* BASS / LEAD / DRUMS */
    if (down)
        note_key_down(k, (uint8_t)role, (uint8_t)(41 + k));   /* tracked, so a later view change releases it */
    else
        note_key_up(k);
}

/* ================================================================ drawing === */

#define ST_CARD_Y 2
#define ST_CARD_H 30
#define ST_GRID_Y 34
#define ST_ROW_H 18
#define ST_CELL 16
#define ST_CELL_GAP 3
#define ST_CELL_X 56

static int st_row_y(int r) { return ST_GRID_Y + r * ST_ROW_H; }

/* one cell: empty (hollow), loaded (tint), playing (colour + progress), queued (blink) */
static void st_cell(int cx, int cy, int role, int clip, int cursor)
{
    int used = live_bridge_clip_used(clip);
    char L[2];
    L[0] = st_letter(clip);
    L[1] = 0;
    if (!used) {
        if (st.copy_src != 0xFF && live_bridge_role_of(st.copy_src) == role) {
            uint16_t bg = (ui.frame & 32u) ? TOK_RAISE : TOK_BG;   /* T7a D7: a copy target blinks */
            box(cx, cy, ST_CELL, ST_CELL, bg);
            frame(cx, cy, ST_CELL, ST_CELL, TOK_ACCENT);
            text_c(cx + ST_CELL / 2, vc(&AF_S, cy, ST_CELL), &AF_S, "+", TOK_ACCENT);
        } else {
            rframe(cx, cy, ST_CELL, ST_CELL, 3, C_DIM);
        }
    } else {
        int playing = live_bridge_playing_clip((uint8_t)role) == clip;
        int q = live_bridge_queued_clip((uint8_t)role), qstart = q == clip;
        int qstop = playing && live_bridge_stop_queued(role);
        uint16_t bg, fg;
        int outline = 0;
        if (st.copy_src != 0xFF && !playing && !qstart) {   /* a copy target: a dashed-ish dim cell */
            bg = dim(ST_COL[role], 4);
            fg = ST_COL[role];
        } else if (qstart && (ui.frame & 4u)) {
            outline = 1;
            bg = C_BLACK;
            fg = ST_COL[role];
        } else if (qstop && (ui.frame & 32u)) {
            bg = dim(ST_COL[role], 8);
            fg = ST_COL[role];
        } else if (playing) {
            bg = ST_COL[role];
            fg = C_BLACK;
        } else {
            bg = dim(ST_COL[role], 4);
            fg = ST_COL[role];
        }
        if (outline)
            frame(cx, cy, ST_CELL, ST_CELL, ST_COL[role]);
        else
            box(cx, cy, ST_CELL, ST_CELL, bg);
        text_c(cx + ST_CELL / 2, vc(&AF_S, cy, ST_CELL), &AF_S, L, fg);
        if (playing) {
            int p = live_bridge_progress((uint8_t)role);
            rbox(cx + 3, cy + ST_CELL - 4, (ST_CELL - 6) * p / 255, 2, 1, C_BLACK);
        }
    }
    if (cursor)                                          /* the cursor: a bright frame */
        frame(cx - 1, cy - 1, ST_CELL + 2, ST_CELL + 2, TOK_ACCENT);
}

static void st_draw_card(int band)
{
    char nm[16], t[28];
    int c = st_cursor_clip();
    int used = c >= 0 && live_bridge_clip_used(c);
    if (band != 0)
        return;
    box(3, ST_CARD_Y, 234, ST_CARD_H, TOK_SURF);
    if (!used) {                                         /* STUDIO 2: the empty-slot card */
        int role = c >= 0 ? live_bridge_role_of(c) : st.role;
        char L[2];
        L[0] = st_letter(c);
        L[1] = 0;
        cv_text(10, vc(&AF_M, ST_CARD_Y + 1, 15), &AF_M, ST_ROLE[role], ST_COL[role]);
        {
            int32_t w = tw(&AF_M, ST_ROLE[role]) + 5;
            put_s(put_s(put_s(t, L), " EMPTY"), "");
            cv_text(10 + w, vc(&AF_M, ST_CARD_Y + 1, 15), &AF_M, t, TOK_TEXT);
        }
        cv_text(10, vc(&AF_S, ST_CARD_Y + 16, 11), &AF_S, "play it in, or step it in", TOK_MID);
        return;
    }
    song_io_name(nm, sizeof nm);
    cv_text(10, vc(&AF_M, ST_CARD_Y + 1, 15), &AF_M, nm[0] ? nm : "UNTITLED", TOK_THEME);
    {   /* T7a D6: "<key> <scale> - 5 roles" */
        char *q = put_s(t, ST_KEY[song_io_key() % 12]);
        *q++ = ' ';
        q = put_s(q, ST_SCALE[song_io_scale() % 5]);
        put_s(q, " - 5 roles");
    }
    cv_text(10, vc(&AF_S, ST_CARD_Y + 16, 11), &AF_S, t, TOK_MID);
    st_name(c, nm);
    text_r(230, vc(&AF_M, ST_CARD_Y + 1, 15), &AF_M, nm, ST_COL[live_bridge_role_of(c)]);
    {   /* T7a D6: the real length (steps under a bar) and mode */
        char u[24], *q = st_len(u, song_io_clip_len(c));
        q = put_s(q, " - ");
        put_s(q, song_io_clip_mode(c) ? "once" : "loop");
        text_r(230, vc(&AF_S, ST_CARD_Y + 16, 11), &AF_S, u, TOK_MID);
    }
}

static void st_draw_grid(int band)
{
    int r, j;
    int cursor = studio_cursor_clip();
    for (r = 0; r < ST_ROLE_N; r++) {
        int first = live_bridge_role_first(r), n = live_bridge_role_nclips(r);
        int gy = st_row_y(r), ly = gy - band * BAND_H;
        if (ly >= BAND_H || ly + ST_ROW_H <= 0)
            continue;
        if (r == (int)st.role) {
            box(2, ly, 236, ST_ROW_H - 1, TOK_SURF);
            play_icon(6, ly + 6, ST_COL[r]);
        }
        cv_text(18, vc(&AF_S, ly, ST_ROW_H - 1), &AF_S, ST_ROLE[r], ST_COL[r]);
        for (j = 0; j < n && j < 7; j++) {
            int clip = first + j, cx = ST_CELL_X + j * (ST_CELL + ST_CELL_GAP);
            if (cx + ST_CELL > 238)
                break;
            st_cell(cx, ly + 1, r, clip, clip == cursor);
        }
    }
}

static void st_draw_toast(int band)
{
    if (!st.toast_until || band != 1)
        return;
    {
        /* T7a D3: the toast lives in the band it is drawn into (the main area is banded), so it
         * is actually blitted: absolute y 100 -> 31 in band 1 */
        int32_t y = 100 - band * BAND_H;
        int32_t w = tw(&AF_S, st.tag) + 8 + tw(&AF_S, st.msg) + 16;
        int32_t x = 120 - w / 2;
        box(x, y, w, 22, TOK_THEME);
        cv_text(x + 8, vc(&AF_S, y, 22), &AF_S, st.tag, TOK_MID);
        cv_text(x + 8 + tw(&AF_S, st.tag) + 8, vc(&AF_S, y, 22), &AF_S, st.msg, TOK_INK);
    }
}

#define ST_SET_CHIP_H 18
static void st_draw_clipset(int band)
{
    int c = st_cursor_clip();
    char nm[16];
    int mode = c >= 0 ? song_io_clip_mode(c) : 0;
    int quant = c >= 0 ? song_io_clip_quant(c) : 0;
    int i;
    static const char *const QN[3] = {"bar", "beat", "end of clip"};   /* the model's Q_* order */
    if (band == 0) {
        st_name(c, nm);
        cv_text(8, vc(&AF_L, 0, 34), &AF_L, nm, c >= 0 ? ST_COL[live_bridge_role_of(c)] : TOK_TEXT);
        cv_text(8, vc(&AF_S, 38, 12), &AF_S, "mode - K2", TOK_MID);
        for (i = 0; i < 2; i++) {                        /* Loop / Once chips */
            int x = 8 + i * 114;
            uint16_t bg = i == mode ? ST_COL[live_bridge_role_of(c)] : TOK_RAISE;
            box(x, 50, 106, ST_SET_CHIP_H, bg);
            text_c(x + 53, vc(&AF_M, 50, ST_SET_CHIP_H), &AF_M, i ? "Once" : "Loop", i == mode ? TOK_INK : TOK_TEXT);
        }
    } else {
        cv_text(8, vc(&AF_S, 0, 12), &AF_S, "launch on - K3", TOK_MID);
        for (i = 0; i < 3; i++) {                        /* bar / beat / end of clip */
            int x = 4 + i * 79;
            uint16_t bg = i == quant ? TOK_THEME : TOK_RAISE;
            box(x, 14, 75, ST_SET_CHIP_H, bg);
            text_c(x + 37, vc(&AF_M, 14, ST_SET_CHIP_H), &AF_M, QN[i], i == quant ? TOK_INK : TOK_TEXT);
        }
        cv_text(8, vc(&AF_S, 46, 12), &AF_S, "loops until stopped or replaced", TOK_MID);
    }
}

/* T7a D2 / TCLS: V_PART is a sound page only — a card, no X0X step grid or gestures */
static void studio_draw_part(int band)
{
    int role = ui.part == PART_303A ? 1 : ui.part == PART_303B ? 2 : 0;
    if (band == 0) {
        box(3, 2, 234, 64, TOK_SURF);
        cv_text(14, vc(&AF_L, 4, 34), &AF_L, ST_ROLE[role], ST_COL[role]);
        cv_text(14, vc(&AF_S, 42, 12), &AF_S, is_303() ? "303 tone - K1-K4" : "909 kit - K1-K4", TOK_MID);
        cv_text(14, vc(&AF_S, 54, 12), &AF_S, is_303() ? "keys play the line" : "keys play the kit", TOK_MID);
    } else {
        cv_text(10, vc(&AF_S, 4, 12), &AF_S, "HOME: song    GLO: setup", TOK_MID);
    }
}

/* V_SOUND: the CHORDS/FM6 and FX/NOISE sound cards (T_RSV macros on K1-K4) */
static void studio_draw_sound(int band)
{
    int role = ui.sound_role;
    const char *eng = role == 3 ? "FM6" : "NOISE";
    if (band == 0) {
        box(3, 2, 234, 64, TOK_SURF);
        cv_text(14, vc(&AF_L, 4, 34), &AF_L, eng, ST_COL[role % ST_ROLE_N]);
        cv_text(14, vc(&AF_S, 42, 12), &AF_S, role == 3 ? "the CHORDS sound" : "the FX sound", TOK_MID);
        cv_text(14, vc(&AF_S, 54, 12), &AF_S, "macros on K1-K4; EDIT: next page", TOK_MID);
    } else {
        cv_text(10, vc(&AF_S, 4, 12), &AF_S, "HOME: song    GLO: setup", TOK_MID);
    }
}

static uint16_t studio_footer_sound(const char **a, const char **b, const char **c)
{
    *a = "K1-K4: SOUND";
    *b = "HOME: SONG";
    *c = 0;
    return C_DIM;
}

static void studio_draw_ring(void)
{
    uint32_t el = plat_ms() - st.clear_t0;
    float p = (float)el / (float)ST_CLEAR_MS;
    int c = st.clear_clip;
    char nm[16], sum[24], *q;
    if (p > 1.0f)
        p = 1.0f;
    box(0, 0, 240, 2 * BAND_H, TOK_BG);                  /* hide the grid behind the dialog */
    box(14, 26, 212, 86, TOK_SURF);
    frame(14, 26, 212, 86, TOK_LINE);
    disc(48.0f, 69.0f, 27.0f, TOK_LINE);
    disc(48.0f, 69.0f, 22.0f, TOK_SURF);
    arc(48, 69, 24, p, TOK_LINE, TOK_REC, 0);
    text_c(48, vc(&AF_M, 60, 18), &AF_M, "\x20", TOK_TEXT);
    st_name(c, nm);
    {
        char L[2];
        L[0] = st_letter(c);
        L[1] = 0;
        text_c(48, vc(&AF_M, 60, 18), &AF_M, L, ST_COL[live_bridge_role_of(c)]);
    }
    cv_text(88, vc(&AF_S, 36, 12), &AF_S, "clear", TOK_REC);
    cv_text(88, vc(&AF_M, 52, 18), &AF_M, nm, TOK_TEXT);
    q = put_i(sum, song_io_clip_len(c) / 16);
    q = put_s(q, song_io_clip_len(c) / 16 == 1 ? " bar - " : " bars - ");
    if (live_bridge_role_of(c) == 0) {                /* DRUMS: a lane count reads truer than notes */
        q = put_i(q, song_io_clip_motion(c));
        q = put_s(q, " knob lanes");
    } else {
        q = put_i(q, song_io_clip_nnotes(c));
        q = put_s(q, " notes");
    }
    cv_text(88, vc(&AF_S, 74, 12), &AF_S, sum, TOK_MID);
    text_c(120, vc(&AF_S, 100, 12), &AF_S, "release: keep", TOK_MID);
}

static void studio_draw(int band)
{
    if (ui.view == V_CLIPSET)
        st_draw_clipset(band);
    else {
        st_draw_card(band);
        st_draw_grid(band);
        st_draw_toast(band);
    }
}

/* ---- the header ---- */
static void studio_draw_header(void)
{
    char t[16], nm[16];
    int32_t x, w;
    int playing = live_bridge_transport_playing();
    w = tw(&AF_S, "STUDIO") + 10;
    box(4, 3, w, 14, TOK_KEY);
    cv_text(4 + 5, vc(&AF_S, 3, 14), &AF_S, "STUDIO", TOK_INK);
    {
        const char *layer;
        int32_t lx = 4 + w + 6;
        if (ui.view == V_CLIPSET) {
            st_name(st_cursor_clip(), nm);
            layer = nm;
        } else {
            layer = help_shift_active() ? "SHIFT" : st.copy_src != 0xFF ? "COPY" : "SONG";
        }
        cv_text(lx, vc(&AF_S, 3, 14), &AF_S, layer, TOK_MID);
        x = lx + tw(&AF_S, layer) + 2;               /* T7a D6: 2 px clearance from the transport */
        if (x < 104)
            x = 104;
    }
    if (playing)
        play_icon(x, 4, C_WHITE);
    else
        box(x + 1, 5, 8, 8, TOK_MID);
    put_i(t, (int)(live_bridge_bpm_x10() / 10u));
    cv_text(x + 14, vc(&AF_M, 1, 17), &AF_M, t, C_WHITE);
    {
        uint8_t bar = 0, beat = 0;
        char u[8], *q = u;
        live_bridge_pos(&bar, &beat, 0);
        q = put_i(q, bar + 1);
        *q++ = '.';
        put_i(q, beat + 1);
        text_r(226, vc(&AF_S, 1, 17), &AF_S, u, playing ? C_HI : C_GRAY);
    }
    if (ui.dirty || song_io_dirty())
        dot(233, 9, 1, C_GRAY);
    cv_commit(0, 0, 0);
}

/* ---- the footer ---- */
static uint16_t studio_footer(const char **a, const char **b, const char **c)
{
    *a = *b = *c = 0;
    if (st.copy_src != 0xFF) {
        *a = "TAP A + SLOT: COPY";
        return C_WHITE;
    }
    if (ui.view == V_CLIPSET) {
        *a = "SONG DEFAULT: BAR";
        *b = "SEQ: PAGE 1";
    } else if (st_cursor_clip() >= 0 && !live_bridge_clip_used(st_cursor_clip())) {
        *a = "SEQ STEPS";
        *b = "HOLD A CLIP: COPY";
        return C_DIM;
    } else {
        *a = "SEQ EDIT";
        *b = "EDIT SOUND";
        *c = "HOLD PLAY: LIVE";
    }
    return C_DIM;
}

/* ---- the key lights: the grid mirrored on the keys, the cursor bright, copy targets blinking ---- */
static void studio_leds(uint32_t *btn, uint32_t *key, uint32_t *dim)
{
    int k;
    int cursor = studio_cursor_clip();
    (void)btn;
    for (k = 0; k < NKEYS; k++) {
        int clip = live_bridge_key_clip(k);
        int role = clip >= 0 ? live_bridge_role_of(clip) : -1;
        int used, playing, q, qstart, qstop;
        if (clip < 0 || role < 0)
            continue;
        used = live_bridge_clip_used(clip);
        if (st.copy_src != 0xFF && !used && live_bridge_role_of(st.copy_src) == role) {
            if (ui.frame & 32u)                          /* a copy target: slow blink */
                *key |= 1u << k;
            else
                *dim |= 1u << k;
            continue;
        }
        playing = live_bridge_playing_clip((uint8_t)role) == clip;
        q = live_bridge_queued_clip((uint8_t)role);
        qstart = q == clip;
        qstop = playing && live_bridge_stop_queued(role);
        if (qstop) {
            if (ui.frame & 32u)
                *key |= 1u << k;
            else
                *dim |= 1u << k;
        } else if (qstart) {
            if (ui.frame & 4u)
                *key |= 1u << k;
        } else if (playing) {
            *key |= 1u << k;
        } else if (used) {
            *dim |= 1u << k;
        }
        if (clip == cursor)
            *key |= 1u << k;                             /* the cursor */
    }
}
