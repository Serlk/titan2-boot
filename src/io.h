/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
#ifndef TITAN2_BOOT_IO_H
#define TITAN2_BOOT_IO_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long u64;
typedef unsigned long size_t;

static inline void mmio_write32(u64 addr, u32 val)
{
	*(volatile u32 *)addr = val;
}
static inline u32 mmio_read32(u64 addr)
{
	return *(volatile u32 *)addr;
}

/* Hardware addresses, physical == what we use (MMU is off). See titan2-docs: docs/hardware. */
#define UART0_BASE	0x11001000UL	/* 8250, mmio32, 921600 */
#define FB_BASE		0xfe04f000UL	/* ARGB8888, 1440x1440 */
#define FB_W		1440
#define FB_H		1440
#define FB_STRIDE	(FB_W * 4)	/* bytes per line = 5760 */

#define KP_BASE		0x1c00e000UL	/* keypad matrix */
#define KP_MEM1		(KP_BASE + 0x04)
#define KP_MEM2		(KP_BASE + 0x08)

#define TOPRGU_BASE	0x1c00a000UL
#define TOPRGU_MODE	(TOPRGU_BASE + 0x00)	/* WDT_MODE, key 0x22 in [31:16] */
#define TOPRGU_SWRST	(TOPRGU_BASE + 0x14)
#define TOPRGU_NONRST2	(TOPRGU_BASE + 0x24)	/* syscon-reboot-mode, [3:0] */
#define TOPRGU_RST	(TOPRGU_BASE + 0x08)	/* WDT_RESTART, write 0x1971 */

/* reboot-mode values (stock DT reboot-mode node) */
#define RBMODE_RECOVERY	2
#define RBMODE_FASTBOOT	3

/* UART 8250 register offsets (x4 because mmio32) */
#define UART_THR	0x00
#define UART_LSR	0x14
#define UART_LSR_THRE	0x20

void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);
void uart_hex(u64 v);

void fb_clear(u32 argb);
void fb_puts(int col, int row, u32 fg, u32 bg, const char *s);

void wdt_kick(void);
void __attribute__((noreturn)) reboot_with_mode(int mode);

int key_vol_down(void);	/* 1 = pressed */
int key_vol_up(void);

#endif
