// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
/*
 * Memory map from LK's FDT. A sorted list of non-overlapping ranges covering
 * DRAM below MEM_LIMIT; marking a range splits the ranges it touches.
 * Reserved always wins over busy, busy over free.
 */
#include <string.h>

#include <libfdt.h>

#include "memmap.h"

static struct mem_range map[MEMMAP_MAX];
static int nmap;

static int rank(enum mem_type t)
{
	switch (t) {
	case MEM_FREE: return 0;
	case MEM_BUSY: return 1;
	case MEM_FB: return 2;
	default: return 3;
	}
}

static void insert_at(int i, const struct mem_range *r)
{
	if (nmap >= MEMMAP_MAX)
		return;
	memmove(&map[i + 1], &map[i], (nmap - i) * sizeof(map[0]));
	map[i] = *r;
	nmap++;
}

/* split so that a range boundary falls on addr */
static void split_at(u64 addr)
{
	int i;

	for (i = 0; i < nmap; i++) {
		if (map[i].start < addr && addr < map[i].end) {
			struct mem_range r = map[i];

			map[i].end = addr;
			r.start = addr;
			insert_at(i + 1, &r);
			return;
		}
	}
}

static void merge(void)
{
	int i = 0;

	while (i + 1 < nmap) {
		if (map[i].end == map[i + 1].start && map[i].type == map[i + 1].type &&
		    map[i].name == map[i + 1].name) {
			map[i].end = map[i + 1].end;
			memmove(&map[i + 1], &map[i + 2], (nmap - i - 2) * sizeof(map[0]));
			nmap--;
		} else {
			i++;
		}
	}
}

void memmap_mark(u64 start, u64 end, enum mem_type type, const char *name)
{
	int i;

	if (end > MEM_LIMIT)
		end = MEM_LIMIT;
	if (start >= end)
		return;
	split_at(start);
	split_at(end);
	for (i = 0; i < nmap; i++) {
		if (map[i].start >= start && map[i].end <= end &&
		    rank(type) >= rank(map[i].type)) {
			map[i].type = type;
			map[i].name = name;
		}
	}
	merge();
}

static u64 cells(const fdt32_t *p, int n)
{
	u64 v = 0;

	while (n--)
		v = v << 32 | fdt32_to_cpu(*p++);
	return v;
}

static int reg_ranges(const void *fdt, int node, int ac, int sc, enum mem_type type,
		      const char *name)
{
	const fdt32_t *reg;
	int len, step = (ac + sc) * 4, i;

	reg = fdt_getprop(fdt, node, "reg", &len);
	if (!reg)
		return 0;
	for (i = 0; i + step <= len; i += step) {
		u64 a = cells(reg + i / 4, ac), s = cells(reg + i / 4 + ac, sc);

		if (type == MEM_FREE) {
			struct mem_range r = { a, a + s > MEM_LIMIT ? MEM_LIMIT : a + s, MEM_FREE, "dram" };

			if (r.start < r.end && nmap == 0)
				insert_at(0, &r);
			else if (r.start < r.end)
				memmap_mark(r.start, r.end, MEM_FREE, "dram");
		} else {
			memmap_mark(a, a + s, type, name);
		}
	}
	return 0;
}

int memmap_init(const void *fdt)
{
	int root_ac, root_sc, mem, rsv, child;

	nmap = 0;
	root_ac = fdt_address_cells(fdt, 0);
	root_sc = fdt_size_cells(fdt, 0);
	if (root_ac < 1 || root_sc < 1)
		return -1;

	mem = fdt_path_offset(fdt, "/memory");
	if (mem < 0)
		return mem;
	/* the first /memory range seeds the list; DRAM is one block below 4 GiB here */
	reg_ranges(fdt, mem, root_ac, root_sc, MEM_FREE, "dram");
	if (!nmap)
		return -2;

	rsv = fdt_path_offset(fdt, "/reserved-memory");
	if (rsv >= 0) {
		int ac = fdt_address_cells(fdt, rsv), sc = fdt_size_cells(fdt, rsv);

		fdt_for_each_subnode(child, fdt, rsv) {
			const char *name = fdt_get_name(fdt, child, NULL);
			enum mem_type t = fdt_node_check_compatible(fdt, child,
						"mediatek,framebuffer") ? MEM_RESERVED : MEM_FB;

			reg_ranges(fdt, child, ac, sc, t, name);
		}
	}
	/* the memory reservation block, for completeness */
	{
		int i, n = fdt_num_mem_rsv(fdt);

		for (i = 0; i < n; i++) {
			u64 a, s;

			if (!fdt_get_mem_rsv(fdt, i, &a, &s))
				memmap_mark(a, a + s, MEM_RESERVED, "memreserve");
		}
	}
	return 0;
}

enum mem_type memmap_type(u64 start, u64 end)
{
	enum mem_type worst = MEM_FREE;
	u64 covered = start;
	int i;

	for (i = 0; i < nmap; i++) {
		if (map[i].end <= start || map[i].start >= end)
			continue;
		if (map[i].start > covered)
			return MEM_RESERVED;	/* a hole: not DRAM */
		if (rank(map[i].type) > rank(worst))
			worst = map[i].type;
		covered = map[i].end;
	}
	return covered >= end ? worst : MEM_RESERVED;
}

u64 memmap_alloc(u64 size, u64 align, u64 min, const char *name)
{
	int i;

	for (i = 0; i < nmap; i++) {
		u64 a;

		if (map[i].type != MEM_FREE || map[i].end <= min)
			continue;
		a = map[i].start > min ? map[i].start : min;
		a = (a + align - 1) & ~(align - 1);
		if (a + size <= map[i].end) {
			memmap_mark(a, a + size, MEM_BUSY, name);
			return a;
		}
	}
	return 0;
}

int memmap_count(void)
{
	return nmap;
}

const struct mem_range *memmap_get(int i)
{
	return i < nmap ? &map[i] : NULL;
}

void memmap_log(void)
{
	static const char *const tn[] = { "free", "busy", "RSVD", "fb" };
	int i;

	for (i = 0; i < nmap; i++) {
		log_puts("mem ");
		uart_hex(map[i].start);
		uart_puts("..");
		uart_hex(map[i].end);
		uart_putc(' ');
		uart_puts(tn[map[i].type]);
		uart_putc(' ');
		uart_puts(map[i].name ? map[i].name : "");
		uart_putc('\n');
	}
}
