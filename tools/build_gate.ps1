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
#
# LIGHT LANE (2026-10-05): a second, independent pool of slots for genuinely
# small builds -- one cl.exe/cmake invocation over a handful of translation
# units that finishes in seconds (check_recovery_*.ps1, check_commonfw_*.ps1).
# Those used to take a HEAVY slot, so with several agents each holding both
# heavy slots for a 7-18 minute host-test or target build, a seconds-long
# check queued 30-50+ minutes and every run_all_checks.ps1 -Fast blew past the
# 30-minute background limit. Pass -Lane light to Enter-KilnBuildGate for those
# callers only; host tests and full target builds stay on the default heavy
# lane. The lanes use different mutex names, so a light acquire never waits on
# a heavy holder. Heavy concurrency is unchanged (still bounded by
# KILNCTL_BUILD_GATE_SLOTS); light callers are bounded separately by
# KILNCTL_LIGHT_GATE_SLOTS (default 4) so small compiles cannot themselves
# pile up unboundedly. Never put a build that runs more than ~a minute on the
# light lane.
#
# KILNCTL_LIGHT_GATE_SLOTS: light-lane slot count, default 4, 0 disables the
# light lane only (heavy lane unaffected).
#
# KILNCTL_LIGHT_GATE_MUTEX_PREFIX: test-only override of the light lane's
# "Global\kilnctl_build_light_" prefix, same caveats as the heavy one below
# (must match mcpkit/buildgate.py's override if set).
#
# KILNCTL_BUILD_GATE_MUTEX_PREFIX: overrides the "Global\kilnctl_build_slot_"
# mutex name prefix below. This exists ONLY so a unit test (Python's
# mcpkit/buildgate.py has the matching override) can point at a private
# Local\ namespace instead of contending with a real build holding the
# machine-wide Global\ slots -- it is not a normal operator knob. If set, it
# MUST be set to the exact same value for every PowerShell AND Python caller
# gating the SAME real build, or the two sides silently gate on different
# mutexes and stop actually admission-controlling each other. Leave it unset
# for every real build; unset on both sides is the only supported steady
# state.

function Get-KilnBuildGateSlotCount {
    param([ValidateSet("heavy", "light")][string]$Lane = "heavy")
    if ($Lane -eq "light") {
        $envName = "KILNCTL_LIGHT_GATE_SLOTS"
        $default = 4
    } else {
        $envName = "KILNCTL_BUILD_GATE_SLOTS"
        $default = 2
    }
    $raw = [Environment]::GetEnvironmentVariable($envName)
    if ([string]::IsNullOrWhiteSpace($raw)) {
        return $default
    }
    $n = 0
    if (-not [int]::TryParse($raw.Trim(), [ref]$n)) {
        [Console]::Error.WriteLine("build gate: $envName='$raw' is not an integer, defaulting to $default")
        return $default
    }
    return $n
}

function Get-KilnBuildGateMutexName {
    param(
        [Parameter(Mandatory = $true)][int]$SlotIndex,
        [ValidateSet("heavy", "light")][string]$Lane = "heavy"
    )
    # "Global\" so every session/user on the machine contends for the same
    # slots, not just the current logon session. The prefix is overridable
    # via KILNCTL_BUILD_GATE_MUTEX_PREFIX -- see that variable's header
    # comment above; must match mcpkit/buildgate.py's own default/override
    # exactly, since this name IS the shared contract between the two
    # languages.
    if ($Lane -eq "light") {
        $prefix = $env:KILNCTL_LIGHT_GATE_MUTEX_PREFIX
        if ([string]::IsNullOrEmpty($prefix)) {
            $prefix = "Global\kilnctl_build_light_"
        }
        return "$prefix$SlotIndex"
    }
    $prefix = $env:KILNCTL_BUILD_GATE_MUTEX_PREFIX
    if ([string]::IsNullOrEmpty($prefix)) {
        $prefix = "Global\kilnctl_build_slot_"
    }
    return "$prefix$SlotIndex"
}

