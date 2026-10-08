# checkcache: ok
# check_iter_tune_write_surface.ps1 -- wrapper so tools/run_all_checks.ps1's
# check_*.ps1 glob picks up tools/PcTools/scripts/iter_tune_write_surface_check.py
# without any further wiring (same pattern as check_ct_cal_write_surface.ps1).
#
# Enforces docs/ITER_TUNE_REDESIGN_PLAN.md sec 5: iter_tune.c/.h never calls
# a setter/persistence/hardware API itself, and no production file wires
# iter_tune_* into the firing pipeline (it is bench-fixture-only, unwired,
# pending owner sign-off per sec 9.3).
#
# A missing python FAILS this check (non-zero exit), not a silent skip --
# same reasoning check_source_path_drift.ps1 documents.
#
# Usage: powershell -File tools\check_iter_tune_write_surface.ps1
$ErrorActionPreference = "Stop"

$toolsDir = $PSScriptRoot
$checkScript = Join-Path $toolsDir "PcTools\scripts\iter_tune_write_surface_check.py"
$repoRoot = (Resolve-Path (Join-Path $toolsDir "..")).Path

$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) {
    $python = Get-Command python3 -ErrorAction SilentlyContinue
}
if (-not $python) {
    Write-Host "ITER_TUNE WRITE SURFACE CHECK: python not found on PATH." -ForegroundColor Red
    Write-Host "  FAILED, NOT SKIPPED -- this environment is expected to have Python." -ForegroundColor Red
    exit 1
}

& $python.Source $checkScript $repoRoot
exit $LASTEXITCODE
