/* SPDX-License-Identifier: GPL-3.0-only */
/* T7b re-review: the host plat_crit is nest-safe — an inner exit must not unmask while an outer
 * section still holds, and the outer exit restores the previous state. The device's irq_save /
 * irq_restore in fm1_flash.h follow the same rule. */
#include <stdio.h>
#include <stdint.h>

extern uint32_t plat_crit_enter(void);
extern void plat_crit_exit(uint32_t prev);
extern int plat_crit_active(void);

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void)
{
    uint32_t a, b;

    CHECK(!plat_crit_active());
    a = plat_crit_enter();                    /* the outer section masks */
    CHECK(a == 0 && plat_crit_active());
    b = plat_crit_enter();                    /* nested: the previous state was "masked" */
    CHECK(b == 1 && plat_crit_active());
    plat_crit_exit(b);                        /* the inner exit must NOT unmask */
    CHECK(plat_crit_active());
    plat_crit_exit(a);                        /* the outer exit restores */
    CHECK(!plat_crit_active());

    a = plat_crit_enter();                    /* a balanced nest of three */
    b = plat_crit_enter();
    plat_crit_exit(b);
    plat_crit_exit(a);
    CHECK(!plat_crit_active());

    if (fails) {
        printf("crit: %d FAILED\n", fails);
        return 1;
    }
    printf("crit: nesting restores the previous state\n");
    return 0;
}
