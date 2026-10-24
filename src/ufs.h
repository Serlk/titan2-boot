/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
#ifndef TITAN2_BOOT_UFS_H
#define TITAN2_BOOT_UFS_H

#include "fs.h"

/*
 * The UFS user LU (the one with the GPT: boot_a, boot_b, userdata, linux,
 * ...), read-only. 0 and *disk set up, or < 0 when the UFS can not be used.
 */
int ufs_open(struct blk *disk);
/* back to the state LK hands over (device powered down and in reset, host off) */
void ufs_close(void);

#ifdef T2_QEMU
/* QEMU test build: a disk image in RAM instead of the UFS */
#define T2_QEMU_DISK		0xc0000000UL
#define T2_QEMU_DISK_SIZE	0x10000000UL
void ufs_qemu_reserve(void);
#endif

#endif
