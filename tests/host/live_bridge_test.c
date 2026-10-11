/* SPDX-License-Identifier: GPL-3.0-only */
/* T3 unit test: the pure X0X-project -> LIVE-song mapping in live_bridge.c, the BS_SLIDE carry,
 * the 64-step (second hit word) and 7-pattern cap, and the bridge's event stream.
 *
 * The bridge sees both X0X's sequencer.h and core/model/model.h, which both name `song_t`;
 * compile this the same way live_bridge.c does: engine.h first, then the model under a rename.
 * engine.h's main-loop command functions are stubbed below (the bridge only uses them for the
 * PLAY layer and for enter/exit; the clip render now emits plain events, captured here). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "firmware/src/app/engine.h"

#define song_t live_song_t
#include "core/model/model.h"
#undef song_t

#include "firmware/src/app/live_bridge.h"
#include "core/seq/take.h"                 /* T9i r1: the take command words, for the race tests */

/* T9i r1: the ISR-side traps the tests arm (defined in clip_engine.c / live_bridge.c under X0X_HOST). */
extern volatile int clip_engine_isr;
extern volatile int live_bridge_isr;
/* T9i r2: the preemption injectors (live_bridge.c's take_play/take_stage, take.c's take_end) and
 * the invariant probe (live_bridge.c). */
extern void (*live_bridge_test_hook)(void);
extern void (*take_test_hook)(void);
extern int live_bridge_test_consistent(void);
/* T9i r1: a mutable sample clock, so the race tests can arm at one sample and record at another. */
static uint32_t g_now;

/* ---- stubs the bridge links against ---- */
seq_t seq;
void engine_drum(int kit, int v, float vel) { (void)kit; (void)v; (void)vel; }
void engine_bass_on(int p, int n, int a, int s) { (void)p; (void)n; (void)a; (void)s; }
void engine_bass_off(int p) { (void)p; }
void engine_note_on(int part, int note, int vel) { (void)part; (void)note; (void)vel; }
void engine_note_off(int part, int note) { (void)part; (void)note; }
void engine_set(int t, int v, int i, int val) { (void)t; (void)v; (void)i; (void)val; }
void engine_all_off(void) {}
/* T9i: the take hooks the bridge links against */
void (*engine_tap)(uint32_t sample, uint32_t cmd);
void engine_take_cmd(uint32_t cmd) { (void)cmd; }
uint32_t engine_sample_now(void) { return g_now; }
const sound_t *engine_sound_mirror(void) { return 0; }
void engine_perform_engage(int fx, int latch) { (void)fx; (void)latch; }
void engine_perform_release(int fx) { (void)fx; }
void engine_perform_reset(void) {}
int engine_perform_latched(int fx) { (void)fx; return 0; }
int engine_perform_active(int fx) { (void)fx; return 0; }
void engine_take_perform(int fx, int how) { (void)fx; (void)how; }
void engine_take_perform_reset(void) {}
int engine_nvoices(int t) { (void)t; return 1; }
int engine_nparams(int t, int v) { (void)t; (void)v; return 0; }
/* T9i r2: model the main-loop critical section with the host's nest-safe plat_crit (host/
 * plat_crit_host.c), so a test hook can ask whether a real IRQ could preempt a point. */
extern uint32_t plat_crit_enter(void);
extern void plat_crit_exit(uint32_t prev);
extern int plat_crit_active(void);
uint32_t engine_crit_enter(void) { return plat_crit_enter(); }
void engine_crit_exit(uint32_t s) { plat_crit_exit(s); }

static int fails;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

static pattern_t pats[NPAT];
static live_song_t sg;

static int count_used(int lo, int n)
{
    int i, k = 0;
    for (i = 0; i < n; i++)
        if (sg.clip[lo + i].used)
            k++;
    return k;
}

/* ---- the bridge's event stream, captured (non-stub) ---- */
static live_ev_t cap[512];
static int ncap;
static void cap_ev(void *ctx, const live_ev_t *e)
{
    (void)ctx;
    if (ncap < (int)(sizeof cap / sizeof cap[0]))
        cap[ncap++] = *e;
}

/* ---- T9i r2: the preemption model --------------------------------------- *
 * A test arms the hook; a main-loop take function (take_play / take_stage / take_end) calls it at a
 * race point. If the host critical section is held, a real IRQ could not have preempted there, so
 * the hook does nothing — which is exactly what the fix relies on. Otherwise it runs one audio
 * block's ISR half (the take pump + the clip render) at the injected base, or a single ISR feed. */
static volatile int hook_armed;
static int hook_mode;                             /* 0 = block, 1 = one take_feed */
static uint32_t hook_base, hook_n;

static void run_isr_block(uint32_t base, uint32_t n)
{
    live_sink_t sink = {cap_ev, 0};
    live_bridge_take_block(base, n);
    live_bridge_render(n, &sink);
}
static void test_hook(void)
{
    if (!hook_armed)
        return;
    hook_armed = 0;                               /* one shot */
    if (plat_crit_active())
        return;                                   /* masked: the IRQ could not run here */
    if (hook_mode == 1)
        live_bridge_take_feed(hook_base, TAKE_FX(0u, TAKE_FX_ON));
    else
        run_isr_block(hook_base, hook_n);
}

