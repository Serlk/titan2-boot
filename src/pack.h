/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
/*
 * The file table appended to titan2-boot by tools/mkpack.py. LK gunzips the
 * whole image (loader + files) to LOAD_ADDR, so every file is in RAM at
 * LOAD_ADDR + offset. One of them, extlinux.conf (src/conf.c), says what the
 * menu offers. Keep in sync with tools/mkpack.py.
 *
 *   0x000000     titan2-boot (< PACK_OFFSET)
 *   PACK_OFFSET  struct pack_header
 *   ...          files, 4 KiB aligned
 */
#ifndef TITAN2_BOOT_PACK_H
#define TITAN2_BOOT_PACK_H

#include "io.h"

#ifdef T2_LOAD_ADDR
#define LOAD_ADDR	T2_LOAD_ADDR	/* QEMU test build */
#else
#define LOAD_ADDR	0x40000000UL
#endif
#define PACK_OFFSET	0x100000UL
#define PACK_MAGIC	"T2BOOTPK"
#define PACK_VERSION	2
#define PACK_MAX	32
#define PACK_CONF	"extlinux.conf"

struct pack_file {
	char name[56];		/* path without the leading '/', NUL-terminated */
	u64 offset;		/* from LOAD_ADDR */
	u64 size;
};

struct pack_header {
	char magic[8];
	u32 version;
	u32 count;
	u64 total_size;		/* everything LK unpacked, from LOAD_ADDR */
	struct pack_file file[PACK_MAX];
};

#endif
