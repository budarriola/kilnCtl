# Builds kiln_core.exe: the REAL KilnFW pid.c + thermal_guard.c +
# heater_output.c, compiled verbatim for the host with MSVC (no ESP-IDF, no
# FreeRTOS, no LVGL, no hardware) plus this directory's own stdio harness
# (main.c). Mirrors firmware/KilnFW/App/test/build_host_tests.ps1's pattern
# and toolchain path exactly, and firmware/SimFW/tools/virtual_dut/
# dut_core/build_host.ps1's structure -- see this directory's README.md for
# why exactly these three KilnFW files (and no others) are compiled here.
#
# Usage: powershell -File build_host.ps1
$ErrorActionPreference = "Stop"

$vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
if (-not (Test-Path $vcvars)) {
    throw "vcvarsall.bat not found at $vcvars -- update this path if MSVC Build Tools moved/were reinstalled."
}

$thisDir = $PSScriptRoot
$kilnfwDrivers = Join-Path $thisDir "..\..\..\..\KilnFW\App\drivers"
$outDir = Join-Path $thisDir "build"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$exe = Join-Path $outDir "kiln_core.exe"

# Exactly three KilnFW sources, all already proven host-portable by
# firmware/KilnFW/App/test/build_host_tests.ps1 (pid.c has test_pid.c there,
# thermal_guard.c has test_thermal_guard.c, heater_output.c has
# test_heater_output.c, and all three together have test_closed_loop.c) --
# none is modified, wrapped, or copied here, only compiled.
$sources = @(
    (Join-Path $thisDir "main.c"),
    (Join-Path $kilnfwDrivers "pid.c"),
    (Join-Path $kilnfwDrivers "thermal_guard.c"),
    (Join-Path $kilnfwDrivers "heater_output.c")
)

$sourceArgs = ($sources | ForEach-Object { '"' + $_ + '"' }) -join " "
$cmd = "call `"$vcvars`" x64 >nul && cl /nologo /W4 /WX /EHsc " +
       "/I `"$thisDir`" /I `"$kilnfwDrivers`" " +
       "/Fo:`"$outDir\\`" /Fe:`"$exe`" $sourceArgs"

cmd.exe /c $cmd
if ($LASTEXITCODE -ne 0) {
    throw "build failed"
}

Write-Host "Built $exe"
