// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
/*
 * titan2-boot: boot menu after the stock LK.
 *
 * LK loads boot_a (this loader and its file pack, src/pack.h) exactly like a
 * kernel, with vendor_boot_a, init_boot_a and dtbo_a stock, so the FDT in x0
 * is Android's. The menu lists the entries of the pack's extlinux.conf
 * (src/conf.c); with "UFS on" in it, one more item, "Linux partition", which
 * starts the UFS only when chosen (or as the default) and lists the entries of
 * /boot/extlinux/extlinux.conf on the "linux" partition; then Fastboot /
 * Recovery / Reboot, which go through LK via the reboot-mode register.
 * Non-normal LK boots (recovery, power-off charging, alarm, ...) go straight
 * to the first Android entry of the pack, which handles them. Neither they
 * nor a normal Android boot ever touch the UFS.
 */
#include <string.h>

#include <libfdt.h>

#include "boot.h"
#include "conf.h"
#include "display.h"
#include "fdtfix.h"
#include "io.h"
#include "memmap.h"
#include "mmu.h"
#include "pack.h"
#include "ufs.h"

extern unsigned long saved_fdt;
extern char _text[], _end[];

/* MediaTek boot modes (atag,boot) that show the menu */
#define BOOTMODE_NORMAL		0
#define BOOTMODE_SW_REBOOT	3

#define LINUX_CONF	"/boot/extlinux/extlinux.conf"
/* DEFAULT label in the pack's config for the "Linux partition" item */
#define UFS_LABEL	"ufs"
#define CONF_TEXT_MAX	65536

#define COL_BG		0xff101418
#define COL_FG		0xffd8dde3
#define COL_DIM		0xff7d8590
#define COL_SEL_BG	0xff3b82f6
#define COL_SEL_FG	0xffffffff
#define COL_ERR		0xffff6b6b
#define COL_BAR_BG	0xff2a3038
#define COL_TITLE	0xff8ab4f8

#define POLL_US		10000
#define DEFAULT_TIMEOUT_DS	50

enum action { ACT_ENTRY, ACT_UFS, ACT_FASTBOOT, ACT_RECOVERY, ACT_REBOOT };

struct item {
	enum action act;
	const char *label;
	const struct boot_entry *entry;
};

static struct boot_conf conf, ufs_conf;
static struct fs packfs, linuxfs;
static struct blk ufs_disk, linux_part;
static struct item items[2 * CONF_MAX_ENTRIES + 4];
static int n_items;
static const char *menu_title = "Titan 2";
static char storage_status[64] = "UFS: off";
static char conf_text[CONF_TEXT_MAX];

/* ---- screen ---- */

#define TITLE_Y		140
#define ITEMS_Y		360
#define ITEMS_END	1000
#define ITEM_X		120
#define HINT_Y		1250
#define STATUS_Y	1080
#define BAR_X		200
#define BAR_W		(FB_W - 2 * BAR_X)
#define BAR_Y		(STATUS_Y + TEXT_H(2) + 24)
#define BAR_H		12

/* set when the PMIC answers: Power selects, else Volume Down does (as in LK's menu) */
static int power_key_ok;
static int item_h, item_s;	/* layout for n_items */

static void layout(void)
{
	int avail = ITEMS_END - ITEMS_Y;

	if (n_items * 120 <= avail) {
		item_h = 120;
		item_s = 2;
	} else {
		item_h = avail / n_items;
		item_s = 1;
	}
}

static void draw_item(int i, int sel)
{
	int y = ITEMS_Y + i * item_h;
	int pad = (item_h - TEXT_H(item_s)) / 2;
	u32 bg = sel ? COL_SEL_BG : COL_BG;
	u32 fg = sel ? COL_SEL_FG : COL_FG;

	if (pad > 20)
		pad = 20;
	fb_fill(ITEM_X - 40, y - pad, FB_W - 2 * (ITEM_X - 40), TEXT_H(item_s) + 2 * pad, bg);
	fb_text(ITEM_X, y, item_s, fg, bg, items[i].label);
}

