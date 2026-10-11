/* SPDX-License-Identifier: GPL-3.0-only */
/* T10c punch FX tests (post-review). Synthetic input is fed through
 * perform_process the way the ISR will, and each effect is checked against its
 * acceptance. The step / seam tests use a click-free sine so the thresholds
 * mean something; the grid-alignment test uses the sine too. Signals are
 * generated with fastmath.h (not libm), so the waveform summaries in
 * tests/golden/perform/ are the same on any host. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "../../firmware/src/dsp/perform.h"
#include "../../firmware/src/dsp/fastmath.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
                           else { printf("  ok   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define NMAX 400000
#define BLK 64

static perform_t P;
static float INL[NMAX], INR[NMAX], OUTL[NMAX], OUTR[NMAX];

/* ---- synthetic transport (integer grid, like clip_engine's) -------------- */
static uint16_t BPM10 = 1200;
static uint32_t POS;
static unsigned long long K16, K8;
static int STOPPED;

static uint32_t grid16(unsigned long long k)
{
    return (uint32_t)((k * 6615000ULL) / (unsigned long long)BPM10);
}
static uint32_t grid8(unsigned long long k) { return grid16(2ULL * k); }

/* feed nframes in place at the absolute transport position */
static void drive(uint32_t n)
{
    uint32_t done = 0;
    while (done < n) {
        uint32_t blk = n - done;
        uint32_t n8, n16;
        if (blk > BLK) blk = BLK;
        memcpy(OUTL + POS, INL + POS, blk * sizeof(float));
        memcpy(OUTR + POS, INR + POS, blk * sizeof(float));
        while (grid16(K16) < POS) K16++;
        while (grid8(K8) < POS) K8++;
        if (STOPPED) {
            n8 = n16 = PERFORM_NO_GRID;
        } else {
            n8 = grid8(K8) - POS;
            n16 = grid16(K16) - POS;
        }
        perform_process(&P, OUTL + POS, OUTR + POS, blk, n8, n16);
        POS += blk;
        done += blk;
    }
}

/* a fresh engine at bpm; the tempo command drains before the first sample */
static void start(uint16_t bpm)
{
    memset(&P, 0, sizeof P);
    perform_init(&P);
    BPM10 = bpm;
    POS = 0;
    K16 = K8 = 0;
    STOPPED = 0;
    perform_set_tempo(&P, bpm);
    perform_process(&P, INL, INR, 0u, 0u, 0u);
}

/* ---- signal helpers ------------------------------------------------------ */
static float q16(float x)
{
    if (x >= 1.0f) return 32767.0f / 32768.0f;
    if (x <= -1.0f) return -32768.0f / 32768.0f;
    return (float)(int)(x * 32767.0f) / 32768.0f;
}

static void gen_tone(float *l, float *r, uint32_t n, float hz, float amp)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        float v = amp * fm_sinf(6.28318530717959f * hz * (float)i / (float)PERFORM_FS);
        l[i] = r[i] = v;
    }
}

static float rms(const float *x, uint32_t a, uint32_t b)
{
    uint32_t i;
    float s = 0.0f;
    if (b <= a) return 0.0f;
    for (i = a; i < b; i++)
        s += x[i] * x[i];
    return fm_sqrtf(s / (float)(b - a));
}

static float max_step(const float *x, uint32_t a, uint32_t b)
{
    uint32_t i;
    float m = 0.0f;
    for (i = a + 1; i < b; i++) {
        float d = fm_fabsf(x[i] - x[i - 1]);
        if (d > m) m = d;
    }
    return m;
}

static float boundary_step(uint32_t center)
{
    uint32_t a = center > 16u ? center - 16u : 0u;
    return max_step(OUTL, a, center + 128u);
}

static int exact(const float *a, const float *b, uint32_t n)
{
    return memcmp(a, b, (size_t)n * sizeof(float)) == 0;
}

