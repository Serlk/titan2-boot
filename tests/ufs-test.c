// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
/*
 * Host test of the M-PHY lane trim arithmetic of src/ufs.c (spec/ufs-mt6878.md
 * 3.2 step 2): the trim index rule and the +0x38 word for the spec's test values.
 */
#include <stdio.h>

#include "io.h"

u32 ufs_mphy_trim_index(u32 x);
u32 ufs_mphy_trim_word(u32 t);

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

	if (fails)
		return 1;
	printf("ufs-test: lane trim OK\n");
	return 0;
}