static void draw_status(const char *s, u32 col)
{
	fb_fill(0, STATUS_Y, FB_W, BAR_Y + BAR_H - STATUS_Y, COL_BG);
	if (s)
		fb_text((FB_W - fb_text_width(2, s)) / 2, STATUS_Y, 2, col, COL_BG, s);
}

/* countdown bar under the status line, full at the start, empty at the deadline */
static void draw_bar(u64 left_us, u64 total_us)
{
	int w = (int)((u64)BAR_W * left_us / total_us);

	fb_fill(BAR_X, BAR_Y, w, BAR_H, COL_SEL_BG);
	fb_fill(BAR_X + w, BAR_Y, BAR_W - w, BAR_H, COL_BAR_BG);
}

static void draw_menu(int sel)
{
	const char *hint = power_key_ok ? "Volume: choose     Power: start" :
					  "Vol Up: next     Vol Down: start";
	int i;

	fb_fill(0, 0, FB_W, FB_H, COL_BG);
	fb_text((FB_W - fb_text_width(3, menu_title)) / 2, TITLE_Y, 3, COL_TITLE, COL_BG, menu_title);
	fb_text((FB_W - fb_text_width(1, storage_status)) / 2, TITLE_Y + TEXT_H(3) + 24, 1, COL_DIM,
		COL_BG, storage_status);
	for (i = 0; i < n_items; i++)
		draw_item(i, i == sel);
	fb_text((FB_W - fb_text_width(1, hint)) / 2, HINT_Y, 1, COL_DIM, COL_BG, hint);
}

static void show_error(const char *what)
{
	log_puts("error: ");
	uart_puts(what);
	uart_putc('\n');
	draw_status(what, COL_ERR);
	disp_flush();
}

/* ---- entries ---- */

static void add_item(enum action act, const char *label, const struct boot_entry *e)
{
	items[n_items].act = act;
	items[n_items].label = label;
	items[n_items].entry = e;
	n_items++;
}

static int fmt_dec(char *p, u64 v)
{
	char tmp[24];
	int n = 0, i;

	do {
		tmp[n++] = '0' + v % 10;
		v /= 10;
	} while (v);
	for (i = 0; i < n; i++)
		p[i] = tmp[n - 1 - i];
	p[n] = 0;
	return n;
}

static void read_conf(struct boot_conf *c, struct fs *fs, const char *path)
{
	long n = fs->load(fs, path, conf_text, sizeof(conf_text) - 1);

	if (n < 0) {
		log_puts("no ");
		uart_puts(path);
		uart_puts(" on ");
		uart_puts(fs->name);
		uart_putc('\n');
		return;
	}
	conf_text[n] = 0;
	log_hex("entries from config: ", conf_parse(c, conf_text, n, fs));
}

static void find_entries(void)
{
	const struct pack_header *pack = (const void *)(LOAD_ADDR + PACK_OFFSET);

	conf_init(&conf);
	if (!packfs_init(&packfs, pack)) {
		memmap_mark(LOAD_ADDR, LOAD_ADDR + pack->total_size, MEM_BUSY, "pack");
		read_conf(&conf, &packfs, PACK_CONF);
	} else {
		log_puts("no file pack\n");
	}
	if (conf.ufs != 1)
		log_puts("UFS not enabled in the pack's config (UFS on)\n");
	if (conf.title[0])
		menu_title = conf.title;
}

