// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
/*
 * Host test for src/fdtfix.c: merge LK's FDT (e.g. /sys/firmware/fdt saved
 * from Android) into a mainline DTB, as titan2-boot does before it starts
 * Linux, and write the result for dtc/fdtget checks.
 *   fdtfix-test <lk.dtb> <mainline.dtb> <out.dtb>
 */
#include <stdio.h>
#include <stdlib.h>

#include <libfdt.h>

#include "fdtfix.h"

void uart_putc(char c)
{
	putchar(c);
}

void uart_puts(const char *s)
{
	fputs(s, stdout);
}

void uart_hex(u64 v)
{
	printf("0x%lx", v);
}

void log_puts(const char *s)
{
	printf("[titan2-boot] %s", s);
}

void log_hex(const char *s, u64 v)
{
	printf("[titan2-boot] %s0x%lx\n", s, v);
}

static void *slurp(const char *path, long *size)
{
	FILE *f = fopen(path, "rb");
	void *buf;

	if (!f) {
		perror(path);
		exit(1);
	}
	fseek(f, 0, SEEK_END);
	*size = ftell(f);
	rewind(f);
	buf = malloc(*size);
	if (fread(buf, 1, *size, f) != (size_t)*size) {
		perror(path);
		exit(1);
	}
	fclose(f);
	return buf;
}

int main(int argc, char **argv)
{
	long lk_size, dtb_size;
	void *lk, *dtb, *out;
	FILE *f;
	int ret;

	if (argc != 4) {
		fprintf(stderr, "usage: %s <lk.dtb> <mainline.dtb> <out.dtb>\n", argv[0]);
		return 2;
	}
	setvbuf(stdout, NULL, _IONBF, 0);
	lk = slurp(argv[1], &lk_size);
	dtb = slurp(argv[2], &dtb_size);
	out = malloc(0x100000);

	printf("LK boot mode %d\n", lk_boot_mode(lk));
	ret = linux_fdt_build(lk, dtb, out, 0x100000,
			      "console=tty0 loglevel=7 t2.root=PARTLABEL=linux",
			      0x40600000, 0x40780000);
	if (ret) {
		fprintf(stderr, "linux_fdt_build: %s\n", fdt_strerror(ret));
		return 1;
	}
	f = fopen(argv[3], "wb");
	if (!f || fwrite(out, 1, fdt_totalsize(out), f) != fdt_totalsize(out)) {
		perror(argv[3]);
		return 1;
	}
	fclose(f);
	free(out);
	free(dtb);
	free(lk);
	return 0;
}
