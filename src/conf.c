// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
/*
 * extlinux.conf parser, the subset U-Boot's boot/pxe_utils.c understands for
 * the usual distro files:
 *
 *   MENU TITLE <text>      DEFAULT <label>      TIMEOUT <tenths of a second>
 *   LABEL <label>
 *     MENU LABEL <text>    MENU DEFAULT
 *     LINUX|KERNEL <path>  INITRD <path>        FDT|DEVICETREE <path>
 *     APPEND <command line>
 *   # comments, blank lines; keywords are case-insensitive
 *
 * plus two of ours: ANDROID <path> | ANDROID part:<partition>, an Android
 * kernel (raw, gzip or lz4 Image, or a boot.img partition) booted with LK's
 * device tree and ramdisk; and UFS on|off (global), whether titan2-boot may
 * bring up the UFS to read partitions. Unknown keywords (PROMPT, FDTDIR, ...)
 * are ignored.
 */
#include <string.h>

#include "conf.h"

static int lower(int c)
{
	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* keyword match at the start of s, followed by space or the end; returns the rest */
static const char *kw(const char *s, const char *end, const char *word)
{
	while (*word) {
		if (s >= end || lower(*s) != *word)
			return NULL;
		s++;
		word++;
	}
	if (s < end && *s != ' ' && *s != '\t')
		return NULL;
	while (s < end && (*s == ' ' || *s == '\t'))
		s++;
	return s;
}

static void copy_arg(char *dst, u64 size, const char *s, const char *end)
{
	u64 n = 0;

	while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
		end--;
	while (s < end && n + 1 < size)
		dst[n++] = *s++;
	dst[n] = 0;
}

void conf_init(struct boot_conf *c)
{
	memset(c, 0, sizeof(*c));
	c->timeout_ds = -1;
	c->ufs = -1;
}

int conf_parse(struct boot_conf *c, const char *text, u64 len, struct fs *fs)
{
	const char *p = text, *end = text + len;
	struct boot_entry *e = NULL;
	int added = 0, have_title = c->title[0] != 0, have_def = c->def[0] != 0;
	int have_timeout = c->timeout_ds >= 0;

	while (p < end) {
		const char *eol = p, *a;

		while (eol < end && *eol != '\n')
			eol++;
		while (p < eol && (*p == ' ' || *p == '\t'))
			p++;
		if (p == eol || *p == '#')
			goto next;

		if ((a = kw(p, eol, "label"))) {
			if (c->n >= CONF_MAX_ENTRIES) {
				e = NULL;
				goto next;
			}
			e = &c->entry[c->n++];
			memset(e, 0, sizeof(*e));
			e->kind = ENTRY_LINUX;
			e->fs = fs;
			copy_arg(e->label, sizeof(e->label), a, eol);
			added++;
		} else if ((a = kw(p, eol, "menu"))) {
			const char *b;

			if ((b = kw(a, eol, "title"))) {
				if (!have_title)
					copy_arg(c->title, sizeof(c->title), b, eol);
			} else if ((b = kw(a, eol, "label"))) {
				if (e)
					copy_arg(e->title, sizeof(e->title), b, eol);
			} else if (kw(a, eol, "default")) {
				if (e && !have_def)
					copy_arg(c->def, sizeof(c->def), e->label, e->label + strlen(e->label));
			}
		} else if ((a = kw(p, eol, "default"))) {
			if (!have_def)
				copy_arg(c->def, sizeof(c->def), a, eol);
		} else if ((a = kw(p, eol, "timeout"))) {
			int v = 0;

			while (a < eol && *a >= '0' && *a <= '9')
				v = v * 10 + (*a++ - '0');
			if (!have_timeout)
				c->timeout_ds = v;
		} else if ((a = kw(p, eol, "ufs"))) {
			if (c->ufs < 0)
				c->ufs = (a < eol && (*a == '1' || lower(a[1]) == 'n')) ? 1 : 0;
		} else if (!e) {
			goto next;
		} else if ((a = kw(p, eol, "linux")) || (a = kw(p, eol, "kernel"))) {
			copy_arg(e->kernel, sizeof(e->kernel), a, eol);
		} else if ((a = kw(p, eol, "android"))) {
			e->kind = ENTRY_ANDROID;
			copy_arg(e->kernel, sizeof(e->kernel), a, eol);
		} else if ((a = kw(p, eol, "initrd"))) {
			copy_arg(e->initrd, sizeof(e->initrd), a, eol);
		} else if ((a = kw(p, eol, "fdt")) || (a = kw(p, eol, "devicetree"))) {
			copy_arg(e->fdt, sizeof(e->fdt), a, eol);
		} else if ((a = kw(p, eol, "append"))) {
			copy_arg(e->append, sizeof(e->append), a, eol);
		}
next:
		p = eol + 1;
	}

	/* entries without a kernel are useless; a missing MENU LABEL shows the label */
	{
		int i, j = c->n - added;

		for (i = c->n - added; i < c->n; i++) {
			struct boot_entry *x = &c->entry[i];

			if (!x->kernel[0])
				continue;
			if (!x->title[0])
				memcpy(x->title, x->label, sizeof(x->title));
			if (j != i)
				c->entry[j] = *x;
			j++;
		}
		added -= c->n - j;
		c->n = j;
	}
	return added;
}

int conf_default_index(const struct boot_conf *c)
{
	int i;

	for (i = 0; i < c->n; i++)
		if (c->def[0] && !strcmp(c->entry[i].label, c->def))
			return i;
	return c->n ? 0 : -1;
}
