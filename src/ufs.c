// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
/*
 * UFS user LU, read-only.
 *
 * Hardware (MT6878): a standard UFSHCI 3.0 host controller with a MediaTek
 * M-PHY. Everything MT6878-specific (addresses, the M-PHY register writes and
 * the lane trim formula, the DME writes, the device reset SMC, timings, the
 * state handed over by the previous stage and expected by the kernel) comes
 * from spec/ufs-mt6878.md; the rest is JEDEC UFS (JESD220/JESD223: UPIU,
 * UTP transfer request descriptor, register map), MIPI UniPro (PA_* and DME
 * attributes) and SCSI (SBC/SPC), with the public Linux drivers
 * drivers/ufs/core/ufshcd.c and drivers/ufs/host/ufs-mediatek.c as a
 * reference for the standard parts.
 *
 * Sequence (section numbers of the spec):
 *   3.1 host enable: HCE off, device reset pulse via the SiP SMC, HCE on
 *   3.2 M-PHY setup: efuse-dependent defaults, per-lane trim, fixed values
 *   3.3 local DME writes before the link comes up
 *   3.4 DME_LINKSTARTUP, device present
 *       (3.1-3.4 up to 4 times)
 *   3.5 transfer request list (one slot), list run-stop on
 *   3.6 NOP OUT / NOP IN, fDeviceInit set and polled until clear
 *   3.7 power mode change to HS-G4 rate B, one lane
 *   3.8 LU 2: TEST UNIT READY until GOOD, READ CAPACITY(10)
 * Then READ(10) on LU 2 through slot 0, polled. ufs_close() puts the device
 * back to sleep (START STOP UNIT PowerDown to the UFS Device W-LUN), drives
 * the device reset and switches the host off, as the kernel expects.
 *
 * Every UPIU goes through ufs_transfer(), and ufs_allowed() there refuses
 * anything outside the read-only whitelist of spec section 5 (no writes, no
 * attribute/descriptor writes, no other flags, nothing to RPMB).
 *
 * The MMU and data cache are on: the descriptor, the command descriptor and
 * the data buffers are cleaned+invalidated to PoC before the doorbell and
 * invalidated after completion. Destinations that are not 64-byte aligned go
 * through a bounce buffer. No interrupts; every wait is bounded and kicks the
 * watchdog.
 *
 * The QEMU test build reads a GPT disk image from RAM instead.
 */
#include <string.h>

#include "memmap.h"
#include "ufs.h"

#ifndef T2_QEMU

/*
 * Lane trim (spec 3.2 step 2), pure arithmetic so the host test can check it.
 * x is the lane's +0x3c register: r = [11:8], c = [4:0].
 */
u32 ufs_mphy_trim_index(u32 x);
u32 ufs_mphy_trim_word(u32 t);

u32 ufs_mphy_trim_index(u32 x)
{
	u32 r = (x >> 8) & 0xf, c = x & 0x1f;

	if (r == 0)
		return c - 4;		/* may wrap: t is used as unsigned 32 bit */
	if (r == 1 || r == 2)
		return r + 27;
	return r + 21;
}

/* the value for the lane's +0x38 register; all arithmetic unsigned 32 bit */
u32 ufs_mphy_trim_word(u32 t)
{
	u32 a = ((t + 26) / 4) % 16;
	u32 b = ((3 * t + 18) / 4) % 32;
	u32 c = ((t + 3) / 6 + 6) % 16;
	u32 d = ((5 * t + 3) / 6 + 4) % 32;

	return a << 24 | b << 16 | c << 8 | d;
}

#ifndef UFS_HOST_TEST

#include "mmu.h"

/* spec 1 */
#define HCI_BASE	0x112b0000UL
#define MPHY_BASE	0x112a0000UL
#define EFUSE_TRIM	0x11f10260UL
#define SIP_UFS_CONTROL	0xc2000276UL
#define SIP_DEVICE_RESET 2

/* UFSHCI registers */
#define REG_AHIT	0x18
#define REG_IS		0x20
#define REG_IE		0x24
#define REG_HCS		0x30
#define REG_HCE		0x34
#define REG_UTRLBA	0x50
#define REG_UTRLBAU	0x54
#define REG_UTRLDBR	0x58
#define REG_UTRLCLR	0x5c
#define REG_UTRLRSR	0x60
#define REG_UICCMD	0x90
#define REG_UCMDARG1	0x94
#define REG_UCMDARG2	0x98
#define REG_UCMDARG3	0x9c