/* ---- T9i r1: the ISR-race tests ----------------------------------------- *
 * B1: the take playback sink runs in the audio ISR. It must call no main-loop API: no clip
 * command-ring push (a trap in clip_engine.c's push_cmd) and no take-intent post (a trap in
 * live_bridge.c's tp_post). The test stages as the main loop does, then calls the ISR block.
 * On the reviewed code the sink called live_bridge_launch_now, which pushed the ring and read
 * song->clip[]; the trap aborts this test. */
static void b1_isr_sink_test(void)
{
    uint32_t *ev;
    live_bridge_take_init();
    live_bridge_enter();                                /* TCLS: rebuild seeds the clips in main */                     /* LIVE is active */
    g_now = 0;
    ev = (uint32_t *)live_bridge_take_events_mut();     /* a stored take: a launch now, then end */
    ev[0] = 0u;
    ev[1] = TAKE_LAUNCH(0u, TAKE_LAUNCH_NOW, LIVE_DRUMS);
    ev[2] = 2000u;
    ev[3] = TAKE_MARK_END;
    live_bridge_take_loaded(2, 2000u, 1200u);
    live_bridge_take_play();
    live_bridge_take_stage();                           /* the main-loop half */
    CHECK(live_bridge_take_playing());
    clip_engine_isr = 1;                                /* now the preempting audio ISR */
    live_bridge_isr = 1;
    live_bridge_take_block(0u, 128u);                   /* the sink runs; any ring push traps */
    clip_engine_isr = 0;
    live_bridge_isr = 0;
    CHECK(1);                                           /* no trap: the sink touched no main-loop ring */
    CHECK(live_bridge_queued_clip(LIVE_DRUMS) == 0);    /* the launch landed ISR-side (clip 0) */
    live_bridge_take_stop_play();
    live_bridge_exit();
}

/* B2: leaving LIVE during a recording finalizes the take at once (no bar wait) and stops the feed,
 * so the recording is neither lost nor carried into STUDIO. On the reviewed code the exit only
 * *requested* a stop at a bar that never came, so the take stayed in REC. */
static void b2_leave_test(void)
{
    int n0;
    live_bridge_take_init();
    live_bridge_enter();                                /* TCLS: rebuild seeds the clips in main */
    g_now = 0;
    live_bridge_transport_start();                      /* playing = 1 */
    live_bridge_take_arm();                             /* arm at the next bar */
    CHECK(live_bridge_take_armed());
    g_now = 200000u;                                    /* past the bar: now REC */
    live_bridge_take_feed(g_now, TAKE_LAUNCH(0u, TAKE_LAUNCH_QUEUE, LIVE_DRUMS));
    CHECK(live_bridge_take_recording());
    live_bridge_exit();                                 /* leave LIVE */
    CHECK(!live_bridge_take_recording());               /* finalized at once ... */
    CHECK(!live_bridge_take_armed());
    CHECK(live_bridge_take_has());                      /* ... with the recording kept */
    n0 = live_bridge_take_nevents();
    live_bridge_take_feed(engine_sample_now(), TAKE_LAUNCH(1u, TAKE_LAUNCH_QUEUE, LIVE_DRUMS));
    CHECK(live_bridge_take_nevents() == n0);            /* outside LIVE: nothing records */
}

/* ---- T9i r2: the race tests that fail on fdbfa34 ------------------------ */

/* B1: the take_play window. The prelude's offset-0 NOW launches must be staged before PLAY is
 * published. The hook fires the ISR at that point; on the fixed code it is masked (the staging ran
 * inside the same critical section) and the launch applies from a valid copy. On fdbfa34 the ISR
 * fired with next_set=0, apply_role set cur without swapping play[], and the invariant broke. */
static void r2_b1_play_race_test(void)
{
    uint32_t *ev;
    live_bridge_take_init();
    live_bridge_enter();                                /* TCLS: rebuild seeds the clips in main */
    g_now = 0;
    /* Prime play[DRUMS] with a DIFFERENT clip (2), so a corrupt apply of clip 0 is detectable by
     * the invariant probe (take_play's reset clears cur[] but not play[]). */
    live_bridge_transport_start();
    live_bridge_launch_now(2);
    live_bridge_render(12000u, 0);
    CHECK(live_bridge_playing_clip(LIVE_DRUMS) == 2);

    ev = (uint32_t *)live_bridge_take_events_mut();
    ev[0] = 0u;      ev[1] = TAKE_LAUNCH(0u, TAKE_LAUNCH_NOW, LIVE_DRUMS);
    ev[2] = 400000u; ev[3] = TAKE_MARK_END;
    live_bridge_take_loaded(2, 800000u, 1200u);
    hook_mode = 0; hook_base = 0; hook_n = 6000; hook_armed = 1;
    live_bridge_take_play();                     /* the hook fires here on fdbfa34 */
    hook_armed = 0;
    run_isr_block(0, 6000);                      /* the first block; on fdbfa34 the second */
    CHECK(live_bridge_playing_clip(LIVE_DRUMS) == 0);
    CHECK(live_bridge_test_consistent());
    live_bridge_take_stop_play();
    live_bridge_exit();
}

/* B2: the recorded tempo must hold through the replay. The take runs at 120, changes to 140 at
 * sample 200; editing the song's tempo mid-replay (to 160) must not move it. At the stop the song's
 * tempo is restored. On fdbfa34 clip_engine_take_bpm only set bpm_src, which the render's song-tempo
 * check reset to 120 on the next block. */
