# build_gate.ps1 -- machine-wide build admission gate.
#
# This PC hard-froze five times in one week (Kernel-Power 41) when 7-11 agents
# each ran a full ESP-IDF target build or MSVC host-test build at once in
# separate C:\wt worktrees. tools/build_lock.ps1 only serializes two writers of
# the SAME build directory; this gate bounds how many compiles run at once
# ACROSS ALL SESSIONS.
#
# THE RULES (owner, 2026-10-07)
#   1. A slot is held ONLY around the actual compile/link command. Never while
#      waiting on a build lock, running vcvarsall/toolchain setup, running
#      tests, polling/sleeping, or around a child that also takes a slot.
#      Order for a caller: setup (Import-KilnVcvarsEnv) -> Enter-BuildLock ->
#      Enter-KilnBuildGate around ONE compile -> Exit-KilnBuildGate at once.
#      tools/check_build_gate_usage.ps1 lints callers for this.
#   2. Re-entrant: the same process (global state) or a child process (env
#      KILNCTL_BUILD_GATE_HELD, verified against the holder's record) that
#      asks again while a slot is held gets a no-op handle, never a second slot.
#   3. Fair: waiters queue by ticket file (FIFO). Only the head of the queue
#      touches the slots, and it waits on ALL slots at once (WaitAny), so a
#      slot freeing at any index is noticed and wake order is arrival order.
#   4. An AbandonedMutexException (previous holder died) counts as acquired and
#      is logged.
#   5. Visibility + self-timeout, no killing of strangers: each holder writes
#      <dir>\<lane>\slot<i>.json (pid, command line, phase, start, worktree)
#      and removes it on release (try/finally). `build_gate.ps1 -Status` prints
#      every slot (held/free/stale, holder pid+cmd, age, pid alive, compiler
#      child running). A record whose pid is dead is STALE and ignored. The
#      HOLDER enforces KILNCTL_BUILD_GATE_MAX_HOLD_SEC (default 2700 = 45 min):
#      past it, the holder kills ITS OWN descendant processes, releases the slot
#      and Exit-KilnBuildGate throws naming the slot and time held. No waiter
#      ever kills another process.
#
# SLOT COUNTS ARE MACHINE-WIDE, not per-tree (2026-10-07 root cause: 58
# worktrees carried an old default of 2, so their waiters never opened slots
# 2-3). <dir>\config.json {"heavy_slots":N,"light_slots":N} (created with the
# defaults on first use) is AUTHORITATIVE; code default 4/4 only when it is
# unreadable. KILNCTL_BUILD_GATE_SLOTS / KILNCTL_LIGHT_GATE_SLOTS may only
# LOWER the count (clamped to <= config); heavy < 1 is refused (the heavy gate
# cannot be disabled from a worktree), light 0 is allowed. Whatever the
# count, a waiter also opens every slot index that has a live record (so a
# count lowered while higher slots are busy cannot hide a holder). A tree pinned
# to an OLD commit still runs its own old gate (default 2 slots, no records, no
# tickets): it contends only for slots 0-1 and is invisible to -Status except
# through the mutex probe (shown as "held-no-record"); it needs a rebase to
# join the shared count and the fair queue.
#
# Max hold is a SOFT limit: past it a holder with a live compiler (ninja/cmake/cl/link/gcc/cc1/ld in its
# tree) keeps the slot; it is killed when idle, or at the hard ceiling (KILNCTL_BUILD_GATE_MAX_HOLD_HARD_SEC /
# config.json max_hold_hard_sec, default 7200) regardless. Kills print 'KILLED BY BUILD GATE'.
# Max hold: the watchdog kills only descendants started AFTER the slot was taken
# (so a sibling process the holder started earlier survives), re-scanning until
# none remain; Exit-KilnBuildGate then throws.
#
# Dir: KILNCTL_BUILD_GATE_DIR (default C:\wt\.buildgate). Test-only mutex name
# prefixes: KILNCTL_BUILD_GATE_MUTEX_PREFIX / KILNCTL_LIGHT_GATE_MUTEX_PREFIX
# (must match mcpkit/buildgate.py). KILNCTL_BUILD_GATE_TIMEOUT_SEC: wait limit
# (default 3600). Lanes: "heavy" = target/host-test builds, "light" = single
# short cl/cmake invocations.
#
# Direct use:  powershell -File tools\build_gate.ps1 -Status [-Json]

$script:KilnGateDefaultSlots = 4
$script:KilnGateDefaultMaxHoldSec = 2700
$script:KilnGateTicketStaleSec = 120
$script:KilnGateCompilerRegex = '^(cl|link|ninja|cmake|cc1|cc1plus|ccache|gcc|g\+\+|ld|.+-gcc|.+-g\+\+|.+-ld|xtensa-.+|arm-none-eabi-.+)(\.exe)?$'
$script:KilnGateDefaultMaxHoldHardSec = 7200
$script:KilnGateHoldPollSec = 30

function Get-KilnBuildGateDir {
    $d = $env:KILNCTL_BUILD_GATE_DIR
    if ([string]::IsNullOrWhiteSpace($d)) { $d = "C:\wt\.buildgate" }
    return $d
}

function Get-KilnBuildGateLaneDir {
    param([string]$Lane = "heavy")
    return (Join-Path (Get-KilnBuildGateDir) $Lane)
}

# Write via temp file + atomic replace so a reader never sees a torn file.
function Write-KilnBuildGateFile {
    param([string]$Path, [string]$Text)
    $tmp = "$Path.$PID.$([guid]::NewGuid().ToString('N').Substring(0, 6)).tmp"
    [IO.File]::WriteAllText($tmp, $Text)
    try { Move-Item -LiteralPath $tmp -Destination $Path -Force -ErrorAction Stop } catch { Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue; throw }
}

