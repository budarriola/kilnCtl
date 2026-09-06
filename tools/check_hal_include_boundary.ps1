# check_hal_include_boundary.ps1 -- HAL_ABSTRACTION_PLAN.md Phase 4
# enforcement. Per that plan's "Phase 4 -- enforcement" section:
#
#   1. RATCHET mechanism (kept, currently empty). Counts every KilnFW/App
#      file (excluding test/, stubs/, build/) and SaftyFW/src file that
#      #includes a tracked header/prefix outside firmware/hwAbstraction/,
#      failing if the count for any one of them rises above the recorded
#      baseline -- same idea as check_stack_margin_baseline.ps1: a number
#      that must never go up, refreshed deliberately with -UpdateBaseline,
#      never crept upward by an unnoticed edit. As of HAL Phase 4 step 2
#      (2026-09-06) every header that was ever on this ratchet (driver/,
#      hardware/, esp_timer.h in step 1; nvs.h/nvs_flash.h in step 2) has
#      been promoted to a strict per-file allowlist below, so $RatchetHeaders
#      is empty and Test-HalRatchet always returns zero failures against it.
#      The mechanism itself is kept rather than deleted: the hwAbstraction
#      upward-scan (Invoke-HalUpwardScan) and its baseline section
#      (hwAbstractionRatchet) share the same RatchetHeaders/Test-HalRatchet
#      plumbing, and test_check_hal_include_boundary.ps1 exercises
#      Test-HalRatchet directly (with a synthetic label, not tied to any
#      currently-tracked header) to prove the comparison itself still works.
#
#   2. STRICT, for small self-contained sets that are auditable file-by-file
#      (unlike a count-only ratchet, which the plan says would be 60-75
#      decorative allowlist entries if done that way for every header):
#        - esp_ota_ops.h: exactly the files on $OtaOpsAllowlist below.
#        - esp_wifi.h / esp_netif.h: wifi_prov family only (also an
#          explicit allowlist, so a new non-wifi_prov file that reaches
#          for esp_wifi.h is caught immediately).
#        - driver/*, hardware/*, esp_timer.h: promoted from the ratchet in
#          step 1 (2026-09-06).
#        - nvs.h / nvs_flash.h: promoted from the ratchet in step 2
#          (2026-09-06) -- see $NvsAllowlist / $NvsFlashAllowlist.
#      A file that includes one of these headers and is NOT on its
#      allowlist is a hard failure, unconditionally.
#
# Mechanics are copied, not reinvented, from two existing checks:
#   - Comment stripping: check_isolation.ps1's Get-CodeOnlyLines (its
#     #include-line handling is load-bearing -- an #include inside a /*
#     */ block must not be counted, and this function already gets that
#     right, so it is reproduced here rather than re-derived).
#   - File enumeration: check_c_files_in_cmakelists.ps1's filter
#     (exclude test/, build/ -- extended here to .h as well as .c, and to
#     also exclude stubs/, which that check does not need to skip but
#     this one does since firmware/KilnFW/App/test/stubs/nvs.h is a host
#     test double, not a real firmware file).
#
# Hard floor: if the combined file count this check scans falls below
# 200, something is broken (wrong root, empty glob, moved directory) and
# this check must not report a quiet, meaningless pass.
#
# Scan roots are firmware/KilnFW/App and firmware/SaftyFW/src only.
# firmware/UnitTestFw/UnitTest/App/drivers/espInterfaces/ is deliberately
# NOT scanned: owner decision is that UnitTestFw is kept as-is (see
# project memory "Keep UnitTestFw for now", 2026-09-05) and is not part of
# the HAL boundary this check enforces -- it is a separate test-fixture
# tree, not production KilnFW/SaftyFW code.
#
# Usage:
#   powershell -File tools\check_hal_include_boundary.ps1
#   powershell -File tools\check_hal_include_boundary.ps1 -UpdateBaseline
#
# -UpdateBaseline overwrites check_hal_include_boundary_baseline.json with
# today's counts (only ever lowers the ratchet, or raises it after a
# deliberate, reviewed increase -- never run this to silence a failure you
# have not looked at).

