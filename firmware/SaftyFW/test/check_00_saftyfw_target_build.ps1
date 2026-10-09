# check_00_saftyfw_target_build.ps1 -- the one check that actually builds
# SaftyFW for the real RP2040 target, publishing a fresh SaftyFW.elf before
# any ELF-reading SaftyFW check runs.
#
# WHY THIS EXISTS. check_saftyfw_task_stack_budgets.ps1/.py has its own
# freshness gate (2026-09-09, widened 2026-09-10 to walk src/update/,
# src/board/, firmware/hwAbstraction/ and firmware/CommonFW/ -- see that
# .py file's FRESHNESS_ROOTS comment) and correctly refuses with "FAIL:
# STALE ELF" whenever firmware/SaftyFW/build/SaftyFW.elf predates a source
# file its own measurement depends on. But nothing in run_all_checks.ps1's
# check_*.ps1 glob ever actually BUILDS SaftyFW -- so on a tree where nobody
# happened to call build_saftyfw (or this check) recently, the stack-budget
# check reliably refuses with STALE ELF, not because anything is wrong, but
# because there is no automated step upstream of it that produces a current
# artifact. KilnFW does not have this gap: check_00_kilnfw_target_build.ps1
# (this file's sibling, firmware/KilnFW/App/test/) sorts first in the glob,
# builds, and publishes a fresh KilnCtrl.elf every run_all_checks.ps1 pass.
# This file closes the identical gap for SaftyFW, one target down: a
# "check_00_" prefix in firmware/SaftyFW/test/ sorts it before
# check_saftyfw_task_stack_budgets.ps1 in the same directory (run_all_checks.ps1
# discovers check_*.ps1 by glob, plain FullName sort order, same convention
# KilnFW's check_00 documents).
#
# NO WORKTREE, UNLIKE check_00_kilnfw_target_build.ps1. That file needs a
# short-path worktree because idf.py's build silently no-ops under an
# inherited MSYSTEM environment and because the default
# .claude\worktrees\... path overflows the MSVC/xtensa command line for that
# toolchain. Neither hazard applies here: SaftyFW's real target build is
# arm-none-eabi-gcc + ninja (see CMakeLists.txt's own header), the same
# toolchain check_bootloader_builds.ps1 already drives in place with no
# worktree at all, and it is MSVC -- not this arm-none-eabi build -- that
# overflows on a long path (see CLAUDE.md's SaftyFW host-test note, which is
# about build_host_tests.ps1's MSVC host build, a wholly different build
# from this one). So this check reconfigures/builds the MAIN TREE's own
# firmware/SaftyFW/build directory in place, exactly like
# check_bootloader_builds.ps1 does for firmware/SaftyFW/bootloader/build --
# mirroring that sibling's structure rather than inventing a second pattern,
# per the owner's explicit instruction not to reinvent
# check_00_kilnfw_target_build.ps1's worktree-mirroring machinery where it
# is not needed.
#
# LOCKING: shared build_lock.ps1 mutex, same as check_bootloader_builds.ps1
# and check_00_kilnfw_target_build.ps1, so two concurrent invocations queue
# instead of racing ninja/cmake in the same build directory.
#
# SKIP CONTRACT: exit 3 with a line containing "SKIP" if arm-none-eabi-gcc
# is not on PATH and PICO_TOOLCHAIN_PATH is not set, AND the existing build
# directory has no CMakeCache.txt to fall back on -- a missing toolchain is
# a missing prerequisite, not a defect. Any other failure to actually invoke
# the compiler (reconfigure failure, build failure, missing artifact after
# a reported-success build) is a FAIL (exit 1), never a silent PASS.
#
# FRESHNESS: this check does not need its own freshness comparison the way
# check_00_kilnfw_target_build.ps1 does (that one publishes into a directory
# OTHER checks read by default, with no freshness gate of their own) --
# check_saftyfw_task_stack_budgets.py already owns a real, source-derived
# freshness gate against the exact artifact this check produces
# (firmware/SaftyFW/build/SaftyFW.elf, DEFAULT_ELF in stack_budget_lib_arm.py).
# This check's job is simply to make sure a BUILD actually runs first so
# that gate has something current to approve -- if the build genuinely did
# not need to relink (no source changed since last time), ninja correctly
# no-ops and the existing SaftyFW.elf's mtime already satisfies that gate
# because it predates nothing new. If a real edit landed and the toolchain
# silently failed to relink against it, ninja's own dependency tracking
# (not a wall-clock guess -- see check_00_kilnfw_target_build.ps1's own
# "FRESHNESS SIGNAL" comment for why a wall-clock gate is the wrong
# question) is what's being trusted here, exactly as check_bootloader_builds.ps1
# already trusts it for the bootloader target with no separate freshness
# check of its own.

$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "..\..\..\tools\build_lock.ps1")
. (Join-Path $PSScriptRoot "..\..\..\tools\build_gate.ps1")

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..\..")
$saftyDir = Resolve-Path (Join-Path $PSScriptRoot "..")
$buildDir = Join-Path $saftyDir "build"

function Fail([string]$msg) {
    Write-Host ""
    Write-Host "FAILED: $msg" -ForegroundColor Red
    exit 1
}

