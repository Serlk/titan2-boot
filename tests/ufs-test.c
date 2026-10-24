// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
/*
 * Host test of the M-PHY lane trim arithmetic of src/ufs.c (spec/ufs-mt6878.md
 * 3.2 step 2): the trim index rule and the +0x38 word for the spec's test values;
 * and the SCSI status decision on a response UPIU (spec 4).
 */
#include <stdio.h>
#include <string.h>

#include "io.h"

u32 ufs_mphy_trim_index(u32 x);
u32 ufs_mphy_trim_word(u32 t);
int ufs_cmd_status(const u8 *rsp, int data, int *sense_key);

static int fails;

static void check(const char *what, u32 got, u32 want)
{
	if (got != want) {
		printf("FAIL %s: 0x%08x, expected 0x%08x\n", what, got, want);
		fails++;
	}
}

int main(void)
{
	/* the word: spec test values */
	check("word(0)", ufs_mphy_trim_word(0), 0x06040604);
	check("word(10)", ufs_mphy_trim_word(10), 0x090c080c);
	check("word(-4)", ufs_mphy_trim_word((u32)-4), 0x0501000b);

	/* the index: c - 4 for r = 0, r + 27 for r = 1, 2, r + 21 otherwise */
	check("index r0 c4", ufs_mphy_trim_index(0x004), 0);
	check("index r0 c14", ufs_mphy_trim_index(0x00e), 10);
	check("index r0 c0", ufs_mphy_trim_index(0x000), (u32)-4);
	check("index r0 c31", ufs_mphy_trim_index(0x01f), 27);
	check("index r1", ufs_mphy_trim_index(0x105), 28);
	check("index r2", ufs_mphy_trim_index(0x21f), 29);
	check("index r3", ufs_mphy_trim_index(0x300), 24);
	check("index r15", ufs_mphy_trim_index(0xf1e), 36);
	/* only [11:8] and [4:0] count */
	check("index other bits", ufs_mphy_trim_index(0xffff30e4), ufs_mphy_trim_index(0x004));
	check("index bits 7:5", ufs_mphy_trim_index(0x0e4), 0);
	/* the test phone's efuse case: index from a register, then the word */
	check("word(index(c=4))", ufs_mphy_trim_word(ufs_mphy_trim_index(0x004)), 0x06040604);
	check("word(index(c=0))", ufs_mphy_trim_word(ufs_mphy_trim_index(0x000)), 0x0501000b);

	/*
	 * Response UPIU seen on the phone for the first TEST UNIT READY and START
	 * STOP UNIT after the device reset: byte 6 = 1, status CHECK CONDITION,
	 * sense key 6 UNIT ATTENTION, ASC 0x29. Must be CHECK CONDITION (retry),
	 * not a failure.
	 */
	{
		static const u8 ua[52] = {
			0x21, 0x00, 0x02, 0x04, 0x00, 0x00, 0x01, 0x02,
			0x00, 0x00, 0x00, 0x14, 0, 0, 0, 0,
			0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
			0x00, 0x12, 0xf0, 0x00, 0x06, 0x10, 0x00, 0xd0,
			0x05, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x29, 0x00,
		};
		u8 r[52];
		int sk = -1;

		check("status UA", (u32)ufs_cmd_status(ua, 0, &sk), 1);
		check("status UA sense key", (u32)sk, 6);
		check("status UA no key ptr", (u32)ufs_cmd_status(ua, 0, NULL), 1);

		memcpy(r, ua, sizeof(r));
		r[7] = 0;			/* GOOD, byte 6 still 1 */
		check("status GOOD resp 1", (u32)ufs_cmd_status(r, 0, NULL), 0);
		r[6] = 0;
		r[10] = r[11] = 0;
		check("status GOOD", (u32)ufs_cmd_status(r, 1, NULL), 0);
		r[1] = 0x20;			/* underflow on a data command */
		check("status underflow", (u32)ufs_cmd_status(r, 1, NULL), (u32)-3);
		check("status flags no data", (u32)ufs_cmd_status(r, 0, NULL), 0);
		r[1] = 0;
		r[7] = 0x08;			/* BUSY */
		check("status BUSY", (u32)ufs_cmd_status(r, 0, NULL), (u32)-2);
		r[7] = 2;			/* CHECK CONDITION without sense data */
		sk = -1;
		check("status CC no sense", (u32)ufs_cmd_status(r, 0, &sk), 1);
		check("status CC no sense key", (u32)sk, 0);
		r[0] = 0x36;			/* not a RESPONSE UPIU */
		check("status not response", (u32)ufs_cmd_status(r, 0, NULL), (u32)-1);
	}

	if (fails)
		return 1;
	printf("ufs-test: lane trim, command status OK\n");
	return 0;
}