function Get-KilnBuildGateConfigPath { return (Join-Path (Get-KilnBuildGateDir) "config.json") }

function Get-KilnBuildGateConfig {
    $path = Get-KilnBuildGateConfigPath
    try {
        if (-not (Test-Path -LiteralPath $path)) {
            $dir = Get-KilnBuildGateDir
            if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
            Write-KilnBuildGateFile -Path $path -Text ('{"heavy_slots": 4, "light_slots": 4}' + "`n")
        }
        return (Get-Content -LiteralPath $path -Raw | ConvertFrom-Json)
    } catch {
        return $null
    }
}

function Get-KilnBuildGateSlotCount {
    param([ValidateSet("heavy", "light")][string]$Lane = "heavy")
    $min = if ($Lane -eq "light") { 0 } else { 1 }
    $limit = $script:KilnGateDefaultSlots
    $cfg = Get-KilnBuildGateConfig
    if ($null -ne $cfg) {
        $val = if ($Lane -eq "light") { $cfg.light_slots } else { $cfg.heavy_slots }
        $n = 0
        if ($null -ne $val -and [int]::TryParse("$val", [ref]$n) -and $n -ge $min) { $limit = $n }
    }
    $envName = if ($Lane -eq "light") { "KILNCTL_LIGHT_GATE_SLOTS" } else { "KILNCTL_BUILD_GATE_SLOTS" }
    $raw = [Environment]::GetEnvironmentVariable($envName)
    if (-not [string]::IsNullOrWhiteSpace($raw)) {
        $n = 0
        if (-not [int]::TryParse($raw.Trim(), [ref]$n)) {
            [Console]::Error.WriteLine("build gate: $envName='$raw' is not an integer, ignoring")
        } elseif ($n -lt $min) {
            [Console]::Error.WriteLine("build gate: $envName='$raw' is below the minimum for the $Lane lane, ignoring")
        } elseif ($n -gt $limit) {
            [Console]::Error.WriteLine("build gate: $envName=$n exceeds machine-wide config ($limit); using $limit")
        } else { return $n }
    }
    return $limit
}

# Slots to open: the configured count, widened to cover any slot with a live record.
function Get-KilnBuildGateEffectiveSlots {
    param([string]$Lane = "heavy")
    $n = Get-KilnBuildGateSlotCount -Lane $Lane
    if ($n -le 0) { return $n }
    foreach ($r in @(Get-KilnBuildGateRecords -Lane $Lane)) {
        if ($r.Alive -and ($r.Slot + 1) -gt $n) { $n = $r.Slot + 1 }
    }
    return $n
}

function Get-KilnBuildGateMaxHoldSeconds {
    $raw = $env:KILNCTL_BUILD_GATE_MAX_HOLD_SEC
    $n = 0.0
    if (-not [string]::IsNullOrWhiteSpace($raw) -and [double]::TryParse($raw.Trim(), [ref]$n) -and $n -gt 0) { return $n }
    return $script:KilnGateDefaultMaxHoldSec
}

# Hard ceiling: env KILNCTL_BUILD_GATE_MAX_HOLD_HARD_SEC > config.json max_hold_hard_sec > 7200; never below the soft max hold.
function Get-KilnBuildGateMaxHoldHardSeconds {
    $n = 0.0; $val = $null
    $raw = $env:KILNCTL_BUILD_GATE_MAX_HOLD_HARD_SEC
    if (-not [string]::IsNullOrWhiteSpace($raw) -and [double]::TryParse($raw.Trim(), [ref]$n) -and $n -gt 0) { $val = $n }
    if ($null -eq $val) {
        $cfg = Get-KilnBuildGateConfig
        if ($null -ne $cfg -and $null -ne $cfg.max_hold_hard_sec -and [double]::TryParse([string]$cfg.max_hold_hard_sec, [ref]$n) -and $n -gt 0) { $val = $n }
    }
    if ($null -eq $val) { $val = $script:KilnGateDefaultMaxHoldHardSec }
    return [Math]::Max($val, (Get-KilnBuildGateMaxHoldSeconds))
}

# Pure max-hold policy (unit-tested with injected values): returns @{Action='keep'|'kill'; Reason=...}.
function Get-KilnBuildGateHoldDecision {
    param([double]$HeldSec, [double]$SoftSec, [double]$HardSec, [string[]]$Names, [string]$CompilerRegex)
    if ($HeldSec -lt $SoftSec) { return @{ Action = 'keep'; Reason = 'under max hold' } }
    if ($HeldSec -ge $HardSec) { return @{ Action = 'kill'; Reason = "hard ceiling ${HardSec}s reached (killed regardless of activity)" } }
    $live = @($Names | Where-Object { $_ -match $CompilerRegex } | Sort-Object -Unique)
    if ($live.Count -gt 0) { return @{ Action = 'keep'; Reason = "past max hold but still compiling ($($live -join ', '))" } }
    return @{ Action = 'kill'; Reason = 'past max hold with no live compiler activity' }
}

function Get-KilnBuildGateTimeoutSeconds {
    $raw = $env:KILNCTL_BUILD_GATE_TIMEOUT_SEC
    $n = 0
    if (-not [string]::IsNullOrWhiteSpace($raw) -and [int]::TryParse($raw.Trim(), [ref]$n) -and $n -gt 0) { return $n }
    return 3600
}

