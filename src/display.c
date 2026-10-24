// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
/*
 * Framebuffer drawing and the command-mode frame trigger.
 *
 * The panel runs in DSI command mode: it keeps the picture in its own frame
 * memory, and new framebuffer contents only reach it when the pipeline LK set
 * up is started for one frame. This is the same sequence as the mainline
 * driver drivers/gpu/drm/sysfb/mtk_cmdfb_kick.c (verified there on the
 * hardware); the register layout and the frame start are those of mainline
 * mediatek-drm (mtk_dsi.c, mtk-mutex.c, mtk_disp_ovl.c) and of the MediaTek
 * downstream MT6878 DSI driver (mtk_dsi_config_trigger): check that the
 * pipeline is in the state LK leaves, keep only
 * the OVL layer that scans out the framebuffer, then per frame wait for idle,
 * enable mutex 0 and pulse DSI START. Timings, PLLs and supplies are never
 * touched, and disp_release() puts OVL back the way LK left it, so the next
 * kernel takes over exactly LK's state.
 */
#include "display.h"
#include "font.h"

#include <string.h>

/* DSI */
#define DSI_START		0x000
#define DSI_INTSTA		0x00c
#define DSI_BUSY		(1u << 31)
#define DSI_MODE_CTRL		0x014
#define DSI_CMDQ_SIZE		0x060
#define DSI_CMDQ0		0xd00
#define DSI_CMDQ_WMS		0x002c3909	/* DCS long write of write_memory_start */
/* mutex */
#define MUTEX_EN0		0x020
#define MUTEX_MOD0		0x030
/* OVL0, 2 layers */
#define OVL_SRC_CON		0x02c
#define OVL_ADDR(n)		(0xf40 + 0x20 * (n))
#define OVL_LAYERS		2

#define FRAME_TIMEOUT_US	100000

static int disp_ok;
static u32 ovl_src_con_lk;

void fb_fill(int x, int y, int w, int h, u32 argb)
{
	int i, j;

	if (x < 0 || y < 0 || x + w > FB_W || y + h > FB_H)
		return;
	for (j = 0; j < h; j++) {
		volatile u32 *p = (volatile u32 *)(FB_BASE + (u64)(y + j) * FB_STRIDE) + x;

		for (i = 0; i < w; i++)
			p[i] = argb;
	}
}

int fb_text_width(int s, const char *str)
{
	return (int)strlen(str) * FONT_W * s;
}

int fb_text(int x, int y, int s, u32 fg, u32 bg, const char *str)
{
	int gx, gy, sx, sy;

	for (; *str; str++, x += FONT_W * s) {
		unsigned char c = (unsigned char)*str;
		const unsigned char *g;

		if (x + FONT_W * s > FB_W || y + FONT_H * s > FB_H)
			break;
		if (c < FONT_FIRST || c > FONT_LAST)
			c = '?';
		g = font_data[c - FONT_FIRST];
		for (gy = 0; gy < FONT_H; gy++) {
			u32 bits = (u32)g[gy * 2] << 8 | g[gy * 2 + 1];

			for (sy = 0; sy < s; sy++) {
				volatile u32 *p = (volatile u32 *)(FB_BASE +
					(u64)(y + gy * s + sy) * FB_STRIDE) + x;

				for (gx = 0; gx < FONT_W; gx++) {
					u32 col = (bits >> (15 - gx)) & 1 ? fg : bg;

					for (sx = 0; sx < s; sx++)
						*p++ = col;
				}
			}
		}
	}
	return x;
}

static int disp_idle(void)
{
	return !(mmio_read32(DISP_DSI_BASE + DSI_INTSTA) & DSI_BUSY) &&
	       !(mmio_read32(DISP_MUTEX_BASE + MUTEX_EN0) & 1);
}

static int disp_wait_idle(void)
{
	u64 end = timer_us() + FRAME_TIMEOUT_US;

	while (!disp_idle())
		if (timer_us() > end)
			return -1;
	return 0;
}

int disp_init(void)
{
	u32 mode = mmio_read32(DISP_DSI_BASE + DSI_MODE_CTRL);
	u32 cmdq_size = mmio_read32(DISP_DSI_BASE + DSI_CMDQ_SIZE);
	u32 cmdq0 = mmio_read32(DISP_DSI_BASE + DSI_CMDQ0);
	u32 mutex_en = mmio_read32(DISP_MUTEX_BASE + MUTEX_EN0);
	u32 mutex_mod = mmio_read32(DISP_MUTEX_BASE + MUTEX_MOD0);
	u32 src_con = mmio_read32(DISP_OVL_BASE + OVL_SRC_CON);
	int i, layer = -1;

	log_hex("DSI MODE_CTRL ", mode);
	log_hex("DSI CMDQ_SIZE ", cmdq_size);
	log_hex("DSI CMDQ0     ", cmdq0);
	log_hex("MUTEX0 EN     ", mutex_en);
	log_hex("MUTEX0 MOD    ", mutex_mod);
	log_hex("OVL SRC_CON   ", src_con);

	if (mode != 0 || !(cmdq_size & 0xff) || cmdq0 != DSI_CMDQ_WMS || !mutex_mod ||
	    (mutex_en & 1) || (src_con & 0xf & ~((1u << OVL_LAYERS) - 1))) {
		log_puts("display not in LK's command-mode state, no frame trigger\n");
		return -1;
	}
	for (i = 0; i < OVL_LAYERS; i++)
		if ((src_con & (1u << i)) && mmio_read32(DISP_OVL_BASE + OVL_ADDR(i)) == (u32)FB_BASE) {
			layer = i;
			break;
		}
	if (layer < 0) {
		log_puts("no OVL layer scans out the framebuffer, no frame trigger\n");
		return -1;
	}

	/* only the framebuffer layer: hides LK overlays such as the orange-state text */
	ovl_src_con_lk = src_con;
	mmio_write32(DISP_OVL_BASE + OVL_SRC_CON,
		     src_con & ~(((1u << OVL_LAYERS) - 1) & ~(1u << layer)));
	disp_ok = 1;
	log_hex("frame trigger on OVL layer ", layer);
	return 0;
}

void disp_flush(void)
{
	if (!disp_ok)
		return;
	if (disp_wait_idle()) {
		log_puts("previous frame not finished, frame trigger off\n");
		disp_ok = 0;
		return;
	}
	asm volatile("dsb sy" ::: "memory");
	mmio_write32(DISP_MUTEX_BASE + MUTEX_EN0, 1);
	mmio_write32(DISP_DSI_BASE + DSI_START, 0);
	mmio_write32(DISP_DSI_BASE + DSI_START, 1);
}

void disp_release(void)
{
	if (!disp_ok)
		return;
	if (disp_wait_idle())
		log_puts("last frame not finished\n");
	mmio_write32(DISP_OVL_BASE + OVL_SRC_CON, ovl_src_con_lk);
	disp_ok = 0;
}
