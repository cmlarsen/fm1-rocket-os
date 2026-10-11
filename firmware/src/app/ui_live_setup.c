/* SPDX-License-Identifier: GPL-3.0-only */
/* TLSET: include-style LIVE list, using X0X's existing canvas and main-loop paths. */
static uint8_t setup_open, setup_row, assign_open;
static int8_t assign_key = -1;
static uint8_t assign_target;

static void live_setup_close(void)
{
    live_mapped_release_all();
    note_release_all();
    setup_open = assign_open = 0;
    assign_key = -1;
    take_open = 0;
}

static void live_setup_enter(void)
{
    live_mapped_release_all();
    note_release_all();
    live_fx_reset_ui();
    setup_open = 1;
    setup_row = 0;
    assign_open = 0;
    assign_key = -1;
    take_open = 0;
}

static int live_setup_button(int b)
{
    if (!setup_open)
        return 0;
    if (b == B_EDIT) {
        live_setup_close();
    } else if (b == B_HOME) {
        live_mapped_release_all();
    note_release_all();
        if (take_open) take_open = 0;
        else if (assign_open) { assign_open = 0; assign_key = -1; }
        else live_setup_close();
    } else if (b == B_SEL && !take_open) {
        if (setup_row == 0) {
            live_mapped_release_all();
    note_release_all();
            assign_open = 1;
        } else if (setup_row == 6) {
            live_mapped_release_all();
    note_release_all();
            take_open = 1;
            if (!song_io_take_pending()) song_io_take_load_ram();
        }
    } else if (b == B_PLAY || b == B_REC || (b == B_SEL && take_open)) {
        return 0;                        /* the take and transport keep T9i's handlers */
    }
    return 1;
}

static void live_setup_value(int row, char *out)
{
    static const char *const quant[] = {"Next bar", "Next beat", "End of clip"};
    switch (row) {
    case 0: put_s(out, "hold a key >"); break;
    case 1: put_s(out, quant[song_io_setup_get(SONG_SETUP_QUANT)]); break;
    case 2: put_s(out, song_io_setup_get(SONG_SETUP_LOCK) ? "On" : "Off"); break;
    case 3: {
        int ch = song_io_setup_get(SONG_SETUP_CHANNEL);
        if (!ch) put_s(out, "Omni");
        else put_i(put_s(out, "Ch "), ch);
        break;
    }
    case 4: put_s(out, song_io_setup_get(SONG_SETUP_PC) ? "On" : "Off"); break;
    case 5: put_s(out, proj.set.rsv[1] ? "Internal" : "Auto"); break;
    default:
        if (live_bridge_take_has()) live_len_str(out, live_bridge_take_length_samples());
        else put_s(out, "no take >");
        break;
    }
}

static void live_setup_input(void)
{
    int e = (int)enc(EN_SELECT);
    if (take_open) return;
    if (assign_open) {
        e = (int)enc(EN_PRESET);
        if (assign_key >= 0 && e) {
            int v = (int)assign_target + e;
            assign_target = (uint8_t)((v % LIVE_ASSIGN_COUNT + LIVE_ASSIGN_COUNT) % LIVE_ASSIGN_COUNT);
        }
        return;
    }
    if (e) {
        int v = (int)setup_row + e;
        setup_row = (uint8_t)(v < 0 ? 0 : v > 6 ? 6 : v);
    }
    e = (int)enc(EN_ALGO);
    if (e && setup_row >= 1 && setup_row <= 5) {
        if (setup_row == 5) {
            int v = (int)proj.set.rsv[1] + e;
            v = v < 0 ? 0 : v > 1 ? 1 : v;
            if (v != (int)proj.set.rsv[1]) {
                pref_set(PR(R_CLKIN, 0, 0, 0), v);
                mark_dirty();
            }
        } else {
            int setting = setup_row - 1;
            int max = setting == SONG_SETUP_CHANNEL ? 16 : setting == SONG_SETUP_QUANT ? 2 : 1;
            int v = song_io_setup_get(setting) + e;
            if (setting == SONG_SETUP_CHANNEL && v != song_io_setup_get(setting)) note_release_all();
            song_io_setup_set(setting, v < 0 ? 0 : v > max ? max : v);
        }
    }
}

static void live_assign_label(int target, char *out)
{
    if (target < (int)LIVE_NCLIPS) {
        int r = live_bridge_role_of(target);
        char *p = put_s(out, LV_ROLE_L[r]);
        *p++ = ' ';
        *p++ = (char)('A' + target - live_bridge_role_first(r));
        *p = 0;
    } else if (target < LIVE_ASSIGN_FX) {
        put_s(put_s(out, "Stop "), LV_ROLE_L[target - LIVE_ASSIGN_STOP]);
    } else if (target < LIVE_ASSIGN_NOTE) {
        put_s(out, LV_FX_NAME[target - LIVE_ASSIGN_FX]);
    } else put_s(out, "Live note");
}

static void live_key_name(int k, char *out)
{
    static const char *const names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    int note = 53 + k;
    put_i(put_s(out, names[note % 12]), note / 12 - 1);
}

static int live_setup_key(int k, int down)
{
    if (!setup_open) return 0;
    if (!assign_open || take_open) return 1;
    if (down && assign_key < 0) {
        assign_key = (int8_t)k;
        assign_target = (uint8_t)live_bridge_key_clip(k);
        if (assign_target >= LIVE_ASSIGN_COUNT) assign_target = 0;
    } else if (!down && k == assign_key) {
        char key[8], target[24], msg[40], *p;
        song_io_set_key_clip(k, assign_target);
        live_key_name(k, key);
        live_assign_label(assign_target, target);
        p = put_s(msg, key);
        p = put_s(p, " -> ");
        put_s(p, target);
        say(msg, 0);
        assign_key = -1;
    }
    return 1;
}

static void live_setup_draw(int band)
{
    static const char *const names[] = {"Assign keys", "Launch on", "Stage lock", "External keys", "Program change", "Clock in", "Take"};
    int i;
    if (assign_open) {
        char title[32];
        if (assign_key < 0) put_s(title, "Hold a key to assign");
        else {
            char key[8];
            live_key_name(assign_key, key);
            put_s(put_s(put_s(title, "Key "), key), " plays");
        }
        cv_text(10, vc(&AF_M, 4 - band * BAND_H, 18), &AF_M, title, TOK_TEXT);
        for (i = 0; i < 5; i++) {
            char label[24];
            int target = ((int)assign_target + i - 2 + LIVE_ASSIGN_COUNT) % LIVE_ASSIGN_COUNT;
            int y = 28 + i * 21 - band * BAND_H;
            if (y >= BAND_H || y + 20 <= 0) continue;
            box(4, y, 232, 19, i == 2 ? TOK_THEME : TOK_SURF);
            live_assign_label(target, label);
            cv_text(10, vc(&AF_S, y, 19), &AF_S, label, i == 2 ? TOK_INK : TOK_TEXT);
        }
        return;
    }
    for (i = 0; i < 7; i++) {
        char value[20];
        int y = 2 + i * 19 - band * BAND_H;
        if (y >= BAND_H || y + 18 <= 0) continue;
        live_setup_value(i, value);
        box(4, y, 232, 18, i == setup_row ? TOK_THEME : TOK_SURF);
        cv_text(8, vc(&AF_S, y, 18), &AF_S, names[i], i == setup_row ? TOK_INK : TOK_TEXT);
        text_r(232, vc(&AF_S, y, 18), &AF_S, value, i == setup_row ? TOK_INK : TOK_MID);
    }
}
