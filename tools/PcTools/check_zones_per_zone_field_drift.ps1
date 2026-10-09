# check_zones_per_zone_field_drift.ps1 -- wrapper so tools/run_all_checks.ps1's
# check_*.ps1 recursive glob picks up zones_per_zone_field_drift_check.py
# without further wiring (same pattern as
# firmware/KilnFW/App/test/check_stub_signature_drift.ps1).
#
# Compares the firmware's per-zone zones JSON field set
# (zones_http_get.c's zones_get_handler / zones_http_post_parse.c's
# zones_http_parse_zone_fields) against kilnctrl.zones_http_client's
# _ZONE_FIELD_FORM_KEY table -- added 2026-09-08 after
# zone_type/failsafe_state/hyst_c/min_on_s/min_off_s (and, found by this
# same check while writing it, the earlier relay_type) shipped in firmware
# with no matching PC-client entry, which made every whole-page zones save
# through this client raise ZonesHttpUnknownFieldError. This is a PER-ZONE
# counterpart to the pre-existing TOP-LEVEL-only
# zones_field_table_checks() in selfcheck_zones_fields.py -- see that
# module's header for why the top-level check alone could not have caught
# either incident.
#
# A missing Python here is a genuine failure, not a skip -- this repo's
# PcTools venv is expected to exist wherever this check runs, same stance as
# check_stub_signature_drift.ps1.
#
# Usage: powershell -File tools\PcTools\check_zones_per_zone_field_drift.ps1
$ErrorActionPreference = "Stop"

$scriptDir = $PSScriptRoot
$checkScript = Join-Path $scriptDir "zones_per_zone_field_drift_check.py"
$venvPython = Join-Path $scriptDir ".venv\Scripts\python.exe"
$venvCfg = Join-Path $scriptDir ".venv\pyvenv.cfg"

# Prefer the real PcTools venv (python.exe AND pyvenv.cfg next to it -- a
# bare python.exe with no pyvenv.cfg is not trusted as a real venv). A
# worktree checkout (under C:\wt\) has no venv of its own -- gitignored,
# per-clone -- so this falls back to `python`/`python3` on PATH there.
# Unlike check_bench_test_registry.ps1's inline script, this check's own
# .py does not insert tools\PcTools\src onto sys.path itself (it imports
# selfcheck_common, which imports kilnctrl.protocol directly), so the PATH
# fallback also sets PYTHONPATH to that src directory for the duration of
# this call so `import kilnctrl...` still resolves without a venv.
$python = $null
$usingVenv = $false
if ((Test-Path $venvPython) -and (Test-Path $venvCfg)) {
    $python = $venvPython
    $usingVenv = $true
} else {
    $cmd = Get-Command python -ErrorAction SilentlyContinue
    if (-not $cmd) { $cmd = Get-Command python3 -ErrorAction SilentlyContinue }
    if ($cmd) { $python = $cmd.Source }
}

if (-not $python) {
    Write-Host "ZONES PER-ZONE FIELD DRIFT CHECK: python not found (checked PcTools venv and PATH)." -ForegroundColor Red
    Write-Host "  FAILED, NOT SKIPPED -- this environment is expected to have Python." -ForegroundColor Red
    exit 1
}

if ($usingVenv) {
    Write-Host "check_zones_per_zone_field_drift.ps1: using venv python at $python"
    & $python $checkScript
} else {
    Write-Host "check_zones_per_zone_field_drift.ps1: no venv at $venvPython -- using PATH python at $python"
    $srcDir = Join-Path $scriptDir "src"
    $prevPythonPath = $env:PYTHONPATH
    if ($prevPythonPath) {
        $env:PYTHONPATH = "$srcDir;$prevPythonPath"
    } else {
        $env:PYTHONPATH = "$srcDir"
    }
    try {
        & $python $checkScript
    } finally {
        $env:PYTHONPATH = $prevPythonPath
    }
}
exit $LASTEXITCODE
