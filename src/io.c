/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
/*
 * Minimal bare-metal I/O for titan2-boot: UART log, ARGB8888 framebuffer text,
 * keypad read and watchdog/reboot. All hardware is already powered and
 * configured by the stock preloader/bl2_ext (titan2-docs: docs/hardware), so we only
 * touch registers, never re-init clocks or power.
 *
 * NOTE: the panel runs in DSI command mode, so writes to the framebuffer only
 * appear after a DSI trigger. Reproducing that trigger is experimental (see
 * dsi_trigger() below, disabled by default); until it is verified on hardware,
 * the UART is the reliable output.
 */
#include "io.h"

/* ---- 8x8 bitmap font: digits, upper-case, and a few symbols only ---- */
#include "font8x8.h"

void uart_init(void)
{
	/* LK already configured the UART (115200/921600). Nothing to do. */
}

void uart_putc(char c)
{
	while (!(mmio_read32(UART0_BASE + UART_LSR) & UART_LSR_THRE))
		;
	mmio_write32(UART0_BASE + UART_THR, (u32)(u8)c);
	if (c == '\n')
		uart_putc('\r');
}

void uart_puts(const char *s)
{
	for (; *s; s++)
		uart_putc(*s);
}

void uart_hex(u64 v)
{
	static const char hx[] = "0123456789abcdef";
	int i;
	uart_puts("0x");
	for (i = 60; i >= 0; i -= 4)
		uart_putc(hx[(v >> i) & 0xf]);
}

void fb_clear(u32 argb)
{
	volatile u32 *p = (volatile u32 *)FB_BASE;
	u64 n = (u64)FB_H * (FB_STRIDE / 4);
	u64 i;
	for (i = 0; i < n; i++)
		p[i] = argb;
}

/* Draw one 8x8 glyph scaled by SCALE, top-left at pixel (x,y). */
#define SCALE 3
static void fb_glyph(int x, int y, u32 fg, u32 bg, char c)
{
	const unsigned char *g = font8x8_get(c);
	int gx, gy, sx, sy;
	for (gy = 0; gy < 8; gy++) {
		for (gx = 0; gx < 8; gx++) {
			u32 col = (g[gy] >> (7 - gx)) & 1 ? fg : bg;	/* bit 7 = leftmost */
			for (sy = 0; sy < SCALE; sy++) {
				volatile u32 *row = (volatile u32 *)(FB_BASE +
					(u64)(y + gy * SCALE + sy) * FB_STRIDE +
					(u64)(x + gx * SCALE) * 4);
				for (sx = 0; sx < SCALE; sx++)
					row[sx] = col;
			}
		}
	}
}

void fb_puts(int col, int row, u32 fg, u32 bg, const char *s)
{
	int x0 = 40 + col * 8 * SCALE;
	int y = 80 + row * 10 * SCALE;
	int x = x0;
	for (; *s; s++) {
		if (*s == '\n') { y += 10 * SCALE; x = x0; continue; }
		if (x + 8 * SCALE > FB_W) { y += 10 * SCALE; x = x0; }
		fb_glyph(x, y, fg, bg, *s);
		x += 8 * SCALE;
	}
}

void wdt_kick(void)
{
	mmio_write32(TOPRGU_RST, 0x1971);
}

void __attribute__((noreturn)) reboot_with_mode(int mode)
{
	/* syscon-reboot-mode: write the mode into TOPRGU nonrst2 [3:0]. */
	u32 v = mmio_read32(TOPRGU_NONRST2);
	v = (v & ~0xfu) | (mode & 0xf);
	mmio_write32(TOPRGU_NONRST2, v);
	/* PSCI SYSTEM_RESET via SMC (stock psci { method = "smc"; }). */
	register u64 x0 asm("x0") = 0x84000009UL;
	asm volatile("smc #0" : : "r"(x0) : "memory");
	for (;;)
		asm volatile("wfe");
}

/*
 * Keypad: a matrix key is pressed when its bit in KP_MEM* reads 0.
 * Assumed: Vol Down = key 0 (KP_MEM1 bit 0), Vol Up = key 17 (KP_MEM2 bit 1).
 * Not confirmed on hardware.
 */
int key_vol_down(void)
{
	return !(mmio_read32(KP_MEM1) & (1u << 0));
}
int key_vol_up(void)
{
	return !(mmio_read32(KP_MEM2) & (1u << 1));
}
