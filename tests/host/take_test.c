/* SPDX-License-Identifier: GPL-3.0-only */
/* T9c host tests for the take recorder and player.
 *
 * The golden cases write a text dump of a take (or of the clip-engine parity run) and compare
 * it with tests/golden/take/<case>.txt. GOLDEN_UPDATE=1 rewrites the goldens. The event-exact
 * checks (round trip, wrapping counter, dense window, clip-engine parity) are assertions in
 * code as well.
 *
 * Build: cc -std=c99 -I. tests/host/take_test.c core/seq/take.c core/seq/clip_engine.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include "core/model/model.h"
#include "core/seq/take.h"
#include "core/seq/clip_engine.h"

#define OUTMAX (1u << 20)
#define CEVMAX 8192

static int fails;

static char out[OUTMAX];
static size_t outn;

static void put(const char *s)
{
    size_t n = strlen(s);
    if (outn + n < OUTMAX) { memcpy(out + outn, s, n); outn += n; }
}

static void putf(const char *fmt, ...)
{
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    put(b);
}

static void failf(const char *fmt, ...)
{
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    printf("  FAIL %s\n", b);
    fails++;
}

/* ---- command words, decoded for the goldens --------------------------------- */

static const char *LNAME[] = { "queue", "now", "stop" };
static const char *FNAME[] = { "off", "on", "latch" };

static void cmd_str(uint32_t cmd, char *b, size_t n)
{
    uint32_t type = TAKE_CMD_TYPE(cmd);
    uint32_t a = TAKE_CMD_A(cmd), bb = TAKE_CMD_B(cmd), c = TAKE_CMD_C(cmd);
    switch (type) {
    case TAKE_T_LAUNCH:
        if (bb == TAKE_LAUNCH_STOP)
            snprintf(b, n, "launch stop role=%u", c);
        else
            snprintf(b, n, "launch clip=%u %s role=%u", a, LNAME[bb < 3 ? bb : 0], c);
        break;
    case TAKE_T_MUTE:
        snprintf(b, n, "mute role=%u on=%u", a, bb);
        break;
    case TAKE_T_FX:
        snprintf(b, n, "fx id=%u %s", a, FNAME[bb < 3 ? bb : 0]);
        break;
    case TAKE_T_MARK:
        if (a == 0u)      snprintf(b, n, "mark start");
        else if (a == 1u) snprintf(b, n, "mark end");
        else              snprintf(b, n, "mark tempo=%u",
                                  (unsigned)TAKE_MARK_TEMPO_GET(bb, c));
        break;
    default:
        snprintf(b, n, "param t=%u a=%u b=%u c=%u", type, a, bb, c);
        break;
    }
}

/* ---- golden compare --------------------------------------------------------- */

static void expect_golden(const char *name)
{
    char path[512];
    FILE *f;

    if (outn >= OUTMAX)
        outn = OUTMAX - 1u;
    out[outn] = 0;                            /* strcmp() and fwrite() need the end */

    snprintf(path, sizeof path, "%s/%s.txt",
             getenv("GOLDEN_DIR") ? getenv("GOLDEN_DIR") : "tests/golden/take", name);

    const char *gu = getenv("GOLDEN_UPDATE");   /* T4 review: exactly "1", as tests/golden.py */
    if (gu && strcmp(gu, "1") == 0) {
        f = fopen(path, "w");
        if (!f) { failf("%s: cannot write %s", name, path); return; }
        fwrite(out, 1, outn, f);
        fclose(f);
        printf("  UPDATE %s\n", name);
        return;
    }
    f = fopen(path, "r");
    if (!f) { failf("%s: no golden %s (run GOLDEN_UPDATE=1)", name, path); return; }
    {
        static char g[OUTMAX];
        size_t gl = fread(g, 1, sizeof g - 1, f);
        g[gl] = 0;
        fclose(f);
        if (strcmp(g, out) != 0) {
            const char *a = g, *b = out;
            int line = 1;
            while (*a && *b && *a == *b) { if (*a == '\n') line++; a++; b++; }
            failf("%s: first difference at line %d\n    golden: %.70s\n    got:    %.70s",
                  name, line, a, b);
            return;
        }
    }
    printf("  ok   %s\n", name);
}

/* ---- deterministic PRNG ----------------------------------------------------- */