function Get-KilnBuildGateMutexName {
    param(
        [Parameter(Mandatory = $true)][int]$SlotIndex,
        [ValidateSet("heavy", "light")][string]$Lane = "heavy"
    )
    if ($Lane -eq "light") {
        $prefix = $env:KILNCTL_LIGHT_GATE_MUTEX_PREFIX
        if ([string]::IsNullOrEmpty($prefix)) { $prefix = "Global\kilnctl_build_light_" }
        return "$prefix$SlotIndex"
    }
    $prefix = $env:KILNCTL_BUILD_GATE_MUTEX_PREFIX
    if ([string]::IsNullOrEmpty($prefix)) { $prefix = "Global\kilnctl_build_slot_" }
    return "$prefix$SlotIndex"
}

# ---- process helpers --------------------------------------------------------

function Get-KilnProcessStartEpoch {
    param([int]$ProcessId)
    try {
        $p = Get-Process -Id $ProcessId -ErrorAction Stop
        return [long]([DateTimeOffset]$p.StartTime).ToUnixTimeSeconds()
    } catch { return $null }
}

# Alive = process exists and (when a start time was recorded) it is the same
# process, not a recycled pid.
function Test-KilnPidAlive {
    param([int]$ProcessId, $StartEpoch = $null)
    $actual = Get-KilnProcessStartEpoch -ProcessId $ProcessId
    if ($null -eq $actual) { return $false }
    if ($null -ne $StartEpoch -and [Math]::Abs($actual - [long]$StartEpoch) -gt 2) { return $false }
    return $true
}

function Get-KilnProcessSnapshot {
    $map = @{}
    try {
        foreach ($p in (Get-CimInstance Win32_Process -Property ProcessId, ParentProcessId, Name)) {
            $map[[int]$p.ProcessId] = @{ Parent = [int]$p.ParentProcessId; Name = [string]$p.Name }
        }
    } catch { }
    return $map
}

# Descendants (deepest first) of $RootPid from a snapshot.
function Get-KilnProcessDescendants {
    param([int]$RootPid, $Snapshot)
    $out = New-Object System.Collections.ArrayList
    $queue = New-Object System.Collections.Queue
    $queue.Enqueue($RootPid)
    $seen = @{ $RootPid = $true }
    while ($queue.Count -gt 0) {
        $cur = $queue.Dequeue()
        foreach ($k in @($Snapshot.Keys)) {
            if ($Snapshot[$k].Parent -eq $cur -and -not $seen.ContainsKey($k)) {
                $seen[$k] = $true
                [void]$out.Add([PSCustomObject]@{ Pid = $k; Name = $Snapshot[$k].Name })
                $queue.Enqueue($k)
            }
        }
    }
    $out.Reverse()
    return ,$out
}

# ---- records (one per held slot) -------------------------------------------

function Get-KilnBuildGateRecordPath {
    param([string]$Lane, [int]$Slot)
    return (Join-Path (Get-KilnBuildGateLaneDir -Lane $Lane) "slot$Slot.json")
}

function Write-KilnBuildGateRecord {
    param([string]$Lane, [int]$Slot, [string]$Label, [string]$Phase)
    $dir = Get-KilnBuildGateLaneDir -Lane $Lane
    if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    $cmd = [Environment]::CommandLine
    if ($cmd.Length -gt 400) { $cmd = $cmd.Substring(0, 400) }
    $now = [DateTimeOffset]::UtcNow
    $rec = [ordered]@{
        pid           = $PID
        proc_start    = (Get-KilnProcessStartEpoch -ProcessId $PID)
        cmdline       = $cmd
        label         = $Label
        lane          = $Lane
        slot          = $Slot
        phase         = $Phase
        started_epoch = [Math]::Round($now.ToUnixTimeMilliseconds() / 1000.0, 3)
        started_utc   = $now.ToString("o")
        worktree      = (Get-Location).Path
    }
    Write-KilnBuildGateFile -Path (Get-KilnBuildGateRecordPath -Lane $Lane -Slot $Slot) -Text ($rec | ConvertTo-Json -Compress)
}

function Remove-KilnBuildGateRecord {
    param([string]$Lane, [int]$Slot)
    $path = Get-KilnBuildGateRecordPath -Lane $Lane -Slot $Slot
    try {
        if (Test-Path -LiteralPath $path) {
            $rec = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
            if ([int]$rec.pid -eq $PID) { Remove-Item -LiteralPath $path -Force }
        }
    } catch { }
}

# Records for a lane, each annotated with Alive.
function Get-KilnBuildGateRecords {
    param([string]$Lane = "heavy")
    $dir = Get-KilnBuildGateLaneDir -Lane $Lane
    $out = @()
    if (-not (Test-Path -LiteralPath $dir)) { return $out }
    foreach ($f in @(Get-ChildItem -LiteralPath $dir -Filter "slot*.json" -ErrorAction SilentlyContinue)) {
        try {
            $r = Get-Content -LiteralPath $f.FullName -Raw -ErrorAction Stop | ConvertFrom-Json
            $out += [PSCustomObject]@{
                Slot = [int]$r.slot; Pid = [int]$r.pid; Alive = (Test-KilnPidAlive -ProcessId ([int]$r.pid) -StartEpoch $r.proc_start)
                Cmd = [string]$r.cmdline; Label = [string]$r.label; Phase = [string]$r.phase
                StartedEpoch = [double]$r.started_epoch; Worktree = [string]$r.worktree
            }
        } catch {
            if (-not (Test-Path -LiteralPath $f.FullName)) { continue }   # REVIEW_WEBFX4 LOW-6: released between enumeration and read -- the slot is free, not unparsable
            if ($f.Name -match '^slot(\d+)\.json$') {   # unparsable record: treat as held, never free
                $out += [PSCustomObject]@{ Slot = [int]$Matches[1]; Pid = 0; Alive = $true; Cmd = "<unparsable record>"; Label = ""; Phase = ""
                    StartedEpoch = [double]([DateTimeOffset]$f.LastWriteTimeUtc).ToUnixTimeSeconds(); Worktree = "" }
            }
        }
    }
    return $out
}

