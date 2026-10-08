# checkcache: ok
# test_check_ui_responsive_sweep.ps1 -- negative test for
# check_ui_responsive_sweep.ps1's classification logic.
#
# check_ui_responsive_sweep.ps1 itself drives a real headless-Chrome sweep,
# too slow/heavy to sabotage-and-rebuild like a build check. What IS
# unit-testable without a browser is ui_responsive_sweep.mjs's
# isTransientHarnessError() -- the pure string classifier that decides
# whether a CDP/network exception during the sweep is a retriable harness
# hiccup or a genuine layout-regression FAIL. A gap in this classifier is
# exactly what caused three real 2026-09-17 misfires under
# run_all_checks.ps1's parallel phase (see ui_responsive_sweep.mjs's own
# comment on isTransientHarnessError() and
# ui_responsive_sweep_classify.test.mjs's header for the full account).
#
# This wrapper just runs that node-level test and translates its exit code,
# following this repo's test_check_*.ps1 wiring convention (run_all_checks.ps1
# wires it in by name, same as the other hand-listed test_check_*/test_*
# scripts, since it does not match the check_*.ps1 discovery glob).
#
# Usage: powershell -File firmware\KilnFW\App\test\test_check_ui_responsive_sweep.ps1
$ErrorActionPreference = "Stop"

$testDir = $PSScriptRoot
$classifyTest = Join-Path $testDir "ui_responsive_sweep_classify.test.mjs"
if (-not (Test-Path $classifyTest)) {
    throw "test_check_ui_responsive_sweep.ps1: ui_responsive_sweep_classify.test.mjs not found at $classifyTest"
}

$node = Get-Command node -ErrorAction SilentlyContinue
if (-not $node) {
    Write-Host "test_check_ui_responsive_sweep.ps1: SKIP -- no `node` on PATH." -ForegroundColor Yellow
    exit 3
}

& node $classifyTest
$exitCode = $LASTEXITCODE
if ($exitCode -ne 0) {
    throw "test_check_ui_responsive_sweep.ps1: ui_responsive_sweep_classify.test.mjs failed (exit $exitCode) -- isTransientHarnessError() classification regressed."
}

Write-Host "test_check_ui_responsive_sweep.ps1: PASS -- isTransientHarnessError() classifications verified." -ForegroundColor Green
exit 0