static void r2_b2_tempo_test(void)
{
    uint32_t *ev;
    live_bridge_take_init();
    live_bridge_enter();                                /* TCLS: rebuild seeds the clips in main */
    g_now = 0;
    ev = (uint32_t *)live_bridge_take_events_mut();
    ev[0] = 0u;      ev[1] = TAKE_MARK_TEMPO(1200u);
    ev[2] = 0u;      ev[3] = TAKE_LAUNCH(0u, TAKE_LAUNCH_NOW, LIVE_DRUMS);
    ev[4] = 200u;    ev[5] = TAKE_MARK_TEMPO(1400u);
    ev[6] = 200u;    ev[7] = TAKE_LAUNCH(2u, TAKE_LAUNCH_NOW, LIVE_DRUMS);
    ev[8] = 800000u; ev[9] = TAKE_MARK_END;
    live_bridge_take_loaded(5, 1600000u, 1200u);
    live_bridge_take_play();
    run_isr_block(0, 128);
    CHECK(live_bridge_bpm_x10() == 1200u);       /* the prelude's tempo */
    ((live_song_t *)live_bridge_song())->bpm_x10 = 1600u;   /* an edit while the take plays */
    live_bridge_take_stage();
    run_isr_block(128, 128);
    CHECK(live_bridge_bpm_x10() == 1400u);       /* the recorded change, not the edit */
    run_isr_block(256, 6000);
    CHECK(live_bridge_bpm_x10() == 1400u);       /* it holds */
    CHECK(live_bridge_playing_clip(LIVE_DRUMS) == 2);
    CHECK(live_bridge_test_consistent());
    live_bridge_take_stop_play();
    CHECK(live_bridge_bpm_x10() == 1600u);       /* the song's tempo comes back */
    live_bridge_exit();
}

/* S3: A queued to the bar then NOW-B in the same role. The main loop stages B once its event is
 * inside the lookahead window; B overrides A before A's boundary, so B must play from B's own copy.
 * On fdbfa34 take_stage never staged B (it saw A queued and re-staged A), the ISR set cur=B without
 * the matching copy, and play[] was wrong. */
static void r2_s3_next_stage_test(void)
{
    uint32_t *ev;
    live_bridge_take_init();
    live_bridge_enter();                                /* TCLS: rebuild seeds the clips in main */
    g_now = 0;
    /* Prime the engine so play[DRUMS] is a known clip (0), which take_play's reset does not clear:
     * if B is applied without its own staged copy, play[] stays clip 0 and the invariant fails. */
    live_bridge_transport_start();
    live_bridge_launch_now(0);
    live_bridge_render(12000u, 0);
    CHECK(live_bridge_playing_clip(LIVE_DRUMS) == 0);

    ev = (uint32_t *)live_bridge_take_events_mut();
    ev[0] = 200u;    ev[1] = TAKE_LAUNCH(0u, TAKE_LAUNCH_QUEUE, LIVE_DRUMS);   /* A: bar */
    ev[2] = 2000u;   ev[3] = TAKE_LAUNCH(2u, TAKE_LAUNCH_NOW, LIVE_DRUMS);     /* B: now */
    ev[4] = 400000u; ev[5] = TAKE_MARK_END;
    live_bridge_take_loaded(3, 800000u, 1200u);
    live_bridge_take_play();                     /* resets cur; play[DRUMS] stays clip 0 */
    run_isr_block(0, 128);                       /* the transport starts; nothing due yet */
    run_isr_block(128, 128);                     /* A fires at sample 200: queued at the bar */
    CHECK(live_bridge_queued_clip(LIVE_DRUMS) == 0);
    run_isr_block(256, 800);                     /* play_pos = 1056 */
    live_bridge_take_stage();                    /* the UI frame: B (2000) is now in the window */
    run_isr_block(1056, 2000);                   /* B fires at 2000 (NOW -> the next step) */
    run_isr_block(3056, 6000);                   /* the next step applies it */
    CHECK(live_bridge_playing_clip(LIVE_DRUMS) == 2);
    CHECK(live_bridge_test_consistent());        /* cur[DRUMS] and play[DRUMS] both clip 2 */
    live_bridge_take_stop_play();
    live_bridge_exit();
}

/* S4: take_finish from the main loop must be masked. The hook runs an ISR take_feed at the exact
 * take_end window (after `ended`, before the END mark). On the fixed code the section is masked and
 * the feed does not run; on fdbfa34 it appended an event that take_end then overwrote, so n grew by
 * two rather than one. */
static void r2_s4_finish_race_test(void)
{
    int n0;
    live_bridge_take_init();
    live_bridge_enter();                                /* TCLS: rebuild seeds the clips in main */
    g_now = 0;
    live_bridge_transport_start();
    live_bridge_take_arm();
    g_now = 200000u;                             /* the arm's bar arrives: now REC */
    live_bridge_take_feed(g_now, TAKE_FX(0u, TAKE_FX_ON));
    CHECK(live_bridge_take_recording());
    n0 = live_bridge_take_nevents();
    hook_mode = 1; hook_base = g_now + 1000u; hook_armed = 1;
    live_bridge_take_finish();                   /* the hook would feed here on fdbfa34 */
    hook_armed = 0;
    CHECK(!live_bridge_take_recording());
    CHECK(live_bridge_take_nevents() == n0 + 1); /* exactly the END mark, once */
    {
        const uint32_t *ev = (const uint32_t *)live_bridge_take_events();
        int n = live_bridge_take_nevents();
        CHECK(ev[(n - 1) * 2 + 1] == TAKE_MARK_END);
    }
    live_bridge_exit();
}

/* ---- T9i r3: the late press (S1) and the stalled UI (S2) ----------------- */

