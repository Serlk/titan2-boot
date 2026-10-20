/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
/*
 * titan2-boot link script. The stock LK gunzips us to 0x40000000 and enters
 * at _start, so we link at that address. _image_size is exported for the
 * arm64 Image header.
 */
OUTPUT_ARCH(aarch64)
ENTRY(_start)

LOAD_ADDR = 0x40000000;

SECTIONS
{
	. = LOAD_ADDR;
	_text = .;
	.head.text : { KEEP(*(.head.text)) }
	.text     : { *(.text .text.*) }
	.rodata   : { *(.rodata .rodata.*) }
	.data     : { *(.data .data.*) }
	. = ALIGN(16);
	_bss_start = .;
	.bss (NOLOAD) : { *(.bss.stack) *(.bss .bss.*) *(COMMON) }
	_bss_end = .;
	_end = .;

	/* Image header image_size: covers text+data+bss, rounded to 64 KiB. */
	_image_size = ALIGN(_end - _text, 0x10000);

	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.interp) }
}
