// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
/*
 * Stage 1 translation at EL1 for the loader: 4 KiB granule, 32-bit input
 * addresses (TCR.T0SZ = 32), level 1 with four 1 GiB entries; the first GiB
 * (MMIO) is one Device block, the other three point to level 2 tables of
 * 2 MiB blocks whose attributes come from the memory map.
 *
 * Why so careful: LK's reserved memory (TEE, GZ, BL31, the modem, ...) is
 * guarded by the EMI MPU, and a speculative read of it by a cacheable
 * mapping can raise violations. Device memory is never accessed
 * speculatively, so all of it is Device.
 */
#include "memmap.h"
#include "mmu.h"

#define BLOCK_2M	(2ULL << 20)
#define GIB		(1ULL << 30)

#define PTE_BLOCK	1ULL
#define PTE_TABLE	3ULL
#define PTE_ATTR(i)	((u64)(i) << 2)
#define PTE_SH_INNER	(3ULL << 8)
#define PTE_SH_OUTER	(2ULL << 8)
#define PTE_AF		(1ULL << 10)
#define PTE_PXN		(1ULL << 53)
#define PTE_UXN		(1ULL << 54)

/* MAIR_EL1 attribute indexes */
#define ATTR_DEVICE	0	/* 0x00 Device-nGnRnE */
#define ATTR_NORMAL	1	/* 0xff Normal, inner/outer write-back RW-allocate */
#define ATTR_NC		2	/* 0x44 Normal, non-cacheable */
#define MAIR_VALUE	(0x00ULL | 0xffULL << 8 | 0x44ULL << 16)

#define SCTLR_M		(1ULL << 0)
#define SCTLR_C		(1ULL << 2)
#define SCTLR_I		(1ULL << 12)

extern char _text[], _end[];

static u64 l1[512] __attribute__((aligned(4096)));
static u64 l2[3][512] __attribute__((aligned(4096)));

static u64 block_desc(u64 pa, enum mem_type t, int exec)
{
	u64 d = pa | PTE_BLOCK | PTE_AF;

	switch (t) {
	case MEM_FREE:
	case MEM_BUSY:
		d |= PTE_ATTR(ATTR_NORMAL) | PTE_SH_INNER;
		break;
	case MEM_FB:
		d |= PTE_ATTR(ATTR_NC) | PTE_SH_OUTER;
		break;
	default:
		d |= PTE_ATTR(ATTR_DEVICE);
		exec = 0;
		break;
	}
	if (!exec)
		d |= PTE_PXN | PTE_UXN;
	return d;
}

/* clean + invalidate to PoC, by address, all memory we use (memmap MEM_BUSY) */
static void clean_busy(void)
{
	int i;

	for (i = 0; i < memmap_count(); i++) {
		const struct mem_range *r = memmap_get(i);

		if (r->type == MEM_BUSY)
			dcache_range(DCACHE_CLEAN_INVAL, r->start, r->end);
	}
}

int mmu_enabled(void)
{
	u64 sctlr;

	asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
	return sctlr & SCTLR_M;
}

void mmu_enable(void)
{
	u64 code_start = (u64)_text & ~(BLOCK_2M - 1);
	u64 code_end = ((u64)_end + BLOCK_2M - 1) & ~(BLOCK_2M - 1);
	u64 mmfr0, tcr, sctlr, pa;
	int g, i, normal = 0;

	l1[0] = block_desc(0, MEM_RESERVED, 0);	/* 0..1 GiB: MMIO */
	for (g = 1; g < 4; g++) {
		for (i = 0; i < 512; i++) {
			u64 b = g * GIB + i * BLOCK_2M;
			enum mem_type t = memmap_type(b, b + BLOCK_2M);
			int exec = b >= code_start && b < code_end;

			l2[g - 1][i] = block_desc(b, t, exec);
			if (t == MEM_FREE || t == MEM_BUSY)
				normal++;
		}
		l1[g] = (u64)l2[g - 1] | PTE_TABLE;
	}
	log_hex("MMU: cacheable 2 MiB blocks ", normal);

	asm volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));
	pa = mmfr0 & 7;
	if (pa > 5)
		pa = 5;
	tcr = 32				/* T0SZ: 4 GiB */
	    | 1ULL << 8 | 1ULL << 10		/* IRGN0, ORGN0: write-back write-allocate */
	    | 3ULL << 12			/* SH0 inner shareable */
	    | 0ULL << 14			/* TG0 4 KiB */
	    | 1ULL << 23			/* EPD1: no TTBR1 walks */
	    | pa << 32;				/* IPS */

	/*
	 * Writes made with the MMU off went straight to memory; drop whatever
	 * lines the caches may still hold for our memory before it is used
	 * cached (the page tables included). By address and clean+invalidate
	 * only: set/way operations from EL1 under GenieZone could discard
	 * other worlds' dirty lines in the shared cache.
	 */
	clean_busy();
	asm volatile("ic iallu");

	asm volatile("msr mair_el1, %0" : : "r"(MAIR_VALUE));
	asm volatile("msr tcr_el1, %0" : : "r"(tcr));
	asm volatile("msr ttbr0_el1, %0" : : "r"((u64)l1));
	asm volatile("dsb ish; tlbi vmalle1; dsb ish; isb" ::: "memory");
	asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
	sctlr |= SCTLR_M | SCTLR_C | SCTLR_I;
	asm volatile("msr sctlr_el1, %0; isb" : : "r"(sctlr) : "memory");
	log_puts("MMU on\n");
}

void mmu_disable(void)
{
	u64 sctlr;

	if (!mmu_enabled())
		return;
	/* everything we wrote through the caches, to PoC (system caches included) */
	clean_busy();
	asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
	sctlr &= ~(SCTLR_M | SCTLR_C);
	asm volatile("dsb sy; msr sctlr_el1, %0; isb" : : "r"(sctlr) : "memory");
	/* and what was written (stack) between that clean and the switch */
	clean_busy();
	sctlr &= ~SCTLR_I;
	asm volatile("msr sctlr_el1, %0; isb" : : "r"(sctlr) : "memory");
	asm volatile("ic iallu; dsb ish; tlbi vmalle1; dsb ish; isb" ::: "memory");
}