# Tries every slot once (WaitOne(0), round robin), then falls back to a
# bounded poll against slot 0 while printing a periodic waiting line so a
# slow build reads as gated, not hung.
function Enter-KilnBuildGate {
    param(
        [Parameter(Mandatory = $true)][string]$Label,
        [int]$TimeoutSeconds = 3600,
        [int]$PollIntervalSeconds = 30,
        [ValidateSet("heavy", "light")][string]$Lane = "heavy"
    )

    $slots = Get-KilnBuildGateSlotCount -Lane $Lane
    if ($slots -le 0) {
        [Console]::Error.WriteLine("build gate: $Lane lane disabled (slot count 0) for '$Label'")
        return [PSCustomObject]@{ Disabled = $true; Mutex = $null; SlotIndex = -1; Label = $Label }
    }

    $mutexes = @()
    for ($i = 0; $i -lt $slots; $i++) {
        $mutexes += New-Object System.Threading.Mutex($false, (Get-KilnBuildGateMutexName -SlotIndex $i -Lane $Lane))
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
            [Console]::Error.WriteLine("build gate: acquired slot $i for '$Label' (lane=$Lane, slots=$slots)")
            return [PSCustomObject]@{ Disabled = $false; Mutex = $mutexes[$i]; SlotIndex = $i; Label = $Label }
        }
    }

    # Every slot busy. Wait on ALL of them (WaitAny), not just slot 0 --
    # watching one slot only means a waiter never notices a DIFFERENT slot
    # freeing up, so it can time out at $TimeoutSeconds while another slot
    # sits idle the whole time. Keep every handle open while waiting; print a
    # waiting line every $PollIntervalSeconds so this looks gated, not hung.
    $elapsed = 0
    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    [Console]::Error.WriteLine("build gate: waiting (label=$Label, lane=$Lane, ${elapsed}s, slots=$slots)")
    while ($elapsed -lt $TimeoutSeconds) {
        $chunk = [Math]::Min($PollIntervalSeconds, $TimeoutSeconds - $elapsed)
        # WaitAny returns 0..n-1 on acquisition, or the sentinel
        # [System.Threading.WaitHandle]::WaitTimeout (258) on a plain
        # timeout -- that sentinel is a small POSITIVE int, not negative, so
        # it must be checked explicitly rather than assumed to be < 0.
        $signaledIndex = [System.Threading.WaitHandle]::WaitTimeout
        try {
            $signaledIndex = [System.Threading.WaitHandle]::WaitAny($mutexes, [TimeSpan]::FromSeconds($chunk))
        } catch [System.Threading.AbandonedMutexException] {
            # .NET still hands back which mutex was abandoned via MutexIndex.
            $signaledIndex = $_.Exception.MutexIndex
        }
        # Real elapsed time, not the requested chunk size -- a wait that
        # returns early (e.g. an abandoned mutex freed by a killed holder,
        # seconds into a 30s chunk) must not be reported as a full chunk.
        $elapsed = [Math]::Round($stopwatch.Elapsed.TotalSeconds)
        if ($signaledIndex -ge 0 -and $signaledIndex -lt $mutexes.Count) {
            $wonMutex = $mutexes[$signaledIndex]
            for ($j = 0; $j -lt $mutexes.Count; $j++) {
                if ($j -ne $signaledIndex) { $mutexes[$j].Dispose() }
            }
            [Console]::Error.WriteLine("build gate: acquired slot $signaledIndex for '$Label' after ${elapsed}s wait")
            return [PSCustomObject]@{ Disabled = $false; Mutex = $wonMutex; SlotIndex = $signaledIndex; Label = $Label }
        }
        [Console]::Error.WriteLine("build gate: waiting (label=$Label, lane=$Lane, ${elapsed}s, slots=$slots)")
    }

    foreach ($m in $mutexes) { $m.Dispose() }
    throw "build gate: timed out after ${TimeoutSeconds}s waiting for a $Lane-lane build slot (label=$Label, slots=$slots) -- another run appears stuck holding every slot"
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
        # Already released/abandoned by us is harmless, but a genuine
        # ReleaseMutex failure here strands the slot until the process exits
        # (or forever, in the long-lived MCP server) -- log it rather than
        # swallowing it silently (opus review advisory b).
        [Console]::Error.WriteLine("build gate: WARNING -- ReleaseMutex failed for slot $($Gate.SlotIndex) ('$($Gate.Label)'): $_")
    }
    $Gate.Mutex.Dispose()
    [Console]::Error.WriteLine("build gate: released slot $($Gate.SlotIndex) for '$($Gate.Label)'")
}
