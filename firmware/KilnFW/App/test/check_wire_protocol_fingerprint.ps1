# checkcache: ok
# check_wire_protocol_fingerprint.ps1 -- wrapper so tools/run_all_checks.ps1's
# check_*.ps1 glob picks up wire_protocol_fingerprint_check.py without any
# further wiring, same shape as check_frame_a_offset_drift.ps1. The check
# itself is Python; this file's only job is: find python, run the check
# (read-only -- never passes --update), propagate its exit code.
#
# A missing python FAILS this check (non-zero exit), not a silent skip --
# same reasoning as check_flash_worker_lint.ps1's missing-python handling.
#
# Usage: powershell -File firmware\KilnFW\App\test\check_wire_protocol_fingerprint.ps1
$ErrorActionPreference = "Stop"

$testDir = $PSScriptRoot
$checkScript = Join-Path $testDir "wire_protocol_fingerprint_check.py"
$repoRoot = (Resolve-Path (Join-Path $testDir "..\..\..\..")).Path

$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) {
    $python = Get-Command python3 -ErrorAction SilentlyContinue
}
if (-not $python) {
    Write-Host "WIRE PROTOCOL FINGERPRINT CHECK: python not found on PATH." -ForegroundColor Red
    Write-Host "  FAILED, NOT SKIPPED -- this environment is expected to have Python." -ForegroundColor Red
    exit 1
}

& $python.Source $checkScript $repoRoot
exit $LASTEXITCODE
