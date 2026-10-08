# pushed_build_stamp.ps1 -- shared result-reuse helper for
# check_01_kilnfw_pushed_build.ps1 and check_01_saftyfw_pushed_build.ps1.
#
# PROBLEM (2026-10-07): with ~15 agent sessions each running the full -Fast
# suite, every one of them fetched origin/main and queued on the 2-slot heavy
# build gate to build the SAME origin/main sha in the SAME shared worktree.
# Eight waiters deep, some hit the gate's 3600 s timeout and failed the suite
# for a reason unrelated to the change under test.
#
# FIX: a build of a given origin/main sha is a pure function of that sha (the
# worktree is reset --hard to it, sdkconfig regenerated from committed
# defaults), so one PASS answers every caller that sees the same sha.
#  * A PASS writes a JSON stamp OUTSIDE the build worktree (the worktree is
#    `git clean -fdx`'d every run, which would delete a stamp kept inside it).
#  * Before taking a heavy gate slot, callers take a per-sha lock. Waiters
#    queue on that lock, NOT on the gate, so they hold no build slot, and
#    they re-read the stamp every few seconds, so the moment the holder's PASS
#    lands they stop waiting and report the reused result.
#  * Only a PASS is ever reused, and only when the stamp's 40-hex sha equals
#    the caller's origin/main sha exactly (case-sensitive). A FAIL is
#    deliberately NOT reused: failures here include transient environment ones
#    (a link error from an out-of-lock deletion of the shared worktree, OOM, a
#    killed process), and one transient FAIL cached against a sha would poison
#    every session until origin/main moved. After a FAIL the next waiter
#    rebuilds, which costs one more build but never turns a one-off into a
#    sticky red.
#  * KILNCTL_PUSHED_BUILD_FORCE=1 ignores any stamp (always rebuild).
#  * KILNCTL_PUSHED_STAMP_DIR overrides the stamp directory (default C:\wt);
#    exists for the unit test (firmware/KilnFW/App/test/test_pushed_build_stamp.ps1).
#
# Requires tools/build_lock.ps1 (Get-BuildLockMutexName) dot-sourced first.

function Get-PushedBuildStampPath {
    param([Parameter(Mandatory = $true)][string]$Name)
    $dir = $env:KILNCTL_PUSHED_STAMP_DIR
    if ([string]::IsNullOrEmpty($dir)) { $dir = "C:\wt" }
    return (Join-Path $dir "checkbuild_origin_$Name.result.json")
}

# Returns the stamp object iff it is a PASS for exactly $Sha, else $null.
function Read-PushedBuildStamp {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Sha
    )
    if ($env:KILNCTL_PUSHED_BUILD_FORCE -eq "1") { return $null }
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    try {
        $s = Get-Content -LiteralPath $Path -Raw -ErrorAction Stop | ConvertFrom-Json -ErrorAction Stop
    } catch {
        return $null
    }
    if ($null -eq $s) { return $null }
    if ($s.schema -ne 1) { return $null }
    if ($s.result -ne "PASS") { return $null }
    if ([string]::IsNullOrEmpty($s.sha)) { return $null }
    if ($Sha -cnotmatch '^[0-9a-f]{40}$') { return $null }
    if ($s.sha -cne $Sha) { return $null }
    return $s
}

function Write-PushedBuildStamp {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Sha,
        [string]$Detail = ""
    )
    $obj = [ordered]@{
        schema    = 1
        sha       = $Sha
        result    = "PASS"
        built_utc = (Get-Date).ToUniversalTime().ToString("o")
        host      = $env:COMPUTERNAME
        detail    = $Detail
    }
    $dir = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
    $tmp = "$Path.$PID.tmp"
    ($obj | ConvertTo-Json) | Set-Content -LiteralPath $tmp -Encoding UTF8
    Move-Item -LiteralPath $tmp -Destination $Path -Force
}

# Take the per-sha lock, or return a reusable stamp. Never holds a gate slot.
# Returns [pscustomobject]@{ Reused; Stamp; Lock }.
function Enter-PushedBuildSlot {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Sha,
        [int]$TimeoutSeconds = 7200,
        [int]$PollSeconds = 5
    )
    $stampPath = Get-PushedBuildStampPath -Name $Name
    $stamp = Read-PushedBuildStamp -Path $stampPath -Sha $Sha
    if ($stamp) { return [PSCustomObject]@{ Reused = $true; Stamp = $stamp; Lock = $null } }

    $mutex = New-Object System.Threading.Mutex($false, (Get-BuildLockMutexName -Name "pushed_${Name}_$Sha"))
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $announced = $false
    while ($true) {
        $acquired = $false
        try { $acquired = $mutex.WaitOne([TimeSpan]::FromSeconds($PollSeconds)) }
        catch [System.Threading.AbandonedMutexException] { $acquired = $true }
        if ($acquired) {
            # Re-check under the lock: the previous holder may have just passed.
            $stamp = Read-PushedBuildStamp -Path $stampPath -Sha $Sha
            if ($stamp) {
                try { $mutex.ReleaseMutex() } catch {}
                $mutex.Dispose()
                return [PSCustomObject]@{ Reused = $true; Stamp = $stamp; Lock = $null }
            }
            return [PSCustomObject]@{ Reused = $false; Stamp = $null; Lock = [PSCustomObject]@{ Mutex = $mutex; Name = "pushed_${Name}_$Sha" } }
        }
        if (-not $announced) {
            Write-Host "another run is already building origin/main $Sha ($Name); waiting for its result instead of queueing a duplicate build ..." -ForegroundColor Yellow
            $announced = $true
        }
        $stamp = Read-PushedBuildStamp -Path $stampPath -Sha $Sha
        if ($stamp) {
            $mutex.Dispose()
            return [PSCustomObject]@{ Reused = $true; Stamp = $stamp; Lock = $null }
        }
        if ($sw.Elapsed.TotalSeconds -ge $TimeoutSeconds) {
            $mutex.Dispose()
            throw "timed out after ${TimeoutSeconds}s waiting for another run building origin/main $Sha ($Name)"
        }
    }
}

function Exit-PushedBuildSlot {
    param($Slot)
    if ($Slot -and $Slot.Lock) {
        try { $Slot.Lock.Mutex.ReleaseMutex() } catch {}
        $Slot.Lock.Mutex.Dispose()
    }
}

function Write-PushedBuildReused {
    param([Parameter(Mandatory = $true)]$Stamp, [Parameter(Mandatory = $true)][string]$Sha, [Parameter(Mandatory = $true)][string]$What)
    Write-Host ""
    Write-Host "REUSED (not rebuilt): result from $($Stamp.built_utc) on $($Stamp.host) for origin/main commit $Sha ($($Stamp.detail))" -ForegroundColor Cyan
    Write-Host "PASS: origin/main (commit $Sha) builds $What cleanly. [reused]" -ForegroundColor Green
}
