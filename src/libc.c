// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
/*
 * String functions for titan2-boot and libfdt. The MMU is off, so every data
 * access is Device memory: unaligned accesses fault (the build uses
 * -mstrict-align) and each access is slow, so the copies move 8 bytes at a
 * time when both pointers allow it.
 */
#include <stdint.h>
#include <string.h>

void *memcpy(void *dst, const void *src, size_t n)
{
	return memmove(dst, src, n);
}

void *memmove(void *dst, const void *src, size_t n)
{
	unsigned char *d = dst;
	const unsigned char *s = src;

	if (d == s || !n)
		return dst;
	if (d < s || d >= s + n) {
		if ((((uintptr_t)d | (uintptr_t)s) & 7) == 0) {
			for (; n >= 8; n -= 8, d += 8, s += 8)
				*(uint64_t *)d = *(const uint64_t *)s;
		}
		while (n--)
			*d++ = *s++;
	} else {
		d += n;
		s += n;
		if ((((uintptr_t)d | (uintptr_t)s) & 7) == 0) {
			for (; n >= 8; n -= 8) {
				d -= 8;
				s -= 8;
				*(uint64_t *)d = *(const uint64_t *)s;
			}
		}
		while (n--)
			*--d = *--s;
	}
	return dst;
}

void *memset(void *dst, int c, size_t n)
{
	unsigned char *d = dst;
	uint64_t v = (unsigned char)c * 0x0101010101010101ull;

	while (n && ((uintptr_t)d & 7)) {
		*d++ = (unsigned char)c;
		n--;
	}
	for (; n >= 8; n -= 8, d += 8)
		*(uint64_t *)d = v;
	while (n--)
		*d++ = (unsigned char)c;
	return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *x = a, *y = b;

	for (; n; n--, x++, y++)
		if (*x != *y)
			return *x - *y;
	return 0;
}

void *memchr(const void *s, int c, size_t n)
{
	const unsigned char *p = s;

	for (; n; n--, p++)
		if (*p == (unsigned char)c)
			return (void *)p;
	return NULL;
}

size_t strlen(const char *s)
{
	const char *p = s;

	while (*p)
		p++;
	return p - s;
}

size_t strnlen(const char *s, size_t max)
{
	size_t n = 0;

	while (n < max && s[n])
		n++;
	return n;
}

int strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
	for (; n; n--, a++, b++) {
		if (*a != *b)
			return (unsigned char)*a - (unsigned char)*b;
		if (!*a)
			break;
	}
	return 0;
}

char *strchr(const char *s, int c)
{
	for (;; s++) {
		if (*s == (char)c)
			return (char *)s;
		if (!*s)
			return NULL;
	}
}

char *strrchr(const char *s, int c)
{
	const char *r = NULL;

	for (;; s++) {
		if (*s == (char)c)
			r = s;
		if (!*s)
			return (char *)r;
	}
}

char *strcpy(char *dst, const char *src)
{
	return memmove(dst, src, strlen(src) + 1);
}

char *strcat(char *dst, const char *src)
{
	strcpy(dst + strlen(dst), src);
	return dst;
}
