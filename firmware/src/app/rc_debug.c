/* SPDX-License-Identifier: GPL-3.0-only */
/* RC1p: the debug-only stress and stats for measuring CPU headroom on the device.
 *
 * Include-style on purpose (like ui_live.c): it is #included by `editor.c` inside `#if X0X_DEBUG`
 * and by `host/x0x_host.c`, so the device's ED_STRESS / ED_STATS and the runner's `stress` /
 * `stats` commands run the same code. It lives in the app's translation unit because it drives the
 * LIVE path the way the UI does (live entry, clip launches, the FM6 PLAY-layer keys) and reads the
 * platform's own counters (plat_fm1.c's audio_peak_pct / audio_xruns).
 *
 * Nothing here is in a release build: `editor.c` only includes it under X0X_DEBUG, and the host
 * always keeps it. It never touches flash and never writes the project: the busy content is its
 * own `rc_pat` (the .pool on the device), so a stress run cannot make the project dirty or autosave.
 *
 * The FM6 voice cap is the debug build's only lie: `tools/build.py` compiles a X0X_DEBUG=1 build
 * with -DENG_FM6_VOICES=4 (the same as release; T9i r1 cut it from 6 once the 8-voice measurement
 * was done), and a level's voice count is how many notes the CHORDS chord holds. Level 2 holds
 * exactly 4, so it measures what a release build would play. */
#include "rc_debug.h"
#if defined(X0X_HOST) || (defined(X0X_DEBUG) && X0X_DEBUG)
#include "plat.h"
#include "engine.h"
#include "live_bridge.h"
#include "../seq/pattern.h"
#include "../seq/tb3po.h"
#include "../seq/sequencer.h"
#include "../dsp/drum909.h"          /* DR_* track order */
#include "../dsp/drum808.h"          /* D8_* track order */
#include "../dsp/perform.h"          /* T10i: PERF_* (stress level 3) */

/* the FM6 adapter's live voice count (eng_fm6.c; not in engine.h) */
extern int eng_fm6_voices_active(void);

#ifndef X0X_HOST
extern uint32_t _bss_start[], _bss_end[], _pool_start[], _pool_end[];
#endif

/* engine.h's PR_* order is part of the ED_STATS layout (rc_debug.h's RC_ST_PROF0) */
typedef char rc_prof_n_fixed[(ENG_PROF_N == 9) ? 1 : -1];

/* ------------------------------------------------------------------ content --- */

/* The generated song lives here, never in proj: the LIVE bridge copies it into its own song and
 * the X0X baseline points the sequencer at it. T10i: one pattern, not NPAT — live_bridge_map_n()
 * maps just it, so the debug build need not carry a full pattern array in the pool. T9i r1: this
 * debug-only buffer lives in .bss (not the pool), which frees the pool's 8 KiB headroom for the
 * take after the FM6 array dropped to 4 voices. */
static pattern_t rc_pat;

/* The 909 / 808 track orders come from drum909.h / drum808.h (DR_* / D8_*, included above). */

/* A dense, fixed pattern (same seed every run: repeated runs measure the same thing). 909/808 on
 * 16ths with accents, both 303s a busy TB-3PO line with slides. */
static void rc_build_pattern(pattern_t *p)
{
    int i;
    pattern_init(p, 0x52433170u, 0x0C1B00D5u);

    {   /* 909 */
        dpart_t *a = &p->drum[0];
        a->len = 16;
        a->rate = RATE_16;
        for (i = 0; i < 16; i++) {
            if ((i & 3) == 0) sm_set(a->hit[DR_BD], i);          /* BD on the beat */
            if ((i & 3) == 2) sm_set(a->hit[DR_SD], i);          /* SD off-beat */
            sm_set(a->hit[DR_CH], i);                            /* closed hats, 16ths */
            if ((i & 3) == 2) sm_set(a->hit[DR_OH], i);          /* open hats */
            if ((i & 3) == 0) sm_set(a->hit[DR_CP], i);          /* clap */
            if (i == 15) sm_set(a->hit[DR_CR], i);               /* crash */
        }
        sm_set(a->accent, 0);
        sm_set(a->accent, 8);
    }
    {   /* 808 */
        dpart_t *b = &p->drum[1];
        b->len = 16;
        b->rate = RATE_16;
        for (i = 0; i < 16; i++) {
            if ((i & 3) == 0) sm_set(b->hit[D8_BD], i);
            if ((i & 3) == 2) sm_set(b->hit[D8_SD], i);
            if ((i & 1) == 0) sm_set(b->hit[D8_CH], i);
            if ((i & 3) == 2) sm_set(b->hit[D8_OH], i);
            if ((i & 7) == 0) sm_set(b->hit[D8_CB], i);
        }
    }
    tb3po_generate(&p->bass[0]);
    tb3po_generate(&p->bass[1]);
    {   /* a few slides, as a busy acid line has (TB-3PO also rolls its own) */
        int b;
        for (b = 0; b < NBASS; b++)
            for (i = 0; i < 16; i++)
                if ((i & 3) == 3 && bstep_gate(&p->bass[b].step[i]) == G_NOTE)
                    p->bass[b].step[i].flags |= BS_SLIDE;
    }
}

