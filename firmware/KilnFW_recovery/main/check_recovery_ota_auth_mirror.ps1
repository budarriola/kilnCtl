# check_recovery_ota_auth_mirror.ps1 -- wrapper so tools/run_all_checks.ps1's
# check_*.ps1 glob picks up recovery_ota_auth_mirror_drift_check.py without
# any further wiring (same pattern as
# firmware/KilnFW/App/test/check_approach_rate_cap_mirror_drift.ps1 -- read
# that one's header comment for the reasoning). The check itself is Python
# (extracting/diffing a code fragment across two C files, plus a simple
# marker-order check, is simpler there than in PowerShell); this file's only
# job is: find python, run the check, propagate its exit code.
#
# A missing python FAILS this check (non-zero exit), not a silent skip --
# this machine is expected to have Python.
#
# Usage: powershell -File firmware\KilnFW_recovery\main\check_recovery_ota_auth_mirror.ps1
$ErrorActionPreference = "Stop"

$scriptDir = $PSScriptRoot
$checkScript = Join-Path $scriptDir "recovery_ota_auth_mirror_drift_check.py"
$repoRoot = (Resolve-Path (Join-Path $scriptDir "..\..\..")).Path

$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) {
    $python = Get-Command python3 -ErrorAction SilentlyContinue
}
if (-not $python) {
    Write-Host "RECOVERY OTA-AUTH MIRROR DRIFT CHECK: python not found on PATH." -ForegroundColor Red
    Write-Host "  FAILED, NOT SKIPPED -- this environment is expected to have Python." -ForegroundColor Red
    exit 1
}

& $python.Source $checkScript $repoRoot
exit $LASTEXITCODE
