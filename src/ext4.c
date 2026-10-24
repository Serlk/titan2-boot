// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
/*
 * Read-only ext4 (also ext2/ext3 files that use extents): superblock, 32/64-bit
 * group descriptors, inodes, extent trees of any depth, linear directory scan
 * (htree directories read fine that way: the index blocks look like one big
 * empty entry), fast and slow symlinks. No journal replay: the loader only
 * reads /boot, which is written rarely; checksums are not verified.
 * Block-mapped (ext2/ext3 indirect) files are not supported.
 * Layout: Documentation/filesystems/ext4/ in the kernel tree.
 */
#include <string.h>

#include "fs.h"

#define EXT4_MAGIC		0xef53
#define EXT4_ROOT_INO		2
#define EXT4_FEATURE_INCOMPAT_64BIT	0x80
#define EXT4_EXTENTS_FL		0x80000
#define EXT4_INLINE_DATA_FL	0x10000000
#define EXT4_EXT_MAGIC		0xf30a
#define S_IFMT			0170000
#define S_IFDIR			0040000
#define S_IFREG			0100000
#define S_IFLNK			0120000
#define MAX_BLOCK		65536
#define MAX_SYMLINKS		8

struct ext4 {
	struct blk *dev;
	u32 bsize;		/* filesystem block size */
	u32 inodes_per_group, inode_size, desc_size, first_data_block;
	u64 groups;
};

struct inode {
	u16 mode;
	u32 flags;
	u64 size;
	u8 block[60];		/* i_block: extent tree root or symlink text */
};

static struct ext4 ext4_state;
static u8 meta[MAX_BLOCK] __attribute__((aligned(4096)));	/* one metadata block */

