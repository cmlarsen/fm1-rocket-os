/* SPDX-License-Identifier: GPL-3.0-only
 * fm1-rocket-os T5c: the FM6 reference translation unit. It includes Felucca's ORIGINAL fm6_core.c and
 * tables, fetched at test time into build/ref/ by tests/host/run_fm6.sh. Nothing of the port is here. */
#include "felucca_tables.h"
#include "fm6_core.c"
#include "fm6_script.h"

void fm6_ref_run(const uint8_t *p, int note, int vel, uint32_t blocks, int32_t *out)
{
    fm6_script(p, note, vel, blocks, out);
}