static float zc_freq(const float *x, uint32_t a, uint32_t b)
{
    uint32_t i;
    int cross = 0, have = 0;
    float prev = 0.0f;
    for (i = a; i < b; i++) {
        float v = x[i];
        if (v > 0.02f) { if (have && prev < -0.02f) cross++; prev = v; have = 1; }
        else if (v < -0.02f) { if (have && prev > 0.02f) cross++; prev = v; have = 1; }
    }
    return (float)cross * 0.5f * (float)PERFORM_FS / (float)(b - a);
}

/* ---- golden summaries ---------------------------------------------------- */
static char out[16384];
static size_t outn;
static void putf(const char *fmt, ...)
{
    va_list ap;
    int m;
    va_start(ap, fmt);
    m = vsnprintf(out + outn, sizeof out - outn, fmt, ap);
    va_end(ap);
    if (m > 0) outn += (size_t)m;
}

static int golden(const char *name, const char *body)
{
    char path[512];
    FILE *f;
    static char want[16384];
    snprintf(path, sizeof path, "%s/%s.txt",
             getenv("GOLDEN_DIR") ? getenv("GOLDEN_DIR") : "tests/golden/perform", name);
    if (getenv("GOLDEN_UPDATE")) {
        f = fopen(path, "w");
        if (!f) { printf("  FAIL cannot write %s\n", path); return 1; }
        fputs(body, f);
        fclose(f);
        printf("  UPDATE %s\n", name);
        return 0;
    }
    f = fopen(path, "r");
    if (!f) { printf("  FAIL %s: no golden %s (run GOLDEN_UPDATE=1)\n", name, path); return 1; }
    {
        size_t gl = fread(want, 1, sizeof want - 1, f);
        want[gl] = 0;
        fclose(f);
    }
    if (strcmp(want, body) != 0) {
        printf("  FAIL %s: golden mismatch\n    golden: %.120s\n    got:    %.120s\n", name, want, body);
        return 1;
    }
    printf("  ok   %s (golden)\n", name);
    return 0;
}

#define XF_BOUND(in_step) ((in_step) + 2.2f / (float)PERFORM_XFADE_IN + 0.01f)

/* ========================================================================== */

static void case_cleanup(void)
{
    printf("cleanup:\n");
    start(1200);
    gen_tone(INL, INR, 40000, 440.0f, 0.5f);
    drive(40000);
    CHECK(exact(OUTL, INL, 40000), "nothing engaged: output == input, sample for sample");
    {
        /* T10perf: the idle fast path is the only thing recording while nothing is
         * engaged. It must still write the exact mono downmix of the live signal,
         * so a repeat / freeze engaged later captures the right audio. */
        uint32_t s, n = PERFORM_HIST_N;
        int ok = 1;
        for (s = 40000u - n; s < 40000u; s++) {
            float mono = (INL[s] + INR[s]) * 0.5f;
            int16_t want = (mono >= 1.0f) ? (int16_t)32767
                         : (mono <= -1.0f) ? (int16_t)-32768
                         : (int16_t)(mono * 32767.0f);
            if (P.hist[s & (PERFORM_HIST_N - 1u)] != want) { ok = 0; break; }
        }
        CHECK(ok, "idle: the history records the mono downmix exactly (last %u samples)", (unsigned)n);
        CHECK(P.hist_w == (40000u & (PERFORM_HIST_N - 1u)),
              "idle: the write head advanced (%u)", (unsigned)P.hist_w);
        CHECK(P.hist_stalled == 0, "idle: the recording head never stalls");
    }
    outn = 0;
    putf("# cleanup: no effect engaged, output == input exactly\n");
    putf("n=40000 exact=%d\n", (int)exact(OUTL, INL, 40000));
    fails += golden("cleanup", out);
}

