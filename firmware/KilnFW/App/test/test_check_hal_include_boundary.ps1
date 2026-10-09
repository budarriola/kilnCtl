# test_check_hal_include_boundary.ps1 -- negative test for
# tools/check_hal_include_boundary.ps1 (HW_ABSTRACTION.md Phase 4).
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
# key at all).
#
# Rewritten again 2026-09-06 (HAL Phase 4 enforcement step 2): nvs.h and
# nvs_flash.h -- the last two headers left on the ratchet -- were also
# promoted to strict per-file allowlists ($NvsAllowlist, $NvsFlashAllowlist).
# $RatchetHeaders is now empty, so the old assertions 3/3b (which took the
# REAL tree's "nvs.h" ratchet count and fed it through Test-HalRatchet) no
# longer have a real-tree ratcheted header to exercise -- RatchetCounts has
# no "nvs.h" key at all any more. Test-HalRatchet itself is still shared
# with the hwAbstraction upward-scan, so assertions 3/3b now call it with a
# synthetic, made-up label instead of a real one -- this proves the
# comparison function's logic directly, independent of what happens to be
# tracked in production today. This version:
#   1. Copies a clean, non-allowlisted file (drivers/pid.c) into the
#      scratchpad, unmodified. Runs the scan -- expects ZERO ratchet hits
#      and ZERO strict violations.
#   2. Copies it again and injects `#include "driver/gpio.h"`. Runs the
#      scan -- expects ZERO ratchet hits (driver/ is no longer ratcheted at
#      all) and EXACTLY ONE strict violation naming driver/gpio.h -- proves
#      the strict driver/gpio.h allowlist has teeth against a file that
#      isn't on it.
#   3. Proves Test-HalRatchet (the PRODUCTION function, the same one
#      check_hal_include_boundary.ps1's main body calls) can still detect a
#      rise, using a synthetic label/count pair (not tied to any header
#      currently tracked in production, since the real ratchet is empty) --
#      expects exactly one violation naming the synthetic label. 3b confirms
#      a baseline at or above the count does NOT trip it.
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
#   7. The same shape for nvs.h: non-allowlisted scratch copy of pid.c with
#      `#include "nvs.h"` injected -- expects exactly one strict violation;
#      the same injection on the allowlisted path (ota_record.c) -- expects
#      zero.
#   8. The same shape for nvs_flash.h: non-allowlisted scratch copy with
#      `#include "nvs_flash.h"` injected -- expects exactly one strict
#      violation; the same injection on an allowlisted path (wifi_prov.c) --
#      expects zero.
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

# --- Assertion 3: Test-HalRatchet (the PRODUCTION function, the exact same
# one check_hal_include_boundary.ps1's main body calls against the real
# baseline file) can still detect a rise. $RatchetHeaders is empty now (HAL
# Phase 4 step 2 promoted its last two labels, nvs.h/nvs_flash.h, to strict
# allowlists), so there is no real-tree ratcheted header left to drive this
# off of -- use a synthetic label/count pair instead. This still calls the
# real comparison, not a reimplementation of it: a bug in Test-HalRatchet
# itself would be caught here, which a hand-rolled `$count -gt $count-1`
# tautology could never do. ---
$syntheticCounts = [ordered]@{ "synthetic_test_header.h" = 5 }
$syntheticBaseline = [ordered]@{ "synthetic_test_header.h" = 4 }

$ratchetResult = Test-HalRatchet -Counts $syntheticCounts -Baseline $syntheticBaseline
$syntheticViolations = @($ratchetResult | Where-Object { $_ -like "synthetic_test_header.h *" })

if ($ratchetResult.Count -ne 1) {
    $failures += "Assertion 3 FAILED: expected exactly 1 ratchet violation from Test-HalRatchet with a synthetic count of 5 vs baseline 4, got $($ratchetResult.Count): $($ratchetResult -join ' | ')"
} elseif ($syntheticViolations.Count -ne 1) {
    $failures += "Assertion 3 FAILED: the single ratchet violation did not name 'synthetic_test_header.h': $($ratchetResult -join ' | ')"
} else {
    Write-Host "Assertion 3 OK: Test-HalRatchet (production function) flagged a synthetic count rise (5 vs baseline 4) -- $($syntheticViolations[0])"
}

