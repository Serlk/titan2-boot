// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
/*
 * Mainline FDT from LK's FDT.
 *
 * With titan2-boot in boot_a, vendor_boot_a and dtbo_a stay stock, so LK
 * builds Android's FDT: memory map, firmware loading and the properties it
 * fills in, all as for Android. A mainline kernel gets its own DTB from the
 * pack instead, and this file carries over what LK found out at run time
 * (titan2-docs: docs/lk-boot-requirements.md, "What LK adds to the DTB"):
 *
 *  - /memory reg (all DRAM);
 *  - every /reserved-memory/mblock-* node (BL31, TEE, GZ, modem, SCP, SSPM,
 *    pstore, framebuffer, ...) without its phandle; the framebuffer gets
 *    no-map, so simplefb can map it;
 *  - the LK entries of /chosen (atag,*, kaslr-seed, ram_console, ...);
 *  - the properties LK writes into device nodes the mainline drivers use
 *    (lk_props[] below; phandles into /reserved-memory: lk_refs[]) and the
 *    nodes LK creates for the modem.
 *
 * bootargs are put together the way LK does it for a kernel booted directly:
 * the DTB's own bootargs (the mainline board DT needs clk_ignore_unused
 * pd_ignore_unused), then LK's earlycon/serial console/ramoops options, then
 * the pack's command line. The initrd is the pack's.
 */
#include <string.h>

#include <libfdt.h>

#include "fdtfix.h"

#define BOOTARGS_MAX	2048	/* arm64 COMMAND_LINE_SIZE */

/* /chosen entries LK writes; only these are copied */
static const char *const chosen_props[] = {
	"atag,*", "kaslr-seed", "ram_console", "log_store", "mrdump,*", "aee,*", NULL
};

/* bootargs options taken over from LK, in front of the pack's command line */
static const char *const lk_bootargs[] = {
	"earlycon", "console=ttyS", "ramoops.", NULL
};

struct lk_prop_rule {
	const char *path;	/* or */
	const char *compatible;
	const char *const props[6];
};

/* properties LK writes into device nodes that mainline drivers read */
static const struct lk_prop_rule lk_props[] = {
	{ "/soc/mddriver", NULL,
	  { "ccci,modem_info_v2", "md_dbm_addr", "md_low_power_addr", "md1_ccb_*", NULL } },
	{ NULL, "mediatek,scp-audio-mbox", { "shared_memory", NULL } },
	{ "/soc/gpueb", NULL, { "gpueb_mem_addr", "gpueb_mem_size", NULL } },
	{ NULL, "mediatek,sspm", { "sspm-res-ram-start", NULL } },
	/* CONNSYS EMI that LK carved out and loaded the firmware into (mtk-conninfra) */
	{ NULL, "mediatek,mt6878-6637-consys", { "emi-addr", NULL } },
};

struct lk_ref_rule {
	const char *path;	/* the same path in both FDTs */
	const char *prop;	/* one phandle to a /reserved-memory node */
};

/*
 * Phandle properties LK writes: the target is one of the /reserved-memory
 * nodes copied above (without LK's phandle), so the reference is set to
 * that copy, by node name.
 */
static const struct lk_ref_rule lk_refs[] = {
	/* WFDMA pool (shared-dma-pool) for mt7921-soc */
	{ "/wifi@18400000", "memory-region" },
};

/* root nodes LK creates for the modem */
static const char *const lk_root_nodes[] = { "mdproperties", "md_attr_node", NULL };

/* "name" matches "name", "pre*" matches every name starting with "pre" */
static int name_match(const char *const *list, const char *name)
{
	for (; *list; list++) {
		size_t n = strlen(*list);

		if ((*list)[n - 1] == '*' ? !strncmp(*list, name, n - 1) : !strcmp(*list, name))
			return 1;
	}
	return 0;
}

static void log_err(const char *what, const char *name, int err)
{
	log_puts(what);
	uart_puts(name);
	uart_puts(": ");
	uart_puts(fdt_strerror(err));
	uart_putc('\n');
}

static int find_node(const void *fdt, const char *path, const char *compatible)
{
	return path ? fdt_path_offset(fdt, path) : fdt_node_offset_by_compatible(fdt, -1, compatible);
}

/* copy node snode of src (properties without phandles, and all children) under dparent */
static int copy_node(void *dst, int dparent, const void *src, int snode)
{
	const char *name = fdt_get_name(src, snode, NULL);
	int d, off, child, err;

	d = fdt_subnode_offset(dst, dparent, name);
	if (d == -FDT_ERR_NOTFOUND)
		d = fdt_add_subnode(dst, dparent, name);
	if (d < 0)
		return d;
	fdt_for_each_property_offset(off, src, snode) {
		const char *pname;
		int len;
		const void *val = fdt_getprop_by_offset(src, off, &pname, &len);

		if (!strcmp(pname, "phandle") || !strcmp(pname, "linux,phandle"))
			continue;
		err = fdt_setprop(dst, d, pname, val, len);
		if (err)
			return err;
	}
	fdt_for_each_subnode(child, src, snode) {
		err = copy_node(dst, d, src, child);
		if (err < 0)
			return err;
	}
	return d;
}

