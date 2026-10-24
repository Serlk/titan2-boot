// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
/* Host test for src/conf.c */
#include <stdio.h>
#include <string.h>

#include "conf.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

static const char pack_conf[] =
	"# titan2-boot\n"
	"MENU TITLE Titan 2\r\n"
	"UFS on\n"
	"TIMEOUT 50\n"
	"DEFAULT android\n"
	"\n"
	"LABEL android\n"
	"  MENU LABEL Android\n"
	"  ANDROID /android/Image.lz4\n"
	"label linux\n"
	"\tmenu label Linux 7.2 (full)\n"
	"\tlinux /linux/Image\n"
	"\tfdt /linux/titan2.dtb\n"
	"\tinitrd /linux/initramfs.cpio.gz\n"
	"\tappend console=tty0 loglevel=7  t2.root=nbd:172.16.42.2:10809:t2root  \n"
	"LABEL broken\n"
	"  MENU LABEL no kernel\n"
	"LABEL drm\n"
	"  LINUX /linux/Image\n"
	"  DEVICETREE /linux/titan2-drm.dtb\n"
	"  FDTDIR /ignored\n";

static const char part_conf[] =
	"DEFAULT other\nTIMEOUT 30\nMENU TITLE ignored\n"
	"LABEL b\n MENU DEFAULT\n ANDROID part:boot_b\n";

int main(void)
{
	static struct boot_conf c;
	struct boot_entry *e;

	conf_init(&c);
	CHECK(conf_parse(&c, pack_conf, sizeof(pack_conf) - 1, NULL) == 3);
	CHECK(!strcmp(c.title, "Titan 2"));
	CHECK(c.timeout_ds == 50);
	CHECK(c.ufs == 1);
	CHECK(!strcmp(c.def, "android"));
	e = &c.entry[0];
	CHECK(e->kind == ENTRY_ANDROID && !strcmp(e->kernel, "/android/Image.lz4") && !strcmp(e->title, "Android"));
	e = &c.entry[1];
	CHECK(e->kind == ENTRY_LINUX && !strcmp(e->title, "Linux 7.2 (full)"));
	CHECK(!strcmp(e->fdt, "/linux/titan2.dtb") && !strcmp(e->initrd, "/linux/initramfs.cpio.gz"));
	CHECK(!strcmp(e->append, "console=tty0 loglevel=7  t2.root=nbd:172.16.42.2:10809:t2root"));
	e = &c.entry[2];
	CHECK(!strcmp(e->label, "drm") && !strcmp(e->title, "drm") && !strcmp(e->fdt, "/linux/titan2-drm.dtb"));
	CHECK(conf_default_index(&c) == 0);

	CHECK(conf_parse(&c, part_conf, sizeof(part_conf) - 1, NULL) == 1);
	CHECK(c.n == 4 && c.timeout_ds == 50 && !strcmp(c.def, "android") && !strcmp(c.title, "Titan 2"));
	CHECK(c.entry[3].kind == ENTRY_ANDROID && !strcmp(c.entry[3].kernel, "part:boot_b"));

	conf_init(&c);
	CHECK(conf_parse(&c, part_conf, sizeof(part_conf) - 1, NULL) == 1);
	CHECK(conf_default_index(&c) == 0 && c.timeout_ds == 30);
	CHECK(c.ufs == -1);
	conf_init(&c);
	CHECK(conf_parse(&c, "ufs off\nUFS on\n", 15, NULL) == 0 && c.ufs == 0);
	printf("conf OK\n");
	return 0;
}
