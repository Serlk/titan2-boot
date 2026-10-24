// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
/*
 * Host test for src/blk.c (GPT) and src/ext4.c on a disk image:
 *   storage-test <disk.img> <block size> <partition> [<path in fs> <host file>]...
 * Each path is loaded through GPT + ext4 and compared with the host file;
 * a host file "-" means the path must not exist.
 */
#include <stdlib.h>
#include <string.h>

#include "fs.h"
#include "host-stubs.h"

static FILE *disk;

static int file_read(struct blk *b, u64 lba, u64 count, void *buf)
{
	if (fseeko(disk, (off_t)(lba * b->bsize), SEEK_SET) ||
	    fread(buf, b->bsize, count, disk) != count)
		return -1;
	return 0;
}

int main(int argc, char **argv)
{
	struct blk dev = { 0 }, part;
	struct fs fs;
	int i, ret;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc < 4)
		return 2;
	disk = fopen(argv[1], "rb");
	if (!disk)
		return 2;
	fseeko(disk, 0, SEEK_END);
	dev.bsize = atoi(argv[2]);
	dev.nblocks = ftello(disk) / dev.bsize;
	dev.read = file_read;

	ret = gpt_find(&dev, argv[3], &part);
	if (ret) {
		printf("FAIL gpt_find %s: %d\n", argv[3], ret);
		return 1;
	}
	printf("partition %s: start %lu, %lu blocks\n", argv[3], part.start, part.nblocks);
	if (gpt_find(&dev, "nonexistent", &part) == 0) {
		printf("FAIL found a nonexistent partition\n");
		return 1;
	}
	gpt_find(&dev, argv[3], &part);
	ret = ext4_mount(&fs, &part);
	if (ret) {
		printf("FAIL ext4_mount: %d\n", ret);
		return 1;
	}
	for (i = 4; i + 1 < argc; i += 2) {
		long size = fs.stat(&fs, argv[i]), n;
		FILE *f;
		char *want, *got;

		if (!strcmp(argv[i + 1], "-")) {
			if (size >= 0) {
				printf("FAIL %s exists\n", argv[i]);
				return 1;
			}
			printf("OK %s missing\n", argv[i]);
			continue;
		}
		f = fopen(argv[i + 1], "rb");
		fseek(f, 0, SEEK_END);
		n = ftell(f);
		rewind(f);
		want = malloc(n + 1);
		got = malloc(n + 1);
		if (fread(want, 1, n, f) != (size_t)n)
			return 2;
		fclose(f);
		if (size != n || fs.load(&fs, argv[i], got, n) != n || memcmp(got, want, n)) {
			printf("FAIL %s: stat %ld, expected %ld\n", argv[i], size, n);
			return 1;
		}
		if (n && fs.load(&fs, argv[i], got, n - 1) >= 0) {
			printf("FAIL %s: loaded into a too small buffer\n", argv[i]);
			return 1;
		}
		printf("OK %s (%ld bytes)\n", argv[i], n);
		free(want);
		free(got);
	}
	fclose(disk);
	return 0;
}