# ---- tickets (fair FIFO queue) ---------------------------------------------

function Get-KilnBuildGateQueueDir {
    param([string]$Lane)
    return (Join-Path (Get-KilnBuildGateLaneDir -Lane $Lane) "queue")
}

function New-KilnBuildGateTicket {
    param([string]$Lane, [string]$Label)
    $dir = Get-KilnBuildGateQueueDir -Lane $Lane
    if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    $ms = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
    $name = "{0:D15}-{1}-{2}.ticket" -f $ms, $PID, ([guid]::NewGuid().ToString("N").Substring(0, 8))
    $body = [ordered]@{ pid = $PID; proc_start = (Get-KilnProcessStartEpoch -ProcessId $PID); label = $Label; lane = $Lane } | ConvertTo-Json -Compress
    Write-KilnBuildGateFile -Path (Join-Path $dir $name) -Text $body
    return $name
}

# Names of live tickets, oldest first. Deletes tickets whose pid is dead.
# Cost matters: every waiter runs this every 500 ms, and with ~100 queued tickets a
# per-ticket Get-Content + ConvertFrom-Json + Get-Process took >13 s per scan on a loaded
# box, starving the head waiter's own heartbeat. So: ONE process snapshot per scan, the pid
# is read from the ticket NAME (<ms>-<pid>-<guid>), and the JSON is parsed only to compare the
# start time of a ticket whose pid is alive.
function Get-KilnBuildGateLiveTickets {
    param([string]$Lane)
    $dir = Get-KilnBuildGateQueueDir -Lane $Lane
    if (-not (Test-Path -LiteralPath $dir)) { return @() }
    $now = [DateTime]::UtcNow
    # One cheap process listing (no per-pid exceptions, no StartTime). A ticket whose pid is
    # gone is deleted; a hung one (no heartbeat) is skipped. Only the first few survivors, the
    # ones that can actually be head, pay for the JSON read + start-time (pid-reuse) check.
    $pids = @{}
    foreach ($p in [System.Diagnostics.Process]::GetProcesses()) { $pids[[int]$p.Id] = $p }
    $cand = @()
    foreach ($f in @([System.IO.DirectoryInfo]::new($dir).GetFiles("*.ticket") | Sort-Object Name)) {
        $tpid = $null
        if ($f.Name -match '^\d+-(\d+)-') { $tpid = [int]$Matches[1] }
        if ($null -ne $tpid -and -not $pids.ContainsKey($tpid)) {
            try { Remove-Item -LiteralPath $f.FullName -Force } catch { }
            continue
        }
        if (($now - $f.LastWriteTimeUtc).TotalSeconds -gt $script:KilnGateTicketStaleSec) { continue }
        $cand += [PSCustomObject]@{ File = $f; Pid = $tpid }
    }
    $live = @()
    $checked = 0
    foreach ($c in $cand) {
        if ($checked -lt 3 -and $null -ne $c.Pid) {
            $checked++
            try {
                $t = Get-Content -LiteralPath $c.File.FullName -Raw | ConvertFrom-Json
                if ($null -ne $t.proc_start -and -not (Test-KilnPidAlive -ProcessId $c.Pid -StartEpoch $t.proc_start)) {
                    try { Remove-Item -LiteralPath $c.File.FullName -Force } catch { }
                    $checked--
                    continue
                }
            } catch { }   # half-written: assume live this pass
        }
        $live += $c.File.Name
    }
    return @($live)
}

function Remove-KilnBuildGateTicket {
    param([string]$Lane, [string]$Name)
    for ($i = 0; $i -lt 5; $i++) {
        try { Remove-Item -LiteralPath (Join-Path (Get-KilnBuildGateQueueDir -Lane $Lane) $Name) -Force -ErrorAction Stop; return } catch { if (-not (Test-Path -LiteralPath (Join-Path (Get-KilnBuildGateQueueDir -Lane $Lane) $Name))) { return }; Start-Sleep -Milliseconds 200 }
    }
}

# ---- re-entrancy ------------------------------------------------------------

function Test-KilnBuildGateCovered {
    if ($null -ne $global:KilnBuildGateHeld) { return $global:KilnBuildGateHeld }
    $raw = $env:KILNCTL_BUILD_GATE_HELD
    if ([string]::IsNullOrWhiteSpace($raw)) { return $null }
    $parts = $raw.Split("|")
    if ($parts.Count -ne 3) { return $null }
    try {
        $path = Get-KilnBuildGateRecordPath -Lane $parts[0] -Slot ([int]$parts[1])
        if (-not (Test-Path -LiteralPath $path)) { return $null }
        $rec = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
        if ([int]$rec.pid -eq [int]$parts[2] -and (Test-KilnPidAlive -ProcessId ([int]$rec.pid) -StartEpoch $rec.proc_start)) {
            return [PSCustomObject]@{ Lane = $parts[0]; SlotIndex = [int]$parts[1]; Depth = 0; External = $true }
        }
    } catch { }
    return $null
}

# ---- watchdog (holder-side max hold) ---------------------------------------