static int fix_memory(void *dst, const void *lk)
{
	int s = fdt_path_offset(lk, "/memory");
	int d = fdt_path_offset(dst, "/memory");
	const void *reg;
	int len;

	if (s < 0)
		return s;
	reg = fdt_getprop(lk, s, "reg", &len);
	if (!reg)
		return len;
	if (d == -FDT_ERR_NOTFOUND) {
		d = fdt_add_subnode(dst, 0, "memory");
		if (d >= 0)
			d = fdt_setprop_string(dst, d, "device_type", "memory") ?: d;
	}
	if (d < 0)
		return d;
	return fdt_setprop(dst, d, "reg", reg, len);
}

static int fix_reserved_memory(void *dst, const void *lk)
{
	int s = fdt_path_offset(lk, "/reserved-memory");
	int d = fdt_path_offset(dst, "/reserved-memory");
	int child, err, n = 0;

	if (s < 0)
		return s;
	if (d == -FDT_ERR_NOTFOUND) {
		d = fdt_add_subnode(dst, 0, "reserved-memory");
		if (d < 0)
			return d;
		err = fdt_setprop_u32(dst, d, "#address-cells", 2) ?:
		      fdt_setprop_u32(dst, d, "#size-cells", 2) ?:
		      fdt_setprop_empty(dst, d, "ranges");
		if (err)
			return err;
	}
	if (d < 0)
		return d;

	fdt_for_each_subnode(child, lk, s) {
		const char *name = fdt_get_name(lk, child, NULL);
		int c;

		if (strncmp(name, "mblock-", 7))
			continue;
		c = copy_node(dst, d, lk, child);
		if (c < 0) {
			log_err("reserved-memory ", name, c);
			return c;
		}
		/* LK leaves the framebuffer mapped; simplefb needs it outside the linear map */
		if (!fdt_node_check_compatible(dst, c, "mediatek,framebuffer")) {
			err = fdt_setprop_empty(dst, c, "no-map");
			if (err)
				return err;
		}
		n++;
		/* adding nodes moves later offsets: find our parent again */
		d = fdt_path_offset(dst, "/reserved-memory");
		if (d < 0)
			return d;
	}
	log_hex("mblock nodes copied: ", n);
	return 0;
}

static int fix_chosen(void *dst, const void *lk, const char *cmdline, u64 initrd_start,
		      u64 initrd_end)
{
	static char bootargs[BOOTARGS_MAX];
	int s = fdt_path_offset(lk, "/chosen");
	int d = fdt_path_offset(dst, "/chosen");
	const char *lkargs, *dtargs;
	size_t n = 0, cl = strlen(cmdline);
	int off, err, len;

	if (d == -FDT_ERR_NOTFOUND)
		d = fdt_add_subnode(dst, 0, "chosen");
	if (d < 0)
		return d;

	if (s >= 0) {
		fdt_for_each_property_offset(off, lk, s) {
			const char *pname;
			const void *val = fdt_getprop_by_offset(lk, off, &pname, &len);

			if (!name_match(chosen_props, pname))
				continue;
			err = fdt_setprop(dst, d, pname, val, len);
			if (err)
				return err;
		}
	}

	/* the DTB's own bootargs first, as LK keeps them */
	dtargs = fdt_getprop(dst, d, "bootargs", &len);
	if (dtargs && len > 1) {
		size_t dl = strnlen(dtargs, len);

		if (dl + 1 + cl + 1 > sizeof(bootargs))
			return -FDT_ERR_NOSPACE;
		memcpy(bootargs, dtargs, dl);
		n = dl;
		bootargs[n++] = ' ';
	}

	/* then LK's earlycon/console/ramoops, so the pack's options win */
	lkargs = s >= 0 ? fdt_getprop(lk, s, "bootargs", &len) : NULL;
	while (lkargs && *lkargs) {
		const char *e;
		size_t tl;

		while (*lkargs == ' ')
			lkargs++;
		for (e = lkargs; *e && *e != ' '; e++)
			;
		tl = e - lkargs;
		if (tl) {
			const char *const *p;

			for (p = lk_bootargs; *p; p++)
				if (!strncmp(lkargs, *p, strlen(*p)))
					break;
			if (*p && n + tl + 1 + cl + 1 <= sizeof(bootargs)) {
				memcpy(bootargs + n, lkargs, tl);
				n += tl;
				bootargs[n++] = ' ';
			}
		}
		lkargs = e;
	}
	if (n + cl + 1 > sizeof(bootargs))
		return -FDT_ERR_NOSPACE;
	memcpy(bootargs + n, cmdline, cl + 1);

	err = fdt_setprop_string(dst, d, "bootargs", bootargs);
	if (err)
		return err;
	if (initrd_end > initrd_start) {
		err = fdt_setprop_u64(dst, d, "linux,initrd-start", initrd_start) ?:
		      fdt_setprop_u64(dst, d, "linux,initrd-end", initrd_end);
	} else {
		fdt_delprop(dst, d, "linux,initrd-start");
		fdt_delprop(dst, d, "linux,initrd-end");
	}
	log_puts("bootargs: ");
	uart_puts(bootargs);
	uart_putc('\n');
	return err;
}

