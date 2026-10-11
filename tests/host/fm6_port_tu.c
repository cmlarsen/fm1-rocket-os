/* SPDX-License-Identifier: GPL-3.0-only
 * fm1-rocket-os T5c: the FM6 port translation unit. It includes the ported fm6_core.h (firmware/src/dsp)
 * and the same note script as the reference, so a byte that differs is a byte the port changed. */
#include "fm6_core.h"
#include "fm6_script.h"

void fm6_port_run(const uint8_t *p, int note, int vel, uint32_t blocks, int32_t *out)
{
    fm6_script(p, note, vel, blocks, out);
}