static void case_repeat(int fx, uint16_t bpm, const char *name)
{
    uint32_t warm, total, G, len, k;
    float in_step, eng_step, rel_step, rs_in;
    int per = 1, aligned = 1;
    float worst = 0.0f;

    len = (uint32_t)(PERFORM_FS * 300u) / bpm;
    if (fx == PERF_REPEAT_16) len = (uint32_t)(PERFORM_FS * 150u) / bpm;
    warm = 3u * ((uint32_t)(PERFORM_FS * 300u) / bpm) + 4096u;
    total = warm + 6u * len + 4096u;

    printf("repeat %s @ %u bpm:\n", fx == PERF_REPEAT_16 ? "1/16" : "1/8", (unsigned)bpm);
    start(bpm);
    gen_tone(INL, INR, total, 440.0f, 0.5f);
    drive(warm);
    if (fx == PERF_REPEAT_16) {
        while (grid16(K16) < POS) K16++;
        G = grid16(K16);
    } else {
        while (grid8(K8) < POS) K8++;
        G = grid8(K8);
    }
    in_step = max_step(INL, G - 200u, G + 300u);
    perform_engage(&P, fx, 1);
    drive(total - warm);

    for (k = 0; G + 256u + k + len < total && k < 6u * len; k++)
        if (OUTL[G + 256u + k] != OUTL[G + 256u + k + len]) { per = 0; break; }
    CHECK(per, "output repeats the captured slice exactly (period %u)", (unsigned)len);

    for (k = 100; k < len && G + k < total; k++) {
        float want = q16(INL[G - len + k]);
        float d = fm_fabsf(OUTL[G + k] - want);
        if (d > 0.0001f) { aligned = 0; if (d > worst) worst = d; }
    }
    CHECK(aligned, "the slice is sample-aligned to the grid (worst |diff| %.5f)", worst);

    eng_step = boundary_step(G);
    CHECK(eng_step <= XF_BOUND(in_step),
          "engage boundary: largest step %.4f <= input %.4f + 2 ms", eng_step, in_step);

    perform_release(&P, fx);
    {
        uint32_t rel, rel0 = POS;
        drive(256);
        rs_in = max_step(INL, rel0 - 16u, rel0 + 128u);
        rel_step = boundary_step(rel0);
        CHECK(rel_step <= XF_BOUND(rs_in),
              "release boundary: largest step %.4f <= input %.4f + 2 ms", rel_step, rs_in);
        drive(256);
        rel = POS;
        drive(2000);
        CHECK(exact(OUTL + rel, INL + rel, 2000), "release: output == input exactly after the crossfade");
    }
    outn = 0;
    putf("# repeat %s @ %u bpm (click-free input)\n", fx == PERF_REPEAT_16 ? "1/16" : "1/8", (unsigned)bpm);
    putf("period=%u grid=%u aligned=%d\n", (unsigned)len, (unsigned)G, aligned);
    putf("in_step=%.4f engage_step=%.4f release_step=%.4f\n", in_step, eng_step, rel_step);
    fails += golden(name, out);
}

static void case_tape(void)
{
    uint32_t warm = 22050, T, i;
    float f[6], in_step, eng_step, tail;
    int mono = 1;
    printf("tape stop:\n");
    start(1200);
    gen_tone(INL, INR, warm + 44100, 220.0f, 0.5f);
    drive(warm);
    T = POS;
    in_step = max_step(INL, T - 16u, T + 128u);
    perform_engage(&P, PERF_TAPE, 1);
    drive(44100);

    eng_step = boundary_step(T);
    CHECK(eng_step <= XF_BOUND(in_step),
          "engage boundary: largest step %.4f <= input %.4f + 2 ms", eng_step, in_step);
    for (i = 0; i < 6; i++)
        f[i] = zc_freq(OUTL, T + i * 4410 + 200, T + (i + 1) * 4410);
    for (i = 1; i < 6; i++)
        if (!(f[i] < f[i - 1])) mono = 0;
    CHECK(mono, "the pitch falls monotonically: %.0f %.0f %.0f %.0f %.0f %.0f Hz",
          f[0], f[1], f[2], f[3], f[4], f[5]);
    CHECK(f[0] < 230.0f && f[0] > 170.0f, "it starts near the input's 220 Hz (%.0f)", f[0]);
    tail = rms(OUTL, T + (uint32_t)(0.64f * (float)PERFORM_FS), T + (uint32_t)(0.70f * (float)PERFORM_FS));
    CHECK(tail < 0.001f, "it ends in silence (RMS %.6f after 0.64 s)", tail);

    perform_release(&P, PERF_TAPE);
    {
        uint32_t rel, rel0 = POS;
        float rs_in, rs;
        drive(256);
        rs_in = max_step(INL, rel0 - 16u, rel0 + 128u);
        rs = boundary_step(rel0);
        CHECK(rs <= XF_BOUND(rs_in), "release boundary: largest step %.4f <= input %.4f + 2 ms", rs, rs_in);
        drive(256);
        rel = POS;
        drive(2000);
        CHECK(exact(OUTL + rel, INL + rel, 2000), "release: output == input exactly after the crossfade");
    }
    outn = 0;
    putf("# tape stop @ 120 bpm\n");
    putf("hz=%.1f %.1f %.1f %.1f %.1f %.1f\n", f[0], f[1], f[2], f[3], f[4], f[5]);
    putf("in_step=%.4f engage_step=%.4f tail_rms=%.6f\n",
         in_step, eng_step, tail);
    fails += golden("tape", out);
}

