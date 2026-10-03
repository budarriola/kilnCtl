# Completed work -- October 2026

Closeout notes relocated out of `ROADMAP.md` per its maintenance rule. All items
below landed on `origin/main` on 2026-10-02 and are host-tested and target-built
only unless a section below says otherwise. **Verified on hardware 2026-10-03**
(`docs/BENCH_TEST_LOG.md`): the S7 guard's 409, recovery entry and exit, and the
unauthenticated LCD-passphrase recovery AP. Everything else here is still
unverified on hardware (the open bench items live in the ROADMAP index rows for
the recovery image, the OTA matrix and the bench test system).

## A3: `crash_report/clear` on `http_async_job` (`32fe5cee`, `689f0f24`)

The coredump erase behind `POST /api/crash_report/clear` now runs on the
`http_async_job` task instead of `httpd_worker`. Same one-POST/one-response
wire contract, a new 503 busy reply, and `GET /api/crash_report` gains
`clear_in_progress` on its `present:false` reply; no new route. Review fixed the
job's stack grade and a GET doc comment. Before the change the bench measured
a 3.4 s httpd stall; the after-change stall is unmeasured.

## S7: single-flight guard for Pico safety-config writers (`beaab290`..`b8c3e4a8`)

One guard serialises every writer of the Pico safety config, wired into the
async job, the sweep, the swap and the reconcile path. The spinlock has not run
on hardware.

Residual closed afterwards: the synchronous HTTP writers (the five
`safety_cfg_http.c` SET_PARAM/commit handlers and `zones_post_handler`) now
claim the guard under a new `HTTP_SYNC` owner for their whole write, release it
on every return path, and answer 409 busy when the claim fails. The kiln config
apply and backup import already claim atomically at admission (swap submit,
async try_start), so they need no extra claim. The swap worker's boot-recovery
claim retries up to 20 times, 100 ms apart, before running unclaimed. Host tests
cover the 409 for every other owner, release on every path and the bounded
retry; negative-tested by removing the release in both HTTP files, which failed
`zones_http` and `safety_cfg_http`. Hardware 2026-10-03: with a zone current
sweep holding the guard, a commissioning POST answered 409 "another
commissioning operation is running" in 0.34 s and the config CRC did not change.
Two HTTP_SYNC callers racing each other were not observable (httpd serialises
them). Landed as `f42ca1c8`;
Opus review `a0d59984` raised `check_all_task_stack_budgets.py`'s
`http_async_job` ceiling to 4576 B (the 48 B `safety_cfg_writer_release()`
call on that task's path had already pushed it past the old 4528 B on main).
Behaviour change to note: the five `safety_cfg_http.c` handlers used to answer
busy with HTTP 200 and `ok:false`; they now answer 409 with the same JSON body,
so `safety_cfg_http_client.post_commissioning()` raises `SafetyCfgHttpError`
on busy instead of returning `ok:false`, and the relay-type page no longer shows
"Saved." on a refused write.

## Recovery entry (`e25d8a30`..`24043ba9`)

`POST /api/ota/esp/recovery_boot` is the deliberate way from the application
into the recovery image; the boot_guard threshold now switches into recovery
the same way, and the `recovery_enter` MCP tool wraps the route (facade count
206 to 207). The boot target is restored on SET_FAILED. Hardware 2026-10-03:
`recovery_enter(confirm=True)` answered 200 and the board booted the recovery
image; `recovery_exit` returned it to `app` with boot_guard cleared and verified.
The boot_guard threshold switch itself was not exercised.
The ESP upload path (`recovery_push_esp_image` over the recovery AP) was also verified on hardware 2026-10-03 (`docs/BENCH_TEST_LOG.md`, "ESP upload through the recovery image (W5)").

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
upload path and relay-hold logic. Hold-watchdog task stack margin, measured on
the bench 2026-10-03: `relay_hold_stack_free` 1952 B of 3072 B, no hold fault.

## Recovery image unauthenticated, LCD-only passphrase (`00e99237`, `581278ba`)

Owner decision 2026-10-02: the recovery image has no route authentication; its
SoftAP uses a random per-boot passphrase drawn from the RNG entropy source and
shown only on the LCD. `KILNCTL_RECOVERY_AP_PASSPHRASE` is a documented
convention for a human or joiner automation to supply the LCD passphrase; no code
reads or prints it, by design (never a call parameter, never echoed).

Hardware 2026-10-03: the LCD showed the SSID, a 12-character passphrase and the
AP IP on one 480x320 page without scrolling; the SSID `kilnctl-recovery` appeared
in a Wi-Fi scan as WPA2-Personal; a PC joined with the LCD passphrase and read
`GET /api/recovery/status` (200, `auth_mode` "lcd_passphrase"); the passphrase is
absent from the status JSON. Not exercised: the "wifi_storage_fail" path, ESP and
Pico uploads, `wifi_reset`.

## `recovery_status` rendering (`c7bd87d9`)

The MCP tool renders the recovery image's new diagnostic keys.

## Bench runner: trip-clear poll window

`wait_for_trip_clear` (`tools/PcTools/src/kilnctrl/bench_test/cases_smoke.py`, shared by
HP-07 and FL-11) polled `safety_get_diag()` for only 3.0 s after `safety_clear_trip()`.
The ESP serves a cache of the Pico's 2000 ms DIAG push and its context age is observed up
to ~3.5 s, so a working clear could still read `trip_reason` 6 at the deadline: HP-07
FAILed in run 20261003T032924Z_heat_b3_hp after 3.04 s, and a manual clear minutes later
read 0. The window is now 10.0 s (interval unchanged, 0.3 s). The helper also records
`clear_ack` (the return string of `safety_clear_trip()`) and `trip_reason_timeline`
(`(elapsed_s, trip_reason)` per poll) into the case's observed dict when given
`observed=`. Fake-board tests: `tools/PcTools/tests/test_bench_test_trip_clear.py`.
