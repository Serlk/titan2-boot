/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
/*
 * Data cache maintenance (ARM ARM D7.5 / the usual U-Boot/Linux idiom).
 *
 * Only by address: set/way operations are deliberately absent (under a
 * hypervisor they can drop other worlds' lines in shared caches).
 * dcache_range(op, start, end): op 0 = DC IVAC, 1 = DC CIVAC, 2 = DC CVAC,
 * by virtual address (== physical here), to the Point of Coherency.
 */
	.text

	.globl dcache_range
dcache_range:
	mrs	x3, ctr_el0
	ubfx	x3, x3, #16, #4		/* DminLine: log2(words) */
	mov	x4, #4
	lsl	x4, x4, x3		/* line size in bytes */
	sub	x5, x4, #1
	bic	x1, x1, x5
1:	cmp	x1, x2
	b.hs	4f
	cbz	x0, 2f
	cmp	x0, #1
	b.eq	3f
	dc	cvac, x1
	b	5f
2:	dc	ivac, x1
	b	5f
3:	dc	civac, x1
5:	add	x1, x1, x4
	b	1b
4:	dsb	sy
	ret
