// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
/*
 * Starting a menu entry. Everything is loaded into memory handed out by
 * memmap.c (free DRAM between LK's reservations); kernels go to 2 MiB-aligned
 * blocks and run there.
 *
 *  - ANDROID: the kernel (raw, gzip or lz4; a file, or a boot.img partition)
 *    with LK's FDT and ramdisk as they are: LK has built them from
 *    vendor_boot_a, init_boot_a and dtbo_a, which stay Android's.
 *  - LINUX: kernel, DTB, initrd from the entry's file source; the DTB gets
 *    LK's run-time data (fdtfix.c), the command line is APPEND.
 */
#include <string.h>

#include "boot.h"
#include "decompress.h"
#include "display.h"
#include "fdtfix.h"
#include "memmap.h"
#include "mmu.h"
#include "ufs.h"

#define BLOCK_2M	(2UL << 20)
#define KERNEL_MAX	(128UL << 20)	/* decompressed kernel, upper bound */
#define FDT_EXTRA	(512UL << 10)	/* room for LK's nodes in the mainline DTB */
#define IMAGE_MAGIC	0x644d5241	/* "ARM\x64" */

static struct blk *disk;

void boot_set_disk(struct blk *d)
{
	disk = d;
}

struct image_header {
	u32 code0, code1;
	u64 text_offset, image_size, flags, res2, res3, res4;
	u32 magic, res5;
};

/* a file of the entry's source: in place if the source maps it, else loaded */
static const void *get_file(struct fs *fs, const char *path, u64 *size, const char *what)
{
	long n;
	void *dst;
	u64 sz;
	const void *p;

	if (fs->map && (p = fs->map(fs, path, &sz))) {
		*size = sz;
		return p;
	}
	n = fs->stat(fs, path);
	if (n < 0)
		return NULL;
	dst = (void *)memmap_alloc(n ? (u64)n : 1, 4096, 0, what);
	if (!dst || fs->load(fs, path, dst, n) != n)
		return NULL;
	*size = n;
	return dst;
}

/* decompress (or copy) a kernel to a fresh 2 MiB-aligned block; its address or 0 */
static u64 place_kernel(const void *src, u64 size, const char **err)
{
	const struct image_header *ih;
	void *tmp;
	long n;
	u64 dst, need;

	tmp = (void *)memmap_alloc(KERNEL_MAX, 4096, 0, "kernel-unpack");
	if (!tmp) {
		*err = "no memory to unpack the kernel";
		return 0;
	}
	n = decompress_any(src, size, tmp, KERNEL_MAX);
	if (n < (long)sizeof(*ih)) {
		*err = "kernel does not unpack (gzip/lz4)";
		return 0;
	}
	ih = tmp;
	if (ih->magic != IMAGE_MAGIC) {
		*err = "kernel is not an arm64 Image";
		return 0;
	}
	need = ih->image_size > (u64)n ? ih->image_size : (u64)n;
	dst = memmap_alloc(need, BLOCK_2M, 0, "kernel");
	if (!dst) {
		*err = "no memory for the kernel";
		return 0;
	}
	memmove((void *)dst, tmp, n);
	log_hex("kernel at ", dst);
	log_hex("kernel size ", need);
	return dst;
}

static void __attribute__((noreturn)) jump(u64 kernel, u64 fdt)
{
	disp_release();
	ufs_close();
	wdt_kick();
	mmu_disable();
	enter_kernel(kernel, fdt);
}

static const char *boot_android(const struct boot_entry *e, const void *lk_fdt)
{
	const char *err = NULL;
	const void *src;
	u64 size, k;

	if (!strncmp(e->kernel, "part:", 5)) {
		struct blk part;
		u64 off;
		long n;
		void *buf;

		if (!disk)
			return "no UFS access for a boot partition";
		if (gpt_find(disk, e->kernel + 5, &part))
			return "boot partition not found";
		n = bootimg_kernel_size(&part, &off);
		if (n < 0)
			return "no Android boot.img in the partition";
		buf = (void *)memmap_alloc(n, 4096, 0, "boot.img kernel");
		if (!buf || bootimg_kernel(&part, buf, n) != n)
			return "could not read the boot.img kernel";
		src = buf;
		size = n;
	} else {
		src = get_file(e->fs, e->kernel, &size, "android kernel file");
		if (!src)
			return "Android kernel file not found";
	}
	k = place_kernel(src, size, &err);
	if (!k)
		return err;
	log_puts("starting Android\n");
	jump(k, (u64)lk_fdt);
}

static const char *boot_linux(const struct boot_entry *e, const void *lk_fdt)
{
	const char *err = NULL;
	const void *kernel, *dtb, *initrd = NULL;
	u64 ksize, dsize, isize = 0, k, fdt, fdt_size;

	if (!e->fdt[0])
		return "no FDT line for this entry";
	kernel = get_file(e->fs, e->kernel, &ksize, "linux kernel file");
	if (!kernel)
		return "kernel file not found";
	dtb = get_file(e->fs, e->fdt, &dsize, "dtb");
	if (!dtb)
		return "device tree file not found";
	if (e->initrd[0]) {
		initrd = get_file(e->fs, e->initrd, &isize, "initrd");
		if (!initrd)
			return "initrd file not found";
	}
	k = place_kernel(kernel, ksize, &err);
	if (!k)
		return err;

	fdt_size = dsize + FDT_EXTRA;
	fdt = memmap_alloc(fdt_size, 4096, 0, "fdt");
	if (!fdt)
		return "no memory for the device tree";
	if (linux_fdt_build(lk_fdt, dtb, (void *)fdt, fdt_size, e->append,
			    initrd ? (u64)initrd : 0, initrd ? (u64)initrd + isize : 0))
		return "could not build the Linux device tree";
	log_puts("starting Linux\n");
	jump(k, fdt);
}

const char *boot_entry(const struct boot_entry *e, const void *lk_fdt)
{
	if (!lk_fdt)
		return "no device tree from LK";
	log_puts("entry ");
	uart_puts(e->label);
	uart_puts(" from ");
	uart_puts(e->fs ? e->fs->name : "?");
	uart_putc('\n');
	return e->kind == ENTRY_ANDROID ? boot_android(e, lk_fdt) : boot_linux(e, lk_fdt);
}
