# check_ramp_lock_decision_mirror_drift.ps1 -- wrapper so tools/run_all_
# checks.ps1's check_*.ps1 glob picks up ramp_lock_decision_mirror_drift_
# check.py without any further wiring. Same pattern as check_approach_rate_
# cap_mirror_drift.ps1: the check itself is Python, so this file's only job
# is find python, run the check, propagate its exit code.
#
# NOTE this check covers only lock_lagging_mask() (the ramp-lock decision).
# step_schedule() (the ramp-stepping gate) is covered separately by
# check_ramp_stepping_gate_mirror_drift.ps1 / ramp_stepping_gate_mirror_
# drift_check.py -- see that module's own docstring for what it compares.
#
# A missing python FAILS this check (non-zero exit), not a silent skip --
# this machine is expected to have Python, so a missing interpreter means
# something is broken about the environment, not that the check has nothing
# to do.
#
# Usage: powershell -File firmware\KilnFW\App\test\check_ramp_lock_decision_mirror_drift.ps1
$ErrorActionPreference = "Stop"

$testDir = $PSScriptRoot
$checkScript = Join-Path $testDir "ramp_lock_decision_mirror_drift_check.py"
$repoRoot = (Resolve-Path (Join-Path $testDir "..\..\..\..")).Path

$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) {
    $python = Get-Command python3 -ErrorAction SilentlyContinue
}
if (-not $python) {
    Write-Host "RAMP-LOCK DECISION MIRROR DRIFT CHECK: python not found on PATH." -ForegroundColor Red
    Write-Host "  FAILED, NOT SKIPPED -- this environment is expected to have Python." -ForegroundColor Red
    exit 1
}

& $python.Source $checkScript $repoRoot
exit $LASTEXITCODE