/* Run one replay: A (bar-quantized) queued at sample 200, C (bar-quantized) pressed D samples after
 * A's boundary (88,200 at 120 bpm). The main loop stages once every `frame` samples, phase-shifted.
 * Returns whether A was heard, and reports the drops and the invariant. */
static int r3_late_run(uint32_t D, uint32_t frame, uint32_t phase, int *drops, int *cons)
{
    uint32_t *ev, s, next_ui;
    int d0, sawA = 0;
    live_bridge_take_init();
    live_bridge_enter();                                /* TCLS: rebuild seeds the clips in main */
    ((live_song_t *)live_bridge_song())->bpm_x10 = 1200u;
    g_now = 0;
    ev = (uint32_t *)live_bridge_take_events_mut();
    ev[0] = 0u;         ev[1] = TAKE_MARK_TEMPO(1200u);   /* pin 120 bpm, so the bar lands at 88,200 */
    ev[2] = 200u;       ev[3] = TAKE_LAUNCH_Q(0u, Q_BAR, LIVE_DRUMS);
    ev[4] = 88200u + D; ev[5] = TAKE_LAUNCH_Q(2u, Q_BAR, LIVE_DRUMS);
    ev[6] = 300000u;    ev[7] = TAKE_MARK_END;
    live_bridge_take_loaded(4, 400000u, 1200u);
    d0 = live_bridge_take_drops();
    live_bridge_take_play();
    next_ui = phase;
    for (s = 0; s < 200000u; s += 128u) {
        if (s >= next_ui) { live_bridge_take_stage(); next_ui += frame; }
        run_isr_block(s, 128u);
        if (s > 90000u && s < 180000u && live_bridge_playing_clip(LIVE_DRUMS) == 0)
            sawA = 1;
    }
    *drops = live_bridge_take_drops() - d0;
    *cons = live_bridge_test_consistent();
    live_bridge_take_stop_play();
    live_bridge_exit();
    return sawA;
}

/* S1: A queued to the bar, C pressed 11 ms after that bar. C's event must not steal A's staged copy
 * before A's boundary (the r2 code staged C up to 31 ms early and dropped A in 4/5 phases). */
static void r3_s1_late_press_test(void)
{
    uint32_t ph;
    for (ph = 0; ph < 705u; ph += 141u) {
        int drops = -1, cons = 0;
        int sawA = r3_late_run(500u, 705u, ph, &drops, &cons);
        CHECK(sawA);                                     /* A is not dropped (the S1 defect) */
        CHECK(drops == 0);                               /* and C still plays: zero drops */
        CHECK(cons);
    }
}

/* S2: a launch well separated from the previous one must survive a stalled UI. `frame` models a
 * 40 ms and a 100 ms frame; the r2 ~28 ms lookahead window was skipped outright, dropping it. */
static void r3_s2_stall_test(void)
{
    static const uint32_t frames[2] = { 1764u, 4410u };  /* 40 ms, 100 ms at 44.1 kHz */
    int fi;
    uint32_t ph;
    for (fi = 0; fi < 2; fi++)
        for (ph = 0; ph < frames[fi]; ph += frames[fi] / 4u) {
            int drops = -1, cons = 0;
            int sawA = r3_late_run(30000u, frames[fi], ph, &drops, &cons);
            CHECK(sawA && drops == 0 && cons);
        }
}

/* ---- T9i r4: the playing-role repro ------------------------------------- */

/* The r3_late_run scenario, but the role is ALREADY PLAYING at the take start: the prelude is a
 * TAKE_LAUNCH_NOW of clip 1 on drums, then A (clip 0, bar) at sample 200 and C (clip 2, bar) at
 * 88200 + D. With one staging slot the main loop can only stage C after A applies, so a C that
 * lands within a single UI frame of A's boundary (D = 500, 60 Hz) has no copy staged in time and
 * is dropped. The two-deep FIFO stages C before A's boundary, so no launch is dropped. */
static int r4_playing_run(uint32_t D, uint32_t frame, uint32_t phase, int *drops, int *cons)
{
    uint32_t *ev, s, next_ui;
    int d0, sawA = 0;
    live_bridge_take_init();
    live_bridge_enter();
    ((live_song_t *)live_bridge_song())->bpm_x10 = 1200u;
    g_now = 0;
    ev = (uint32_t *)live_bridge_take_events_mut();
    ev[0] = 0u;         ev[1] = TAKE_MARK_TEMPO(1200u);   /* pin 120 bpm, so the bar lands at 88,200 */
    ev[2] = 0u;         ev[3] = TAKE_LAUNCH(1u, TAKE_LAUNCH_NOW, LIVE_DRUMS);   /* the role plays */
    ev[4] = 200u;       ev[5] = TAKE_LAUNCH_Q(0u, Q_BAR, LIVE_DRUMS);            /* A: the bar */
    ev[6] = 88200u + D; ev[7] = TAKE_LAUNCH_Q(2u, Q_BAR, LIVE_DRUMS);            /* C: just after */
    ev[8] = 400000u;    ev[9] = TAKE_MARK_END;
    live_bridge_take_loaded(5, 800000u, 1200u);
    d0 = live_bridge_take_drops();
    live_bridge_take_play();
    next_ui = phase;
    for (s = 0; s < 200000u; s += 128u) {
        if (s >= next_ui) { live_bridge_take_stage(); next_ui += frame; }
        run_isr_block(s, 128u);
        if (s > 90000u && s < 180000u && live_bridge_playing_clip(LIVE_DRUMS) == 0)
            sawA = 1;
    }
    *drops = live_bridge_take_drops() - d0;
    *cons = live_bridge_test_consistent();
    live_bridge_take_stop_play();
    live_bridge_exit();
    return sawA;
}

