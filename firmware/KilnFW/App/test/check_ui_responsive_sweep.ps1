# check_ui_responsive_sweep.ps1 -- Phase 0 of WEB_UI_RESPONSIVE.md
# ("a real test matrix", sec 4). Runs ui_responsive_sweep.mjs (this
# directory) -- a real headless-Chrome sweep over every *_page.html at
# 320/360/390/768/1280/1920px, asserting no horizontal overflow, no
# overlapping/occluded interactive elements, no undersized touch target,
# and no off-viewport interactive element. See that script's own header
# comment for the full design (why a same-origin static server and not
# file://, why the browser is not fetched, why the board is never touched).
#
# Wired into tools/run_all_checks.ps1's check_*.ps1 discovery like every
# other guard in this repo, per its own convention (glob-discovered, not
# hand-listed).
#
# Usage: powershell -File firmware\KilnFW\App\test\check_ui_responsive_sweep.ps1
$ErrorActionPreference = "Stop"

$testDir = $PSScriptRoot
$sweepScript = Join-Path $testDir "ui_responsive_sweep.mjs"
if (-not (Test-Path $sweepScript)) {
    throw "check_ui_responsive_sweep.ps1: ui_responsive_sweep.mjs not found at $sweepScript"
}

$node = Get-Command node -ErrorAction SilentlyContinue
if (-not $node) {
    Write-Host "check_ui_responsive_sweep.ps1: SKIP -- no `node` on PATH, cannot run the sweep." -ForegroundColor Yellow
    # Node not being installed is an environment fact, not a code defect --
    # this repo's other UI checks (label overflow-wrap, kv narrow stack) are
    # pure grep/regex and still run without it. exit 3 is run_all_checks.ps1's
    # reserved SKIP status (see that script's header) -- distinct from exit
    # 0/PASS so this shows up in the suite summary as skipped, not passed.
    exit 3
}

# Run node via Start-Process (not a bare `&`, not Start-Job) with a hard
# wall-clock cap. A headless Chrome that fails to start cleanly (a stuck
# leftover profile lock, a machine under heavy concurrent load, AV scanning
# a freshly spawned exe) has been observed to leave the node process itself
# sitting idle before it ever reaches the script's own internal 30s
# DevTools-port timeout, which would otherwise hang this check, and with it
# run_all_checks.ps1, indefinitely. ui_responsive_sweep.mjs's own SKIP path
# (see its main()) already treats "DevTools port never came up" as an
# environment fact, not a FAIL; this wrapper extends the same treatment to
# the pathological case where node does not even get that far in time.
# Start-Process (not Start-Job) specifically so a timeout can `taskkill /T`
# the real process tree -- Stop-Job only tears down the job's runspace and
# was observed in testing to leave the actual node.exe (and any Chrome it
# had spawned) running as orphans, which is exactly the kind of leftover
# process that causes the port/profile-dir collisions this whole fix exists
# to prevent.
$stdoutFile = Join-Path $env:TEMP "kc-ui-sweep-stdout-$PID.txt"
$stderrFile = Join-Path $env:TEMP "kc-ui-sweep-stderr-$PID.txt"
$proc = Start-Process -FilePath $node.Source -ArgumentList @($sweepScript) `
    -NoNewWindow -PassThru `
    -RedirectStandardOutput $stdoutFile -RedirectStandardError $stderrFile

$finished = $proc.WaitForExit(180000)
if (-not $finished) {
    & taskkill /PID $proc.Id /T /F 2>&1 | Out-Null
    Write-Host "check_ui_responsive_sweep.ps1: SKIP -- sweep did not finish within 180s (node/Chrome startup stalled). Environment condition, not evidence of a UI regression -- re-run when the machine is less loaded." -ForegroundColor Yellow
    Remove-Item -Path $stdoutFile, $stderrFile -Force -ErrorAction SilentlyContinue
    exit 3
}

$stdout = if (Test-Path $stdoutFile) { Get-Content -Raw $stdoutFile } else { "" }
$stderrText = if (Test-Path $stderrFile) { Get-Content -Raw $stderrFile } else { "" }
Remove-Item -Path $stdoutFile, $stderrFile -Force -ErrorAction SilentlyContinue
if ($stdout) { Write-Host $stdout }
if ($stderrText) { Write-Host $stderrText }
$text = "$stdout`n$stderrText"

if ($text -match 'ui_responsive_sweep: SKIPPED') {
    Write-Host "check_ui_responsive_sweep.ps1: SKIP -- sweep SKIPPED internally (see reason above)."
    exit 3
}
if ($text -match '\d+ of \d+ .* checks FAILED') {
    throw "check_ui_responsive_sweep.ps1: sweep FAILED -- see output above for the specific (page, width, assertion) failures."
}
if ($text -notmatch 'All \d+ .* checks passed') {
    throw "check_ui_responsive_sweep.ps1: sweep ended without a recognized PASS/FAIL/SKIP marker -- treating as a failure. Output above."
}

Write-Host "check_ui_responsive_sweep.ps1: sweep passed."
exit 0