function Start-KilnBuildGateWatchdog {
    param([int]$Slot, [string]$Lane, [string]$Label, [double]$MaxHoldSec, [string]$RecordPath, [double]$HardSec = 0)
    $acquiredUtc = [DateTime]::UtcNow
    $state = [hashtable]::Synchronized(@{ Stop = $false; Expired = $false; HeldSec = 0.0 })
    # One runspace per process, reused by every gate (a host-test build takes
    # hundreds of short gates; opening a runspace each time was the cost).
    if ($null -eq $global:KilnGateWatchdogRunspace -or $global:KilnGateWatchdogRunspace.RunspaceStateInfo.State -ne 'Opened') {
        $global:KilnGateWatchdogRunspace = [runspacefactory]::CreateRunspace()
        $global:KilnGateWatchdogRunspace.Open()
    }
    $rs = $global:KilnGateWatchdogRunspace
    $rs.SessionStateProxy.SetVariable("state", $state)
    $rs.SessionStateProxy.SetVariable("ownerPid", $PID)
    $rs.SessionStateProxy.SetVariable("maxHold", $MaxHoldSec)
    $rs.SessionStateProxy.SetVariable("hardHold", $(if ($HardSec -gt 0) { [Math]::Max($HardSec, $MaxHoldSec) } else { [Math]::Max((Get-KilnBuildGateMaxHoldHardSeconds), $MaxHoldSec) }))
    $rs.SessionStateProxy.SetVariable("decide", ${function:Get-KilnBuildGateHoldDecision})
    $rs.SessionStateProxy.SetVariable("compRegex", $script:KilnGateCompilerRegex)
    $rs.SessionStateProxy.SetVariable("pollSec", $script:KilnGateHoldPollSec)
    $rs.SessionStateProxy.SetVariable("slot", $Slot)
    $rs.SessionStateProxy.SetVariable("lane", $Lane)
    $rs.SessionStateProxy.SetVariable("label", $Label)
    $rs.SessionStateProxy.SetVariable("recordPath", $RecordPath)
    $rs.SessionStateProxy.SetVariable("acquiredUtc", $acquiredUtc)
    $ps = [powershell]::Create()
    $ps.Runspace = $rs
    [void]$ps.AddScript({
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        while (-not $state.Stop -and $sw.Elapsed.TotalSeconds -lt $maxHold) { Start-Sleep -Milliseconds 200 }
        if ($state.Stop) { return }
        # Past the soft max hold: kill only if the holder's tree has no live
        # compiler activity, or the hard ceiling is reached.
        $reason = ''
        while ($true) {
            $names = @()
            try {
                $m2 = @{}; $n2 = @{}
                foreach ($p in (Get-CimInstance Win32_Process -Property ProcessId, ParentProcessId, Name)) { $m2[[int]$p.ProcessId] = [int]$p.ParentProcessId; $n2[[int]$p.ProcessId] = [string]$p.Name }
                $q2 = New-Object System.Collections.Queue; $q2.Enqueue([int]$ownerPid); $seen2 = @{}
                while ($q2.Count -gt 0) { $c2 = $q2.Dequeue(); foreach ($k in @($m2.Keys)) { if ($m2[$k] -eq $c2 -and -not $seen2.ContainsKey($k)) { $seen2[$k] = $true; $names += $n2[$k]; $q2.Enqueue($k) } } }
            } catch { }
            $held = [Math]::Round($sw.Elapsed.TotalSeconds, 1)
            $d = & $decide -HeldSec $held -SoftSec $maxHold -HardSec $hardHold -Names $names -CompilerRegex $compRegex
            if ($d.Action -eq 'kill') { $reason = $d.Reason; break }
            [Console]::Error.WriteLine("build gate: $lane slot $slot ('$label') held ${held}s > max hold ${maxHold}s: $($d.Reason); keeping slot (hard ceiling ${hardHold}s)")
            $until = [Math]::Min($pollSec, [Math]::Max(1, $hardHold - $held))
            $w = [System.Diagnostics.Stopwatch]::StartNew()
            while (-not $state.Stop -and $w.Elapsed.TotalSeconds -lt $until) { Start-Sleep -Milliseconds 200 }
            if ($state.Stop) { return }
        }
        $state.HeldSec = [Math]::Round($sw.Elapsed.TotalSeconds, 1)
        $state.Expired = $true
        [Console]::Error.WriteLine("build gate: KILLED BY BUILD GATE -- $lane slot $slot ('$label') held $($state.HeldSec)s: $reason; killing the build processes this holder started since taking the slot, and releasing it")
        # Kill only OUR descendants started since the slot was taken (never
        # anyone else's, never an older sibling), deepest first, re-scanning
        # until none remain.
        $cut = $acquiredUtc.AddSeconds(-1)
        for ($pass = 0; $pass -lt 10; $pass++) {
            $map = @{}; $born = @{}
            foreach ($p in (Get-CimInstance Win32_Process -Property ProcessId, ParentProcessId, CreationDate)) {
                $map[[int]$p.ProcessId] = [int]$p.ParentProcessId
                $born[[int]$p.ProcessId] = $p.CreationDate
            }
            $order = New-Object System.Collections.ArrayList
            $q = New-Object System.Collections.Queue; $q.Enqueue([int]$ownerPid)
            while ($q.Count -gt 0) { $c = $q.Dequeue(); foreach ($k in @($map.Keys)) { if ($map[$k] -eq $c) { $q.Enqueue($k); if ($born[$k] -and $born[$k].ToUniversalTime() -ge $cut) { [void]$order.Add($k) } } } }
            if ($order.Count -eq 0) { break }
            $order.Reverse()
            foreach ($d in $order) { try { Stop-Process -Id $d -Force -ErrorAction Stop } catch { } }
            Start-Sleep -Milliseconds 200
        }
        # If the owner never gets to Exit-KilnBuildGate (hung in PowerShell
        # itself), exiting the process makes the kernel release the mutex.
        $grace = [System.Diagnostics.Stopwatch]::StartNew()
        while (-not $state.Stop -and $grace.Elapsed.TotalSeconds -lt 30) { Start-Sleep -Milliseconds 200 }
        if (-not $state.Stop) {
            try { Remove-Item -LiteralPath $recordPath -Force } catch { }
            [Console]::Error.WriteLine("build gate: holder unresponsive 30s after max-hold kill; exiting process to release $lane slot $slot")
            [Environment]::Exit(124)
        }
    })
    $handle = $ps.BeginInvoke()
    return [PSCustomObject]@{ State = $state; Ps = $ps; Runspace = $rs; Handle = $handle }
}

