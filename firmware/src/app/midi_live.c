/* SPDX-License-Identifier: GPL-3.0-only */
/* T11i: the app's MIDI-in glue; see midi_live.h.
 *
 * One midi_in_t (core/midi/midi_in.c, T11c) is fed from the shared TRS/USB ring in the MAIN LOOP,
 * because everything it drives — the LIVE play path, the clip engine's launch/transport/tempo —
 * is a main-loop API (X0X's command queue is produced only from the main loop; see engine.c's
 * `in_isr` trap). The clock-lost check runs in the same call, on the same sample counter, so the
 * byte timestamps line up with it.
 *
 * Routing is by the song: notes on `song.live_channel` (0 = omni) play the focused role through
 * the LIVE bridge, Program Change (when `song.pc_launch`) launches that clip quantized, CC 120 /
 * 123 release everything, and 24-ppqn clock (Internal/Auto, a GLOBAL row) follows the tempo and
 * drives Start / Continue / Stop. In Classic X0X's own per-part handling owns the ring instead
 * (live_bridge_active() is 0), so this unit does nothing there.
 *
 * The two `song_t`s (X0X's and model.h's) collide, so this unit speaks model.h's only: it includes
 * midi_in.h (which includes model.h) and declares the live-bridge/engine entry points it needs,
 * rather than including live_bridge.h/engine.h. */
#include "midi_live.h"
#include "core/midi/midi_in.h"     /* midi_in_t + midi_cfg_from_song (model.h's song_t) */

/* The app it feeds. */
extern int  plat_midi_in(uint32_t *pkt);                     /* the TRS/USB ring (plat.h) */
extern void *live_bridge_song(void);                         /* live_bridge.h */
extern int  live_bridge_active(void);
extern void live_bridge_play_key(uint8_t role, uint8_t note, int down, uint8_t vel);
extern void live_bridge_transport_start(void);
extern void live_bridge_transport_stop(void);
extern void live_bridge_set_bpm(uint16_t bpm_x10);
extern void live_bridge_launch_quant(uint8_t clip, uint8_t quant);   /* T11i (additive, live_bridge.c) */
extern void engine_all_off(void);                            /* engine.h */
extern int  live_focus(void);                                /* ui_live.c: the PLAY layer's focus */

#define ML_TEMPO_STEP 5u        /* bpm_x10: send the tempo only on a real change (0.5 bpm) */

static midi_in_t g_in;
static uint8_t   g_ready;
static uint8_t   g_clk_internal;    /* the GLOBAL row: 1 = Internal (ignore the incoming clock) */
static uint8_t   g_ext;             /* following an external clock (the EXT chip) */
static uint16_t  g_sent_bpm;        /* the tempo last handed to the clip engine (0 = none) */
static uint8_t   mh[128];           /* note -> the role it is held on, + 1; 0 = not held */
static uint8_t   g_nheld;

/* ----------------------------------------------------------------- the sink --- */

static void ml_note_on(void *x, uint8_t note, uint8_t vel)
{
    int role = live_focus();
    uint8_t prev = mh[note];
    (void)x;
    if (role < 0)
        role = 0;
    if (prev && prev != (uint8_t)(role + 1))
        live_bridge_play_key((uint8_t)(prev - 1u), note, 0, 0);   /* the focus moved: free the old role */
    if (!mh[note] && g_nheld < 255u)
        g_nheld++;
    mh[note] = (uint8_t)(role + 1);                 /* the release finds its role even if focus moved */
    live_bridge_play_key((uint8_t)role, note, 1, vel);
}

static void ml_note_off(void *x, uint8_t note)
{
    uint8_t r = mh[note];
    (void)x;
    if (!r)
        return;
    mh[note] = 0;
    if (g_nheld)
        g_nheld--;
    live_bridge_play_key((uint8_t)(r - 1u), note, 0, 0);
}