param(
    [switch]$UpdateBaseline
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$baselinePath = Join-Path $PSScriptRoot "check_hal_include_boundary_baseline.json"

# --- Comment stripper, reproduced from firmware/SaftyFW/tools/check_isolation.ps1
# Get-CodeOnlyLines. Kept byte-for-byte equivalent in behavior: block-comment
# state carries across lines, line comments (//) are stripped, and an
# include directive survives because none of the patterns below (which match
# the widened `#\s*include` form, tolerating whitespace between `#` and
# `include`, e.g. `#  include <foo.h>`) ever appear after a // or inside a
# /* */ span in any file this check scans -- the same property
# check_isolation.ps1 relies on. No shared PowerShell module exists in this
# repo to import this from instead (see check_c_files_in_cmakelists.ps1's
# header for the same note about its own comment stripper). ---
function Get-CodeOnlyLines {
    param([string]$Path)
    $inBlockComment = $false
    $lines = Get-Content -Path $Path
    $result = @()
    foreach ($line in $lines) {
        $code = $line
        if ($inBlockComment) {
            $endIdx = $code.IndexOf("*/")
            if ($endIdx -ge 0) {
                $code = $code.Substring($endIdx + 2)
                $inBlockComment = $false
            } else {
                $result += ""
                continue
            }
        }
        $lineCommentIdx = $code.IndexOf("//")
        if ($lineCommentIdx -ge 0) {
            $code = $code.Substring(0, $lineCommentIdx)
        }
        while ($true) {
            $startIdx = $code.IndexOf("/*")
            if ($startIdx -lt 0) { break }
            $endIdx = $code.IndexOf("*/", $startIdx)
            if ($endIdx -ge 0) {
                $code = $code.Substring(0, $startIdx) + $code.Substring($endIdx + 2)
            } else {
                $code = $code.Substring(0, $startIdx)
                $inBlockComment = $true
                break
            }
        }
        $result += $code
    }
    return $result
}

# --- File enumeration, extending check_c_files_in_cmakelists.ps1's filter
# to .h as well as .c, and adding stubs/ to the excluded directory names
# (a host-test double, not a real firmware file). Returns repo-root-relative
# paths, forward slashes only. ---
function Get-ScanFiles {
    param([string]$Dir, [string]$RepoRoot)
    if (-not (Test-Path $Dir)) { return @() }
    Get-ChildItem -Path $Dir -Recurse -File |
        Where-Object {
            ($_.Extension -ieq ".c" -or $_.Extension -ieq ".h") -and
            $_.FullName -notmatch '[\\/]test[\\/]' -and
            $_.FullName -notmatch '[\\/]stubs[\\/]' -and
            $_.FullName -notmatch '[\\/]build[\\/]'
        } |
        ForEach-Object {
            ($_.FullName.Substring($RepoRoot.Length + 1) -replace '\\', '/')
        } |
        Sort-Object
}

# --- The count-ratchet is now empty: nvs.h/nvs_flash.h (the last two labels
# on it) were promoted to strict per-file allowlists below (HAL Phase 4
# enforcement, step 2, docs/HW_ABSTRACTION_PLAN.md "Phase 4 -- enforcement").
# The mechanism is kept (rather than deleted) because the hwAbstraction
# upward-scan (Invoke-HalUpwardScan) and the negative test both still call
# Test-HalRatchet against RatchetHeaders/RatchetCounts, and the baseline file
# keeps an (now-empty) top-level object plus a populated hwAbstractionRatchet
# section for the same two headers, tracking KV owner code inside
# firmware/hwAbstraction/. ---
$RatchetHeaders = [ordered]@{
}

# --- Strict, per-file allowlists. Full repo-relative paths only -- never a
# directory prefix (SaftyFW has no espInterfaces/-style subdirectory to
# anchor one, and KilnFW's hwAbstraction/ boundary is handled separately
# below by simply excluding that tree from the scan, same as the ratchet
# does). Each entry names the header it is allowlisted for, so a file that
# is allowlisted for esp_wifi.h but somehow also picked up esp_ota_ops.h
# still fails on the latter. ---

# esp_ota_ops.h -- measured 2026-09-06 against the current tree: 6 files.
# dashboard_http.c and partition_info_http.c were allowlisted here as of
# 2026-09-05 (8 files) but the hal_sysinfo migration moved both onto
# hal_sysinfo_get_running_partition()/_get_build_info()/_reset_reason()
# instead, so neither includes esp_ota_ops.h any more -- entries removed.
$OtaOpsAllowlist = @(
    @{ RelPath = "firmware/KilnFW/App/drivers/http/ota_http.c";            Header = "esp_ota_ops.h"; Reason = "OTA HTTP handler orchestration -- direct partition/OTA-write owner"; ExpiresAtPhase = "n/a (out of scope: OTA partition writes, see plan)" }
    @{ RelPath = "firmware/KilnFW/App/drivers/http/ota_http_esp.c";        Header = "esp_ota_ops.h"; Reason = "ESP-side OTA write/verify/set-boot-partition implementation"; ExpiresAtPhase = "n/a (out of scope: OTA partition writes, see plan)" }
    @{ RelPath = "firmware/KilnFW/App/drivers/http/ota_http_pico.c";       Header = "esp_ota_ops.h"; Reason = "relays an OTA image to the Pico; still touches the ESP-side esp_ota_ops API for its own state"; ExpiresAtPhase = "n/a (out of scope: OTA partition writes, see plan)" }
    @{ RelPath = "firmware/KilnFW/App/drivers/http/ota_http_recovery.c";   Header = "esp_ota_ops.h"; Reason = "recovery-mode OTA rollback path -- reads/sets boot partition"; ExpiresAtPhase = "n/a (out of scope: OTA partition writes, see plan)" }
    @{ RelPath = "firmware/KilnFW/App/drivers/ui/ui_page_diagnostics.c"; Header = "esp_ota_ops.h"; Reason = "diagnostics LCD page displays running partition/build info"; ExpiresAtPhase = "n/a (out of scope: OTA partition writes, see plan)" }
    @{ RelPath = "firmware/KilnFW/App/main_network_http.c";           Header = "esp_ota_ops.h"; Reason = "wires the OTA HTTP handlers into the httpd instance at boot"; ExpiresAtPhase = "n/a (out of scope: OTA partition writes, see plan)" }
)

# esp_wifi.h / esp_netif.h -- wifi_prov family only, per the plan
# ("Deliberately out of scope: ... wifi_prov is already a sole owner").
$WifiAllowlist = @(
    @{ RelPath = "firmware/KilnFW/App/drivers/net/wifi_prov.c";          Header = "esp_wifi.h";  Reason = "wifi_prov family -- sole Wi-Fi driver owner"; ExpiresAtPhase = "n/a (out of scope: Wi-Fi portability, see plan)" }
    @{ RelPath = "firmware/KilnFW/App/drivers/net/wifi_prov.c";          Header = "esp_netif.h"; Reason = "wifi_prov family -- sole Wi-Fi driver owner"; ExpiresAtPhase = "n/a (out of scope: Wi-Fi portability, see plan)" }
    @{ RelPath = "firmware/KilnFW/App/drivers/net/wifi_prov_api.c";      Header = "esp_wifi.h";  Reason = "wifi_prov family"; ExpiresAtPhase = "n/a (out of scope: Wi-Fi portability, see plan)" }
    @{ RelPath = "firmware/KilnFW/App/drivers/net/wifi_prov_link.c";     Header = "esp_wifi.h";  Reason = "wifi_prov family"; ExpiresAtPhase = "n/a (out of scope: Wi-Fi portability, see plan)" }
    @{ RelPath = "firmware/KilnFW/App/drivers/net/wifi_prov_internal.h"; Header = "esp_netif.h"; Reason = "wifi_prov family -- shared internal header"; ExpiresAtPhase = "n/a (out of scope: Wi-Fi portability, see plan)" }
)


# driver/*.h -- promoted from the count-ratchet to a strict per-file, per-
# specific-header allowlist (HAL Phase 4 enforcement, step 1,
# docs/HW_ABSTRACTION_PLAN.md "Phase 4 -- enforcement"). Every site below is
# a documented legitimate pin/type-only holdout, not a TEMPORARY placeholder
# -- none of today's driver/* sites carry a `// TEMPORARY` marker in the
# plan, so every entry's ExpiresAtPhase is "n/a", citing the plan section
# that accepts it as out of scope for the HAL boundary.
$DriverGpioAllowlist = @(
    @{ RelPath = "firmware/KilnFW/App/drivers/bridge/uart_bridge.c";        Header = "driver/gpio.h"; Reason = "UART bridge diagnostic/scope-marker GPIO toggling, bridge-layer instrumentation predating the GPIO HAL"; ExpiresAtPhase = "n/a (pin/type-only holdout, see plan)" }
    @{ RelPath = "firmware/KilnFW/App/drivers/bridge/uart_bridge_info.c";   Header = "driver/gpio.h"; Reason = "UART bridge diagnostic/scope-marker GPIO toggling, bridge-layer instrumentation predating the GPIO HAL"; ExpiresAtPhase = "n/a (pin/type-only holdout, see plan)" }
    @{ RelPath = "firmware/KilnFW/App/drivers/bridge/uart_bridge_io.c";     Header = "driver/gpio.h"; Reason = "one of the two raw IRQ owners (SX1509 ~INT via gpio_isr_handler_add, per-pin) named explicitly in the plan's Phase 4 expected-final-entries list"; ExpiresAtPhase = "n/a (out of scope: raw IRQ owner, see plan)" }
    @{ RelPath = "firmware/KilnFW/App/drivers/bridge/uart_bridge_safety.c"; Header = "driver/gpio.h"; Reason = "UART bridge diagnostic/scope-marker GPIO toggling, bridge-layer instrumentation predating the GPIO HAL"; ExpiresAtPhase = "n/a (pin/type-only holdout, see plan)" }
    @{ RelPath = "firmware/KilnFW/App/drivers/bridge/uart_bridge_system.c"; Header = "driver/gpio.h"; Reason = "UART bridge diagnostic/scope-marker GPIO toggling, bridge-layer instrumentation predating the GPIO HAL"; ExpiresAtPhase = "n/a (pin/type-only holdout, see plan)" }
    @{ RelPath = "firmware/KilnFW/App/drivers/bridge/uart_bridge_thermo.c"; Header = "driver/gpio.h"; Reason = "UART bridge diagnostic/scope-marker GPIO toggling, bridge-layer instrumentation predating the GPIO HAL"; ExpiresAtPhase = "n/a (pin/type-only holdout, see plan)" }
    @{ RelPath = "firmware/KilnFW/App/drivers/bridge/uart_bridge_touch.c";  Header = "driver/gpio.h"; Reason = "UART bridge diagnostic/scope-marker GPIO toggling, bridge-layer instrumentation predating the GPIO HAL"; ExpiresAtPhase = "n/a (pin/type-only holdout, see plan)" }
    @{ RelPath = "firmware/KilnFW/App/drivers/bridge/uart_bridge_ui_test.c";Header = "driver/gpio.h"; Reason = "UART bridge diagnostic/scope-marker GPIO toggling, bridge-layer instrumentation predating the GPIO HAL"; ExpiresAtPhase = "n/a (pin/type-only holdout, see plan)" }
    @{ RelPath = "firmware/KilnFW/App/drivers/hw/MAX31856.c";               Header = "driver/gpio.h"; Reason = "CS-line bit-bang for the MAX31856 SPI transfer sequence -- pin-level, not a HAL abstraction candidate"; ExpiresAtPhase = "n/a (pin/type-only holdout, see plan)" }
    @{ RelPath = "firmware/KilnFW/App/drivers/hw/panel_spi.c";              Header = "driver/gpio.h"; Reason = "panel CS/reset bit-bang for the ST7796 SPI sequence -- pin-level, not a HAL abstraction candidate"; ExpiresAtPhase = "n/a (pin/type-only holdout, see plan)" }
    @{ RelPath = "firmware/KilnFW/App/drivers/hw/settings.h";               Header = "driver/gpio.h"; Reason = "shared pin/type declarations header for the hw/ family -- type-only holdout"; ExpiresAtPhase = "n/a (pin/type-only holdout, see plan)" }
    @{ RelPath = "firmware/KilnFW/App/drivers/hw/SX1509.c";                 Header = "driver/gpio.h"; Reason = "SX1509 ~INT pin configured as polled input; owned/read by uart_bridge_io.c's raw IRQ path per the plan"; ExpiresAtPhase = "n/a (pin/type-only holdout, see plan)" }
    @{ RelPath = "firmware/KilnFW/App/drivers/safety/safety_link.c";        Header = "driver/gpio.h"; Reason = "safety link handshake GPIO line, pin-level control alongside the UART driver it pairs with"; ExpiresAtPhase = "n/a (pin/type-only holdout, see plan)" }
    @{ RelPath = "firmware/KilnFW/App/drivers/safety/safety_link_commands.c"; Header = "driver/gpio.h"; Reason = "safety link handshake GPIO line, pin-level control alongside the UART driver it pairs with"; ExpiresAtPhase = "n/a (pin/type-only holdout, see plan)" }
    @{ RelPath = "firmware/KilnFW/App/drivers/safety/safety_link_frames.c"; Header = "driver/gpio.h"; Reason = "safety link handshake GPIO line, pin-level control alongside the UART driver it pairs with"; ExpiresAtPhase = "n/a (pin/type-only holdout, see plan)" }
    @{ RelPath = "firmware/KilnFW/App/drivers/safety/safety_link_inbox.c";  Header = "driver/gpio.h"; Reason = "safety link handshake GPIO line, pin-level control alongside the UART driver it pairs with"; ExpiresAtPhase = "n/a (pin/type-only holdout, see plan)" }
    @{ RelPath = "firmware/KilnFW/App/drivers/safety/safety_link_payload.c"; Header = "driver/gpio.h"; Reason = "safety link handshake GPIO line, pin-level control alongside the UART driver it pairs with"; ExpiresAtPhase = "n/a (pin/type-only holdout, see plan)" }
    @{ RelPath = "firmware/KilnFW/App/drivers/safety/safety_link_poll.c";   Header = "driver/gpio.h"; Reason = "safety link handshake GPIO line, pin-level control alongside the UART driver it pairs with"; ExpiresAtPhase = "n/a (pin/type-only holdout, see plan)" }
)

$DriverI2cAllowlist = @(
    @{ RelPath = "firmware/KilnFW/App/drivers/hw/NS2009.h";     Header = "driver/i2c_master.h"; Reason = "legacy resistive touch controller type-only holdout (bus handle type), superseded path retained per PROJECT_STATUS.md"; ExpiresAtPhase = "n/a (pin/type-only holdout, see plan)" }
    @{ RelPath = "firmware/KilnFW/App/drivers/hw/SX1509.c";     Header = "driver/i2c_master.h"; Reason = "SX1509 I/O expander I2C transfers -- direct bus owner"; ExpiresAtPhase = "n/a (pin/type-only holdout, see plan)" }
    @{ RelPath = "firmware/KilnFW/App/drivers/hw/SX1509.h";     Header = "driver/i2c_master.h"; Reason = "SX1509 I/O expander I2C bus handle type declaration"; ExpiresAtPhase = "n/a (pin/type-only holdout, see plan)" }
    @{ RelPath = "firmware/KilnFW/App/drivers/hw/i2c_scan.h";   Header = "driver/i2c_master.h"; Reason = "diagnostic I2C bus scan utility -- direct bus owner by design"; ExpiresAtPhase = "n/a (pin/type-only holdout, see plan)" }
    @{ RelPath = "firmware/KilnFW/App/drivers/hw/panel_spi.h";  Header = "driver/i2c_master.h"; Reason = "shares the hw/ header's bus handle type declarations"; ExpiresAtPhase = "n/a (pin/type-only holdout, see plan)" }
    @{ RelPath = "firmware/KilnFW/App/main_boot_early.c";       Header = "driver/i2c_master.h"; Reason = "bus init named explicitly in the plan's Phase 4 expected-final-entries list (main_boot_early.c: bus init)"; ExpiresAtPhase = "n/a (out of scope: bus init, see plan)" }
    @{ RelPath = "firmware/KilnFW/App/main_internal.h";         Header = "driver/i2c_master.h"; Reason = "shared internal header for main_boot_early.c's bus-init bus handle types"; ExpiresAtPhase = "n/a (out of scope: bus init, see plan)" }
)

$DriverSpiAllowlist = @(
    @{ RelPath = "firmware/KilnFW/App/drivers/hw/MAX31856.h";  Header = "driver/spi_master.h"; Reason = "spi_host_device_t type only -- transfers go through hal_spi.h now, per the header's own comment"; ExpiresAtPhase = "n/a (pin/type-only holdout, see plan)" }
    @{ RelPath = "firmware/KilnFW/App/drivers/hw/panel_spi.h";  Header = "driver/spi_master.h"; Reason = "panel SPI bus handle/type declarations"; ExpiresAtPhase = "n/a (pin/type-only holdout, see plan)" }
    @{ RelPath = "firmware/KilnFW/App/drivers/hw/settings.h";   Header = "driver/spi_master.h"; Reason = "shared pin/type declarations header for the hw/ family -- type-only holdout"; ExpiresAtPhase = "n/a (pin/type-only holdout, see plan)" }
    @{ RelPath = "firmware/KilnFW/App/main_boot_early.c";       Header = "driver/spi_master.h"; Reason = "bus init named explicitly in the plan's Phase 4 expected-final-entries list (main_boot_early.c: bus init)"; ExpiresAtPhase = "n/a (out of scope: bus init, see plan)" }
)

$DriverUartAllowlist = @(
    @{ RelPath = "firmware/KilnFW/App/drivers/hw/settings.h";               Header = "driver/uart.h"; Reason = "shared pin/type declarations header for the hw/ family -- type-only holdout"; ExpiresAtPhase = "n/a (pin/type-only holdout, see plan)" }
    @{ RelPath = "firmware/KilnFW/App/drivers/safety/safety_link.c";        Header = "driver/uart.h"; Reason = "safety link's direct UART driver owner -- the hardened link protocol needs the raw UART API"; ExpiresAtPhase = "n/a (pin/type-only holdout, see plan)" }
    @{ RelPath = "firmware/KilnFW/App/drivers/safety/safety_link_commands.c"; Header = "driver/uart.h"; Reason = "safety link's direct UART driver owner -- the hardened link protocol needs the raw UART API"; ExpiresAtPhase = "n/a (pin/type-only holdout, see plan)" }
    @{ RelPath = "firmware/KilnFW/App/drivers/safety/safety_link_frames.c"; Header = "driver/uart.h"; Reason = "safety link's direct UART driver owner -- the hardened link protocol needs the raw UART API"; ExpiresAtPhase = "n/a (pin/type-only holdout, see plan)" }
    @{ RelPath = "firmware/KilnFW/App/drivers/safety/safety_link_inbox.c";  Header = "driver/uart.h"; Reason = "safety link's direct UART driver owner -- the hardened link protocol needs the raw UART API"; ExpiresAtPhase = "n/a (pin/type-only holdout, see plan)" }
    @{ RelPath = "firmware/KilnFW/App/drivers/safety/safety_link_payload.c"; Header = "driver/uart.h"; Reason = "safety link's direct UART driver owner -- the hardened link protocol needs the raw UART API"; ExpiresAtPhase = "n/a (pin/type-only holdout, see plan)" }
    @{ RelPath = "firmware/KilnFW/App/drivers/safety/safety_link_poll.c";   Header = "driver/uart.h"; Reason = "safety link's direct UART driver owner -- the hardened link protocol needs the raw UART API"; ExpiresAtPhase = "n/a (pin/type-only holdout, see plan)" }
)

$DriverTempSensorAllowlist = @(
    # board_temps.c migrated to hal_sysinfo_temp_* 2026-09-06 (HW_ABSTRACTION_PLAN.md
    # "hal_time / hal_wdt / hal_pwm / hal_sysinfo" item 4) -- no remaining
    # driver/temperature_sensor.h include outside firmware/hwAbstraction/.
)

# hardware/*.h (SaftyFW/RP2040 pico-sdk) -- same promotion, one entry per
# file (the ratchet counted "does this file include anything under
# hardware/" regardless of which specific pico-sdk header, so the allowlist
# stays at that granularity rather than splintering by sub-header).
$HardwareAllowlist = @(
    @{ RelPath = "firmware/SaftyFW/src/main.c";                   Header = "hardware/"; Reason = "pico-sdk GPIO/timer/watchdog bring-up at boot (hardware/gpio.h: GPIO6-low, the literal first statement of main(), deliberately raw; hardware/regs/timer.h, hardware/timer.h: TIMER_DBGPAUSE_BITS/timer_hw->dbgpause register, no HAL primitive covers this; hardware/watchdog.h: watchdog_caused_reboot()/watchdog_enable_caused_reboot() boot-reason reads and the raw watchdog_hw->scratch[5] write in vApplicationStackOverflowHook(), neither expressible through hal_wdt.h/hal_scratch.h) -- SaftyFW hardware/* holdout named in the plan"; ExpiresAtPhase = "n/a (pin/type-only holdout, see plan)" }
    @{ RelPath = "firmware/SaftyFW/src/tasks/console_uart.c";      Header = "hardware/"; Reason = "write-only diagnostic UART/GPIO, no IRQ -- named explicitly in the plan's Phase 4 expected-final-entries list"; ExpiresAtPhase = "n/a (out of scope: console_uart.c, see plan)" }
    @{ RelPath = "firmware/SaftyFW/src/tasks/thermo_task.c";       Header = "hardware/"; Reason = "one of the two raw IRQ owners (DRDY via gpio_set_irq_enabled_with_callback, shared) named explicitly in the plan's Phase 4 expected-final-entries list"; ExpiresAtPhase = "n/a (out of scope: raw IRQ owner, see plan)" }
)

# esp_timer.h -- promoted from the count-ratchet to a strict per-file
# allowlist. All seven sites use esp_timer for periodic/one-shot callback
# scheduling (LVGL tick, HTTP/OTA/wifi timing, safety link timing) with no
# cross-target HAL timer abstraction defined by this plan (deliberately out
# of scope: see "Deliberately out of scope" -- timer portability is not
# listed as an in-scope HAL surface).
$EspTimerAllowlist = @(
    @{ RelPath = "firmware/KilnFW/App/drivers/net/wifi_prov.c";            Header = "esp_timer.h"; Reason = "wifi_prov family -- sole Wi-Fi driver owner, already exempt for esp_wifi.h/esp_netif.h; esp_timer_create/esp_timer_start_periodic for the AP-fallback/rescan timer objects only, no timestamp reads"; ExpiresAtPhase = "n/a (out of scope: Wi-Fi portability, see plan)" }
    @{ RelPath = "firmware/KilnFW/App/drivers/net/wifi_prov_internal.h";   Header = "esp_timer.h"; Reason = "wifi_prov family -- shared internal header (esp_timer_handle_t fields only)"; ExpiresAtPhase = "n/a (out of scope: Wi-Fi portability, see plan)" }
    @{ RelPath = "firmware/KilnFW/App/drivers/ui/lvgl_port.c";             Header = "esp_timer.h"; Reason = "esp_timer_create/esp_timer_start_periodic for the 1ms lv_tick callback -- named explicitly in the plan's Phase 4 expected-final-entries list (lvgl_port)"; ExpiresAtPhase = "n/a (out of scope: lvgl_port, see plan)" }
)

# nvs.h / nvs_flash.h -- promoted from the count-ratchet to strict per-file
# allowlists (HAL Phase 4 enforcement, step 2, docs/HW_ABSTRACTION_PLAN.md
# "Phase 4 -- enforcement"). Measured 2026-09-06 against the current tree
# (production files only -- firmware/KilnFW/App/test/test_profiles_http.c
# also includes nvs.h but is excluded from the scan by Get-ScanFiles's
# test/ filter, and firmware/hwAbstraction/esp/kv/hal_kv_esp.c is the actual
# KV-owner backend, excluded from strict checks entirely by Test-InsideHal):
# exactly one nvs.h site and three nvs_flash.h sites remain, each for a
# single named constant with no nvs_*()/nvs_flash_*() function call.
$NvsAllowlist = @(
    @{ RelPath = "firmware/KilnFW/App/drivers/persist/ota_record.c"; Header = "nvs.h"; Reason = "ESP_ERR_NVS_NOT_FOUND only, per ota_record.h's doc contract -- no nvs_*()/nvs_flash_*() function call in this file, actual NVS access goes through hal_kv"; ExpiresAtPhase = "n/a (constant-only holdout, see plan)" }
)

$NvsFlashAllowlist = @(
    @{ RelPath = "firmware/KilnFW/App/drivers/net/wifi_prov.c";              Header = "nvs_flash.h"; Reason = "NVS_DEFAULT_PART_NAME only -- HAL Phase 3 item 3, actual NVS access goes through hal_kv"; ExpiresAtPhase = "n/a (constant-only holdout, see plan)" }
    @{ RelPath = "firmware/KilnFW/App/drivers/net/wifi_prov_nvs.c";          Header = "nvs_flash.h"; Reason = "NVS_DEFAULT_PART_NAME only -- see wifi_prov.c's holdout, actual NVS access goes through hal_kv"; ExpiresAtPhase = "n/a (constant-only holdout, see plan)" }
    @{ RelPath = "firmware/KilnFW/App/drivers/persist/zones_config_store.c"; Header = "nvs_flash.h"; Reason = "NVS_DEFAULT_PART_NAME only -- see wifi_prov_nvs.c's holdout, actual NVS access goes through hal_kv"; ExpiresAtPhase = "n/a (constant-only holdout, see plan)" }
)

$StrictHeaders = [ordered]@{
    "nvs.h"                      = @{ Pattern = '^\s*#\s*include\s*["<]nvs\.h[">]';                    Allowlist = $NvsAllowlist }
    "nvs_flash.h"                = @{ Pattern = '^\s*#\s*include\s*["<]nvs_flash\.h[">]';               Allowlist = $NvsFlashAllowlist }
    "esp_ota_ops.h"              = @{ Pattern = '^\s*#\s*include\s*["<]esp_ota_ops\.h[">]';            Allowlist = $OtaOpsAllowlist }
    "esp_wifi.h"                 = @{ Pattern = '^\s*#\s*include\s*["<]esp_wifi\.h[">]';                Allowlist = $WifiAllowlist }
    "esp_netif.h"                = @{ Pattern = '^\s*#\s*include\s*["<]esp_netif\.h[">]';               Allowlist = $WifiAllowlist }
    "driver/gpio.h"               = @{ Pattern = '^\s*#\s*include\s*["<]driver/gpio\.h[">]';             Allowlist = $DriverGpioAllowlist }
    "driver/i2c_master.h"         = @{ Pattern = '^\s*#\s*include\s*["<]driver/i2c_master\.h[">]';       Allowlist = $DriverI2cAllowlist }
    "driver/spi_master.h"         = @{ Pattern = '^\s*#\s*include\s*["<]driver/spi_master\.h[">]';       Allowlist = $DriverSpiAllowlist }
    "driver/uart.h"                = @{ Pattern = '^\s*#\s*include\s*["<]driver/uart\.h[">]';            Allowlist = $DriverUartAllowlist }
    "driver/temperature_sensor.h" = @{ Pattern = '^\s*#\s*include\s*["<]driver/temperature_sensor\.h[">]'; Allowlist = $DriverTempSensorAllowlist }
    "hardware/"                   = @{ Pattern = '^\s*#\s*include\s*["<]hardware/';                      Allowlist = $HardwareAllowlist }
    "esp_timer.h"                 = @{ Pattern = '^\s*#\s*include\s*["<]esp_timer\.h[">]';               Allowlist = $EspTimerAllowlist }
}

# --- Stage 3: firmware/hwAbstraction/** upward-include boundary. ---
#
# The two stages above deliberately exclude firmware/hwAbstraction/ entirely
# (Test-InsideHal). That exclusion is correct for the ratchet's *purpose*
# (catching KilnFW/SaftyFW app code that reaches for vendor headers instead
# of going through the HAL) but it also means nobody was checking the HAL
# boundary's OTHER direction: hwAbstraction code reaching back UP into
# KilnFW/SaftyFW.
#
# CLOSED 2026-09-06 (HAL Phase 1b, docs/HW_ABSTRACTION_PLAN.md): the four
# pico-side sites that used to #include "board_pins.h" straight across this
# boundary --
#   - firmware/hwAbstraction/pico/spi/spi_owner.c
#   - firmware/hwAbstraction/pico/spi/hal_spi_pico.c
#   - firmware/hwAbstraction/pico/uart/uart_owner.c
#   - firmware/hwAbstraction/pico/uart/hal_uart_pico.c
# now take their pin values as init-time parameters instead
# (spi_owner_pins_t/uart_owner_pins_t, passed by SaftyFW's own callers --
# main.c and max31856.c -- which still read board_pins.h themselves). None of
# the four includes board_pins.h any more, so they are off the allowlist
# below; SaftyFW/CMakeLists.txt's hwabstraction_pico PRIVATE include dir on
# src/board has also been removed (nothing in the library needs it any more).
#
# Two files still legitimately include stack_margin.h as pre-existing,
# ACCEPTED (not temporary) behavior -- they are the same owner modules
# check_stack_margin_registration.ps1 documents as relocated byte-identical
# by the move and already calling stack_margin_register() directly:
#   - firmware/hwAbstraction/esp/i2c/i2c_owner.c
#   - firmware/hwAbstraction/esp/spi/esp_spi_owner.c
# The allowlist below is per-file/per-header, same shape as $OtaOpsAllowlist
# above, and is STRICT (any other hwAbstraction file including the header is
# a hard failure, not a ratchet).
$HalUpwardAllowlist = @(
    @{ RelPath = "firmware/hwAbstraction/esp/i2c/i2c_owner.c";      Header = "stack_margin.h"; Reason = "pre-existing owner module relocated byte-identical by HAL Phase 1a; already calls stack_margin_register() directly (accepted, not temporary -- see check_stack_margin_registration.ps1)" }
    @{ RelPath = "firmware/hwAbstraction/esp/spi/esp_spi_owner.c";  Header = "stack_margin.h"; Reason = "pre-existing owner module relocated byte-identical by HAL Phase 1a; already calls stack_margin_register() directly (accepted, not temporary -- see check_stack_margin_registration.ps1)" }
)

$HalUpwardHeaders = [ordered]@{
    "board_pins.h"   = '^\s*#\s*include\s*["<]board_pins\.h[">]'
    "stack_margin.h" = '^\s*#\s*include\s*["<]stack_margin\.h[">]'
}

# Any #include of a relative path that climbs out of firmware/hwAbstraction/
# into ../../KilnFW/App or ../../SaftyFW/src (or similar) is always a hard
# failure -- no allowlist, since none of today's known TEMPORARY sites use
# this form (they rely on a private include-dir, not a relative path).
$HalUpwardPathPattern = '^\s*#\s*include\s*"\.\./.*(App[/\\]drivers|SaftyFW[/\\]src)'

function Get-HalScanFiles {
    param([string]$Dir, [string]$RepoRoot)
    if (-not (Test-Path $Dir)) { return @() }
    Get-ChildItem -Path $Dir -Recurse -File |
        Where-Object {
            ($_.Extension -ieq ".c" -or $_.Extension -ieq ".h") -and
            $_.FullName -notmatch '[\\/]test[\\/]'
        } |
        ForEach-Object {
            ($_.FullName.Substring($RepoRoot.Length + 1) -replace '\\', '/')
        } |
        Sort-Object
}

function Invoke-HalUpwardScan {
    param(
        [string[]]$RelPaths,
        [string]$FileRoot
    )

    $ratchetCounts = [ordered]@{}
    foreach ($label in $RatchetHeaders.Keys) { $ratchetCounts[$label] = 0 }
    $strictViolations = @()

    foreach ($rel in $RelPaths) {
        $full = Join-Path $FileRoot ($rel -replace '/', '\')
        if (-not (Test-Path $full)) { continue }
        $codeLines = Get-CodeOnlyLines -Path $full

        # Ratchet: honest accounting of vendor/App headers used by
        # hwAbstraction itself (mostly the relocated owner modules --
        # this is what a2a1300 stopped counting when it excluded
        # firmware/hwAbstraction/ from the main scan).
        foreach ($label in $RatchetHeaders.Keys) {
            $pattern = $RatchetHeaders[$label]
            foreach ($line in $codeLines) {
                if ($line -match $pattern) { $ratchetCounts[$label] += 1; break }
            }
        }

        # Strict: upward includes into KilnFW/SaftyFW proper.
        foreach ($headerName in $HalUpwardHeaders.Keys) {
            $pattern = $HalUpwardHeaders[$headerName]
            $hit = $false
            foreach ($line in $codeLines) {
                if ($line -match $pattern) { $hit = $true; break }
            }
            if (-not $hit) { continue }
            $allowed = $false
            foreach ($a in $HalUpwardAllowlist) {
                if ($a.RelPath -ieq $rel -and $a.Header -ieq $headerName) { $allowed = $true; break }
            }
            if (-not $allowed) {
                $strictViolations += "${rel}: includes $headerName (upward into KilnFW/SaftyFW) and is not on the hwAbstraction upward-include allowlist"
            }
        }

        foreach ($line in $codeLines) {
            if ($line -match $HalUpwardPathPattern) {
                $strictViolations += "${rel}: includes a relative path that climbs into App/drivers or SaftyFW/src -- hwAbstraction must not reach upward by relative path"
                break
            }
        }
    }

    return @{ RatchetCounts = $ratchetCounts; StrictViolations = $strictViolations }
}

# Excludes firmware/hwAbstraction/ (the boundary this check exists to
# enforce -- HAL backend code is exempt by definition) from a repo-relative
# path.
function Test-InsideHal {
    param([string]$RelPath)
    return ($RelPath -match '(^|/)firmware/hwAbstraction/')
}

# --- Core scan function. Takes a list of repo-relative file paths (so the
# negative test below can hand it a synthetic path list without touching
# the real tree) and returns a hashtable:
#   RatchetCounts   = @{ "driver/" = <int>; ... }
#   StrictViolations = @( "relpath: header not allowlisted", ... )
# $FileRoot is the absolute directory those relative paths are rooted at
# (defaults to the repo root; the negative test overrides it to point at
# the scratch copy). ---
function Invoke-HalBoundaryScan {
    param(
        [string[]]$RelPaths,
        [string]$FileRoot
    )

    $ratchetCounts = [ordered]@{}
    foreach ($label in $RatchetHeaders.Keys) { $ratchetCounts[$label] = 0 }
    $strictViolations = @()
    $skippedFiles = @()

    foreach ($rel in $RelPaths) {
        if (Test-InsideHal -RelPath $rel) { continue }

        $full = Join-Path $FileRoot ($rel -replace '/', '\')
        if (-not (Test-Path $full)) { $skippedFiles += $rel; continue }
        $codeLines = Get-CodeOnlyLines -Path $full

        foreach ($label in $RatchetHeaders.Keys) {
            $pattern = $RatchetHeaders[$label]
            $hit = $false
            foreach ($line in $codeLines) {
                if ($line -match $pattern) { $hit = $true; break }
            }
            if ($hit) { $ratchetCounts[$label] += 1 }
        }

        foreach ($headerName in $StrictHeaders.Keys) {
            $entry = $StrictHeaders[$headerName]
            $hit = $false
            foreach ($line in $codeLines) {
                if ($line -match $entry.Pattern) { $hit = $true; break }
            }
            if (-not $hit) { continue }
            $allowed = $false
            foreach ($a in $entry.Allowlist) {
                if ($a.RelPath -ieq $rel -and $a.Header -ieq $headerName) { $allowed = $true; break }
            }
            if (-not $allowed) {
                $strictViolations += "${rel}: includes $headerName outside hwAbstraction/ and is not on its allowlist -- add a { RelPath; Header = `"$headerName`"; Reason; ExpiresAtPhase } entry to the matching allowlist array in tools/check_hal_include_boundary.ps1, after review, or move the include behind the HAL"
            }
        }
    }

    return @{ RatchetCounts = $ratchetCounts; StrictViolations = $strictViolations; SkippedFiles = $skippedFiles }
}

# --- The ratchet comparison itself, extracted so the negative test can call
# the SAME logic the main body below uses (instead of reimplementing it
# inline, which would only prove the test's own copy can fail, not the
# production comparison). $Counts is a ratchet-counts hashtable/ordered-dict
# (label -> int, e.g. Invoke-HalBoundaryScan's .RatchetCounts). $Baseline is
# whatever ConvertFrom-Json produced from the baseline file (a PSCustomObject
# with one property per label) -- or, for the negative test, a synthetic
# hashtable/PSCustomObject built the same shape. Returns an array of
# human-readable violation strings; empty means no ratchet failure. ---
function Test-HalRatchet {
    param(
        $Counts,
        $Baseline
    )
    $failures = @()
    foreach ($label in $Counts.Keys) {
        $baselineVal = $Baseline.$label
        if ($null -eq $baselineVal) {
            $failures += "$label : no baseline entry recorded (baseline file is missing this key -- run -UpdateBaseline after reviewing)"
            continue
        }
        if ($Counts[$label] -gt [int]$baselineVal) {
            $failures += "$label : count rose to $($Counts[$label]) (baseline $baselineVal) -- a file outside firmware/hwAbstraction/ started including $label. Move it behind the HAL, or if this is a deliberate, reviewed increase, rerun with -UpdateBaseline."
        }
    }
    return $failures
}

# ---------------------------------------------------------------------
# Real-tree run starts here. Everything above is reusable by the negative
# test (dot-sourced).
# ---------------------------------------------------------------------

if ($MyInvocation.InvocationName -eq '.') {
    # Dot-sourced by the negative test -- define functions/data only, run
    # nothing else.
    return
}

$appDir = Join-Path $repoRoot "firmware\KilnFW\App"
$saftySrcDir = Join-Path $repoRoot "firmware\SaftyFW\src"

if (-not (Test-Path $appDir)) { throw "check_hal_include_boundary: $appDir not found -- has it moved?" }
if (-not (Test-Path $saftySrcDir)) { throw "check_hal_include_boundary: $saftySrcDir not found -- has it moved?" }

$repoRootResolved = (Resolve-Path $repoRoot).Path
$allFiles = @()
$allFiles += Get-ScanFiles -Dir (Resolve-Path $appDir).Path -RepoRoot $repoRootResolved
$allFiles += Get-ScanFiles -Dir (Resolve-Path $saftySrcDir).Path -RepoRoot $repoRootResolved

if ($allFiles.Count -lt 200) {
    throw "check_hal_include_boundary: only found $($allFiles.Count) candidate files -- expected 200+. The glob is probably broken or a directory moved; this check has gone blind, not found a shrunk tree."
}

$scan = Invoke-HalBoundaryScan -RelPaths $allFiles -FileRoot $repoRootResolved
$counts = $scan.RatchetCounts
$violations = $scan.StrictViolations

if ($scan.SkippedFiles.Count -gt 0) {
    throw "check_hal_include_boundary: $($scan.SkippedFiles.Count) file(s) enumerated by Get-ScanFiles could not be found/read during the scan (Test-Path failed) -- this check must not silently skip real files: $($scan.SkippedFiles -join ', ')"
}

# --- Baseline load / update ---
if (-not (Test-Path $baselinePath)) {
    throw "check_hal_include_boundary: baseline file $baselinePath not found -- run with -UpdateBaseline once to create it after reviewing the counts it would record."
}

$baselineRaw = Get-Content -Path $baselinePath -Raw | ConvertFrom-Json
$ratchetFailures = Test-HalRatchet -Counts $counts -Baseline $baselineRaw

# --- Stage 3: firmware/hwAbstraction/** upward-include boundary ---
$hwAbstractionDir = Join-Path $repoRoot "firmware\hwAbstraction"
if (-not (Test-Path $hwAbstractionDir)) { throw "check_hal_include_boundary: $hwAbstractionDir not found -- has it moved?" }
$halFiles = Get-HalScanFiles -Dir (Resolve-Path $hwAbstractionDir).Path -RepoRoot $repoRootResolved
$halScan = Invoke-HalUpwardScan -RelPaths $halFiles -FileRoot $repoRootResolved
$halCounts = $halScan.RatchetCounts
$halViolations = $halScan.StrictViolations

if (-not $baselineRaw.hwAbstractionRatchet) {
    throw "check_hal_include_boundary: baseline file has no hwAbstractionRatchet section -- run with -UpdateBaseline once to create it after reviewing the counts it would record."
}
$halRatchetFailures = Test-HalRatchet -Counts $halCounts -Baseline $baselineRaw.hwAbstractionRatchet

if ($UpdateBaseline) {
    # Handled above for the main counts; also refresh the hwAbstraction
    # section here so a single -UpdateBaseline run keeps both honest.
    $baselineObj = [ordered]@{ "_note" = $baselineRaw._note }
    foreach ($label in $counts.Keys) { $baselineObj[$label] = $counts[$label] }
    $baselineObj["hwAbstractionRatchet"] = $halCounts
    $baselineObj | ConvertTo-Json | Set-Content -Path $baselinePath -Encoding utf8
    Write-Host "Baseline updated (including hwAbstractionRatchet) at $baselinePath"
    exit 0
}

if ($ratchetFailures.Count -gt 0 -or $violations.Count -gt 0 -or $halRatchetFailures.Count -gt 0 -or $halViolations.Count -gt 0) {
    Write-Host "HAL INCLUDE BOUNDARY CHECK FAILED:" -ForegroundColor Red
    foreach ($f in $ratchetFailures) { Write-Host "  RATCHET: $f" -ForegroundColor Red }
    foreach ($v in $violations) { Write-Host "  STRICT:  $v" -ForegroundColor Red }
    foreach ($f in $halRatchetFailures) { Write-Host "  HAL-UPWARD RATCHET: $f" -ForegroundColor Red }
    foreach ($v in $halViolations) { Write-Host "  HAL-UPWARD STRICT:  $v" -ForegroundColor Red }
    throw "$($ratchetFailures.Count) ratchet failure(s), $($violations.Count) strict allowlist violation(s), $($halRatchetFailures.Count) hwAbstraction ratchet failure(s), $($halViolations.Count) hwAbstraction upward-include violation(s) -- see HW_ABSTRACTION_PLAN.md Phase 4."
}

Write-Host "HAL include boundary check passed ($($allFiles.Count) files scanned):"
foreach ($label in $counts.Keys) {
    Write-Host ("  {0,-14} {1} (baseline {2})" -f $label, $counts[$label], $baselineRaw.$label)
}
Write-Host "  esp_ota_ops.h and esp_wifi.h/esp_netif.h strict allowlists: OK"
Write-Host "hwAbstraction/** upward-include check passed ($($halFiles.Count) files scanned):"
foreach ($label in $halCounts.Keys) {
    Write-Host ("  {0,-14} {1} (baseline {2})" -f $label, $halCounts[$label], $baselineRaw.hwAbstractionRatchet.$label)
}
Write-Host "  board_pins.h / stack_margin.h upward-include allowlist: OK"
exit 0