#define IS_UPMS		(1u << 4)	/* power mode status */
#define IS_UCCS		(1u << 10)	/* UIC command completion */
#define IS_ERRORS	((1u << 2) | (1u << 11) | (1u << 16) | (1u << 17))
#define HCS_DP		(1u << 0)
#define HCS_UCRDY	(1u << 3)

#define UIC_GET		0x01
#define UIC_SET		0x02
#define UIC_PEER_GET	0x03
#define UIC_PEER_SET	0x04
#define UIC_LINKSTARTUP	0x16

/* UPIU transaction codes */
#define UPIU_NOP_OUT	0x00
#define UPIU_COMMAND	0x01
#define UPIU_QUERY	0x16
#define UPIU_NOP_IN	0x20
#define UPIU_RESPONSE	0x21
#define UPIU_QUERY_RSP	0x36

#define QUERY_STD_READ	0x01
#define QUERY_STD_WRITE	0x81
#define QUERY_READ_FLAG	0x05
#define QUERY_SET_FLAG	0x06
#define IDN_FDEVICEINIT	0x01

#define SCSI_TUR	0x00
#define SCSI_REQSENSE	0x03
#define SCSI_INQUIRY	0x12
#define SCSI_SSU	0x1b
#define SCSI_RC10	0x25
#define SCSI_READ10	0x28

#define LU_USER		2
#define WLUN_DEVICE	0xd0
#define BLOCK_SIZE	4096

/* timeouts, microseconds (spec 2-4) */
#define T_HCE_OFF	10000
#define T_HCE_ON	100000
#define T_UIC		500000
#define T_UCRDY		100000
#define T_LISTRDY	100000
#define T_CMD		5000000
#define T_NOP		100000
#define T_QUERY		1700000
#define T_DEVINIT	5000000
#define T_CLEAR		100000

#define DD_FROM_DEVICE	0x04000000u
#define DD_MASK		0x06000000u
#define PRDT_ENTRY_MAX	(256 * 1024)
#define PRDT_ENTRIES	128
#define READ10_MAX	0x2000		/* blocks per READ(10), spec 4 */
#define BOUNCE_SIZE	(128 * 1024)

/* command descriptor layout (spec 4) */
#define UCD_RSP		0x200
#define UCD_PRDT	0x400
#define UCD_SIZE	0xc00

/* DMA objects: each a whole number of 64-byte cache lines */
static u32 utrl[16] __attribute__((aligned(1024)));	/* slot 0 = dwords 0-7 */
static u8 ucd[UCD_SIZE] __attribute__((aligned(128)));
static u8 bounce[BOUNCE_SIZE] __attribute__((aligned(64)));

static int hw_touched;	/* ufs_open() has touched the hardware */
static int can_send;	/* list running and the device answered NOP OUT */
static int slot_dead;	/* slot 0 could not be cleared: send nothing more */
static u8 task_tag;

static inline u32 hci_read(u32 off)
{
	return mmio_read32(HCI_BASE + off);
}

static inline void hci_write(u32 off, u32 v)
{
	mmio_write32(HCI_BASE + off, v);
}

/* wait until (reg & mask) == val, at most us microseconds: 0 or -1 */
static int hci_poll(u32 off, u32 mask, u32 val, u64 us)
{
	u64 end = timer_us() + us;

	for (;;) {
		int expired = timer_us() > end;

		if ((hci_read(off) & mask) == val)
			return 0;
		if (expired)
			return -1;
		wdt_kick();
	}
}

/* ---- device reset (MediaTek SiP SMC) ---- */