static void ml_all_off(void *x) { (void)x; midi_live_note_all_off(); engine_all_off(); }
static void ml_launch(void *x, uint8_t clip, uint8_t quant)
{
    (void)x;
    if (clip < NCLIPS)
        live_bridge_launch_quant(clip, quant);
}
static void ml_start(void *x) { (void)x; live_bridge_transport_start(); }
static void ml_cont(void *x)  { (void)x; live_bridge_transport_start(); }   /* resume where it is */
static void ml_stop(void *x)  { (void)x; live_bridge_transport_stop(); }
static void ml_lost(void *x)  { (void)x; g_ext = 0; }

static const midi_sink_t SINK = {
    0, ml_note_on, ml_note_off, ml_all_off, ml_launch, ml_start, ml_cont, ml_stop, 0, ml_lost
};

/* --------------------------------------------------------------- the api --- */

static void ml_ensure(void)
{
    if (g_ready)
        return;
    midi_in_init(&g_in, 0, &SINK);
    g_ready = 1;
}

static void ml_sync_cfg(void)
{
    const song_t *s = (const song_t *)live_bridge_song();
    midi_cfg_t c;
    if (s) {
        midi_cfg_from_song(&c, s);
    } else {
        c.live_channel = 0u;
        c.pc_launch = 0u;
        c.pc_quant = Q_BAR;
        c.cable = 0u;
    }
    midi_in_set_cfg(&g_in, &c);
}

void midi_live_feed(uint32_t pkt, uint32_t sample)
{
    ml_ensure();
    if (g_clk_internal && ((pkt >> 8) & 0xFFu) >= 0xF0u)
        return;                                     /* Internal: the clock/transport are not ours */
    midi_in_packet(&g_in, pkt, sample);
}

void midi_live_tick(uint32_t sample)
{
    uint16_t bpm;
    uint32_t d;
    ml_ensure();
    midi_in_tick(&g_in, sample);                    /* fires clock_lost -> ml_lost clears g_ext */
    if (!midi_in_clock_ok(&g_in)) {
        g_ext = 0;
        return;
    }
    g_ext = (uint8_t)!g_clk_internal;
    bpm = midi_in_bpm_x10(&g_in);
    d = bpm > g_sent_bpm ? (uint32_t)(bpm - g_sent_bpm) : (uint32_t)(g_sent_bpm - bpm);
    if (bpm && (!g_sent_bpm || d >= ML_TEMPO_STEP)) {
        live_bridge_set_bpm(bpm);                   /* the clip engine follows only while clocked */
        g_sent_bpm = bpm;
    }
}

int midi_live_poll(uint32_t sample)
{
    uint32_t pkt;
    int n = 0;
    if (!live_bridge_active()) {                    /* Classic: engine.c owns the ring and X0X's handling */
        if (g_nheld)
            midi_live_reset();
        return 0;
    }
    ml_ensure();
    ml_sync_cfg();
    while (n < 256 && plat_midi_in(&pkt)) {
        midi_live_feed(pkt, sample);
        n++;
    }
    midi_live_tick(sample);
    return n;
}

void midi_live_set_song_cfg(uint8_t live_channel, uint8_t pc_launch)
{
    song_t *s = (song_t *)live_bridge_song();
    if (!s)
        return;
    s->live_channel = live_channel > 16u ? 0u : live_channel;
    s->pc_launch = pc_launch ? 1u : 0u;
}

void midi_live_set_clock(int internal) { g_clk_internal = (uint8_t)(internal != 0); }

void midi_live_note_all_off(void)
{
    int note;
    for (note = 0; note < 128; note++) {
        if (!mh[note])
            continue;
        live_bridge_play_key((uint8_t)(mh[note] - 1u), (uint8_t)note, 0, 0);
        mh[note] = 0;
    }
    g_nheld = 0;
}

void midi_live_reset(void)
{
    int note;
    for (note = 0; note < 128; note++)
        mh[note] = 0;
    g_nheld = 0;
}

int midi_live_ext(void) { return g_ext; }
uint16_t midi_live_bpm_x10(void) { return midi_in_bpm_x10(&g_in); }
int midi_live_held(void) { return g_nheld; }
