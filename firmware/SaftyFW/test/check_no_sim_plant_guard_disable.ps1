# check_no_sim_plant_guard_disable.ps1 -- fails if a SaftyFW build was
# configured with -DSAFTYFW_HONOR_SIM_PLANT=ON, which lets a remote (ESP-
# supplied) SIM_PLANT context flag disable S2 (sustained over-setpoint), S3
# (load stuck on) and S4 (load inactive, WARN-only). TODO.md "Honour the
# SIM_PLANT flag" -- that option exists so a bench build can be told to trust
# a simulated plant model and skip nuisance-tripping on synthetic data; it
# must never ship in a build anyone could mistake for a real safety image.
#
# WHY A GENERATED HEADER, NOT THE CMAKE CACHE. CMakeCache.txt records every
# option ever passed to configure, including ones a later `cmake .` no longer
# restates, and it is not something the running firmware carries with it --
# reading it proves what the LAST configure said, not what actually got
# compiled into the ELF that shipped. saftyfw_build_info.h
# (gen_build_info.cmake, regenerated on every build via add_custom_target)
# bakes SAFTYFW_BUILD_SIM_PLANT_HONORED as a plain 0/1 macro alongside the
# git commit/dirty state already used to identify a build -- the same place
# this project already looks to answer "what is this build, really." See
# CMakeLists.txt's SAFTYFW_HONOR_SIM_PLANT option comment for the full
# design (compile-time AND runtime gate; default OFF; a `message(WARNING ...)`
# at configure time when ON).
#
# SKIP CONTRACT: exit 3 with a line containing "SKIP" if the generated header
# does not exist -- this is check_00_saftyfw_target_build.ps1's output, and a
# machine that never ran that check (no toolchain, fresh clone) has nothing
# to grade yet. That is a missing prerequisite, not a defect, exactly like
# check_00_saftyfw_target_build.ps1's own SKIP contract. Any header content
# this check cannot parse (the macro line missing or malformed) is a FAIL,
# never a silent PASS -- a check that cannot prove the flag is off must not
# report green.

$ErrorActionPreference = "Stop"

$saftyDir = Resolve-Path (Join-Path $PSScriptRoot "..")
$header = Join-Path $saftyDir "build\saftyfw_build_info.h"

function Fail([string]$msg) {
    Write-Host ""
    Write-Host "FAILED: $msg" -ForegroundColor Red
    exit 1
}

if (-not (Test-Path $header)) {
    Write-Host "SKIP: $header does not exist -- no SaftyFW target build has run yet on this machine (check_00_saftyfw_target_build.ps1 produces it)" -ForegroundColor Yellow
    exit 3
}

$content = Get-Content -Raw -LiteralPath $header
$match = [regex]::Match($content, '#define\s+SAFTYFW_BUILD_SIM_PLANT_HONORED\s+(\d+)')
if (-not $match.Success) {
    Fail "$header does not contain a SAFTYFW_BUILD_SIM_PLANT_HONORED macro -- gen_build_info.cmake should always emit one (0 or 1). Header may be stale or the generator changed shape; refusing to report PASS without a value to check."
}

$value = $match.Groups[1].Value
if ($value -ne "0") {
    Fail "$header reports SAFTYFW_BUILD_SIM_PLANT_HONORED $value -- this build was configured with -DSAFTYFW_HONOR_SIM_PLANT=ON, which lets the ESP's SIM_PLANT context flag disable S2/S3/S4. That option is BENCH-ONLY and must be OFF (the default) in any build that could be mistaken for production. Reconfigure without -DSAFTYFW_HONOR_SIM_PLANT=ON (or explicitly with =OFF) and rebuild."
}

Write-Host ""
Write-Host "PASS: $header reports SAFTYFW_BUILD_SIM_PLANT_HONORED 0 -- SIM_PLANT guard-disable is compiled out." -ForegroundColor Green
exit 0
