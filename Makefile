# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
# titan2-boot: aarch64 bare-metal boot menu after the stock LK, packaged as an
# arm64 Image so the stock Titan 2 LK loads it like a kernel.
#
# Build (nothing here is flashed):
#   make TOOLCHAIN=/path/to/clang-r574158   (AOSP prebuilt clang 21, see README)
#   make test LK_FDT=<LK's FDT> DTB=<mainline DTB> [DECOMP_IMAGE=<Image>]
#                                           host tests: FDT merge, gzip/lz4, memory map,
#                                           extlinux.conf, GPT + ext4, UFS lane trim
#                                           (host cc, ASan/UBSan)
#   make qemu                               out-qemu/: PL011 log for tests/qemu-test.sh
# Output: out/titan2-boot.bin (raw Image, no payloads). tools/mkpack.py adds
# the kernels and makes the boot.img.

TOOLCHAIN ?= ../toolchains/clang-r574158
CC        := $(TOOLCHAIN)/bin/clang
LD        := $(TOOLCHAIN)/bin/ld.lld
OBJCOPY   := $(TOOLCHAIN)/bin/llvm-objcopy
HOSTCC    ?= cc

TARGET := aarch64-linux-gnu
# MMU off: all data accesses are Device memory, unaligned ones fault
CFLAGS := --target=$(TARGET) -ffreestanding -fno-stack-protector -fno-pic \
          -mgeneral-regs-only -mstrict-align -march=armv8-a -O2 -Wall -Wextra \
          -fno-builtin -nostdlib -nostdlibinc -Isrc/include -Isrc -Ilibfdt $(EXTRA_CFLAGS)
ASFLAGS := --target=$(TARGET) -c
LDFLAGS := -nostdlib -static --no-dynamic-linker -z noexecstack

LIBFDT := fdt fdt_ro fdt_rw fdt_wip fdt_strerror fdt_addresses
OUT ?= out
OBJS := $(OUT)/head.o $(OUT)/cache.o $(OUT)/io.o $(OUT)/libc.o $(OUT)/display.o \
        $(OUT)/fdtfix.o $(OUT)/memmap.o $(OUT)/mmu.o $(OUT)/inflate.o $(OUT)/lz4.o \
        $(OUT)/conf.o $(OUT)/blk.o $(OUT)/ext4.o $(OUT)/packfs.o $(OUT)/bootimg.o \
        $(OUT)/ufs.o $(OUT)/boot.o $(OUT)/main.o $(LIBFDT:%=$(OUT)/libfdt/%.o)
HDRS := $(wildcard src/*.h src/include/*.h libfdt/*.h)

all: $(OUT)/titan2-boot.bin

$(OUT) $(OUT)/libfdt $(OUT)/host:
	mkdir -p $@

$(OUT)/%.o: src/%.S | $(OUT)
	$(CC) $(ASFLAGS) $< -o $@

$(OUT)/%.o: src/%.c $(HDRS) | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

# libfdt is upstream code: keep its warnings out of ours
$(OUT)/libfdt/%.o: libfdt/%.c $(HDRS) | $(OUT)/libfdt
	$(CC) $(CFLAGS) -Wno-sign-compare -Wno-unused-parameter -c $< -o $@

$(OUT)/titan2-boot.elf: $(OBJS) src/titan2-boot.lds
	$(LD) $(LDFLAGS) $(EXTRA_LDFLAGS) -T src/titan2-boot.lds $(OBJS) -o $@

$(OUT)/titan2-boot.bin: $(OUT)/titan2-boot.elf
	$(OBJCOPY) -O binary $< $@
	@python3 tools/check-image.py $@

# Host test: the FDT merge on a real LK FDT and a mainline DTB.
#   make test LK_FDT=<LK's FDT, e.g. /sys/firmware/fdt from Android> DTB=<mainline dtb>
$(OUT)/host/fdtfix-test: tests/fdtfix-test.c src/fdtfix.c $(LIBFDT:%=libfdt/%.c) $(HDRS) | $(OUT)/host
	$(HOSTCC) -O1 -g -fsanitize=address,undefined -Isrc -Ilibfdt -o $@ \
		tests/fdtfix-test.c src/fdtfix.c $(LIBFDT:%=libfdt/%.c)

$(OUT)/host/decompress-test: tests/decompress-test.c src/inflate.c src/lz4.c $(HDRS) | $(OUT)/host
	$(HOSTCC) -O2 -g -fsanitize=address,undefined -Isrc -o $@ tests/decompress-test.c src/inflate.c src/lz4.c

$(OUT)/host/memmap-test: tests/memmap-test.c src/memmap.c $(LIBFDT:%=libfdt/%.c) $(HDRS) | $(OUT)/host
	$(HOSTCC) -O1 -g -fsanitize=address,undefined -Isrc -Ilibfdt -o $@ tests/memmap-test.c src/memmap.c $(LIBFDT:%=libfdt/%.c)

$(OUT)/host/conf-test: tests/conf-test.c src/conf.c $(HDRS) | $(OUT)/host
	$(HOSTCC) -O1 -g -fsanitize=address,undefined -Isrc -o $@ tests/conf-test.c src/conf.c

$(OUT)/host/storage-test: tests/storage-test.c src/blk.c src/ext4.c $(HDRS) | $(OUT)/host
	$(HOSTCC) -O2 -g -fsanitize=address,undefined -Isrc -o $@ tests/storage-test.c src/blk.c src/ext4.c

# UFS M-PHY lane trim arithmetic (src/ufs.c built with only its pure functions)
$(OUT)/host/ufs-test: tests/ufs-test.c src/ufs.c $(HDRS) | $(OUT)/host
	$(HOSTCC) -O1 -g -fsanitize=address,undefined -DUFS_HOST_TEST -Isrc -o $@ tests/ufs-test.c src/ufs.c

test: $(OUT)/host/fdtfix-test $(OUT)/host/decompress-test $(OUT)/host/memmap-test \
      $(OUT)/host/conf-test $(OUT)/host/storage-test $(OUT)/host/ufs-test
	$< $(LK_FDT) $(DTB) $(OUT)/host/merged.dtb
	$(OUT)/host/memmap-test $(LK_FDT)
	$(OUT)/host/conf-test
	$(OUT)/host/ufs-test
	tests/storage.sh $(OUT)/host
	tests/decompress.sh $(OUT)/host

qemu:
	$(MAKE) OUT=out-qemu EXTRA_CFLAGS="-DT2_QEMU -DT2_LOAD_ADDR=0x40200000UL" \
		EXTRA_LDFLAGS="--defsym=T2_LOAD_ADDR=0x40200000" all

clean:
	rm -rf out out-qemu

.PHONY: all clean test qemu
