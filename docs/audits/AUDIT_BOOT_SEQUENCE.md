# Audit: KilnFW boot sequence (app_main to fully up)

Date: 2026-10-14. Label: auboot. Base: origin/dev d5bbf13fc. Code-only, no board access.

Scope: `main.c` app_main, `main_boot_early.c`, `main_control_bringup.c`,
`main_network_http.c`, `main_bridges_bringup.c`; init ordering and failure paths;
relay and heat-enable state before control starts; boot_guard counting,
`recovery_switch_at_boot_threshold()` and recovery-mode gating; the first-boot-after-update
confirm (`main_ota_rollback_confirm_task`).

Not re-reported: AUDIT_UPDATE_FAULTS MED-3 (the one-shot `nvs_ok`/`web_ok`/`ota_ok`
snapshot handed to the confirm task, and the `nvs_report_capture()` one-shot class). Another
change is fixing that. Prior boot audits (`boot_guard_recovery_loop_2026-09-08.md`,
`boot_guard_post_flash_recovery_footgun_2026-09-08.md`, `boot_hang_2026-09-08.md`,
`recovery_bootloader_audit_2026-10-02.md`, `UPDATE_PATH_ROBUSTNESS_AUDIT_2026-10-13.md`) were
read, and nothing they fixed is repeated here.

HIGH 0, MED 2, LOW 3, INFO 3

## Key question

**Can one init failure energize relays?** It cannot energize a relay that was off.
However, one failure, an I2C probe miss on the SX1509, skips the reset that drops relays a
warm restart left latched ON. They then stay latched and cannot be commanded for the whole
boot (MED-2). The Pico's K4 is still in series and is opened by the ESP fault/link-loss path,
so element current depends on SaftyFW alone for that boot.

**Can one init failure walk a healthy board into recovery?** Only through a *persistent*
web/OTA route failure (LOW-1, by design) or the excluded MED-3 snapshot class. The bigger
problem is the reverse (MED-1). The boot is confirmed healthy, which clears the counter and
cancels the rollback, before roughly half of boot has run. A deterministic failure in the
second half therefore reset-loops forever and never reaches recovery.

## Findings

### MED-1: boot is confirmed (counter cleared, OTA rollback cancelled) before boot finishes, so late-boot reset loops never reach recovery

Evidence:
- `main_network_http.c:780`: the `ota_confirm` task is created at `tskIDLE_PRIORITY + 1`, the
  same priority as app_main.
- On its first loop iteration, with no minimum uptime and no wait for boot completion, it
  calls `esp_ota_mark_app_valid_cancel_rollback()` (`:234`) and `boot_guard_mark_healthy()`
  (`:216`/`:264`).
- At that point app_main has still to run all of the following:
  - `kiln_cfg_store_init()` (`:820`), the aux outputs, the swap worker, the `kiln_cfg_http`,
    backup, `safety_cfg_store_init` and stack-margin HTTP starts;
  - `monitor_task_start()` (`:953`);
  - `uart_owner_init()`/`uart_protocol_init()` (`:975`);
  - all of `main_bridges_bringup()` (`main.c:251`): `lvgl_port_start()`
    (`main_bridges_bringup.c:113`), `telemetry_log_start()` (`:207`), the link watchdog and
    `dram_watch_start()`.
- The health criterion (`boot_guard.c:594`) is only `nvs_ok && web_ok && ota_routes_ok`.

Failure scenarios:
1. **monitor_task_start() fails.** `monitor_task_start()` returning `!= pdPASS`, for example
   from internal-RAM exhaustion with an 8 KB floor, is logged as non-fatal (`:953-956`).
   - `monitor_task.c:78` is the only `rtc_watchdog_feed()` call site. The RTC watchdog was
     armed at `main_boot_early.c:422` with a 20000 ms timeout, so it resets the chip about
     20 s after boot.
   - By then `ota_confirm` has already cleared the counter, so every boot counts 1 and then
     clears to 0. The board resets every 20 s forever.
   - It never reaches `RECOVERY_MODE_BOOT_THRESHOLD` (3), never switches to the recovery
     partition, and on an OTA-slot image never rolls back, because rollback was cancelled in
     the first milliseconds.
2. **An update panics in late boot.** The update's new code panics deterministically in
   `kiln_cfg_store_init()` (a schema migration on real data), `lvgl_port_start()` or
   `telemetry_log_start()`.
   - The image is marked valid before the panic, so the bootloader rollback that exists for
     exactly this case is defeated.
   - boot_guard is defeated the same way.
   - With USB-serial recovery dropped (owner decision 2026-10-09), JTAG is the only exit.

