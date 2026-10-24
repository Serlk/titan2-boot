// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
/*
 * LZ4 decoder for the "legacy" frame (magic 0x184c2102), the format of
 * Android kernels (Image.lz4, `lz4 -l`): a sequence of compressed blocks,
 * each preceded by its 32-bit little-endian size, blocks up to 8 MiB of
 * output. Block format as in lz4/doc/lz4_Block_format.md.
 */
#include <string.h>

#include "decompress.h"

#define LZ4_LEGACY_MAGIC	0x184c2102
#define LZ4_LEGACY_BLOCK	(8u << 20)

static u32 le32(const u8 *p)
{
	return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

/* one block: returns bytes written or < 0 */
static long lz4_block(const u8 *in, u64 inlen, u8 *out, u64 outlen)
{
	const u8 *ip = in, *iend = in + inlen;
	u8 *op = out, *oend = out + outlen;

	while (ip < iend) {
		u32 token = *ip++;
		u64 lit = token >> 4, mlen;
		u32 off;
		const u8 *match;

		if (lit == 15) {
			u8 b;

			do {
				if (ip >= iend)
					return -1;
				b = *ip++;
				lit += b;
			} while (b == 255);
		}
		if (lit > (u64)(iend - ip) || lit > (u64)(oend - op))
			return -2;
		memcpy(op, ip, lit);
		op += lit;
		ip += lit;
		if (ip >= iend)
			break;		/* the last sequence has literals only */

		if (iend - ip < 2)
			return -3;
		off = ip[0] | (u32)ip[1] << 8;
		ip += 2;
		if (!off || off > (u64)(op - out))
			return -4;
		mlen = (token & 15) + 4;
		if ((token & 15) == 15) {
			u8 b;

			do {
				if (ip >= iend)
					return -5;
				b = *ip++;
				mlen += b;
			} while (b == 255);
		}
		if (mlen > (u64)(oend - op))
			return -6;
		match = op - off;
		while (mlen--)		/* overlapping copy, byte by byte */
			*op++ = *match++;
	}
	return op - out;
}

long lz4_legacy(const void *src, u64 srclen, void *dst, u64 dstlen)
{
	const u8 *in = src;
	u8 *out = dst;
	u64 pos = 4, done = 0;

	if (srclen < 4 || le32(in) != LZ4_LEGACY_MAGIC)
		return -20;
	while (pos + 4 <= srclen) {
		u32 bsize = le32(in + pos);
		long n;

		/* the next frame (or padding) starts with a magic, not a block */
		if (bsize == LZ4_LEGACY_MAGIC)
			break;
		pos += 4;
		if (bsize > srclen - pos)
			return -21;
		n = lz4_block(in + pos, bsize, out + done, dstlen - done);
		if (n < 0)
			return n;
		if ((u64)n > LZ4_LEGACY_BLOCK)
			return -22;
		done += n;
		pos += bsize;
	}
	return (long)done;
}

long decompress_any(const void *src, u64 srclen, void *dst, u64 dstlen)
{
	const u8 *in = src;

	if (srclen >= 4 && le32(in) == LZ4_LEGACY_MAGIC)
		return lz4_legacy(src, srclen, dst, dstlen);
	if (srclen >= 3 && in[0] == 0x1f && in[1] == 0x8b && in[2] == 8)
		return gunzip(src, srclen, dst, dstlen);
	if (srclen > dstlen)
		return -30;
	memmove(dst, src, srclen);
	return (long)srclen;
}
