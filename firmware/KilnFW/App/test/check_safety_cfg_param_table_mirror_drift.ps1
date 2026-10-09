# checkcache: ok
# check_safety_cfg_param_table_mirror_drift.ps1 -- wrapper so tools/
# run_all_checks.ps1's check_*.ps1 glob picks up safety_cfg_param_table_
# mirror_drift_check.py without any further wiring. Same "find python, run
# the check, propagate its exit code" shape as check_approach_rate_cap_
# mirror_drift.ps1; read that file's own header comment for why a missing
# python FAILS this check rather than skipping it.
#
# Usage: powershell -File firmware\KilnFW\App\test\check_safety_cfg_param_table_mirror_drift.ps1
$ErrorActionPreference = "Stop"

$testDir = $PSScriptRoot
$checkScript = Join-Path $testDir "safety_cfg_param_table_mirror_drift_check.py"
$repoRoot = (Resolve-Path (Join-Path $testDir "..\..\..\..")).Path

$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) {
    $python = Get-Command python3 -ErrorAction SilentlyContinue
}
if (-not $python) {
    Write-Host "SAFETY-CFG PARAM TABLE MIRROR DRIFT CHECK: python not found on PATH." -ForegroundColor Red
    Write-Host "  FAILED, NOT SKIPPED -- this environment is expected to have Python." -ForegroundColor Red
    exit 1
}

& $python.Source $checkScript $repoRoot
exit $LASTEXITCODE