This is the "boot order beats bounded waits" class. Confirming health from a task that races
the rest of boot is a timing assumption, not an ordering guarantee.

Fix:
- Signal "boot complete" from the end of `main_bridges_bringup()` (after `app_main_done`)
  through a flag or event-group bit. Have `ota_confirm` wait for it, plus a short soak (for
  example 30 s, longer than `RTC_WATCHDOG_TIMEOUT_MS`), before calling
  `esp_ota_mark_app_valid_cancel_rollback()` or `boot_guard_mark_healthy()`.
- Treat `monitor_task_start()` failure as fatal-to-confirm: no confirm on that boot. Otherwise
  the guaranteed RTC reset is undetectable.
- Keep the existing retry loop.

Host test: none. `test_boot_guard.c` covers the pure `boot_confirm_decide()` truth table only.
Nothing covers when the confirm may run relative to boot completion. A pure
`boot_confirm_decide(..., boot_complete, uptime_ms)` extension would make this testable.

### MED-2: SX1509 probe failure skips the expander reset, so relays latched ON by a warm restart stay ON and uncommandable

Evidence:
- `drivers/hw/SX1509.c:539-543`: `SX1509_start()` returns `ESP_ERR_NOT_FOUND` on an
  `i2c_master_probe()` miss.
- That return comes **before** `SX1509_init()` (which configures the `~RESET` GPIO) and
  before `SX1509_reset(e, e->reset_gpio >= 0)` (`:556`).
- `main_boot_early.c:372-376` shows the same gap when `i2c_new_master_bus()` itself fails: no
  reset of any kind is attempted.
- `~RESET` is a real wired GPIO (`CONFIG_KILNCTL_SX1509_RESET_IO`, default 10,
  `drivers/Kconfig:274`). Pulsing it needs no I2C at all.
- The comment at `SX1509.c:551-555` states the reset exists because "a warm restart ... leaves
  the expander still holding whatever the previous run configured -- possibly with relays
  energized".
- `ctx->io_ready` stays false (`main_boot_early.c:384-397`). `kiln_io_owner_start()` is then
  skipped and nothing calls `kiln_io_reinit()` later in boot, so no path can command the
  relays off that boot.

Failure scenario:
1. The ESP panics or is watchdog-reset mid-firing while K1-K3 are energized. The expander
   stays powered, and a slave left mid-transfer holds SDA, or the probe simply times out once.
2. The probe misses, so the board boots with zone relays still latched ON.
3. `boot_fault_sources |= SAFETY_FAULT_SRC_APP` (`:602`) tells the Pico. K4 opening then
   depends entirely on SaftyFW receiving that fault or detecting the link loss (S6a/S6b).
   Firmware's own "relays come up off" invariant (`kiln_io.h`, the
   `main_boot_early.c:400-402` comment, "Relays are latched off above ... whether or not it
   succeeded") is false on this path.

Fix: in `main_boot_early.c`, before `i2c_new_master_bus()`, drive `SX1509_RESET_IO` low for
the datasheet minimum and release it, independent of I2C. Alternatively, have
`SX1509_start()` pulse `~RESET` before probing. Also consider one bounded probe retry after a
9-clock SCL bus clear.

Host test: none for this path. `test_kiln_io_sx_fake.c`/`test_kiln_io_owner*.c` start from an
expander that answers.

### LOW-1: a persistent web/OTA route failure walks a board with working control into recovery after 3 power cycles

Evidence:
- `boot_confirm_is_healthy()` requires `web_ok` (`boot_guard.c:594-598`).
- `web_ok = dashboard_http_start() == ESP_OK` (`main_network_http.c:311`).
- `dashboard_http_start()` fails if `wifi_provision_http_get_server()` is NULL
  (`dashboard_http.c:819-823`), which happens whenever `wifi_prov_start()` returns before
  `wifi_provision_http_start()` (`wifi_prov.c` around 520-640: `esp_netif_init`,
  `esp_wifi_init` NO_MEM, `esp_wifi_set_mode`, `esp_wifi_start`).
- It also fails if any one of the roughly 30 `kiln_http_register()` calls fails.

Scenario: the radio fails to initialize on every boot (a hardware fault, or a deterministic
allocation failure). Control, safety and the LCD work, but each boot leaves the counter
uncleared. The third boot hits threshold and `recovery_switch_at_boot_threshold()` reboots
into the recovery image. That image needs its own SoftAP to be exited, so the radio fault now
also takes away firing from the LCD.

This is distinct from MED-3: the failure is real and persistent, not a transient snapshot. It
is a deliberate trade-off (web/OTA reachability is what proves an update usable), hence LOW.