static u64 smc_device_reset(u64 release)
{
	register u64 x0 asm("x0") = SIP_UFS_CONTROL;
	register u64 x1 asm("x1") = SIP_DEVICE_RESET;
	register u64 x2 asm("x2") = release;
	register u64 x3 asm("x3") = 0;

	asm volatile("smc #0"
		     : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
		     :
		     : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12",
		       "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

/* ---- 3.1 host enable ---- */

static int host_disable(void)
{
	if (!(hci_read(REG_HCE) & 1))
		return 0;
	hci_write(REG_HCE, 0);
	return hci_poll(REG_HCE, 1, 0, T_HCE_OFF);
}

static int host_enable(void)
{
	log_puts("UFS: host disable\n");
	if (host_disable()) {
		log_puts("UFS: HCE does not clear\n");
		return -1;
	}
	log_puts("UFS: device reset\n");
	if (smc_device_reset(0)) {
		log_puts("UFS: device reset SMC (assert) failed\n");
		return -1;
	}
	udelay(10);
	if (smc_device_reset(1)) {
		log_puts("UFS: device reset SMC (release) failed\n");
		return -1;
	}
	udelay(10000);
	log_puts("UFS: host enable\n");
	hci_write(REG_HCE, 1);
	udelay(1000);
	if (hci_poll(REG_HCE, 1, 1, T_HCE_ON)) {
		log_puts("UFS: HCE does not come up\n");
		return -1;
	}
	hci_write(REG_IE, hci_read(REG_IE) | 0x470);
	return 0;
}

/* ---- 3.2 M-PHY ---- */

#define BITS(h, l)	((u32)(((1ull << ((h) - (l) + 1)) - 1) << (l)))

static void mphy_write(u32 off, u32 v)
{
	mmio_write32(MPHY_BASE + off, v);
}

/* one read-modify-write: the bits in mask become val (already shifted) */
static void mphy_rmw(u32 off, u32 mask, u32 val)
{
	u32 v = mmio_read32(MPHY_BASE + off);

	mphy_write(off, (v & ~mask) | (val & mask));
}

static void mphy_setup(void)
{
	static const u32 lane[2] = { 0x8000, 0x8100 };
	static const u32 blk[2] = { 0xa000, 0xa100 };
	static const u32 fixed[6] = {
		0x1a191104, 0x1a191105, 0x09080602, 0x09080602, 0x0a0a0a04, 0x0a0a0a05,
	};
	u32 e = mmio_read32(EFUSE_TRIM);
	int i, j;

	/* 1: defaults for untrimmed lanes */
	if (!(e & 1)) {
		mphy_rmw(0x803c, BITS(11, 8) | BITS(4, 0), 0x1e);
		mphy_rmw(0x1004, BITS(29, 24), 0x13u << 24);
	}
	if (!(e & 2))
		mphy_rmw(0x813c, BITS(11, 8) | BITS(4, 0), 0x1e);

	/* 2: per-lane trim */
	for (i = 0; i < 2; i++) {
		u32 t = ufs_mphy_trim_index(mmio_read32(MPHY_BASE + lane[i] + 0x3c));

		mphy_write(lane[i] + 0x38, ufs_mphy_trim_word(t));
	}

	/* 3 */
	mphy_rmw(0x2088, BITS(31, 24), 0x4au << 24);
	mphy_rmw(0x2094, BITS(15, 0), 0x4a4a);
	mphy_rmw(0x2098, BITS(31, 24), 0x4au << 24);

	/* 4: one block completely before the next */
	for (i = 0; i < 2; i++) {
		u32 b = blk[i];

		mphy_rmw(b + 0x34, BITS(10, 0), 0x1c1);
		mphy_rmw(b + 0x38, BITS(26, 16), 0x1c2u << 16);
		mphy_rmw(b + 0x3c, BITS(10, 0), 0x1c7);
		mphy_rmw(b + 0x3c, BITS(26, 16), 0x1f9u << 16);
		mphy_rmw(b + 0x44, BITS(26, 16), 0x1efu << 16);
		mphy_rmw(b + 0xcc, BITS(10, 0), 0x1ff);
	}

	/* 5 */
	for (i = 0; i < 2; i++)
		for (j = 0; j < 6; j++)
			mphy_write(blk[i] + 0x54 + 4 * j, fixed[j]);

	/* 6 */
	for (i = 0; i < 2; i++)
		mphy_rmw(blk[i] + 0xdc, BITS(29, 24) | BITS(13, 8), 0x30u << 24 | 0x30u << 8);

	/* 7 */
	mphy_rmw(0x209c, BITS(31, 16), 0x8e8eu << 16);
	mphy_rmw(0x2090, BITS(23, 16), 0x01u << 16);
	mphy_rmw(0x2098, BITS(2, 0), 0);

	/* 8: two separate writes */
	mphy_rmw(0x208c, BITS(11, 8), 0x9u << 8);
	mphy_rmw(0x208c, BITS(27, 24), 0x1u << 24);

	/* 9 */
	mphy_rmw(0x1010, 1u << 7, 0);
}

/* ---- UIC / DME (3.3, 3.4, 3.7) ---- */

static int uic_cmd(u32 cmd, u32 attr, u32 val, u32 *out)
{
	u32 res;

	if (hci_poll(REG_HCS, HCS_UCRDY, HCS_UCRDY, T_UCRDY)) {
		log_hex("UFS: UIC not ready for command ", cmd);
		return -1;
	}
	hci_write(REG_IS, IS_UCCS);
	hci_write(REG_UCMDARG1, attr << 16);
	hci_write(REG_UCMDARG2, 0);
	hci_write(REG_UCMDARG3, val);
	hci_write(REG_UICCMD, cmd);
	if (hci_poll(REG_IS, IS_UCCS, IS_UCCS, T_UIC)) {
		log_hex("UFS: UIC command timeout, attribute ", attr);
		return -1;
	}
	hci_write(REG_IS, IS_UCCS);
	res = hci_read(REG_UCMDARG2) & 0xff;
	if (res) {
		log_hex("UFS: UIC command failed, attribute ", attr);
		log_hex("UFS:   result ", res);
		return -1;
	}
	if (out)
		*out = hci_read(REG_UCMDARG3);
	return 0;
}

static int dme_set(u32 attr, u32 v)
{
	return uic_cmd(UIC_SET, attr, v, NULL);
}

static int dme_get(u32 attr, u32 *v)
{
	return uic_cmd(UIC_GET, attr, 0, v);
}

/* read-modify-write of a local attribute */
static int dme_rmw(u32 attr, u32 clear, u32 set)
{
	u32 v;

	if (dme_get(attr, &v))
		return -1;
	return dme_set(attr, (v & ~clear) | set);
}

static int dme_pre_link(void)
{
	if (dme_rmw(0xd0a6, 1u << 6, 0) ||			/* VS_SAVEPOWERCONTROL */
	    dme_set(0x1585, 0) ||				/* PA_SCRAMBLING */
	    dme_rmw(0xd0a6, (1u << 0) | (1u << 2) | (1u << 3), 0) ||
	    dme_rmw(0xd0a1, 0, 1u << 5)) {			/* VS_DEBUGCLOCKENABLE */
		log_puts("UFS: DME setup before link startup failed\n");
		return -1;
	}
	return 0;
}

static int link_startup(void)
{
	if (uic_cmd(UIC_LINKSTARTUP, 0, 0, NULL)) {
		log_puts("UFS: link startup failed\n");
		return -1;
	}
	if (!(hci_read(REG_HCS) & HCS_DP)) {
		log_puts("UFS: no device present after link startup\n");
		return -1;
	}
	return 0;
}

static int set_userdata(void)
{
	static const u32 v[6] = { 0x1fff, 0xffff, 0x7fff, 0x1fff, 0xffff, 0x7fff };
	int i;

	for (i = 0; i < 6; i++)
		if (dme_set(0x15b0 + i, v[i]))
			return -1;
	return 0;
}

static int power_mode_hs(void)
{
	const u32 gear = 4;
	u32 v;

	if (set_userdata() ||
	    dme_set(0xd041, 0x1fff) || dme_set(0xd042, 0xffff) || dme_set(0xd043, 0x7fff) ||
	    dme_set(0x15d4, gear == 4 ? 1 : 3) ||		/* PA_TXHSADAPTTYPE */
	    set_userdata())
		return -1;
	/* 5: exchange 0x1543 -> 0x1564 in both directions */
	if (uic_cmd(UIC_PEER_GET, 0x1543, 0, &v) || dme_set(0x1564, v) ||
	    dme_get(0x1543, &v) || uic_cmd(UIC_PEER_SET, 0x1564, v, NULL))
		return -1;
	if (dme_set(0x1569, 1) || dme_set(0x1584, 1) ||	/* terminations */
	    dme_set(0x1568, gear) || dme_set(0x1583, gear) ||	/* gears */
	    dme_set(0x1560, 1) || dme_set(0x1580, 1) ||		/* lanes */
	    dme_set(0x156a, 2))					/* rate B */
		return -1;
	hci_write(REG_IS, IS_UPMS);
	if (dme_set(0x1571, 0x11))				/* PA_PWRMODE fast/fast */
		return -1;
	if (hci_poll(REG_IS, IS_UPMS, IS_UPMS, T_UIC)) {
		log_puts("UFS: power mode change timeout\n");
		return -1;
	}
	hci_write(REG_IS, IS_UPMS);
	v = (hci_read(REG_HCS) >> 8) & 7;
	if (v != 1) {
		log_hex("UFS: power mode change status ", v);
		return -1;
	}
	return 0;
}

/* ---- transfer requests (spec 4, 5) ---- */

static void put_be32(u8 *p, u32 v)
{
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
}

static u32 get_be32(const u8 *p)
{
	return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}

/*
 * The whitelist of spec section 5, applied to exactly what the controller is
 * about to fetch: the request UPIU in the command descriptor and the
 * descriptor's data direction. 1 = may be sent.
 */
static int ufs_allowed(const u8 *upiu, u32 dw0, u32 len)
{
	static const u8 ssu_powerdown[6] = { SCSI_SSU, 0, 0, 0, 0x30, 0 };
	const u8 *cdb = upiu + 16;
	u32 dd = dw0 & DD_MASK;
	int i;

	/* never data to the device, no EHS, no data segment */
	if (dd != 0 && dd != DD_FROM_DEVICE)
		return 0;
	if ((dd != 0) != (len != 0))
		return 0;
	if (upiu[8] || upiu[10] || upiu[11])
		return 0;

	switch (upiu[0]) {
	case UPIU_NOP_OUT:
		return !len;
	case UPIU_QUERY:
		if (len || upiu[13] != IDN_FDEVICEINIT || upiu[14] || upiu[15])
			return 0;
		return (upiu[5] == QUERY_STD_READ && upiu[12] == QUERY_READ_FLAG) ||
		       (upiu[5] == QUERY_STD_WRITE && upiu[12] == QUERY_SET_FLAG);
	case UPIU_COMMAND:
		break;
	default:
		return 0;
	}

	/* COMMAND: read flag only together with data from the device */
	if (upiu[1] != (len ? 0x40 : 0x00) || get_be32(upiu + 12) != len)
		return 0;
	switch (cdb[0]) {
	case SCSI_TUR:
		return upiu[2] <= 2 && !len;
	case SCSI_REQSENSE:
	case SCSI_INQUIRY:
	case SCSI_RC10:
	case SCSI_READ10:
		return upiu[2] <= 2 && len;
	case SCSI_SSU:
		if (upiu[2] != WLUN_DEVICE || len)
			return 0;
		for (i = 0; i < 16; i++)
			if (cdb[i] != (i < 6 ? ssu_powerdown[i] : 0))
				return 0;
		return 1;
	default:
		return 0;
	}
}

/*
 * Send one request UPIU (32 bytes: header + transaction fields) through slot
 * 0 and wait for completion. buf/len: data from the device, or len 0. buf
 * is 64-byte aligned and len a multiple of 4; the caller owns the cache lines
 * up to the next 64-byte boundary after buf + len. The response UPIU is at
 * ucd + UCD_RSP afterwards. 0 or < 0.
 */
static int ufs_transfer(const u8 *req, void *buf, u32 len, u64 timeout)
{
	u64 ucd_pa = (u64)ucd, buf_pa = (u64)buf;
	u32 n = 0, off, is;
	u8 *prdt = ucd + UCD_PRDT;

	if (slot_dead)
		return -1;
	if (len && ((buf_pa & 63) || (len & 3) || len > PRDT_ENTRIES * PRDT_ENTRY_MAX))
		return -1;

	memset(ucd, 0, sizeof(ucd));
	memcpy(ucd, req, 32);
	ucd[3] = ++task_tag;
	for (off = 0; off < len; off += PRDT_ENTRY_MAX, n++) {
		u32 chunk = len - off < PRDT_ENTRY_MAX ? len - off : PRDT_ENTRY_MAX;
		u64 a = buf_pa + off;
		u8 *e = prdt + 16 * n;

		/* little-endian dwords: address low, address high, 0, byte count - 1 */
		for (int i = 0; i < 4; i++) {
			e[i] = (u8)(a >> (8 * i));
			e[4 + i] = (u8)(a >> (32 + 8 * i));
			e[12 + i] = (u8)((chunk - 1) >> (8 * i));
		}
	}

	memset(utrl, 0, sizeof(utrl));
	utrl[0] = 0x10000000u | (len ? DD_FROM_DEVICE : 0);
	utrl[2] = 0x0000000f;
	utrl[4] = (u32)ucd_pa;
	utrl[5] = (u32)(ucd_pa >> 32);
	utrl[6] = 0x00800080;
	utrl[7] = 0x01000000u | n;

	if (!ufs_allowed(ucd, utrl[0], len)) {
		log_hex("UFS: refused request, UPIU type ", ucd[0]);
		log_hex("UFS:   opcode/CDB[0] ", ucd[0] == UPIU_COMMAND ? ucd[16] : ucd[12]);
		return -1;
	}

	dcache_range(DCACHE_CLEAN_INVAL, (u64)utrl, (u64)utrl + sizeof(utrl));
	dcache_range(DCACHE_CLEAN_INVAL, ucd_pa, ucd_pa + sizeof(ucd));
	if (len)
		dcache_range(DCACHE_CLEAN_INVAL, buf_pa, buf_pa + len);

	hci_write(REG_IS, hci_read(REG_IS));
	hci_write(REG_UTRLDBR, 1);
	if (hci_poll(REG_UTRLDBR, 1, 0, timeout)) {
		log_hex("UFS: request timeout, UPIU type ", ucd[0]);
		hci_write(REG_UTRLCLR, ~1u);
		if (hci_poll(REG_UTRLDBR, 1, 0, T_CLEAR)) {
			log_puts("UFS: slot 0 does not clear, UFS disabled\n");
			slot_dead = 1;
		}
		return -1;
	}
	asm volatile("dsb sy" ::: "memory");
	dcache_range(DCACHE_INVAL, (u64)utrl, (u64)utrl + sizeof(utrl));
	dcache_range(DCACHE_INVAL, ucd_pa, ucd_pa + sizeof(ucd));
	if (len)
		dcache_range(DCACHE_INVAL, buf_pa, buf_pa + len);

	is = hci_read(REG_IS);
	hci_write(REG_IS, is);
	if (is & IS_ERRORS) {
		log_hex("UFS: error interrupt status ", is);
		return -1;
	}
	if (utrl[2] & 0xff) {
		log_hex("UFS: overall command status ", utrl[2] & 0xff);
		return -1;
	}
	if (ucd[UCD_RSP + 3] != ucd[3]) {
		log_puts("UFS: response with a wrong task tag\n");
		return -1;
	}
	return 0;
}

static int nop_out(void)
{
	u8 req[32] = { UPIU_NOP_OUT };

	if (ufs_transfer(req, NULL, 0, T_NOP))
		return -1;
	return ucd[UCD_RSP] == UPIU_NOP_IN && ucd[UCD_RSP + 6] == 0 ? 0 : -1;
}

/* QUERY on fDeviceInit: READ FLAG (*flag set) or SET FLAG */
static int query_flag(int set, u32 *flag)
{
	u8 req[32] = { UPIU_QUERY };
	const u8 *rsp = ucd + UCD_RSP;

	req[5] = set ? QUERY_STD_WRITE : QUERY_STD_READ;
	req[12] = set ? QUERY_SET_FLAG : QUERY_READ_FLAG;
	req[13] = IDN_FDEVICEINIT;
	if (ufs_transfer(req, NULL, 0, T_QUERY))
		return -1;
	if (rsp[0] != UPIU_QUERY_RSP || rsp[6]) {
		log_hex("UFS: query response ", rsp[6]);
		return -1;
	}
	if (flag)
		*flag = rsp[23] & 1;
	return 0;
}

static int device_init(void)
{
	u64 end;
	u32 flag = 1;
	int i;

	for (i = 0; i < 10; i++)
		if (!nop_out())
			break;
	if (i == 10) {
		log_puts("UFS: no NOP IN from the device\n");
		return -1;
	}
	can_send = 1;
	if (query_flag(1, NULL)) {
		log_puts("UFS: setting fDeviceInit failed\n");
		return -1;
	}
	end = timer_us() + T_DEVINIT;
	for (;;) {
		if (query_flag(0, &flag)) {
			log_puts("UFS: reading fDeviceInit failed\n");
			return -1;
		}
		if (!flag)
			return 0;
		if (timer_us() > end)
			break;
		wdt_kick();
		udelay(1000);
	}
	log_puts("UFS: fDeviceInit does not clear\n");
	return -1;
}

/*
 * SCSI command to a LU: 0 = GOOD, 1 = CHECK CONDITION (*sense_key set),
 * < 0 = transport failure or another status.
 */
static int scsi_cmd(u8 lun, const u8 *cdb, u32 cdb_len, void *buf, u32 len,
		    int *sense_key)
{
	u8 req[32] = { UPIU_COMMAND };
	const u8 *rsp = ucd + UCD_RSP;

	req[1] = len ? 0x40 : 0;
	req[2] = lun;
	put_be32(req + 12, len);
	memcpy(req + 16, cdb, cdb_len);
	if (ufs_transfer(req, buf, len, T_CMD))
		return -1;
	if (rsp[0] != UPIU_RESPONSE || rsp[6]) {
		log_hex("UFS: command response ", rsp[6]);
		return -1;
	}
	if (rsp[7] == 2) {
		if (sense_key)
			*sense_key = rsp[0x24] & 0xf;
		return 1;
	}
	if (rsp[7]) {
		log_hex("UFS: SCSI status ", rsp[7]);
		return -1;
	}
	/* data commands: no underflow / overflow */
	if (len && (rsp[1] & 0x60)) {
		log_hex("UFS: residual transfer count ", get_be32(rsp + 12));
		return -1;
	}
	return 0;
}

static int read10(u32 lba, u32 count, void *buf)
{
	u8 cdb[10] = { SCSI_READ10, 0 };
	int tries, r = -1, sk = 0;

	put_be32(cdb + 2, lba);
	cdb[7] = count >> 8;
	cdb[8] = count;
	for (tries = 0; tries < 3; tries++) {
		r = scsi_cmd(LU_USER, cdb, sizeof(cdb), buf, count * BLOCK_SIZE, &sk);
		if (r <= 0)
			break;
		log_hex("UFS: READ(10) check condition, sense key ", sk);
	}
	if (r)
		log_hex("UFS: READ(10) failed at LBA ", lba);
	return r ? -1 : 0;
}

static int lu_read(struct blk *b, u64 lba, u64 count, void *buf)
{
	u8 *out = buf;

	(void)b;
	while (count) {
		int direct = !((u64)out & 63);
		u64 max = direct ? READ10_MAX : BOUNCE_SIZE / BLOCK_SIZE;
		u64 n = count < max ? count : max;

		if (lba + n > 0x100000000ull)
			return -1;
		if (read10((u32)lba, (u32)n, direct ? (void *)out : (void *)bounce))
			return -1;
		if (!direct)
			memcpy(out, bounce, n * BLOCK_SIZE);
		out += n * BLOCK_SIZE;
		lba += n;
		count -= n;
		wdt_kick();
	}
	return 0;
}

static int user_lu(struct blk *disk)
{
	static const u8 tur[6] = { SCSI_TUR };
	static const u8 rc10[10] = { SCSI_RC10 };
	u32 last, bs;
	int i, r = -1, sk = 0;

	for (i = 0; i < 10; i++) {
		r = scsi_cmd(LU_USER, tur, sizeof(tur), NULL, 0, &sk);
		if (r <= 0)
			break;
		log_hex("UFS: TEST UNIT READY: sense key ", sk);
		udelay(1000);
	}
	if (r) {
		log_puts("UFS: LU 2 not ready\n");
		return -1;
	}
	memset(bounce, 0, 8);
	if (scsi_cmd(LU_USER, rc10, sizeof(rc10), bounce, 8, NULL)) {
		log_puts("UFS: READ CAPACITY(10) failed\n");
		return -1;
	}
	last = get_be32(bounce);
	bs = get_be32(bounce + 4);
	log_hex("UFS: LU 2 last LBA ", last);
	if (bs != BLOCK_SIZE) {
		log_hex("UFS: unsupported block size ", bs);
		return -1;
	}
	if (last == 0xffffffff) {
		log_puts("UFS: LU 2 needs READ CAPACITY(16)\n");
		return -1;
	}
	memset(disk, 0, sizeof(*disk));
	disk->nblocks = (u64)last + 1;
	disk->bsize = BLOCK_SIZE;
	disk->read = lu_read;
	return 0;
}

int ufs_open(struct blk *disk)
{
	u64 list = (u64)utrl;
	int i;

	log_puts("UFS: starting\n");
	hw_touched = 1;
	can_send = 0;
	slot_dead = 0;

	for (i = 0; i < 4; i++) {
		if (!host_enable()) {
			log_puts("UFS: M-PHY setup\n");
			mphy_setup();
			log_puts("UFS: DME before link startup\n");
			if (!dme_pre_link()) {
				log_puts("UFS: link startup\n");
				if (!link_startup())
					break;
			}
		}
		log_hex("UFS: bring-up attempt failed: ", i + 1);
	}
	if (i == 4)
		goto fail;
	log_puts("UFS: link up\n");

	memset(utrl, 0, sizeof(utrl));
	dcache_range(DCACHE_CLEAN_INVAL, list, list + sizeof(utrl));
	hci_write(REG_UTRLBA, (u32)list);
	hci_write(REG_UTRLBAU, (u32)(list >> 32));
	hci_write(REG_IE, hci_read(REG_IE) | 0x30a75);
	if (hci_poll(REG_HCS, 0xfe, 0x0e, T_LISTRDY)) {
		log_hex("UFS: host not ready for requests, HCS ", hci_read(REG_HCS));
		goto fail;
	}
	hci_write(REG_UTRLRSR, 1);

	if (device_init())
		goto fail;
	log_puts("UFS: device initialised\n");
	if (power_mode_hs()) {
		log_puts("UFS: HS-G4 power mode change failed\n");
		goto fail;
	}
	log_puts("UFS: HS-G4 rate B, 1 lane\n");
	if (user_lu(disk))
		goto fail;
	log_hex("UFS: LU 2 ready, blocks ", disk->nblocks);
	return 0;

fail:
	log_puts("UFS: start failed, shutting down\n");
	ufs_close();
	return -1;
}

void ufs_close(void)
{
	static const u8 ssu[6] = { SCSI_SSU, 0, 0, 0, 0x30, 0 };
	int i;

	if (!hw_touched)
		return;
	hci_write(REG_AHIT, 0);
	if (can_send && !slot_dead) {
		for (i = 0; i < 3; i++)
			if (!scsi_cmd(WLUN_DEVICE, ssu, sizeof(ssu), NULL, 0, NULL))
				break;
		if (i == 3)
			log_puts("UFS: START STOP UNIT (power down) failed\n");
	}
	if (smc_device_reset(0))
		log_puts("UFS: device reset SMC failed\n");
	hci_write(REG_HCE, 0);
	if (hci_poll(REG_HCE, 1, 0, T_HCE_OFF))
		log_puts("UFS: HCE does not clear\n");
	hw_touched = 0;
	can_send = 0;
	log_puts("UFS: closed\n");
}

#endif /* !UFS_HOST_TEST */

#else /* T2_QEMU */


static int ram_read(struct blk *b, u64 lba, u64 count, void *buf)
{
	memcpy(buf, (const u8 *)T2_QEMU_DISK + lba * b->bsize, count * b->bsize);
	return 0;
}

void ufs_qemu_reserve(void)
{
	memmap_mark(T2_QEMU_DISK, T2_QEMU_DISK + T2_QEMU_DISK_SIZE, MEM_BUSY, "qemu-disk");
}

int ufs_open(struct blk *disk)
{
	if (memcmp((const u8 *)T2_QEMU_DISK + 4096, "EFI PART", 8))
		return -1;
	memset(disk, 0, sizeof(*disk));
	disk->bsize = 4096;
	disk->nblocks = T2_QEMU_DISK_SIZE / 4096;
	disk->read = ram_read;
	log_puts("QEMU RAM disk as the UFS user LU\n");
	return 0;
}

void ufs_close(void)
{
}

#endif
