# Recovery image and Pico bootloader source audit, 2026-10-02

Source-only audit at origin/main cf5cbbde. Nothing here was run on hardware.
Three audits: the `KILNCTL_RECOVERY_AP_PASSPHRASE` convention, the Pico
bootloader pre-jump watchdog, and the recovery image's passphrase and relay
hold handling.

## Findings, ranked

No high-severity defect found.

### Low

1. Negative test of the new watchdog source-scan test is NOT done. The
   permission classifier denied the edit that would have commented out the
   `watchdog_enable` line in `firmware/SaftyFW/bootloader/main.c` (reason
   "Security Weaken") and the rebuild that followed. Not retried in another
   form, and `bootloader/main.c` was never modified. The test passes on the
   real source (72/72 in the bootloader test executable) but has not been shown
   to fail when the arm is removed. Someone should run that negative test.
2. The 8000 ms bootloader pre-jump window is an estimate. No on-target
   measurement of time from jump to the application's 1000 ms re-arm exists.
3. `firmware/KilnFW_recovery/main/recovery_wifi.c:242`:
   `esp_wifi_set_storage(WIFI_STORAGE_RAM)` failure only logs a warning, so a
   failure could let the driver persist the SoftAP passphrase to NVS. Not a
   leak today (the call has no known failure mode) but nothing enforces it.
4. `firmware/KilnFW_recovery/main/recovery_io.c:321-328`:
   `recovery_io_set_lcd_pins` reads `s_data` without the lock the hold task
   uses. Minor race on a value the hold task re-verifies every cycle.
5. The recovery hold task (`relay_hold`, 3072 B stack,
   `recovery_io.c:207`/`:288`) is not covered by
   `tools/check_stack_margin_registration.ps1`: its scope is only KilnFW
   `App`, `App/drivers` and `hwAbstraction/esp` (script lines 63-66, 238-245),
   and the recovery image has no stack-margin API (comment at
   `recovery_io.c:272`). Its stack high-water mark is instead logged at
   `recovery_io.c:274` and surfaced in the recovery status JSON. Not
   measured on hardware yet (see `docs/COMPLETED_2026-10.md`).

## Audit 1: KILNCTL_RECOVERY_AP_PASSPHRASE

Mentions: `CLAUDE.md:26`, `docs/MCP_SERVERS.md:495`,
`docs/RECOVERY_IMAGE_PLAN.md:131`,
`tools/PcTools/src/kilnctrl/mcp_server_recovery.py:18`, `ROADMAP.md:96`,
`docs/COMPLETED_2026-10.md:53-54`. No code under `firmware/` or `tools/` reads
or prints it. It is a documented convention for a human or joiner automation
to supply the LCD passphrase; never a call parameter, never echoed. The last
two docs (ROADMAP, COMPLETED) were reworded in this change to say so
consistently.

## Audit 2: Pico bootloader pre-jump watchdog

- Armed before the jump: `bootloader/main.c:192`
  `watchdog_enable(BOOTLOADER_APP_WATCHDOG_MS, true)` precedes the `bx` at
  `:198-202`. `BOOTLOADER_APP_WATCHDOG_MS` is 8000 (`:174`); hardware max is
  8388 ms.
- Application re-arm: `src/main.c:320` calls
  `watchdog_enable(SAFTYFW_WATCHDOG_TIMEOUT_MS = 1000)` (`:57`) before
  `vTaskStartScheduler` (`:613`). `SaftyFW`, `SaftyFW_slotA` and `SaftyFW_slotB`
  share the same sources (`CMakeLists.txt:309`, `:552`, `:651-652`), so every
  variant re-arms. `watchdog_task.c:290-300` deliberately does not re-arm.
- State 8 (slot-linkage REJECTED, `recovery_update.c:86`, sent `:440`): cannot
  run with the watchdog armed. `main()` clears the enable bit at
  `bootloader/main.c:221` before any `enter_recovery()` (`:121-149`, no timeout)
  and `recovery_update.c` has no watchdog code.
- Test coverage: `test_bootloader_recovery_update.c` had none. Added
  `test_prejump_watchdog_sources()`, a comment-stripped source scan asserting
  arm-before-jump ordering, enable-bit clear before the first `enter_recovery()`,
  0 < 8000 <= 8388 and bootloader ms > app ms, a single app re-arm before the
  scheduler with no `watchdog_disable`, and no watchdog text in
  `recovery_update.c`. Passes; negative test blocked (finding 1).

## Audit 3: recovery image

- Passphrase never reaches `ESP_LOG`: the only log line that mentions it
  (`recovery_wifi.c:181`) omits the value. It is generated at `:199-207`, set
  on the SoftAP in `start_softap` (`:144-192`) and drawn on the LCD only
  (`recovery_lcd.c:120`, `:359`, `:462-469`).
- HTTP JSON: `recovery_http.c:393` reports only `"auth_mode":"lcd_passphrase"`.
- NVS: the only NVS key in the image is `ap_ssid` (`recovery_wifi.c:26`,
  `:147`); no passphrase write. See finding 3 on the driver's own storage.
- UART: no passphrase output path found.
- Relay hold: `recovery_io.c:220-256` writes only SAFE_DATA and DIR_VALUE
  (relays and LCD pins OFF); `recovery_hold.c` is pure logic.
- Stack margin registration: see finding 5.