static void case_filter(void)
{
    uint32_t warm = 4096, T, i;
    float rin, rout, drop, in_step, eng_step, mid_amt, re_step, rel_step;
    printf("filter dive:\n");
    start(1200);
    for (i = 0; i < warm + 140000u; i++) {
        float v = 0.5f * fm_sinf(6.2831853f * 3000.0f * (float)i / (float)PERFORM_FS);
        INL[i] = INR[i] = v;
    }
    drive(warm);
    T = POS;
    in_step = max_step(INL, T - 16u, T + 128u);
    perform_engage(&P, PERF_FILTER, 1);
    drive(140000);
    eng_step = boundary_step(T);
    CHECK(eng_step <= XF_BOUND(in_step),
          "engage boundary: largest step %.4f <= input %.4f + 2 ms", eng_step, in_step);
    rin = rms(INL, T + 26460u, T + 44100u);
    rout = rms(OUTL, T + 26460u, T + 44100u);
    drop = 20.0f * 0.30102999566f * fm_log2f(rout / (rin + 1e-12f));
    CHECK(drop < -20.0f, "energy at 3 kHz drops %.1f dB (want < -20) once held at 200 Hz", drop);
    {
        float r1 = rms(OUTL, T + 26460u, T + 33000u);
        float r2 = rms(OUTL, T + 33000u, T + 40000u);
        CHECK(fm_fabsf(r1 - r2) < 0.2f * (r1 + r2 + 1e-9f), "it holds at 200 Hz (steady, %.5f vs %.5f)", r1, r2);
    }
    perform_release(&P, PERF_FILTER);
    {
        uint32_t rel, rel0 = POS;
        float rs_in;
        drive(256);
        rs_in = max_step(INL, rel0 - 16u, rel0 + 128u);
        rel_step = boundary_step(rel0);
        CHECK(rel_step <= XF_BOUND(rs_in),
              "release boundary: largest step %.4f <= input %.4f + 2 ms", rel_step, rs_in);
        drive(5512u + 256u);
        rel = POS;
        drive(2000);
        CHECK(exact(OUTL + rel, INL + rel, 2000), "release: output == input exactly after the sweep back");
    }

    /* re-engage mid-dive continues from the current amount, without a click */
    start(1200);
    for (i = 0; i < 80000; i++) {
        float v = 0.5f * fm_sinf(6.2831853f * 3000.0f * (float)i / (float)PERFORM_FS);
        INL[i] = INR[i] = v;
    }
    drive(4096);
    T = POS;
    perform_engage(&P, PERF_FILTER, 0);
    drive(11025);                       /* half a beat: amt ~ 0.5 */
    mid_amt = P.filt_amt;
    in_step = max_step(INL, POS - 16u, POS + 128u);
    perform_engage(&P, PERF_FILTER, 0); /* re-engage mid-dive */
    drive(256);
    re_step = boundary_step(POS - 256u);
    CHECK(mid_amt > 0.3f && mid_amt < 0.7f, "half a beat in, the dive is at %.2f", mid_amt);
    CHECK(P.filt_amt >= mid_amt, "re-engaging continues from the current amount (%.2f -> %.2f)",
          mid_amt, P.filt_amt);
    CHECK(re_step <= XF_BOUND(in_step),
          "re-engage boundary: largest step %.4f <= input %.4f + 2 ms", re_step, in_step);

    outn = 0;
    putf("# filter dive @ 120 bpm, a 3 kHz tone held to 200 Hz\n");
    putf("rms_in=%.6f rms_out=%.6f drop_db=%.2f\n", rin, rout, drop);
    putf("in_step=%.4f engage_step=%.4f mid_amt=%.3f re_step=%.4f\n", in_step, eng_step, mid_amt, re_step);
    fails += golden("filter", out);
}