/* start the UFS once, on first use; returns an error or NULL */
static const char *start_ufs(void)
{
	static int tried;
	struct blk part;
	u64 gib;
	char *p;

	if (ufs_disk.nblocks)
		return NULL;
	if (tried)
		return "UFS did not start";
	tried = 1;
	if (ufs_open(&ufs_disk)) {
		strcpy(storage_status, "UFS: failed to start");
		return "UFS did not start";
	}
	gib = (ufs_disk.nblocks * ufs_disk.bsize) >> 30;
	boot_set_disk(&ufs_disk);
	strcpy(storage_status, "UFS: LU2 ");
	p = storage_status + strlen(storage_status);
	p += fmt_dec(p, gib);
	strcpy(p, gpt_find(&ufs_disk, "boot_a", &part) ? " GiB, no GPT" : " GiB, GPT ok");
	return NULL;
}

/*
 * The "Linux partition" item: start the UFS, read the linux partition's
 * extlinux.conf into ufs_conf. Returns an error for the status line or NULL.
 */
static const char *open_linux_partition(void)
{
	const char *err = start_ufs();

	if (err)
		return err;
	if (gpt_find(&ufs_disk, "linux", &linux_part))
		return "no partition \"linux\"";
	if (ext4_mount(&linuxfs, &linux_part)) {
		strcat(storage_status, ", linux: not ext4");
		return "partition \"linux\" is not ext4";
	}
	strcat(storage_status, ", linux: ext4");
	conf_init(&ufs_conf);
	read_conf(&ufs_conf, &linuxfs, LINUX_CONF);
	if (!ufs_conf.n)
		return "no entries in " LINUX_CONF;
	return NULL;
}

/* replace the "Linux partition" item at index at by the partition's entries */
static void expand_linux_partition(int at)
{
	int i, n = ufs_conf.n;

	for (i = n_items - 1; i > at; i--)
		items[i + n - 1] = items[i];
	for (i = 0; i < n; i++) {
		items[at + i].act = ACT_ENTRY;
		items[at + i].label = ufs_conf.entry[i].title;
		items[at + i].entry = &ufs_conf.entry[i];
	}
	n_items += n - 1;
}

static void run(const struct item *it, const void *lk_fdt)
{
	const char *err;

	switch (it->act) {
	case ACT_ENTRY:
		draw_status(it->entry->kind == ENTRY_ANDROID ? "Starting Android..." :
			    "Starting Linux...", COL_FG);
		disp_flush();
		err = NULL;
		if (it->entry->kind == ENTRY_ANDROID && !strncmp(it->entry->kernel, "part:", 5))
			err = start_ufs();
		if (!err)
			err = boot_entry(it->entry, lk_fdt);
		show_error(err);
		break;
	case ACT_UFS:
		/* see linux_partition() */
		break;
	case ACT_FASTBOOT:
		disp_release();
		reboot_with_mode(RBMODE_FASTBOOT);
	case ACT_RECOVERY:
		disp_release();
		reboot_with_mode(RBMODE_RECOVERY);
	case ACT_REBOOT:
		disp_release();
		reboot_with_mode(RBMODE_NORMAL);
	}
}

/*
 * The "Linux partition" item at index at was started: by the countdown
 * (automatic: boot the partition's default entry) or by a key (show its
 * entries). Returns the item to select afterwards.
 */
static int linux_partition(int at, int automatic, const void *lk_fdt)
{
	const char *err;
	int d;

	draw_status("Starting the UFS...", COL_FG);
	disp_flush();
	err = open_linux_partition();
	if (err) {
		draw_menu(at);
		show_error(err);
		return at;
	}
	d = conf_default_index(&ufs_conf);
	expand_linux_partition(at);
	layout();
	draw_menu(at + d);
	if (automatic)
		run(&items[at + d], lk_fdt);
	else
		draw_status(NULL, COL_FG);
	disp_flush();
	return at + d;
}

static void countdown_text(char *buf, const char *name, u64 left_us)
{
	static const char pre[] = "Booting ";
	char *p = buf;
	u64 s = (left_us + 999999) / 1000000;
	const char *q;

	for (q = pre; *q; q++)
		*p++ = *q;
	for (q = name; *q && p < buf + 40; q++)
		*p++ = *q;
	for (q = " in "; *q; q++)
		*p++ = *q;
	if (s >= 100)
		*p++ = '0' + (s / 100) % 10;
	if (s >= 10)
		*p++ = '0' + (s / 10) % 10;
	*p++ = '0' + s % 10;
	*p++ = ' ';
	*p++ = 's';
	*p = 0;
}