static uint32_t rnd(uint32_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

/* ---- a caller-supplied bar grid (what the sequencer would provide) ---------- */

typedef struct { uint32_t origin, bar_len; } clock_t;

static uint32_t bar_next(void *ctx, uint32_t sample)
{
    clock_t *c = (clock_t *)ctx;
    uint32_t d = sample - c->origin;
    uint32_t k = d / c->bar_len;
    if (k * c->bar_len < d)
        k++;
    return c->origin + k * c->bar_len;
}

/* ---- the one take ----------------------------------------------------------- */

static take_t tk;

/* ---- replay capture --------------------------------------------------------- */

static uint32_t rp_base;
static uint32_t rp_offs[TAKE_MAX_EVENTS];
static uint32_t rp_cmds[TAKE_MAX_EVENTS];
static int rp_n;

static void rp_sink(void *ctx, uint32_t off, uint32_t cmd)
{
    (void)ctx;
    if (rp_n < (int)TAKE_MAX_EVENTS) {
        rp_offs[rp_n] = rp_base + off;
        rp_cmds[rp_n] = cmd;
        rp_n++;
    }
}

static const take_sink_t RSINK = { rp_sink, 0 };

/* Replay the whole take in random-width windows. Returns the number of events seen. */
static int replay_random(take_t *t, uint32_t seed)
{
    static uint32_t st;
    st = seed ? seed : 1u;
    rp_n = 0;
    rp_base = 0;
    take_play(t, 777u);
    for (;;) {
        uint32_t w = (rnd(&st) % 700u) + 1u;
        take_render(t, w, &RSINK);
        rp_base += w;
        if ((int32_t)(rp_base - (t->hdr.length_samples + 1u)) >= 0)
            break;
    }
    return rp_n;
}

/* Compare the replayed stream with the recorded log; the offset of a replayed event is
 * reconstructed from the window bases, so this is the event-exact check. */
static int replay_matches(take_t *t)
{
    int i;
    if (rp_n != (int)t->n) {
        failf("replay: %d events, log has %u", rp_n, (unsigned)t->n);
        return 0;
    }
    for (i = 0; i < rp_n; i++) {
        if (rp_offs[i] != t->ev[i].sample || rp_cmds[i] != t->ev[i].cmd) {
            failf("replay event %d: got (%u,0x%08x) want (%u,0x%08x)", i,
                  rp_offs[i], rp_cmds[i], t->ev[i].sample, t->ev[i].cmd);
            return 0;
        }
    }
    return 1;
}

static void dump_log(take_t *t)
{
    int i;
    for (i = 0; i < (int)t->n; i++) {
        char b[96];
        cmd_str(t->ev[i].cmd, b, sizeof b);
        putf("log %u 0x%08x %s\n", t->ev[i].sample, t->ev[i].cmd, b);
    }
}

static void dump_replay(void)
{
    int i;
    for (i = 0; i < rp_n; i++) {
        char b[96];
        cmd_str(rp_cmds[i], b, sizeof b);
        putf("play %u 0x%08x %s\n", rp_offs[i], rp_cmds[i], b);
    }
}

/* ---- case: record + replay round trip, random windows ----------------------- */

static void case_record_replay(void)
{
    clock_t clk = { 0u, 96000u };
    take_t *t = &tk;

    outn = 0;
    putf("# case record_replay\n");
    take_init(t);
    take_set_bar_fn(t, bar_next, &clk);
    take_set_bpm(t, 1200u);
    take_arm(t, 1000u);                                    /* start = 96000 */
    take_feed(t, 1000u, TAKE_LAUNCH(12u, TAKE_LAUNCH_QUEUE, ROLE_LEAD));  /* pre-start: dropped */
    take_feed(t, 96000u, TAKE_LAUNCH(12u, TAKE_LAUNCH_QUEUE, ROLE_LEAD));
    take_feed(t, 96005u, TAKE_LAUNCH(13u, TAKE_LAUNCH_NOW, ROLE_LEAD));
    take_feed(t, 96005u, TAKE_MUTE(ROLE_BASS, 1u));
    take_feed(t, 136000u, TAKE_FX(0u, TAKE_FX_ON));
    take_stop(t, 150000u);                                 /* stop_at = 192000 */
    take_feed(t, 192000u, TAKE_LAUNCH(CLIP_NONE, TAKE_LAUNCH_STOP, ROLE_BASS)); /* at stop: dropped */

    putf("state=%d nevents=%u length=%u full=%d\n",
         take_state(t), (unsigned)take_nevents(t),
         (unsigned)t->hdr.length_samples, take_full(t));
    dump_log(t);

    replay_random(t, 0xC0FFEEu);
    dump_replay();
    putf("# replay_events=%d\n", rp_n);
    replay_matches(t);
    /* take_play resets the cursor: a second pass produces the identical stream */
    {
        int r1 = rp_n;
        replay_random(t, 0xC0FFEEu);
        if (rp_n != r1 || !replay_matches(t))
            failf("the second replay pass differed");
        putf("# replay_pass2=%d\n", rp_n);
    }
    expect_golden("record_replay");
}

/* ---- case: arm and stop land on bar boundaries ------------------------------ */

static void case_bar_boundaries(void)
{
    clock_t clk = { 0u, 96000u };
    take_t *t = &tk;

    outn = 0;
    putf("# case bar_boundaries\n");
    take_init(t);
    take_set_bar_fn(t, bar_next, &clk);
    take_set_bpm(t, 1200u);
    take_arm(t, 5000u);
    putf("armed start=%u state=%d\n", (unsigned)t->start, take_state(t));
    take_feed(t, 5000u, TAKE_LAUNCH(12u, TAKE_LAUNCH_QUEUE, ROLE_LEAD));   /* dropped */
    take_feed(t, 95999u, TAKE_LAUNCH(12u, TAKE_LAUNCH_QUEUE, ROLE_LEAD));  /* dropped */
    putf("prestart nevents=%u state=%d\n", (unsigned)take_nevents(t), take_state(t));
    take_feed(t, 96000u, TAKE_LAUNCH(12u, TAKE_LAUNCH_QUEUE, ROLE_LEAD));  /* offset 0 */
    take_feed(t, 110000u, TAKE_MUTE(ROLE_FX, 1u));                         /* offset 14000 */
    take_stop(t, 110000u);                                                 /* stop_at = 192000 */
    putf("stopping start=%u stop_at=%u state=%d\n",
         (unsigned)t->start, (unsigned)t->stop_at, take_state(t));
    take_tick(t, 191999u);
    putf("before stop nevents=%u state=%d\n", (unsigned)take_nevents(t), take_state(t));
    take_tick(t, 192000u);
    putf("stopped nevents=%u length=%u state=%d full=%d\n",
         (unsigned)take_nevents(t), (unsigned)t->hdr.length_samples,
         take_state(t), take_full(t));
    dump_log(t);

    /* an arm that is cancelled before its boundary leaves no take */
    take_init(t);
    take_set_bar_fn(t, bar_next, &clk);
    take_arm(t, 5000u);
    take_stop(t, 6000u);
    putf("cancelled has=%d state=%d nevents=%u\n",
         take_has(t), take_state(t), (unsigned)take_nevents(t));

    /* arming exactly on a boundary records at once (offset 0) */
    take_init(t);
    take_set_bar_fn(t, bar_next, &clk);
    take_arm(t, 96000u);
    take_feed(t, 96000u, TAKE_FX(1u, TAKE_FX_LATCH));
    putf("on_boundary start=%u nevents=%u state=%d\n",
         (unsigned)t->start, (unsigned)take_nevents(t), take_state(t));
    expect_golden("bar_boundaries");
}

/* ---- case: events stamped before the take's start are dropped --------------- */

static void case_prestart_drop(void)
{
    take_t *t = &tk;
    int i, ok = 1;

    outn = 0;
    putf("# case prestart_drop\n");
    take_init(t);
    take_arm(t, 1000u);                        /* no bar fn: start = 1000, recording at once */
    take_feed(t, 990u,  TAKE_FX(9u, TAKE_FX_ON));    /* before start: dropped */
    take_feed(t, 1000u, TAKE_MUTE(ROLE_DRUMS, 1u));  /* offset 0 */
    take_feed(t, 999u,  TAKE_FX(8u, TAKE_FX_ON));    /* before start: dropped */
    take_feed(t, 1010u, TAKE_FX(1u, TAKE_FX_ON));    /* offset 10 */
    take_feed(t, 1015u, TAKE_FX(2u, TAKE_FX_ON));    /* offset 15 */
    take_stop(t, 1015u);
    take_tick(t, 1015u);

    putf("nevents=%u length=%u\n",
         (unsigned)take_nevents(t), (unsigned)t->hdr.length_samples);
    dump_log(t);

    if (take_nevents(t) != 5u)                 /* start + 3 events + end */
        ok = 0;
    if (t->ev[0].cmd != TAKE_MARK_START ||
        t->ev[1].sample != 0u || t->ev[1].cmd != TAKE_MUTE(ROLE_DRUMS, 1u) ||
        t->ev[2].sample != 10u || t->ev[3].sample != 15u)
        ok = 0;
    /* the log is sorted by offset */
    for (i = 1; i < (int)take_nevents(t); i++)
        if ((int32_t)(t->ev[i].sample - t->ev[i - 1u].sample) < 0)
            ok = 0;
    if (!ok)
        failf("a pre-start event was not dropped or the log was unsorted");
    putf("# ok=%d\n", ok);
    expect_golden("prestart_drop");
}

/* ---- case: the prelude restores the state ----------------------------------- */

typedef struct {
    uint8_t  clip[NROLES];
    uint8_t  mute[NROLES];
    uint16_t bpm;
    uint8_t  par[6];
} st_t;

static st_t src_state, dst_state;

/* When set, the prelude tries to feed the take mid-build: it must be ignored (state == IDLE),
 * and no prelude event may be lost. */
static uint32_t prelude_bad_cmd;

static void prelude_fn(void *ctx, take_t *t)
{
    st_t *s = (st_t *)ctx;
    int i, r;
    for (r = 0; r < NROLES; r++)
        if (s->clip[r] != CLIP_NONE)
            take_setup_event(t, TAKE_LAUNCH(s->clip[r], TAKE_LAUNCH_NOW, (uint32_t)r));
    if (prelude_bad_cmd)
        take_feed(t, 0u, prelude_bad_cmd);     /* an ISR feed during the build: ignored */
    for (r = 0; r < NROLES; r++)
        take_setup_event(t, TAKE_MUTE((uint32_t)r, s->mute[r]));
    take_setup_event(t, TAKE_MARK_TEMPO(s->bpm));
    for (i = 0; i < 6; i++)
        take_setup_event(t, TAKE_CMD(0u, (uint32_t)i, 0u, s->par[i]));
    take_set_bpm(t, s->bpm);
}

static void restore_sink(void *ctx, uint32_t off, uint32_t cmd)
{
    (void)ctx;
    (void)off;
    if (TAKE_CMD_TYPE(cmd) == TAKE_T_LAUNCH) {
        uint32_t a = TAKE_CMD_A(cmd), b = TAKE_CMD_B(cmd), c = TAKE_CMD_C(cmd);
        if (b == TAKE_LAUNCH_NOW && c < NROLES)
            dst_state.clip[c] = (uint8_t)a;
    } else if (TAKE_CMD_TYPE(cmd) == TAKE_T_MUTE) {
        uint32_t a = TAKE_CMD_A(cmd);
        if (a < NROLES)
            dst_state.mute[a] = (uint8_t)TAKE_CMD_B(cmd);
    } else if (TAKE_CMD_TYPE(cmd) == TAKE_T_MARK && TAKE_CMD_A(cmd) == 2u) {
        dst_state.bpm = (uint16_t)TAKE_MARK_TEMPO_GET(TAKE_CMD_B(cmd), TAKE_CMD_C(cmd));
    } else if (TAKE_CMD_TYPE(cmd) == 0u) {
        uint32_t a = TAKE_CMD_A(cmd);
        if (a < 6)
            dst_state.par[a] = (uint8_t)TAKE_CMD_C(cmd);
    }
}

static const take_sink_t DRSINK = { restore_sink, 0 };

static void case_prelude_restore(void)
{
    take_t *t = &tk;
    int i, r, ok = 1;

    outn = 0;
    putf("# case prelude_restore\n");
    memset(&src_state, 0, sizeof src_state);
    memset(&dst_state, 0, sizeof dst_state);
    for (i = 0; i < NROLES; i++)
        dst_state.clip[i] = CLIP_NONE;        /* CLIP_NONE = a role that is stopped */
    src_state.clip[ROLE_DRUMS]  = 0u;
    src_state.clip[ROLE_BASS]   = 7u;
    src_state.clip[ROLE_LEAD]   = 12u;
    src_state.clip[ROLE_CHORDS] = CLIP_NONE;
    src_state.clip[ROLE_FX]     = 26u;
    src_state.mute[ROLE_BASS]   = 1u;
    src_state.mute[ROLE_FX]     = 1u;
    src_state.bpm = 1435u;
    for (i = 0; i < 6; i++)
        src_state.par[i] = (uint8_t)(10 + i * 7);

    take_init(t);
    take_set_bpm(t, 1200u);
    take_set_prelude(t, prelude_fn, &src_state);
    prelude_bad_cmd = TAKE_CMD(0u, 0xAAu, 0xBBu, 0xCCCu);   /* must be dropped mid-build */
    take_arm(t, 0u);
    take_stop(t, 0u);                         /* request the end at offset 0 */
    take_tick(t, 0u);                         /* the ISR path writes the END mark */

    putf("header nevents=%u bpm=%u length=%u\n",
         (unsigned)take_nevents(t), (unsigned)t->hdr.bpm_x10,
         (unsigned)t->hdr.length_samples);
    dump_log(t);

    /* the mid-build feed must not appear, and every prelude event must be present */
    {
        int bad = 0;
        for (i = 0; i < (int)take_nevents(t); i++)
            if (t->ev[i].cmd == prelude_bad_cmd)
                bad = 1;
        putf("bad_feed_present=%d\n", bad);
        if (bad)
            ok = 0;
    }
    prelude_bad_cmd = 0u;

    take_play(t, 100u);
    take_render(t, 1000u, &DRSINK);

    for (r = 0; r < NROLES; r++) {
        putf("role %d clip=%u mute=%u\n", r, dst_state.clip[r], dst_state.mute[r]);
        if (dst_state.clip[r] != src_state.clip[r] || dst_state.mute[r] != src_state.mute[r])
            ok = 0;
    }
    for (i = 0; i < 6; i++) {
        putf("param %d = %u\n", i, dst_state.par[i]);
        if (dst_state.par[i] != src_state.par[i])
            ok = 0;
    }
    putf("restored bpm=%u (header says %u)\n", dst_state.bpm, (unsigned)t->hdr.bpm_x10);
    if (dst_state.bpm != t->hdr.bpm_x10)
        ok = 0;
    if (!ok)
        failf("prelude did not restore the state");
    expect_golden("prelude_restore");
}

/* ---- case: a full buffer ---------------------------------------------------- */

static void case_full_buffer(void)
{
    take_t *t = &tk;
    uint32_t i;
    int ok;

    outn = 0;
    putf("# case full_buffer\n");
    take_init(t);
    take_arm(t, 0u);
    for (i = 0; i < TAKE_MAX_EVENTS + 100u; i++)
        take_feed(t, i, TAKE_FX(i & 0xFFu, TAKE_FX_ON));

    putf("nevents=%u full=%d state=%d last=0x%08x\n",
         (unsigned)take_nevents(t), take_full(t), take_state(t),
         (unsigned)t->ev[t->n - 1].cmd);
    putf("last_off=%u length=%u\n",
         (unsigned)t->ev[t->n - 1].sample, (unsigned)t->hdr.length_samples);

    ok = take_nevents(t) == (unsigned)TAKE_MAX_EVENTS && take_full(t) &&
         t->ev[t->n - 1].cmd == TAKE_MARK_END && take_state(t) == TAKE_IDLE;
    /* every slot but the reserved end mark is a real fed event, in order */
    for (i = 1; i < take_nevents(t) - 1u; i++) {
        if (t->ev[i].cmd != TAKE_FX((i - 1u) & 0xFFu, TAKE_FX_ON) ||
            t->ev[i].sample != i - 1u)
            ok = 0;
    }
    if (t->ev[0].cmd != TAKE_MARK_START)
        ok = 0;
    if (!ok)
        failf("full buffer did not stop cleanly at the cap");

    replay_random(t, 0x1234u);
    putf("replay_events=%d\n", rp_n);
    if (!replay_matches(t))
        ok = 0;
    putf("# ok=%d\n", ok);
    expect_golden("full_buffer");
}

/* ---- case: the 32-bit sample counter wraps during a take -------------------- */

static void case_counter_wrap(void)
{
    take_t *t = &tk;
    uint32_t start = 0xFFFFF000u;
    int ok;

    outn = 0;
    putf("# case counter_wrap\n");
    take_init(t);
    take_arm(t, start);                       /* no bar fn: start = now */
    take_feed(t, start,               TAKE_LAUNCH(0u, TAKE_LAUNCH_QUEUE, ROLE_DRUMS));
    take_feed(t, start + 0xF0u,       TAKE_LAUNCH(1u, TAKE_LAUNCH_QUEUE, ROLE_DRUMS));
    take_feed(t, 0x00000010u,         TAKE_MUTE(ROLE_LEAD, 1u));   /* past 2^32 */
    take_feed(t, 0x00000040u,         TAKE_FX(2u, TAKE_FX_LATCH));
    take_stop(t, 0x00000100u);
    take_tick(t, 0x00000100u);

    putf("start=%u nevents=%u length=0x%x\n",
         (unsigned)t->start, (unsigned)take_nevents(t), (unsigned)t->hdr.length_samples);
    dump_log(t);

    ok = take_nevents(t) == 6u && t->hdr.length_samples == 0x1100u;
    if (t->ev[1].sample != 0u || t->ev[2].sample != 0xF0u ||
        t->ev[3].sample != 0x1010u || t->ev[4].sample != 0x1040u ||
        t->ev[5].sample != 0x1100u)
        ok = 0;
    replay_random(t, 0xABCDu);
    putf("replay_events=%d\n", rp_n);
    if (!replay_matches(t))
        ok = 0;
    if (!ok)
        failf("the counter wrap was not handled");
    putf("# ok=%d\n", ok);
    expect_golden("counter_wrap");
}

/* ---- case: a dense take replayed in one-frame windows ----------------------- */

#define DENSE_N 1000u

static void case_dense_window(void)
{
    take_t *t = &tk;
    uint32_t i;
    int ok, per_window_max = 0, cur = 0;

    outn = 0;
    putf("# case dense_window\n");
    take_init(t);
    take_arm(t, 0u);
    /* one event on every sample of the take */
    for (i = 0; i < DENSE_N; i++)
        take_feed(t, i, TAKE_FX(i & 0xFFu, TAKE_FX_ON));
    take_stop(t, DENSE_N);                     /* no bar fn: end at offset DENSE_N */
    take_tick(t, DENSE_N);

    /* replay it in one-frame windows: every event must fire on its own sample */
    rp_n = 0;
    rp_base = 0;
    take_play(t, 0u);
    for (i = 0; i <= DENSE_N + 1u; i++) {
        int before = rp_n;
        take_render(t, 1u, &RSINK);
        rp_base += 1u;
        cur = rp_n - before;
        if (cur > per_window_max)
            per_window_max = cur;
    }

    putf("nevents=%u full=%d replay_events=%d\n",
         (unsigned)take_nevents(t), take_full(t), rp_n);
    putf("first_off=%u last_off=%u per_window_max=%d\n",
         (unsigned)t->ev[1].sample, (unsigned)t->ev[t->n - 1].sample, per_window_max);
    ok = replay_matches(t);
    /* one event per sample: the window offsets are dense and in order */
    for (i = 1; i + 1u < take_nevents(t); i++)
        if (t->ev[i].sample != i - 1u)
            ok = 0;
    /* the start mark and the first event share sample 0, so at most 2 fire in one frame */
    if (per_window_max > 2)
        ok = 0;
    if (!ok)
        failf("the dense take did not replay exactly in one-frame windows");
    putf("# ok=%d\n", ok);
    expect_golden("dense_window");
}

/* ---- case: a main-loop stop writes exactly one END -------------------------- */

static void case_stop_once(void)
{
    clock_t clk = { 0u, 96000u };
    take_t *t = &tk;
    int i, ends = 0, ok = 1;

    outn = 0;
    putf("# case stop_once\n");
    take_init(t);
    take_set_bar_fn(t, bar_next, &clk);
    take_arm(t, 0u);                           /* start = 0: recording at once */
    take_feed(t, 0u, TAKE_FX(0u, TAKE_FX_ON));
    take_stop(t, 1000u);                       /* the main loop only requests: stop_at = 96000 */
    putf("after_request state=%d nevents=%u stop_at=%u\n",
         take_state(t), (unsigned)take_nevents(t), (unsigned)t->stop_at);

    /* feeds keep coming; the take stays open until the boundary */
    for (i = 1000; i < 96000; i += 1000)
        take_feed(t, (uint32_t)i, TAKE_FX((uint32_t)(i / 1000) & 0xFFu, TAKE_FX_ON));
    take_tick(t, 95000u);
    putf("pre_end state=%d nevents=%u\n", take_state(t), (unsigned)take_nevents(t));

    take_tick(t, 96000u);                      /* the ISR path writes the END */
    take_feed(t, 96000u, TAKE_FX(7u, TAKE_FX_ON));   /* after the END: ignored */
    take_tick(t, 200000u);

    for (i = 0; i < (int)take_nevents(t); i++)
        if (t->ev[i].cmd == TAKE_MARK_END)
            ends++;
    putf("ended state=%d nevents=%u ends=%d length=%u\n",
         take_state(t), (unsigned)take_nevents(t), ends, (unsigned)t->hdr.length_samples);
    dump_log(t);

    if (ends != 1 || take_state(t) != TAKE_IDLE || t->hdr.length_samples != 96000u)
        ok = 0;
    if (!ok)
        failf("a main-loop stop did not write exactly one END");
    putf("# ok=%d\n", ok);
    expect_golden("stop_once");
}

/* ---- case: replay through a real clip_engine -------------------------------- */

typedef struct { uint32_t sample; uint8_t kind, role, a, b, c; } cherec_t;

static song_t s1, s2;
static clip_engine_t e1, e2;
static cherec_t cev1[CEVMAX], cev2[CEVMAX];
static int cn1, cn2;
static uint32_t cev_base1, cev_base2;

typedef struct { cherec_t *arr; int *n; uint32_t *base; } col_t;

static void cev_sink(void *ctx, const clip_event_t *e)
{
    col_t *c = (col_t *)ctx;
    if (*c->n < CEVMAX) {
        c->arr[*c->n].sample = *c->base + e->sample;
        c->arr[*c->n].kind = e->kind;
        c->arr[*c->n].role = e->role;
        c->arr[*c->n].a = e->a;
        c->arr[*c->n].b = e->b;
        c->arr[*c->n].c = e->c;
        (*c->n)++;
    }
}

static col_t COL1 = { cev1, &cn1, &cev_base1 };
static col_t COL2 = { cev2, &cn2, &cev_base2 };

static void song_setup(song_t *s)
{
    memset(s, 0, sizeof *s);
    s->bpm_x10 = 1200u;
    s->quant_default = Q_BAR;
    s->role[ROLE_DRUMS].first_clip = 0;  s->role[ROLE_DRUMS].nclips = ROLE_CLIPS_DRUMS;
    s->role[ROLE_BASS].first_clip = 7;   s->role[ROLE_BASS].nclips = ROLE_CLIPS_BASS;
    s->role[ROLE_LEAD].first_clip = 12;  s->role[ROLE_LEAD].nclips = ROLE_CLIPS_LEAD;
    s->role[ROLE_CHORDS].first_clip = 19; s->role[ROLE_CHORDS].nclips = ROLE_CLIPS_CHORDS;
    s->role[ROLE_FX].first_clip = 24;    s->role[ROLE_FX].nclips = ROLE_CLIPS_FX;
}

static void clip_setup(song_t *s, int idx, int len, int mode)
{
    clip_t *c = &s->clip[idx];
    int lane, st;
    memset(c, 0, sizeof *c);
    c->used = 1;
    c->len = (uint8_t)len;
    c->mode = (uint8_t)mode;
    c->quant = Q_BAR;
    for (lane = 0; lane < (int)CLIP_MOTION_LANES; lane++) {
        c->motion[lane].param = PARAM_NONE;
        for (st = 0; st < (int)CLIP_MAX_STEPS; st++)
            c->motion[lane].v[st] = MOTION_NONE;
    }
}

static void clip_note(song_t *s, int idx, int step, int note, int vel, int len)
{
    clip_t *c = &s->clip[idx];
    int i;
    for (i = 0; i < (int)CLIP_MAX_NOTES; i++)
        if (c->d.notes[i].len == 0) {
            c->d.notes[i].step = (uint8_t)step;
            c->d.notes[i].note = (uint8_t)note;
            c->d.notes[i].vel = (uint8_t)vel;
            c->d.notes[i].len = (uint8_t)len;
            c->nnotes++;
            return;
        }
}

static void song_clips(song_t *s)
{
    clip_setup(s, 0, 4, CLIP_LOOP);          /* drums */
    drum_set(&s->clip[0].d.drum, 0, 0, 1);
    drum_set(&s->clip[0].d.drum, 0, 2, 1);
    clip_setup(s, 7, 4, CLIP_LOOP);          /* bass */
    clip_note(s, 7, 0, 40, 100, 1);
    clip_note(s, 7, 2, 43, 100, 1);
    clip_setup(s, 12, 4, CLIP_LOOP);         /* lead */
    clip_note(s, 12, 0, 60, 100, 2);
    clip_setup(s, 13, 4, CLIP_LOOP);
    clip_note(s, 13, 0, 62, 100, 1);
    clip_note(s, 13, 2, 65, 90, 1);
    clip_setup(s, 19, 8, CLIP_LOOP);         /* chords */
    clip_note(s, 19, 0, 64, 90, 4);
    clip_note(s, 19, 0, 67, 90, 4);
}

typedef struct { int block, kind; uint8_t a, role; uint16_t bpm; } act_t;
/* kind: 0 launch (clip's own quantize), 1 launch now, 2 stop role, 3 tempo */
static const act_t SESSION[] = {
    { 1,   0, 12, ROLE_LEAD,   0 },
    { 3,   0,  7, ROLE_BASS,   0 },
    { 5,   0,  0, ROLE_DRUMS,  0 },
    { 20,  1, 13, ROLE_LEAD,   0 },
    { 50,  3,  0, 0,         900 },
    { 70,  2,  1, ROLE_BASS,   0 },
    { 80,  0, 19, ROLE_CHORDS, 0 },
    { 120, 2,  0, ROLE_DRUMS,  0 },
};
#define NSESSION ((int)(sizeof SESSION / sizeof SESSION[0]))
#define BLK 1024u
#define NBLK 200

static void apply_actions(int block, clip_engine_t *e, take_t *tap, uint32_t base)
{
    int i;
    for (i = 0; i < NSESSION; i++) {
        if (SESSION[i].block != block)
            continue;
        switch (SESSION[i].kind) {
        case 0:
            clip_engine_launch(e, SESSION[i].a);
            if (tap)
                take_feed(tap, base, TAKE_LAUNCH(SESSION[i].a, TAKE_LAUNCH_QUEUE, SESSION[i].role));
            break;
        case 1:
            clip_engine_launch_now(e, SESSION[i].a);
            if (tap)
                take_feed(tap, base, TAKE_LAUNCH(SESSION[i].a, TAKE_LAUNCH_NOW, SESSION[i].role));
            break;
        case 2:
            clip_engine_stop(e, SESSION[i].a);
            if (tap)
                take_feed(tap, base, TAKE_LAUNCH(CLIP_NONE, TAKE_LAUNCH_STOP, SESSION[i].role));
            break;
        case 3:
            clip_engine_queue_bpm(e, SESSION[i].bpm);
            if (tap)
                take_feed(tap, base, TAKE_MARK_TEMPO(SESSION[i].bpm));
            break;
        default:
            break;
        }
    }
}

static void replay_ev(void *ctx, uint32_t off, uint32_t cmd)
{
    clip_engine_t *e = (clip_engine_t *)ctx;
    (void)off;
    if (TAKE_CMD_TYPE(cmd) == TAKE_T_LAUNCH) {
        uint32_t b = TAKE_CMD_B(cmd);
        if (b == TAKE_LAUNCH_QUEUE)
            clip_engine_launch(e, (uint8_t)TAKE_CMD_A(cmd));
        else if (b == TAKE_LAUNCH_NOW)
            clip_engine_launch_now(e, (uint8_t)TAKE_CMD_A(cmd));
        else if (b == TAKE_LAUNCH_STOP)
            clip_engine_stop(e, (uint8_t)TAKE_CMD_C(cmd));
    } else if (TAKE_CMD_TYPE(cmd) == TAKE_T_MARK && TAKE_CMD_A(cmd) == 2u) {
        clip_engine_queue_bpm(e, (uint16_t)TAKE_MARK_TEMPO_GET(TAKE_CMD_B(cmd), TAKE_CMD_C(cmd)));
    }
}

static const take_sink_t PLAY_SINK = { replay_ev, &e2 };

static void case_clip_engine_parity(void)
{
    take_t *t = &tk;
    int block, i, ok = 1;

    outn = 0;
    putf("# case clip_engine_parity\n");
    song_setup(&s1);
    song_clips(&s1);
    song_setup(&s2);
    song_clips(&s2);
    clip_engine_init(&e1, &s1);
    clip_engine_init(&e2, &s2);
    clip_engine_start(&e1);
    clip_engine_start(&e2);
    cn1 = cn2 = 0;

    take_init(t);
    take_arm(t, 0u);
    for (block = 0; block < NBLK; block++) {
        uint32_t base = (uint32_t)block * BLK;
        apply_actions(block, &e1, t, base);
        cev_base1 = base;
        clip_engine_render(&e1, BLK, &(const clip_sink_t){ cev_sink, &COL1 });
    }
    take_stop(t, (uint32_t)NBLK * BLK);
    take_tick(t, (uint32_t)NBLK * BLK);

    /* replay the recorded launches into a second engine */
    take_play(t, 0u);
    for (block = 0; block < NBLK; block++) {
        cev_base2 = (uint32_t)block * BLK;
        take_render(t, BLK, &PLAY_SINK);
        clip_engine_render(&e2, BLK, &(const clip_sink_t){ cev_sink, &COL2 });
    }

    putf("take nevents=%u recorded_events=%d replayed_events=%d\n",
         (unsigned)take_nevents(t), cn1, cn2);
    if (cn1 != cn2) {
        ok = 0;
        failf("clip engine event counts differ: %d vs %d", cn1, cn2);
    } else {
        for (i = 0; i < cn1; i++) {
            if (cev1[i].sample != cev2[i].sample || cev1[i].kind != cev2[i].kind ||
                cev1[i].role != cev2[i].role || cev1[i].a != cev2[i].a ||
                cev1[i].b != cev2[i].b || cev1[i].c != cev2[i].c) {
                ok = 0;
                failf("clip engine event %d differs: (%u,%u) vs (%u,%u)", i,
                      cev1[i].sample, cev1[i].kind, cev2[i].sample, cev2[i].kind);
                break;
            }
        }
    }
    for (i = 0; i < cn1; i++)
        putf("ev %u %u %u %u %u %u\n", cev1[i].sample, cev1[i].kind, cev1[i].role,
             cev1[i].a, cev1[i].b, cev1[i].c);
    putf("# ok=%d\n", ok);
    expect_golden("clip_engine_parity");
}

/* ---- main ------------------------------------------------------------------- */

int main(void)
{
    printf("take test\n");

    case_record_replay();
    case_bar_boundaries();
    case_prestart_drop();
    case_prelude_restore();
    case_full_buffer();
    case_counter_wrap();
    case_dense_window();
    case_stop_once();
    case_clip_engine_parity();

    if (fails) {
        printf("take: FAIL (%d)\n", fails);
        return 1;
    }
    printf("take: all cases pass\n");
    return 0;
}
