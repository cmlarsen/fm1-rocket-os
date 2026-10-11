/* SPDX-License-Identifier: GPL-3.0-only
 * fm1-rocket-os T5c: the NOISE port translation unit. It drives the ported DSP (firmware/src/dsp) with
 * the same note case as the reference, so a byte that differs is a byte the port changed. */
#include "eng_noise.h"
#include "noise_case.h"
#include <string.h>

void noise_port_run(const noise_case_t *c, int32_t *out, uint32_t n)
{
    eng_noise_state_t st;
    eng_noise_vmod_t m;
    memset(&st, 0, sizeof st);
    memset(&m, 0, sizeof m);
    eng_noise_seed(&st, c->note, c->fine);
    m.inc = c->inc;
    m.pitch16 = c->pitch16;
    m.amp0 = c->amp0;
    m.amp1 = c->amp1;
    m.cutoff = c->cutoff;
    m.shape = c->shape;
    eng_noise_dsp(c->p, &st, out, n, &m);
}
