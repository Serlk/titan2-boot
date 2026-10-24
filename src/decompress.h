/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
#ifndef TITAN2_BOOT_DECOMPRESS_H
#define TITAN2_BOOT_DECOMPRESS_H

#include "io.h"

/* all return the decompressed size, or < 0 on an error */
long gunzip(const void *src, u64 srclen, void *dst, u64 dstlen);
long lz4_legacy(const void *src, u64 srclen, void *dst, u64 dstlen);
/* lz4 legacy, gzip, or a plain copy, by the magic */
long decompress_any(const void *src, u64 srclen, void *dst, u64 dstlen);

#endif
