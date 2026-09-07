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

function Enter-BuildLock {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [int]$TimeoutSeconds = 900
    )

    # "Global\" makes this visible across all sessions/users on the machine,
    # not just the current one -- two separate agent/session processes must
    # see the same mutex.
    $mutexName = "Global\kilnCtl_buildlock_$Name"
    $mutex = New-Object System.Threading.Mutex($false, $mutexName)

    $acquired = $false
    try {
        $acquired = $mutex.WaitOne(0)
    } catch [System.Threading.AbandonedMutexException] {
        # A previous holder crashed/was killed without releasing -- we still
        # got ownership. Treat as acquired.
        $acquired = $true
    }

    if (-not $acquired) {
        Write-Host "waiting for another run to finish using build lock '$Name' ..." -ForegroundColor Yellow
        try {
            $acquired = $mutex.WaitOne([TimeSpan]::FromSeconds($TimeoutSeconds))
        } catch [System.Threading.AbandonedMutexException] {
            $acquired = $true
        }
        if (-not $acquired) {
            $mutex.Dispose()
            throw "timed out after ${TimeoutSeconds}s waiting for build lock '$Name' (another run appears stuck)"
        }
        Write-Host "acquired build lock '$Name'" -ForegroundColor Yellow
    }

    return [PSCustomObject]@{ Mutex = $mutex; Name = $Name }
}

function Exit-BuildLock {
    param(
        [Parameter(Mandatory = $true)]$Lock
    )
    try {
        $Lock.Mutex.ReleaseMutex()
    } catch {
        # already released/abandoned -- nothing to do
    }
    $Lock.Mutex.Dispose()
}
