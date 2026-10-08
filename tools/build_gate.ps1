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
#      tools/check_build_gate.ps1 lints callers for this.
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
# 2-3). Resolution order: env (KILNCTL_BUILD_GATE_SLOTS / KILNCTL_LIGHT_GATE_SLOTS,
# 0 disables that lane) > <dir>\config.json {"heavy_slots":N,"light_slots":N}
# (created with the defaults on first use) > code default 4/4. Whatever the
# count, a waiter also opens every slot index that has a live record (so a
# count lowered while higher slots are busy cannot hide a holder). A tree pinned
# to an OLD commit still runs its own old gate (default 2 slots, no records, no
# tickets): it contends only for slots 0-1 and is invisible to -Status except
# through the mutex probe (shown as "held-no-record"); it needs a rebase to
# join the shared count and the fair queue.
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
$script:KilnGateCompilerRegex = '^(cl|link|ninja|cmake|cc1|cc1plus|ccache|xtensa-.+|arm-none-eabi-.+)(\.exe)?$'

function Get-KilnBuildGateDir {
    $d = $env:KILNCTL_BUILD_GATE_DIR
    if ([string]::IsNullOrWhiteSpace($d)) { $d = "C:\wt\.buildgate" }
    return $d
}

function Get-KilnBuildGateLaneDir {
    param([string]$Lane = "heavy")
    return (Join-Path (Get-KilnBuildGateDir) $Lane)
}

function Get-KilnBuildGateConfigPath { return (Join-Path (Get-KilnBuildGateDir) "config.json") }

function Get-KilnBuildGateConfig {
    $path = Get-KilnBuildGateConfigPath
    try {
        if (-not (Test-Path -LiteralPath $path)) {
            $dir = Get-KilnBuildGateDir
            if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
            [IO.File]::WriteAllText($path, '{"heavy_slots": 4, "light_slots": 4}' + "`n")
        }
        return (Get-Content -LiteralPath $path -Raw | ConvertFrom-Json)
    } catch {
        return $null
    }
}

function Get-KilnBuildGateSlotCount {
    param([ValidateSet("heavy", "light")][string]$Lane = "heavy")
    $envName = if ($Lane -eq "light") { "KILNCTL_LIGHT_GATE_SLOTS" } else { "KILNCTL_BUILD_GATE_SLOTS" }
    $raw = [Environment]::GetEnvironmentVariable($envName)
    if (-not [string]::IsNullOrWhiteSpace($raw)) {
        $n = 0
        if ([int]::TryParse($raw.Trim(), [ref]$n)) { return $n }
        [Console]::Error.WriteLine("build gate: $envName='$raw' is not an integer, ignoring")
    }
    $cfg = Get-KilnBuildGateConfig
    if ($null -ne $cfg) {
        $val = if ($Lane -eq "light") { $cfg.light_slots } else { $cfg.heavy_slots }
        $n = 0
        if ($null -ne $val -and [int]::TryParse("$val", [ref]$n) -and $n -ge 0) { return $n }
    }
    return $script:KilnGateDefaultSlots
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
    [IO.File]::WriteAllText((Get-KilnBuildGateRecordPath -Lane $Lane -Slot $Slot), ($rec | ConvertTo-Json -Compress))
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
            $r = Get-Content -LiteralPath $f.FullName -Raw | ConvertFrom-Json
            $out += [PSCustomObject]@{
                Slot = [int]$r.slot; Pid = [int]$r.pid; Alive = (Test-KilnPidAlive -ProcessId ([int]$r.pid) -StartEpoch $r.proc_start)
                Cmd = [string]$r.cmdline; Label = [string]$r.label; Phase = [string]$r.phase
                StartedEpoch = [double]$r.started_epoch; Worktree = [string]$r.worktree
            }
        } catch { }
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
    [IO.File]::WriteAllText((Join-Path $dir $name), $body)
    return $name
}

# Names of live tickets, oldest first. Deletes tickets whose pid is dead.
function Get-KilnBuildGateLiveTickets {
    param([string]$Lane)
    $dir = Get-KilnBuildGateQueueDir -Lane $Lane
    $live = @()
    if (-not (Test-Path -LiteralPath $dir)) { return $live }
    $now = [DateTime]::UtcNow
    foreach ($f in @(Get-ChildItem -LiteralPath $dir -Filter "*.ticket" -ErrorAction SilentlyContinue)) {
        $alive = $false
        try {
            $t = Get-Content -LiteralPath $f.FullName -Raw | ConvertFrom-Json
            $alive = Test-KilnPidAlive -ProcessId ([int]$t.pid) -StartEpoch $t.proc_start
        } catch { $alive = $true }   # half-written: assume live this pass
        if (-not $alive) {
            try { Remove-Item -LiteralPath $f.FullName -Force } catch { }
            continue
        }
        # A live pid that stopped touching its ticket (hung) is skipped, not deleted.
        if (($now - $f.LastWriteTimeUtc).TotalSeconds -gt $script:KilnGateTicketStaleSec) { continue }
        $live += $f.Name
    }
    return @($live | Sort-Object)
}