# Toolchain presence check -- mirrors check_bootloader_builds.ps1's own
# PICO_SDK_PATH default, but this is a SKIP (missing prerequisite) rather
# than an unconditional default, because a machine with neither the
# toolchain on PATH nor an already-configured build dir cannot build at all
# (not even the bootloader check gets this right for a truly fresh clone
# with no toolchain -- that gap is not this file's to close).
$haveToolchainOnPath = [bool](Get-Command "arm-none-eabi-gcc" -ErrorAction SilentlyContinue)
$haveConfiguredBuild = Test-Path (Join-Path $buildDir "CMakeCache.txt")
if (-not $haveToolchainOnPath -and -not $env:PICO_TOOLCHAIN_PATH -and -not $haveConfiguredBuild) {
    Write-Host "SKIP: arm-none-eabi-gcc not on PATH, PICO_TOOLCHAIN_PATH not set, and no existing $buildDir\CMakeCache.txt to reconfigure -- arm-none-eabi toolchain not set up on this machine" -ForegroundColor Yellow
    exit 3
}

# Build lock FIRST; a build-gate slot is held only around the ninja compile
# (never while waiting for this lock, never around cmake configure)
$lock = Enter-BuildLock -Name "saftyfw_target_build"
try {
    if (-not (Test-Path (Join-Path $buildDir "CMakeCache.txt"))) {
        Write-Host "No $buildDir\CMakeCache.txt -- first-time configure (cmake -G Ninja -B build .) ..."
        if (-not (Test-Path $buildDir)) {
            New-Item -ItemType Directory -Path $buildDir | Out-Null
        }
        if (-not $env:PICO_SDK_PATH) {
            # Mirrors tools/PcTools/src/mcpkit/pico_sdk.py's
            # DEFAULT_PICO_SDK_PATH -- that module is the single source of
            # truth for build_saftyfw()'s own configure-from-scratch path;
            # keep both in sync if this default ever moves.
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
        # Deliberately no 2>&1 merge -- same PowerShell 5.1 NativeCommandError
        # trap check_bootloader_builds.ps1 and check_00_kilnfw_target_build.ps1
        # both document: cmake/ninja write informational lines to stderr, and
        # merging turns a genuine exit-0 success into a thrown error under
        # $ErrorActionPreference = "Stop".
        Write-Host "Reconfiguring SaftyFW (cmake .) ..."
        cmake . | Write-Host
        if ($LASTEXITCODE -ne 0) {
            Fail "cmake reconfigure failed with exit code $LASTEXITCODE"
        }

        Write-Host "Building SaftyFW (ninja) ..."
        $buildGate = Enter-KilnBuildGate -Label "saftyfw_target_build"
        try {
            ninja | Write-Host
            $ninjaExit = $LASTEXITCODE
        } finally {
            Exit-KilnBuildGate -Gate $buildGate
        }
        if ($ninjaExit -ne 0) {
            Fail "ninja build failed (exit $ninjaExit) -- see output above."
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

# SaftyFW_slotA.bin / SaftyFW_slotB.bin -- raw objcopy images of the two
# slot-linked executables, produced by a POST_BUILD add_custom_command on
# each SaftyFW_slotX target (firmware/SaftyFW/CMakeLists.txt,
# saftyfw_add_slot_executable()). These feed the ESP-side embed-and-push-to-
# Pico auto-update path (docs/PICO_AUTO_UPDATE_PLAN.md, owner decision
# 2026-09-20). Checked here, not in a separate check file, because this is
# the one check that actually runs the target build both slot executables
# come from. The two images MUST NOT be byte-identical -- they are
# position-dependent code linked at two different flash origins
# (0x10011000 vs 0x100E1000), so identical bytes would mean the slot-specific
# origin never actually took effect (e.g. a stale/cached link, or the wrong
# linker script applied to both).
$slotABin = Join-Path $buildDir "SaftyFW_slotA.bin"
$slotBBin = Join-Path $buildDir "SaftyFW_slotB.bin"
if (-not (Test-Path $slotABin)) {
    Fail "ninja reported success but $slotABin does not exist -- SaftyFW_slotA's POST_BUILD objcopy step did not produce it."
}
if (-not (Test-Path $slotBBin)) {
    Fail "ninja reported success but $slotBBin does not exist -- SaftyFW_slotB's POST_BUILD objcopy step did not produce it."
}
$slotABytes = [System.IO.File]::ReadAllBytes($slotABin)
$slotBBytes = [System.IO.File]::ReadAllBytes($slotBBin)
if ($slotABytes.Length -ne $slotBBytes.Length) {
    Fail "$slotABin is $($slotABytes.Length) bytes but $slotBBin is $($slotBBytes.Length) bytes -- slot A and slot B are built from the SAME application sources and should produce equal-length images; a length mismatch means the two slot targets diverged (stale build, wrong sources, or a linker script problem)."
}
if ([System.Linq.Enumerable]::SequenceEqual([byte[]]$slotABytes, [byte[]]$slotBBytes)) {
    Fail "$slotABin and $slotBBin are byte-identical -- these are position-dependent images linked at two different flash origins (0x10011000 / 0x100E1000) and must differ. Identical bytes means the slot-specific link origin did not actually take effect."
}

Write-Host ""
Write-Host "PASS: SaftyFW target build succeeded, $elf produced; SaftyFW_slotA.bin ($($slotABytes.Length) bytes) and SaftyFW_slotB.bin differ as expected." -ForegroundColor Green
exit 0
