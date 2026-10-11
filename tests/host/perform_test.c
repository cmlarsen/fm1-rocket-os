/* SPDX-License-Identifier: GPL-3.0-only */
/* T10c punch FX tests. Synthetic input (a sine plus clicks on the beat) is fed
 * through perform_process the way the ISR will, and each effect is checked
 * against its acceptance: the repeat's exactness and grid alignment, the tape
 * stop's falling pitch and silence, the filter dive's >20 dB drop above 1 kHz,
 * the echo's 1/8-note taps and decaying tail, the freeze's periodicity, the
 * cleanup's exactness, and the tempo (60/120/200, plus the slow-tempo cap).
 *
 * Waveform summaries (period, RMS, dB, tap positions) are written to
 * tests/golden/perform/<case>.txt; GOLDEN_UPDATE=1 rewrites them. The numeric
 * checks are what make the test fail, the goldens record the shapes. The
 * signals are generated with fastmath.h, not libm, so the summaries are the
 * same on every host. */
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
static unsigned long long K16, K8;    /* index of the next 1/16, 1/8 grid */
static int STOPPED;

static uint32_t grid16(unsigned long long k)
{
    return (uint32_t)((k * 6615000ULL) / (unsigned long long)BPM10);
}
static uint32_t grid8(unsigned long long k) { return grid16(2ULL * k); }

/* feed nframes in place at the absolute transport position; POS advances and
 * the grid distances are recomputed each block */
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

/* a fresh engine at bpm (the tempo command drains before the first sample) */
static void start(uint16_t bpm)
{
    memset(&P, 0, sizeof P);
    perform_init(&P);
    BPM10 = bpm;
    POS = 0;
    K16 = K8 = 0;
    STOPPED = 0;
    perform_set_tempo(&P, bpm);
    perform_process(&P, INL, INR, 0u, 0u, 0u);   /* drain the tempo command only */
}

/* ---- signal helpers ------------------------------------------------------ */
static float q16(float x)                 /* the value the int16 history stores */
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

static void gen_sine_clicks(float *l, float *r, uint32_t n, uint32_t step)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        float v = 0.5f * fm_sinf(6.28318530717959f * 440.0f * (float)i / (float)PERFORM_FS);
        if (step && i % step == 0)
            v += 0.6f;
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

static int exact(const float *a, const float *b, uint32_t n)
{
    return memcmp(a, b, (size_t)n * sizeof(float)) == 0;
}

/* zero-crossing frequency with a +/-0.02 deadband (amplitude independent) */
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