static void memory_setup(const void *lk_fdt)
{
	const fdt32_t *p;
	int chosen, len;

	if (memmap_init(lk_fdt)) {
		log_puts("no memory map from LK's FDT, MMU stays off\n");
		return;
	}
	memmap_mark((u64)_text, (u64)_end, MEM_BUSY, "titan2-boot");
	memmap_mark((u64)lk_fdt, (u64)lk_fdt + fdt_totalsize(lk_fdt), MEM_BUSY, "lk-fdt");
	/* Android's ramdisk and bootconfig as LK placed them, and all of mb_ramdisk */
	memmap_mark(RAMDISK_ADDR, RAMDISK_ADDR + RAMDISK_SIZE, MEM_BUSY, "android-ramdisk");
	chosen = fdt_path_offset(lk_fdt, "/chosen");
	p = chosen >= 0 ? fdt_getprop(lk_fdt, chosen, "linux,initrd-start", &len) : NULL;
	if (p) {
		u64 s = len == 8 ? (u64)fdt32_to_cpu(p[0]) << 32 | fdt32_to_cpu(p[1]) : fdt32_to_cpu(p[0]);
		const fdt32_t *q = fdt_getprop(lk_fdt, chosen, "linux,initrd-end", &len);

		if (q) {
			u64 e = len == 8 ? (u64)fdt32_to_cpu(q[0]) << 32 | fdt32_to_cpu(q[1]) :
				fdt32_to_cpu(q[0]);

			memmap_mark(s, e, MEM_BUSY, "android-ramdisk");
		}
	}
#ifdef T2_QEMU
	ufs_qemu_reserve();
#endif
}

