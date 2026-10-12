/* SPDX-License-Identifier: GPL-3.0-only */
/* T8: the SONGS view - see ui_songs.h.
 *
 * No flash, no engine, no heap: the state machine runs from the events the shell feeds
 * it and everything the songs themselves need goes through songs_ops_t. Drawing uses
 * only gfx.c's canvas (cv_begin / cv_rect / cv_line / cv_text / cv_blit) and the AF_
 * faces, so it is compiled after gfx.c like ui_live.c and ui_name.c. */
#include "ui_songs.h"

/* The New-song key picker's own constants (model-free): 12 majors then 12 minors, the
 * same 0..23 the templates take. A MIN is the default the design highlights. */
#define SONGS_NKEYS 24u
#define SONGS_KEY_DEFAULT 21u
#define SONGS_BPM_DEFAULT 1250u
#define SONGS_BPM_MIN 200u
#define SONGS_BPM_MAX 3000u
static const char *const SR_KEYN[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static const char *sr_key_name(int key)
{
    static char b[8];
    int k = key % (int)SONGS_NKEYS;
    const char *n;
    int i = 0, j;
    if (k < 0)
        k += (int)SONGS_NKEYS;
    n = SR_KEYN[k % 12];
    for (j = 0; n[j] && i < 3; j++)
        b[i++] = n[j];
    b[i++] = ' ';
    b[i++] = 'M';
    if (k / 12) {
        b[i++] = 'I';
        b[i++] = 'N';
    } else {
        b[i++] = 'A';
        b[i++] = 'J';
    }
    b[i] = 0;
    return b;
}
static uint16_t sr_bpm(int v)
{
    if (v < (int)SONGS_BPM_MIN)
        return SONGS_BPM_MIN;
    if (v > (int)SONGS_BPM_MAX)
        return SONGS_BPM_MAX;
    return (uint16_t)v;
}

/* ---- tiny string helpers (no libc on the device path) ----------------------- */
static char *sr_itoa(char *b, int v)
{
    char t[4];
    int i = 0, n = v < 0 ? 0 : v;
    if (n == 0)
        t[i++] = '0';
    while (n > 0 && i < 4) {
        t[i++] = (char)('0' + n % 10);
        n /= 10;
    }
    while (i > 0)
        *b++ = t[--i];
    *b = 0;
    return b;
}
static char *sr_cat(char *b, const char *s)
{
    while (*s)
        *b++ = *s++;
    *b = 0;
    return b;
}
static void sr_copy(char *dst, uint32_t cap, const char *src)
{
    uint32_t i = 0;
    if (!cap)
        return;
    while (i + 1u < cap && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

/* ---- input helpers ---------------------------------------------------------- */
static int sr_clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}
static void sr_say(songs_ui_t *u, const char *msg, uint32_t ms)
{
    sr_copy(u->msg, sizeof u->msg, msg);
    u->msg_until = ms + SONGS_TOAST_MS;
}

/* ============================================================== the state ==== */

void ui_songs_refresh(songs_ui_t *u)
{
    int i, n = 0;
    if (!u->ops || !u->ops->list)
        return;
    for (i = 0; i < (int)SONGS_SLOTS; i++) {
        u->ops->list(u->ops->ctx, i, &u->rows[i]);
        if (u->rows[i].used)
            n++;
    }
    u->nused = n;
}

static int sr_current_slot(const songs_ui_t *u)
{
    int i;
    if (u->ops && u->ops->current_slot)
        return u->ops->current_slot(u->ops->ctx);
    for (i = 0; i < (int)SONGS_SLOTS; i++)
        if (u->rows[i].current)
            return i;
    return -1;
}

void ui_songs_begin(songs_ui_t *u, const songs_ops_t *ops)
{
    int cur;
    sr_copy(u->msg, sizeof u->msg, "");
    u->ops = ops;
    u->open = 1;
    u->screen = SO_LIST;
    u->slot = 0;
    u->row = 0;
    u->tmpl = 1;                       /* Starter, as the design highlights */
    u->key = (uint8_t)SONGS_KEY_DEFAULT;
    u->bpm_x10 = SONGS_BPM_DEFAULT;
    u->del_held = u->del_done = 0;
    u->del_t0 = 0;
    u->home_used = 0;
    u->prev_btn = u->prev_keys = 0;
    u->msg[0] = 0;
    u->msg_until = 0;
    ui_songs_refresh(u);
    cur = sr_current_slot(u);
    u->row = (uint8_t)(cur >= 0 && cur < (int)SONGS_SLOTS ? 1 + cur : 0);
}

void ui_songs_end(songs_ui_t *u)
{
    u->open = 0;
}

int ui_songs_active(const songs_ui_t *u)
{
    return u->open != 0;
}

/* Enter SO_NEW, seeded from the current screen. */
static void sr_open_new(songs_ui_t *u)
{
    u->screen = SO_NEW;
    u->row = (uint8_t)u->tmpl;
}

/* the menu song's clip count (one flash read when the menu opens, not per frame) */
static int sr_clip_count(const songs_ui_t *u, int slot);

static void sr_create(songs_ui_t *u, int *action, uint32_t ms)
{
    int ns;
    if (u->ops->first_free && u->ops->first_free(u->ops->ctx) < 0) {
        sr_say(u, "5 SONGS MAX: DELETE ONE", ms);
        return;
    }
    if (u->row == 2) {                 /* Copy current */
        int cur = sr_current_slot(u);
        if (cur < 0)                   /* nothing to copy: the row is inert */
            return;
        ns = u->ops->copy ? u->ops->copy(u->ops->ctx, cur) : -1;
    } else {
        int kind = (u->row == 1) ? 1 : 0;      /* 0 Empty, 1 Starter */
        ns = u->ops->create_new ? u->ops->create_new(u->ops->ctx, kind, u->bpm_x10, (int)u->key) : -1;
    }
    if (ns == -2) {
        sr_say(u, "5 SONGS MAX: DELETE ONE", ms);
        return;
    }
    if (ns < 0) {
        sr_say(u, "SAVE FAILED", ms);
        return;
    }
    ui_songs_refresh(u);
    if (u->ops->open && u->ops->open(u->ops->ctx, ns) == 0)
        *action = SONG_ACTION_OPENED;
    else
        sr_say(u, "SAVE FAILED", ms);
}

int ui_songs_input(songs_ui_t *u, const song_event_t *ev)
{
    uint32_t down = ev->btn & ~u->prev_btn;
    uint32_t up = u->prev_btn & ~ev->btn;
    int sel = (int)ev->enc[EN_SELECT];
    int action = SONG_ACTION_NONE;

    if (u->msg[0] && (int32_t)(ev->ms - u->msg_until) >= 0)
        u->msg[0] = 0;

    if (u->screen == SO_RENAME) {      /* the name editor owns the frame */
        int r = name_edit_input(&u->name, ev);
        if (r == NAME_DONE) {
            char buf[SONGS_NAME];
            int rc;
            name_edit_result(&u->name, buf);
            rc = u->ops->rename ? u->ops->rename(u->ops->ctx, u->slot, buf) : -1;
            if (rc == 0) {
                ui_songs_refresh(u);
                u->screen = SO_MENU;
                u->row = 2;            /* stay on Rename */
                sr_say(u, "RENAMED", ev->ms);
            } else {
                sr_say(u, "SAVE FAILED", ev->ms);   /* leave the editor open to retry */
            }
        } else if (r == NAME_CANCELLED) {
            u->screen = SO_MENU;
            u->row = 2;
        }
        u->prev_btn = ev->btn;
        u->prev_keys = ev->keys;
        return action;
    }

    /* HOME + REC: undo a delete (SONGS owns the one-level delete undo; clip undo is cleared
     * when the song changes, so outside SONGS it never reaches a delete). */
    if ((down & (1u << B_REC)) && (ev->btn & (1u << B_HOME))) {
        int can = u->ops->can_undelete ? u->ops->can_undelete(u->ops->ctx) : 0;
        int rc = can ? (u->ops->undelete ? u->ops->undelete(u->ops->ctx) : 0) : 0;
        u->home_used = 1;              /* the release must not go back */
        ui_songs_refresh(u);
        sr_say(u, rc == 1 ? "RESTORED" : "CANNOT UNDO", ev->ms);
        u->prev_btn = ev->btn;
        u->prev_keys = ev->keys;
        return action;
    }
    /* HOME + PLAY: nothing here (no redo of a delete), and the HOME release must not go back */
    if ((down & (1u << B_PLAY)) && (ev->btn & (1u << B_HOME))) {
        u->home_used = 1;
        u->prev_btn = ev->btn;
        u->prev_keys = ev->keys;
        return action;
    }

    switch (u->screen) {
    case SO_LIST:
        if (sel)
            u->row = (uint8_t)sr_clamp(u->row + (sel > 0 ? 1 : -1), 0, (int)SONGS_SLOTS);
        if (down & (1u << B_SEL)) {
            if (u->row == 0) {
                sr_open_new(u);
            } else {
                int slot = u->row - 1;
                if (u->rows[slot].used) {
                    int rc = u->ops->open ? u->ops->open(u->ops->ctx, slot) : -1;
                    if (rc == 0)
                        action = SONG_ACTION_OPENED;
                    else
                        sr_say(u, rc == -2 ? "SAVE FAILED" : "OPEN FAILED", ev->ms);
                } else {
                    sr_say(u, "EMPTY SLOT", ev->ms);
                }
            }
        }
        if (down & (1u << B_EDIT)) {
            if (u->row > 0 && u->rows[u->row - 1].used) {
                u->slot = (uint8_t)(u->row - 1);
                u->screen = SO_MENU;
                u->row = 0;
                u->menu_clips = sr_clip_count(u, u->slot);   /* cache: no flash read per draw */
            } else if (u->row == 0) {
                sr_open_new(u);
            }
        }
        if ((up & (1u << B_HOME)) && !u->home_used)
            action = SONG_ACTION_CLOSE;
        break;

    case SO_NEW:
        if (sel)
            u->row = (uint8_t)sr_clamp(u->row + (sel > 0 ? 1 : -1), 0, 2);
        if (ev->enc[EN_K1])
            u->bpm_x10 = sr_bpm((uint16_t)sr_clamp((int)u->bpm_x10 + (int)ev->enc[EN_K1] * 10,
                                                           (int)SONGS_BPM_MIN, (int)SONGS_BPM_MAX));
        if (ev->enc[EN_K2])
            u->key = (uint8_t)(((int)u->key + (ev->enc[EN_K2] > 0 ? 1 : -1) + (int)SONGS_NKEYS) %
                               (int)SONGS_NKEYS);
        u->tmpl = u->row;
        if (down & (1u << B_SEL))
            sr_create(u, &action, ev->ms);
        if ((up & (1u << B_HOME)) && !u->home_used)
            u->screen = SO_LIST;
        break;

    case SO_MENU: {
        int held = (int)((ev->btn >> B_SEL) & 1u);
        if (u->row == 3) {             /* Delete: hold SEL for 1.5 s */
            if (down & (1u << B_SEL)) {
                u->del_held = 1;
                u->del_done = 0;
                u->del_t0 = ev->ms;
            }
            if (u->del_held && !u->del_done && (ev->ms - u->del_t0) >= SONGS_DELETE_MS) {
                int rc = u->ops->del ? u->ops->del(u->ops->ctx, u->slot) : -1;
                u->del_held = 0;
                if (rc == 0) {
                    u->del_done = 1;
                    ui_songs_refresh(u);
                    u->screen = SO_LIST;
                    u->row = (uint8_t)sr_clamp(1 + u->slot, 0, (int)SONGS_SLOTS);
                    sr_say(u, "DELETED", ev->ms);
                } else {
                    sr_say(u, "SAVE FAILED", ev->ms);   /* leave the menu unchanged */
                }
            } else if (u->del_held && !held) {
                u->del_held = 0;       /* released early: keep the song */
            }
        } else {
            if (sel)
                u->row = (uint8_t)sr_clamp(u->row + (sel > 0 ? 1 : -1), 0, 3);
            if (down & (1u << B_SEL)) {
                switch (u->row) {
                case 0:
                    if (u->ops->open && u->ops->open(u->ops->ctx, u->slot) == 0)
                        action = SONG_ACTION_OPENED;
                    else
                        sr_say(u, "SAVE FAILED", ev->ms);
                    break;
                case 1: {
                    int ns = -1;
                    if (u->ops->first_free && u->ops->first_free(u->ops->ctx) < 0) {
                        sr_say(u, "5 SONGS MAX: DELETE ONE", ev->ms);
                        break;
                    }
                    if (u->ops->copy)
                        ns = u->ops->copy(u->ops->ctx, u->slot);
                    if (ns == -2)
                        sr_say(u, "5 SONGS MAX: DELETE ONE", ev->ms);
                    else if (ns < 0)
                        sr_say(u, "SAVE FAILED", ev->ms);
                    else {
                        ui_songs_refresh(u);
                        u->screen = SO_LIST;
                        u->row = (uint8_t)(1 + ns);
                        sr_say(u, "COPIED", ev->ms);
                    }
                    break;
                }
                case 2:
                    name_edit_begin(&u->name, u->rows[u->slot].name);
                    u->name.prev_btn = ev->btn;      /* the SEL that opened it is not a confirm */
                    u->name.prev_keys = ev->keys;
                    u->screen = SO_RENAME;
                    break;
                default:
                    break;             /* row 3 is the hold, above */
                }
            }
        }
        if ((up & (1u << B_HOME)) && !u->home_used) {
            u->del_held = 0;
            u->screen = SO_LIST;
        }
        break;
    }
    default:
        break;
    }

    if (up & (1u << B_HOME))
        u->home_used = 0;
    u->prev_btn = ev->btn;
    u->prev_keys = ev->keys;
    return action;
}

const char *ui_songs_toast(const songs_ui_t *u)
{
    return u->msg[0] ? u->msg : 0;
}

void ui_songs_leds(const songs_ui_t *u, uint32_t *btn, uint32_t *key, uint32_t *dim)
{
    (void)key;
    (void)dim;
    if (u->open && btn)
        *btn |= 1u << B_SAVE;          /* the way in, lit while the list is up */
}

/* ============================================================== the drawing == */

static int32_t sr_off;                 /* the band's screen-row offset (0 or 144) */
#define SY(v) ((v) - sr_off)

/* every y goes through SY: the screen is drawn in two canvas bands (0..143 then
 * 144..239), each blitted on its own, so a band's rows are the screen rows minus
 * its offset */
static int32_t sr_text(int32_t x, int32_t y, const aafont_t *f, const char *s, uint16_t c)
{
    return cv_text(x, SY(y), f, s, c);
}

static int sr_isqrt(int v)
{
    int s = 0;
    while ((s + 1) * (s + 1) <= v)
        s++;
    return s;
}
static int32_t sr_vc(const aafont_t *f, int32_t y0, int32_t h)
{
    int ch, top;
    if (f == &AF_L) {
        ch = AF_L_CAP_B - AF_L_CAP_Y + 1;
        top = AF_L_CAP_Y;
    } else if (f == &AF_M) {
        ch = AF_M_CAP_B - AF_M_CAP_Y + 1;
        top = AF_M_CAP_Y;
    } else {
        ch = AF_S_CAP_B - AF_S_CAP_Y + 1;
        top = AF_S_CAP_Y;
    }
    return y0 + (h - ch + 1) / 2 - top;
}
static void sr_text_r(int32_t rx, int32_t y, const aafont_t *f, const char *s, uint16_t c)
{
    sr_text(rx - text_w(f, s), y, f, s, c);
}
static void sr_text_c(int32_t cx, int32_t y, const aafont_t *f, const char *s, uint16_t c)
{
    sr_text(cx - text_w(f, s) / 2, y, f, s, c);
}
static void sr_rrect(int x, int y, int w, int h, int r, uint16_t c)
{
    int i;
    if (r <= 0) {
        cv_rect(x, SY(y), w, h, c);
        return;
    }
    if (2 * r > h)
        r = h / 2;
    if (2 * r > w)
        r = w / 2;
    for (i = 0; i < h; i++) {
        int dy = i < r ? r - i : i >= h - r ? i - (h - r) + 1 : 0;
        int inset = dy ? r - sr_isqrt(r * r - dy * dy) : 0;
        cv_rect(x + inset, SY(y + i), w - 2 * inset, 1, c);
    }
}
static void sr_frame(int x, int y, int w, int h, uint16_t c)
{
    cv_rect(x, SY(y), w, 1, c);
    cv_rect(x, SY(y + h - 1), w, 1, c);
    cv_rect(x, SY(y), 1, h, c);
    cv_rect(x + w - 1, SY(y), 1, h, c);
}
static int sr_chip(int x, int y, const char *t)
{
    int w = (int)text_w(&AF_S, t) + 8;
    sr_rrect(x, y, w, 16, 4, TOK_KEY);
    sr_text(x + 4, sr_vc(&AF_S, y, 16), &AF_S, t, TOK_INK);
    return w;
}
static int sr_hint(int x, int y, const char *key, const char *label)
{
    x += sr_chip(x, y, key) + 3;
    x = sr_text(x, sr_vc(&AF_S, y, 16), &AF_S, label, TOK_MID) + 10;
    return x;
}
static void sr_battery(int x, int y)
{
    cv_rect(x, SY(y), 16, 1, TOK_MID);
    cv_rect(x, SY(y + 9), 16, 1, TOK_MID);
    cv_rect(x, SY(y), 1, 10, TOK_MID);
    cv_rect(x + 15, SY(y), 1, 10, TOK_MID);
    cv_rect(x + 16, SY(y + 3), 2, 4, TOK_MID);
    cv_rect(x + 2, SY(y + 2), 10, 6, TOK_THEME);
}
static void sr_header(const songs_ui_t *u)
{
    static const char *const LAYER[4] = {"SONGS", "NEW SONG", "SONGS", "NAME"};
    int w = sr_chip(5, 4, "STUDIO");
    sr_text(5 + w + 6, sr_vc(&AF_S, 4, 16), &AF_S, LAYER[u->screen], TOK_MID);
    sr_battery(217, 7);
}
/* a dashed border, as the + New song row is drawn in the design */
static void sr_dash_rect(int x, int y, int w, int h, uint16_t c)
{
    int i;
    for (i = 0; i < w; i += 6) {
        int n = i + 4 <= w ? 4 : 1;
        cv_rect(x + i, SY(y), n, 1, c);
        cv_rect(x + i, SY(y + h - 1), n, 1, c);
    }
    for (i = 0; i < h; i += 6) {
        int n = i + 4 <= h ? 4 : 1;
        cv_rect(x, SY(y + i), 1, n, c);
        cv_rect(x + w - 1, SY(y + i), 1, n, c);
    }
}
static void sr_row(int y, const char *name, const char *meta, int sel, int on, int dim, int dashed)
{
    uint16_t nc = dim ? TOK_DIM : on ? TOK_THEME : TOK_TEXT;
    uint16_t mc = dim ? TOK_DIM : TOK_MID;
    if (dashed) {
        sr_dash_rect(4, y, 232, 16, TOK_DIM);
        nc = dim ? TOK_DIM : TOK_THEME;
    } else {
        sr_rrect(4, y, 232, 16, 4, sel ? TOK_RAISE : TOK_SURF);
    }
    if (sel)
        cv_rect(4, SY(y), 2, 16, TOK_THEME);
    sr_text(12, sr_vc(&AF_S, y, 16), &AF_S, name, nc);
    if (meta[0])
        sr_text_r(232, sr_vc(&AF_S, y, 16), &AF_S, meta, mc);
}
static void sr_list_rows(const songs_ui_t *u, int dim, int cursor)
{
    int i;
    for (i = 0; i < (int)SONGS_ROWS; i++) {
        int y = 26 + i * 19;
        const char *name, *meta;
        int on = 0, sel = 0;
        if (i == 0) {
            name = "+ New song";
            meta = "template";
            sel = cursor && u->row == 0;
            sr_row(y, name, meta, sel, 0, dim, 1);
            continue;
        }
        {
            const song_row_t *r = &u->rows[i - 1];
            name = r->used && r->name[0] ? r->name : r->used ? "SONG" : "empty";
            meta = !r->used ? "" : r->current ? "open now" : r->has_take ? "1 take" : "no take";
            on = r->current;
            sel = cursor && u->row == i;
        }
        sr_row(y, name, meta, sel, on, dim, 0);
    }
}
static void sr_meter(const songs_ui_t *u)
{
    char t[12], *q;
    int barw = 176;
    sr_rrect(8, 160, barw, 6, 3, TOK_LINE);
    if (u->nused)
        sr_rrect(8, 160, barw * u->nused / (int)SONGS_SLOTS, 6, 3, TOK_SEL);
    q = sr_itoa(t, u->nused);
    q = sr_cat(q, " OF 5");
    (void)q;
    sr_text_r(232, sr_vc(&AF_S, 156, 14), &AF_S, t, TOK_MID);
}

static const uint16_t SR_TICK[5] = {TOK_DRUM, TOK_BASS, TOK_LEAD, TOK_CHORD, TOK_FX};
static void sr_new_card(int y, const char *title, const char *sub, const int *bright, int sel, int dim)
{
    uint16_t bg = dim ? TOK_BG : sel ? TOK_SEL : TOK_SURF;
    uint16_t tc = dim ? TOK_DIM : sel ? TOK_INK : TOK_THEME;
    uint16_t sc = dim ? TOK_DIM : sel ? TOK_INK : TOK_MID;
    int i;
    sr_rrect(4, y, 232, 36, 5, bg);
    if (dim)
        sr_frame(4, y, 232, 36, TOK_LINE);
    sr_text(10, sr_vc(&AF_M, y + 3, 18), &AF_M, title, tc);
    sr_text_r(230, sr_vc(&AF_S, y + 5, 14), &AF_S, sub, sc);
    for (i = 0; i < 5; i++)
        cv_rect(10 + i * 45, SY(y + 26), 41, 4, (bright[i] && !dim) ? SR_TICK[i] : TOK_RAISE);
}
static void sr_kv(int x, int y, const char *lbl, const char *val)
{
    sr_rrect(x, y, 114, 38, 5, TOK_SURF);
    sr_text(x + 8, sr_vc(&AF_S, y + 3, 14), &AF_S, lbl, TOK_MID);
    sr_text(x + 8, sr_vc(&AF_M, y + 17, 18), &AF_M, val, TOK_THEME);
}
static void sr_new_screen(const songs_ui_t *u)
{
    static const int B0[5] = {0, 0, 0, 0, 0}, B1[5] = {1, 1, 0, 0, 0}, B2[5] = {1, 1, 1, 1, 1};
    char bpm[8], *q;
    int cur = sr_current_slot(u);
    int can_copy = (cur >= 0 && cur < (int)SONGS_SLOTS);
    const char *copyname = can_copy ? u->rows[cur].name : "NO SONG";
    sr_new_card(26, "Empty", "5 roles, no clips", B0, u->row == 0, 0);
    sr_new_card(65, "Starter", "beat + bass ready", B1, u->row == 1, 0);
    sr_new_card(104, "Copy current", copyname, B2, u->row == 2, !can_copy);
    q = sr_itoa(bpm, (int)u->bpm_x10 / 10);
    (void)q;
    sr_kv(4, 150, "K1 tempo", bpm);
    sr_kv(122, 150, "K2 key", sr_key_name(u->key));
    sr_hint(5, 216, "SEL", "create");
    {
        int x = 120;
        sr_text(x, sr_vc(&AF_S, 216, 16), &AF_S, "current song is saved", TOK_MID);
    }
}

static int sr_clip_count(const songs_ui_t *u, int slot)
{
    if (u->ops->clip_count)
        return u->ops->clip_count(u->ops->ctx, slot);
    return -1;
}
static void sr_menu_meta(char *out, uint32_t cap, const songs_ui_t *u)
{
    const song_row_t *r = &u->rows[u->slot];
    char *q = out;
    int n = u->menu_clips;                 /* cached when the menu opened: no flash read per draw */
    q = sr_itoa(q, n < 0 ? 0 : n);
    q = sr_cat(q, " clips ");
    q = sr_cat(q, r->has_take ? "1 take" : "0 takes");
    (void)cap;
}
static void sr_menu(const songs_ui_t *u)
{
    static const char *const ROW[4] = {"Open", "Copy", "Rename", "Delete"};
    static const char *const RH[4] = {"SEL", "AS A COPY", ">", "HOLD"};
    char meta[24];
    const song_row_t *r = &u->rows[u->slot];
    int i;
    sr_rrect(10, 40, 220, 170, 6, TOK_SURF);
    sr_frame(10, 40, 220, 170, TOK_LINE);
    sr_text(18, sr_vc(&AF_M, 46, 18), &AF_M, r->name[0] ? r->name : "SONG", TOK_THEME);
    sr_menu_meta(meta, sizeof meta, u);
    sr_text_r(222, sr_vc(&AF_S, 48, 14), &AF_S, meta, TOK_MID);
    for (i = 0; i < 4; i++) {
        int y = 78 + i * 28, sel = (u->row == i);
        uint16_t tc = i == 3 ? TOK_REC : TOK_TEXT;
        sr_rrect(16, y, 208, 24, 4, sel ? TOK_RAISE : TOK_BG);
        sr_text(24, sr_vc(&AF_M, y, 24), &AF_M, ROW[i], tc);
        sr_text_r(216, sr_vc(&AF_S, y, 24), &AF_S, RH[i], i == 3 ? TOK_MID : TOK_MID);
    }
}
static void sr_ring(int cx, int cy, int r, int pct, uint16_t track, uint16_t fill)
{
    int off;
    for (off = -2; off <= 2; off++) {      /* a 5 px stroke, as the design draws it */
        int rr = r + off;
        int px = 0, py = -rr, i;
        if (rr <= 0)
            continue;
        for (i = 1; i <= 72; i++) {
            int nx = (px * 4080 - py * 357) >> 12;    /* cos 5 deg, sin 5 deg in Q12 */
            int ny = (px * 357 + py * 4080) >> 12;
            int d = sr_isqrt(nx * nx + ny * ny);
            if (d > 0) {
                nx = nx * rr / d;
                ny = ny * rr / d;
            }
            cv_line(cx + px, SY(cy + py), cx + nx, SY(cy + ny), (i * 100 / 72) <= pct ? fill : track);
            px = nx;
            py = ny;
        }
    }
}
static void sr_delete(const songs_ui_t *u, uint32_t ms)
{
    const song_row_t *r = &u->rows[u->slot];
    char meta[24];
    int pct = 0;
    cv_rect(0, SY(30), 240, 184, TOK_BG);   /* blank the rows and the menu behind the dialog */
    sr_rrect(14, 34, 212, 140, 6, TOK_SURF);
    sr_frame(14, 34, 212, 140, TOK_LINE);
    if (u->del_done)
        pct = 100;
    else if (u->del_held && ms - u->del_t0 < SONGS_DELETE_MS)
        pct = (int)((ms - u->del_t0) * 100u / SONGS_DELETE_MS);
    sr_ring(54, 104, 26, pct, TOK_LINE, TOK_REC);
    sr_text_c(54, sr_vc(&AF_S, 98, 14), &AF_S, "SEL", TOK_MID);
    sr_text(96, sr_vc(&AF_S, 46, 14), &AF_S, "DELETE SONG", TOK_REC);
    sr_text(96, sr_vc(&AF_M, 64, 18), &AF_M, r->name[0] ? r->name : "SONG", TOK_THEME);
    sr_menu_meta(meta, sizeof meta, u);
    sr_text(96, sr_vc(&AF_S, 86, 14), &AF_S, meta, TOK_MID);
    sr_text_c(120, sr_vc(&AF_S, 146, 14), &AF_S, "UNDO BRINGS IT BACK", TOK_MID);
    sr_text_c(120, sr_vc(&AF_S, 160, 14), &AF_S, "UNTIL THE NEXT DELETE", TOK_MID);
}
static void sr_footer_list(void)
{
    int x = 5;
    x = sr_hint(x, 216, "SEL", "open");
    x = sr_hint(x, 216, "EDIT", "menu");
    sr_hint(x, 216, "HOME", "back");
}
static void sr_toast(const songs_ui_t *u, uint32_t ms)
{
    int w, x;
    if (!u->msg[0] || (int32_t)(ms - u->msg_until) >= 0)
        return;
    w = (int)text_w(&AF_M, u->msg) + 16;
    x = (240 - w) / 2;
    sr_rrect(x, 116, w, 24, 6, TOK_ACCENT);
    sr_text_c(120, sr_vc(&AF_M, 116, 24), &AF_M, u->msg, TOK_INK);
}
static void sr_scene(const songs_ui_t *u, uint32_t ms)
{
    sr_header(u);
    switch (u->screen) {
    case SO_LIST:
        sr_list_rows(u, 0, 1);
        sr_meter(u);
        sr_footer_list();
        break;
    case SO_NEW:
        sr_new_screen(u);
        break;
    case SO_MENU:
        sr_list_rows(u, 1, 0);
        sr_menu(u);
        if (u->del_held || u->del_done) {
            sr_delete(u, ms);
            {
                int x = 5;
                x = sr_hint(x, 216, "RELEASE", "keep");
                sr_hint(x, 216, "HOME+REC", "undo");
            }
        } else {
            int x = 5;
            x = sr_hint(x, 216, "SELECT", "row");
            x = sr_hint(x, 216, "SEL", "do it");
            sr_hint(x, 216, "HOME", "close");
        }
        break;
    default:
        break;
    }
    sr_toast(u, ms);
}

void ui_songs_draw(const songs_ui_t *u, uint32_t ms)
{
    if (u->screen == SO_RENAME) {
        name_edit_draw(&u->name, ms);
        return;
    }
    sr_off = 0;
    cv_begin(240, 144, TOK_BG);
    sr_scene(u, ms);
    cv_blit(0, 0);
    sr_off = 144;
    cv_begin(240, 96, TOK_BG);
    sr_scene(u, ms);
    cv_blit(0, 144);
}