static void find_peak(const float *x, uint32_t a, uint32_t b, uint32_t *at, float *amp)
{
    uint32_t i;
    float best = 0.0f;
    uint32_t bi = a;
    for (i = a; i < b; i++)
        if (fm_fabsf(x[i]) > best) { best = fm_fabsf(x[i]); bi = i; }
    *at = bi;
    *amp = best;
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

/* ========================================================================== */

static void case_cleanup(void)
{
    printf("cleanup:\n");
    start(1200);
    gen_sine_clicks(INL, INR, 40000, 11025);
    drive(40000);
    CHECK(exact(OUTL, INL, 40000), "nothing engaged: output == input, sample for sample");
    outn = 0;
    putf("# cleanup: no effect engaged, output == input exactly\n");
    putf("n=40000 exact=%d\n", (int)exact(OUTL, INL, 40000));
    fails += golden("cleanup", out);
}

static void case_repeat(int fx, uint16_t bpm, const char *name)
{
    uint32_t warm, total, G, len, k;
    float in_step, out_step;
    int per = 1, aligned = 1;
    float worst = 0.0f;

    len = (uint32_t)(PERFORM_FS * 300u) / bpm;             /* 1/8 */
    if (fx == PERF_REPEAT_16) len = (uint32_t)(PERFORM_FS * 150u) / bpm;
    if (len > PERFORM_HIST_N) len = PERFORM_HIST_N;
    warm = 3u * ((uint32_t)(PERFORM_FS * 300u) / bpm) + 4096u;
    total = warm + 6u * len + 4096u;

    printf("repeat %s @ %u bpm:\n", fx == PERF_REPEAT_16 ? "1/16" : "1/8", (unsigned)bpm);
    start(bpm);
    gen_sine_clicks(INL, INR, total, (uint32_t)(PERFORM_FS * 300u) / bpm);
    drive(warm);
    if (fx == PERF_REPEAT_16) {
        while (grid16(K16) < POS) K16++;
        G = grid16(K16);
    } else {
        while (grid8(K8) < POS) K8++;
        G = grid8(K8);
    }
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

    in_step = max_step(INL, G - 200u, G + 300u);
    out_step = max_step(OUTL, G - 200u, G + 300u);
    CHECK(out_step <= in_step + 2.2f / (float)PERFORM_XFADE_IN + 0.01f,
          "engage crossfade: no step beyond input %.4f + 2 ms (%u) -> out %.4f",
          in_step, (unsigned)PERFORM_XFADE_IN, out_step);

    perform_release(&P, fx);
    {
        uint32_t rel, rel0 = POS;
        float rs_in, rs_out;
        drive(256);
        rs_in = max_step(INL, rel0 - 8u, rel0 + 128u);
        rs_out = max_step(OUTL, rel0 - 8u, rel0 + 128u);
        CHECK(rs_out <= rs_in + 2.2f / (float)PERFORM_XFADE_OUT + 0.01f,
              "release crossfade: no step beyond input %.4f -> out %.4f", rs_in, rs_out);
        drive(256);
        rel = POS;
        drive(2000);
        CHECK(exact(OUTL + rel, INL + rel, 2000), "release: output == input exactly after the crossfade");
        out_step = out_step > rs_out ? out_step : rs_out;
    }
    outn = 0;
    putf("# repeat %s @ %u bpm\n", fx == PERF_REPEAT_16 ? "1/16" : "1/8", (unsigned)bpm);
    putf("period=%u grid=%u aligned=%d\n", (unsigned)len, (unsigned)G, aligned);
    putf("max_step_in=%.4f max_step_out=%.4f\n", in_step, out_step);
    fails += golden(name, out);
}

static void case_tape(void)
{
    uint32_t warm = 22050, T, i;
    float f[6];
    int mono = 1;
    float tail;
    printf("tape stop:\n");
    start(1200);
    gen_tone(INL, INR, warm + 44100, 220.0f, 0.5f);
    drive(warm);
    T = POS;
    perform_engage(&P, PERF_TAPE, 1);
    drive(44100);

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
        uint32_t rel;
        drive(512);
        rel = POS;
        drive(2000);
        CHECK(exact(OUTL + rel, INL + rel, 2000), "release: output == input exactly after the crossfade");
    }
    outn = 0;
    putf("# tape stop @ 120 bpm\n");
    putf("hz=%.1f %.1f %.1f %.1f %.1f %.1f\n", f[0], f[1], f[2], f[3], f[4], f[5]);
    putf("tail_rms=%.6f\n", tail);
    fails += golden("tape", out);
}

static void case_filter(void)
{
    uint32_t warm = 4096, T, i;
    float rin, rout, drop;
    printf("filter dive:\n");
    start(1200);
    for (i = 0; i < warm + 140000u; i++) {
        float v = 0.5f * fm_sinf(6.28318530717959f * 3000.0f * (float)i / (float)PERFORM_FS);
        INL[i] = INR[i] = v;
    }
    drive(warm);
    T = POS;
    perform_engage(&P, PERF_FILTER, 1);
    drive(140000);
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
        uint32_t rel;
        drive(5512u + 512u);
        rel = POS;
        drive(2000);
        CHECK(exact(OUTL + rel, INL + rel, 2000), "release: output == input exactly after the sweep back");
    }
    outn = 0;
    putf("# filter dive @ 120 bpm, a 3 kHz tone held to 200 Hz\n");
    putf("rms_in=%.6f rms_out=%.6f drop_db=%.2f\n", rin, rout, drop);
    fails += golden("filter", out);
}

