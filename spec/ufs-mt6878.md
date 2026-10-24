<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# UFS on the Titan 2 (MT6878): behavioural specification for a read-only reader

This is a facts-only description of the hardware and of the state the boot chain leaves it in:
register addresses, fields, values, ordering and timing. It contains no code. The MT6878-specific
values (the M-PHY settings in §3.2, the device reset call, the hand-off state) are facts of
proprietary origin: they were determined by analysing the behaviour of the boot loader shipped
on the phone and checked against its boot log; they are not found in public source code. It is the input
for implementing `src/ufs.c` (`ufs_open()`, `ufs_close()`, see `src/ufs.h`). The standard parts
(UFSHCI 3.0 register map and transfer request format, UPIU, UniPro attributes, SCSI) are as
in JEDEC JESD223/JESD220 and the MIPI UniPro specification; the public Linux drivers
`drivers/ufs/core/ufshcd.c` and `drivers/ufs/host/ufs-mediatek.c` use the same definitions.

## 1. Resources

| what | value |
|---|---|
| UFSHCI registers (standard map) | 0x112b0000 |
| M-PHY registers | 0x112a0000 |
| PHY trim efuse word | 0x11f10260 (read-only) |
| secure monitor call for the device reset | SMC `0xc2000276` (MediaTek SiP "UFS control", as in mainline `ufs-mediatek-sip.h`), x1 = 2 (device reset), x2 = 0 drive RST_n low / 1 release it. Return 0 on success |
| interrupts | not used; everything is polled |
| memory | MMU on, caches on (normal write-back) for the loader's memory: the transfer request list, the command descriptor and data buffers must be cleaned to PoC before the doorbell and invalidated after completion (`dcache_range()` in `src/mmu.h`) |

## 2. State at entry

When `ufs_open()` runs, the previous boot stage has: disabled the host (HCE = 0), set the auto-hibernate
timer register (AHIT, 0x18) to 0, sent START STOP UNIT with POWER CONDITION PowerDown to the
UFS Device well-known LUN (0xD0), driven the device reset RST_n low (and left it low). Clocks,
supplies and the M-PHY configuration are still on. Interrupts are masked in the CPU.

`ufs_close()` must leave exactly this state again before a kernel starts (the kernel's UFS driver
starts from it): AHIT = 0; START STOP UNIT to W-LUN 0xD0, CDB `1b 00 00 00 30 00`, up to 3
attempts; SMC device reset with x2 = 0; HCE = 0.

## 3. Bring-up sequence

Up to 4 attempts of steps 3.1–3.4; any failure restarts at 3.1.

### 3.1 Host enable

1. If HCE bit 0 is set, write HCE = 0 and wait (poll, up to 10 ms) for it to read 0.
2. Device reset: SMC with x2 = 0, wait 10 µs, SMC with x2 = 1, wait 10 ms.
3. HCE = 1, wait 1 ms, then poll HCE bit 0 = 1 (up to 100 ms).
4. IE |= 0x470.

### 3.2 M-PHY setup

All offsets are from 0x112a0000. "field[h:l] = v" means read-modify-write of only those bits.
Steps in this order:

1. Let E = the efuse word. If E bit 0 is 0: 0x803c field[11:8] = 0 and field[4:0] = 0x1e (bits
   [7:5] and [31:12] kept); then 0x1004 field[29:24] = 0x13. If E bit 1 is 0: 0x813c field[11:8] = 0
   and field[4:0] = 0x1e. (On the test phone E = 0x881d3c03, so both are skipped.)
2. For each lane block base B in 0x8000, 0x8100 (in this order): read X = B+0x3c. Let r = X[11:8]
   and c = X[4:0]. The trim index t is: c − 4 when r = 0; r + 27 when r is 1 or 2; r + 21 otherwise.
   Write B+0x38 (whole register) with four fields, everything else 0, all arithmetic on t taken as an
   unsigned 32-bit number (t may be negative, i.e. wrap around), divisions rounding down:
   - [27:24] = ((t + 26) / 4) mod 16
   - [20:16] = ((3t + 18) / 4) mod 32
   - [11:8]  = ((t + 3) / 6 + 6) mod 16
   - [4:0]   = ((5t + 3) / 6 + 4) mod 32
   Test values: t = 0 → 0x06040604; t = 10 → 0x090c080c; t = −4 → 0x0501000b.
3. 0x2088 field[31:24] = 0x4a; 0x2094 field[15:0] = 0x4a4a; 0x2098 field[31:24] = 0x4a.
4. For each block base B in 0xa000, 0xa100 (in this order, all of one block before the next):
   B+0x34 field[10:0] = 0x1c1; B+0x38 field[26:16] = 0x1c2; B+0x3c field[10:0] = 0x1c7 (one write);
   B+0x3c field[26:16] = 0x1f9 (a second write); B+0x44 field[26:16] = 0x1ef; B+0xcc field[10:0] = 0x1ff.
5. Whole-register writes, block 0xa000 then 0xa100: +0x54 = 0x1a191104, +0x58 = 0x1a191105,
   +0x5c = 0x09080602, +0x60 = 0x09080602, +0x64 = 0x0a0a0a04, +0x68 = 0x0a0a0a05.
6. 0xa0dc field[29:24] = 0x30 and field[13:8] = 0x30 (one write); the same for 0xa1dc.
7. 0x209c field[31:16] = 0x8e8e; 0x2090 field[23:16] = 0x01; 0x2098 field[2:0] = 0.
8. 0x208c field[11:8] = 0x9; then (a second write) 0x208c field[27:24] = 0x1.
9. 0x1010 bit 7 = 0.

The step uses only the current register contents and the efuse, so repeating it is harmless.

