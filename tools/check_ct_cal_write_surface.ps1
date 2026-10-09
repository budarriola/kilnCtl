# check_ct_cal_write_surface.ps1 -- wrapper so tools/run_all_checks.ps1's
# check_*.ps1 glob (see firmware/KilnFW/App/test/check_source_path_drift.ps1's
# own header comment for why this discovery mechanism exists) picks up
# tools/PcTools/scripts/ct_cal_write_surface_check.py without any further
# wiring. That script enforces that SAFETY_CMD_SET_CT_CAL's PC-side write
# surface (removed 2026-09-08 -- see its module docstring for the S3-trip
# incident this closes) never comes back in kilnctrl/safety.py or
# kilnctrl/mcp_server_safety.py.
#
# A missing python FAILS this check (non-zero exit), not a silent skip --
# same reasoning check_source_path_drift.ps1 documents.
#
# Usage: powershell -File tools\check_ct_cal_write_surface.ps1
$ErrorActionPreference = "Stop"

$toolsDir = $PSScriptRoot
$checkScript = Join-Path $toolsDir "PcTools\scripts\ct_cal_write_surface_check.py"
$repoRoot = (Resolve-Path (Join-Path $toolsDir "..")).Path

$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) {
    $python = Get-Command python3 -ErrorAction SilentlyContinue
}
if (-not $python) {
    Write-Host "CT_CAL WRITE SURFACE CHECK: python not found on PATH." -ForegroundColor Red
    Write-Host "  FAILED, NOT SKIPPED -- this environment is expected to have Python." -ForegroundColor Red
    exit 1
}

& $python.Source $checkScript $repoRoot
exit $LASTEXITCODE
