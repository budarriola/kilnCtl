# check_cfg_convert_field_mirror_drift.ps1 -- wrapper so tools/run_all_checks.ps1's
# check_*.ps1 glob picks up cfg_convert_field_mirror_drift_check.py without any
# further wiring. See that script's own docstring for what it compares (the
# host-side tools/PcTools/src/kilnctrl/cfg_convert.py's field vocabulary and
# version constants against what backup_export.c/backup_import.c/
# backup_http_internal.h actually emit/read).
#
# Same "missing python FAILS, does not SKIP" convention as
# check_approach_rate_cap_mirror_drift.ps1.
#
# Usage: powershell -File firmware\KilnFW\App\test\check_cfg_convert_field_mirror_drift.ps1
$ErrorActionPreference = "Stop"

$testDir = $PSScriptRoot
$checkScript = Join-Path $testDir "cfg_convert_field_mirror_drift_check.py"
$repoRoot = (Resolve-Path (Join-Path $testDir "..\..\..\..")).Path

$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) {
    $python = Get-Command python3 -ErrorAction SilentlyContinue
}
if (-not $python) {
    Write-Host "CFG-CONVERT FIELD MIRROR DRIFT CHECK: python not found on PATH." -ForegroundColor Red
    Write-Host "  FAILED, NOT SKIPPED -- this environment is expected to have Python." -ForegroundColor Red
    exit 1
}

& $python.Source $checkScript $repoRoot
exit $LASTEXITCODE
