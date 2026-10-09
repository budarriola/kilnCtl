# build_lock.ps1 -- shared helper for check_*.ps1 scripts that configure/build
# a CMake+ninja tree in place. Two concurrent runs against the SAME build
# directory (e.g. two `run_all_checks.ps1` invocations, or a manual run
# overlapping a scheduled one) race cmake's `.ninja_log`/`.ninja_deps`
# recompaction and CMake's generated-header writes -- confirmed 2026-09-06
# by running check_bootloader_builds.ps1 twice concurrently: one side got
# "CMake Error ... generate_config_header.cmake:29 (configure_file): No such
# file or directory" (the file the other side's configure was mid-writing).
#
# Fix: serialize on a global named Mutex keyed off the build directory path,
# so concurrent invocations queue up and run one at a time instead of
# corrupting each other's build tree. This is a mutex (not a per-run temp
# build dir) because these builds reconfigure/rebuild an existing tree
# in-place for incremental speed -- a fresh build dir per run would pay the
# full first-time SDK configure cost every time.
#
# Usage (dot-source, then wrap the build in a try/finally):
#   . (Join-Path $PSScriptRoot "..\..\..\tools\build_lock.ps1")   # adjust depth
#   $lock = Enter-BuildLock -Name "saftyfw_bootloader_build"
#   try {
#       ... cmake / ninja calls ...
#   } finally {
#       Exit-BuildLock -Lock $lock
#   }

# THE MUTEX NAME IS PUBLIC, NOT AN INTERNAL DETAIL (2026-09-16, N3 of
# docs/audits/review_check00_d1_d6_closure_2026-09-16.md).
# check_00_kilnfw_target_build.ps1's prune pass has to build the name of a
# lock it does not own, so that it can test whether ANOTHER tree's check run
# is currently using the build directory it is about to delete. Until now it
# spelled "Global\kilnCtl_buildlock_" out itself, giving two copies of a
# naming contract expressed nowhere -- CLAUDE.md's "reset one side of a pair"
# class, and a one-character drift there was measured to flip a held-lock case
# straight back to deleting a directory mid-build, silently. Both sides now
# call this one function, so the contract cannot drift.
function Get-BuildLockMutexName {
    param([Parameter(Mandatory = $true)][string]$Name)
    # "Global\" makes this visible across all sessions/users on the machine,
    # not just the current one -- two separate agent/session processes must
    # see the same mutex.
    return "Global\kilnCtl_buildlock_$Name"
}

# HOLDER RECORD + PROGRESS-BASED WAIT (2026-10-08). The old waiter gave up
# after a flat 900 s and blamed "another run appears stuck", but the usual
# holder is not stuck: a KilnFW origin build can legitimately take over an
# hour under load (it also queues for a heavy gate slot while holding the
# lock). A dead holder never was the problem -- the OS marks its mutex
# abandoned and the next WaitOne takes it -- so the flat timeout only ever
# fired on live, slow holders.
#
# Now: the holder writes <lockdir>\<name>.holder.json (pid, process start
# time, start of hold). A waiter polls the mutex and, while the holder is
# ALIVE (pid exists AND start time matches, so PID reuse is not fooled),
# keeps waiting as long as it sees progress: the newest mtime under
# -ProgressPath, or the CPU time of the holder's process tree, changed within
# -StallSeconds. A holder alive but with no progress for StallSeconds, or
# any wait past MaxWaitSeconds, makes the waiter fail loud NAMING the
# holder (pid, since, age). A record whose pid is dead is stale: it is
# reported, the lock is taken over (the mutex is abandoned or free, so
# WaitOne returns at once) and the record is rewritten.
# Result reuse for the same commit lives one level up (pushed_build_stamp.ps1).
# KILNCTL_BUILD_LOCK_DIR overrides the record directory (default C:\wt\buildlocks).

function Get-BuildLockRecordPath {
    param([Parameter(Mandatory = $true)][string]$Name)
    $dir = $env:KILNCTL_BUILD_LOCK_DIR
    if ([string]::IsNullOrEmpty($dir)) { $dir = "C:\wt\buildlocks" }
    return (Join-Path $dir (($Name -replace '[^A-Za-z0-9_.-]', '_') + ".holder.json"))
}

function Get-BuildLockProcStart {
    param([int]$ProcId)
    try { return (Get-Process -Id $ProcId -ErrorAction Stop).StartTime.ToUniversalTime().ToString("o") } catch { return $null }
}

# Returns the record object, or $null if absent/unreadable.
function Read-BuildLockHolder {
    param([Parameter(Mandatory = $true)][string]$Name)
    $p = Get-BuildLockRecordPath -Name $Name
    if (-not (Test-Path -LiteralPath $p)) { return $null }
    try { return (Get-Content -LiteralPath $p -Raw -ErrorAction Stop | ConvertFrom-Json -ErrorAction Stop) } catch { return $null }
}

function Test-BuildLockHolderAlive {
    param($Holder)
    if ($null -eq $Holder -or $null -eq $Holder.pid) { return $false }
    $cur = Get-BuildLockProcStart -ProcId ([int]$Holder.pid)
    return ($null -ne $cur -and $cur -eq $Holder.proc_start_utc)
}

function Write-BuildLockHolder {
    param([Parameter(Mandatory = $true)][string]$Name, [string]$Note = "")
    $p = Get-BuildLockRecordPath -Name $Name
    $dir = Split-Path -Parent $p
    try {
        if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
        $obj = [ordered]@{
            schema = 1; name = $Name; pid = $PID
            proc_start_utc = (Get-BuildLockProcStart -ProcId $PID)
            held_since_utc = (Get-Date).ToUniversalTime().ToString("o")
            host = $env:COMPUTERNAME; cmd = $Note
        }
        $tmp = "$p.$PID.tmp"
        ($obj | ConvertTo-Json) | Set-Content -LiteralPath $tmp -Encoding UTF8
        Move-Item -LiteralPath $tmp -Destination $p -Force
    } catch { Write-Host "warning: could not write build-lock holder record: $_" -ForegroundColor Yellow }
}