/* ------------------------------------------------------------------ LIVE --- */

/* The stress chord: up to six notes (the debug array's cap, T9i). Level 2 holds the first four
 * (the release cap). */
static const uint8_t RC_CHORD[6] = {48, 52, 55, 59, 62, 65};

/* Enter LIVE with the stress patterns: ui_live.c's live_enter, but the bridge maps rc_pat, not the
 * project. The UI switches to V_LIVE so the LEDs, the header and autosave behave as in LIVE. */
static void rc_live_enter(void)
{
    uint16_t bpm = (uint16_t)(seq.bpm * 10.0f + 0.5f);
    live.back = ui.view;
    live.layer = LIVE_PADS;
    live.focus = LIVE_BASS;
    live.octv = 0;
    note_release_all();
    live_bridge_rebuild_n(&rc_pat, 1);     /* T7a/T10i: map the one stress pattern (no auto-remap) */
    live_bridge_enter(&rc_pat, bpm ? bpm : 1200u);
    set_view(V_LIVE);
    say("LIVE", 0);
}

/* Launch the first used clip on every role except CHORDS (played live as a chord). Every clip is
 * quantized to a bar, exactly as a pad tap is. */
static void rc_launch_all(void)
{
    int r;
    for (r = 0; r < LIVE_NROLES; r++) {
        int first, n, j;
        if (r == LIVE_CHORDS)
            continue;
        first = live_bridge_role_first(r);
        n = live_bridge_role_nclips(r);
        for (j = 0; j < n; j++)
            if (first >= 0 && live_bridge_clip_used((uint8_t)(first + j))) {
                live_bridge_launch((uint8_t)(first + j));
                break;
            }
    }
}

/* Hold a k-note FM6 chord on the PLAY layer, the way a user holds keys (live_bridge_play_key is
 * the one call ui_live.c's PLAY layer makes). The voices sustain until LIVE exits. */
static void rc_hold_chord(int k)
{
    int i;
    live.layer = LIVE_PLAY;
    live.focus = LIVE_CHORDS;
    for (i = 0; i < k && i < 8; i++)
        live_bridge_play_key(LIVE_CHORDS, RC_CHORD[i], 1, 88);
}

/* ------------------------------------------------------------------ state --- */

static int rc_level;
static int rc_have_seq;
static uint8_t rc_sav_view;
static pattern_t *rc_sav_pat;
static uint8_t rc_sav_ppat[NPARTS], rc_sav_cue[NPARTS];
static uint32_t rc_sav_mute;
static int rc_sav_song, rc_sav_ca, rc_sav_cb;
static float rc_sav_bpm;

int rc_stress_level(void) { return rc_level; }
int rc_stress_active(void) { return rc_level != 0; }

/* Let a few audio blocks pass so the engine sees a transport change. Entering LIVE in the same
 * main-loop tick as stopping X0X would let engine.c's "X0X stopped" all-off (the render after
 * mot_playing falls) release the chord the new level just held. */
static void rc_settle(void)
{
#ifdef X0X_HOST
    extern void rc_host_pump(uint32_t ms);
    rc_host_pump(30);
#else
    fm1_delay_ms(30);
#endif
}

static void rc_save(void)
{
    int p;
    rc_sav_view = (uint8_t)ui.view;
    rc_sav_pat = seq.pat;
    for (p = 0; p < NPARTS; p++) {
        rc_sav_ppat[p] = seq.ppat[p];
        rc_sav_cue[p] = seq.pcue[p];
    }
    rc_sav_mute = seq.mute;
    rc_sav_song = seq.song_on;
    rc_sav_ca = seq.chain_a;
    rc_sav_cb = seq.chain_b;
    rc_sav_bpm = seq.bpm;
    rc_have_seq = 1;
}

static void rc_restore_seq(void)
{
    int p;
    if (!rc_have_seq)
        return;
    seq.pat = rc_sav_pat;
    for (p = 0; p < NPARTS; p++) {
        seq.ppat[p] = rc_sav_ppat[p];
        seq.pcue[p] = rc_sav_cue[p];
    }
    seq.mute = rc_sav_mute;
    seq.song_on = (uint8_t)rc_sav_song;
    seq_chain(&seq, rc_sav_ca, rc_sav_cb);
    seq.bpm = rc_sav_bpm;
    rc_have_seq = 0;
}

/* Stop whatever the last level started and put the sequencer and the view back. */
static void rc_stop(void)
{
    if (!rc_level)
        return;
    if (live_bridge_active()) {
        live_bridge_exit();
        note_release_all();
        set_view(rc_sav_view == V_LIVE ? V_HOME : rc_sav_view);
        rc_settle();                    /* T5perf: let the exit land, as the sequencer stop below does */
    } else if (seq.playing || seq.req_start || seq.req_stop) {
        seq_stop(&seq);
        rc_settle();                    /* let the stop land before the next level starts */
    }
    engine_guard_enable(1);
    engine_core2_force_off(0);          /* T5perf: core 1 is available again after the run */
    engine_perform_reset();             /* T10i: release every punch FX the run left on */
    rc_restore_seq();
    rc_level = 0;
}

