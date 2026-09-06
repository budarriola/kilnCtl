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
# What it does:
#   1. Copies a clean, non-allowlisted file (drivers/pid.c) into the
#      scratchpad, unmodified. Runs the scan -- expects ZERO violations.
#   2. Copies it again and injects `#include "driver/gpio.h"`. Runs the
#      scan on that copy -- expects EXACTLY ONE ratchet hit (the "driver/"
#      label) and zero strict violations (driver/gpio.h is not one of the
#      two strictly-enforced headers).
#   3. Proves the ratchet itself can fail: takes the REAL tree's current
#      "driver/" count and calls the PRODUCTION Test-HalRatchet function
#      (the same one check_hal_include_boundary.ps1's main body calls) with
#      a synthetic baseline set one below that real count -- expects exactly
#      one violation naming "driver/". 3b confirms a baseline at the real
#      count does NOT trip it.
#   4. Proves the strict allowlist has teeth: injects
#      `#include "esp_ota_ops.h"` into a non-allowlisted scratch copy --
#      expects exactly one strict violation.
#   5. The same injected include on an allowlisted path (dashboard_http.c)
#      -- expects zero strict violations, proving the allowlist is checked
#      by path, not just by header name.
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

# --- Assertion 2: inject `#include "driver/gpio.h"` -> exactly one
# ratchet hit (the "driver/" label), zero strict violations. ---
$dirtyRel = "scratch_pid_dirty.c"
$dirtyFull = Join-Path $scratchDir $dirtyRel
$content = Get-Content -Path $cleanFull
$injected = @('#include "driver/gpio.h"') + $content
Set-Content -Path $dirtyFull -Value $injected -Encoding utf8

$dirtyScan = Invoke-HalBoundaryScan -RelPaths @($dirtyRel) -FileRoot $scratchDir
$dirtyRatchetTotal = 0
foreach ($k in $dirtyScan.RatchetCounts.Keys) { $dirtyRatchetTotal += $dirtyScan.RatchetCounts[$k] }
if ($dirtyRatchetTotal -ne 1) {
    $failures += "Assertion 2 FAILED: injected driver/gpio.h scored $dirtyRatchetTotal total ratchet hit(s), expected exactly 1."
}
if ($dirtyScan.RatchetCounts["driver/"] -ne 1) {
    $failures += "Assertion 2 FAILED: injected driver/gpio.h did not register under the 'driver/' ratchet label (got $($dirtyScan.RatchetCounts['driver/']))."
}
if ($dirtyScan.StrictViolations.Count -ne 0) {
    $failures += "Assertion 2 FAILED: injected driver/gpio.h scored $($dirtyScan.StrictViolations.Count) strict violation(s), expected 0 (driver/gpio.h is not a strictly-enforced header)."
}

# --- Assertion 3: the ratchet itself can fail. Scan the REAL tree, take
# the real "driver/" count, and call the PRODUCTION Test-HalRatchet function
# (the exact same function check_hal_include_boundary.ps1's main body calls
# against the real baseline file) with a synthetic baseline set one below
# that real count. Expect exactly one ratchet violation, naming "driver/".
# This calls the real comparison, not a reimplementation of it -- a bug in
# Test-HalRatchet itself would be caught here, which the old inline
# `$count -gt $count-1` tautology could never do. ---
$appDirReal = Join-Path $repoRoot "firmware\KilnFW\App"
$saftyDirReal = Join-Path $repoRoot "firmware\SaftyFW\src"
$realFiles = @()
$realFiles += Get-ScanFiles -Dir (Resolve-Path $appDirReal).Path -RepoRoot $repoRoot
$realFiles += Get-ScanFiles -Dir (Resolve-Path $saftyDirReal).Path -RepoRoot $repoRoot

$realScan = Invoke-HalBoundaryScan -RelPaths $realFiles -FileRoot $repoRoot
$realDriverCount = $realScan.RatchetCounts["driver/"]

# Guard against a broken glob (e.g. a moved directory returning an empty
# file list) making assertions 3/3b pass vacuously: with $realFiles.Count
# at 0, $realDriverCount would be 0, $tooLowBaseline would be -1, and
# Test-HalRatchet would still legitimately report "count rose" -- a false
# positive that looks like a real, meaningful pass but proves nothing about
# the actual tree. Fail loudly here instead of building the synthetic
# baseline off a hollow scan.
if ($realFiles.Count -lt 200 -or $realDriverCount -lt 1) {
    throw "test_check_hal_include_boundary: real-tree scan looks broken (realFiles.Count=$($realFiles.Count), driver/ count=$realDriverCount) -- expected at least 200 files and at least 1 driver/ include. The glob is probably pointed at the wrong directory; assertions 3/3b would pass vacuously against this scan."
}

$tooLowBaseline = $realDriverCount - 1

$syntheticBaseline = [ordered]@{}
foreach ($label in $realScan.RatchetCounts.Keys) { $syntheticBaseline[$label] = $realScan.RatchetCounts[$label] }
$syntheticBaseline["driver/"] = $tooLowBaseline

$ratchetResult = Test-HalRatchet -Counts $realScan.RatchetCounts -Baseline $syntheticBaseline
$driverViolations = @($ratchetResult | Where-Object { $_ -like "driver/*" })

if ($ratchetResult.Count -ne 1) {
    $failures += "Assertion 3 FAILED: expected exactly 1 ratchet violation from Test-HalRatchet with a baseline one below the real 'driver/' count ($realDriverCount vs baseline $tooLowBaseline), got $($ratchetResult.Count): $($ratchetResult -join ' | ')"
} elseif ($driverViolations.Count -ne 1) {
    $failures += "Assertion 3 FAILED: the single ratchet violation did not name 'driver/': $($ratchetResult -join ' | ')"
} else {
    Write-Host "Assertion 3 OK: Test-HalRatchet (production function) flagged 'driver/' with baseline $tooLowBaseline vs real count $realDriverCount -- $($driverViolations[0])"
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
# not just by header name. dashboard_http.c is on $OtaOpsAllowlist for
# esp_ota_ops.h. ---
$allowlistedRel = "firmware/KilnFW/App/drivers/http/dashboard_http.c"
$allowlistedScanDir = Join-Path $scratchDir "allowlisted_root"
$allowlistedFull = Join-Path $allowlistedScanDir ($allowlistedRel -replace '/', '\')
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $allowlistedFull) | Out-Null
Set-Content -Path $allowlistedFull -Value $otaInjected -Encoding utf8

$allowlistedScan = Invoke-HalBoundaryScan -RelPaths @($allowlistedRel) -FileRoot $allowlistedScanDir
if ($allowlistedScan.StrictViolations.Count -ne 0) {
    $failures += "Assertion 5 FAILED: allowlisted path ($allowlistedRel) with injected esp_ota_ops.h scored $($allowlistedScan.StrictViolations.Count) strict violation(s), expected 0."
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

Write-Host "test_check_hal_include_boundary: all assertions passed (clean=0 violations, injected=1 ratchet hit, ratchet-fail-detection confirmed via production Test-HalRatchet, strict allowlist negative/positive confirmed)." -ForegroundColor Green
exit 0
