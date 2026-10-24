<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Open questions on spec/ufs-mt6878.md

Points where the spec is silent or ambiguous. Each one notes what `src/ufs.c`
does for now (the most conservative reading).

1. **Doorbell timeout.** §4 gives timeouts but not what to do after one. §3.6.1 retries NOP OUT
   after a 100 ms timeout, so the slot has to be freed first. Implemented: write UTRLCLR (0x5c) =
   ~1 (UFSHCI: a 0 bit clears that slot) and poll UTRLDBR bit 0 clear for up to 100 ms. If it
   does not clear, the driver sends nothing more (later reads fail, and ufs_close() skips START
   STOP UNIT). Is UTRLCLR usable on this host, and is there a preferred recovery?
2. **Failures after link startup.** The 4 attempts cover §3.1–3.4 only. A failure in §3.5–3.8
   (list not ready, no NOP IN, fDeviceInit, power mode change, LU 2 not ready) is not retried:
   ufs_open() calls ufs_close() and fails. Should any of these restart at §3.1, and should a
   failed HS power mode change fall back to the PWM link instead of failing?
3. **"Must be set / must equal" checks.** HCS bit 3 before a UIC command (§3.3) and
   `(HCS & 0xfe) == 0x0e` (§3.5) have no timeout in the spec. Implemented: poll for up to 100 ms
   each, then fail. Is a wait expected here, or should it be a single read?
4. **Retries that are not in the spec.** TEST UNIT READY (§3.8): 1 ms pause between tries (none
   is specified). READ(10) that returns CHECK CONDITION: retried up to 3 times (reads only;
   other statuses fail at once). Is any delay or retry policy required?
5. **Data length and residuals.** Expected transfer length = the exact byte count (8 for READ
   CAPACITY(10), blocks × 4096 for READ(10)). A response with the underflow or overflow flag
   (byte 1 bits 5/6) fails the command. Correct for this device?
6. **IS error bits.** IS bits 2, 11, 16 and 17 seen after a completion fail that request (IS is
   cleared before each doorbell). Can bit 2 (UIC error) be set by something harmless, e.g. a
   line reset during the power mode change, so that it should be ignored?
7. **ufs_close() without a working device.** If ufs_open() was never called, ufs_close() does
   nothing. If the device never answered NOP OUT (or the slot is stuck), START STOP UNIT is
   skipped and only AHIT = 0, the SMC reset (x2 = 0) and HCE = 0 are done. A failed SMC in
   ufs_close() is logged and the shutdown continues. Is that the right hand-off in those cases?
8. **UIC argument details.** UCMDARG2 is 0 for gets and peer accesses too, and the selector is 0
   for peer get/set (§3.7 step 5). The link startup result code (UCMDARG2 & 0xff) is checked as
   for other UIC commands. Confirm.
