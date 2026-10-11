/* SPDX-License-Identifier: GPL-3.0-only */
/* T6i: the song store's flash backend, declared apart from plat.h.
 *
 * plat.h carries X0X's `NKEYS` (27); core/model/model.h carries its own (27u). song_io.c sees the
 * model, so it includes this instead of plat.h, and plat.h includes it for the device and the host
 * runner. The region is pinned to 0xA0000..0xDBFFF (fm1_flash.h FL_SONG_OK); offsets are absolute
 * flash addresses, and each call goes through X0X's own guarded erase/program path. 0 = ok. */
#pragma once
#include <stdint.h>

#define PLAT_SONG_SECTOR 0x1000u                /* one erase sector */
int plat_song_read(uint32_t off, void *dst, uint32_t n);
int plat_song_erase(uint32_t off);
int plat_song_program(uint32_t off, const void *src, uint32_t n);
