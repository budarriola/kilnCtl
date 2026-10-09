# check_build_lock_holder.ps1 -- behavioural test of tools/build_lock.ps1's
# holder record + progress-based wait (see the HOLDER RECORD comment there).
# Scenarios (all against a private lock name and record dir, no real build):
#   1. dead holder: a child takes the lock then is killed -> a waiter takes
#      over at once (abandoned mutex) and rewrites the record to its own pid.
#   2. stale record: a record naming a dead pid with a free mutex -> taken over
#      AND REPORTED ("stale record"); deleting the stale-record logic makes this
#      FAIL (the record is overwritten either way, so the report is what proves it).
#   3. live holder, no progress: waiter throws, message names holder pid.
#   4. live holder that releases within the window: waiter waits, then acquires.
#   5. a live holder whose progress keeps changing past StallSeconds is NOT given
#      up on (a constant progress token makes the waiter throw -> FAIL).
#   6. PID reuse: a live pid with the wrong start time counts as dead.
#   7. holder with NO record (older build_lock.ps1 on the shared mutex): the waiter
#      does not throw "no progress" at StallSeconds; only MaxWaitSeconds ends the wait.
#   8. nested Enter in one process: the inner Exit keeps the record, the outer removes it.
#   9. the progress token is sampled at most every ProgressCheckSeconds (default 30),
#      not on every poll.
$ErrorActionPreference = "Continue"
. (Join-Path $PSScriptRoot "build_lock.ps1")
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("bl_test_" + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Path $tmp | Out-Null
$env:KILNCTL_BUILD_LOCK_DIR = $tmp
$blLib = Join-Path $PSScriptRoot "build_lock.ps1"
$fails = @()
function Start-Holder([string]$name, [int]$holdSeconds, [string]$progressDir = "", [switch]$NoRecord) {
    $work = "Start-Sleep -Seconds $holdSeconds"
    if ($progressDir) { $work = "for (`$i = 0; `$i -lt $holdSeconds; `$i++) { Set-Content -LiteralPath '$progressDir\tick.txt' -Value `$i; Start-Sleep -Seconds 1 }" }
    if ($NoRecord) {
        # Raw mutex only, no holder record: what an older build_lock.ps1 does.
        $cmd = "`$m = New-Object System.Threading.Mutex(`$false, '$(Get-BuildLockMutexName -Name $name)'); [void]`$m.WaitOne(); Write-Host READY; $work; `$m.ReleaseMutex()"
    } else {
        $cmd = ". '$blLib'; `$l = Enter-BuildLock -Name '$name'; Write-Host READY; $work; Exit-BuildLock -Lock `$l"
    }
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = "powershell.exe"
    $psi.Arguments = "-NoProfile -ExecutionPolicy Bypass -Command `"$cmd`""
    $psi.UseShellExecute = $false; $psi.RedirectStandardOutput = $true; $psi.CreateNoWindow = $true
    $psi.EnvironmentVariables["KILNCTL_BUILD_LOCK_DIR"] = $tmp
    $proc = [System.Diagnostics.Process]::Start($psi)
    while (-not $proc.StandardOutput.EndOfStream) { if ($proc.StandardOutput.ReadLine() -match "READY") { break } }
    return $proc
}
# Enter-BuildLock, capturing the Write-Host (information stream) text it prints.
function Enter-Captured([string]$name, [hashtable]$more = @{}) {
    $script:captured = (& { $script:lk = Enter-BuildLock -Name $name @more } 6>&1 | Out-String)
    return $script:lk
}
try {
    # 1. dead holder
    $n = "t_dead_$PID"
    $h = Start-Holder $n 600
    $rec = Read-BuildLockHolder -Name $n
    if (-not $rec -or $rec.pid -ne $h.Id) { $fails += "1: holder record missing/wrong pid" }
    $h.Kill(); $h.WaitForExit()
    $l = Enter-BuildLock -Name $n -StallSeconds 30 -PollSeconds 1
    $rec = Read-BuildLockHolder -Name $n
    if ($rec.pid -ne $PID) { $fails += "1: takeover did not rewrite the record to this pid" }
    Exit-BuildLock -Lock $l
    if (Read-BuildLockHolder -Name $n) { $fails += "1: record not removed on release" }

    # 2. stale record, free mutex
    $n = "t_stale_$PID"
    $p = Get-BuildLockRecordPath -Name $n
    '{"schema":1,"pid":999999,"proc_start_utc":"2000-01-01T00:00:00.0000000Z","held_since_utc":"2000-01-01T00:00:00Z","host":"x","cmd":"stale"}' | Set-Content -LiteralPath $p
    $l = Enter-Captured $n @{ StallSeconds = 30; PollSeconds = 1 }
    if ((Read-BuildLockHolder -Name $n).pid -ne $PID) { $fails += "2: stale record not replaced" }
    if ($script:captured -notmatch "stale record \(pid 999999 is dead\)") { $fails += "2: stale record takeover not reported: $($script:captured)" }
    Exit-BuildLock -Lock $l

    # 3. live holder without progress -> fail loud naming holder
    $n = "t_live_$PID"
    $h = Start-Holder $n 120
    $msg = $null
    try { $l = Enter-BuildLock -Name $n -StallSeconds 8 -PollSeconds 1; Exit-BuildLock -Lock $l } catch { $msg = "$_" }
    if (-not $msg) { $fails += "3: waiter acquired a lock held by a live process" }
    elseif ($msg -notmatch "pid $($h.Id)\b") { $fails += "3: failure did not name the holder pid: $msg" }
    else { Write-Host "scenario 3 message: $msg" }
    $h.Kill(); $h.WaitForExit()

    # 4. live holder releases inside the window -> waiter waits then acquires
    $n = "t_wait_$PID"
    $h = Start-Holder $n 6
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $l = Enter-BuildLock -Name $n -StallSeconds 60 -PollSeconds 1
    if ($sw.Elapsed.TotalSeconds -lt 3) { $fails += "4: did not actually wait for the live holder" }
    Exit-BuildLock -Lock $l
    $h.WaitForExit()

    # 5. live holder with CHANGING progress is never given up on, even past StallSeconds
    $n = "t_prog_$PID"
    $pd = Join-Path $tmp "progress_a"; New-Item -ItemType Directory -Path $pd | Out-Null
    $h = Start-Holder $n 14 $pd
    $msg = $null; $got = $false
    try { $l = Enter-BuildLock -Name $n -StallSeconds 5 -MaxWaitSeconds 60 -PollSeconds 1 -ProgressCheckSeconds 1 -ProgressPath @($pd); $got = $true; Exit-BuildLock -Lock $l } catch { $msg = "$_" }
    if (-not $got) { $fails += "5: waiter gave up on a live holder whose progress kept changing: $msg" }
    $h.WaitForExit()

    # 6. PID reuse: live pid with the wrong start time counts as dead
    $live = [pscustomobject]@{ pid = $PID; proc_start_utc = (Get-BuildLockProcStart -ProcId $PID) }
    if (-not (Test-BuildLockHolderAlive -Holder $live)) { $fails += "6: a live pid with the right start time must count as alive" }
    $reused = [pscustomobject]@{ pid = $PID; proc_start_utc = "2001-01-01T00:00:00.0000000Z" }
    if (Test-BuildLockHolderAlive -Holder $reused) { $fails += "6: a live pid with the WRONG start time (PID reuse) must count as dead" }
    $n = "t_reuse_$PID"
    $sleeper = Start-Process -FilePath powershell.exe -ArgumentList "-NoProfile", "-Command", "Start-Sleep -Seconds 60" -PassThru -WindowStyle Hidden
    try {
        $p = Get-BuildLockRecordPath -Name $n
        ('{"schema":1,"pid":' + $sleeper.Id + ',"proc_start_utc":"2001-01-01T00:00:00.0000000Z","held_since_utc":"2001-01-01T00:00:00Z","host":"x","cmd":"reused pid"}') | Set-Content -LiteralPath $p
        $l = Enter-Captured $n @{ StallSeconds = 30; PollSeconds = 1 }
        if ($script:captured -notmatch ("stale record \(pid " + $sleeper.Id + " is dead\)")) { $fails += "6: reused-pid record not treated as stale: $($script:captured)" }
        Exit-BuildLock -Lock $l
    } finally { try { $sleeper.Kill() } catch {} }

    # 7. holder without a record: no "no progress" failure at StallSeconds; MaxWaitSeconds ends it
    $n = "t_norec_$PID"
    $h = Start-Holder $n 120 -NoRecord
    $sw = [Diagnostics.Stopwatch]::StartNew(); $msg = $null
    try { $l = Enter-BuildLock -Name $n -StallSeconds 2 -MaxWaitSeconds 8 -PollSeconds 1; Exit-BuildLock -Lock $l } catch { $msg = "$_" }
    if (-not $msg) { $fails += "7: waiter acquired a mutex held by a record-less holder" }
    elseif ($msg -match "no progress") { $fails += "7: record-less holder reported as 'no progress': $msg" }
    elseif ($msg -notmatch "waited 8s in total") { $fails += "7: expected the MaxWaitSeconds failure: $msg" }
    elseif ($sw.Elapsed.TotalSeconds -lt 7) { $fails += "7: gave up after only $([int]$sw.Elapsed.TotalSeconds)s" }
    $h.Kill(); $h.WaitForExit()

    # 8. nested Enter in one process
    $n = "t_nest_$PID"
    $outer = Enter-BuildLock -Name $n
    $inner = Enter-BuildLock -Name $n
    Exit-BuildLock -Lock $inner
    if (-not (Read-BuildLockHolder -Name $n)) { $fails += "8: inner Exit deleted the record while the outer still holds the lock" }
    Exit-BuildLock -Lock $outer
    if (Read-BuildLockHolder -Name $n) { $fails += "8: outer Exit did not remove the record" }

    # 9. progress token sampled at most every ProgressCheckSeconds (default 30), not per poll
    $n = "t_thr_$PID"
    $script:tokCalls = 0
    function Get-BuildLockProgressToken { param($Holder, [string[]]$ProgressPath) $script:tokCalls++; return "const" }
    $h = Start-Holder $n 9
    $l = Enter-BuildLock -Name $n -StallSeconds 600 -PollSeconds 1
    Exit-BuildLock -Lock $l
    $h.WaitForExit()
    if ($script:tokCalls -lt 1 -or $script:tokCalls -gt 2) { $fails += "9: progress token sampled $($script:tokCalls) times over a ~9 s wait; expected 1-2 (30 s throttle)" }
} finally {
    Remove-Item Env:KILNCTL_BUILD_LOCK_DIR -ErrorAction SilentlyContinue
    [IO.Directory]::Delete($tmp, $true)
}
if ($fails.Count -gt 0) {
    $fails | ForEach-Object { Write-Host "FAILED: $_" -ForegroundColor Red }
    exit 1
}
Write-Host "PASS: build lock holder record: dead-holder takeover, stale record reported, live-holder fail-loud naming pid, live-holder wait, changing-progress holder kept, PID reuse, record-less holder, nested Enter, token throttle" -ForegroundColor Green
exit 0