static void case_freeze(void)
{
    uint32_t warm = 20000, T, i, len = PERFORM_FREEZE_N, X = PERFORM_XFADE_FREEZE, period = len - X;
    int per = 1;
    float in_step, seam_step;
    printf("freeze:\n");
    start(1200);
    gen_tone(INL, INR, warm + 6u * len + 4096u, 440.0f, 0.5f);
    in_step = max_step(INL, 0, 4096);
    drive(warm);
    T = POS;
    perform_engage(&P, PERF_FREEZE, 1);
    drive(6u * len + 4096u);
    /* pass 1 plays [0, L); from pass 2 the loop is [X, L) with period L - X */
    for (i = T + len + 256u; i + period < POS; i++)
        if (OUTL[i] != OUTL[i + period]) { per = 0; break; }
    CHECK(per, "after the first pass the freeze is periodic with the %u-sample loop", (unsigned)period);
    seam_step = max_step(OUTL, T + 256u, POS);
    CHECK(seam_step <= 2.0f * in_step,
          "the seam's largest step %.4f stays within 2x the input step %.4f", seam_step, in_step);
    perform_release(&P, PERF_FREEZE);
    {
        uint32_t rel;
        drive(512);
        rel = POS;
        drive(2000);
        CHECK(exact(OUTL + rel, INL + rel, 2000), "release: output == input exactly after the crossfade");
    }
    outn = 0;
    putf("# freeze @ 120 bpm (click-free input)\n");
    putf("grain=%u wrap=%u period=%u periodic=%d\n", (unsigned)len, (unsigned)X, (unsigned)period, per);
    putf("in_step=%.4f seam_step=%.4f\n", in_step, seam_step);
    fails += golden("freeze", out);
}

