<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# titan2-boot

[Русская версия](README.ru.md)

A minimal aarch64 bare-metal second-stage bootloader for the Unihertz Titan 2 (MT6878). The stock
LK is never replaced or reflashed: it loads this image from `boot_a` exactly like a kernel
(gunzip to 0x40000000, jump at EL1 with x0 = the FDT the stock LK has already built). How the
stock LK loads an image is described in titan2-docs: `docs/lk-boot-requirements.md`.

**Not part of the alpha1 images.** It has not been flashed or tested on a phone yet.

## Status

Proof-of-life skeleton:
- a valid arm64 Image header, so LK (and any arm64 loader) accepts it;
- reads the FDT passed in x0 and logs `bootargs` and other facts over the UART
  (`0x11001000`, 921600);
- draws a menu on the LK framebuffer (`0xfe04f000`, ARGB8888, 1440x1440);
- reads Volume Up / Volume Down from the keypad matrix;
- kicks the watchdog while idle;
- the Fastboot / Recovery / Reboot entries use `syscon-reboot-mode` and PSCI.

It deliberately does **not** chain-load a kernel yet. Turning that on is a separate step that needs
the hardware, and nothing in this repository flashes the device.

To be confirmed on hardware (marked in the code):
- the exact Volume Up matrix bit (`io.c:key_vol_up`);
- the panel is in DSI command mode, so framebuffer writes only show after a DSI frame trigger
  (see titan2-docs: `docs/hardware/display.md`). This is not implemented yet; the UART is the
  reliable output for now.

## Build

```
make TOOLCHAIN=/path/to/clang-r574158
```

The toolchain is the AOSP prebuilt clang r574158 (clang 21). The output is `out/titan2-boot.bin`
(raw Image) and `out/titan2-boot.bin.gz`. The Makefile runs `tools/check-image.py`, which checks
the header and size offline.

## Next steps

1. Vendored libfdt and node copy for the mainline DT path.
2. DSI command-mode trigger, so that the menu is visible.
3. Chain-loading (behind an explicit build flag), tested only when the user decides to flash
   `boot_a`.
4. Later: a UFS reader, so that kernels can live in real partitions instead of inside `boot_a`.

## Sources

The hardware facts used here (addresses, register values, the state the stock
boot loader leaves the hardware in) come from the phone itself: register
readings on the device, the factory device tree and boot logs, the phone's
GPL-declared kernel modules and an analysis of the behaviour of the stock boot
loader (no boot loader code is reproduced); public MediaTek GPL sources and
upstream Linux were used for comparison. No proprietary code, firmware or blobs
are included.

## License

Copyright (c) 2026 serlk. GPL-2.0-or-later, see [LICENSE](LICENSE). `src/font8x8.h` contains
glyphs from the Linux kernel console font (`lib/fonts/font_8x8.c`, GPL-2.0-only), so the
resulting binary is distributed under GPL-2.0-only.

This project is not affiliated with Unihertz or MediaTek.
