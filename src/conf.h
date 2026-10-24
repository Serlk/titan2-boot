/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
#ifndef TITAN2_BOOT_CONF_H
#define TITAN2_BOOT_CONF_H

#include "io.h"

struct fs;

#define CONF_MAX_ENTRIES	12
#define CONF_PATH		128
#define CONF_APPEND		1024
#define CONF_NAME		48

enum entry_kind {
	ENTRY_LINUX,		/* LINUX + FDT (+ INITRD, APPEND) */
	ENTRY_ANDROID,		/* ANDROID <kernel file> | part:<boot partition> */
};

struct boot_entry {
	enum entry_kind kind;
	char label[CONF_NAME];		/* LABEL */
	char title[CONF_NAME];		/* MENU LABEL, shown in the menu */
	char kernel[CONF_PATH];		/* LINUX/KERNEL, or ANDROID's argument */
	char fdt[CONF_PATH];		/* FDT/DEVICETREE */
	char initrd[CONF_PATH];		/* INITRD */
	char append[CONF_APPEND];	/* APPEND */
	struct fs *fs;			/* where the paths are */
};

struct boot_conf {
	char title[CONF_NAME];		/* MENU TITLE */
	char def[CONF_NAME];		/* DEFAULT label */
	int timeout_ds;			/* TIMEOUT, tenths of a second; -1 = not set */
	int ufs;			/* UFS on|off: read the UFS (titan2-boot); -1 = not set */
	int n;
	struct boot_entry entry[CONF_MAX_ENTRIES];
};

void conf_init(struct boot_conf *c);
/*
 * Parse an extlinux.conf-style text (U-Boot's pxe/extlinux subset plus our
 * ANDROID line) and append its entries; fs is where its paths are. DEFAULT,
 * TIMEOUT and MENU TITLE are taken only if not set yet, so the first file
 * parsed wins. Returns the number of entries added.
 */
int conf_parse(struct boot_conf *c, const char *text, u64 len, struct fs *fs);
int conf_default_index(const struct boot_conf *c);

#endif
