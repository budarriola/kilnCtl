# checkcache: ok
# check_config_convert_mirror.ps1 -- wrapper so tools/run_all_checks.ps1's
# check_*.ps1 glob picks up check_config_convert_mirror.py without any
# further wiring. See that script's own docstring for what it compares
# (PROFILE_VERSION and friends, firmware vs
# tools/PcTools/src/kilnctrl/config_convert.py).
#
# Same "missing python FAILS, does not SKIP" convention as
# check_cfg_convert_field_mirror_drift.ps1.
#
# Usage: powershell -File tools\check_config_convert_mirror.ps1
$ErrorActionPreference = "Stop"

$toolsDir = $PSScriptRoot
$checkScript = Join-Path $toolsDir "check_config_convert_mirror.py"
$repoRoot = (Resolve-Path (Join-Path $toolsDir "..")).Path

$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) {
    $python = Get-Command python3 -ErrorAction SilentlyContinue
}
if (-not $python) {
    Write-Host "CONFIG-CONVERT MIRROR CHECK: python not found on PATH." -ForegroundColor Red
    Write-Host "  FAILED, NOT SKIPPED -- this environment is expected to have Python." -ForegroundColor Red
    exit 1
}

& $python.Source $checkScript $repoRoot
exit $LASTEXITCODE
