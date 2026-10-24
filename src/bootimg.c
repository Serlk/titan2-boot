// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
/*
 * The kernel field of an Android boot.img on a partition (mkbootimg format,
 * header v0..v4: magic, kernel_size at 8, header_version at 40; v0-v2 page
 * size at 36, v3/v4 a fixed 4096; the kernel starts at the first page).
 * The ramdisks are not needed: LK has already loaded vendor_boot and
 * init_boot of slot A for Android.
 */
#include <string.h>

#include "boot.h"
#include "fs.h"

long bootimg_kernel_size(struct blk *part, u64 *offset)
{
	u8 h[64];
	u32 ksize, version, page;

	if (blk_read_bytes(part, 0, sizeof(h), h) || memcmp(h, "ANDROID!", 8))
		return -1;
	ksize = h[8] | (u32)h[9] << 8 | (u32)h[10] << 16 | (u32)h[11] << 24;
	version = h[40] | (u32)h[41] << 8 | (u32)h[42] << 16 | (u32)h[43] << 24;
	page = version >= 3 ? 4096 : (h[36] | (u32)h[37] << 8 | (u32)h[38] << 16 | (u32)h[39] << 24);
	if (version > 4 || page < 2048 || page > 65536 || (page & (page - 1)) || !ksize)
		return -2;
	if (page + (u64)ksize > part->nblocks * part->bsize)
		return -3;
	*offset = page;
	return ksize;
}

long bootimg_kernel(struct blk *part, void *dst, u64 max)
{
	u64 off;
	long n = bootimg_kernel_size(part, &off);

	if (n < 0)
		return n;
	if ((u64)n > max)
		return -4;
	if (blk_read_bytes(part, off, n, dst))
		return -5;
	return n;
}
