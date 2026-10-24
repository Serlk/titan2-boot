/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
#ifndef TITAN2_BOOT_MMU_H
#define TITAN2_BOOT_MMU_H

#include "io.h"

/* cache.S */
#define DCACHE_INVAL	0
#define DCACHE_CLEAN_INVAL 1
#define DCACHE_CLEAN	2
void dcache_range(int op, u64 start, u64 end);

/*
 * Identity map of the low 4 GiB from the memory map (memmap.h, must be set
 * up first): free and busy DRAM in 2 MiB blocks as normal write-back
 * memory, the framebuffer as normal non-cacheable, everything else (LK's
 * reserved memory, partly reserved blocks, MMIO) as Device-nGnRnE, so the
 * CPU never touches protected memory speculatively. Only the block with the
 * loader is executable.
 */
void mmu_enable(void);
/* clean the caches to PoC, MMU and caches off: the state a kernel expects */
void mmu_disable(void);
int mmu_enabled(void);

#endif
