# checkcache: ok
# check_source_path_drift.ps1 -- wrapper so tools/run_all_checks.ps1's
# check_*.ps1 glob (see check_js_host_tests.ps1's own header comment for why
# that discovery mechanism exists) picks up source_path_drift_check.py
# without any further wiring. The check itself is Python (regex extraction
# plus filesystem existence checks across four different directories is
# simpler there than in PowerShell), so this file's only job is: find
# python, run the check, propagate its exit code.
#
# A missing python FAILS this check (non-zero exit), not a silent skip --
# same reasoning as check_flash_worker_lint.ps1's missing-python handling:
# this machine is expected to have Python, so a missing interpreter here
# means something is broken about the environment, not that the check has
# nothing to do.
#
# Usage: powershell -File firmware\KilnFW\App\test\check_source_path_drift.ps1
$ErrorActionPreference = "Stop"

$testDir = $PSScriptRoot
$checkScript = Join-Path $testDir "source_path_drift_check.py"
$repoRoot = (Resolve-Path (Join-Path $testDir "..\..\..\..")).Path

$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) {
    $python = Get-Command python3 -ErrorAction SilentlyContinue
}
if (-not $python) {
    Write-Host "SOURCE PATH DRIFT CHECK: python not found on PATH." -ForegroundColor Red
    Write-Host "  FAILED, NOT SKIPPED -- this environment is expected to have Python." -ForegroundColor Red
    exit 1
}

& $python.Source $checkScript $repoRoot
exit $LASTEXITCODE
