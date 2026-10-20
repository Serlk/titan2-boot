# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
# titan2-boot: minimal aarch64 bare-metal second stage, packaged as an arm64
# Image (gzip) so the stock Titan 2 LK loads it like a kernel.
#
# Build (nothing here is flashed):
#   make TOOLCHAIN=/path/to/clang-r574158   (AOSP prebuilt clang 21, see README)
# Output: out/titan2-boot.bin (raw Image) and out/titan2-boot.bin.gz.

TOOLCHAIN ?= ../toolchains/clang-r574158
CC        := $(TOOLCHAIN)/bin/clang
LD        := $(TOOLCHAIN)/bin/ld.lld
OBJCOPY   := $(TOOLCHAIN)/bin/llvm-objcopy

TARGET := aarch64-linux-gnu
CFLAGS := --target=$(TARGET) -ffreestanding -fno-stack-protector -fno-pic \
          -mgeneral-regs-only -march=armv8-a -O2 -Wall -Wextra \
          -fno-builtin -nostdlib -Isrc
ASFLAGS := --target=$(TARGET) -c
LDFLAGS := -nostdlib -static --no-dynamic-linker -z noexecstack

OUT := out
OBJS := $(OUT)/head.o $(OUT)/io.o $(OUT)/main.o

all: $(OUT)/titan2-boot.bin.gz

$(OUT):
	mkdir -p $(OUT)

$(OUT)/head.o: src/head.S | $(OUT)
	$(CC) $(ASFLAGS) $< -o $@

$(OUT)/%.o: src/%.c src/io.h | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/titan2-boot.elf: $(OBJS) src/titan2-boot.lds
	$(LD) $(LDFLAGS) -T src/titan2-boot.lds $(OBJS) -o $@

$(OUT)/titan2-boot.bin: $(OUT)/titan2-boot.elf
	$(OBJCOPY) -O binary $< $@

$(OUT)/titan2-boot.bin.gz: $(OUT)/titan2-boot.bin
	gzip -9 -n -c $< > $@
	@python3 tools/check-image.py $(OUT)/titan2-boot.bin

clean:
	rm -rf $(OUT)

.PHONY: all clean