static int fix_lk_props(void *dst, const void *lk)
{
	unsigned int i;
	int off, err;

	for (i = 0; i < sizeof(lk_props) / sizeof(lk_props[0]); i++) {
		const struct lk_prop_rule *r = &lk_props[i];
		const char *what = r->path ? r->path : r->compatible;
		int s = find_node(lk, r->path, r->compatible);
		int d = find_node(dst, r->path, r->compatible);

		if (s < 0 || d < 0) {
			log_puts(what);
			uart_puts(s < 0 ? ": not in LK's FDT\n" : ": not in the mainline DTB\n");
			continue;
		}
		fdt_for_each_property_offset(off, lk, s) {
			const char *pname;
			int len;
			const void *val = fdt_getprop_by_offset(lk, off, &pname, &len);

			if (!name_match(r->props, pname))
				continue;
			err = fdt_setprop(dst, d, pname, val, len);
			if (err) {
				log_err(what, pname, err);
				return err;
			}
		}
	}

	for (i = 0; i < sizeof(lk_refs) / sizeof(lk_refs[0]); i++) {
		const struct lk_ref_rule *r = &lk_refs[i];
		const fdt32_t *ref;
		const char *name;
		uint32_t ph;
		int len, s, t, rm, d;

		s = fdt_path_offset(lk, r->path);
		ref = s >= 0 ? fdt_getprop(lk, s, r->prop, &len) : NULL;
		if (!ref || len != 4)
			continue;
		t = fdt_node_offset_by_phandle(lk, fdt32_to_cpu(*ref));
		rm = fdt_path_offset(dst, "/reserved-memory");
		name = t >= 0 ? fdt_get_name(lk, t, NULL) : NULL;
		d = name && rm >= 0 ? fdt_subnode_offset(dst, rm, name) : -FDT_ERR_NOTFOUND;
		if (d < 0) {
			log_puts(r->path);
			uart_puts(": target of ");
			uart_puts(r->prop);
			uart_puts(" not among the copied reserved-memory nodes\n");
			continue;
		}
		ph = fdt_get_phandle(dst, d);
		if (!ph) {
			err = fdt_generate_phandle(dst, &ph);
			if (!err)
				err = fdt_setprop_u32(dst, d, "phandle", ph);
			if (err) {
				log_err(r->path, r->prop, err);
				return err;
			}
		}
		d = fdt_path_offset(dst, r->path);
		if (d < 0) {
			log_puts(r->path);
			uart_puts(": not in the mainline DTB\n");
			continue;
		}
		err = fdt_setprop_u32(dst, d, r->prop, ph);
		if (err) {
			log_err(r->path, r->prop, err);
			return err;
		}
	}

	for (i = 0; lk_root_nodes[i]; i++) {
		int s = fdt_subnode_offset(lk, 0, lk_root_nodes[i]);

		if (s < 0)
			continue;
		err = copy_node(dst, 0, lk, s);
		if (err < 0) {
			log_err("/", lk_root_nodes[i], err);
			return err;
		}
	}
	return 0;
}

int linux_fdt_build(const void *lk_fdt, const void *dtb, void *out, u64 out_size,
		    const char *cmdline, u64 initrd_start, u64 initrd_end)
{
	int err;

	err = fdt_check_header(lk_fdt);
	if (err) {
		log_err("", "LK FDT", err);
		return err;
	}
	err = fdt_open_into(dtb, out, (int)out_size);
	if (err) {
		log_err("", "mainline DTB", err);
		return err;
	}

	err = fix_memory(out, lk_fdt);
	if (err) {
		log_err("", "/memory", err);
		return err;
	}
	err = fix_reserved_memory(out, lk_fdt);
	if (err) {
		log_err("", "/reserved-memory", err);
		return err;
	}
	err = fix_chosen(out, lk_fdt, cmdline, initrd_start, initrd_end);
	if (err) {
		log_err("", "/chosen", err);
		return err;
	}
	err = fix_lk_props(out, lk_fdt);
	if (err)
		return err;

	err = fdt_pack(out);
	if (!err)
		log_hex("mainline FDT size ", fdt_totalsize(out));
	return err;
}

int lk_boot_mode(const void *lk_fdt)
{
	int s = fdt_path_offset(lk_fdt, "/chosen");
	const fdt32_t *tag;
	int len;

	if (s < 0)
		return -1;
	/* struct tag_bootmode { u32 size, tag, bootmode, boottype; } in CPU (little) endian */
	tag = fdt_getprop(lk_fdt, s, "atag,boot", &len);
	if (!tag || len < 12)
		return -1;
	{
		const u8 *b = (const u8 *)tag + 8;

		return (int)((u32)b[0] | (u32)b[1] << 8 | (u32)b[2] << 16 | (u32)b[3] << 24);
	}
}