function Stop-KilnBuildGateWatchdog {
    param($Watchdog)
    if ($null -eq $Watchdog) { return }
    $Watchdog.State.Stop = $true
    try { [void]$Watchdog.Ps.EndInvoke($Watchdog.Handle) } catch { }
    try { $Watchdog.Ps.Dispose() } catch { }   # the runspace is shared and stays open
}

# ---- acquire / release ------------------------------------------------------

function Open-KilnBuildGateMutexes {
    param([string]$Lane, [int]$Slots)
    $m = @()
    for ($i = 0; $i -lt $Slots; $i++) {
        $m += New-Object System.Threading.Mutex($false, (Get-KilnBuildGateMutexName -SlotIndex $i -Lane $Lane))
    }
    return $m
}

function New-KilnBuildGateHeld {
    param($Mutexes, [int]$Index, [string]$Label, [string]$Lane, [string]$Phase, [bool]$Abandoned)
    for ($j = 0; $j -lt $Mutexes.Count; $j++) { if ($j -ne $Index) { $Mutexes[$j].Dispose() } }
    $gate = $null
    try {
    if ($Abandoned) {
        [Console]::Error.WriteLine("build gate: WARNING -- $Lane slot $Index was ABANDONED by a dead process; reclaimed it for '$Label'")
    }
    $maxHold = Get-KilnBuildGateMaxHoldSeconds
    $recPath = Get-KilnBuildGateRecordPath -Lane $Lane -Slot $Index
    try { Write-KilnBuildGateRecord -Lane $Lane -Slot $Index -Label $Label -Phase $Phase } catch {
        [Console]::Error.WriteLine("build gate: WARNING -- could not write holder record: $_")
    }
    $gate = [PSCustomObject]@{
        Disabled = $false; Reentrant = $false; Mutex = $Mutexes[$Index]; SlotIndex = $Index; Label = $Label; Lane = $Lane
        Depth = 1; Watchdog = $null; PrevEnv = $env:KILNCTL_BUILD_GATE_HELD; AcquiredAt = [DateTime]::UtcNow; MaxHold = $maxHold
    }
    $gate.Watchdog = Start-KilnBuildGateWatchdog -Slot $Index -Lane $Lane -Label $Label -MaxHoldSec $maxHold -RecordPath $recPath
    $env:KILNCTL_BUILD_GATE_HELD = "$Lane|$Index|$PID"
    $global:KilnBuildGateHeld = $gate
    } catch {
        # Interrupted (e.g. Ctrl-C) between acquiring and handing the gate back:
        # never leave the slot held with nobody to release it.
        if ($null -ne $gate -and $null -ne $gate.Watchdog) { Stop-KilnBuildGateWatchdog -Watchdog $gate.Watchdog }
        Remove-KilnBuildGateRecord -Lane $Lane -Slot $Index
        try { $Mutexes[$Index].ReleaseMutex() } catch { }
        $Mutexes[$Index].Dispose()
        $global:KilnBuildGateHeld = $null
        throw
    }
    return $gate
}

# Returns @(acquired, abandoned)
function Try-KilnBuildGateSlotNow {
    param($Mutex)
    try { return @($Mutex.WaitOne(0), $false) } catch [System.Threading.AbandonedMutexException] { return @($true, $true) }
}