Fix: either document it as intended in `boot_guard.h`, or split the criterion. Keep `web_ok`
for `esp_ota_mark_app_valid_cancel_rollback()`, but let a boot that ran past boot-complete
with control, safety link and LCD up clear the counter.

Host test: `test_boot_guard.c` asserts the current truth table (web_ok false gives
`SKIP_NOT_HEALTHY`). No test covers the intended policy.

### LOW-2: boot_guard_init() runs after Wi-Fi, httpd and several NVS-reading inits, so an early deterministic crash is never counted

Evidence:
- `boot_guard_init()` is at `main_boot_early.c:416`.
- It comes after `crash_report_init()`, `estop_verification_init()`, `board_temps_start()`,
  `time_sync_start()`, `wifi_prov_start()` (`:312`, which loads Wi-Fi blobs, starts the radio
  and starts httpd serving), `mdns_init()`, the I2C bus and SX1509/kiln_io init.
- The comment at `:406` says "Nothing before this point can plausibly run long enough to trip
  either mechanism". That covers watchdogs but not panics: a panic in any of these on every
  boot loops without ever incrementing the counter.
- For a PENDING_VERIFY OTA image the bootloader rollback still catches it. For an already-valid
  image whose crash comes from persisted data (for example a malformed `saved_nets` blob in
  `wifi_prov_nvs_load_from`), nothing does.

Fix: call `boot_guard_init()` (and `recovery_switch_at_boot_threshold()`) right after
`crash_report_init()`. It needs only kiln_nvs and has no dependency on Wi-Fi or I2C. If it
must stay after the SX1509 reset so relays are dropped before a recovery reboot, move just the
counter increment earlier and keep the switch where it is.

Host test: none (ordering).

### LOW-3: load_count() collapses an NVS open/read error to count 0

Evidence: `boot_guard.c:266-300`. `hal_kv_open()` failure returns 0, as does a blob error with
no legacy record. `boot_guard_init()` then decides `recovery_mode` from 0 and persists 1.

Scenario: a kiln_nvs that fails reads on every boot but accepts writes, or one where a read
fails transiently right after a reset. This erases the counter's history, so a crash-looping
board is restarted from 0 on every such boot and may never reach threshold. A strict reader
already exists (`load_count_strict()`, used by `boot_guard_get_persisted_count()`).

Fix: in `boot_guard_init()`, distinguish "not found" (count 0) from "read error". On a read
error, do not overwrite: keep the stored record and either increment from the RTC shadow
(`s_bg_rtc`) or treat the boot as at threshold.

Host test: `test_boot_guard.c` covers missing/bad-CRC records. No test injects an open/read
error.

### INFO-1: relay and heat-enable state before control starts is otherwise sound

- `SX1509_start()` resets the expander, then `kiln_io_init()` resets again, loads latches LOW
  before setting directions, and returns relay pins to inputs on any configure failure
  (`kiln_io.c:401-445`).
- `kiln_io_set_relay_mask_locked()` refuses ON while `relay_state_unknown` or when the pin
  direction is not output.
- `heat_enable_init()` runs before any executor, and the executor and autotune are skipped in
  recovery mode with NULL-tolerant pointers (`main_control_bringup.c`).
- The UART owner/protocol failure paths call `main_kiln_enter_safe_state()`
  (`main_network_http.c:986`, `:1016`).

### INFO-2: recovery switch SET_FAILED path is bounded and leaves the counter at threshold

`recovery_switch.c:109-121` restores otadata once with no loop. The counter stays at threshold,
so the next boot retries. No issue.

### INFO-3: ota_confirm create failure is handled

A calloc or `xTaskCreate` failure (`main_network_http.c:764-799`) leaves the counter uncleared
for that boot, which is fail-toward-recovery, and it is logged. A single transient allocation
failure costs one count, not a brick.

## Test gaps

1. No test pins when the confirm may run (MED-1). Add a boot-complete input to the pure
   decision function and a host test that `CONFIRM_*` is impossible before it.
2. No fake-expander test for a probe miss on a board whose latches were left ON (MED-2). The
   fake should model a warm-restart latch state and assert that the `~RESET` GPIO is pulsed.
3. No test for `load_count()` open/read errors (LOW-3).
4. No ordering check that `boot_guard_init()` precedes Wi-Fi/httpd start (LOW-2). A
   `check_*.ps1` source-order guard like `test_flash_worker_boot_order.c` would do.
5. `monitor_task_start()` failure is not covered as a reset-loop cause anywhere (MED-1,
   scenario 1).
