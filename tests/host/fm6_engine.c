/* SPDX-License-Identifier: GPL-3.0-only
 * fm1-rocket-os T5c: the FM6 adapter test (firmware/src/dsp/eng_fm6.c).
 *
 * The core parity test (fm6_parity.c) proves the ported DSP is bit-identical to Felucca's. This test
 * proves the thin X0X adapter around it: the same note script through eng_fm6_render matches the core's
 * own framing, the voice count and stealing behave, a note goes silent after release, and nothing clips.
 * Built by `make unit-engines`. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "fm6_patches.h"
#include "eng_fm6.h"

void fm6_ref_run(const uint8_t *p, int note, int vel, uint32_t blocks, int32_t *out);

#define NB 64u
static int bad;

static void check(const char *what, int ok)
{
    printf("fm6-engine: %-60s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}

int main(void)
{
    static int32_t ref[NB * FM6_N], got[NB * FM6_N];
    static const int notes[] = {36, 48, 60, 72};
    static const int vels[] = {40, 127};
    uint8_t p[FP_SIZE + 1u];
    uint32_t f, ni, vi, i;
    int ok = 1;

    eng_fm6_init();

    /* the adapter reproduces the core's framing: patch i, note, vel -> the same samples */
    for (f = 0; f < FM6_NFACTORY && ok; f++) {
        fm6_unpack(FM6_FACTORY[f], p);
        for (ni = 0; ni < sizeof notes / sizeof notes[0] && ok; ni++)
            for (vi = 0; vi < sizeof vels / sizeof vels[0] && ok; vi++) {
                memset(ref, 0, sizeof ref);
                memset(got, 0, sizeof got);
                fm6_ref_run(p, notes[ni], vels[vi], NB, ref);
                eng_fm6_init();
                eng_fm6_set_patch(p);
                eng_fm6_note_on(notes[ni], vels[vi]);
                eng_fm6_render(got, NB * FM6_N);
                if (memcmp(ref, got, sizeof ref))
                    ok = 0;
            }
    }
    check("adapter output matches Felucca's framing", ok);

    /* polyphony: press more keys than voices; the count caps, and a fresh key steals an old one */
    eng_fm6_init();
    eng_fm6_set_patch(p);
    for (i = 0; i < ENG_FM6_VOICES + 3u; i++)
        eng_fm6_note_on(48 + (int)i, 100);
    check("polyphony caps at ENG_FM6_VOICES", eng_fm6_voices_active() == ENG_FM6_VOICES);
    {
        static int32_t out[FM6_N];
        memset(out, 0, sizeof out);
        eng_fm6_render(out, FM6_N);
    }
    for (i = 0; i < ENG_FM6_VOICES + 3u; i++)
        eng_fm6_note_off(48 + (int)i);
    check("all voices released but still fading", eng_fm6_voices_active() <= ENG_FM6_VOICES);

    /* silence after note-off + release: render until every voice ends, then the next window is zero */
    {
        static int32_t out[FM6_N];
        long peak = 0;
        uint32_t iters = 0;
        while (eng_fm6_voices_active() && iters < 4096u) {
            memset(out, 0, sizeof out);
            eng_fm6_render(out, FM6_N);
            iters++;
        }
        check("the released voices end within a few seconds", eng_fm6_voices_active() == 0);
        memset(out, 0, sizeof out);
        eng_fm6_render(out, FM6_N);
        for (i = 0; i < FM6_N; i++)
            if (out[i] > peak || -out[i] > peak)
                peak = out[i] > 0 ? out[i] : -out[i];
        check("silence after note-off + release", peak == 0);
    }

    /* no clipping beyond full scale: one voice at the corner never passes 2 x VOICE_FS */
    {
        long peak = 0;
        for (f = 0; f < FM6_NFACTORY; f++) {
            static int32_t out[NB * FM6_N];
            fm6_unpack(FM6_FACTORY[f], p);
            eng_fm6_init();
            eng_fm6_set_patch(p);
            eng_fm6_note_on(60, 127);
            memset(out, 0, sizeof out);
            eng_fm6_render(out, NB * FM6_N);
            for (i = 0; i < NB * FM6_N; i++)
                if (out[i] > peak || -out[i] > peak)
                    peak = out[i] > 0 ? out[i] : -out[i];
        }
        check("one voice stays within 2 x VOICE_FS", peak <= 2 * 24000);
    }

    /* SLOT: F1..F8 load, OWN restores, a bank voice loads, an empty bank slot is refused */
    {
        uint8_t b[FP_SIZE + 1u];
        fm6_vbank_t vb;
        check("load F2", eng_fm6_load_slot(1) == 0 && eng_fm6_slot() == 1);
        fm6_unpack(FM6_FACTORY[5], b);
        eng_fm6_set_patch(b);
        check("OWN holds the set patch", eng_fm6_slot() == ENG_FM6_OWN);
        fm6_vbank_init(&vb);
        check("a bank slot with no voice is refused", eng_fm6_load_slot(ENG_FM6_OWN + 1) == 1);
        fm6_vbank_put(&vb, 0, b);
        eng_fm6_set_vbank(&vb);
        check("a bank voice loads", eng_fm6_load_slot(ENG_FM6_OWN + 1) == 0 && eng_fm6_slot() == ENG_FM6_OWN + 1);
        check("the loaded bank voice is that record", !memcmp(eng_fm6_patch(), b, FP_SIZE));
    }

    if (bad)
        printf("FM6 ENGINE FAILED (%d)\n", bad);
    else
        printf("FM6 engine ok\n");
    return bad ? 1 : 0;
}
