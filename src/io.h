/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
#ifndef TITAN2_BOOT_IO_H
#define TITAN2_BOOT_IO_H

#include <stddef.h>

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long u64;

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

/* GPIO (pinctrl-mt6878.c): DIN 0x200 + 0x10 * (n / 32), bit n % 32; mode 0x300, 4 bits per pin */
#define GPIO_BASE	0x10005000UL
#define GPIO_DIR(n)	(GPIO_BASE + 0x000 + 0x10 * ((n) / 32))
#define GPIO_DIN(n)	(GPIO_BASE + 0x200 + 0x10 * ((n) / 32))
#define GPIO_MODE(n)	(GPIO_BASE + 0x300 + 0x10 * ((n) / 8))
#define IOCFG_BR_BASE	0x11d40000UL	/* IES 0x70, PD 0xa0, PU 0xb0: bits 15/16 for GPIO54/55 */
#define IOCFG_IES	(IOCFG_BR_BASE + 0x70)
#define IOCFG_PD	(IOCFG_BR_BASE + 0xa0)
#define IOCFG_PU	(IOCFG_BR_BASE + 0xb0)
/* volume keys, low = pressed (mainline DT gpio-keys, verified on the hardware) */
#define GPIO_VOL_UP	54
#define GPIO_VOL_DOWN	55

/*
 * PMIC over SPMI: PMIF software channel 2 (spmi-mtk-pmif.c, mt6878: soc_chan 2,
 * mt8195_regs: SWINF_n at 0x800 + 0x40 * n). The power key is MT6363 (slave 4)
 * TOPSTATUS bit 1, 0 = pressed (mtk-pmic-keys.c).
 */
#define PMIF_BASE		0x1cc04000UL
#define PMIF_SWINF(n)		(PMIF_BASE + 0x0800 + 0x40 * (n))
#define PMIF_SWINF2_ACC		(PMIF_SWINF(2) + 0x00)
#define PMIF_SWINF2_RDATA	(PMIF_SWINF(2) + 0x14)
#define PMIF_SWINF2_VLD_CLR	(PMIF_SWINF(2) + 0x24)
#define PMIF_SWINF2_STA		(PMIF_SWINF(2) + 0x28)
#define MT6363_SID		4
#define MT6363_TOPSTATUS	0x1e
#define MT6363_PWRKEY_DEB	(1u << 1)

#define TOPRGU_BASE	0x1c00a000UL
#define TOPRGU_MODE	(TOPRGU_BASE + 0x00)	/* WDT_MODE, key 0x22 in [31:16] */
#define TOPRGU_SWRST	(TOPRGU_BASE + 0x14)
#define TOPRGU_NONRST2	(TOPRGU_BASE + 0x24)	/* syscon-reboot-mode, [3:0] */
#define TOPRGU_RST	(TOPRGU_BASE + 0x08)	/* WDT_RESTART, write 0x1971 */

/* reboot-mode values (stock DT reboot-mode node) */
#define RBMODE_NORMAL	0
#define RBMODE_RECOVERY	2
#define RBMODE_FASTBOOT	3

/* UART 8250 register offsets (x4 because mmio32) */
#define UART_THR	0x00
#define UART_LSR	0x14
#define UART_LSR_THRE	0x20

void uart_putc(char c);
void uart_puts(const char *s);
void uart_hex(u64 v);
void log_puts(const char *s);		/* "[titan2-boot] " + s */
void log_hex(const char *s, u64 v);	/* "[titan2-boot] " + s + hex + "\n" */

u64 timer_us(void);			/* microseconds since an arbitrary point */
void udelay(u64 us);

void wdt_kick(void);
void __attribute__((noreturn)) reboot_with_mode(int mode);

void key_init(void);	/* GPIO54/55: GPIO mode, input, input enable, pull-up */
int key_vol_down(void);	/* 1 = pressed */
int key_vol_up(void);
int key_power(void);	/* 1 = pressed, 0 = released, -1 = PMIC not readable */
int pmic_read(unsigned int sid, unsigned int addr);	/* one byte, or -1 */

/* boot.S: clean up and enter a kernel; x0 = fdt, x1..x3 = 0 */
void __attribute__((noreturn)) enter_kernel(u64 entry, u64 fdt);

#endif