int rc_stress(int n, int cap)
{
    int single = 0, base = n;
    rc_stop();
    if (n <= 0)
        return 0;
    /* T5perf: levels 5/6 are levels 2/1 with core 1 forced off (the single-core numbers a unit that
     * cannot run the second core would see). */
    if (n == 5) {
        single = 1;
        base = 2;
    } else if (n == 6) {
        single = 1;
        base = 1;
    } else if (n > 4)
        return -1;
    rc_save();
    rc_build_pattern(&rc_pat);
    rc_level = n;
    engine_core2_force_off(single);

    if (base == 1) {                    /* the X0X baseline: all four parts, the sequencer's own path */
        int p;
        if (seq.playing)
            seq_stop(&seq);
        seq.pat = &rc_pat;
        for (p = 0; p < NPARTS; p++) {
            seq.ppat[p] = 0;
            seq.pcue[p] = 0xFF;
        }
        seq.mute = 0;
        seq.song_on = 0;
        seq_chain(&seq, 0, 0);
        seq.bpm = 125.0f;
        engine_guard_enable(1);         /* the guard is part of what RC1 measures */
        seq_start(&seq);
        return 0;
    }

    /* base = 2, 3 or 4: LIVE. Level 4 measures the unguarded cap (the guard would shed FM6 to two
     * voices, which is not what "the cap" means); 2 and 3 keep the guard on. Level 3 adds every
     * wired punch FX (T10i): it engages all six in turn, a few blocks each, and leaves the three
     * that stack (freeze + filter dive + echo throw) running for the measurement. */
    engine_guard_enable(base == 4 ? 0 : 1);
    rc_live_enter();
    live_bridge_transport_start();
    rc_launch_all();
    {
        int voices = (base == 4) ? (cap > 0 ? cap : 6) : 4;
        if (voices > 6)
            voices = 6;                              /* T9i: the debug array holds six */
        rc_hold_chord(voices);
    }
    if (base == 3) {
        static const int FX[6] = {PERF_REPEAT_8, PERF_REPEAT_16, PERF_TAPE, PERF_FILTER,
                                  PERF_ECHO, PERF_FREEZE};
        int i;
        engine_perform_reset();
        for (i = 0; i < 6; i++) {
            engine_perform_engage(FX[i], 1);    /* latch: the effect stays through the measurement */
            rc_settle();                        /* let a few blocks run under it before the next */
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ stats --- */

#if defined(X0X_HOST)
static uint32_t rc_peak(void) { return cpu_peak; }          /* x0x_host.c's scripted peak */
static uint32_t rc_xruns(void) { return 0; }
#else
static uint32_t rc_peak(void) { return audio_peak_pct; }    /* plat_fm1.c's counters, read in place */
static uint32_t rc_xruns(void) { return audio_xruns; }
#endif

void rc_stats_reset(void)
{
    engine_core1_stats_reset();          /* T5perf: the core-1 busy mean is per run */
#if defined(X0X_HOST)
    cpu_peak = 0;
#else
    audio_peak_pct = 0;
    audio_xruns = 0;
#endif
}

int rc_stats(uint32_t *out, int max)
{
    int n = 0, k;
#define RC_PUT(v) do { if (n < max) out[n] = (uint32_t)(v); n++; } while (0)
    RC_PUT(plat_cpu_pct());
    RC_PUT(rc_peak());
    RC_PUT(rc_xruns());
    RC_PUT(engine_guard_active());
    RC_PUT(eng_guard_count);
    RC_PUT(engine_core2_forced_off() ? 0u : (uint32_t)plat_cpu2_on());
    RC_PUT(plat_cpu2_gave_up());
    RC_PUT((uint32_t)eng_fm6_voices_active());
    RC_PUT(engine_core1_pct());          /* T5perf: core 1's own busy share (invisible in cpu when unwaited) */
    for (k = 0; k < ENG_PROF_N; k++)
        RC_PUT(eng_prof[k]);
#ifndef X0X_HOST
    RC_PUT((uint32_t)((const char *)_bss_end - (const char *)_bss_start));
    RC_PUT((uint32_t)((const char *)_pool_end - (const char *)_pool_start));
    RC_PUT(0x54000u);                       /* the .pool region's size (app.ld) */
#else
    RC_PUT(0);
    RC_PUT(0);
    RC_PUT(0);
#endif
    RC_PUT(plat_irq_stack_used());
    RC_PUT((uint32_t)live_bridge_take_drops());   /* T9i r2: the take player's drop counter */
#undef RC_PUT
    return n;
}

#endif /* X0X_HOST || X0X_DEBUG */
