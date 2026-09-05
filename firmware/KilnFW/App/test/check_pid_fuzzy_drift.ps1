# check_pid_fuzzy_drift.ps1 -- wrapper so tools/run_all_checks.ps1's
# check_*.ps1 glob picks up pid_fuzzy_drift_check.py without further wiring.
# Same shape as check_approach_rate_cap_mirror_drift.ps1: the check itself is
# Python (it builds a small MSVC harness around the real pid_fuzzy.c and
# compares its output numerically against the Python port in
# tools/PcTools/src/kilnctrl/fuzzy_band_probe.py -- see that script's own
# header comment for why numerical equivalence, not a textual diff, is used
# here), so this file's only job is: find python, run the check, propagate
# its exit code.
#
# A missing python FAILS this check (non-zero exit), not a silent skip --
# this machine is expected to have Python.
#
# Usage: powershell -File firmware\KilnFW\App\test\check_pid_fuzzy_drift.ps1
$ErrorActionPreference = "Stop"

$testDir = $PSScriptRoot
$checkScript = Join-Path $testDir "pid_fuzzy_drift_check.py"
$repoRoot = (Resolve-Path (Join-Path $testDir "..\..\..\..")).Path

$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) {
    $python = Get-Command python3 -ErrorAction SilentlyContinue
}
if (-not $python) {
    Write-Host "PID_FUZZY DRIFT CHECK: python not found on PATH." -ForegroundColor Red
    Write-Host "  FAILED, NOT SKIPPED -- this environment is expected to have Python." -ForegroundColor Red
    exit 1
}

& $python.Source $checkScript $repoRoot
exit $LASTEXITCODE
