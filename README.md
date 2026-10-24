<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# titan2-boot

[Русская версия](README.ru.md)

A boot menu for the Unihertz Titan 2 (MT6878): Android or mainline Linux. It is an aarch64
bare-metal second stage after the stock LK. The stock LK is never replaced or reflashed: it loads
this image from `boot_a` exactly like a kernel (gunzip to 0x40000000, jump at EL1 with x0 = the FDT
it has already built). How the stock LK loads an image is described in titan2-docs:
`docs/lk-boot-requirements.md`.

Works on the phone (2026-10-24): the menu, Android, recovery and power-off charging, mainline
from the pack, and the UFS reader with the entries of the `linux` partition.

## How it works

`boot_a` = titan2-boot + a pack of named files (`src/pack.h`, `tools/mkpack.py`), among them an
`extlinux.conf` (U-Boot's syntax, `src/conf.c`) that lists the menu entries. With `UFS on` in it,
the menu gets one more item, "Linux partition": only when it is chosen (or is the default,
`DEFAULT ufs`) does titan2-boot bring up the UFS (read-only, `src/ufs.c`) and list the entries of
`/boot/extlinux/extlinux.conf` on the ext4 partition `linux` (`src/blk.c` GPT, `src/ext4.c`).
The Android kernel stays in the pack, so recovery, power-off charging and a normal Android boot
never depend on the UFS reader.
`vendor_boot_a`, `init_boot_a` and `dtbo_a` stay Android's, so LK builds the stock FDT and loads
the firmware (SCP, modem, ...) as it does for Android.

Kernels may be raw, gzip or lz4 (`src/inflate.c`, `src/lz4.c`). They are unpacked into free
memory taken from LK's memory map (`src/memmap.c`); the MMU maps that memory cacheable and
everything LK reserved as Device, so the CPU never touches protected memory (`src/mmu.c`).

Entry types (extlinux.conf):
- `LINUX`/`FDT`/`INITRD`/`APPEND` — mainline; `ANDROID <file>` or `ANDROID part:boot_b` — Android.

- **Android:** the kernel gets LK's FDT and ramdisk unchanged.
- **Linux:** the mainline DTB from the pack gets what LK found out at boot (`src/fdtfix.c`):
  `/memory`, every `/reserved-memory/mblock-*` node (the framebuffer gets `no-map`), LK's
  `/chosen` entries (`atag,*`, `kaslr-seed`, ...), the properties LK writes into the modem
  nodes, and `/mdproperties`, `/md_attr_node`. bootargs as LK builds them: the DTB's own, then
  LK's earlycon, console and ramoops options, then the pack's command line.
- **Fastboot / Recovery / Reboot:** through the reboot-mode register (`TOPRGU+0x24`) and a PSCI
  reset.
- When LK did not boot normally (recovery, power-off charging, alarm, meta), there is no menu:
  Android is started at once, because it handles those modes.

The menu is drawn into LK's framebuffer (Terminus 16x32). The panel runs in DSI command mode, so
after drawing, one frame is started through LK's pipeline (mutex 0, OVL0, DSI0). The sequence
is the one in the mainline driver `mtk_cmdfb_kick.c`, which has been verified on the hardware.
Before jumping to a kernel, the pipeline is put back the way LK left it. Keys: Volume Up/Down
move, Power starts the entry. The volume keys are GPIO54/55 (set up as input with pull-up, as
Android does); the power key is read from the MT6363 PMIC over SPMI (PMIF software channel 2,
`TOPSTATUS`). If the PMIC does not answer, Volume Down starts instead. Until a key is pressed, a
countdown with a progress bar runs to the default entry.

## Build

```
make TOOLCHAIN=/path/to/clang-r574158       # out/titan2-boot.bin (no pack)
make test LK_FDT=<LK's FDT> DTB=<mainline DTB>   # the LK data merge on the host (cc, ASan/UBSan)
make qemu                                   # test build for qemu virt (out-qemu/)
```

The toolchain is the AOSP prebuilt clang r574158 (clang 21). The build uses `-mstrict-align`
(the code runs with the MMU off until the memory map is set up). The boot.img is made by the
scripts of titan2-tools: `scripts/make-bootmenu.sh` (with `tools/mkpack.py` from here), and the
qemu test is `scripts/test-bootmenu-qemu.sh`. Installing Android + Linux side by side:
titan2-docs, `docs/DUALBOOT.md`.

Verified on the phone (2026-10-24): the menu, the keys, Android (also after the UFS was used),
recovery and power-off charging (no menu, straight to Android), mainline from the pack, the
MMU, unpacking gzip/lz4 kernels, and the UFS reader (HS-G4, GPT, ext4) with the entries of the
`linux` partition. On the phone the UFS reader logs into Android's pstore from the moment it
starts, so a hang can be read back in `/sys/fs/pstore/console-ramoops-0` after the reset.

## How this project is made / affiliation

- This project is developed with the help of AI coding assistants (Anthropic Claude). Commits
  carry a `Co-Authored-By: Claude` trailer; every change is reviewed and tested on the device
  by the human maintainer (serlk), who is responsible for it.
- It is **not affiliated with postmarketOS** or MT6878-Mainline. We build on their GPL kernel
  work (thanks!), but postmarketOS does not accept AI-assisted contributions, so nothing from
  here is submitted to postmarketOS, and this is not a postmarketOS port.

## Sources

The hardware facts used here (addresses, register values, the state the stock
boot loader leaves the hardware in) come from the phone itself: register
readings on the device, the factory device tree and boot logs, the phone's
GPL-declared kernel modules and an analysis of the behaviour of the stock boot
loader (no boot loader code is reproduced); public MediaTek GPL sources and
upstream Linux were used for comparison. The UFS reader is written by the
clean-room method: from `spec/ufs-mt6878.md`, a description of facts only, by
a developer who did not see the boot loader analysis. No proprietary code,
firmware or blobs are included.

## License

Copyright (c) 2026 serlk. GPL-2.0-or-later, see [LICENSE](LICENSE). `src/font.h` is generated
from the Linux kernel font `lib/fonts/font_ter16x32.c` (GPL-2.0), so the resulting binary is
distributed under GPL-2.0-only. `libfdt/` is taken unmodified from the Linux kernel
(GPL-2.0-or-later OR BSD-2-Clause). `src/inflate.c` is an altered version of zlib's puff.c
(Zlib, see the file). No vendor code: the UFS and display code is written from our own
descriptions of the hardware behaviour.

This project is not affiliated with Unihertz or MediaTek.
