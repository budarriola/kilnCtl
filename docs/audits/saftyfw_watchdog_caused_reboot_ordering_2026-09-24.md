# SaftyFW watchdog_enable_caused_reboot() ordering -- premise not confirmed

**Date:** 2026-09-24
**Scope:** `firmware/SaftyFW/src/main.c` lines ~313-333 (`watchdog_enable()` then
`boot_reason_read(watchdog_caused_reboot(), watchdog_enable_caused_reboot())`),
`firmware/SaftyFW/src/boot_reason.c`.

## Suspected defect (reviewer finding, not confirmed)

`watchdog_enable_caused_reboot()` is called after `watchdog_enable()` has
already run in `main.c`. The concern: if `watchdog_enable()` writes the same
scratch register that `watchdog_enable_caused_reboot()` reads to distinguish a
genuine watchdog-timeout reboot from a debugger/`watchdog_reboot()` reset,
that signal would be destroyed before it is read, on every boot, making the
flag meaningless.

## SDK evidence

Toolchain SDK checked: `C:\pico-tools\pico-sdk\src\rp2_common\hardware_watchdog\watchdog.c`.

```c
#define WATCHDOG_NON_REBOOT_MAGIC 0x6ab73121

void watchdog_enable(uint32_t delay_ms, bool pause_on_debug) {
    // update scratch[4] to distinguish from magic used for reboot to specific address, or 0 used to reboot
    // into regular flash path
    watchdog_hw->scratch[4] = WATCHDOG_NON_REBOOT_MAGIC;
    _watchdog_enable(delay_ms, pause_on_debug);
}
...
bool watchdog_caused_reboot(void) {
    // If any reason bits are set this is true
    ...
    return watchdog_hw->reason && rom_get_last_boot_type() == BOOT_TYPE_NORMAL;
}

bool watchdog_enable_caused_reboot(void) {
    return watchdog_hw->reason && watchdog_hw->scratch[4] == WATCHDOG_NON_REBOOT_MAGIC;
}
```

Two facts follow directly from this source:

1. `watchdog_enable()` always writes the **same fixed constant**
   (`WATCHDOG_NON_REBOOT_MAGIC`) into `scratch[4]`, every single boot,
   unconditionally. It is not a per-boot value and it does not depend on, or
   read, any prior state of `scratch[4]` -- there is nothing to "clobber" in
   the sense of destroying information from the *previous* boot, because the
   write is identical regardless of what was there before.
2. The actual per-boot signal `watchdog_enable_caused_reboot()` depends on is
   `watchdog_hw->reason`, a separate hardware-latched register set by the
   watchdog block itself on a real timeout. That register is untouched by
   `watchdog_enable()`/`_watchdog_enable()` -- neither function writes
   `watchdog_hw->reason` anywhere in this file.

Consequently, calling `watchdog_enable_caused_reboot()` after
`watchdog_enable()` in the same boot returns exactly the same answer as
calling it before: `scratch[4]` already reads as `WATCHDOG_NON_REBOOT_MAGIC`
in both cases (this boot is about to write, or just wrote, that same
constant), and `watchdog_hw->reason` -- the register that actually carries
the "did a timeout or a debugger/reboot-to-address event cause this boot"
information -- was latched by hardware before any of `main()` ran and is
unaffected by the ordering of these two calls.

The only way `watchdog_enable_caused_reboot()`'s answer could be corrupted by
call ordering is if something wrote a *different* value into `scratch[4]`
between reset and the read -- e.g. `watchdog_reboot()`, which writes
`0xb007c0d3` or `0` depending on whether a target PC was given. That path is
not part of this boot sequence: SaftyFW's own boot only calls
`watchdog_enable()`, and its magic constant is stable across every boot that
takes this path.

## Existing in-repo documentation

`firmware/SaftyFW/src/main.c:324-331` already carries a comment making this
exact argument (that the `scratch[4]` write is a constant marker, not a
per-boot value, so reading before/after `watchdog_enable()` is equivalent,
and that what actually matters is `watchdog_hw->reason` being latched by
hardware before firmware runs). This audit independently verified that
comment against the pico-sdk source actually used by this build
(`C:\pico-tools\pico-sdk`) and confirms it is accurate.

## Verdict

**Not confirmed.** The SDK does write `watchdog_hw->scratch[4]` inside
`watchdog_enable()`, but that write is an idempotent, boot-invariant constant
that carries no per-boot information -- it is not the register that encodes
"did the watchdog actually cause this reset" (that is
`watchdog_hw->reason`, untouched here). The current ordering in
`firmware/SaftyFW/src/main.c` (call `watchdog_enable()` first, then
`watchdog_caused_reboot()`/`watchdog_enable_caused_reboot()`) produces the
same result as the reverse ordering would. No functional defect exists. No
code change made.

## "Reset one side of a pair" check

Considered who else reads `scratch[4]`/`watchdog_hw->reason` or consumes
`boot_reason`: `boot_reason.c` owns `scratch[0]`/`scratch[1]` (this
firmware's own trip-reason latch, a distinct pair from the SDK's
`scratch[4]`), and `hal_scratch_claim()` in `main.c` registers ownership of
scratch slots 0/1/2/3/5/6/7 for bookkeeping (slot 4 is not claimed there,
consistent with slot 4 being pico-sdk's own reserved slot, not this
firmware's). No other module in `firmware/SaftyFW` reads `scratch[4]` or
`watchdog_hw->reason` directly (only `main.c`, via the two SDK accessor
functions). No cross-module pairing defect found.