function Enter-KilnBuildGate {
    param(
        [Parameter(Mandatory = $true)][string]$Label,
        [int]$TimeoutSeconds = 0,
        [int]$PollIntervalSeconds = 30,
        [ValidateSet("heavy", "light")][string]$Lane = "heavy",
        [string]$Phase = "compile"
    )

    $covered = Test-KilnBuildGateCovered
    if ($null -ne $covered) {
        $covered.Depth++
        [Console]::Error.WriteLine("build gate: '$Label' already covered by held $($covered.Lane) slot $($covered.SlotIndex); re-entrant, no second slot")
        return [PSCustomObject]@{ Disabled = $false; Reentrant = $true; Parent = $covered; Label = $Label; SlotIndex = $covered.SlotIndex; Lane = $covered.Lane }
    }

    $slots = Get-KilnBuildGateEffectiveSlots -Lane $Lane
    if ($slots -le 0) {
        [Console]::Error.WriteLine("build gate: $Lane lane disabled (slot count 0) for '$Label'")
        return [PSCustomObject]@{ Disabled = $true; Reentrant = $false; Mutex = $null; SlotIndex = -1; Label = $Label; Lane = $Lane }
    }
    if ($TimeoutSeconds -le 0) { $TimeoutSeconds = Get-KilnBuildGateTimeoutSeconds }

    $mutexes = @(Open-KilnBuildGateMutexes -Lane $Lane -Slots $slots)

    # Fast path: nobody queued ahead of us -> try every slot once.
    if (@(Get-KilnBuildGateLiveTickets -Lane $Lane).Count -eq 0) {
        for ($i = 0; $i -lt $mutexes.Count; $i++) {
            $r = Try-KilnBuildGateSlotNow -Mutex $mutexes[$i]
            if ($r[0]) {
                [Console]::Error.WriteLine("build gate: acquired $Lane slot $i for '$Label' (slots=$slots)")
                return New-KilnBuildGateHeld -Mutexes $mutexes -Index $i -Label $Label -Lane $Lane -Phase $Phase -Abandoned $r[1]
            }
        }
    }

    # Queue. Only the head ticket touches the slots; it waits on ALL of them.
    # Reap tickets this very process leaked earlier (a failed delete is swallowed): a live pid
    # with an old ticket must never queue behind itself.
    foreach ($old in @(Get-ChildItem -LiteralPath (Get-KilnBuildGateQueueDir -Lane $Lane) -Filter "*-$PID-*.ticket" -ErrorAction SilentlyContinue)) { Remove-KilnBuildGateTicket -Lane $Lane -Name $old.Name }
    $ticket = New-KilnBuildGateTicket -Lane $Lane -Label $Label
    $ticketPath = Join-Path (Get-KilnBuildGateQueueDir -Lane $Lane) $ticket
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $lastLog = -$PollIntervalSeconds
    try {
        while ($sw.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
            $live = @(Get-KilnBuildGateLiveTickets -Lane $Lane)
            if (-not (Test-Path -LiteralPath $ticketPath)) {
                Write-KilnBuildGateFile -Path $ticketPath -Text (@{ pid = $PID; proc_start = (Get-KilnProcessStartEpoch -ProcessId $PID); label = $Label; lane = $Lane } | ConvertTo-Json -Compress)
            } else {
                (Get-Item -LiteralPath $ticketPath).LastWriteTimeUtc = [DateTime]::UtcNow
            }
            $rank = [Array]::IndexOf($live, $ticket)
            if ($rank -lt 0) { $rank = $live.Count }
            if ($sw.Elapsed.TotalSeconds - $lastLog -ge $PollIntervalSeconds) {
                $lastLog = $sw.Elapsed.TotalSeconds
                [Console]::Error.WriteLine("build gate: waiting (label=$Label, lane=$Lane, $([Math]::Round($sw.Elapsed.TotalSeconds))s, queue position $($rank + 1) of $($live.Count), slots=$slots)")
            }
            if ($rank -ne 0) { Start-Sleep -Milliseconds 500; continue }
            $idx = [System.Threading.WaitHandle]::WaitTimeout
            $abandoned = $false
            try {
                $idx = [System.Threading.WaitHandle]::WaitAny($mutexes, [TimeSpan]::FromMilliseconds(1000))
            } catch [System.Threading.AbandonedMutexException] {
                $idx = $_.Exception.MutexIndex
                $abandoned = $true
            }
            if ($idx -ge 0 -and $idx -lt $mutexes.Count) {
                [Console]::Error.WriteLine("build gate: acquired $Lane slot $idx for '$Label' after $([Math]::Round($sw.Elapsed.TotalSeconds))s wait (slots=$slots)")
                return New-KilnBuildGateHeld -Mutexes $mutexes -Index $idx -Label $Label -Lane $Lane -Phase $Phase -Abandoned $abandoned
            }
        }
    } finally {
        Remove-KilnBuildGateTicket -Lane $Lane -Name $ticket
    }
    foreach ($m in $mutexes) { $m.Dispose() }
    throw "build gate: timed out after ${TimeoutSeconds}s waiting for a $Lane-lane build slot (label=$Label, slots=$slots) -- run tools\build_gate.ps1 -Status to see who holds them"
}

function Exit-KilnBuildGate {
    param([Parameter(Mandatory = $true)]$Gate)
    if ($Gate.Disabled) { return }
    if ($Gate.Reentrant) {
        if ($null -ne $Gate.Parent -and $Gate.Parent.Depth -gt 0) { $Gate.Parent.Depth-- }
        return
    }
    $expired = $false
    $heldSec = ([DateTime]::UtcNow - $Gate.AcquiredAt).TotalSeconds
    if ($null -ne $Gate.Watchdog) {
        $expired = [bool]$Gate.Watchdog.State.Expired
        Stop-KilnBuildGateWatchdog -Watchdog $Gate.Watchdog
    }
    Remove-KilnBuildGateRecord -Lane $Gate.Lane -Slot $Gate.SlotIndex
    $releaseError = $null
    try {
        $Gate.Mutex.ReleaseMutex()
    } catch {
        $releaseError = "build gate: ReleaseMutex failed for $($Gate.Lane) slot $($Gate.SlotIndex) ('$($Gate.Label)'): $_"
        [Console]::Error.WriteLine($releaseError)
    }
    $Gate.Mutex.Dispose()
    $env:KILNCTL_BUILD_GATE_HELD = $Gate.PrevEnv
    $global:KilnBuildGateHeld = $null
    [Console]::Error.WriteLine("build gate: released $($Gate.Lane) slot $($Gate.SlotIndex) for '$($Gate.Label)' (held $([Math]::Round($heldSec))s)")
    if ($releaseError) { throw $releaseError }
    if ($expired) {
        throw "build gate: $($Gate.Lane) slot $($Gate.SlotIndex) held by '$($Gate.Label)' for $([Math]::Round($heldSec))s, over the max hold of $($Gate.MaxHold)s (KILNCTL_BUILD_GATE_MAX_HOLD_SEC); KILLED BY BUILD GATE, build children killed, slot released"
    }
}

# ---- vcvars, once, outside the gate ----------------------------------------

# Runs vcvarsall once and applies the resulting environment to THIS process so
# later `cl` calls need no per-compile `call vcvarsall` (which used to run
# inside the slot). Idempotent per (vcvars path, arch).
function Import-KilnVcvarsEnv {
    param([Parameter(Mandatory = $true)][string]$Vcvars, [string]$Arch = "x64")
    $key = "$Vcvars|$Arch"
    if ($global:KilnVcvarsImported -eq $key) { return }
    $cmd = "set `"PATH=%PATH%;C:\Program Files (x86)\Microsoft Visual Studio\Installer`" && call `"$Vcvars`" $Arch >nul && set"
    $lines = & cmd.exe /c $cmd
    if ($LASTEXITCODE -ne 0) { throw "vcvarsall failed (exit $LASTEXITCODE) for $Vcvars" }
    foreach ($line in $lines) {
        $eq = $line.IndexOf("=")
        if ($eq -gt 0) { [Environment]::SetEnvironmentVariable($line.Substring(0, $eq), $line.Substring($eq + 1), "Process") }
    }
    $global:KilnVcvarsImported = $key
}

