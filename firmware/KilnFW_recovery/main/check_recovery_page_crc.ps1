# checkcache: ok
# check_recovery_page_crc.ps1 -- standing wrapper so tools/run_all_checks.ps1
# runs tools/PcTools/tests/test_recovery_page_crc.py: the recovery page's
# pure-JS CRC32 (executed under node) must match Python's zlib, and the page
# must carry no challenge/signature code (the recovery image is unauthenticated,
# owner decision 2026-10-02). Contract: exit 0 PASS, 3 SKIP (no node/python),
# else FAIL. Never `uv run`; plain python + unittest.
$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
if (-not (Get-Command node -ErrorAction SilentlyContinue)) {
    Write-Host "SKIP: no node on PATH -- cannot run the recovery page JS CRC test."
    exit 3
}
$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) { $python = Get-Command python3 -ErrorAction SilentlyContinue }
if (-not $python) {
    Write-Host "SKIP: python not found."
    exit 3
}
Push-Location (Join-Path $repoRoot "tools\PcTools")
try {
    $ErrorActionPreference = "Continue"
    $out = & $python.Source -m unittest tests.test_recovery_page_crc -v 2>&1
    $code = $LASTEXITCODE
    $ErrorActionPreference = "Stop"
    $out | ForEach-Object { Write-Host $_ }
    if ($code -ne 0) { throw "test_recovery_page_crc failed (exit $code)." }
    $text = $out -join "`n"
    if ($text -match "skipped") { throw "test_recovery_page_crc was skipped -- refusing to grade a skip as a pass." }
    if ($text -notmatch "Ran (\d+) tests" -or [int]$Matches[1] -lt 4) { throw "fewer than 4 tests ran." }
    Write-Host "check_recovery_page_crc: PASS ($($Matches[1]) tests)"
    exit 0
}
finally { Pop-Location }
