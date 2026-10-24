// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
/* The pack (src/pack.h) as a read-only file source: named files already in RAM. */
#include <string.h>

#include "fs.h"
#include "pack.h"

static const struct pack_file *find(struct fs *fs, const char *path, u64 *size)
{
	const struct pack_header *h = fs->priv;
	u32 i;

	while (*path == '/')
		path++;
	for (i = 0; i < h->count; i++) {
		const struct pack_file *f = &h->file[i];

		if (!strncmp(f->name, path, sizeof(f->name)) && strlen(path) < sizeof(f->name)) {
			*size = f->size;
			return f;
		}
	}
	return NULL;
}

static const void *pack_map(struct fs *fs, const char *path, u64 *size)
{
	const struct pack_file *f = find(fs, path, size);

	return f ? (const void *)((u64)fs->priv - PACK_OFFSET + f->offset) : NULL;
}

static long pack_stat(struct fs *fs, const char *path)
{
	u64 size;

	return find(fs, path, &size) ? (long)size : -1;
}

static long pack_load(struct fs *fs, const char *path, void *dst, u64 max)
{
	u64 size;
	const void *p = pack_map(fs, path, &size);

	if (!p)
		return -1;
	if (size > max)
		return -2;
	memmove(dst, p, size);
	return (long)size;
}

int packfs_init(struct fs *fs, const void *pack)
{
	const struct pack_header *h = pack;
	u32 i;

	if (memcmp(h->magic, PACK_MAGIC, 8) || h->version != PACK_VERSION || h->count > PACK_MAX)
		return -1;
	for (i = 0; i < h->count; i++)
		if (h->file[i].offset < PACK_OFFSET ||
		    h->file[i].offset + h->file[i].size > h->total_size ||
		    !memchr(h->file[i].name, 0, sizeof(h->file[i].name)))
			return -2;
	fs->name = "pack";
	fs->stat = pack_stat;
	fs->load = pack_load;
	fs->map = pack_map;
	fs->priv = (void *)pack;
	return 0;
}