/* The playing-role repro must survive every phase at 16/40/100 ms UI frames, for a C that lands
 * 500 and 3,000 samples after A's boundary (the r3 defect drops it at the short gap). */
static void r4_playing_role_test(void)
{
    static const uint32_t frames[3] = { 705u, 1764u, 4410u };   /* 16, 40, 100 ms at 44.1 kHz */
    static const uint32_t Ds[2] = { 500u, 3000u };
    int fi, di;
    uint32_t ph;
    for (fi = 0; fi < 3; fi++)
        for (di = 0; di < 2; di++)
            for (ph = 0; ph < frames[fi]; ph += frames[fi] / 4u) {
                int drops = -1, cons = 0;
                int sawA = r4_playing_run(Ds[di], frames[fi], ph, &drops, &cons);
                CHECK(sawA);                             /* A is heard ... */
                CHECK(drops == 0);                       /* ... and no launch was dropped */
                CHECK(cons);
            }
}

/* NIT: the take_stage injection point must be live (it runs between roles, outside the role's
 * critical section) — arm it and prove it fires. */
static void r3_stage_hook_test(void)
{
    uint32_t *ev;
    live_bridge_take_init();
    live_bridge_enter();                                /* TCLS: rebuild seeds the clips in main */
    ((live_song_t *)live_bridge_song())->bpm_x10 = 1200u;
    g_now = 0;
    ev = (uint32_t *)live_bridge_take_events_mut();
    ev[0] = 0u;      ev[1] = TAKE_MARK_TEMPO(1200u);
    ev[2] = 0u;      ev[3] = TAKE_LAUNCH(0u, TAKE_LAUNCH_NOW, LIVE_DRUMS);     /* the prelude */
    ev[4] = 40000u;  ev[5] = TAKE_LAUNCH(2u, TAKE_LAUNCH_QUEUE, LIVE_DRUMS);   /* a later launch */
    ev[6] = 400000u; ev[7] = TAKE_MARK_END;
    live_bridge_take_loaded(4, 800000u, 1200u);
    live_bridge_take_play();                     /* stages the prelude (masked: the hook is a no-op) */
    run_isr_block(0, 128u);                      /* the prelude NOW launch applies */
    CHECK(live_bridge_playing_clip(LIVE_DRUMS) == 0);
    hook_mode = 0; hook_base = 128u; hook_n = 128u; hook_armed = 1;
    live_bridge_take_stage();                    /* main loop: stages clip 2, then fires the hook */
    CHECK(!hook_armed);                          /* the injection actually ran (NIT) */
    CHECK(live_bridge_test_consistent());
    live_bridge_take_stop_play();
    live_bridge_exit();
}

/* T9i r4: while a take plays, its recorded tempo wins: an external clock the user follows and a UI
 * tempo edit are both ignored for the transport. At the stop the pre-take source (the followed
 * clock) is restored. (This reverses r3's "the clock wins".) */
static void r4_tempo_test(void)
{
    uint32_t *ev;
    live_bridge_take_init();
    live_bridge_enter();                                /* TCLS: rebuild seeds the clips in main */
    ((live_song_t *)live_bridge_song())->bpm_x10 = 1200u;
    g_now = 0;
    /* follow an external clock at 160 bpm before the take */
    live_bridge_set_bpm_soft(1600u);
    run_isr_block(0, 128);
    CHECK(live_bridge_bpm_x10() == 1600u);
    ev = (uint32_t *)live_bridge_take_events_mut();
    ev[0] = 0u;   ev[1] = TAKE_MARK_TEMPO(1200u);
    ev[2] = 200u; ev[3] = TAKE_MARK_TEMPO(1400u);        /* the recorded change */
    ev[4] = 400000u; ev[5] = TAKE_MARK_END;
    live_bridge_take_loaded(3, 800000u, 1200u);
    live_bridge_take_play();                     /* the take's recorded tempo owns the transport */
    run_isr_block(128, 128);
    CHECK(live_bridge_bpm_x10() == 1200u);       /* the recorded tempo, not the 160-bpm clock */
    live_bridge_set_bpm_soft(1500u);             /* a clock change mid-replay is ignored */
    live_bridge_set_bpm(1800u);                  /* a UI tempo edit is ignored for the transport */
    run_isr_block(256, 128);
    CHECK(live_bridge_bpm_x10() == 1400u);       /* the recorded change, not the edits */
    run_isr_block(384, 6000);
    CHECK(live_bridge_bpm_x10() == 1400u);       /* it holds */
    live_bridge_take_stop_play();
    CHECK(live_bridge_bpm_x10() == 1600u);       /* the pre-take followed clock comes back */
    live_bridge_exit();
}

/* T9i r4: a relay launch (the same clip pressed again while the first is still queued) cancels the
 * queue. The FIFO must consume both staged copies so a later launch still stages cleanly. */
