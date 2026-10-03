# Completed work -- October 2026

Closeout notes relocated out of `ROADMAP.md` per its maintenance rule. All items
below landed on `origin/main` on 2026-10-02 and are host-tested and target-built
only; **none has been verified on hardware yet** (the open bench items live in
the ROADMAP index rows for the recovery image, the OTA matrix and the bench test
system).

## A3: `crash_report/clear` on `http_async_job` (`32fe5cee`, `689f0f24`)

The coredump erase behind `POST /api/crash_report/clear` now runs on the
`http_async_job` task instead of `httpd_worker`. Same one-POST/one-response
wire contract, a new 503 busy reply, and `GET /api/crash_report` gains
`clear_in_progress` on its `present:false` reply; no new route. Review fixed the
job's stack grade and a GET doc comment. Before the change the bench measured
a 3.4 s httpd stall; the after-change stall is unmeasured.

## S7: single-flight guard for Pico safety-config writers (`beaab290`..`b8c3e4a8`)

One guard serialises every writer of the Pico safety config, wired into the
async job, the sweep, the swap and the reconcile path. Residual, documented in
`b8c3e4a8`: the synchronous HTTP writers still only check `ASYNC_JOB`, and the
boot-recovery path is not covered. The spinlock has not run on hardware.

## Recovery entry (`e25d8a30`..`24043ba9`)

`POST /api/ota/esp/recovery_boot` is the deliberate way from the application
into the recovery image; the boot_guard threshold now switches into recovery
the same way, and the `recovery_enter` MCP tool wraps the route (facade count
206 to 207). The boot target is restored on SET_FAILED.

## Pico bootloader hardening (`79264f5f`..`d7d6e9fc`)

The bootloader checks slot linkage before PENDING_VERIFY, `UPDATE_STATUS`
carries a slot trailer, and an 8 s watchdog is armed before the jump to the
app. Host test for `recovery_update.c`, plus the recovery ESP power-cycle
mapping helper and slot/end-state tests.

## Recovery image hardening A-G and hold watchdog (`7a3331a8`..`d4eac0f6`)

Wi-Fi degrades instead of rebooting in a loop, honest `sw_reset`/`recovery_exit`/
`wifi_reset` failures, richer status (uptime, reset reason, OTA state, coredump,
otadata, AP event counts), a per-client nonce ring, relays and LCD pins held
under a 1 s verifying watchdog, a latched hold fault reported in `relay_fault`
and on the LCD, `sw_reset` no longer writes otadata, and host tests for the
upload path and relay-hold logic. The hold-watchdog task stack margin is not
yet measured.

## Recovery image unauthenticated, LCD-only passphrase (`00e99237`, `581278ba`)

Owner decision 2026-10-02: the recovery image has no route authentication; its
SoftAP uses a random per-boot passphrase drawn from the RNG entropy source and
shown only on the LCD. `KILNCTL_RECOVERY_AP_PASSPHRASE` is a documented
convention for a human or joiner automation to supply the LCD passphrase; no code
reads or prints it, by design (never a call parameter, never echoed).

## `recovery_status` rendering (`c7bd87d9`)

The MCP tool renders the recovery image's new diagnostic keys.
