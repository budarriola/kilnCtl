# check_ui_content_smoke.ps1 -- runs ui_content_smoke.mjs (this directory): a
# headless-Chrome content smoke that loads zones_page.html (aux editor + spare
# relay device-type selects), diagnostics_page.html, readiness_page.html and the
# nav menu against MOCKED /api data and asserts key DOM, nav group placement,
# nav links resolving to real firmware routes, and no console errors.
# Sibling of check_ui_responsive_sweep.ps1 (layout only); same SKIP/FAIL policy:
# no node / no Chrome / DevTools port never up -> exit 3 (SKIP), assertion
# failure or hang -> throw.
#
# Usage: powershell -ExecutionPolicy Bypass -File firmware\KilnFW\App\test\check_ui_content_smoke.ps1
$ErrorActionPreference = "Stop"

$script = Join-Path $PSScriptRoot "ui_content_smoke.mjs"
if (-not (Test-Path $script)) { throw "check_ui_content_smoke.ps1: ui_content_smoke.mjs not found at $script" }

$node = Get-Command node -ErrorAction SilentlyContinue
if (-not $node) {
    Write-Host "check_ui_content_smoke.ps1: SKIP -- no `node` on PATH." -ForegroundColor Yellow
    exit 3
}

$stdoutFile = Join-Path $env:TEMP "kc-ui-smoke-stdout-$PID.txt"
$stderrFile = Join-Path $env:TEMP "kc-ui-smoke-stderr-$PID.txt"
$proc = Start-Process -FilePath $node.Source -ArgumentList @($script) -NoNewWindow -PassThru `
    -RedirectStandardOutput $stdoutFile -RedirectStandardError $stderrFile
$null = $proc.Handle  # cache the handle, else ExitCode reads back empty after WaitForExit
if (-not $proc.WaitForExit(240000)) {
    try {
        $tk = Start-Process -FilePath taskkill -ArgumentList @('/PID', "$($proc.Id)", '/T', '/F') -NoNewWindow -PassThru
        if (-not $tk.WaitForExit(15000)) { try { $tk.Kill() } catch {} }
    } catch {}
    Remove-Item -Path $stdoutFile, $stderrFile -Force -ErrorAction SilentlyContinue
    throw "check_ui_content_smoke.ps1: FAIL -- smoke did not finish within 240s; process tree killed."
}
$proc.WaitForExit()
$code = $proc.ExitCode
$stdout = if (Test-Path $stdoutFile) { Get-Content -Raw $stdoutFile } else { "" }
$stderrText = if (Test-Path $stderrFile) { Get-Content -Raw $stderrFile } else { "" }
Remove-Item -Path $stdoutFile, $stderrFile -Force -ErrorAction SilentlyContinue
if ($stdout) { Write-Host $stdout }
if ($stderrText) { Write-Host $stderrText }

if ($code -eq 3) {
    Write-Host "check_ui_content_smoke.ps1: SKIP -- smoke SKIPPED or hit a harness error (see output above)." -ForegroundColor Yellow
    exit 3
}
if ($code -ne 0) { throw "check_ui_content_smoke.ps1: FAIL -- ui_content_smoke.mjs exit $code (see output above)." }
if ("$stdout" -notmatch 'All \d+ page content checks passed') {
    throw "check_ui_content_smoke.ps1: FAIL -- no PASS marker in output; treating as failure."
}
Write-Host "check_ui_content_smoke.ps1: passed."
exit 0
