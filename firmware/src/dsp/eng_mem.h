/* SPDX-License-Identifier: GPL-3.0-only
 * fm1-rocket-os T5c: tiny memory helpers for the ported engines.
 * The JieLi toolchain (pi32v2 clang 4.0.1) has no <string.h>, and X0X's DSP files avoid it. These are
 * the few operations the engines need; they behave exactly like libc's for the byte counts used, so the
 * host tests and the device agree. No heap, no float. */
#pragma once

static inline void eng_memcpy(void *d, const void *s, unsigned n)
{
    unsigned char *a = (unsigned char *)d;
    const unsigned char *b = (const unsigned char *)s;
    while (n--)
        *a++ = *b++;
}

static inline void eng_memset(void *d, int c, unsigned n)
{
    unsigned char *a = (unsigned char *)d;
    while (n--)
        *a++ = (unsigned char)c;
}

static inline int eng_memcmp(const void *a, const void *b, unsigned n)
{
    const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;
    while (n--) {
        if (*x != *y)
            return (int)*x - (int)*y;
        x++;
        y++;
    }
    return 0;
}

static inline void eng_memmove(void *d, const void *s, unsigned n)
{
    unsigned char *a = (unsigned char *)d;
    const unsigned char *b = (const unsigned char *)s;
    if (a <= b) {
        while (n--)
            *a++ = *b++;
    } else {
        a += n;
        b += n;
        while (n--)
            *--a = *--b;
    }
}
