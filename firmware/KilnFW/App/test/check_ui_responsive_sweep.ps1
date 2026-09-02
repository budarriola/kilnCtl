# check_ui_responsive_sweep.ps1 -- Phase 0 of WEB_UI_RESPONSIVE_PLAN.md
# ("a real test matrix", sec 4). Runs ui_responsive_sweep.mjs (this
# directory) -- a real headless-Chrome sweep over every *_page.html at
# 320/360/390/768/1280/1920px, asserting no horizontal overflow, no
# overlapping/occluded interactive elements, no undersized touch target,
# and no off-viewport interactive element. See that script's own header
# comment for the full design (why a same-origin static server and not
# file://, why the browser is not fetched, why the board is never touched).
#
# Wired into tools/run_all_checks.ps1's check_*.ps1 discovery like every
# other guard in this repo, per its own convention (glob-discovered, not
# hand-listed).
#
# Usage: powershell -File firmware\KilnFW\App\test\check_ui_responsive_sweep.ps1
$ErrorActionPreference = "Stop"

$testDir = $PSScriptRoot
$sweepScript = Join-Path $testDir "ui_responsive_sweep.mjs"
if (-not (Test-Path $sweepScript)) {
    throw "check_ui_responsive_sweep.ps1: ui_responsive_sweep.mjs not found at $sweepScript"
}

$node = Get-Command node -ErrorAction SilentlyContinue
if (-not $node) {
    Write-Host "check_ui_responsive_sweep.ps1: SKIPPED -- no `node` on PATH, cannot run the sweep." -ForegroundColor Yellow
    # Node not being installed is an environment fact, not a code defect --
    # this repo's other UI checks (label overflow-wrap, kv narrow stack) are
    # pure grep/regex and still run without it. Exit 0 rather than fail every
    # machine that has never needed Node before today.
    exit 0
}

$output = & node $sweepScript 2>&1
$code = $LASTEXITCODE
$output | ForEach-Object { Write-Host $_ }

if ($code -ne 0) {
    throw "check_ui_responsive_sweep.ps1: sweep FAILED (exit $code) -- see output above for the specific (page, width, assertion) failures."
}

Write-Host "check_ui_responsive_sweep.ps1: sweep passed."
exit 0