### 3.3 Before link startup (DME, local)

UIC commands as in UFSHCI (UCMDARG1 = attribute << 16 | selector 0, UCMDARG2 = 0 for a normal
set, UCMDARG3 = value; UICCMD = 1 get / 2 set / 3 peer get / 4 peer set / 0x16 link startup; wait
IS bit 10 up to 0.5 s, clear it by writing 1; result code = UCMDARG2 & 0xff, 0 = success; HCS bit 3
must be set before issuing). In order:

1. VS_SAVEPOWERCONTROL (0xD0A6): clear bit 6.
2. PA_SCRAMBLING (0x1585) = 0.
3. 0xD0A6: clear bits 0, 2, 3.
4. VS_DEBUGCLOCKENABLE (0xD0A1): set bit 5.

### 3.4 Link startup

UIC 0x16 with all arguments 0; wait for IS bit 10 (0.5 s); then HCS bit 0 (device present) must be 1.

### 3.5 Transfer request list

UTRLBA (0x50) / UTRLBAU (0x54) = physical address of a 1 KiB-aligned list (slot 0 is enough);
IE |= 0x30a75; then (HCS & 0xfe) must equal 0x0e; UTRLRSR (0x60) = 1.

### 3.6 Device initialisation

1. NOP OUT (transaction 0x00, no data) until a NOP IN with response 0 comes back, up to 10 tries,
   100 ms each.
2. QUERY REQUEST (0x16), function 0x81 (standard write), opcode 0x06 SET FLAG, IDN 0x01 fDeviceInit.
3. Poll QUERY function 0x01, opcode 0x05 READ FLAG, IDN 0x01 until the flag reads 0, up to 5 s
   (flag value: bit 0 of byte 23 of the QUERY RESPONSE UPIU).

### 3.7 High-speed mode (HS-G4, rate B, 1 lane)

All local DME sets unless marked peer:
1. 0x15B0..0x15B5 (PA_PWRMODEUSERDATA0..5) = 0x1fff, 0xffff, 0x7fff, 0x1fff, 0xffff, 0x7fff.
2. 0xD041, 0xD042, 0xD043 = 0x1fff, 0xffff, 0x7fff.
3. 0x15D4 (PA_TXHSADAPTTYPE) = 1 for gear 4 (3 for gears 1–3).
4. 0x15B0..0x15B5 again as in 1.
5. Peer get 0x1543 → local set 0x1564; local get 0x1543 → peer set 0x1564.
6. 0x1569 = 1, 0x1584 = 1 (terminations); 0x1568 = gear, 0x1583 = gear; 0x1560 = 1, 0x1580 = 1
   (lanes); 0x156A = 2 (series B).
7. Clear IS bit 4, then 0x1571 (PA_PWRMODE) = 0x11 (fast mode both directions); wait IS bit 4
   (0.5 s), clear it; HCS bits [10:8] must read 1.

### 3.8 The user LU

LU 2 holds the GPT with all named partitions (boot_a, boot_b, userdata, …). Logical block size
4096 bytes; READ CAPACITY(10) reports 124960768 blocks on the test phone. After the reset the
first commands return CHECK CONDITION / UNIT ATTENTION (sense key 6): repeat TEST UNIT READY on
LU 2 until GOOD (up to 10 times), then READ CAPACITY(10).

## 4. Transfer requests

One slot, polled. Transfer request descriptor (32 bytes): dword 0 = 0x10000000, plus 0x04000000
for data from the device; dword 2 = 0x0000000f before the request (overall command status, 0 =
success afterwards); dwords 4/5 = address of the command descriptor (128-byte aligned); dword 6 =
0x00800080 (response UPIU at 0x200 bytes, 0x80 dwords long); dword 7 = PRDT entry count in [15:0],
0x0100 in [31:16] (PRDT at 0x400 bytes). Command descriptor 0xc00 bytes: command UPIU at 0x000,
response UPIU at 0x200, PRDT at 0x400 (128 entries of 16 bytes: address low, address high, 0,
byte count − 1; at most 256 KiB per entry; addresses and lengths dword aligned).

Doorbell: clear IS, write UTRLDBR (0x58) = 1, poll until bit 0 clears (5 s for commands, 100 ms
for NOP OUT, 1.7 s for QUERY). IS bits 2, 11, 16, 17 report errors. Response UPIU: byte 0 = 0x21
(response) / 0x20 (NOP IN) / 0x36 (query response); byte 6 = response (0 = target success);
byte 7 = SCSI status (0 GOOD, 2 CHECK CONDITION, the sense key is then in the low nibble of byte
0x24).

READ(10): CDB `28 00 LBA(4, big endian) 00 COUNT(2, big endian) 00`, at most 0x2000 blocks per
command. COMMAND UPIU: byte 0 = 0x01, byte 1 = 0x40 (read) or 0, byte 2 = LUN, byte 3 = task tag,
bytes 12–15 = expected transfer length (big endian), CDB from byte 16.

## 5. What must never be sent (bricking or data loss)

Only these may reach the device: NOP OUT; QUERY READ FLAG / SET FLAG of IDN 1 (fDeviceInit) only;
TEST UNIT READY, REQUEST SENSE, INQUIRY, READ CAPACITY(10), READ(10) to LUs 0–2; START STOP UNIT to
W-LUN 0xD0 with power condition PowerDown only. Everything else is refused in one place: in
particular any WRITE, UNMAP, WRITE BUFFER (firmware update), FORMAT UNIT, SANITIZE, any QUERY write
of attributes or descriptors (bBootLunEn selects the preloader copy; bConfigDescrLock is
permanent), any other flag (fPermanentWPEn is permanent), and anything to the RPMB W-LUN 0xC4.
