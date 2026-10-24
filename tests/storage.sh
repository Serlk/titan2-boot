#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
# GPT + ext4 test images for storage-test, built without root (sfdisk, mke2fs -d):
# a 4 KiB-sector disk like the UFS user LU, partitions boot_a, boot_b, linux.
# Two file systems: 4 KiB blocks (the normal case) and 1 KiB blocks, where a
# 160 MiB file needs more than 4 extents, i.e. an extent tree of depth 1.
set -eu
H=$1
T=$H/storage
rm -rf "$T"
mkdir -p "$T/root/boot/extlinux" "$T/root/boot/dtbs" "$T/root/many"
printf 'DEFAULT linux\nLABEL linux\n  LINUX /boot/Image\n' >"$T/root/boot/extlinux/extlinux.conf"
head -c 5000000 /dev/urandom >"$T/root/boot/Image"
head -c 40000 /dev/urandom >"$T/root/boot/dtbs/titan2.dtb"
: >"$T/root/boot/empty"
ln -s Image "$T/root/boot/vmlinuz"
ln -s /boot/dtbs "$T/root/dtb-abs"
ln -s ../boot/Image "$T/root/many/up"
for i in $(seq 3000); do echo "$i" >"$T/root/many/file-$i"; done
seq 1 30000000 | head -c 167772160 >"$T/big"
# holes: a sparse file
dd if=/dev/urandom of="$T/root/sparse" bs=4096 count=1 seek=1000 status=none

for bs in 4096 1024; do
	img=$T/fs$bs.img
	rm -f "$img"
	if [ $bs = 1024 ]; then
		cp "$T/big" "$T/root/big"
		size=400M
	else
		rm -f "$T/root/big"
		size=64M
	fi
	mke2fs -q -t ext4 -b $bs -d "$T/root" "$img" $size
	fsz=$(stat -c %s "$img")
	disk=$T/disk$bs.img
	rm -f "$disk"
	truncate -s $((fsz + 64 * 1048576)) "$disk"
	sfdisk -q --sector-size 4096 "$disk" <<EOT
label: gpt
size=1MiB, name="boot_a"
size=8MiB, name="boot_b"
size=$((fsz / 1024))KiB, name="linux"
EOT
	start=$(sfdisk -q --sector-size 4096 -d "$disk" | awk -F'[=,]' '/name="linux"/ {gsub(/ /,"",$2); print $2}')
	dd if="$img" of="$disk" bs=4096 seek="$start" conv=notrunc status=none
	set -- "/boot/Image" "$T/root/boot/Image" "/boot/vmlinuz" "$T/root/boot/Image" \
		"/boot/dtbs/titan2.dtb" "$T/root/boot/dtbs/titan2.dtb" "/dtb-abs/titan2.dtb" "$T/root/boot/dtbs/titan2.dtb" \
		"/many/up" "$T/root/boot/Image" "/many/file-2999" "$T/root/many/file-2999" \
		"//boot/./extlinux/../extlinux/extlinux.conf" "$T/root/boot/extlinux/extlinux.conf" \
		"/boot/empty" "$T/root/boot/empty" "/sparse" "$T/root/sparse" "/boot/nope" - "/many" -
	[ $bs = 1024 ] && set -- "$@" "/big" "$T/big"
	echo "== ext4 block size $bs"
	"$H/storage-test" "$disk" 4096 linux "$@"
done