static void case_replacement(void)
{
    uint32_t warm = 40000, total = 130000, k, len = 13230000u / 1200u, rel, G, rr0;
    float in_step, rep_step, rel_step;
    int per = 1, finite = 1;
    printf("engagement rules:\n");

    /* a new buffer effect replaces the old, with a crossfade (tape -> repeat) */
    start(1200);
    gen_tone(INL, INR, total, 440.0f, 0.5f);
    drive(warm);
    perform_engage(&P, PERF_TAPE, 1);
    drive(total - warm - 60000);
    while (grid8(K8) < POS) K8++;
    G = grid8(K8);
    in_step = max_step(INL, G - 16u, G + 128u);
    perform_engage(&P, PERF_REPEAT_8, 1);
    drive(60000);
    rep_step = boundary_step(G);
    CHECK(rep_step <= XF_BOUND(in_step), "replace boundary: largest step %.4f <= input %.4f + 2 ms", rep_step, in_step);
    for (k = G + 512; k + len < POS; k++)
        if (OUTL[k] != OUTL[k + len]) { per = 0; break; }
    CHECK(per, "the new repeat replaced the tape: the loop is periodic (%u)", (unsigned)len);

    /* releasing the *outgoing* effect mid-replacement fades it, never cuts it */
    start(1200);
    gen_tone(INL, INR, total, 440.0f, 0.5f);
    drive(warm);
    perform_engage(&P, PERF_REPEAT_8, 1);
    drive(4u * len);                    /* the repeat is looping */
    perform_engage(&P, PERF_FREEZE, 1);
    drive(20);                          /* the replacement crossfade is under way */
    in_step = max_step(INL, POS - 16u, POS + 128u);
    rr0 = POS;
    perform_release(&P, PERF_REPEAT_8); /* release the old, mid-replacement */
    drive(256);
    rel_step = boundary_step(rr0);
    CHECK(rel_step <= XF_BOUND(in_step),
          "releasing the old mid-replacement: largest step %.4f <= input %.4f + 2 ms", rel_step, in_step);
    perform_reset(&P);
    drive(512);
    rel = POS;
    drive(2000);
    CHECK(exact(OUTL + rel, INL + rel, 2000), "reset after the replacement is exact");

    /* filter dive and echo stack on top of a buffer effect */
    start(1200);
    gen_tone(INL, INR, total, 440.0f, 0.5f);
    drive(warm);
    perform_engage(&P, PERF_REPEAT_8, 1);
    perform_engage(&P, PERF_FILTER, 1);
    perform_echo_throw(&P, 1);
    drive(60000);
    for (k = warm; k < warm + 60000u; k++)
        if (!(OUTL[k] > -1e30f && OUTL[k] < 1e30f)) finite = 0;
    CHECK(finite, "repeat + filter dive + echo stack and stay finite");
    perform_reset(&P);
    drive(512);
    rel = POS;
    drive(2000);
    CHECK(exact(OUTL + rel, INL + rel, 2000), "reset clears the stacked effects exactly");

    /* a repeat engaged while the transport is stopped freezes the last period */
    start(1200);
    STOPPED = 1;
    gen_tone(INL, INR, total, 440.0f, 0.5f);
    drive(warm);
    perform_engage(&P, PERF_REPEAT_8, 1);
    drive(40000);
    per = 1;
    for (k = warm + 512; k + len < POS; k++)
        if (OUTL[k] != OUTL[k + len]) { per = 0; break; }
    CHECK(per, "a stopped transport has no grid: the repeat freezes the last %u-sample period", (unsigned)len);

    outn = 0;
    putf("# engagement: replace / release-old / stack / stopped\n");
    putf("replace_step=%.4f release_old_step=%.4f stacked_finite=%d stopped_periodic=%d\n",
         rep_step, rel_step, finite, per);
    fails += golden("replace", out);
}

static void case_tape_after_loop(void)
{
    uint32_t phase = 60000, warm = 20000;
    float amp;
    printf("history keeps recording under a loop:\n");

    /* a freeze plays while the input amplitude steps 0.1 -> 0.5; a tape engaged
     * then reads the *current* audio, not the frozen snapshot */
    start(1200);
    {
        uint32_t i;
        for (i = 0; i < phase + 60000; i++) {
            float amp_i = i < phase ? 0.1f : 0.5f;
            INL[i] = INR[i] = amp_i * fm_sinf(6.2831853f * 440.0f * (float)i / (float)PERFORM_FS);
        }
    }
    drive(warm);
    perform_engage(&P, PERF_FREEZE, 1);   /* freezes the 0.1-amplitude grain */
    drive(phase - warm + 20000);          /* the input has stepped to 0.5 */
    CHECK(P.hist_stalled == 0, "a freeze does not stop the history recording");
    perform_engage(&P, PERF_TAPE, 1);     /* replace the freeze with a tape stop */
    drive(400);
    {
        uint32_t t0 = POS;
        drive(4000);
        amp = rms(OUTL, t0, t0 + 4000u) * 1.41421356f;   /* peak estimate */
        CHECK(amp > 0.3f && amp < 0.7f,
              "tape after a loop reads current audio (peak %.2f, not the frozen 0.1)", amp);
    }
    perform_reset(&P);

    /* fallback: a loop that fills the whole buffer leaves nowhere to record, so
     * a tape stop plays the live signal instead of the stale snapshot */
    start(600);                            /* 60 bpm: 1/8 = 22050 > 16384, capped */
    gen_tone(INL, INR, 120000, 440.0f, 0.5f);
    drive(warm);
    perform_engage(&P, PERF_REPEAT_8, 1);
    drive(80000);
    CHECK(P.hist_stalled == 1, "a loop that fills the buffer stalls the recording head");
    {
        uint32_t t = POS;
        perform_engage(&P, PERF_TAPE, 1);
        drive(512);
        {
            uint32_t rel = POS;
            drive(2000);
            CHECK(exact(OUTL + rel, INL + rel, 2000),
                  "fallback: the tape plays the live signal, not stale audio");
        }
        (void)t;
    }
    outn = 0;
    putf("# history under a loop: recorded current audio, and the full-buffer fallback\n");
    putf("tape_after_freeze_peak=%.3f\n", amp);
    fails += golden("loop_record", out);
}

