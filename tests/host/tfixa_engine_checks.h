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
