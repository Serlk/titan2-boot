/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
/*
 * titan2-boot: entry point packaged as an arm64 Linux Image, so the stock
 * Titan 2 LK loads and jumps to it exactly like a kernel.
 *
 * Stock LK gunzips us to 0x40000000 and enters at the first instruction with
 * MMU off, D-cache off, IRQs masked, at EL1 (GenieZone owns EL2), x0 = FDT
 * blob at 0x47c80000 (see titan2-docs: docs/lk-boot-requirements.md).
 *
 * The 64-byte arm64 Image header (Documentation/arm64/booting.rst) lets any
 * arm64 bootloader load us; LK itself does not read it, but keeping it valid
 * means mkbootimg/unpack tools and U-Boot still recognise the image.
 */
	.section .head.text, "ax"
	.globl _start
_start:
	/*
	 * code0/code1: the first two 32-bit words. On a real kernel code0 is a
	 * "MZ" so EFI sees a PE image; we just branch to the C entry. code1 is
	 * the 32-bit "b" that the arm64 header spec wants right after code0.
	 */
	b	_text_start		/* code0: executable branch */
	.long	0			/* code1 */
	.quad	0			/* text_offset (image load offset) */
	.quad	_image_size		/* image_size, filled by linker */
	.quad	0			/* flags: LE, 4K pages, 2MB anywhere */
	.quad	0			/* reserved */
	.quad	0			/* reserved */
	.quad	0			/* reserved */
	.ascii	"ARM\x64"		/* magic */
	.long	0			/* reserved (PE header offset) */

_text_start:
	/* Save the FDT pointer LK passed in x0. */
	adrp	x1, saved_fdt
	add	x1, x1, :lo12:saved_fdt
	str	x0, [x1]

	/* Set up a stack at the top of our BSS. */
	adrp	x1, _stack_top
	add	x1, x1, :lo12:_stack_top
	mov	sp, x1

	bl	bl_main

	/* bl_main should not return; if it does, spin. */
1:	wfe
	b	1b

	.section .bss.stack, "aw", @nobits
	.balign 16
	.skip 0x4000
_stack_top:

	.data
	.globl saved_fdt
	.balign 8
saved_fdt:
	.quad 0
