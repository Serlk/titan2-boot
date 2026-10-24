#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
"""Pack titan2-boot and its files into a boot.img for boot_a.

  tools/mkpack.py --loader out/titan2-boot.bin --conf extlinux.conf \\
      --file android/Image.lz4=Image.lz4 --file linux/Image.gz=Image.gz ... \\
      [--android-boot boot.img] --out boot.img [--raw pack.bin]
  tools/mkpack.py --info boot.img

The pack is a table of named files (src/pack.h); extlinux.conf (src/conf.c)
refers to them by path ("/linux/Image.gz"). Kernels may be raw, gzip or lz4:
titan2-boot unpacks them. LK gunzips the whole pack to 0x40000000
(titan2-docs: docs/lk-boot-requirements.md §0, §4), at most 0x7c80000 bytes.
"""
import argparse
import gzip
import struct
import sys

LOAD = 0x40000000
PACK_OFFSET = 0x100000
LK_FDT = 0x47C80000              # LK's FDT (mb_dtb): the pack must end below it
BOOT_PART = 64 << 20             # boot_a
PAGE = 4096

MAGIC = b"T2BOOTPK"
VERSION = 2
PACK_MAX = 32
HDR_FMT = "<8sIIQ"               # magic, version, count, total_size
ENT_FMT = "<56sQQ"               # name, offset, size
CONF = "extlinux.conf"

BOOT_MAGIC = b"ANDROID!"
BOOT_V4_HDR = 1584


def align(v, a):
    return (v + a - 1) // a * a


def read_boot(path):
    """kernel and os_version from a boot.img (v0..v4)."""
    d = open(path, "rb").read()
    if d[:8] != BOOT_MAGIC:
        sys.exit("%s: not a boot.img" % path)
    ksize, _, os_version = struct.unpack_from("<III", d, 8)
    version = struct.unpack_from("<I", d, 40)[0]
    page = PAGE if version >= 3 else struct.unpack_from("<I", d, 36)[0]
    return d[page:page + ksize], os_version


def boot_img(kernel, os_version):
    """boot.img v4, kernel only, empty command line (LK reads it from the wrong offset anyway)."""
    hdr = struct.pack("<8sIIII4I I", BOOT_MAGIC, len(kernel), 0, os_version, BOOT_V4_HDR,
                      0, 0, 0, 0, 4)
    hdr += b"\0" * 1536 + struct.pack("<I", 0)
    assert len(hdr) == BOOT_V4_HDR
    out = hdr.ljust(PAGE, b"\0") + kernel
    return out.ljust(align(len(out), PAGE), b"\0")


def pack(a):
    loader = bytearray(open(a.loader, "rb").read())
    if len(loader) > PACK_OFFSET:
        sys.exit("loader larger than PACK_OFFSET")
    if loader[56:60] != b"ARM\x64":
        sys.exit("loader is not an arm64 Image")

    files = [(CONF, open(a.conf, "rb").read())]
    for spec in a.file or []:
        name, _, path = spec.partition("=")
        name = name.lstrip("/")
        if not path or len(name.encode()) >= 56:
            sys.exit("--file NAME=PATH, NAME shorter than 56 bytes: %s" % spec)
        if name in (f[0] for f in files):
            sys.exit("duplicate file %s" % name)
        files.append((name, open(path, "rb").read()))
    if len(files) > PACK_MAX:
        sys.exit("more than %d files" % PACK_MAX)
    check_conf(files)

    hdr_size = struct.calcsize(HDR_FMT) + PACK_MAX * struct.calcsize(ENT_FMT)
    off = align(PACK_OFFSET + hdr_size, PAGE)
    entries = []
    for name, data in files:
        entries.append((name, off, data))
        off = align(off + len(data), PAGE)
    total = off
    if LOAD + total > LK_FDT:
        sys.exit("pack is 0x%x bytes and would reach LK's FDT at 0x%x" % (total, LK_FDT))

    img = bytearray(total)
    img[:len(loader)] = loader
    struct.pack_into("<Q", img, 16, align(total, 0x10000))   # arm64 header image_size
    h = struct.pack(HDR_FMT, MAGIC, VERSION, len(entries), total)
    for name, o, data in entries:
        h += struct.pack(ENT_FMT, name.encode(), o, len(data))
        img[o:o + len(data)] = data
    img[PACK_OFFSET:PACK_OFFSET + hdr_size] = h.ljust(hdr_size, b"\0")

    gz = gzip.compress(bytes(img), compresslevel=9, mtime=0)
    os_version = read_boot(a.android_boot)[1] if a.android_boot else 0
    boot = boot_img(gz, os_version)
    if len(boot) > BOOT_PART:
        sys.exit("boot.img is %d bytes, boot_a holds %d" % (len(boot), BOOT_PART))
    open(a.out, "wb").write(boot)
    if a.raw:
        open(a.raw, "wb").write(img)
    show(bytes(img))
    print("boot.img: %s, %d bytes (gzip %d, unpacked 0x%x)" % (a.out, len(boot), len(gz), total))


def check_conf(files):
    """every path the config names must be in the pack (or a part:<name>)"""
    names = {f[0] for f in files}
    text = files[0][1].decode()
    bad = []
    for line in text.splitlines():
        w = line.split(None, 1)
        if len(w) == 2 and w[0].lower() in ("linux", "kernel", "initrd", "fdt", "devicetree", "android"):
            p = w[1].strip()
            if p.startswith("part:"):
                continue
            if p.lstrip("/") not in names:
                bad.append(p)
    if bad:
        sys.exit("extlinux.conf names files not in the pack: %s" % ", ".join(bad))


def show(img):
    magic, version, count, total = struct.unpack_from(HDR_FMT, img, PACK_OFFSET)
    if magic != MAGIC or version != VERSION:
        sys.exit("no pack v%d header" % VERSION)
    print("pack v%d: %d files, 0x%x bytes" % (version, count, total))
    base = PACK_OFFSET + struct.calcsize(HDR_FMT)
    for i in range(count):
        name, o, size = struct.unpack_from(ENT_FMT, img, base + i * struct.calcsize(ENT_FMT))
        print("  0x%08x %10d  %s" % (LOAD + o, size, name.rstrip(b"\0").decode()))
        if name.rstrip(b"\0").decode() == CONF:
            for line in img[o:o + size].decode().splitlines():
                print("      | " + line)


def info(path):
    kernel, _ = read_boot(path)
    show(gzip.decompress(kernel))


def main():
    global LOAD
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--info", metavar="BOOT_IMG")
    p.add_argument("--loader")
    p.add_argument("--conf", help="extlinux.conf for the menu")
    p.add_argument("--file", action="append", metavar="NAME=PATH")
    p.add_argument("--android-boot", help="Android boot.img to take os_version from")
    p.add_argument("--out")
    p.add_argument("--raw", help="also write the unpacked image")
    p.add_argument("--load-addr", type=lambda v: int(v, 0), default=LOAD,
                   help="where the pack runs (QEMU test build: 0x40200000)")
    a = p.parse_args()
    LOAD = a.load_addr
    if a.info:
        info(a.info)
    elif a.loader and a.conf and a.out:
        pack(a)
    else:
        p.error("--loader, --conf and --out, or --info")


if __name__ == "__main__":
    main()
