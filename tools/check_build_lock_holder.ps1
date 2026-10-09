# check_build_lock_holder.ps1 -- behavioural test of tools/build_lock.ps1's
# holder record + progress-based wait (see the HOLDER RECORD comment there).
# Scenarios (all against a private lock name and record dir, no real build):
#   1. dead holder: a child takes the lock then is killed -> a waiter takes
#      over at once (abandoned mutex) and rewrites the record to its own pid.
#   2. stale record: a record naming a dead pid with a free mutex -> taken over.
#   3. live holder, no progress: waiter throws, message names holder pid.
#   4. live holder that releases within the window: waiter waits, then acquires.
$ErrorActionPreference = "Continue"
. (Join-Path $PSScriptRoot "build_lock.ps1")
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("bl_test_" + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Path $tmp | Out-Null
$env:KILNCTL_BUILD_LOCK_DIR = $tmp
$blLib = Join-Path $PSScriptRoot "build_lock.ps1"
$fails = @()
function Start-Holder([string]$name, [int]$holdSeconds) {
    $cmd = ". '$blLib'; `$l = Enter-BuildLock -Name '$name'; Write-Host READY; Start-Sleep -Seconds $holdSeconds; Exit-BuildLock -Lock `$l"
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = "powershell.exe"
    $psi.Arguments = "-NoProfile -ExecutionPolicy Bypass -Command `"$cmd`""
    $psi.UseShellExecute = $false; $psi.RedirectStandardOutput = $true; $psi.CreateNoWindow = $true
    $psi.EnvironmentVariables["KILNCTL_BUILD_LOCK_DIR"] = $tmp
    $proc = [System.Diagnostics.Process]::Start($psi)
    while (-not $proc.StandardOutput.EndOfStream) { if ($proc.StandardOutput.ReadLine() -match "READY") { break } }
    return $proc
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
    $l = Enter-BuildLock -Name $n -StallSeconds 30 -PollSeconds 1
    if ((Read-BuildLockHolder -Name $n).pid -ne $PID) { $fails += "2: stale record not replaced" }
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
} finally {
    Remove-Item Env:KILNCTL_BUILD_LOCK_DIR -ErrorAction SilentlyContinue
    [IO.Directory]::Delete($tmp, $true)
}
if ($fails.Count -gt 0) {
    $fails | ForEach-Object { Write-Host "FAILED: $_" -ForegroundColor Red }
    exit 1
}
Write-Host "PASS: build lock holder record: dead-holder takeover, stale record, live-holder fail-loud naming pid, live-holder wait" -ForegroundColor Green
exit 0