function Remove-KilnBuildGateTicket {
    param([string]$Lane, [string]$Name)
    try { Remove-Item -LiteralPath (Join-Path (Get-KilnBuildGateQueueDir -Lane $Lane) $Name) -Force -ErrorAction Stop } catch { }
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
    param([int]$Slot, [string]$Lane, [string]$Label, [double]$MaxHoldSec, [string]$RecordPath)
    $state = [hashtable]::Synchronized(@{ Stop = $false; Expired = $false; HeldSec = 0.0 })
    $rs = [runspacefactory]::CreateRunspace()
    $rs.Open()
    $rs.SessionStateProxy.SetVariable("state", $state)
    $rs.SessionStateProxy.SetVariable("ownerPid", $PID)
    $rs.SessionStateProxy.SetVariable("maxHold", $MaxHoldSec)
    $rs.SessionStateProxy.SetVariable("slot", $Slot)
    $rs.SessionStateProxy.SetVariable("lane", $Lane)
    $rs.SessionStateProxy.SetVariable("label", $Label)
    $rs.SessionStateProxy.SetVariable("recordPath", $RecordPath)
    $ps = [powershell]::Create()
    $ps.Runspace = $rs
    [void]$ps.AddScript({
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        while (-not $state.Stop -and $sw.Elapsed.TotalSeconds -lt $maxHold) { Start-Sleep -Milliseconds 200 }
        if ($state.Stop) { return }
        $state.HeldSec = [Math]::Round($sw.Elapsed.TotalSeconds, 1)
        $state.Expired = $true
        [Console]::Error.WriteLine("build gate: MAX HOLD EXCEEDED -- $lane slot $slot ('$label') held $($state.HeldSec)s > limit ${maxHold}s (KILNCTL_BUILD_GATE_MAX_HOLD_SEC); killing this holder's own build children and releasing the slot")
        # Kill only OUR descendants (never anyone else's), deepest first.
        $map = @{}
        foreach ($p in (Get-CimInstance Win32_Process -Property ProcessId, ParentProcessId)) { $map[[int]$p.ProcessId] = [int]$p.ParentProcessId }
        $order = New-Object System.Collections.ArrayList
        $q = New-Object System.Collections.Queue; $q.Enqueue([int]$ownerPid)
        while ($q.Count -gt 0) { $c = $q.Dequeue(); foreach ($k in @($map.Keys)) { if ($map[$k] -eq $c) { [void]$order.Add($k); $q.Enqueue($k) } } }
        $order.Reverse()
        foreach ($d in $order) { try { Stop-Process -Id $d -Force -ErrorAction Stop } catch { } }
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
    try { $Watchdog.Ps.Dispose(); $Watchdog.Runspace.Dispose() } catch { }
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
    $ticket = New-KilnBuildGateTicket -Lane $Lane -Label $Label
    $ticketPath = Join-Path (Get-KilnBuildGateQueueDir -Lane $Lane) $ticket
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $lastLog = -$PollIntervalSeconds
    try {
        while ($sw.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
            $live = @(Get-KilnBuildGateLiveTickets -Lane $Lane)
            if (-not (Test-Path -LiteralPath $ticketPath)) {
                [IO.File]::WriteAllText($ticketPath, (@{ pid = $PID; proc_start = (Get-KilnProcessStartEpoch -ProcessId $PID); label = $Label; lane = $Lane } | ConvertTo-Json -Compress))
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
    try {
        $Gate.Mutex.ReleaseMutex()
    } catch {
        [Console]::Error.WriteLine("build gate: WARNING -- ReleaseMutex failed for $($Gate.Lane) slot $($Gate.SlotIndex) ('$($Gate.Label)'): $_")
    }
    $Gate.Mutex.Dispose()
    $env:KILNCTL_BUILD_GATE_HELD = $Gate.PrevEnv
    $global:KilnBuildGateHeld = $null
    [Console]::Error.WriteLine("build gate: released $($Gate.Lane) slot $($Gate.SlotIndex) for '$($Gate.Label)' (held $([Math]::Round($heldSec))s)")
    if ($expired) {
        throw "build gate: $($Gate.Lane) slot $($Gate.SlotIndex) held by '$($Gate.Label)' for $([Math]::Round($heldSec))s, over the max hold of $($Gate.MaxHold)s (KILNCTL_BUILD_GATE_MAX_HOLD_SEC); build children killed, slot released"
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
