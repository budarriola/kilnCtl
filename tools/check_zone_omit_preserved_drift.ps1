# check_zone_omit_preserved_drift.ps1 -- runs tools/PcTools/tests/test_zone_omit_preserved_drift.py
# (BENCHFX2..11 drift test: zones_http_client's omit-preserved key lists vs
# zones_http_post_parse.c, plus the zones_page.html Save-handler params whitelist).
# Pure source parsing, no board, a few seconds. Exit 0 on pass, 1 on any failure.
$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
. (Join-Path $PSScriptRoot "lib_pctools_python.ps1")
$python = Resolve-PcToolsPython -RepoRoot $repoRoot
if (-not $python) {
    Write-Host "check_zone_omit_preserved_drift.ps1: FAIL no python"
    exit 1
}
$env:PYTHONPATH = (Join-Path $repoRoot "tools\PcTools\src")
Push-Location (Join-Path $repoRoot "tools\PcTools\tests")
try {
    $ErrorActionPreference = "Continue"
    $out = & $python -m unittest test_zone_omit_preserved_drift 2>&1 | Out-String
    $rc = $LASTEXITCODE
} finally { Pop-Location }
if ($rc -ne 0) {
    Write-Host $out
    Write-Host "check_zone_omit_preserved_drift.ps1: FAIL"
    exit 1
}
Write-Host "check_zone_omit_preserved_drift.ps1: PASS"
exit 0
