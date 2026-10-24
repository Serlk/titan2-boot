/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
#ifndef TITAN2_BOOT_MEMMAP_H
#define TITAN2_BOOT_MEMMAP_H

#include "io.h"

/*
 * Physical memory below 4 GiB as LK leaves it: DRAM from /memory, minus every
 * /reserved-memory reg (LK's mblock-* nodes: BL31, TEE, GZ, modem, SCP, ...).
 * Our own areas (the loader and its payloads, LK's FDT, Android's ramdisk)
 * are marked busy, and alloc() hands out the rest.
 */
#define MEMMAP_MAX	128
#define MEM_LIMIT	0x100000000ULL	/* the MMU maps the low 4 GiB only */

enum mem_type {
	MEM_FREE,	/* DRAM nobody uses */
	MEM_BUSY,	/* DRAM we use (loader, payloads, LK's FDT, allocations) */
	MEM_RESERVED,	/* LK reserved it: never touched, not even mapped cacheable */
	MEM_FB,		/* LK's framebuffer (reserved, we draw into it) */
};

struct mem_range {
	u64 start, end;	/* [start, end) */
	enum mem_type type;
	const char *name;
};

int memmap_init(const void *lk_fdt);	/* 0 or < 0 */
void memmap_mark(u64 start, u64 end, enum mem_type type, const char *name);
/* lowest free range of size bytes, aligned, at or above min; 0 if none */
u64 memmap_alloc(u64 size, u64 align, u64 min, const char *name);
/* type of [start, end): FREE/BUSY only if all of it is; else the "worst" type in it */
enum mem_type memmap_type(u64 start, u64 end);
int memmap_count(void);
const struct mem_range *memmap_get(int i);
void memmap_log(void);

#endif
