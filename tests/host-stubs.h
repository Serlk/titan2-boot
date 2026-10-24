/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com> */
/* UART log functions of src/io.c for host tests: to stdout. */
#include <stdio.h>

#include "io.h"

void uart_putc(char c) { putchar(c); }
void uart_puts(const char *s) { fputs(s, stdout); }
void uart_hex(u64 v) { printf("0x%lx", v); }
void log_puts(const char *s) { printf("[titan2-boot] %s", s); }
void log_hex(const char *s, u64 v) { printf("[titan2-boot] %s0x%lx\n", s, v); }
