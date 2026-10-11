/* SPDX-License-Identifier: GPL-3.0-only
 * fm1-rocket-os T5c: the NOISE reference translation unit. It includes Felucca's ORIGINAL dsp.c and
 * eng_noise.c (fetched at test time into build/ref/) plus the compat shim. Nothing of the port is here. */
#include "felucca_tables.h"
#include "felucca_eng_shim.h"
#include "dsp.c"
#include "eng_noise.c"
#include "noise_case.h"
#include <string.h>

void noise_ref_run(const noise_case_t *c, int32_t *out, uint32_t n)
{
    track_t t;
    voice_t v;
    vmod_t m;
    int i;
    memset(&t, 0, sizeof t);
    memset(&v, 0, sizeof v);
    memset(&m, 0, sizeof m);
    for (i = 0; i < 8; i++)
        t.p[P_E0 + i] = c->p[i];
    v.note = (uint8_t)c->note;
    v.fine = c->fine;
    noise_note_on(&t, &v);
    m.inc = c->inc;
    m.pitch16 = c->pitch16;
    m.amp0 = c->amp0;
    m.amp1 = c->amp1;
    m.cutoff = c->cutoff;
    m.shape = c->shape;
    noise_render(&t, &v, out, n, &m);
}
