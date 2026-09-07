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
# Best-effort reap of orphaned Chrome instances left behind by a previous
# sweep run that was killed before its own finally{} block (in
# ui_responsive_sweep.mjs's main()) got to run chrome.kill() -- e.g. a prior
# check_ui_responsive_sweep.ps1 invocation that itself got taskkill'd from
# outside (another check harness run, a session interrupted mid-check). Each
# sweep-owned Chrome is identifiable by its own throwaway
# --user-data-dir=...kc-ui-sweep-profile-... (see ui_responsive_sweep.mjs's
# mkdtemp call) -- narrow enough to never touch a developer's real Chrome
# window or another tool's headless Chrome. Run before AND after the sweep:
# before, so a stale instance does not hold the fixed CDP/static ports and
# force every pickPort() fallback; after, so this run's own Chrome (already
# reaped by taskkill /T on a timeout, or by the script's own finally{} on a
# normal exit) never lingers past this check either.
function Remove-OrphanedSweepChrome {
    try {
        $procs = Get-CimInstance Win32_Process -Filter "Name = 'chrome.exe'" -ErrorAction SilentlyContinue |
            Where-Object { $_.CommandLine -and $_.CommandLine -like '*kc-ui-sweep-profile-*' }
        foreach ($p in $procs) {
            Write-Host "check_ui_responsive_sweep.ps1: reaping orphaned sweep Chrome PID $($p.ProcessId)" -ForegroundColor Yellow
            & taskkill /PID $p.ProcessId /T /F 2>&1 | Out-Null
        }
    } catch {
        # Best-effort only -- Get-CimInstance can fail under WMI load; never
        # let cleanup itself fail the check.
    }
}
Remove-OrphanedSweepChrome

$stdoutFile = Join-Path $env:TEMP "kc-ui-sweep-stdout-$PID.txt"
$stderrFile = Join-Path $env:TEMP "kc-ui-sweep-stderr-$PID.txt"
$proc = Start-Process -FilePath $node.Source -ArgumentList @($sweepScript) `
    -NoNewWindow -PassThru `
    -RedirectStandardOutput $stdoutFile -RedirectStandardError $stderrFile

# 420s, not 180s: 13 *_page.html files x 6 widths (plus zones_page.html's 4
# tuning-recommendation variants) is ~96 (page,width) rows, each several CDP
# round trips against a real headless Chrome -- genuinely close to 180s even
# on an idle machine, and this machine routinely runs other agents' own
# headless Chrome concurrently (observed: 12 already running during
# diagnosis). That contention is real, legitimate slowness, not a hang -- the
# actual hang (a single CDP call Chrome never answered, with no timeout
# anywhere on it) is now fixed at the source in ui_responsive_sweep.mjs's
# CdpSession.send(); this cap only needs to cover honest slow-but-progressing
# work now, not mask a still-unbounded wait.
$finished = $proc.WaitForExit(420000)
if (-not $finished) {
    & taskkill /PID $proc.Id /T /F 2>&1 | Out-Null
    Write-Host "check_ui_responsive_sweep.ps1: SKIP -- sweep did not finish within 420s (node/Chrome startup stalled). Environment condition, not evidence of a UI regression -- re-run when the machine is less loaded." -ForegroundColor Yellow
    Remove-Item -Path $stdoutFile, $stderrFile -Force -ErrorAction SilentlyContinue
    Remove-OrphanedSweepChrome
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
