# test_check_hal_include_boundary.ps1 -- negative test for
# tools/check_hal_include_boundary.ps1 (HW_ABSTRACTION_PLAN.md Phase 4).
#
# A NEW PRECEDENT: no existing tools/check_*.ps1 has a negative test today
# (see that plan's "Phase 4" section). Every one of them has been proven
# able to fail by hand at some point in its history, but none of that proof
# is checked in or re-run. This script is the first one that is: it proves
# tools/check_hal_include_boundary.ps1's scan function can actually detect
# a violation, not just that it reports "all clear" on a tree that happens
# to already be clean (a check that always says pass is worse than no check
# at all, because it looks like evidence).
#
# Rewritten 2026-09-06 (HAL Phase 4 enforcement step 1): driver/, hardware/
# and esp_timer.h moved from the count-ratchet to strict per-file allowlists
# ($DriverGpioAllowlist etc., $HardwareAllowlist, $EspTimerAllowlist). The
# old assertions 2/3/3b assumed "driver/" was still a ratchet label and
# would fail against the new script (RatchetCounts no longer has a "driver/"
# key at all). This version:
#   1. Copies a clean, non-allowlisted file (drivers/pid.c) into the
#      scratchpad, unmodified. Runs the scan -- expects ZERO ratchet hits
#      and ZERO strict violations.
#   2. Copies it again and injects `#include "driver/gpio.h"`. Runs the
#      scan -- expects ZERO ratchet hits (driver/ is no longer ratcheted at
#      all) and EXACTLY ONE strict violation naming driver/gpio.h -- proves
#      the strict driver/gpio.h allowlist has teeth against a file that
#      isn't on it.
#   3. Proves the ratchet itself can still fail for the headers that remain
#      on it (nvs.h): takes the REAL tree's current "nvs.h" count and calls
#      the PRODUCTION Test-HalRatchet function (the same one
#      check_hal_include_boundary.ps1's main body calls) with a synthetic
#      baseline set one below that real count -- expects exactly one
#      violation naming "nvs.h". 3b confirms a baseline at the real count
#      does NOT trip it.
#   4. Proves the esp_ota_ops.h strict allowlist has teeth: injects
#      `#include "esp_ota_ops.h"` into a non-allowlisted scratch copy --
#      expects exactly one strict violation.
#   5. The same injected esp_ota_ops.h include on an allowlisted path
#      (ota_http.c) -- expects zero strict violations, proving the
#      allowlist is checked by path, not just by header name.
#   6. The same shape for driver/gpio.h specifically: injecting it into an
#      allowlisted path (uart_bridge_io.c, on $DriverGpioAllowlist) --
#      expects zero strict violations, proving the new driver/gpio.h
#      allowlist is also consulted by path.
#
# This does not touch the real repo tree or the real baseline file; it
# dot-sources tools/check_hal_include_boundary.ps1 (which, per its own
# guard, only defines functions/data when dot-sourced and runs nothing)
# and calls Invoke-HalBoundaryScan directly against a synthetic file list
# rooted at the scratchpad directory.
#
# Usage: powershell -ExecutionPolicy Bypass -File App\test\test_check_hal_include_boundary.ps1
# Exit code 0 = all assertions passed.

$ErrorActionPreference = "Stop"

$testDir = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $testDir "..\..\..\..")).Path
$checkScript = Join-Path $repoRoot "tools\check_hal_include_boundary.ps1"

if (-not (Test-Path $checkScript)) {
    throw "test_check_hal_include_boundary: expected $checkScript not found -- has it moved?"
}

# Dot-source: per check_hal_include_boundary.ps1's own guard, this defines
# Get-CodeOnlyLines / Invoke-HalBoundaryScan / etc. and runs nothing else.
. $checkScript

$scratchDir = Join-Path $env:TEMP "hal_boundary_test_$PID"
if (-not (Test-Path $scratchDir)) {
    New-Item -ItemType Directory -Force -Path $scratchDir | Out-Null
}

$sourcePid = Join-Path $repoRoot "firmware\KilnFW\App\drivers\control\pid.c"
if (-not (Test-Path $sourcePid)) {
    throw "test_check_hal_include_boundary: expected $sourcePid not found -- has it moved? Pick another clean, non-allowlisted file if pid.c has been relocated."
}