void bl_main(void)
{
	const void *lk_fdt = (const void *)saved_fdt;
	int mode, sel = 0, def = 0, prev_up, prev_dn, prev_pwr, counting, i, android = -1;
	u64 el, deadline = 0, last_s = ~0ul, timeout_us;
	char buf[64];

#ifdef T2_QEMU
	/* QEMU's generic loader starts us without x0: the test puts LK's FDT where LK does */
	if (!lk_fdt)
		lk_fdt = (const void *)LK_FDT_ADDR;
#endif
	log_puts("stage 2 alive\n");
	asm volatile("mrs %0, CurrentEL" : "=r"(el));
	log_hex("EL ", el >> 2);
	log_hex("fdt ", (u64)lk_fdt);

	if (!lk_fdt || fdt_check_header(lk_fdt)) {
		log_puts("no valid FDT from LK\n");
		lk_fdt = NULL;
	} else {
		memory_setup(lk_fdt);
		if (memmap_count())
			mmu_enable();
	}

	find_entries();
	for (i = 0; i < conf.n; i++) {
		add_item(ACT_ENTRY, conf.entry[i].title, &conf.entry[i]);
		/* the first Android kernel in the pack: no UFS needed for it */
		if (conf.entry[i].kind == ENTRY_ANDROID &&
		    (android < 0 || !strncmp(conf.entry[android].kernel, "part:", 5)) &&
		    strncmp(conf.entry[i].kernel, "part:", 5))
			android = i;
		else if (android < 0 && conf.entry[i].kind == ENTRY_ANDROID)
			android = i;
	}
	def = conf_default_index(&conf);
	if (conf.ufs == 1) {
		if (!strcmp(conf.def, UFS_LABEL))
			def = n_items;
		add_item(ACT_UFS, "Linux partition", NULL);
	}
	add_item(ACT_FASTBOOT, "Fastboot", NULL);
	add_item(ACT_RECOVERY, "Recovery", NULL);
	add_item(ACT_REBOOT, "Reboot", NULL);

	mode = lk_fdt ? lk_boot_mode(lk_fdt) : -1;
	log_hex("LK boot mode ", (u64)mode);
	if (android >= 0 && mode != BOOTMODE_NORMAL && mode != BOOTMODE_SW_REBOOT) {
		/* recovery, power-off charging, alarm, meta, ...: Android's business */
		log_puts(boot_entry(&conf.entry[android], lk_fdt));
		uart_putc('\n');
	}

	timeout_us = (u64)(conf.timeout_ds >= 0 ? conf.timeout_ds : DEFAULT_TIMEOUT_DS) * 100000;
	if (def < 0)
		def = 0;
	sel = def;
	counting = n_items > 3 && timeout_us;
	if (n_items > 3 && !timeout_us) {
		if (items[def].act == ACT_UFS)
			sel = def = linux_partition(def, 1, lk_fdt);
		else
			run(&items[def], lk_fdt);
	}

	key_init();
	/* the first read may only clean up after LK (channel left in WFVLDCLR) */
	for (i = 0, prev_pwr = -1; i < 5 && prev_pwr < 0; i++)
		prev_pwr = key_power();
	power_key_ok = prev_pwr >= 0;
	if (!power_key_ok)
		log_puts("power key not readable over SPMI, Volume Down selects\n");
	disp_init();
	layout();
	draw_menu(sel);
	if (n_items <= 3)
		draw_status("Nothing to boot: no extlinux.conf", COL_ERR);
	disp_flush();
	if (counting)
		deadline = timer_us() + timeout_us;

	/* keys held from LK (Vol Up + Power for its menu) count only after release */
	prev_up = key_vol_up();
	prev_dn = key_vol_down();
	for (;;) {
		int up = key_vol_up(), dn = key_vol_down();
		int pwr = key_power();
		int move = 0, start = 0;

		wdt_kick();
		if (pwr >= 0 && !power_key_ok) {
			/* the PMIC answers after all: switch to Power = start */
			power_key_ok = 1;
			prev_pwr = pwr;
			draw_menu(sel);
			disp_flush();
			log_puts("power key readable now\n");
		}
		if (up && !prev_up)
			move = power_key_ok ? -1 : 1;
		if (dn && !prev_dn) {
			if (power_key_ok)
				move = 1;
			else
				start = 1;
		}
		if (power_key_ok && pwr > 0 && prev_pwr == 0)
			start = 1;
		if (move || start) {
			if (counting) {
				counting = 0;
				draw_status(NULL, COL_FG);
			}
			if (start && items[sel].act == ACT_UFS) {
				log_hex("selected ", sel);
				sel = linux_partition(sel, 0, lk_fdt);
			} else if (start) {
				log_hex("selected ", sel);
				run(&items[sel], lk_fdt);
				/* only back here on an error, which is on the screen */
			} else {
				draw_item(sel, 0);
				sel = (sel + move + n_items) % n_items;
				draw_item(sel, 1);
			}
			disp_flush();
		}
		prev_up = up;
		prev_dn = dn;
		if (pwr >= 0)
			prev_pwr = pwr;

		if (counting) {
			u64 now = timer_us();

			if (now >= deadline) {
				counting = 0;
				if (items[def].act == ACT_UFS)
					sel = linux_partition(def, 1, lk_fdt);
				else
					run(&items[def], lk_fdt);
				disp_flush();
			} else if ((deadline - now) / 100000 != last_s) {
				/* bar every 0.1 s, text when the second changes */
				u64 left = deadline - now;

				if ((left + 999999) / 1000000 != (left + 100000 + 999999) / 1000000 ||
				    last_s == ~0ul) {
					countdown_text(buf, items[def].label, left);
					draw_status(buf, COL_FG);
				}
				last_s = left / 100000;
				draw_bar(left, timeout_us);
				disp_flush();
			}
		}
		udelay(POLL_US);
	}
}
