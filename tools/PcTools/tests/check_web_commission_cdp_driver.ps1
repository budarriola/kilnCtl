# check_web_commission_cdp_driver.ps1 -- standing wrapper around
# web_commission_cdp_driver.test.mjs (this directory), the end-to-end test
# of tools/PcTools/scripts/_web_commission_cdp.mjs's selector kinds
# ("aria-label"/"css"), its --steps runner, and CdpSession.waitForPost()'s
# post-consumption cursor.
#
# Why a wrapper at all: that driver is the only thing standing between
# web_commission_row.py's live commissioning rows and a click on the WRONG
# control of a real board -- a per-row delete checkbox, a bulk delete, a
# Danger Mode confirm. Its selector resolution and its "did the POST I was
# waiting for actually complete, and was it THE one" reporting have no
# other coverage in the suite, and _cdp_post_status() turns that report
# straight into a row's PASS/FAIL verdict. Without this wrapper the test
# existed but nothing ever ran it.
#
# Wired into tools/run_all_checks.ps1's check_*.ps1 discovery like every
# other guard in this repo (glob-discovered, not hand-listed), and pulled
# into that script's PHASE 3 (serial, -MaxParallel 1) alongside
# check_ui_responsive_sweep.ps1 for the same reason: it drives real
# headless Chrome over CDP with its own internal wall-clock timeouts, and
# phase 2's 8-way parallel throttle is exactly the contention that turned
# those timeouts into misfires on 2026-09-17.
#
# Deliberately does NOT reap stray chrome.exe the way
# check_ui_responsive_sweep.ps1 does. That script can safely taskkill any
# Chrome holding a `kc-ui-sweep-profile-*` user-data-dir because only the
# sweep ever creates one. This test's driver uses `kc-web-commission-*`,
# and so does every LIVE commissioning run of web_commission_row.py against
# the real board -- reaping by that prefix would tear down an authorized
# bench run mid-row. The test's own finally{} block kills its Chrome, and
# the timeout path below kills the process TREE, so no reaper is needed.
#
# Usage: powershell -File tools\PcTools\tests\check_web_commission_cdp_driver.ps1
$ErrorActionPreference = "Stop"

$testScript = Join-Path $PSScriptRoot "web_commission_cdp_driver.test.mjs"
if (-not (Test-Path $testScript)) {
    throw "check_web_commission_cdp_driver.ps1: web_commission_cdp_driver.test.mjs not found at $testScript"
}

$node = Get-Command node -ErrorAction SilentlyContinue
if (-not $node) {
    Write-Host "check_web_commission_cdp_driver.ps1: SKIP -- no ``node`` on PATH, cannot run the driver test." -ForegroundColor Yellow
    # exit 3 is run_all_checks.ps1's reserved SKIP status, same convention
    # as check_ui_responsive_sweep.ps1's own no-node branch.
    exit 3
}

# Bounded taskkill of a whole process tree (never blocks the check on a
# wedged taskkill/handle).
function Stop-ProcessTreeBounded {
    param([int]$ProcessId)
    try {
        $tk = Start-Process -FilePath taskkill -ArgumentList @('/PID', "$ProcessId", '/T', '/F') -NoNewWindow -PassThru -ErrorAction Stop
        if (-not $tk.WaitForExit(15000)) { try { $tk.Kill() } catch {} }
    } catch {
        # best-effort
    }
}

$stdoutFile = Join-Path $env:TEMP "kc-web-commission-cdp-stdout-$PID.txt"
$stderrFile = Join-Path $env:TEMP "kc-web-commission-cdp-stderr-$PID.txt"
# Start-Process (not `&`, not Start-Job) so a timeout can taskkill /T the
# real process tree: Stop-Job only tears down the runspace and leaves the
# node.exe and any Chrome it spawned running as orphans.
$proc = Start-Process -FilePath $node.Source -ArgumentList @($testScript) `
    -NoNewWindow -PassThru `
    -RedirectStandardOutput $stdoutFile -RedirectStandardError $stderrFile

# 180s: the test performs four short driver runs, each a full Chrome
# launch (~2-4s idle, several times that on a machine already running a
# dozen concurrent headless Chrome from sibling agents). Generous enough
# to cover honest slow-but-progressing work, not so generous that a real
# hang holds up run_all_checks.ps1 for long.
$finished = $proc.WaitForExit(180000)
if (-not $finished) {
    Stop-ProcessTreeBounded -ProcessId $proc.Id
    $partial = if (Test-Path $stdoutFile) { Get-Content -Raw $stdoutFile } else { "" }
    Remove-Item -Path $stdoutFile, $stderrFile -Force -ErrorAction SilentlyContinue
    if ($partial) { Write-Host $partial }
    # A hang is a FAIL, not a SKIP (2026-10-04); see check_ui_responsive_sweep.ps1.
    throw "check_web_commission_cdp_driver.ps1: FAIL -- test did not finish within 180s; node/Chrome process tree killed. The harness hung."
}

$stdout = if (Test-Path $stdoutFile) { Get-Content -Raw $stdoutFile } else { "" }
$stderrText = if (Test-Path $stderrFile) { Get-Content -Raw $stderrFile } else { "" }
Remove-Item -Path $stdoutFile, $stderrFile -Force -ErrorAction SilentlyContinue
if ($stdout) { Write-Host $stdout }
if ($stderrText) { Write-Host $stderrText }
$text = "$stdout`n$stderrText"

# Graded on the test's own printed markers, NOT on $proc.ExitCode.
# Start-Process -PassThru's object returns an EMPTY ExitCode here in
# Windows PowerShell 5.1 once the bounded WaitForExit(ms) overload is used
# -- observed while writing this wrapper: a run whose test had just printed
# 14/14 was reported FAILED because "" -ne 0. check_ui_responsive_sweep.ps1
# grades on markers for the same reason; this follows it.

# The test's own no-browser SKIP. Chrome/Edge not being installed is an
# environment fact, not a driver defect.
if ($text -match 'web_commission_cdp_driver\.test\.mjs: SKIP') {
    Write-Host "check_web_commission_cdp_driver.ps1: SKIP -- no Chrome/Edge binary found (see output above)." -ForegroundColor Yellow
    exit 3
}
# A failed assertion prints "FAIL: <label>" and must win over everything
# below, including a malformed tally.
if ($text -match '(?m)^FAIL:') {
    throw "check_web_commission_cdp_driver.ps1: driver test FAILED -- see the FAIL: lines above for the specific assertion."
}
# No tally at all means the test returned before running its assertions --
# the "green with zero coverage" shape this repo has shipped before.
if ($text -notmatch '(\d+)/(\d+) assertions passed') {
    throw "check_web_commission_cdp_driver.ps1: test printed no assertion tally -- treating as a failure, not a pass. Output above."
}
if ($Matches[1] -ne $Matches[2]) {
    throw "check_web_commission_cdp_driver.ps1: $($Matches[1])/$($Matches[2]) assertions passed -- not all assertions passed."
}
if ([int]$Matches[2] -lt 14) {
    throw "check_web_commission_cdp_driver.ps1: only $($Matches[2]) assertions ran (expected at least 14) -- assertions have been removed or the test returned early. Lower this floor deliberately if that was intended."
}

Write-Host "check_web_commission_cdp_driver.ps1: driver test passed ($($Matches[2]) assertions)."
exit 0
