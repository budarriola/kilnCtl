# check_bootloader_builds.ps1 -- proves firmware/SaftyFW/bootloader still
# builds as its own, separate top-level CMake project (bootloader/CMakeLists.txt's
# own header comment: "wholly separate top-level CMake project from
# ../CMakeLists.txt, never configured in the same build tree").
#
# Why this exists: build_saftyfw (and this repo's normal SaftyFW CMake
# configure/build) only ever builds the SaftyFW/SaftyFW_slotA/SaftyFW_slotB
# app targets -- none of them pull in bootloader/CMakeLists.txt at all, so a
# change to a header the bootloader includes (e.g. HAL Phase 1b's
# src/board_pins.h -> src/board/board_pins.h move, 53706f7) can silently
# break the bootloader's OWN build while every other build/check in the repo
# stays green. That exact gap was the finding this check was added to close
# (commit around f0d5eb7): bootloader/CMakeLists.txt:77-81 only granted
# ../src and ../src/update as private include dirs, so bootloader/main.c's
# `#include "board_pins.h"` silently stopped resolving the moment the header
# moved, and nothing in run_all_checks.ps1 or build_saftyfw noticed.
#
# What this does: reconfigures (cmake .) and rebuilds (ninja) the existing
# firmware/SaftyFW/bootloader/build directory in place if it already exists
# (fast -- no fresh pico-sdk configure). On a genuinely fresh clone (no
# build/CMakeCache.txt yet) it runs the first-time configure itself
# (cmake -G Ninja -B build .) instead of hard-failing -- a check that can
# only ever pass on a machine someone already hand-configured isn't
# actually guarding anything for a fresh clone or a fresh CI runner, which
# is exactly the scenario "the bootloader still builds" most needs to
# cover. The first-time configure is slower (pico-sdk discovery), but that
# cost is paid once per machine/build-dir, same as every other CMake
# project in this repo.

$ErrorActionPreference = "Stop"

# Two concurrent runs of this script (e.g. overlapping run_all_checks.ps1
# invocations) reconfigure/build the SAME firmware/SaftyFW/bootloader/build
# tree and race each other's cmake/ninja recompaction -- reproduced
# 2026-09-06 by running this script twice concurrently, which threw
# "CMake Error ... generate_config_header.cmake:29 (configure_file): No
# such file or directory" on one side while the other side succeeded.
# Serialize via a global named mutex so concurrent invocations queue up
# instead of corrupting the shared build tree.
. (Join-Path $PSScriptRoot "..\..\..\tools\build_lock.ps1")
. (Join-Path $PSScriptRoot "..\..\..\tools\build_gate.ps1")
# Build lock FIRST; a gate slot is held only around the compile (never while queued on a lock).
$buildLock = $null

$bootloaderDir = Join-Path $PSScriptRoot "..\bootloader"
$buildDir = Join-Path $bootloaderDir "build"

try {
# Key the lock by the RESOLVED build dir: the tree is per-worktree
# (<worktree>irmware\SaftyFWootloaderuild), so only runs sharing one
# tree need to serialize. A fixed name made every worktree on the machine
# queue on one mutex and time out after 900 s under parallel load.
$resolvedBuildDir = [System.IO.Path]::GetFullPath($buildDir).TrimEnd('').ToLowerInvariant()
$sha = [System.Security.Cryptography.SHA1]::Create()
$dirHash = -join ($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($resolvedBuildDir))[0..7] | ForEach-Object { $_.ToString("x2") })
$buildLock = Enter-BuildLock -Name "saftyfw_bootloader_build_$dirHash"

if (-not (Test-Path (Join-Path $buildDir "CMakeCache.txt"))) {
    Write-Host "No $buildDir\CMakeCache.txt -- first-time configure (cmake -G Ninja -B build .) ..."
    if (-not (Test-Path $buildDir)) {
        New-Item -ItemType Directory -Path $buildDir | Out-Null
    }
    # ../CMakeLists.txt's/bootloader/CMakeLists.txt's own header comments
    # both document PICO_SDK_PATH as an environment variable the invoker
    # must set (their example: $env:PICO_SDK_PATH = "C:\pico-tools\pico-sdk"),
    # normally provided by whatever process starts a build (e.g. the
    # kilnctrl MCP server's build_saftyfw). This check can run standalone
    # with no such process around it, so fall back to that same documented
    # default -- but only if the caller hasn't already set one, so a
    # machine with a different SDK location is never silently overridden.
    if (-not $env:PICO_SDK_PATH) {
        $env:PICO_SDK_PATH = "C:\pico-tools\pico-sdk"
        Write-Host "PICO_SDK_PATH not set -- defaulting to $env:PICO_SDK_PATH (see CMakeLists.txt header comment)"
    }
    Push-Location $bootloaderDir
    try {
        # See the note below (before the reconfigure/build block) on why
        # stderr is not merged with 2>&1.
        cmake -G Ninja -B build .
        if ($LASTEXITCODE -ne 0) {
            # A failed first-time configure can still leave a partial
            # CMakeCache.txt behind (cmake writes it early, before every
            # check passes) -- if left in place, the NEXT run of this
            # script sees that CMakeCache.txt, takes the "already
            # configured" fast path above instead of retrying the
            # first-time configure, and reconfigures/builds against a
            # poisoned cache that never actually succeeded once. Remove
            # the whole build dir so the next run starts clean and retries
            # first-time configure for real.
            Write-Host "cmake first-time configure failed -- removing $buildDir so the next run retries cleanly instead of reusing a poisoned CMakeCache.txt"
            Pop-Location
            Remove-Item -Recurse -Force $buildDir -ErrorAction SilentlyContinue
            throw "cmake first-time configure failed with exit code $LASTEXITCODE"
        }
    }
    finally {
        if ((Get-Location).Path -eq $bootloaderDir) {
            Pop-Location
        }
    }
}

Push-Location $buildDir
try {
    # Deliberately no 2>&1 merge here: in Windows PowerShell 5.1, redirecting
    # a native exe's stderr wraps every stderr line in a NativeCommandError
    # and flips $? to false even on a genuine exit-code-0 success (cmake and
    # ninja both write informational lines to stderr) -- with
    # $ErrorActionPreference = "Stop" that turns a passing build into a
    # thrown error. $LASTEXITCODE alone is the real signal.
    Write-Host "Reconfiguring saftyfw_bootloader (cmake .) ..."
    cmake .
    if ($LASTEXITCODE -ne 0) {
        throw "cmake reconfigure failed with exit code $LASTEXITCODE"
    }

    Write-Host "Building saftyfw_bootloader (ninja) ..."
    $buildGate = Enter-KilnBuildGate -Label "saftyfw_bootloader_build" -Lane heavy
    try {
        ninja
        $ninjaExit = $LASTEXITCODE
    } finally {
        Exit-KilnBuildGate -Gate $buildGate
    }
    if ($ninjaExit -ne 0) {
        throw "ninja build failed with exit code $ninjaExit"
    }
}
finally {
    Pop-Location
}

$elf = Join-Path $buildDir "saftyfw_bootloader.elf"
if (-not (Test-Path $elf)) {
    Write-Host "FAILED: build reported success but $elf does not exist." -ForegroundColor Red
    exit 1
}

Write-Host "OK: saftyfw_bootloader.elf built successfully" -ForegroundColor Green
exit 0

}
finally {
    if ($null -ne $buildLock) { Exit-BuildLock -Lock $buildLock }
}
