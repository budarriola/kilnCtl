# check_01_saftyfw_pushed_build.ps1 -- builds SaftyFW against the ACTUAL
# content of origin/main, not the local working tree.
#
# See firmware/KilnFW/App/test/check_01_kilnfw_pushed_build.ps1 for the full
# rationale (same problem, same fix, mirrored for the RP2040 target): that
# check_00_*_target_build.ps1 siblings in both directories build the LOCAL
# tree (KilnFW's via robocopy mirror including uncommitted edits, SaftyFW's
# in place in the main tree), which proves nothing about what is actually
# pushed to origin/main. origin/main broke at least four times on
# 2026-09-10 with the same shape: a commit whose dependency closure was not
# fully committed, invisible to any check that only ever looks at a local
# tree. This file closes that gap for SaftyFW; check_01_kilnfw_pushed_build.ps1
# closes it for KilnFW.
#
# UNLIKE check_00_saftyfw_target_build.ps1 (no worktree, builds the main
# tree's own firmware/SaftyFW/build in place): this check needs a SEPARATE,
# dedicated worktree checked out at origin/main, because the whole point is
# to build content that is NOT the local tree's HEAD-plus-uncommitted-edits.
# It otherwise follows check_00's toolchain/no-worktree-hazard reasoning:
# arm-none-eabi-gcc + cmake/ninja, same as check_bootloader_builds.ps1.
#
# FAILURE MESSAGE CONTRACT: identical shape to check_01_kilnfw_pushed_build.ps1
# -- FAILED text always says "origin/main does not build" / names the commit,
# never "your tree does not build". check_00's failure text is the opposite.
#
# COST: dedicated persistent worktree, incremental cmake/ninja build every
# run (fetch + checkout + reset --hard to origin/main's current tip, then
# build) -- same "pay once, incremental thereafter" shape as
# check_01_kilnfw_pushed_build.ps1, and SaftyFW's arm-none-eabi build is
# the smaller of the two targets so the steady-state added cost here is
# lower than the KilnFW sibling's.
#
# SKIP CONTRACT: exit 3, with a "SKIP" line, if there is no network path to
# `git fetch origin`, no `origin` remote/branch, or (same as check_00) no
# arm-none-eabi-gcc on PATH / PICO_TOOLCHAIN_PATH set with no existing
# CMakeCache.txt in this dedicated worktree to fall back on. Any other
# failure to actually invoke the compiler is a FAIL, never a silent PASS.

$ErrorActionPreference = "Stop"
$ErrorActionPreference = "Continue"

. (Join-Path $PSScriptRoot "..\..\..\tools\build_lock.ps1")
. (Join-Path $PSScriptRoot "..\..\..\tools\build_gate.ps1")
. (Join-Path $PSScriptRoot "..\..\..\tools\pushed_build_stamp.ps1")

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..\..")

function Fail([string]$msg) {
    Write-Host ""
    Write-Host "FAILED: origin/main does not build -- $msg" -ForegroundColor Red
    Write-Host "        This is a PUSHED-REF failure, not a local-tree failure: what is" -ForegroundColor Red
    Write-Host "        actually on origin/main right now fails to build SaftyFW. Do not" -ForegroundColor Red
    Write-Host "        pull/base new work on origin/main until this is fixed there." -ForegroundColor Red
    exit 1
}

$fetchOutput = & git -C $repoRoot fetch origin main 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "SKIP: 'git fetch origin main' failed (exit $LASTEXITCODE) -- no network path to origin, or no such remote/branch. Output:" -ForegroundColor Yellow
    $fetchOutput | Write-Host
    exit 3
}

$originSha = (& git -C $repoRoot rev-parse origin/main).Trim()
if (-not $originSha) {
    Write-Host "SKIP: could not resolve origin/main after fetch -- no such ref" -ForegroundColor Yellow
    exit 3
}

$WorktreePath = "C:\wt\checkbuild_origin_saftyfw"
$buildDir = Join-Path $WorktreePath "firmware\SaftyFW\build"

$haveToolchainOnPath = [bool](Get-Command "arm-none-eabi-gcc" -ErrorAction SilentlyContinue)
$haveConfiguredBuild = Test-Path (Join-Path $buildDir "CMakeCache.txt")
if (-not $haveToolchainOnPath -and -not $env:PICO_TOOLCHAIN_PATH -and -not $haveConfiguredBuild) {
    Write-Host "SKIP: arm-none-eabi-gcc not on PATH, PICO_TOOLCHAIN_PATH not set, and no existing $buildDir\CMakeCache.txt to reconfigure -- arm-none-eabi toolchain not set up on this machine" -ForegroundColor Yellow
    exit 3
}

