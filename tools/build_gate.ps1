# build_gate.ps1 -- machine-wide heavy-build gate.
#
# This PC hard-froze five times in one week (Kernel-Power 41, BugcheckCode 0,
# no dump, no prior error event) every time coincided with 7-11 agents each
# running a full ESP-IDF target build or an MSVC host-test build concurrently
# in separate C:\wt worktrees. tools/build_lock.ps1 only serializes two
# writers of the SAME build directory -- it does nothing when N agents each
# build their OWN isolated directory at once, which is exactly the shape that
# froze the machine (three full target builds per run_all_checks.ps1 call,
# each at ninja's default job count of cores+2, times N concurrent agents).
#
# This gate is a separate, machine-wide admission control: at most
# $KILNCTL_BUILD_GATE_SLOTS heavy builds may run at once ACROSS ALL SESSIONS,
# regardless of how many separate worktrees or processes are asking.
#
# Deliberately a MUTEX, not a Semaphore, per slot. A named Semaphore's count
# is never given back if the holding process is killed or crashes -- the slot
# is gone until reboot. A Mutex is released by the kernel the moment its
# owning process dies, and WaitOne() on an abandoned mutex still returns
# ownership (as an AbandonedMutexException in .NET) -- exactly the "another
# agent got killed mid-build" case this machine needs to recover from
# automatically, the same reasoning build_lock.ps1 already documents for its
# own single mutex.
#
# Usage (dot-source, then wrap ONLY the heavy phase -- the acquiring
# PowerShell process must live for the whole heavy phase; never hold this
# across a `Start-Process` return):
#   . (Join-Path $PSScriptRoot "...\tools\build_gate.ps1")   # adjust depth
#   $gate = Enter-KilnBuildGate -Label "kilnfw_target_build"
#   try {
#       ... idf.py / ninja / cmake --build ...
#   } finally {
#       Exit-KilnBuildGate -Gate $gate
#   }
#
# KILNCTL_BUILD_GATE_SLOTS: number of heavy builds allowed machine-wide at
# once. Default 2. Set to 0 to disable the gate entirely -- this is meant
# ONLY for a machine known to run a single session at a time; on a shared
# machine running multiple agent sessions, leaving it enabled is the whole
# point.

function Get-KilnBuildGateSlotCount {
    $raw = $env:KILNCTL_BUILD_GATE_SLOTS
    if ([string]::IsNullOrWhiteSpace($raw)) {
        return 2
    }
    $n = 0
    if (-not [int]::TryParse($raw.Trim(), [ref]$n)) {
        Write-Host "build gate: KILNCTL_BUILD_GATE_SLOTS='$raw' is not an integer, defaulting to 2" -ForegroundColor Yellow
        return 2
    }
    return $n
}

function Get-KilnBuildGateMutexName {
    param([Parameter(Mandatory = $true)][int]$SlotIndex)
    # "Global\" so every session/user on the machine contends for the same
    # slots, not just the current logon session.
    return "Global\kilnctl_build_slot_$SlotIndex"
}

# Tries every slot once (WaitOne(0), round robin), then falls back to a
# bounded poll against slot 0 while printing a periodic waiting line so a
# slow build reads as gated, not hung.
function Enter-KilnBuildGate {
    param(
        [Parameter(Mandatory = $true)][string]$Label,
        [int]$TimeoutSeconds = 3600,
        [int]$PollIntervalSeconds = 30
    )

    $slots = Get-KilnBuildGateSlotCount
    if ($slots -le 0) {
        Write-Host "build gate: disabled (KILNCTL_BUILD_GATE_SLOTS=0) for '$Label'" -ForegroundColor DarkGray
        return [PSCustomObject]@{ Disabled = $true; Mutex = $null; SlotIndex = -1; Label = $Label }
    }

    $mutexes = @()
    for ($i = 0; $i -lt $slots; $i++) {
        $mutexes += New-Object System.Threading.Mutex($false, (Get-KilnBuildGateMutexName -SlotIndex $i))
    }

    # First pass: a non-blocking try at every slot, round robin. One of these
    # succeeding (including via an abandoned-mutex exception, which still
    # means acquired) is the common case on an idle-ish machine.
    for ($i = 0; $i -lt $mutexes.Count; $i++) {
        $acquired = $false
        try {
            $acquired = $mutexes[$i].WaitOne(0)
        } catch [System.Threading.AbandonedMutexException] {
            $acquired = $true
        }
        if ($acquired) {
            for ($j = 0; $j -lt $mutexes.Count; $j++) {
                if ($j -ne $i) { $mutexes[$j].Dispose() }
            }
            Write-Host "build gate: acquired slot $i for '$Label' (slots=$slots)" -ForegroundColor DarkGray
            return [PSCustomObject]@{ Disabled = $false; Mutex = $mutexes[$i]; SlotIndex = $i; Label = $Label }
        }
    }

    # Every slot busy. Poll slot 0 with a bounded wait, printing a waiting
    # line every $PollIntervalSeconds so this looks gated, not hung.
    for ($j = 1; $j -lt $mutexes.Count; $j++) { $mutexes[$j].Dispose() }
    $mutex0 = $mutexes[0]
    $elapsed = 0
    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    Write-Host "build gate: waiting (label=$Label, ${elapsed}s, slots=$slots)" -ForegroundColor Yellow
    while ($elapsed -lt $TimeoutSeconds) {
        $chunk = [Math]::Min($PollIntervalSeconds, $TimeoutSeconds - $elapsed)
        $acquired = $false
        try {
            $acquired = $mutex0.WaitOne([TimeSpan]::FromSeconds($chunk))
        } catch [System.Threading.AbandonedMutexException] {
            $acquired = $true
        }
        # Real elapsed time, not the requested chunk size -- a wait that
        # returns early (e.g. an abandoned mutex freed by a killed holder,
        # seconds into a 30s chunk) must not be reported as a full chunk.
        $elapsed = [Math]::Round($stopwatch.Elapsed.TotalSeconds)
        if ($acquired) {
            Write-Host "build gate: acquired slot 0 for '$Label' after ${elapsed}s wait" -ForegroundColor DarkGray
            return [PSCustomObject]@{ Disabled = $false; Mutex = $mutex0; SlotIndex = 0; Label = $Label }
        }
        Write-Host "build gate: waiting (label=$Label, ${elapsed}s, slots=$slots)" -ForegroundColor Yellow
    }

    $mutex0.Dispose()
    throw "build gate: timed out after ${TimeoutSeconds}s waiting for a heavy-build slot (label=$Label, slots=$slots) -- another run appears stuck holding every slot"
}

function Exit-KilnBuildGate {
    param(
        [Parameter(Mandatory = $true)]$Gate
    )
    if ($Gate.Disabled) {
        return
    }
    try {
        $Gate.Mutex.ReleaseMutex()
    } catch {
        # already released/abandoned -- nothing to do
    }
    $Gate.Mutex.Dispose()
    Write-Host "build gate: released slot $($Gate.SlotIndex) for '$($Gate.Label)'" -ForegroundColor DarkGray
}