static void r4_relay_test(void)
{
    uint32_t *ev;
    live_bridge_take_init();
    live_bridge_enter();
    ((live_song_t *)live_bridge_song())->bpm_x10 = 1200u;
    g_now = 0;
    ev = (uint32_t *)live_bridge_take_events_mut();
    ev[0] = 0u;      ev[1] = TAKE_MARK_TEMPO(1200u);
    ev[2] = 200u;    ev[3] = TAKE_LAUNCH_Q(0u, Q_BAR, LIVE_DRUMS);   /* A: the bar */
    ev[4] = 400u;    ev[5] = TAKE_LAUNCH_Q(0u, Q_BAR, LIVE_DRUMS);   /* the relay: cancels A */
    ev[6] = 90000u;  ev[7] = TAKE_LAUNCH_Q(2u, Q_BAR, LIVE_DRUMS);   /* a later launch still works */
    ev[8] = 400000u; ev[9] = TAKE_MARK_END;
    live_bridge_take_loaded(5, 800000u, 1200u);
    {
        int d0 = live_bridge_take_drops();
        live_bridge_take_play();
        run_isr_block(0, 128);
        live_bridge_take_stage();
        run_isr_block(128, 512);                 /* A and its relay replay */
        CHECK(live_bridge_queued_clip(LIVE_DRUMS) == CLIP_NONE);   /* A was cancelled */
        live_bridge_take_stage();
        run_isr_block(640, 90000);               /* up to the later launch */
        live_bridge_take_stage();
        run_isr_block(90640, 128);
        CHECK(live_bridge_take_drops() - d0 == 0);
        CHECK(live_bridge_test_consistent());
        live_bridge_take_stop_play();
    }
    live_bridge_exit();
}

/* T9i r5: a song edit during playback returns at end only when no clock was followed.
 * Edit just before stopping, without a render, to pin the source on the following render too. */
static void r5_tempo_restore_test(void)
{
    int clock;
    for (clock = 0; clock < 2; clock++) {
        uint32_t *ev;
        live_song_t *song;
        live_bridge_take_init();
        live_bridge_enter();
        song = (live_song_t *)live_bridge_song();
        song->bpm_x10 = 1200u;
        g_now = 0;
        live_bridge_set_bpm(1200u);
        run_isr_block(0, 128);
        if (clock) {
            live_bridge_set_bpm_soft(1600u);
            run_isr_block(128, 128);
        }
        ev = (uint32_t *)live_bridge_take_events_mut();
        ev[0] = 0u; ev[1] = TAKE_MARK_TEMPO(1400u);
        ev[2] = 400000u; ev[3] = TAKE_MARK_END;
        live_bridge_take_loaded(2, 800000u, 1400u);
        live_bridge_take_play();
        run_isr_block(256, 128);
        CHECK(live_bridge_bpm_x10() == 1400u);
        song->bpm_x10 = 1800u;
        live_bridge_take_stop_play();
        CHECK(live_bridge_bpm_x10() == (clock ? 1600u : 1800u));
        run_isr_block(384, 128);
        CHECK(live_bridge_bpm_x10() == (clock ? 1600u : 1800u));
        live_bridge_exit();
    }
}

/* T9i r5: one UI-frame overflow must not poison later launches of the same role. */
static void r5_drop_recovery_test(void)
{
    uint32_t *ev, s;
    int d0;
    live_bridge_take_init();
    live_bridge_enter();
    ((live_song_t *)live_bridge_song())->bpm_x10 = 1200u;
    g_now = 0;
    ev = (uint32_t *)live_bridge_take_events_mut();
    ev[0] = 0u; ev[1] = TAKE_MARK_TEMPO(1200u);
    ev[2] = 200u; ev[3] = TAKE_LAUNCH(0u, TAKE_LAUNCH_NOW, LIVE_DRUMS);
    ev[4] = 300u; ev[5] = TAKE_LAUNCH(1u, TAKE_LAUNCH_NOW, LIVE_DRUMS);
    ev[6] = 400u; ev[7] = TAKE_LAUNCH(2u, TAKE_LAUNCH_NOW, LIVE_DRUMS);
    ev[8] = 50000u; ev[9] = TAKE_LAUNCH(0u, TAKE_LAUNCH_NOW, LIVE_DRUMS);
    ev[10] = 100000u; ev[11] = TAKE_LAUNCH(1u, TAKE_LAUNCH_NOW, LIVE_DRUMS);
    ev[12] = 400000u; ev[13] = TAKE_MARK_END;
    live_bridge_take_loaded(7, 800000u, 1200u);
    d0 = live_bridge_take_drops();
    live_bridge_take_play();
    for (s = 0; s < 150000u; s += 128u) {
        if (s && s % 4352u == 0)
            live_bridge_take_stage();
        run_isr_block(s, 128u);
        if (s == 60032u) CHECK(live_bridge_playing_clip(LIVE_DRUMS) == 0);
        if (s == 120064u) CHECK(live_bridge_playing_clip(LIVE_DRUMS) == 1);
        CHECK(live_bridge_test_consistent());
    }
    CHECK(live_bridge_take_drops() - d0 == 1);
    live_bridge_take_stop_play();
    live_bridge_exit();
}