static void case_echo(void)
{
    uint32_t D, i, k;
    int taps_ok = 1, dec_ok = 1;
    float amps[8];
    printf("echo throw:\n");
    start(2000);
    D = (uint32_t)(PERFORM_FS * 300u) / 2000u;      /* 1/8 at 200 bpm = 6615 */
    for (i = 0; i < D * 14; i++)
        INL[i] = INR[i] = 0.0f;
    INL[0] = INR[0] = 0.8f;                          /* one click into the echo */
    perform_engage(&P, PERF_ECHO, 1);
    drive(D * 6);                                    /* held for six taps */
    perform_release(&P, PERF_ECHO);
    drive(D * 8);                                    /* the tail decays, uncut */
    for (k = 1; k <= 7; k++) {
        uint32_t at;
        float am;
        find_peak(OUTL, k * D - 4, k * D + 5, &at, &am);
        amps[k] = am;
        if (at < k * D - 2 || at > k * D + 2) taps_ok = 0;
        if (k >= 2 && !(amps[k] < amps[k - 1])) dec_ok = 0;
    }
    CHECK(taps_ok, "taps fall at 1/8-note spacing (D = %u samples)", (unsigned)D);
    CHECK(dec_ok, "the taps decay: %.3f %.3f %.3f %.3f %.3f %.3f %.3f",
          amps[1], amps[2], amps[3], amps[4], amps[5], amps[6], amps[7]);
    CHECK(amps[6] > 0.0005f && amps[7] < amps[6], "the tail keeps decaying after release (%.4f -> %.4f)",
          amps[6], amps[7]);
    outn = 0;
    putf("# echo throw @ 200 bpm, 1/8 = %u samples\n", (unsigned)D);
    putf("taps=%.4f %.4f %.4f %.4f %.4f %.4f %.4f\n",
         amps[1], amps[2], amps[3], amps[4], amps[5], amps[6], amps[7]);
    fails += golden("echo", out);

    /* slow tempo: a 1/8 note needs more than the 16 KiB line, so it halves */
    start(1200);
    D = (uint32_t)(PERFORM_FS * 300u) / 1200u;       /* 1/8 = 11025, does not fit */
    {
        uint32_t want = D;
        while (want > PERFORM_ECHO_N - 1u) want >>= 1;
        for (i = 0; i < want * 8; i++)
            INL[i] = INR[i] = 0.0f;
        INL[0] = INR[0] = 0.8f;
        perform_engage(&P, PERF_ECHO, 1);
        drive(want * 4);
        {
            uint32_t at;
            float am;
            find_peak(OUTL, want - 4, want + 5, &at, &am);
            CHECK(at >= want - 2 && at <= want + 2,
                  "at 120 bpm a 1/8 note does not fit 16 KiB: the delay halves to %u samples", (unsigned)want);
            outn = 0;
            putf("# echo throw @ 120 bpm: 1/8 = %u > %u, halved to %u\n",
                 (unsigned)D, (unsigned)(PERFORM_ECHO_N - 1u), (unsigned)want);
            putf("tap_at=%u want=%u\n", (unsigned)at, (unsigned)want);
            fails += golden("echo_slow", out);
        }
    }
}

static void case_freeze(void)
{
    uint32_t warm = 20000, T, i, len = PERFORM_FREEZE_N;
    int per = 1;
    float a, b;
    printf("freeze:\n");
    start(1200);
    gen_sine_clicks(INL, INR, warm + 5u * len + 4096u, 11025u);
    drive(warm);
    T = POS;
    perform_engage(&P, PERF_FREEZE, 1);
    drive(5u * len + 4096u);
    for (i = T + 256; i + len < POS; i++)
        if (OUTL[i] != OUTL[i + len]) { per = 0; break; }
    CHECK(per, "the freeze output is periodic with the %u-sample grain", (unsigned)len);
    a = rms(OUTL, T + 256, T + 256 + len);
    b = rms(OUTL, T + 256 + len, T + 256 + 2u * len);
    CHECK(fm_fabsf(a - b) < 1e-4f, "consecutive grains are the same (%.5f vs %.5f)", a, b);
    perform_release(&P, PERF_FREEZE);
    {
        uint32_t rel;
        drive(512);
        rel = POS;
        drive(2000);
        CHECK(exact(OUTL + rel, INL + rel, 2000), "release: output == input exactly after the crossfade");
    }
    outn = 0;
    putf("# freeze @ 120 bpm\n");
    putf("grain=%u periodic=%d\n", (unsigned)len, per);
    putf("rms_a=%.6f rms_b=%.6f\n", a, b);
    fails += golden("freeze", out);
}

