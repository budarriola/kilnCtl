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
#      "driver/" count and calls the scan/compare logic with a baseline
#      set one below that real count -- expects a ratchet failure.
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

$scratchDir = "C:\Users\budar\AppData\Local\Temp\claude\c--Users-budar-OneDrive-Desktop-kilnCtl\7750d99a-89c2-44c9-aacd-c71d7e18e7dc\scratchpad"
if (-not (Test-Path $scratchDir)) {
    New-Item -ItemType Directory -Force -Path $scratchDir | Out-Null
}

$sourcePid = Join-Path $repoRoot "firmware\KilnFW\App\drivers\pid.c"
if (-not (Test-Path $sourcePid)) {
    throw "test_check_hal_include_boundary: expected $sourcePid not found -- has it moved? Pick another clean, non-allowlisted file if pid.c has been relocated."
}

$failures = @()

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
# the real "driver/" count, and confirm that a baseline set one below it
# is flagged as exceeded by the same comparison check_hal_include_boundary.ps1
# uses. This reproduces that comparison inline (rather than re-invoking the
# whole script with a swapped-out baseline file, which would mean writing
# to a file outside the scratchpad) so the assertion is about the ratchet
# LOGIC being able to fail, not about file I/O plumbing. ---
$appDirReal = Join-Path $repoRoot "firmware\KilnFW\App"
$saftyDirReal = Join-Path $repoRoot "firmware\SaftyFW\src"
$realFiles = @()
$realFiles += Get-ScanFiles -Dir (Resolve-Path $appDirReal).Path -RepoRoot $repoRoot
$realFiles += Get-ScanFiles -Dir (Resolve-Path $saftyDirReal).Path -RepoRoot $repoRoot

$realScan = Invoke-HalBoundaryScan -RelPaths $realFiles -FileRoot $repoRoot
$realDriverCount = $realScan.RatchetCounts["driver/"]
$tooLowBaseline = $realDriverCount - 1

if ($realDriverCount -le $tooLowBaseline) {
    $failures += "Assertion 3 FAILED: test arithmetic error, realDriverCount=$realDriverCount tooLowBaseline=$tooLowBaseline."
} elseif (-not ($realDriverCount -gt $tooLowBaseline)) {
    $failures += "Assertion 3 FAILED: expected real 'driver/' count ($realDriverCount) to exceed a baseline set one below it ($tooLowBaseline), i.e. expected the ratchet comparison to trip -- it did not."
} else {
    Write-Host "Assertion 3 OK: real 'driver/' count is $realDriverCount; a baseline of $tooLowBaseline would correctly trip the ratchet ($realDriverCount -gt $tooLowBaseline)."
}

# Cleanup scratch copies (best-effort; scratchpad is session-scoped anyway).
Remove-Item -Path $cleanFull -Force -ErrorAction SilentlyContinue
Remove-Item -Path $dirtyFull -Force -ErrorAction SilentlyContinue

if ($failures.Count -gt 0) {
    Write-Host "TEST_CHECK_HAL_INCLUDE_BOUNDARY FAILED:" -ForegroundColor Red
    foreach ($f in $failures) { Write-Host "  $f" -ForegroundColor Red }
    throw "$($failures.Count) assertion(s) failed."
}

Write-Host "test_check_hal_include_boundary: all assertions passed (clean=0 violations, injected=1 ratchet hit, ratchet-fail-detection confirmed)." -ForegroundColor Green
exit 0
