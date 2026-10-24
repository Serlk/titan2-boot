// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
/* Block devices: partitions on top of a disk, GPT lookup by name. */
#include <string.h>

#include "fs.h"

#define BOUNCE_MAX	65536

static u8 bounce[BOUNCE_MAX] __attribute__((aligned(4096)));

int blk_read(struct blk *b, u64 lba, u64 count, void *buf)
{
	if (!count)
		return 0;
	if (lba + count > b->nblocks || lba + count < lba)
		return -1;
	if (b->parent)
		return blk_read(b->parent, b->start + lba, count, buf);
	return b->read(b, lba, count, buf);
}

int blk_read_bytes(struct blk *b, u64 off, u64 len, void *buf)
{
	u8 *out = buf;
	u64 bs = b->bsize;

	if (bs > BOUNCE_MAX)
		return -1;
	while (len) {
		u64 lba = off / bs, in = off % bs;

		if (in || len < bs) {
			u64 n = bs - in < len ? bs - in : len;

			if (blk_read(b, lba, 1, bounce))
				return -1;
			memcpy(out, bounce + in, n);
			out += n;
			off += n;
			len -= n;
		} else {
			u64 cnt = len / bs;

			if (blk_read(b, lba, cnt, out))
				return -1;
			out += cnt * bs;
			off += cnt * bs;
			len -= cnt * bs;
		}
	}
	return 0;
}

struct gpt_header {
	char sig[8];
	u32 revision, header_size, header_crc, reserved;
	u64 my_lba, alt_lba, first_lba, last_lba;
	u8 guid[16];
	u64 entries_lba;
	u32 nentries, entry_size, entries_crc;
};

struct gpt_entry {
	u8 type[16], guid[16];
	u64 first, last, attrs;
	u16 name[36];
};

static int name_eq(const u16 *u, const char *a)
{
	int i;

	for (i = 0; i < 36; i++) {
		u16 c = u[i];

		if (c != (u8)a[i])
			return 0;
		if (!c)
			return 1;
	}
	return a[36] == 0;
}

int gpt_find(struct blk *disk, const char *name, struct blk *part)
{
	struct gpt_header h;
	u64 i;

	if (blk_read_bytes(disk, disk->bsize, sizeof(h), &h) || memcmp(h.sig, "EFI PART", 8))
		return -1;
	if (h.entry_size < sizeof(struct gpt_entry) || h.entry_size > 1024 || h.nentries > 1024)
		return -2;
	for (i = 0; i < h.nentries; i++) {
		struct gpt_entry e;

		if (blk_read_bytes(disk, h.entries_lba * disk->bsize + i * h.entry_size, sizeof(e), &e))
			return -3;
		if (!e.first && !e.last)
			continue;
		if (name_eq(e.name, name)) {
			if (e.last < e.first || e.last >= disk->nblocks)
				return -4;
			memset(part, 0, sizeof(*part));
			part->parent = disk;
			part->bsize = disk->bsize;
			part->start = e.first;
			part->nblocks = e.last - e.first + 1;
			return 0;
		}
	}
	return -5;
}
