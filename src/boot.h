/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
#ifndef TITAN2_BOOT_BOOT_H
#define TITAN2_BOOT_BOOT_H

#include "conf.h"
#include "fs.h"

/* LK's mb_ramdisk: Android's ramdisk (vendor_boot + init_boot + bootconfig) */
#define RAMDISK_ADDR	0x66f00000UL
#define RAMDISK_SIZE	0x04000000UL
#define LK_FDT_ADDR	0x47c80000UL	/* where LK puts its FDT (mb_dtb) */

/* bootimg.c: the (compressed) kernel of a boot.img partition */
long bootimg_kernel_size(struct blk *part, u64 *offset);
long bootimg_kernel(struct blk *part, void *dst, u64 max);

/* boot.c */
void boot_set_disk(struct blk *disk);	/* the UFS user LU, for ANDROID part:<name> */
/*
 * Load and start an entry. Returns only on an error, with a message for the
 * screen; the MMU may be off then and the caller must not rely on it.
 */
const char *boot_entry(const struct boot_entry *e, const void *lk_fdt);

#endif
