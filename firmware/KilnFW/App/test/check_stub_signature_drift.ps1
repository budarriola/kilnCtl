# check_stub_signature_drift.ps1 -- wrapper so tools/run_all_checks.ps1's
# check_*.ps1 glob (see check_js_host_tests.ps1's own header comment) picks
# up stub_signature_drift_check.py without further wiring.
#
# UNLIKE check_flash_worker_lint.ps1's "missing python fails" stance, a
# missing ESP-IDF installation is a LEGITIMATE, expected state on plenty of
# machines that build/run the host tests just fine (the host tests exist
# specifically to need no ESP-IDF at all) -- so the underlying Python script
# skips gracefully (prints why, exits 0) when it cannot find one, and this
# wrapper does not second-guess that. Only a MISSING PYTHON is treated as a
# genuine failure here, same reasoning as check_flash_worker_lint.ps1.
#
# --fatal-on-clean is passed: the 2026-09-04 audit run (see this script's
# neighbor .py file's own header) found the stub tree currently clean --
# zero mismatches across all 26 stub headers matched to real IDF headers --
# so per ROADMAP.md B8's "non-fatal warning first IF you find any current
# mismatches, fatal if the tree is currently clean", this starts fatal.
#
# Usage: powershell -File firmware\KilnFW\App\test\check_stub_signature_drift.ps1
$ErrorActionPreference = "Stop"

$testDir = $PSScriptRoot
$checkScript = Join-Path $testDir "stub_signature_drift_check.py"

$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) {
    $python = Get-Command python3 -ErrorAction SilentlyContinue
}
if (-not $python) {
    Write-Host "STUB SIGNATURE DRIFT CHECK: python not found on PATH." -ForegroundColor Red
    Write-Host "  FAILED, NOT SKIPPED -- this environment is expected to have Python." -ForegroundColor Red
    exit 1
}

& $python.Source $checkScript --fatal-on-clean
exit $LASTEXITCODE