# --- Assertion 3b: negate the check -- a baseline AT or ABOVE the count must
# NOT trip the ratchet. Proves Test-HalRatchet isn't just always-fail. ---
$syntheticBaselineOk = [ordered]@{ "synthetic_test_header.h" = 5 }
$ratchetResultOk = Test-HalRatchet -Counts $syntheticCounts -Baseline $syntheticBaselineOk
if ($ratchetResultOk.Count -ne 0) {
    $failures += "Assertion 3b FAILED: baseline equal to the count should produce 0 ratchet violations, got $($ratchetResultOk.Count): $($ratchetResultOk -join ' | ')"
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
# two raw IRQ owners named in HW_ABSTRACTION.md's Phase 4 section) must
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

# --- Assertion 7: same shape as 4/5/6, for the nvs.h strict allowlist added
# in HAL Phase 4 step 2. Non-allowlisted scratch copy -> exactly one strict
# violation; allowlisted path (ota_record.c, on $NvsAllowlist) -> zero. ---
$nvsDirtyRel = "scratch_pid_nvs_dirty.c"
$nvsDirtyFull = Join-Path $scratchDir $nvsDirtyRel
$nvsInjected = @('#include "nvs.h"') + $content
Set-Content -Path $nvsDirtyFull -Value $nvsInjected -Encoding utf8

$nvsDirtyScan = Invoke-HalBoundaryScan -RelPaths @($nvsDirtyRel) -FileRoot $scratchDir
if ($nvsDirtyScan.StrictViolations.Count -ne 1) {
    $failures += "Assertion 7 FAILED: non-allowlisted file with injected nvs.h scored $($nvsDirtyScan.StrictViolations.Count) strict violation(s), expected exactly 1."
} elseif ($nvsDirtyScan.StrictViolations[0] -notlike "*nvs.h*") {
    $failures += "Assertion 7 FAILED: the single strict violation did not name nvs.h: $($nvsDirtyScan.StrictViolations[0])"
} else {
    Write-Host "Assertion 7 OK: non-allowlisted nvs.h injection produced exactly one strict violation -- $($nvsDirtyScan.StrictViolations[0])"
}

$nvsAllowlistedRel = "firmware/KilnFW/App/drivers/persist/ota_record.c"
$nvsAllowlistedScanDir = Join-Path $scratchDir "nvs_allowlisted_root"
$nvsAllowlistedFull = Join-Path $nvsAllowlistedScanDir ($nvsAllowlistedRel -replace '/', '\')
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $nvsAllowlistedFull) | Out-Null
Set-Content -Path $nvsAllowlistedFull -Value $nvsInjected -Encoding utf8

$nvsAllowlistedScan = Invoke-HalBoundaryScan -RelPaths @($nvsAllowlistedRel) -FileRoot $nvsAllowlistedScanDir
if ($nvsAllowlistedScan.StrictViolations.Count -ne 0) {
    $failures += "Assertion 7 FAILED: allowlisted path ($nvsAllowlistedRel) with injected nvs.h scored $($nvsAllowlistedScan.StrictViolations.Count) strict violation(s), expected 0."
} else {
    Write-Host "Assertion 7 OK: allowlisted nvs.h path ($nvsAllowlistedRel) scored 0 strict violations."
}

# --- Assertion 8: same shape, for the nvs_flash.h strict allowlist.
# Non-allowlisted scratch copy -> exactly one strict violation; allowlisted
# path (wifi_prov.c, on $NvsFlashAllowlist) -> zero. ---
$nvsFlashDirtyRel = "scratch_pid_nvsflash_dirty.c"
$nvsFlashDirtyFull = Join-Path $scratchDir $nvsFlashDirtyRel
$nvsFlashInjected = @('#include "nvs_flash.h"') + $content
Set-Content -Path $nvsFlashDirtyFull -Value $nvsFlashInjected -Encoding utf8

$nvsFlashDirtyScan = Invoke-HalBoundaryScan -RelPaths @($nvsFlashDirtyRel) -FileRoot $scratchDir
if ($nvsFlashDirtyScan.StrictViolations.Count -ne 1) {
    $failures += "Assertion 8 FAILED: non-allowlisted file with injected nvs_flash.h scored $($nvsFlashDirtyScan.StrictViolations.Count) strict violation(s), expected exactly 1."
} elseif ($nvsFlashDirtyScan.StrictViolations[0] -notlike "*nvs_flash.h*") {
    $failures += "Assertion 8 FAILED: the single strict violation did not name nvs_flash.h: $($nvsFlashDirtyScan.StrictViolations[0])"
} else {
    Write-Host "Assertion 8 OK: non-allowlisted nvs_flash.h injection produced exactly one strict violation -- $($nvsFlashDirtyScan.StrictViolations[0])"
}

$nvsFlashAllowlistedRel = "firmware/KilnFW/App/drivers/net/wifi_prov.c"
$nvsFlashAllowlistedScanDir = Join-Path $scratchDir "nvsflash_allowlisted_root"
$nvsFlashAllowlistedFull = Join-Path $nvsFlashAllowlistedScanDir ($nvsFlashAllowlistedRel -replace '/', '\')
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $nvsFlashAllowlistedFull) | Out-Null
Set-Content -Path $nvsFlashAllowlistedFull -Value $nvsFlashInjected -Encoding utf8

$nvsFlashAllowlistedScan = Invoke-HalBoundaryScan -RelPaths @($nvsFlashAllowlistedRel) -FileRoot $nvsFlashAllowlistedScanDir
if ($nvsFlashAllowlistedScan.StrictViolations.Count -ne 0) {
    $failures += "Assertion 8 FAILED: allowlisted path ($nvsFlashAllowlistedRel) with injected nvs_flash.h scored $($nvsFlashAllowlistedScan.StrictViolations.Count) strict violation(s), expected 0."
} else {
    Write-Host "Assertion 8 OK: allowlisted nvs_flash.h path ($nvsFlashAllowlistedRel) scored 0 strict violations."
}

# --- Assertion 9: esp_random.h strict allowlist, added 2026-09-06 when the
# HW_ABSTRACTION.md "esp_random.h was never classified" open item was
# closed -- every real call site migrated onto hal_sysinfo_random_u32()/
# hal_sysinfo_fill_random() and no holdout remains, so $EspRandomAllowlist is
# EMPTY. Unlike nvs.h/nvs_flash.h/esp_ota_ops.h (which pair a non-allowlisted
# negative with an allowlisted-path positive), there is no allowlisted path
# to use for a positive case here -- the positive half of this pair instead
# proves the empty allowlist has no accidental grandfather entry: injecting
# esp_random.h into safety_link.c itself (the file that used to be the real
# esp_random() call site, until this migration) must ALSO score a strict
# violation, exactly like any other non-allowlisted file. ---
$randDirtyRel = "scratch_pid_esprandom_dirty.c"
$randDirtyFull = Join-Path $scratchDir $randDirtyRel
$randInjected = @('#include "esp_random.h"') + $content
Set-Content -Path $randDirtyFull -Value $randInjected -Encoding utf8

$randDirtyScan = Invoke-HalBoundaryScan -RelPaths @($randDirtyRel) -FileRoot $scratchDir
if ($randDirtyScan.StrictViolations.Count -ne 1) {
    $failures += "Assertion 9 FAILED: non-allowlisted file with injected esp_random.h scored $($randDirtyScan.StrictViolations.Count) strict violation(s), expected exactly 1."
} elseif ($randDirtyScan.StrictViolations[0] -notlike "*esp_random.h*") {
    $failures += "Assertion 9 FAILED: the single strict violation did not name esp_random.h: $($randDirtyScan.StrictViolations[0])"
} else {
    Write-Host "Assertion 9 OK: non-allowlisted esp_random.h injection produced exactly one strict violation -- $($randDirtyScan.StrictViolations[0])"
}

$randFormerHoldoutRel = "firmware/KilnFW/App/drivers/safety/safety_link.c"
$randFormerHoldoutScanDir = Join-Path $scratchDir "esprandom_former_holdout_root"
$randFormerHoldoutFull = Join-Path $randFormerHoldoutScanDir ($randFormerHoldoutRel -replace '/', '\')
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $randFormerHoldoutFull) | Out-Null
Set-Content -Path $randFormerHoldoutFull -Value $randInjected -Encoding utf8

$randFormerHoldoutScan = Invoke-HalBoundaryScan -RelPaths @($randFormerHoldoutRel) -FileRoot $randFormerHoldoutScanDir
if ($randFormerHoldoutScan.StrictViolations.Count -ne 1) {
    $failures += "Assertion 9 FAILED: former-holdout path ($randFormerHoldoutRel) with injected esp_random.h scored $($randFormerHoldoutScan.StrictViolations.Count) strict violation(s), expected exactly 1 (the empty allowlist must not grandfather this file back in)."
} else {
    Write-Host "Assertion 9 OK: former-holdout esp_random.h path ($randFormerHoldoutRel) still scored 1 strict violation -- empty allowlist has no grandfather entry."
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

Write-Host "test_check_hal_include_boundary: all assertions passed (clean=0 violations, non-allowlisted driver/gpio.h=1 strict violation/0 ratchet, ratchet-fail-detection confirmed via production Test-HalRatchet on a synthetic label, esp_ota_ops.h/driver/gpio.h/nvs.h/nvs_flash.h strict allowlists all negative/positive confirmed, esp_random.h strict allowlist confirmed empty with no grandfathered holdout)." -ForegroundColor Green
exit 0
