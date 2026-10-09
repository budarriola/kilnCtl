# checkcache: ok
# check_cfgfs_nvs_only_drift.ps1 -- wrapper so tools/run_all_checks.ps1's
# check_*.ps1 glob picks up cfgfs_nvs_only_drift_check.py without any
# further wiring (same pattern as check_power_diag_flag_mirror_drift.ps1).
#
# A missing python FAILS this check (non-zero exit), not a silent skip --
# this machine is expected to have Python, so a missing interpreter means
# something is broken about the environment, not that the check has nothing
# to do.
#
# Usage: powershell -File firmware\KilnFW\App\test\check_cfgfs_nvs_only_drift.ps1
$ErrorActionPreference = "Stop"

$testDir = $PSScriptRoot
$checkScript = Join-Path $testDir "cfgfs_nvs_only_drift_check.py"
$repoRoot = (Resolve-Path (Join-Path $testDir "..\..\..\..")).Path

$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) {
    $python = Get-Command python3 -ErrorAction SilentlyContinue
}
if (-not $python) {
    Write-Host "CFGFS NVS_ONLY DRIFT CHECK: python not found on PATH." -ForegroundColor Red
    Write-Host "  FAILED, NOT SKIPPED -- this environment is expected to have Python." -ForegroundColor Red
    exit 1
}

& $python.Source $checkScript $repoRoot
exit $LASTEXITCODE
