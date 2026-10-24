// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
/*
 * Minimal bare-metal I/O for titan2-boot: UART log, timer, keypad read and
 * watchdog/reboot. All hardware is already powered and configured by the
 * stock preloader/bl2_ext/LK (titan2-docs: docs/hardware), so we only touch
 * registers, never re-init clocks or power.
 */
#include "io.h"

/*
 * LK leaves the UART configured. Nobody may be listening (no test points
 * soldered) and the TX FIFO drains anyway, but never spin forever on it.
 * The QEMU test build (make qemu) writes to the PL011 of the virt machine.
 */
/*
 * Copy of the log in the console zone of Android's ramoops (pstore), started
 * by pstore_log_start(): after a watchdog reset the next Android boot shows
 * it in /sys/fs/pstore/console-ramoops-0. Layout from the stock command line
 * (ramoops.mem_address=0x48090000 mem_size=0xe0000 console_size=0x40000
 * pmsg_size=0x80000) and Android's record_size 0x1000, ftrace_size 0x1000:
 * 31 dmesg zones, then the console zone. Zone header as in the kernel's
 * struct persistent_ram_buffer: signature "DBGC", start, size, then the text.
 */
#define PSTORE_CONSOLE		0x480af000UL
#define PSTORE_CONSOLE_SIZE	0x40000
#define PSTORE_SIG		0x43474244

static volatile u32 *plog;
static u32 plog_len;

void pstore_log_start(void)
{
	plog = (volatile u32 *)PSTORE_CONSOLE;
	plog_len = 0;
	plog[1] = 0;
	plog[2] = 0;
	plog[0] = PSTORE_SIG;
}

static void plog_putc(char c)
{
	if (!plog || plog_len >= PSTORE_CONSOLE_SIZE - 12)
		return;
	((volatile u8 *)&plog[3])[plog_len++] = c;
	asm volatile("dsb st" ::: "memory");
	plog[2] = plog_len;
	plog[1] = plog_len;
	asm volatile("dsb st" ::: "memory");
}

void uart_putc(char c)
{
	int n = 100000;

	if (c != '\r')
		plog_putc(c);
	if (c == '\n')
		uart_putc('\r');
#ifdef T2_QEMU
	while (mmio_read32(0x09000000UL + 0x18) & (1u << 5))	/* FR.TXFF */
		if (!--n)
			return;
	mmio_write32(0x09000000UL, (u32)(u8)c);
#else
	while (!(mmio_read32(UART0_BASE + UART_LSR) & UART_LSR_THRE))
		if (!--n)
			return;
	mmio_write32(UART0_BASE + UART_THR, (u32)(u8)c);
#endif
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
	for (i = 60; i > 0 && !(v >> i); i -= 4)
		;
	for (; i >= 0; i -= 4)
		uart_putc(hx[(v >> i) & 0xf]);
}

void log_puts(const char *s)
{
	uart_puts("[titan2-boot] ");
	uart_puts(s);
}

void log_hex(const char *s, u64 v)
{
	log_puts(s);
	uart_hex(v);
	uart_putc('\n');
}

/* Generic timer: CNTFRQ is set by ATF (13 MHz on MediaTek). */
u64 timer_us(void)
{
	u64 cnt, frq;

	asm volatile("isb; mrs %0, cntpct_el0" : "=r"(cnt));
	asm volatile("mrs %0, cntfrq_el0" : "=r"(frq));
	if (!frq)
		frq = 13000000;
	return cnt / (frq / 1000000);
}

void udelay(u64 us)
{
	u64 end = timer_us() + us;

	while (timer_us() < end)
		;
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
	asm volatile("smc #0" : "+r"(x0) : : "x1", "x2", "x3", "memory");
	for (;;)
		asm volatile("wfe");
}

/*
 * Volume keys are plain GPIOs (GPIO54 up, GPIO55 down, active low), as the
 * mainline DT describes them (the keypad matrix 0x1c00e000 sees neither them
 * nor the power key). The level is read from DIN after key_init().
 */
static void rmw(u64 reg, u32 clr, u32 set)
{
	mmio_write32(reg, (mmio_read32(reg) & ~clr) | set);
}

/*
 * What Android does before it reads them (stock DT key-default: input-enable,
 * bias-pull-up; gpio-keys requests the GPIO as input) and mainline likewise.
 * After LK the reading did not change on key presses.
 */
void key_init(void)
{
	static const unsigned int pins[] = { GPIO_VOL_UP, GPIO_VOL_DOWN };
	static const u32 iocfg_bit[] = { 1u << 15, 1u << 16 };
	unsigned int i;

	for (i = 0; i < 2; i++) {
		unsigned int n = pins[i];

		rmw(GPIO_MODE(n), 0xfu << (n % 8 * 4), 0);
		rmw(GPIO_DIR(n), 1u << (n % 32), 0);
		rmw(IOCFG_IES, 0, iocfg_bit[i]);
		rmw(IOCFG_PD, iocfg_bit[i], 0);
		rmw(IOCFG_PU, 0, iocfg_bit[i]);
	}
	udelay(1000);
}

static int gpio_low(unsigned int n)
{
	return !(mmio_read32(GPIO_DIN(n)) & (1u << (n % 32)));
}

int key_vol_down(void)
{
	return gpio_low(GPIO_VOL_DOWN);
}

int key_vol_up(void)
{
	return gpio_low(GPIO_VOL_UP);
}

#define SWINF_STATE(sta)	(((sta) >> 1) & 7)
#define SWINF_IDLE		0
#define SWINF_WFVLDCLR		6
#define PMIF_CMD_EXT_REG_LONG	3
#define PMIF_TIMEOUT_US		10000

static int pmif_wait(unsigned int state)
{
	u64 end = timer_us() + PMIF_TIMEOUT_US;

	while (SWINF_STATE(mmio_read32(PMIF_SWINF2_STA)) != state)
		if (timer_us() > end)
			return -1;
	return 0;
}

/* pmif_spmi_read_cmd() with len 1 */
int pmic_read(unsigned int sid, unsigned int addr)
{
	u32 v;

	if (pmif_wait(SWINF_IDLE)) {
		if (SWINF_STATE(mmio_read32(PMIF_SWINF2_STA)) == SWINF_WFVLDCLR)
			mmio_write32(PMIF_SWINF2_VLD_CLR, 1);
		return -1;
	}
	mmio_write32(PMIF_SWINF2_ACC, PMIF_CMD_EXT_REG_LONG << 30 | (sid & 0xf) << 24 | (addr & 0xffff));
	if (pmif_wait(SWINF_WFVLDCLR))
		return -2;
	v = mmio_read32(PMIF_SWINF2_RDATA);
	mmio_write32(PMIF_SWINF2_VLD_CLR, 1);
	return v & 0xff;
}

int key_power(void)
{
	int v = pmic_read(MT6363_SID, MT6363_TOPSTATUS);

	if (v < 0)
		return -1;
	return !(v & MT6363_PWRKEY_DEB);
}