static void case_echo(void)
{
    perform_echo_t e;
    int r;
    printf("echo throw controller:\n");
    start(1200);
    r = perform_echo_targets(&P, &e);
    CHECK(r && e.send == 0u && e.fdbk == 52u && e.time == 7u && e.on == 0u,
          "init: the song's own delay settings are the base (%u,%u,%u)", e.send, e.fdbk, e.time);
    perform_echo_base(&P, 10, 30, 4);
    r = perform_echo_targets(&P, &e);
    CHECK(r && e.send == 10u && e.fdbk == 30u && e.time == 4u && e.on == 0u,
          "base update is emitted (%u,%u,%u)", e.send, e.fdbk, e.time);
    perform_echo_throw(&P, 1);
    perform_process(&P, INL, INR, 0u, 0u, 0u);      /* drain the throw */
    r = perform_echo_targets(&P, &e);
    CHECK(r && e.on == 1u && e.send == PERFORM_ECHO_SEND && e.fdbk == PERFORM_ECHO_FDBK &&
          e.time == PERFORM_ECHO_TIME,
          "held: the delay is overridden to send %u, fdbk %u, 1/8 time %u", e.send, e.fdbk, e.time);
    CHECK(perform_echo_targets(&P, &e) == 0, "targets are one-shot (no change -> 0)");
    perform_echo_throw(&P, 0);
    perform_process(&P, INL, INR, 0u, 0u, 0u);
    r = perform_echo_targets(&P, &e);
    CHECK(r && e.on == 0u && e.send == 10u && e.fdbk == 30u && e.time == 4u,
          "release: the song's own settings are restored (%u,%u,%u)", e.send, e.fdbk, e.time);

    /* a toggle that lands in the same window as a report is not lost: change the base and the
     * throw together, then drain, and the LATEST state is delivered */
    start(1200);
    perform_echo_targets(&P, &e);                 /* consume the init change */
    perform_echo_base(&P, 10, 30, 4);
    r = perform_echo_targets(&P, &e);
    CHECK(r && e.on == 0u && e.send == 10u, "toggle: base first (%u,%u,%u)", e.send, e.fdbk, e.time);
    perform_echo_throw(&P, 1);                    /* toggle on ... */
    perform_echo_base(&P, 20, 44, 2);             /* ... and a base change before the report */
    perform_process(&P, INL, INR, 0u, 0u, 0u);    /* drain the throw */
    r = perform_echo_targets(&P, &e);
    CHECK(r && e.on == 1u && e.send == PERFORM_ECHO_SEND,
          "a toggle between reports is not lost (the latest throw state wins)");
    CHECK(perform_echo_targets(&P, &e) == 0, "toggle: one-shot again");
    perform_echo_throw(&P, 0);
    perform_process(&P, INL, INR, 0u, 0u, 0u);
    r = perform_echo_targets(&P, &e);
    CHECK(r && e.on == 0u && e.send == 20u && e.fdbk == 44u && e.time == 2u,
          "and the release restores the new base (%u,%u,%u)", e.send, e.fdbk, e.time);

    /* the echo is external: engaged, the audio is untouched */
    start(1200);
    gen_tone(INL, INR, 40000, 440.0f, 0.5f);
    perform_echo_throw(&P, 1);
    drive(40000);
    CHECK(exact(OUTL, INL, 40000), "the echo controller leaves the audio untouched");
    outn = 0;
    putf("# echo throw controller: base/held/release targets\n");
    putf("send=%u fdbk=%u time=%u held_send=%u held_fdbk=%u held_time=%u\n",
         10u, 30u, 4u, (unsigned)PERFORM_ECHO_SEND, (unsigned)PERFORM_ECHO_FDBK, (unsigned)PERFORM_ECHO_TIME);
    fails += golden("echo", out);
}