static void case_engage_rules(void)
{
    uint32_t warm = 40000, total = 130000, G, k, len = 13230000u / 1200u, rel;
    int per = 1, finite = 1;
    float in_step, out_step;
    printf("engagement rules:\n");

    /* a new buffer effect replaces the old, with a crossfade (tape -> repeat 1/8) */
    start(1200);
    gen_sine_clicks(INL, INR, total, 11025u);
    drive(warm);
    perform_engage(&P, PERF_TAPE, 1);
    drive(total - warm - 60000);
    while (grid8(K8) < POS) K8++;
    G = grid8(K8);
    perform_engage(&P, PERF_REPEAT_8, 1);
    drive(60000);
    for (k = G + 512; k + len < POS; k++)
        if (OUTL[k] != OUTL[k + len]) { per = 0; break; }
    CHECK(per, "a new buffer effect replaces the old: the repeat loop is periodic (%u)", (unsigned)len);
    in_step = max_step(INL, G - 16u, G + 128u);
    out_step = max_step(OUTL, G - 16u, G + 128u);
    CHECK(out_step <= in_step + 2.2f / (float)PERFORM_XFADE_IN + 0.02f,
          "the replacement hand-off does not click (in %.3f -> out %.3f)", in_step, out_step);
    perform_release(&P, PERF_REPEAT_8);
    drive(512);
    rel = POS;
    drive(2000);
    CHECK(exact(OUTL + rel, INL + rel, 2000), "after the replacement, release is exact again");
    outn = 0;
    putf("# engagement: tape replaced by repeat 1/8 at %u\n", (unsigned)G);
    putf("period=%u max_step_in=%.4f max_step_out=%.4f\n", (unsigned)len, in_step, out_step);
    fails += golden("replace", out);

    /* filter dive and echo stack on top of a buffer effect */
    start(1200);
    gen_sine_clicks(INL, INR, total, 11025u);
    drive(warm);
    perform_engage(&P, PERF_REPEAT_8, 1);
    perform_engage(&P, PERF_FILTER, 1);
    perform_engage(&P, PERF_ECHO, 1);
    drive(60000);
    for (k = warm; k < warm + 60000u; k++)
        if (!(OUTL[k] > -1e30f && OUTL[k] < 1e30f)) finite = 0;   /* no NaN / inf */
    CHECK(finite, "repeat + filter dive + echo stack and stay finite");
    perform_reset(&P);
    drive(512);
    rel = POS;
    drive(2000);
    CHECK(exact(OUTL + rel, INL + rel, 2000), "reset clears the stacked effects exactly");
    outn = 0;
    putf("# engagement: repeat + filter + echo stacked, then reset\n");
    putf("finite=%d exact_after_reset=%d\n", finite, (int)exact(OUTL + rel, INL + rel, 2000));
    fails += golden("stack", out);

    /* a repeat engaged while the transport is stopped falls back to the last period */
    start(1200);
    STOPPED = 1;
    gen_sine_clicks(INL, INR, total, 11025u);
    drive(warm);
    perform_engage(&P, PERF_REPEAT_8, 1);
    drive(40000);
    per = 1;
    for (k = warm + 512; k + len < POS; k++)
        if (OUTL[k] != OUTL[k + len]) { per = 0; break; }
    CHECK(per, "a stopped transport has no grid: the repeat freezes the last %u-sample period", (unsigned)len);
    outn = 0;
    putf("# engagement: repeat 1/8 engaged while stopped (no grid)\n");
    putf("period=%u periodic=%d\n", (unsigned)len, per);
    fails += golden("stopped", out);
}

static void case_reset(void)
{
    uint32_t rel;
    printf("reset (HOME+FX):\n");
    start(1200);
    gen_sine_clicks(INL, INR, 60000, 11025u);
    drive(20000);
    perform_engage(&P, PERF_FREEZE, 1);
    perform_engage(&P, PERF_FILTER, 1);
    perform_engage(&P, PERF_ECHO, 1);
    drive(4000);
    perform_reset(&P);
    drive(256);
    rel = POS;
    drive(4000);
    CHECK(exact(OUTL + rel, INL + rel, 4000), "reset returns the output to the input exactly");
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
    gen_sine_clicks(INL, INR, total, (uint32_t)(PERFORM_FS * 300u) / (uint32_t)bpm);
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
    case_echo();
    case_freeze();
    case_engage_rules();
    case_reset();
    case_tempo(600);
    case_tempo(1200);
    case_tempo(2000);

    printf("perform: %s\n", fails ? "FAIL" : "ok");
    return fails != 0;
}
