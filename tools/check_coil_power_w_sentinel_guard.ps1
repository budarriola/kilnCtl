# check_coil_power_w_sentinel_guard.ps1 -- wrapper so tools/run_all_checks.ps1's
# check_*.ps1 glob (see firmware/KilnFW/App/test/check_source_path_drift.ps1's
# own header comment for why this discovery mechanism exists) picks up
# tools/PcTools/scripts/coil_power_w_sentinel_guard_check.py without any
# further wiring. That script enforces that zone_cfg_t::coil_power_w's
# 0.0f "not overridden" sentinel (ZONES_CFG_VERSION 24->25, dbd8ff52) stays
# confined to its reviewed producer/consumer files and that its one real
# consumer (zone_sweep_expected_coil_current_a(),
# zones_current_sweep_engine.c) keeps the guard that stops 0.0f being read
# as a real zero-watt coil. See that script's module docstring for the full
# background -- this is a narrow, field-pinned check, not a general
# "every field must have a producer" rule (this repo has rejected that
# broader shape before for the false-positive rate on legitimately-optional
# fields).
#
# A missing python FAILS this check (non-zero exit), not a silent skip --
# same reasoning check_ct_cal_write_surface.ps1 documents.
#
# Usage: powershell -File tools\check_coil_power_w_sentinel_guard.ps1
$ErrorActionPreference = "Stop"

$toolsDir = $PSScriptRoot
$checkScript = Join-Path $toolsDir "PcTools\scripts\coil_power_w_sentinel_guard_check.py"
$repoRoot = (Resolve-Path (Join-Path $toolsDir "..")).Path

$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) {
    $python = Get-Command python3 -ErrorAction SilentlyContinue
}
if (-not $python) {
    Write-Host "COIL_POWER_W SENTINEL GUARD CHECK: python not found on PATH." -ForegroundColor Red
    Write-Host "  FAILED, NOT SKIPPED -- this environment is expected to have Python." -ForegroundColor Red
    exit 1
}

& $python.Source $checkScript $repoRoot
exit $LASTEXITCODE
