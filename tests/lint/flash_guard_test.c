/* SPDX-License-Identifier: GPL-3.0-only
 * T2b: the flash write path refuses the protected regions.
 *
 * X0X guards every app flash erase/program with FL_STORE_OK (firmware/hal/fm1_flash.h), called from
 * st_erase / st_prog in firmware/src/x0x.c before fl_erase4k / fl_write. FL_STORE_OK allows only the
 * object store (0x97000..0xE0000) and the settings block (0xFC000..0xFF000). This test pins that:
 * the boot area 0x0000..0x3FFF, the stock VM backup 0xE8000, the BTIF region 0xE9000 and the key/MAC
 * page 0xFF000 must all be refused, and the real object sectors must be accepted.
 *
 * Host build: fm1_flash.h pulls in the register/RAM-code definitions, so neutralise the attributes
 * and only ever call the range macro (the RAM functions are never referenced). */
#define __attribute__(x)
#include <stdio.h>
#include "fm1_flash.h"

static int fails;

static void want_refuse(uint32_t off, uint32_t n, const char *what)
{
    if (FL_STORE_OK(off, n)) {
        printf("FAIL: write allowed in %s (%#x + %u)\n", what, off, n);
        fails++;
    }
}

static void want_allow(uint32_t off, uint32_t n, const char *what)
{
    if (!FL_STORE_OK(off, n)) {
        printf("FAIL: write refused in %s (%#x + %u)\n", what, off, n);
        fails++;
    }
}

int main(void)
{
    /* the regions that must never be written */
    want_refuse(0x0000u, 1u, "header");
    want_refuse(0x0000u, 0x4000u, "boot area");
    want_refuse(0x3FFFu, 1u, "boot area top");
    want_refuse(0xE8000u, 1u, "stock VM backup");
    want_refuse(0xE8000u, 0x1000u, "stock VM backup sector");
    want_refuse(0xE9000u, 1u, "BTIF");
    want_refuse(0xE9000u, 0x1000u, "BTIF sector");
    want_refuse(0xFF000u, 1u, "key / MAC page");
    want_refuse(0x4000u, 1u, "app image");
    want_refuse(0x93000u, 1u, "app image top");
    want_refuse(0xE5000u, 1u, "past the OTA staging (not the app's store)");
    /* a range that starts in the store and runs into a protected one */
    want_refuse(0xDFFF0u, 0x2000u, "store running past 0xE0000");
    want_refuse(0xFEF00u, 0x200u, "settings running past 0xFF000");

    /* the app's own objects (firmware/src/storage.c st_sector) */
    want_allow(0x97000u, 0x1000u, "sound");
    want_allow(0x9D000u, 0x1000u, "song");
    want_allow(0xDC000u, 0x1000u, "panel calibration");
    want_allow(0xDE000u, 0x1000u, "motion");
    want_allow(0xFC000u, 0x1000u, "settings");

    if (fails) {
        printf("flash guard: %d FAILED\n", fails);
        return 1;
    }
    printf("flash guard: protected regions refused, store accepted\n");
    return 0;
}
