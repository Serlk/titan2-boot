#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
"""Offline sanity check for the titan2-boot arm64 Image (no device needed).

Verifies the 64-byte arm64 Image header (Documentation/arm64/booting.rst) and
that the image fits where the stock LK will place it (0x40000000, limit
0x7c80000; see titan2-docs: docs/lk-boot-requirements.md). Run by the Makefile after each build.
"""
import struct
import sys

LOAD = 0x40000000
LIMIT = 0x7C80000  # mb_kernel size


def main(path):
    d = open(path, "rb").read()
    ok = True
    if len(d) < 64:
        print("FAIL: image shorter than header")
        return 1
    (code0, code1, text_off, img_sz, flags, r0, r1, r2) = struct.unpack_from("<IIQQQQQQ", d, 0)
    magic = d[56:60]
    print("code0        0x%08x (branch)" % code0)
    print("text_offset  0x%x" % text_off)
    print("image_size   0x%x" % img_sz)
    print("flags        0x%x" % flags)
    print("magic        %r" % magic)
    print("file size    0x%x (%d bytes)" % (len(d), len(d)))
    if magic != b"ARM\x64":
        print("FAIL: bad arm64 magic")
        ok = False
    if (code0 & 0xFC000000) != 0x14000000:
        print("FAIL: code0 is not an unconditional branch")
        ok = False
    if img_sz == 0:
        print("WARN: image_size is 0 (bootloader guesses a default)")
    if len(d) > LIMIT:
        print("FAIL: raw image larger than mb_kernel 0x%x" % LIMIT)
        ok = False
    if img_sz and img_sz > LIMIT:
        print("FAIL: image_size (incl bss) larger than mb_kernel 0x%x" % LIMIT)
        ok = False
    print("load/run     0x%x .. 0x%x" % (LOAD, LOAD + max(img_sz, len(d))))
    print("OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "out/titan2-boot.bin"))