static void case_ring(void)
{
    uint32_t rel, i, warm = 20000;
    uint16_t co;
    printf("command ring:\n");
    start(1200);
    gen_tone(INL, INR, warm + 8000, 440.0f, 0.5f);
    drive(warm);
    perform_engage(&P, PERF_TAPE, 1);
    drive(1000);
    CHECK(perform_latched(&P, PERF_TAPE) == 1, "the tape is engaged");
    for (i = 0; i < 20u; i++)
        perform_set_tempo(&P, (uint16_t)(1200u + i));   /* coalesced */
    perform_release(&P, PERF_TAPE);
    drive(400);
    CHECK(perform_latched(&P, PERF_TAPE) == 0, "20 tempo commands then a release: the effect is off");
    CHECK(P.bpm_x10 == 1219u, "the tempo coalesced to the latest value (%u)", (unsigned)P.bpm_x10);
    co = P.bpm_x10;
    rel = POS;
    drive(2000);
    CHECK(exact(OUTL + rel, INL + rel, 2000), "after the release the output is the input exactly");

    /* a full ring never drops a release */
    start(1200);
    drive(warm);
    perform_engage(&P, PERF_TAPE, 0);
    for (i = 0; i < 20u; i++)
        perform_engage(&P, PERF_TAPE, 0);               /* the ring fills */
    perform_release(&P, PERF_TAPE);                     /* goes to the override flag */
    drive(400);
    CHECK(perform_latched(&P, PERF_TAPE) == 0, "a release overrides a full ring");
    outn = 0;
    putf("# command ring: coalesced tempo, and a release that overrides a full ring\n");
    putf("tempo=%u latched_after_release=%d\n", (unsigned)co, perform_latched(&P, PERF_TAPE));
    fails += golden("ring", out);
}

static void case_tempo(int bpm)
{
    uint32_t warm, total, G, expect, i;
    int per = 1;
    char name[32];
    printf("tempo %u bpm:\n", (unsigned)bpm);
    start((uint16_t)bpm);
    expect = (uint32_t)(PERFORM_FS * 300u) / (uint32_t)bpm;
    if (expect > PERFORM_HIST_N) expect = PERFORM_HIST_N;
    warm = 2u * ((uint32_t)(PERFORM_FS * 300u) / (uint32_t)bpm) + 4096u;
    total = warm + 5u * expect + 4096u;
    gen_tone(INL, INR, total, 440.0f, 0.5f);
    drive(warm);
    while (grid8(K8) < POS) K8++;
    G = grid8(K8);
    perform_engage(&P, PERF_REPEAT_8, 1);
    drive(total - warm);
    for (i = G + 256; i + expect < total; i++)
        if (OUTL[i] != OUTL[i + expect]) { per = 0; break; }
    if ((uint32_t)(PERFORM_FS * 300u) / (uint32_t)bpm > PERFORM_HIST_N)
        CHECK(per, "%u bpm: a 1/8 note is capped at the longest that fits (%u samples)", (unsigned)bpm, (unsigned)expect);
    else
        CHECK(per, "%u bpm: the 1/8 slice is %u samples", (unsigned)bpm, (unsigned)expect);
    snprintf(name, sizeof name, "tempo_%u", (unsigned)bpm);
    outn = 0;
    putf("# repeat 1/8 @ %u bpm\n", (unsigned)bpm);
    putf("period=%u grid=%u capped=%d\n", (unsigned)expect, (unsigned)G,
         (int)((uint32_t)(PERFORM_FS * 300u) / (uint32_t)bpm > PERFORM_HIST_N));
    fails += golden(name, out);
}

int main(void)
{
    case_cleanup();
    case_repeat(PERF_REPEAT_8, 1200, "repeat_8");
    case_repeat(PERF_REPEAT_16, 1200, "repeat_16");
    case_tape();
    case_filter();
    case_freeze();
    case_replacement();
    case_tape_after_loop();
    case_echo();
    case_ring();
    case_tempo(600);
    case_tempo(1200);
    case_tempo(2000);

    printf("perform: %s\n", fails ? "FAIL" : "ok");
    return fails != 0;
}