static u16 le16(const u8 *p)
{
	return p[0] | (u16)p[1] << 8;
}
static u32 le32(const u8 *p)
{
	return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static int read_fs(struct ext4 *x, u64 off, u64 len, void *buf)
{
	return blk_read_bytes(x->dev, off, len, buf);
}

static int read_inode(struct ext4 *x, u32 ino, struct inode *in)
{
	u8 d[64], raw[160];
	u64 group, index, table, off;
	u32 n;

	if (!ino)
		return -1;
	group = (ino - 1) / x->inodes_per_group;
	index = (ino - 1) % x->inodes_per_group;
	if (group >= x->groups)
		return -1;
	off = (u64)(x->first_data_block + 1) * x->bsize + group * x->desc_size;
	if (read_fs(x, off, x->desc_size > 64 ? 64 : x->desc_size, d))
		return -1;
	table = le32(d + 8);
	if (x->desc_size >= 64)
		table |= (u64)le32(d + 0x28) << 32;
	n = x->inode_size < sizeof(raw) ? x->inode_size : sizeof(raw);
	if (read_fs(x, table * x->bsize + index * x->inode_size, n, raw))
		return -1;
	in->mode = le16(raw);
	in->size = le32(raw + 4) | (u64)le32(raw + 0x6c) << 32;
	in->flags = le32(raw + 0x20);
	memcpy(in->block, raw + 0x28, 60);
	return 0;
}

/*
 * Physical block of logical block lblk, 0 for a hole; *run = how many
 * following logical blocks map contiguously (for one big read).
 */
static long long map_block(struct ext4 *x, const struct inode *in, u32 lblk, u32 *run)
{
	const u8 *node = in->block;
	int depth, i, entries, level = 0;

	if (!(in->flags & EXT4_EXTENTS_FL))
		return -1;
	for (;;) {
		if (le16(node) != EXT4_EXT_MAGIC)
			return -2;
		entries = le16(node + 2);
		depth = le16(node + 6);
		if (depth == 0) {
			for (i = 0; i < entries; i++) {
				const u8 *e = node + 12 + i * 12;
				u32 first = le32(e);
				u32 len = le16(e + 4);
				u64 start = le32(e + 8) | (u64)le16(e + 6) << 32;

				if (len > 32768)	/* uninitialised extent: reads as zeros */
					len -= 32768, start = 0;
				if (lblk >= first && lblk < first + len) {
					*run = first + len - lblk;
					return start ? (long long)(start + (lblk - first)) : 0;
				}
			}
			*run = 1;
			return 0;	/* hole */
		} else {
			u64 child = 0;

			for (i = 0; i < entries; i++) {
				const u8 *e = node + 12 + i * 12;

				if (le32(e) > lblk)
					break;
				child = le32(e + 4) | (u64)le16(e + 8) << 32;
			}
			if (!child || ++level > 8 || x->bsize > MAX_BLOCK)
				return -3;
			if (read_fs(x, child * x->bsize, x->bsize, meta))
				return -4;
			node = meta;
		}
	}
}

/* bytes [0, len) of the inode's data to buf */
static long read_data(struct ext4 *x, const struct inode *in, void *buf, u64 len)
{
	u8 *out = buf;
	u64 done = 0;

	if (in->flags & EXT4_INLINE_DATA_FL)
		return -10;
	while (done < len) {
		u32 lblk = (u32)(done / x->bsize), run;
		long long pblk = map_block(x, in, lblk, &run);
		u64 n = (u64)run * x->bsize;

		if (pblk < 0)
			return (long)pblk;
		if (n > len - done)
			n = len - done;
		if (pblk == 0)
			memset(out + done, 0, n);
		else if (read_fs(x, (u64)pblk * x->bsize, n, out + done))
			return -11;
		done += n;
	}
	return (long)done;
}

/* entry name in directory dir; inode number or 0 */
static u32 dir_lookup(struct ext4 *x, const struct inode *dir, const char *name, u64 nlen)
{
	static u8 blk[MAX_BLOCK] __attribute__((aligned(4096)));
	u64 off;

	for (off = 0; off < dir->size; off += x->bsize) {
		u32 lblk = (u32)(off / x->bsize), run, pos = 0;
		long long pblk = map_block(x, dir, lblk, &run);

		if (pblk <= 0 || read_fs(x, (u64)pblk * x->bsize, x->bsize, blk))
			continue;
		while (pos + 8 <= x->bsize) {
			u32 ino = le32(blk + pos);
			u16 rec = le16(blk + pos + 4);
			u8 nl = blk[pos + 6];

			if (rec < 8 || pos + rec > x->bsize)
				break;
			if (ino && nl == nlen && !memcmp(blk + pos + 8, name, nlen))
				return ino;
			pos += rec;
		}
	}
	return 0;
}

static int resolve(struct ext4 *x, const char *path, struct inode *out, int depth)
{
	static char paths[MAX_SYMLINKS + 1][1024];
	static char link[1024];
	const char *start = path;
	struct inode cur;

	if (depth > MAX_SYMLINKS || read_inode(x, EXT4_ROOT_INO, &cur))
		return -1;
	while (*path) {
		const char *comp, *s;
		u32 ino;

		while (*path == '/')
			path++;
		if (!*path)
			break;
		comp = path;
		for (s = path; *s && *s != '/'; s++)
			;
		if ((cur.mode & S_IFMT) != S_IFDIR)
			return -2;
		ino = dir_lookup(x, &cur, comp, s - comp);
		if (!ino || read_inode(x, ino, &cur))
			return -3;
		path = s;
		if ((cur.mode & S_IFMT) == S_IFLNK) {
			char *np = paths[depth];
			u64 l = cur.size, pre = 0, rest = strlen(path);

			if (l >= sizeof(link))
				return -4;
			if (l < 60 && !(cur.flags & EXT4_EXTENTS_FL))
				memcpy(link, cur.block, l);
			else if (read_data(x, &cur, link, l) != (long)l)
				return -5;
			/* a relative target is relative to the directory of the link */
			if (link[0] != '/')
				pre = comp - start;
			if (pre + l + rest + 1 > sizeof(paths[0]))
				return -6;
			memcpy(np, start, pre);
			memcpy(np + pre, link, l);
			memcpy(np + pre + l, path, rest + 1);
			return resolve(x, np, out, depth + 1);
		}
	}
	*out = cur;
	return 0;
}

static long ext4_stat(struct fs *fs, const char *path)
{
	struct inode in;

	if (resolve(fs->priv, path, &in, 0) || (in.mode & S_IFMT) != S_IFREG)
		return -1;
	return (long)in.size;
}

static long ext4_load(struct fs *fs, const char *path, void *dst, u64 max)
{
	struct inode in;

	if (resolve(fs->priv, path, &in, 0) || (in.mode & S_IFMT) != S_IFREG)
		return -1;
	if (in.size > max)
		return -2;
	return read_data(fs->priv, &in, dst, in.size);
}

int ext4_mount(struct fs *fs, struct blk *part)
{
	struct ext4 *x = &ext4_state;
	static u8 sb[1024];
	u32 log, incompat;
	u64 blocks;

	if (blk_read_bytes(part, 1024, sizeof(sb), sb) || le16(sb + 0x38) != EXT4_MAGIC)
		return -1;
	log = le32(sb + 0x18);
	if (log > 6)
		return -2;
	memset(x, 0, sizeof(*x));
	x->dev = part;
	x->bsize = 1024u << log;
	x->first_data_block = le32(sb + 0x14);
	x->inodes_per_group = le32(sb + 0x28);
	x->inode_size = le32(sb + 0x4c) >= 1 ? le16(sb + 0x58) : 128;
	incompat = le32(sb + 0x60);
	x->desc_size = (incompat & EXT4_FEATURE_INCOMPAT_64BIT) && le16(sb + 0xfe) ? le16(sb + 0xfe) : 32;
	blocks = le32(sb + 4) | ((incompat & EXT4_FEATURE_INCOMPAT_64BIT) ? (u64)le32(sb + 0x150) << 32 : 0);
	if (!x->inodes_per_group || !le32(sb + 0x20) || x->inode_size < 128)
		return -3;
	x->groups = (blocks - x->first_data_block + le32(sb + 0x20) - 1) / le32(sb + 0x20);
	fs->name = "ext4";
	fs->stat = ext4_stat;
	fs->load = ext4_load;
	fs->map = NULL;
	fs->priv = x;
	return 0;
}
