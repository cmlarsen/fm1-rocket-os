/* SPDX-License-Identifier: GPL-3.0-only */
/* T7b: the host's nest-safe critical-section model. The device wraps irq_save / irq_restore in
 * fm1_flash.h (a software count, since the pi32v2 status does not expose the interrupt-enable bit);
 * here the same: enter returns the previous state, exit restores it, so an inner exit never
 * unmasks while an outer section still holds. plat_crit_active() is for the nesting test. */
#include <stdint.h>

static uint32_t crit_nest;

uint32_t plat_crit_enter(void)
{
    uint32_t prev = crit_nest;
    crit_nest++;
    return prev;
}

void plat_crit_exit(uint32_t prev)
{
    crit_nest = prev;
}

int plat_crit_active(void) { return crit_nest != 0; }