int main(void)
{
    int i;
    memset(pats, 0, sizeof pats);

    /* P0: a 909 kick on steps 0 and 4 (accented on 0), a snare on step 8; a 303 A line
     * (accented note, a SLIDE tie, a plain note, a rest) with a +2 transpose; a 303 B note. */
    sm_set(pats[0].drum[0].hit[0], 0);
    sm_set(pats[0].drum[0].hit[0], 4);
    sm_set(pats[0].drum[0].hit[3], 8);
    pats[0].drum[0].accent[0] = 1u << 0;
    pats[0].drum[0].len = 16;

    pats[0].bass[0].len = 8;
    pats[0].bass[0].transpose = 24 + 2;
    pats[0].bass[0].step[0] = (bstep_t){36, (uint8_t)(G_NOTE | BS_ACCENT)};
    pats[0].bass[0].step[1] = (bstep_t){0, (uint8_t)(G_TIE | BS_SLIDE)};   /* the next note slides */
    pats[0].bass[0].step[2] = (bstep_t){38, G_NOTE};
    pats[0].bass[0].step[3] = (bstep_t){0, G_REST};

    pats[0].bass[1].len = 4;
    pats[0].bass[1].transpose = 24;
    pats[0].bass[1].step[0] = (bstep_t){48, G_NOTE};

    /* P1: a 909 snare/clap on step 2 only (no drums before it); no 303 lines */
    sm_set(pats[1].drum[0].hit[1], 2);
    pats[1].drum[0].len = 16;

    /* P2: no 909 at all; a 303 A note (a second BASS clip) */
    pats[2].bass[0].len = 4;
    pats[2].bass[0].transpose = 24;
    pats[2].bass[0].step[0] = (bstep_t){40, G_NOTE};

    /* P4: a 64-step 909 pattern with a hit in the second word (step 40) — clip C */
    sm_set(pats[4].drum[0].hit[0], 40);               /* word 1, bit 8 */
    pats[4].drum[0].len = 64;

    /* P5..P9: 5 more drum patterns (so 8 non-empty in total; the 7th slot caps the map) */
    for (i = 5; i <= 9; i++) {
        sm_set(pats[i].drum[0].hit[2], i);
        pats[i].drum[0].len = 16;
    }

    live_bridge_map(pats, &sg);

    /* roles and their clip ranges */
    CHECK(sg.role[ROLE_DRUMS].engine == ENG_909);
    CHECK(sg.role[ROLE_DRUMS].first_clip == 0 && sg.role[ROLE_DRUMS].nclips == ROLE_CLIPS_DRUMS);
    CHECK(sg.role[ROLE_BASS].engine == ENG_303);
    CHECK(sg.role[ROLE_BASS].first_clip == 7 && sg.role[ROLE_BASS].nclips == ROLE_CLIPS_BASS);
    CHECK(sg.role[ROLE_LEAD].engine == ENG_303);
    CHECK(sg.role[ROLE_LEAD].first_clip == 12 && sg.role[ROLE_LEAD].nclips == ROLE_CLIPS_LEAD);
    CHECK(sg.role[ROLE_CHORDS].nclips == ROLE_CLIPS_CHORDS);
    CHECK(sg.role[ROLE_FX].nclips == ROLE_CLIPS_FX);

    /* drums: the first 7 non-empty patterns fill all 7 slots, the 8th is dropped; a direct
     * dpart -> drum_steps_t map (including the second 32-step word and a 64-step length) */
    CHECK(count_used(0, ROLE_CLIPS_DRUMS) == 7);
    CHECK(sg.clip[0].used && sg.clip[1].used && sg.clip[2].used && sg.clip[6].used);
    CHECK(sg.clip[0].len == 16 && sg.clip[0].mode == CLIP_LOOP && sg.clip[0].quant == Q_BAR);
    CHECK(drum_get(&sg.clip[0].d.drum, 0, 0) && drum_get(&sg.clip[0].d.drum, 0, 4));
    CHECK(!drum_get(&sg.clip[0].d.drum, 0, 1));
    CHECK(drum_get(&sg.clip[0].d.drum, 3, 8));
    CHECK((sg.clip[0].d.drum.accent[0] & 1u) == 1u);          /* step 0 accented */
    CHECK(drum_get(&sg.clip[1].d.drum, 1, 2) && !drum_get(&sg.clip[1].d.drum, 0, 0));
    CHECK(sg.clip[2].len == 64 && drum_get(&sg.clip[2].d.drum, 0, 40) &&
          !drum_get(&sg.clip[2].d.drum, 0, 39));

    /* bass: P0 -> clip 7 (first non-empty 303 A), P2 -> clip 8; the tie extends the note, the
     * transpose is applied, and the note after a BS_SLIDE step carries the slide in vel bit 7 */
    CHECK(count_used(7, ROLE_CLIPS_BASS) == 2);
    CHECK(sg.clip[7].used && sg.clip[8].used && !sg.clip[9].used);
    CHECK(sg.clip[7].nnotes == 2);
    CHECK(sg.clip[7].d.notes[0].step == 0 && sg.clip[7].d.notes[0].note == 38 /* 36 + 2 */);
    CHECK(sg.clip[7].d.notes[0].vel == 127 && sg.clip[7].d.notes[0].len == 2);   /* accented, tie */
    CHECK(sg.clip[7].d.notes[1].step == 2 && sg.clip[7].d.notes[1].note == 40 /* 38 + 2 */);
    CHECK(sg.clip[7].d.notes[1].vel == (88u | 0x80u));                           /* slid into, not accented */
    CHECK(sg.clip[7].d.notes[1].len == 1);
    CHECK(sg.clip[8].d.notes[0].note == 40 && sg.clip[8].nnotes == 1);

    /* lead: P0's 303 B -> clip 12 */
    CHECK(count_used(12, ROLE_CLIPS_LEAD) == 1);
    CHECK(sg.clip[12].used && sg.clip[12].d.notes[0].note == 48);

    /* T5i: the CHORDS clips are derived from the BASS clips (each note a chord at root+12/+19/+24,
     * vel 90, the same step and length), and the three FX clips are generated */
    CHECK(count_used(19, ROLE_CLIPS_CHORDS) == 2);
    CHECK(sg.clip[19].used && sg.clip[20].used && !sg.clip[21].used);
    CHECK(sg.clip[19].nnotes == 6);
    CHECK(sg.clip[19].d.notes[0].step == 0 && sg.clip[19].d.notes[0].note == 50 &&
          sg.clip[19].d.notes[0].vel == 90 && sg.clip[19].d.notes[0].len == 2);
    CHECK(sg.clip[19].d.notes[1].note == 57 && sg.clip[19].d.notes[2].note == 62);
    CHECK(sg.clip[19].d.notes[3].step == 2 && sg.clip[19].d.notes[3].note == 52 &&
          sg.clip[19].d.notes[3].len == 1);
    CHECK(sg.clip[19].d.notes[4].note == 59 && sg.clip[19].d.notes[5].note == 64);
    CHECK(sg.clip[20].nnotes == 3 && sg.clip[20].d.notes[0].note == 52 &&
          sg.clip[20].d.notes[1].note == 59 && sg.clip[20].d.notes[2].note == 64);
    CHECK(count_used(24, ROLE_CLIPS_FX) == 3);
    CHECK(sg.clip[24].len == 16 && sg.clip[24].nnotes == 4 && sg.clip[24].d.notes[0].step == 2 &&
          sg.clip[24].d.notes[0].note == 60 && sg.clip[24].d.notes[0].vel == 100 &&
          sg.clip[24].d.notes[3].step == 14);
    CHECK(sg.clip[25].len == 16 && sg.clip[25].nnotes == 16 && sg.clip[25].d.notes[0].vel == 20 &&
          sg.clip[25].d.notes[0].step == 0 && sg.clip[25].d.notes[15].vel == 127 &&
          sg.clip[25].d.notes[15].step == 15);
    CHECK(sg.clip[26].len == 32 && sg.clip[26].nnotes == 3 && sg.clip[26].d.notes[0].step == 0 &&
          sg.clip[26].d.notes[1].step == 11 && sg.clip[26].d.notes[2].step == 22 &&
          sg.clip[26].d.notes[0].note == 48 && sg.clip[26].d.notes[0].vel == 80 &&
          sg.clip[26].d.notes[0].len == 4);

    /* every clip's motion lanes are empty (marked MOTION_NONE, not zero) */
    CHECK(sg.clip[0].motion[0].param == PARAM_NONE && sg.clip[0].motion[1].param == PARAM_NONE);
    CHECK(sg.clip[0].motion[0].v[0] == MOTION_NONE && sg.clip[7].motion[1].v[15] == MOTION_NONE);

    /* the key map defaults to identity */
    {
        int id = 1;
        for (i = 0; i < (int)NKEYS; i++)
            if (sg.key_clip[i] != i)
                id = 0;
        CHECK(id);
    }

    /* the bridge's event stream: launch a drum clip and a 303 clip "now" and render; the sink
     * (not a stub) sees the DRUM with its lane and the NOTE_ON/NOTE_OFF of the bass. */
    live_bridge_rebuild(pats);                        /* TCLS: enter no longer seeds from patterns */
    live_bridge_enter();
    live_bridge_transport_start();
    live_bridge_launch_now(0);                        /* drum clip A: a kick on step 0 */
    live_bridge_launch_now(7);                        /* bass clip A: a note on step 0 */
    ncap = 0;
    {
        live_sink_t sink = {cap_ev, 0};
        live_bridge_render(20000u, &sink);            /* ~0.45 s at 44.1 kHz: step 0 and its off */
    }
    {
        int drum = 0, non = 0, noff = 0;
        for (i = 0; i < ncap; i++) {
            if (cap[i].kind == LIVE_EV_DRUM && cap[i].role == LIVE_DRUMS && cap[i].a == 0)
                drum = 1;
            if (cap[i].kind == LIVE_EV_NOTE_ON && cap[i].role == LIVE_BASS && cap[i].a == 38)
                non = 1;
            if (cap[i].kind == LIVE_EV_NOTE_OFF && cap[i].role == LIVE_BASS)
                noff = 1;
        }
        CHECK(drum && non && noff);
    }
    live_bridge_exit();

    /* T9i r1: the ISR-race tests (they re-enter LIVE themselves). b2 first, then b1 (which traps on
     * the reviewed code, so it must run last to let b2's assertion be seen). */
    live_bridge_enter();                                /* TCLS: rebuild seeds the clips in main */
    b2_leave_test();
    b1_isr_sink_test();

    /* T9i r2: the preemption-injection race tests and the tempo hold. */
    live_bridge_test_hook = test_hook;
    take_test_hook = test_hook;
    r2_b1_play_race_test();
    r2_b2_tempo_test();
    r2_s3_next_stage_test();
    r2_s4_finish_race_test();

    /* T9i r3: the late press and the stalled-UI staging, the live take_stage hook, the clock. */
    r3_stage_hook_test();
    r3_s1_late_press_test();
    r3_s2_stall_test();

    /* T9i r4: the playing-role repro (the two-deep staging FIFO) and the take-owned tempo. */
    r4_playing_role_test();
    r4_tempo_test();
    r4_relay_test();
    r5_drop_recovery_test();
    r5_tempo_restore_test();

    if (fails) {
        printf("live_bridge: %d check(s) failed\n", fails);
        return 1;
    }
    printf("live_bridge: mapping and events ok\n");
    return 0;
}
