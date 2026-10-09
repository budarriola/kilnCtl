# test_check_uri_handler_cap_max_routes.ps1 -- negative test for the KILN_HTTP_MAX_ROUTES >= cap rule in
# tools/check_uri_handler_cap.ps1 (DEV_TOOLS_REVIEW_2026-10-09 LOW-4). Copies the real drivers tree to a
# scratch dir (read-only on the repo), lowers the define there, and expects the check to refuse.
$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..\..")).Path
$check = Join-Path $repoRoot "tools\check_uri_handler_cap.ps1"
$src = (Resolve-Path (Join-Path $repoRoot "firmware\KilnFW\App\drivers")).Path
$scratch = Join-Path $env:TEMP "uri_cap_maxroutes_$PID"
New-Item -ItemType Directory -Force -Path $scratch | Out-Null
try {
    Copy-Item -Path $src -Destination $scratch -Recurse -Force
    $copy = Join-Path $scratch "drivers"
    function Run([string]$dir) {
        $ErrorActionPreference = "Continue"
        $o = & powershell -NoProfile -ExecutionPolicy Bypass -File $check -DriversDir $dir 2>&1 | Out-String
        return @{ Rc = $LASTEXITCODE; Out = $o }
    }
    $ok = Run $copy
    if ($ok.Rc -ne 0) { throw "baseline copy of the real tree did not pass (exit $($ok.Rc)):`n$($ok.Out)" }
    $f = (Get-ChildItem -Path $copy -Filter "http_auth_http.c" -File -Recurse | Select-Object -First 1).FullName
    $text = [System.IO.File]::ReadAllText($f)
    $new = [regex]::Replace($text, '(#\s*define\s+KILN_HTTP_MAX_ROUTES\s+)\d+', '${1}1')
    if ($new -eq $text) { throw "could not find KILN_HTTP_MAX_ROUTES define to lower in $f" }
    [System.IO.File]::WriteAllText($f, $new)
    $bad = Run $copy
    if ($bad.Rc -eq 0 -or $bad.Out -notmatch "KILN_HTTP_MAX_ROUTES") { throw "lowered KILN_HTTP_MAX_ROUTES was not refused (exit $($bad.Rc)):`n$($bad.Out)" }
    Write-Host "test_check_uri_handler_cap_max_routes: PASS (baseline passes, KILN_HTTP_MAX_ROUTES=1 refused)"
} finally {
    Remove-Item -Recurse -Force -LiteralPath $scratch -ErrorAction SilentlyContinue
}
