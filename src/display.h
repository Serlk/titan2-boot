/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
#ifndef TITAN2_BOOT_DISPLAY_H
#define TITAN2_BOOT_DISPLAY_H

#include "io.h"

#define FB_BASE		0xfe04f000UL	/* LK framebuffer, ARGB8888 */
#define FB_W		1440
#define FB_H		1440
#define FB_STRIDE	(FB_W * 4)

/* LK's display pipeline (mutex 0 -> OVL0 -> ... -> DSI0), as in mtk_cmdfb_kick.c */
#define DISP_MUTEX_BASE	0x14001000UL
#define DISP_OVL_BASE	0x14002000UL
#define DISP_DSI_BASE	0x1401a000UL

void fb_fill(int x, int y, int w, int h, u32 argb);
/* text with the 16x32 font scaled by s; returns the x after the last glyph */
int fb_text(int x, int y, int s, u32 fg, u32 bg, const char *str);
int fb_text_width(int s, const char *str);
#define TEXT_H(s)	(32 * (s))	/* font height (src/font.h) at scale s */

int disp_init(void);	/* 0 if the pipeline is as LK leaves it and frames can be started */
void disp_flush(void);	/* show the framebuffer on the panel (one command-mode frame) */
void disp_release(void);	/* wait for the last frame, give the pipeline back as LK left it */

#endif
