#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
# Vectors for decompress-test: gzip (all levels, incompressible = stored blocks, empty) and
# lz4 legacy (`lz4 -l`, like Android kernels), from generated data and, if given, a real
# kernel: DECOMP_IMAGE=<raw arm64 Image>.
set -eu
H=$1
T=$H/decomp
mkdir -p "$T"
head -c 3000000 /dev/urandom >"$T/random"
: >"$T/empty"
yes "titan2-boot inflate test line" | head -c 5000000 >"$T/text"
for f in random empty text ${DECOMP_IMAGE:+"$DECOMP_IMAGE"}; do
	src=$f
	[ -f "$src" ] || src=$T/$f
	b=$T/$(basename "$src")
	for lvl in 1 6 9; do
		gzip -$lvl -n -c "$src" >"$b.$lvl.gz"
		"$H/decompress-test" "$b.$lvl.gz" "$src"
	done
	if [ -s "$src" ]; then
		lz4 -l -q -f -c "$src" >"$b.lz4"
		"$H/decompress-test" "$b.lz4" "$src"
	fi
done
