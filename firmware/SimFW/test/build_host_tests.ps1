# Builds and runs the host-side unit tests for firmware/SimFW/src/sim/ with
# MSVC, entirely off-target -- no pico-sdk, no FreeRTOS, no hardware. Mirrors
# firmware/SaftyFW/test/build_host_tests.ps1's pattern exactly (docs/PLAN.md
# section 13.1: "everything in src/sim/ is pure and runs on the PC ... same
# MSVC/CMake harness pattern SaftyFW/test uses").
#
# Usage: powershell -File test\build_host_tests.ps1
$ErrorActionPreference = "Stop"

$vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
if (-not (Test-Path $vcvars)) {
    throw "vcvarsall.bat not found at $vcvars -- update this path if MSVC Build Tools moved/were reinstalled."
}

$testDir = $PSScriptRoot
$simDir = Join-Path $testDir "..\src\sim"
$outDir = Join-Path $testDir "build"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$exe = Join-Path $outDir "simfw_host_tests.exe"

$sources = @(
    (Join-Path $testDir "test_main.c"),
    (Join-Path $testDir "test_thermal_model.c"),
    (Join-Path $testDir "test_max31856_regs.c"),
    (Join-Path $testDir "test_sine_synth.c"),
    (Join-Path $testDir "test_fault_engine.c"),
    (Join-Path $testDir "test_tc_fault_state.c"),
    (Join-Path $testDir "test_gap_closure_logic.c"),
    (Join-Path $simDir "thermal_model.c"),
    (Join-Path $simDir "max31856_regs.c"),
    (Join-Path $simDir "sine_synth.c"),
    (Join-Path $simDir "fault_engine.c"),
    (Join-Path $simDir "tc_fault_state.c")
)

$sourceArgs = ($sources | ForEach-Object { '"' + $_ + '"' }) -join " "
$cmd = "call `"$vcvars`" x64 >nul && cl /nologo /W4 /WX /EHsc /I `"$simDir`" /Fo:`"$outDir\\`" /Fe:`"$exe`" $sourceArgs"

cmd.exe /c $cmd
if ($LASTEXITCODE -ne 0) {
    throw "build failed"
}

& $exe
exit $LASTEXITCODE
