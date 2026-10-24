// SPDX-License-Identifier: Zlib
/*
 * gzip / raw DEFLATE decoder (RFC 1951, RFC 1952).
 *
 * Altered version of puff.c from zlib (contrib/puff), Copyright (C) 2002-2013
 * Mark Adler, zlib license (LICENSES/Zlib.txt): the canonical Huffman decoder
 * and block handling follow puff; changed for titan2-boot by serlk (2026):
 * reading from and writing to flat buffers only, a gzip member wrapper with
 * CRC32 and ISIZE checks, kernel style. This is not the original software.
 */
#include <string.h>

#include "decompress.h"

#define MAXBITS		15
#define MAXLCODES	286
#define MAXDCODES	30
#define FIXLCODES	288

struct inf {
	const u8 *in;
	u64 inlen, incnt;
	u8 *out;
	u64 outlen, outcnt;
	u32 bitbuf;
	int bitcnt;
	int err;
};

struct huffman {
	short count[MAXBITS + 1];
	short symbol[FIXLCODES];
};

static int bits(struct inf *s, int need)
{
	u32 val = s->bitbuf;

	while (s->bitcnt < need) {
		if (s->incnt >= s->inlen) {
			s->err = -1;
			return 0;
		}
		val |= (u32)s->in[s->incnt++] << s->bitcnt;
		s->bitcnt += 8;
	}
	s->bitbuf = val >> need;
	s->bitcnt -= need;
	return (int)(val & ((1u << need) - 1));
}

static int stored(struct inf *s)
{
	u32 len;

	s->bitbuf = 0;
	s->bitcnt = 0;
	if (s->incnt + 4 > s->inlen)
		return -1;
	len = s->in[s->incnt] | (u32)s->in[s->incnt + 1] << 8;
	if ((s->in[s->incnt + 2] | (u32)s->in[s->incnt + 3] << 8) != (~len & 0xffff))
		return -2;
	s->incnt += 4;
	if (s->incnt + len > s->inlen)
		return -1;
	if (s->outcnt + len > s->outlen)
		return -3;
	memcpy(s->out + s->outcnt, s->in + s->incnt, len);
	s->incnt += len;
	s->outcnt += len;
	return 0;
}

static int decode(struct inf *s, const struct huffman *h)
{
	int code = 0, first = 0, index = 0, len;

	for (len = 1; len <= MAXBITS; len++) {
		int count;

		code |= bits(s, 1);
		if (s->err)
			return -1;
		count = h->count[len];
		if (code - count < first)
			return h->symbol[index + (code - first)];
		index += count;
		first += count;
		first <<= 1;
		code <<= 1;
	}
	return -10;
}

static int construct(struct huffman *h, const short *length, int n)
{
	short offs[MAXBITS + 1];
	int symbol, len, left;

	for (len = 0; len <= MAXBITS; len++)
		h->count[len] = 0;
	for (symbol = 0; symbol < n; symbol++)
		h->count[length[symbol]]++;
	if (h->count[0] == n)
		return 0;
	left = 1;
	for (len = 1; len <= MAXBITS; len++) {
		left <<= 1;
		left -= h->count[len];
		if (left < 0)
			return left;
	}
	offs[1] = 0;
	for (len = 1; len < MAXBITS; len++)
		offs[len + 1] = offs[len] + h->count[len];
	for (symbol = 0; symbol < n; symbol++)
		if (length[symbol] != 0)
			h->symbol[offs[length[symbol]]++] = symbol;
	return left;
}

