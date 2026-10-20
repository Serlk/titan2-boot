/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
/*
 * titan2-boot stage-2 entry. For now this is the proof-of-life skeleton the
 * task asks for: take the FDT that the stock LK passed in x0, print facts
 * about the boot to the UART, draw a menu on the LK framebuffer, and wait.
 *
 * It does NOT yet chain-load anything: booting a kernel from here must only
 * happen once this has been verified on hardware, which is the user's call.
 * Nothing in this tree flashes or boots the device.
 */
#include "io.h"

#define NULL ((void *)0)

extern unsigned long saved_fdt;

/* Big-endian helpers: the FDT is big-endian. */
static u32 be32(const void *p)
{
	const u8 *b = p;
	return ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | b[3];
}

#define FDT_MAGIC 0xd00dfeed

struct fdt_header {
	u32 magic, totalsize, off_dt_struct, off_dt_strings;
	u32 off_mem_rsvmap, version, last_comp_version;
	u32 boot_cpuid_phys, size_dt_strings, size_dt_struct;
};

/*
 * Walk the flattened tree looking for property "name" inside the node at
 * absolute path "path" (only "/chosen" style single-level paths are needed
 * here). Returns pointer to the (big-endian) value and its length, or NULL.
 * This is deliberately tiny; the real node-copy logic will use a vendored
 * libfdt later (see our-bootloader.md).
 */
#define FDT_BEGIN_NODE 1
#define FDT_END_NODE   2
#define FDT_PROP       3
#define FDT_NOP        4
#define FDT_END        9

static const char *fdt_find_chosen_prop(const void *fdt, const char *want,
					u32 *len_out)
{
	const struct fdt_header *h = fdt;
	const u8 *strs = (const u8 *)fdt + be32(&h->off_dt_strings);
	const u32 *p = (const u32 *)((const u8 *)fdt + be32(&h->off_dt_struct));
	int depth = 0;
	int in_chosen = 0;

	for (;;) {
		u32 tag = be32(p++);
		if (tag == FDT_END)
			break;
		if (tag == FDT_BEGIN_NODE) {
			const char *nm = (const char *)p;
			int l = 0;
			while (nm[l])
				l++;
			depth++;
			/* chosen is a direct child of root (depth 2). */
			in_chosen = (depth == 2) &&
				    nm[0] == 'c' && nm[1] == 'h' && nm[2] == 'o';
			p += (l + 4) / 4;
			continue;
		}
		if (tag == FDT_END_NODE) {
			depth--;
			if (depth < 2)
				in_chosen = 0;
			continue;
		}
		if (tag == FDT_NOP)
			continue;
		if (tag == FDT_PROP) {
			u32 plen = be32(p++);
			u32 nameoff = be32(p++);
			const char *pn = (const char *)(strs + nameoff);
			const u8 *val = (const u8 *)p;
			if (in_chosen) {
				const char *a = pn, *b = want;
				while (*a && *a == *b) { a++; b++; }
				if (*a == 0 && *b == 0) {
					if (len_out)
						*len_out = plen;
					return (const char *)val;
				}
			}
			p += (plen + 3) / 4;
			continue;
		}
		break;	/* unknown tag */
	}
	return NULL;
}

static void draw_menu(int sel)
{
	static const char *items[] = {
		"ANDROID   STOCK KERNEL",
		"MAINLINE  LINUX",
		"FASTBOOT  LK",
		"RECOVERY",
		"REBOOT",
	};
	int i;
	fb_clear(0xff101014);
	fb_puts(0, 0, 0xff58a6ff, 0xff101014, "TITAN2-BOOT  STAGE 2");
	for (i = 0; i < 5; i++) {
		u32 fg = (i == sel) ? 0xff000000 : 0xffd0d0d0;
		u32 bg = (i == sel) ? 0xff58a6ff : 0xff101014;
		fb_puts(2, 2 + i, fg, bg, items[i]);
	}
	fb_puts(0, 9, 0xff808080, 0xff101014,
		"VOL-UP  MOVE    VOL-DOWN  SELECT");
}

void bl_main(void)
{
	const void *fdt = (const void *)saved_fdt;
	const char *args;
	u32 len = 0;
	int sel = 0, prev_up = 0, prev_dn = 0;
	unsigned long spin = 0;

	uart_init();
	uart_puts("\n[titan2-boot] stage 2 alive\n");
	uart_puts("[titan2-boot] fdt = ");
	uart_hex(saved_fdt);
	uart_putc('\n');

	if (fdt && be32(fdt) == FDT_MAGIC) {
		uart_puts("[titan2-boot] fdt magic OK, totalsize = ");
		uart_hex(be32((const u8 *)fdt + 4));
		uart_putc('\n');
		args = fdt_find_chosen_prop(fdt, "bootargs", &len);
		if (args) {
			uart_puts("[titan2-boot] bootargs: ");
			uart_puts(args);
			uart_putc('\n');
		}
	} else {
		uart_puts("[titan2-boot] WARNING: no valid FDT in x0\n");
	}

	draw_menu(sel);
	uart_puts("[titan2-boot] menu drawn, waiting (no chain-load yet)\n");

	/*
	 * Idle loop: keep the watchdog happy, read keys, redraw. We never jump
	 * anywhere yet — enabling chain-load is a separate, hardware-gated step.
	 */
	for (;;) {
		int up = key_vol_up();
		int dn = key_vol_down();

		if (up && !prev_up) {
			sel = (sel + 1) % 5;
			draw_menu(sel);
		}
		if (dn && !prev_dn) {
			uart_puts("[titan2-boot] selected item ");
			uart_hex(sel);
			uart_putc('\n');
			if (sel == 2)
				reboot_with_mode(RBMODE_FASTBOOT);
			if (sel == 3)
				reboot_with_mode(RBMODE_RECOVERY);
			if (sel == 4)
				reboot_with_mode(0);
			/* items 0/1 (chain-load) intentionally do nothing yet */
		}
		prev_up = up;
		prev_dn = dn;

		if ((spin++ & 0xffff) == 0)
			wdt_kick();
	}
}
