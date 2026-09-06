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
# firmware/SaftyFW/bootloader/build directory in place -- it does NOT create
# a fresh build tree (that's a much slower first-time pico-sdk configure,
# and this check runs on every check pass). If build/ does not exist yet
# (e.g. a fresh clone before anyone has configured the bootloader once),
# this check reports that clearly and fails rather than silently skipping,
# since a check that can silently no-op is worse than no check (this
# project's own "negative-test every check" rule).

$ErrorActionPreference = "Stop"

$bootloaderDir = Join-Path $PSScriptRoot "..\bootloader"
$buildDir = Join-Path $bootloaderDir "build"

if (-not (Test-Path (Join-Path $buildDir "CMakeCache.txt"))) {
    Write-Host "FAILED: $buildDir has no CMakeCache.txt -- configure the bootloader's CMake project at least once (cmake -G Ninja -B bootloader/build bootloader) before this check can run." -ForegroundColor Red
    exit 1
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
    ninja
    if ($LASTEXITCODE -ne 0) {
        throw "ninja build failed with exit code $LASTEXITCODE"
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