function Get-BuildLockProgressToken {
    param($Holder, [string[]]$ProgressPath)
    $parts = @()
    foreach ($pp in $ProgressPath) {
        if ($pp -and (Test-Path -LiteralPath $pp)) {
            $m = Get-ChildItem -LiteralPath $pp -Recurse -File -ErrorAction SilentlyContinue |
                Measure-Object -Property LastWriteTimeUtc -Maximum
            if ($m.Maximum) { $parts += "m:$($m.Maximum.Ticks):$($m.Count)" }
        }
    }
    try {
        $ids = @([int]$Holder.pid)
        $all = @(Get-CimInstance Win32_Process -ErrorAction Stop | Select-Object ProcessId, ParentProcessId)
        for ($k = 0; $k -lt 6; $k++) {
            $kids = @($all | Where-Object { ($ids -contains $_.ParentProcessId) -and ($ids -notcontains $_.ProcessId) } | ForEach-Object { $_.ProcessId })
            if ($kids.Count -eq 0) { break }
            $ids += $kids
        }
        $cpu = 0.0
        foreach ($i in $ids) { try { $cpu += (Get-Process -Id $i -ErrorAction Stop).TotalProcessorTime.TotalSeconds } catch {} }
        $parts += ("c:{0:N0}:{1}" -f $cpu, $ids.Count)
    } catch {}
    return ($parts -join "|")
}

function Format-BuildLockHolder {
    param($Holder, [string]$Name)
    if ($null -eq $Holder) { return "holder unknown (no record at $(Get-BuildLockRecordPath -Name $Name))" }
    $age = ""
    try { $age = ", held for {0:N0} min" -f ((Get-Date).ToUniversalTime() - [datetime]::Parse($Holder.held_since_utc).ToUniversalTime()).TotalMinutes } catch {}
    return "holder pid $($Holder.pid) on $($Holder.host) since $($Holder.held_since_utc)$age, cmd: $($Holder.cmd)"
}

function Enter-BuildLock {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [int]$StallSeconds = 3600,
        [int]$MaxWaitSeconds = 21600,
        [int]$PollSeconds = 5,
        [string[]]$ProgressPath = @()
    )

    $mutexName = Get-BuildLockMutexName -Name $Name
    $mutex = New-Object System.Threading.Mutex($false, $mutexName)
    $note = [Environment]::CommandLine
    if ($note.Length -gt 200) { $note = $note.Substring(0, 200) }

    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $announced = $false
    $lastToken = $null
    $lastProgress = [System.Diagnostics.Stopwatch]::StartNew()
    while ($true) {
        $acquired = $false
        $wait = 0
        if ($announced) { $wait = $PollSeconds }
        try { $acquired = $mutex.WaitOne([TimeSpan]::FromSeconds($wait)) }
        catch [System.Threading.AbandonedMutexException] {
            # A previous holder crashed/was killed without releasing -- we own it now.
            $acquired = $true
            Write-Host "build lock '$Name': previous holder died without releasing; taking over" -ForegroundColor Yellow
        }
        if ($acquired) {
            $old = Read-BuildLockHolder -Name $Name
            if ($old -and $old.pid -ne $PID -and -not (Test-BuildLockHolderAlive -Holder $old)) {
                Write-Host "build lock '$Name': stale record (pid $($old.pid) is dead); taking over" -ForegroundColor Yellow
            }
            Write-BuildLockHolder -Name $Name -Note $note
            if ($announced) { Write-Host "acquired build lock '$Name'" -ForegroundColor Yellow }
            return [PSCustomObject]@{ Mutex = $mutex; Name = $Name }
        }
        $holder = Read-BuildLockHolder -Name $Name
        if (-not $announced) {
            Write-Host "waiting for build lock '$Name': $(Format-BuildLockHolder -Holder $holder -Name $Name)" -ForegroundColor Yellow
            $announced = $true
        }
        if ($holder -and (Test-BuildLockHolderAlive -Holder $holder)) {
            $tok = Get-BuildLockProgressToken -Holder $holder -ProgressPath $ProgressPath
            if ($tok -ne $lastToken) { $lastToken = $tok; $lastProgress.Restart() }
        }
        if ($lastProgress.Elapsed.TotalSeconds -ge $StallSeconds -or $sw.Elapsed.TotalSeconds -ge $MaxWaitSeconds) {
            $why = "no progress from the holder for ${StallSeconds}s"
            if ($sw.Elapsed.TotalSeconds -ge $MaxWaitSeconds) { $why = "waited ${MaxWaitSeconds}s in total" }
            $mutex.Dispose()
            throw "build lock '$Name' not acquired: $why; $(Format-BuildLockHolder -Holder $holder -Name $Name)"
        }
    }
}

function Exit-BuildLock {
    param(
        [Parameter(Mandatory = $true)]$Lock
    )
    try {
        $rec = Read-BuildLockHolder -Name $Lock.Name
        if ($rec -and $rec.pid -eq $PID) {
            [System.IO.File]::Delete((Get-BuildLockRecordPath -Name $Lock.Name))
        }
    } catch {}
    try {
        $Lock.Mutex.ReleaseMutex()
    } catch {
        # already released/abandoned -- nothing to do
    }
    $Lock.Mutex.Dispose()
}
