/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
#ifndef TITAN2_BOOT_FDTFIX_H
#define TITAN2_BOOT_FDTFIX_H

#include "io.h"

/*
 * Build the FDT for a mainline kernel in out[out_size]: the mainline DTB from
 * the pack plus what LK wrote into its own (stock) FDT. Returns 0 or a
 * negative libfdt error.
 */
int linux_fdt_build(const void *lk_fdt, const void *dtb, void *out, u64 out_size,
		    const char *cmdline, u64 initrd_start, u64 initrd_end);

/* MediaTek boot mode from /chosen atag,boot (0 normal, 2 recovery, 8 KPOC, ...), -1 if absent */
int lk_boot_mode(const void *lk_fdt);

#endif
