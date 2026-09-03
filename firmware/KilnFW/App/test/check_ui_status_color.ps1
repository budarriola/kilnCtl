# check_ui_status_color.ps1 -- standing guard from the 2026-09
# colour-dependence audit (WEB_UI_RESPONSIVE.md).
#
# Runs ui_status_color_check.mjs (this directory): asserts every
# --ok/--warn/--bad/--neutral status token clears a stated contrast floor
# against its own background in both themes (known pre-existing gaps are
# tracked, not hidden, in ui_status_color_contrast_exceptions.json), and
# that no NEW colour-only CSS rule (status-named selector distinguished
# only by color/background/border-color on a status token) has appeared
# without being reviewed into ui_status_color_allowlist.json alongside a
# note on its accompanying non-colour cue.
#
# Wired into tools/run_all_checks.ps1's check_*.ps1 discovery like every
# other guard in this repo (glob-discovered, not hand-listed).
#
# Usage: powershell -File firmware\KilnFW\App\test\check_ui_status_color.ps1
$ErrorActionPreference = "Stop"

$testDir = $PSScriptRoot
$checkScript = Join-Path $testDir "ui_status_color_check.mjs"
if (-not (Test-Path $checkScript)) {
    throw "check_ui_status_color.ps1: ui_status_color_check.mjs not found at $checkScript"
}

$node = Get-Command node -ErrorAction SilentlyContinue
if (-not $node) {
    # Consistent with check_ui_responsive_sweep.ps1's own handling: no Node on
    # PATH is an environment fact, not a code defect. This check is pure
    # regex/arithmetic on the *_page.html files, but it's still written in
    # Node for the same reason the sweep is, so it skips the same way.
    Write-Host "check_ui_status_color.ps1: SKIPPED -- no `node` on PATH, cannot run the check." -ForegroundColor Yellow
    exit 0
}

$output = & node $checkScript 2>&1
$code = $LASTEXITCODE
$output | ForEach-Object { Write-Host $_ }

if ($code -ne 0) {
    throw "check_ui_status_color.ps1: FAILED (exit $code) -- see output above for the specific token/page or the specific new colour-only rule."
}

Write-Host "check_ui_status_color.ps1: passed."
exit 0
