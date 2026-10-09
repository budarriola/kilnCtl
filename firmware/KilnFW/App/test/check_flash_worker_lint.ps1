# checkcache: ok
# check_flash_worker_lint.ps1 -- wrapper so tools/run_all_checks.ps1's
# check_*.ps1 glob (see check_js_host_tests.ps1's own header comment for why
# that discovery mechanism exists) picks up flash_worker_lint.py without any
# further wiring. The lint itself is Python (a line-oriented grep over
# drivers/*.c is simpler there than in PowerShell), so this file's only job
# is: find python, run the lint, propagate its exit code.
#
# A missing python FAILS this check (non-zero exit), not a silent skip --
# same reasoning as check_js_host_tests.ps1's missing-node handling: this
# machine is expected to have Python (tools/mykicadMcp and friends already
# depend on it), so a missing interpreter here means something is broken
# about the environment, not that the check has nothing to do.
#
# Usage: powershell -File firmware\KilnFW\App\test\check_flash_worker_lint.ps1
$ErrorActionPreference = "Stop"

$testDir = $PSScriptRoot
$lintScript = Join-Path $testDir "flash_worker_lint.py"

$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) {
    $python = Get-Command python3 -ErrorAction SilentlyContinue
}
if (-not $python) {
    Write-Host "FLASH WORKER LINT: python not found on PATH." -ForegroundColor Red
    Write-Host "  FAILED, NOT SKIPPED -- this environment is expected to have Python." -ForegroundColor Red
    exit 1
}

& $python.Source $lintScript
exit $LASTEXITCODE