# RESULT REUSE (2026-10-07, tools/pushed_build_stamp.ps1): see the KilnFW sibling.
Write-Host "origin/main is $originSha"
$pushedSlot = Enter-PushedBuildSlot -Name "saftyfw" -Sha $originSha
if ($pushedSlot.Reused) {
    Write-PushedBuildReused -Stamp $pushedSlot.Stamp -Sha $originSha -What "SaftyFW"
    exit 0
}
Write-Host "No reusable PASS stamp for $originSha -- building (this run owns the build)."
try {
if (-not (Test-Path $WorktreePath)) {
    Write-Host "Setting up persistent origin/main build worktree at $WorktreePath (first run) ..."
    & git -C $repoRoot worktree add --detach $WorktreePath $originSha 2>&1 | Write-Host
    if ($LASTEXITCODE -ne 0) {
        Fail "git worktree add failed (exit $LASTEXITCODE)"
    }
}

# Build lock FIRST; a gate slot is held only around the compile (never while queued on a lock).
$lock = Enter-BuildLock -Name "saftyfw_checkbuild_origin_worktree"
try {
    Write-Host "Checking out origin/main ($originSha) in $WorktreePath, discarding any prior state there ..."
    # Dedicated worktree, nothing else writes here deliberately -- a hard
    # reset to the fetched ref is the whole point (exact origin/main
    # content, no local-tree contamination), unlike check_00's in-place
    # main-tree build.
    & git -C $WorktreePath fetch origin main 2>&1 | Write-Host
    if ($LASTEXITCODE -ne 0) {
        Fail "git fetch inside worktree failed (exit $LASTEXITCODE)"
    }
    & git -C $WorktreePath checkout --detach $originSha 2>&1 | Write-Host
    if ($LASTEXITCODE -ne 0) {
        Fail "git checkout $originSha in worktree failed (exit $LASTEXITCODE)"
    }
    & git -C $WorktreePath reset --hard $originSha 2>&1 | Write-Host
    if ($LASTEXITCODE -ne 0) {
        Fail "git reset --hard $originSha in worktree failed (exit $LASTEXITCODE)"
    }
    & git -C $WorktreePath clean -fdx -e build 2>&1 | Write-Host

    $saftyDir = Join-Path $WorktreePath "firmware\SaftyFW"

    if (-not (Test-Path (Join-Path $buildDir "CMakeCache.txt"))) {
        Write-Host "No $buildDir\CMakeCache.txt -- first-time configure (cmake -G Ninja -B build .) ..."
        if (-not (Test-Path $buildDir)) {
            New-Item -ItemType Directory -Path $buildDir | Out-Null
        }
        if (-not $env:PICO_SDK_PATH) {
            $env:PICO_SDK_PATH = "C:\pico-tools\pico-sdk"
            Write-Host "PICO_SDK_PATH not set -- defaulting to $env:PICO_SDK_PATH (see CMakeLists.txt header comment)"
        }
        Push-Location $saftyDir
        try {
            cmake -G Ninja -B build .
            if ($LASTEXITCODE -ne 0) {
                Pop-Location
                Remove-Item -Recurse -Force $buildDir -ErrorAction SilentlyContinue
                Fail "cmake first-time configure failed with exit code $LASTEXITCODE -- removed $buildDir so the next run retries cleanly instead of reusing a poisoned CMakeCache.txt"
            }
        } finally {
            if ((Get-Location).Path -eq $saftyDir.Path) {
                Pop-Location
            }
        }
    }

    Push-Location $buildDir
    try {
        Write-Host "Reconfiguring SaftyFW against origin/main (cmake .) ..."
        cmake . | Write-Host
        if ($LASTEXITCODE -ne 0) {
            Fail "cmake reconfigure failed with exit code $LASTEXITCODE"
        }

        Write-Host "Building SaftyFW against origin/main ($originSha) (ninja) ..."
        $buildGate = Enter-KilnBuildGate -Label "saftyfw_pushed_build"
        try {
            ninja | Write-Host
            $ninjaExit = $LASTEXITCODE
        } finally {
            Exit-KilnBuildGate -Gate $buildGate
        }
        if ($ninjaExit -ne 0) {
            Fail "ninja build failed (exit $ninjaExit) against origin/main commit $originSha -- see output above."
        }
    } finally {
        Pop-Location
    }
} finally {
    Exit-BuildLock -Lock $lock
}

$elf = Join-Path $buildDir "SaftyFW.elf"
if (-not (Test-Path $elf)) {
    Fail "ninja reported success (exit 0) but $elf does not exist -- refusing to report PASS without a real build artifact."
}

Write-PushedBuildStamp -Path (Get-PushedBuildStampPath -Name "saftyfw") -Sha $originSha -Detail "SaftyFW.elf $((Get-Item $elf).Length) bytes"
} finally {
    Exit-PushedBuildSlot -Slot $pushedSlot
}

Write-Host ""
Write-Host "PASS: origin/main (commit $originSha) builds SaftyFW target cleanly. [built]" -ForegroundColor Green
exit 0
