/* SPDX-License-Identifier: GPL-3.0-only
 * fm1-rocket-os T5i: the FM6 voice bank's flash import (firmware/src/dsp/fm6_vbank.c) and the ROM
 * fallback (eng_fm6.c eng_fm6_load_preset). X0X reserves 0x9F000 for the FM6 patches but never writes
 * it, so an erased or corrupt sector must be refused and the SLOT loader must still use the ROM voices.
 * Built by `make unit-fm6bank`. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "fm6_patches.h"
#include "eng_fm6.h"

static int bad;

static void check(const char *what, int ok)
{
    printf("fm6-bank: %-58s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}

int main(void)
{
    static uint8_t img[FM6_NBANK * FM6_PACKED];
    fm6_vbank_t vb;
    uint8_t v[FP_SIZE + 1u], w[FP_SIZE + 1u];
    uint32_t k;

    /* a valid bank: 32 packed voices (the ROM factory ones repeated) */
    for (k = 0; k < FM6_NBANK; k++)
        memcpy(img + k * FM6_PACKED, FM6_FACTORY[k % FM6_NFACTORY], FM6_PACKED);
    check("a valid bank imports", fm6_vbank_import(&vb, img, sizeof img) == 1);
    fm6_unpack(img + FM6_PACKED, v);
    check("B2 reads its own record", fm6_vbank_read(&vb, 1, w) == 0 && memcmp(v, w, FP_SIZE) == 0);

    /* erased flash (0xFF): every field out of range */
    memset(img, 0xFF, sizeof img);
    check("erased flash is refused", fm6_vbank_import(&vb, img, sizeof img) == 0);

    /* blank flash (0x00): the name bytes fail the round trip */
    memset(img, 0, sizeof img);
    check("blank flash is refused", fm6_vbank_import(&vb, img, sizeof img) == 0);

    /* one corrupt record (an operator rate above its max of 99) refuses the whole bank */
    for (k = 0; k < FM6_NBANK; k++)
        memcpy(img + k * FM6_PACKED, FM6_FACTORY[k % FM6_NFACTORY], FM6_PACKED);
    img[3] = 0x7F;
    check("a corrupt record is refused", fm6_vbank_import(&vb, img, sizeof img) == 0);

    /* too short to hold a bank */
    check("a short image is refused", fm6_vbank_import(&vb, img, FM6_PACKED) == 0);

    /* the ROM fallback: with no bank at all, a bank preset still loads (a factory voice) */
    eng_fm6_init();
    eng_fm6_set_vbank(0);
    fm6_unpack(FM6_FACTORY[0], v);
    check("no bank: a bank preset falls back to a ROM voice",
          eng_fm6_load_preset(ENG_FM6_B1) == 0 && eng_fm6_slot() < FM6_NFACTORY &&
          memcmp(eng_fm6_patch(), v, FP_SIZE) == 0);
    fm6_unpack(FM6_FACTORY[7], v);
    check("the fallback cycles the ROM voices",
          eng_fm6_load_preset(ENG_FM6_B1 + 7) == 0 && memcmp(eng_fm6_patch(), v, FP_SIZE) == 0);

    /* an imported (real) bank: a bank preset loads the bank voice, not a ROM one */
    for (k = 0; k < FM6_NBANK; k++)
        memcpy(img + k * FM6_PACKED, FM6_FACTORY[k % FM6_NFACTORY], FM6_PACKED);
    check("a valid bank imports (again)", fm6_vbank_import(&vb, img, sizeof img) == 1);
    eng_fm6_set_vbank(&vb);
    fm6_unpack(img + 2 * FM6_PACKED, v);
    check("a bank preset loads the bank voice",
          eng_fm6_load_preset(ENG_FM6_B1 + 2) == 0 && eng_fm6_slot() == ENG_FM6_B1 + 2 &&
          memcmp(eng_fm6_patch(), v, FP_SIZE) == 0);

    /* a bank that imported as empty (the erased / corrupt case) falls back the same way */
    fm6_vbank_init(&vb);
    eng_fm6_set_vbank(&vb);
    fm6_unpack(FM6_FACTORY[3], v);
    check("an empty bank preset falls back to a ROM voice",
          eng_fm6_load_preset(ENG_FM6_B1 + 3) == 0 && memcmp(eng_fm6_patch(), v, FP_SIZE) == 0);

    if (bad)
        printf("FM6 BANK FAILED (%d)\n", bad);
    else
        printf("FM6 bank ok\n");
    return bad ? 1 : 0;
}
