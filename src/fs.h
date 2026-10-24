/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
#ifndef TITAN2_BOOT_FS_H
#define TITAN2_BOOT_FS_H

#include "io.h"

/* A block device: the UFS user LU, or a partition of it. */
struct blk {
	u64 nblocks;
	u32 bsize;		/* bytes per block, a power of two */
	int (*read)(struct blk *b, u64 lba, u64 count, void *buf);	/* 0 or < 0 */
	void *priv;
	struct blk *parent;	/* partitions: the disk */
	u64 start;		/* partitions: first block on the disk */
};

int blk_read(struct blk *b, u64 lba, u64 count, void *buf);
/* bytes [off, off + len) of the device, through a bounce buffer for the unaligned ends */
int blk_read_bytes(struct blk *b, u64 off, u64 len, void *buf);

/* GPT: the partition with this name (ASCII, as in /dev/block/by-name), 0 or < 0 */
int gpt_find(struct blk *disk, const char *name, struct blk *part);

/* A read-only file source: the pack in boot_a, or an ext4 partition. */
struct fs {
	const char *name;
	/* file size, or < 0 if there is no such file */
	long (*stat)(struct fs *fs, const char *path);
	/* read the whole file to dst (at most max bytes); size or < 0 */
	long (*load)(struct fs *fs, const char *path, void *dst, u64 max);
	/* the file in memory without a copy (the pack only), or NULL */
	const void *(*map)(struct fs *fs, const char *path, u64 *size);
	void *priv;
};

int packfs_init(struct fs *fs, const void *pack);	/* 0 or < 0 */
int ext4_mount(struct fs *fs, struct blk *part);	/* 0 or < 0 */

#endif