static int codes(struct inf *s, const struct huffman *lencode, const struct huffman *distcode)
{
	static const short lbase[29] = {
		3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
		35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
	static const short lext[29] = {
		0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
		3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
	static const short dbase[30] = {
		1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
		257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
		8193, 12289, 16385, 24577 };
	static const short dext[30] = {
		0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
		7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
	int symbol, len;
	u32 dist;

	do {
		symbol = decode(s, lencode);
		if (symbol < 0)
			return symbol;
		if (symbol < 256) {
			if (s->outcnt >= s->outlen)
				return -3;
			s->out[s->outcnt++] = (u8)symbol;
		} else if (symbol > 256) {
			symbol -= 257;
			if (symbol >= 29)
				return -10;
			len = lbase[symbol] + bits(s, lext[symbol]);
			symbol = decode(s, distcode);
			if (symbol < 0)
				return symbol;
			if (symbol >= 30)
				return -11;
			dist = dbase[symbol] + bits(s, dext[symbol]);
			if (s->err)
				return -1;
			if (dist > s->outcnt)
				return -11;
			if (s->outcnt + len > s->outlen)
				return -3;
			while (len--) {
				s->out[s->outcnt] = s->out[s->outcnt - dist];
				s->outcnt++;
			}
		}
	} while (symbol != 256);
	return 0;
}

static int fixed(struct inf *s)
{
	static struct huffman lencode, distcode;
	static int virgin = 1;

	if (virgin) {
		short lengths[FIXLCODES];
		int symbol;

		for (symbol = 0; symbol < 144; symbol++)
			lengths[symbol] = 8;
		for (; symbol < 256; symbol++)
			lengths[symbol] = 9;
		for (; symbol < 280; symbol++)
			lengths[symbol] = 7;
		for (; symbol < FIXLCODES; symbol++)
			lengths[symbol] = 8;
		construct(&lencode, lengths, FIXLCODES);
		for (symbol = 0; symbol < MAXDCODES; symbol++)
			lengths[symbol] = 5;
		construct(&distcode, lengths, MAXDCODES);
		virgin = 0;
	}
	return codes(s, &lencode, &distcode);
}

static int dynamic(struct inf *s)
{
	static const short order[19] = {
		16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
	struct huffman lencode, distcode;
	short lengths[MAXLCODES + MAXDCODES];
	int nlen, ndist, ncode, index, err;

	nlen = bits(s, 5) + 257;
	ndist = bits(s, 5) + 1;
	ncode = bits(s, 4) + 4;
	if (s->err)
		return -1;
	if (nlen > MAXLCODES || ndist > MAXDCODES)
		return -3;
	for (index = 0; index < ncode; index++)
		lengths[order[index]] = bits(s, 3);
	for (; index < 19; index++)
		lengths[order[index]] = 0;
	if (s->err)
		return -1;
	if (construct(&lencode, lengths, 19) != 0)
		return -4;

	index = 0;
	while (index < nlen + ndist) {
		int symbol = decode(s, &lencode);
		int len;

		if (symbol < 0)
			return symbol;
		if (symbol < 16) {
			lengths[index++] = symbol;
			continue;
		}
		len = 0;
		if (symbol == 16) {
			if (index == 0)
				return -5;
			len = lengths[index - 1];
			symbol = 3 + bits(s, 2);
		} else if (symbol == 17) {
			symbol = 3 + bits(s, 3);
		} else {
			symbol = 11 + bits(s, 7);
		}
		if (s->err)
			return -1;
		if (index + symbol > nlen + ndist)
			return -6;
		while (symbol--)
			lengths[index++] = len;
	}
	if (lengths[256] == 0)
		return -9;
	err = construct(&lencode, lengths, nlen);
	if (err && (err < 0 || nlen != lencode.count[0] + lencode.count[1]))
		return -7;
	err = construct(&distcode, lengths + nlen, ndist);
	if (err && (err < 0 || ndist != distcode.count[0] + distcode.count[1]))
		return -8;
	return codes(s, &lencode, &distcode);
}

static int inflate_raw(struct inf *s)
{
	int last, type, err;

	do {
		last = bits(s, 1);
		type = bits(s, 2);
		if (s->err)
			return -1;
		if (type == 0)
			err = stored(s);
		else if (type == 1)
			err = fixed(s);
		else if (type == 2)
			err = dynamic(s);
		else
			err = -2;
		if (err)
			return err;
	} while (!last);
	return 0;
}

static u32 crc32_update(u32 crc, const u8 *p, u64 n)
{
	static u32 table[256];
	static int ready;
	u64 i;

	if (!ready) {
		u32 c;
		int k, b;

		for (k = 0; k < 256; k++) {
			c = k;
			for (b = 0; b < 8; b++)
				c = c & 1 ? 0xedb88320 ^ (c >> 1) : c >> 1;
			table[k] = c;
		}
		ready = 1;
	}
	crc = ~crc;
	for (i = 0; i < n; i++)
		crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
	return ~crc;
}

/* gzip member (RFC 1952): header, DEFLATE data, CRC32 and size checked */
long gunzip(const void *src, u64 srclen, void *dst, u64 dstlen)
{
	const u8 *in = src;
	struct inf s;
	u64 pos = 10;
	u8 flg;
	u32 crc, isize;

	if (srclen < 18 || in[0] != 0x1f || in[1] != 0x8b || in[2] != 8)
		return -20;
	flg = in[3];
	if (flg & 4) {			/* FEXTRA */
		pos += 2 + (in[10] | (u32)in[11] << 8);
	}
	if (flg & 8)			/* FNAME */
		while (pos < srclen && in[pos++])
			;
	if (flg & 16)			/* FCOMMENT */
		while (pos < srclen && in[pos++])
			;
	if (flg & 2)			/* FHCRC */
		pos += 2;
	if (pos >= srclen)
		return -21;

	memset(&s, 0, sizeof(s));
	s.in = in + pos;
	s.inlen = srclen - pos;
	s.out = dst;
	s.outlen = dstlen;
	if (inflate_raw(&s))
		return -22;
	pos += s.incnt;
	if (pos + 8 > srclen)
		return -23;
	crc = in[pos] | (u32)in[pos + 1] << 8 | (u32)in[pos + 2] << 16 | (u32)in[pos + 3] << 24;
	isize = in[pos + 4] | (u32)in[pos + 5] << 8 | (u32)in[pos + 6] << 16 | (u32)in[pos + 7] << 24;
	if (isize != (u32)s.outcnt || crc != crc32_update(0, dst, s.outcnt))
		return -24;
	return (long)s.outcnt;
}
