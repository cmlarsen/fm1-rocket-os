/* SPDX-License-Identifier: GPL-3.0-only
 * fm1-rocket-os T5c: FM6 bit-identical parity test.
 *
 * tests/host/run_fm6.sh fetches Felucca's ORIGINAL fm6_core.c and tables from git into build/ref/ and
 * builds fm6_ref_tu.c (the reference). fm6_port_tu.c builds the ported core from firmware/src/dsp.
 * Both run the same note script (tests/host/fm6_script.h): every factory patch, several notes and
 * velocities. The output must be byte for byte the same, and the port must stay in range (no clipping).
 * Built by `make unit-engines`. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "fm6_patches.h"
#include "fm6_vbank.h"

void fm6_ref_run(const uint8_t *p, int note, int vel, uint32_t blocks, int32_t *out);
void fm6_port_run(const uint8_t *p, int note, int vel, uint32_t blocks, int32_t *out);

#define NB 64u                                    /* CTL-sample blocks rendered per note */
static int bad;

static void check(const char *what, int ok)
{
    printf("fm6: %-72s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}

/* 24-bit full scale, the ceiling Felucca's own fm6_test checks against (2 x VOICE_FS a voice) */
#define CLIP (2 * 24000)

int main(void)
{
    static int32_t ref[NB * FM6_N], port[NB * FM6_N];
    static const int notes[] = {24, 36, 48, 60, 72, 84, 96};
    static const int vels[] = {30, 100, 127};
    uint8_t p[FP_SIZE + 1u];
    uint32_t f, ni, vi, i;
    uint64_t ncmp = 0;
    long peak = 0;

    for (f = 0; f < FM6_NFACTORY; f++) {
        fm6_unpack(FM6_FACTORY[f], p);
        for (ni = 0; ni < sizeof notes / sizeof notes[0]; ni++) {
            for (vi = 0; vi < sizeof vels / sizeof vels[0]; vi++) {
                memset(ref, 0, sizeof ref);
                memset(port, 0, sizeof port);
                fm6_ref_run(p, notes[ni], vels[vi], NB, ref);
                fm6_port_run(p, notes[ni], vels[vi], NB, port);
                if (memcmp(ref, port, sizeof ref)) {
                    char b[96];
                    snprintf(b, sizeof b, "patch %u note %d vel %d is bit-identical", f, notes[ni], vels[vi]);
                    check(b, 0);
                } else {
                    ncmp++;
                }
                for (i = 0; i < NB * FM6_N; i++)
                    if (port[i] > peak || -port[i] > peak)
                        peak = port[i] > 0 ? port[i] : -port[i];
            }
        }
        /* the patch survives a pack -> unpack round trip (the Python generator and the C packer agree) */
        {
            uint8_t b[FM6_PACKED], q[FP_SIZE + 1u];
            fm6_pack(p, b);
            fm6_unpack(b, q);
            if (memcmp(p, q, FP_SIZE))
                check("factory patch pack/unpack round trip", 0);
        }
    }
    check("every patch x note x velocity is bit-identical", ncmp == (uint64_t)FM6_NFACTORY * 7 * 3);
    check("no output beyond 2 x VOICE_FS (full scale)", peak <= CLIP);

    /* the 32-voice bank: put, read back, and the voices are the records' patches */
    {
        fm6_vbank_t vb;
        uint8_t r[FP_SIZE + 1u];
        fm6_unpack(FM6_FACTORY[3], p);
        fm6_vbank_init(&vb);
        check("an empty bank has no voice", fm6_vbank_read(&vb, 0, r) == 1);
        check("put voice 5", fm6_vbank_put(&vb, 5, p) == 0);
        check("read voice 5 back", fm6_vbank_read(&vb, 5, r) == 0 && !memcmp(p, r, FP_SIZE));
        check("voice 4 is still empty", fm6_vbank_read(&vb, 4, r) == 1);
        check("a bad index is refused", fm6_vbank_put(&vb, 32, p) == 1);
    }

    printf("fm6 parity: %llu comparisons\n", (unsigned long long)ncmp);
    if (bad)
        printf("FM6 PARITY FAILED (%d)\n", bad);
    else
        printf("FM6 parity ok\n");
    return bad ? 1 : 0;
}
