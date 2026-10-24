// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
/*
 * Host test for src/inflate.c and src/lz4.c:
 *   decompress-test <compressed> <expected>
 * decompresses with decompress_any() and compares with the expected file.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "decompress.h"

static void *slurp(const char *path, long *size)
{
	FILE *f = fopen(path, "rb");
	void *buf;

	if (!f) {
		perror(path);
		exit(2);
	}
	fseek(f, 0, SEEK_END);
	*size = ftell(f);
	rewind(f);
	buf = malloc(*size ? *size : 1);
	if (fread(buf, 1, *size, f) != (size_t)*size) {
		perror(path);
		exit(2);
	}
	fclose(f);
	return buf;
}

int main(int argc, char **argv)
{
	long csize, esize, n;
	void *c, *e, *out;

	if (argc != 3) {
		fprintf(stderr, "usage: %s <compressed> <expected>\n", argv[0]);
		return 2;
	}
	c = slurp(argv[1], &csize);
	e = slurp(argv[2], &esize);
	out = malloc(esize + 4096);
	n = decompress_any(c, csize, out, esize + 4096);
	if (n != esize || memcmp(out, e, esize)) {
		printf("FAIL %s: got %ld, expected %ld\n", argv[1], n, esize);
		return 1;
	}
	printf("OK %s (%ld -> %ld)\n", argv[1], csize, n);
	free(out);
	free(e);
	free(c);
	return 0;
}
