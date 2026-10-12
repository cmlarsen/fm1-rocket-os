/* SPDX-License-Identifier: GPL-3.0-only */
/* Host-only assertions inspect real DSP gates and the production queue. */
int engine_test_tfixa(void)
{
    int fails = 0, p, i;
    for (p = 0; p < NBASS; p++) {
        cq_w = cq_r = 0;
        live_bridge_play_key((uint8_t)(p ? LIVE_LEAD : LIVE_BASS), 60, 1, 100);
        live_bridge_play_key((uint8_t)(p ? LIVE_LEAD : LIVE_BASS), 64, 1, 100);
        live_bridge_play_key((uint8_t)(p ? LIVE_LEAD : LIVE_BASS), 60, 0, 0);
        drain(0);
        if (!b303[p].gate) fails++;
        live_bridge_play_key((uint8_t)(p ? LIVE_LEAD : LIVE_BASS), 64, 0, 0);
        drain(0);
        if (b303[p].gate) fails++;
        {
            live_ctx_t ctx = {0, 0, 0, 0, 0};
            live_ev_t ev = {0, LIVE_EV_NOTE_ON, (uint8_t)(p ? LIVE_LEAD : LIVE_BASS), 60, 100, 0};
            live_apply(&ev, &ctx);
            ev.a = 64; live_apply(&ev, &ctx);
            ev.kind = LIVE_EV_NOTE_OFF; ev.a = 60; live_apply(&ev, &ctx);
            if (!b303[p].gate) fails++;
            ev.a = 64; live_apply(&ev, &ctx);
            if (b303[p].gate) fails++;
        }
    }
    cq_w = cq_r = 0;
    engine_note_on(PART_FM6, 60, 100); drain(0);
    for (i = 0; i < 1100; i++) engine_set(T_MIX, 0, MX_LEVEL, 100);
    engine_note_off(PART_FM6, 60); drain(0);
    if (eng_fm6_voices_held()) fails++;
    return fails;
}

#include <stdio.h>
#include <string.h>
#define song_t live_song_t
#include "core/model/model.h"
#undef song_t
static FILE *phase_log;
static uint32_t phase_base;
static void phase_tap(uint32_t sample, uint32_t cmd)
{
    uint32_t type = cmd >> 28;
    if ((type == 9u || type == 10u) && sample - phase_base < 88000u)
        fprintf(phase_log, "%u %08x\n", sample - phase_base, cmd);
}
static void phase_run_to(uint32_t end)
{
    int32_t out[512];
    while (engine_sample_now() < end) {
        uint32_t n = end - engine_sample_now();
        engine_render(out, n > 256 ? 256 : n);
    }
}
int engine_test_tfixa_phase(const char *dir)
{
    char path[512];
    int i;
    live_song_t *song = (live_song_t *)live_bridge_song();
    memset(song, 0, sizeof *song);
    song->bpm_x10 = 1200;
    song->clip[12].used = 1; song->clip[12].len = 32; song->clip[12].nnotes = 32;
    for (i = 0; i < 32; i++)
        song->clip[12].d.notes[i] = (note_t){(uint8_t)i, (uint8_t)(40+i), 100, 1};
    live_bridge_song_reload(); live_bridge_take_init(); live_bridge_enter();
    eng_prof_frames = 0;
    live_bridge_transport_start(); phase_run_to(12000);
    live_bridge_launch_now(12); phase_run_to(17000);
    live_bridge_take_arm(); phase_run_to(88200);
    snprintf(path, sizeof path, "%s/phase_record.txt", dir);
    phase_log = fopen(path, "w"); if (!phase_log) return 1;
    phase_base = 88200; engine_tap = phase_tap;
    phase_run_to(176200);
    fclose(phase_log); engine_tap = 0;
    live_bridge_take_finish(); live_bridge_take_play();
    snprintf(path, sizeof path, "%s/phase_replay.txt", dir);
    phase_log = fopen(path, "w"); if (!phase_log) return 1;
    phase_base = 176200; engine_tap = phase_tap;
    phase_run_to(264200);
    fclose(phase_log); engine_tap = 0;
    return 0;
}