# Run ONE compile/link command line through cmd.exe /c under a slot taken just
# for that command and released the moment it returns (try/finally). The
# caller owns everything else (build lock, vcvars import, running the tests).
# Output flows to the pipeline exactly like a bare `cmd.exe /c $Command`, and
# $LASTEXITCODE is the command's exit code afterwards.
function Invoke-KilnGatedCmd {
    param([Parameter(Mandatory = $true)][string]$Command,
          [Parameter(Mandatory = $true)][string]$Label,
          [ValidateSet("heavy", "light")][string]$Lane = "heavy")
    $gate = Enter-KilnBuildGate -Label $Label -Lane $Lane
    try {
        & cmd.exe /c $Command
        $exit = $LASTEXITCODE
    } finally {
        Exit-KilnBuildGate -Gate $gate
    }
    $global:LASTEXITCODE = $exit
}
# ---- status ------------------------------------------------------------------

function Format-KilnAge {
    param([double]$Sec)
    if ($Sec -ge 3600) { return ("{0}h{1:D2}m" -f [int][Math]::Floor($Sec / 3600), [int][Math]::Floor(($Sec % 3600) / 60)) }
    if ($Sec -ge 60) { return ("{0}m{1:D2}s" -f [int][Math]::Floor($Sec / 60), [int]($Sec % 60)) }
    return ("{0}s" -f [int]$Sec)
}

function Get-KilnBuildGateStatus {
    $rows = @()
    $snapshot = Get-KilnProcessSnapshot
    $nowEpoch = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() / 1000.0
    foreach ($lane in @("heavy", "light")) {
        $slots = Get-KilnBuildGateEffectiveSlots -Lane $lane
        $recs = @{}
        foreach ($r in @(Get-KilnBuildGateRecords -Lane $lane)) { $recs[$r.Slot] = $r }
        for ($i = 0; $i -lt [Math]::Max($slots, 0); $i++) {
            $row = [ordered]@{ lane = $lane; slot = $i; state = "free"; pid = $null; cmd = $null; label = $null; phase = $null
                               age_sec = $null; pid_alive = $null; compiling = $false; compilers = @(); worktree = $null }
            $rec = $recs[$i]
            if ($null -ne $rec) {
                $row.pid = $rec.Pid; $row.cmd = $rec.Cmd; $row.label = $rec.Label; $row.phase = $rec.Phase
                $row.worktree = $rec.Worktree; $row.pid_alive = $rec.Alive
                $row.age_sec = [Math]::Round($nowEpoch - $rec.StartedEpoch, 1)
                if ($rec.Alive) {
                    $row.state = "held"
                    $kids = Get-KilnProcessDescendants -RootPid $rec.Pid -Snapshot $snapshot
                    $comp = @($kids | Where-Object { $_.Name -match $script:KilnGateCompilerRegex } | ForEach-Object { $_.Name })
                    $row.compilers = $comp
                    $row.compiling = ($comp.Count -gt 0)
                } else {
                    $row.state = "stale"
                }
            }
            if ($row.state -ne "held") {
                # No live record: probe the mutex so an older-checkout holder
                # (which writes no record) still shows as held. Released at once.
                $m = $null
                try {
                    $m = New-Object System.Threading.Mutex($false, (Get-KilnBuildGateMutexName -SlotIndex $i -Lane $lane))
                    $got = $false
                    try { $got = $m.WaitOne(0) } catch [System.Threading.AbandonedMutexException] { $got = $true }
                    if ($got) { $m.ReleaseMutex() } else { $row.state = "held-no-record" }
                } catch { } finally { if ($null -ne $m) { $m.Dispose() } }
            }
            $rows += [PSCustomObject]$row
        }
    }
    return $rows
}

function Show-KilnBuildGateStatus {
    param([switch]$Json)
    $rows = @(Get-KilnBuildGateStatus)
    if ($Json) { ConvertTo-Json -InputObject $rows -Depth 4; return }
    Write-Host "build gate status (dir $(Get-KilnBuildGateDir), max hold $(Get-KilnBuildGateMaxHoldSeconds)s)"
    foreach ($r in $rows) {
        $line = "{0,-5} slot {1}: {2,-14}" -f $r.lane, $r.slot, $r.state.ToUpper()
        if ($null -ne $r.pid) {
            $comp = ""
            if (@($r.compilers).Count -gt 0) { $comp = " [" + (@($r.compilers) -join ",") + "]" }
            $line += " pid={0} alive={1} age={2} compiling={3}{4} label='{5}' cmd={6}" -f $r.pid, $r.pid_alive, (Format-KilnAge $r.age_sec), $r.compiling, $comp, $r.label, $r.cmd
            if ($r.state -eq "stale") { $line += "  (stale record, ignored)" }
        }
        Write-Host $line
    }
    foreach ($lane in @("heavy", "light")) {
        $q = @(Get-KilnBuildGateLiveTickets -Lane $lane)
        Write-Host ("{0,-5} queue: {1} waiting" -f $lane, $q.Count)
    }
}

# Invoked directly (not dot-sourced): `build_gate.ps1 -Status [-Json]`.
if ($MyInvocation.InvocationName -ne '.' -and $args -contains '-Status') {
    Show-KilnBuildGateStatus -Json:($args -contains '-Json')
}
