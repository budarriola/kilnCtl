# check_ota_http_context_mirror.ps1 -- wrapper so tools/run_all_checks.ps1's
# check_*.ps1 glob picks up check_ota_http_context_mirror.py without any
# further wiring. See that script's own docstring for what it compares
# (the OTA HMAC context-string set, hand-copied across ota_http.c's switch,
# its _Static_assert table, test_ota_http.c's test oracle, and
# tools/PcTools/src/kilnctrl/ota_http_client.py's derive_mac() allow-list).
#
# Same "missing python FAILS, does not SKIP" convention as
# check_config_convert_mirror.ps1.
#
# Usage: powershell -File tools\check_ota_http_context_mirror.ps1
$ErrorActionPreference = "Stop"

$toolsDir = $PSScriptRoot
$checkScript = Join-Path $toolsDir "check_ota_http_context_mirror.py"
$repoRoot = (Resolve-Path (Join-Path $toolsDir "..")).Path

$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) {
    $python = Get-Command python3 -ErrorAction SilentlyContinue
}
if (-not $python) {
    Write-Host "OTA-HTTP CONTEXT MIRROR CHECK: python not found on PATH." -ForegroundColor Red
    Write-Host "  FAILED, NOT SKIPPED -- this environment is expected to have Python." -ForegroundColor Red
    exit 1
}

& $python.Source $checkScript $repoRoot
exit $LASTEXITCODE