$failures = @()

try {

# --- Assertion 1: unmodified copy -> zero violations. ---
$cleanRel = "scratch_pid_clean.c"
$cleanFull = Join-Path $scratchDir $cleanRel
Copy-Item -Path $sourcePid -Destination $cleanFull -Force

$cleanScan = Invoke-HalBoundaryScan -RelPaths @($cleanRel) -FileRoot $scratchDir
$cleanRatchetTotal = 0
foreach ($k in $cleanScan.RatchetCounts.Keys) { $cleanRatchetTotal += $cleanScan.RatchetCounts[$k] }
if ($cleanRatchetTotal -ne 0) {
    $failures += "Assertion 1 FAILED: unmodified pid.c copy scored $cleanRatchetTotal ratchet hit(s), expected 0."
}
if ($cleanScan.StrictViolations.Count -ne 0) {
    $failures += "Assertion 1 FAILED: unmodified pid.c copy scored $($cleanScan.StrictViolations.Count) strict violation(s), expected 0."
}

# --- Assertion 2: inject `#include "driver/gpio.h"` into a NON-allowlisted
# scratch copy of pid.c -> ZERO ratchet hits (driver/ is no longer ratcheted
# at all, since HAL Phase 4 step 1 moved it to a strict per-file allowlist)
# and EXACTLY ONE strict violation naming driver/gpio.h. This is the
# rewritten version of the old "driver/ is a ratchet label" assertion. ---
$dirtyRel = "scratch_pid_dirty.c"
$dirtyFull = Join-Path $scratchDir $dirtyRel
$content = Get-Content -Path $cleanFull
$injected = @('#include "driver/gpio.h"') + $content
Set-Content -Path $dirtyFull -Value $injected -Encoding utf8

$dirtyScan = Invoke-HalBoundaryScan -RelPaths @($dirtyRel) -FileRoot $scratchDir
$dirtyRatchetTotal = 0
foreach ($k in $dirtyScan.RatchetCounts.Keys) { $dirtyRatchetTotal += $dirtyScan.RatchetCounts[$k] }
if ($dirtyRatchetTotal -ne 0) {
    $failures += "Assertion 2 FAILED: injected driver/gpio.h scored $dirtyRatchetTotal ratchet hit(s), expected 0 (driver/ is no longer a ratchet label)."
}
if ($dirtyScan.StrictViolations.Count -ne 1) {
    $failures += "Assertion 2 FAILED: non-allowlisted file with injected driver/gpio.h scored $($dirtyScan.StrictViolations.Count) strict violation(s), expected exactly 1."
} elseif ($dirtyScan.StrictViolations[0] -notlike "*driver/gpio.h*") {
    $failures += "Assertion 2 FAILED: the single strict violation did not name driver/gpio.h: $($dirtyScan.StrictViolations[0])"
} else {
    Write-Host "Assertion 2 OK: non-allowlisted driver/gpio.h injection produced exactly one strict violation -- $($dirtyScan.StrictViolations[0])"
}

# --- Assertion 3: the ratchet itself can still fail, for a header that
# remains on it (nvs.h -- driver/hardware/esp_timer.h moved to strict
# allowlists in HAL Phase 4 step 1 and are no longer ratchet labels). Scan
# the REAL tree, take the real "nvs.h" count, and call the PRODUCTION
# Test-HalRatchet function (the exact same function
# check_hal_include_boundary.ps1's main body calls against the real
# baseline file) with a synthetic baseline set one below that real count.
# Expect exactly one ratchet violation, naming "nvs.h". This calls the real
# comparison, not a reimplementation of it -- a bug in Test-HalRatchet
# itself would be caught here, which a hand-rolled `$count -gt $count-1`
# tautology could never do. ---
$appDirReal = Join-Path $repoRoot "firmware\KilnFW\App"
$saftyDirReal = Join-Path $repoRoot "firmware\SaftyFW\src"
$realFiles = @()
$realFiles += Get-ScanFiles -Dir (Resolve-Path $appDirReal).Path -RepoRoot $repoRoot
$realFiles += Get-ScanFiles -Dir (Resolve-Path $saftyDirReal).Path -RepoRoot $repoRoot

$realScan = Invoke-HalBoundaryScan -RelPaths $realFiles -FileRoot $repoRoot
$realNvsCount = $realScan.RatchetCounts["nvs.h"]

# Guard against a broken glob (e.g. a moved directory returning an empty
# file list) making assertions 3/3b pass vacuously: with $realFiles.Count
# at 0, $realNvsCount would be 0, $tooLowBaseline would be -1, and
# Test-HalRatchet would still legitimately report "count rose" -- a false
# positive that looks like a real, meaningful pass but proves nothing about
# the actual tree. Fail loudly here instead of building the synthetic
# baseline off a hollow scan.
if ($realFiles.Count -lt 200 -or $realNvsCount -lt 1) {
    throw "test_check_hal_include_boundary: real-tree scan looks broken (realFiles.Count=$($realFiles.Count), nvs.h count=$realNvsCount) -- expected at least 200 files and at least 1 nvs.h include. The glob is probably pointed at the wrong directory; assertions 3/3b would pass vacuously against this scan."
}

$tooLowBaseline = $realNvsCount - 1

$syntheticBaseline = [ordered]@{}
foreach ($label in $realScan.RatchetCounts.Keys) { $syntheticBaseline[$label] = $realScan.RatchetCounts[$label] }
$syntheticBaseline["nvs.h"] = $tooLowBaseline

$ratchetResult = Test-HalRatchet -Counts $realScan.RatchetCounts -Baseline $syntheticBaseline
$nvsViolations = @($ratchetResult | Where-Object { $_ -like "nvs.h *" })

if ($ratchetResult.Count -ne 1) {
    $failures += "Assertion 3 FAILED: expected exactly 1 ratchet violation from Test-HalRatchet with a baseline one below the real 'nvs.h' count ($realNvsCount vs baseline $tooLowBaseline), got $($ratchetResult.Count): $($ratchetResult -join ' | ')"
} elseif ($nvsViolations.Count -ne 1) {
    $failures += "Assertion 3 FAILED: the single ratchet violation did not name 'nvs.h': $($ratchetResult -join ' | ')"
} else {
    Write-Host "Assertion 3 OK: Test-HalRatchet (production function) flagged 'nvs.h' with baseline $tooLowBaseline vs real count $realNvsCount -- $($nvsViolations[0])"
}

# --- Assertion 3b: negate the check -- a baseline AT or ABOVE the real count
# must NOT trip the ratchet. Proves Test-HalRatchet isn't just always-fail. ---
$syntheticBaselineOk = [ordered]@{}
foreach ($label in $realScan.RatchetCounts.Keys) { $syntheticBaselineOk[$label] = $realScan.RatchetCounts[$label] }
$ratchetResultOk = Test-HalRatchet -Counts $realScan.RatchetCounts -Baseline $syntheticBaselineOk
if ($ratchetResultOk.Count -ne 0) {
    $failures += "Assertion 3b FAILED: baseline equal to the real counts should produce 0 ratchet violations, got $($ratchetResultOk.Count): $($ratchetResultOk -join ' | ')"
}

# --- Assertion 4: strict allowlist has teeth. Copy a non-allowlisted file
# (pid.c, same clean copy from assertion 1) and inject
# `#include "esp_ota_ops.h"`. Since scratch_pid_clean.c is not on
# $OtaOpsAllowlist, this must produce exactly one strict violation. ---
$otaDirtyRel = "scratch_pid_ota_dirty.c"
$otaDirtyFull = Join-Path $scratchDir $otaDirtyRel
$otaInjected = @('#include "esp_ota_ops.h"') + $content
Set-Content -Path $otaDirtyFull -Value $otaInjected -Encoding utf8

$otaDirtyScan = Invoke-HalBoundaryScan -RelPaths @($otaDirtyRel) -FileRoot $scratchDir
if ($otaDirtyScan.StrictViolations.Count -ne 1) {
    $failures += "Assertion 4 FAILED: non-allowlisted file with injected esp_ota_ops.h scored $($otaDirtyScan.StrictViolations.Count) strict violation(s), expected exactly 1."
}

# --- Assertion 5: the SAME injected include, on an allowlisted path, must
# score ZERO strict violations -- proves the allowlist is consulted by path,
# not just by header name. ota_http.c is on $OtaOpsAllowlist for
# esp_ota_ops.h. (dashboard_http.c and partition_info_http.c were dropped
# from the allowlist 2026-09-06: the hal_sysinfo migration moved both onto
# hal_sysinfo_get_running_partition()/_get_build_info()/_reset_reason(), so
# neither includes esp_ota_ops.h any more -- see $OtaOpsAllowlist's own
# comment in check_hal_include_boundary.ps1.) ---
$allowlistedRel = "firmware/KilnFW/App/drivers/http/ota_http.c"
$allowlistedScanDir = Join-Path $scratchDir "allowlisted_root"
$allowlistedFull = Join-Path $allowlistedScanDir ($allowlistedRel -replace '/', '\')
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $allowlistedFull) | Out-Null
Set-Content -Path $allowlistedFull -Value $otaInjected -Encoding utf8

$allowlistedScan = Invoke-HalBoundaryScan -RelPaths @($allowlistedRel) -FileRoot $allowlistedScanDir
if ($allowlistedScan.StrictViolations.Count -ne 0) {
    $failures += "Assertion 5 FAILED: allowlisted path ($allowlistedRel) with injected esp_ota_ops.h scored $($allowlistedScan.StrictViolations.Count) strict violation(s), expected 0."
}

# --- Assertion 6: same shape as 4/5, but for the NEW driver/gpio.h strict
# allowlist added in HAL Phase 4 step 1. Injecting driver/gpio.h into an
# ALLOWLISTED path (uart_bridge_io.c, on $DriverGpioAllowlist as one of the
# two raw IRQ owners named in HW_ABSTRACTION_PLAN.md's Phase 4 section) must
# score ZERO strict violations -- proves the driver/gpio.h allowlist is also
# consulted by path, not just by header name (assertion 2 above already
# proved the negative case: a NON-allowlisted path fails). ---
$gpioAllowlistedRel = "firmware/KilnFW/App/drivers/bridge/uart_bridge_io.c"
$gpioAllowlistedScanDir = Join-Path $scratchDir "gpio_allowlisted_root"
$gpioAllowlistedFull = Join-Path $gpioAllowlistedScanDir ($gpioAllowlistedRel -replace '/', '\')
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $gpioAllowlistedFull) | Out-Null
Set-Content -Path $gpioAllowlistedFull -Value $injected -Encoding utf8

$gpioAllowlistedScan = Invoke-HalBoundaryScan -RelPaths @($gpioAllowlistedRel) -FileRoot $gpioAllowlistedScanDir
if ($gpioAllowlistedScan.StrictViolations.Count -ne 0) {
    $failures += "Assertion 6 FAILED: allowlisted path ($gpioAllowlistedRel) with injected driver/gpio.h scored $($gpioAllowlistedScan.StrictViolations.Count) strict violation(s), expected 0."
} else {
    Write-Host "Assertion 6 OK: allowlisted driver/gpio.h path ($gpioAllowlistedRel) scored 0 strict violations."
}

} finally {
    # Cleanup: remove the whole per-PID scratch directory now that the test
    # is done with it (this test owns $scratchDir exclusively -- it is
    # created above, named with this process's PID). In `finally` so it
    # still runs if an assertion block throws instead of just recording a
    # failure. Best-effort: a failure here must not mask real assertion
    # failures or replace whatever exception is already propagating.
    Remove-Item -Path $scratchDir -Recurse -Force -ErrorAction SilentlyContinue
}

if ($failures.Count -gt 0) {
    Write-Host "TEST_CHECK_HAL_INCLUDE_BOUNDARY FAILED:" -ForegroundColor Red
    foreach ($f in $failures) { Write-Host "  $f" -ForegroundColor Red }
    throw "$($failures.Count) assertion(s) failed."
}

Write-Host "test_check_hal_include_boundary: all assertions passed (clean=0 violations, non-allowlisted driver/gpio.h=1 strict violation/0 ratchet, ratchet-fail-detection confirmed via production Test-HalRatchet on nvs.h, esp_ota_ops.h and driver/gpio.h strict allowlists both negative/positive confirmed)." -ForegroundColor Green
exit 0
