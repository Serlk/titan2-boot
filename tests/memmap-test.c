// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
/* Host test for src/memmap.c on LK's FDT: memmap-test <lk.dtb> */
#include <stdlib.h>

#include <libfdt.h>

#include "host-stubs.h"
#include "memmap.h"

static void *slurp(const char *path)
{
	FILE *f = fopen(path, "rb");
	long n;
	void *b;

	if (!f)
		exit(2);
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	rewind(f);
	b = malloc(n);
	if (fread(b, 1, n, f) != (size_t)n)
		exit(2);
	fclose(f);
	return b;
}

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

int main(int argc, char **argv)
{
	void *fdt;
	u64 a, b;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc != 2)
		return 2;
	fdt = slurp(argv[1]);
	CHECK(memmap_init(fdt) == 0);
	memmap_mark(0x40000000, 0x40200000, MEM_BUSY, "titan2-boot");
	memmap_mark(0x47c80000, 0x47d00000, MEM_BUSY, "lk-fdt");
	memmap_mark(0x66f00000, 0x6af00000, MEM_BUSY, "android-ramdisk");
	memmap_log();

	CHECK(memmap_type(0x48800000, 0x48a00000) == MEM_RESERVED);	/* BL31 */
	CHECK(memmap_type(0x40000000, 0x40100000) == MEM_BUSY);
	CHECK(memmap_type(0x40200000, 0x40400000) == MEM_FREE);
	CHECK(memmap_type(0xfe04f000, 0xfe050000) == MEM_FB);
	CHECK(memmap_type(0x30000000, 0x30001000) == MEM_RESERVED);	/* not DRAM */
	CHECK(memmap_type(0x47c00000, 0x47e00000) == MEM_BUSY);

	a = memmap_alloc(48 << 20, 2 << 20, 0, "kernel");
	printf("kernel at 0x%lx\n", a);
	CHECK(a == 0x40200000);
	b = memmap_alloc(256 << 20, 4096, 0, "scratch");
	printf("scratch at 0x%lx\n", b);
	CHECK(b && memmap_type(b, b + (256 << 20)) == MEM_BUSY);
	CHECK(memmap_alloc(8ULL << 30, 4096, 0, "too big") == 0);
	free(fdt);
	printf("memmap OK\n");
	return 0;
}
