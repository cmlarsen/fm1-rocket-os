/* SPDX-License-Identifier: GPL-3.0-only
 * fm1-rocket-os T5c: NOISE bit-identical parity test.
 *
 * tests/host/run_noise.sh fetches Felucca's ORIGINAL dsp.c and eng_noise.c from git into build/ref/ and
 * builds noise_ref_tu.c (the reference). noise_port_tu.c builds the ported DSP from firmware/src/dsp.
 * Both run the same note case (tests/host/noise_case.h): every preset, several notes, two filter shapes.
 * The output must be byte for byte the same. Built by `make unit-engines`. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "fm6_tables.h"          /* pitch_inc */
#include "noise_case.h"
#include "eng_noise.h"

void noise_ref_run(const noise_case_t *c, int32_t *out, uint32_t n);
void noise_port_run(const noise_case_t *c, int32_t *out, uint32_t n);

#define NS 512u
static int bad;

static void check(const char *what, int ok)
{
    printf("noise: %-70s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}

/* Felucca's NOISE presets {MODE, COLR, FREQ, RES, TRK, DENS, DRFT, CRSH} */
static const int16_t PRESETS[4][8] = {
    {0, 64, 70, 100, 127, 0, 70, 0},      /* WIND (ANLG) */
    {1, 30, 88, 80, 64, 105, 40, 0},      /* RAIN (DUST) */
    {2, 0, 127, 0, 127, 112, 0, 84},      /* ARCADE (LFSR) */
    {3, 0, 100, 40, 100, 32, 0, 0},       /* METAL (META) */
};

int main(void)
{
    static int32_t ref[NS], port[NS];
    static const int notes[] = {36, 48, 60, 72};
    noise_case_t c;
    uint32_t pi, ni, si, i;
    uint64_t ncmp = 0;
    long peak = 0;

    memset(&c, 0, sizeof c);
    for (pi = 0; pi < 4u; pi++) {
        memcpy(c.p, PRESETS[pi], sizeof c.p);
        for (ni = 0; ni < sizeof notes / sizeof notes[0]; ni++) {
            c.note = notes[ni];
            c.fine = 0;
            c.pitch16 = notes[ni] * 16;
            c.inc = pitch_inc((uint32_t)c.pitch16);
            c.amp0 = c.amp1 = 32767;
            for (si = 0; si < 2u; si++) {
                c.cutoff = si ? 40 << 8 : 0;
                c.shape = si ? 80 << 8 : 64 << 8;
                memset(ref, 0, sizeof ref);
                memset(port, 0, sizeof port);
                noise_ref_run(&c, ref, NS);
                noise_port_run(&c, port, NS);
                if (memcmp(ref, port, sizeof ref)) {
                    char b[96];
                    snprintf(b, sizeof b, "preset %u note %d shape %u is bit-identical", pi, notes[ni], si);
                    check(b, 0);
                } else {
                    ncmp++;
                }
                for (i = 0; i < NS; i++)
                    if (port[i] > peak || -port[i] > peak)
                        peak = port[i] > 0 ? port[i] : -port[i];
            }
        }
    }
    check("every preset x note x shape is bit-identical", ncmp == 4u * 4u * 2u);
    check("no output beyond 2 x VOICE_FS", peak <= 2 * 24000);

    /* determinism: the same note from silence renders the same samples twice */
    {
        static int32_t a[NS], b[NS];
        memcpy(c.p, PRESETS[0], sizeof c.p);
        c.note = 60;
        c.pitch16 = 60 * 16;
        c.inc = pitch_inc((uint32_t)c.pitch16);
        c.amp0 = c.amp1 = 32767;
        c.cutoff = 0;
        c.shape = 64 << 8;
        memset(a, 0, sizeof a);
        memset(b, 0, sizeof b);
        noise_port_run(&c, a, NS);
        noise_port_run(&c, b, NS);
        check("a note from silence is deterministic", !memcmp(a, b, sizeof a));
    }

    /* the adapter: playable, and silent after note-off + release */
    {
        static int32_t out[NS];
        long p2 = 0;
        uint32_t iters = 0;
        eng_noise_init();
        eng_noise_set_env(5, 100, 70, 50);
        eng_noise_note_on(60, 110);
        check("the adapter has a sounding voice", eng_noise_voices_active() == 1);
        memset(out, 0, sizeof out);
        eng_noise_render(out, NS);
        for (i = 0; i < NS; i++)
            if (out[i] > p2 || -out[i] > p2)
                p2 = out[i] > 0 ? out[i] : -out[i];
        check("the adapter renders sound", p2 > 0);
        eng_noise_note_off(60);
        while (eng_noise_voices_active() && iters < 4096u) {
            memset(out, 0, sizeof out);
            eng_noise_render(out, 64u);
            iters++;
        }
        check("the released voice ends", eng_noise_voices_active() == 0);
    }

    printf("noise parity: %llu comparisons\n", (unsigned long long)ncmp);
    if (bad)
        printf("NOISE PARITY FAILED (%d)\n", bad);
    else
        printf("NOISE parity ok\n");
    return bad ? 1 : 0;
}
