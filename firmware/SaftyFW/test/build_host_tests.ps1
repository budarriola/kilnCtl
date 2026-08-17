# Builds and runs the host-side unit tests for safety_guards.c with MSVC,
# entirely off-target -- no pico-sdk, no FreeRTOS, no hardware. Mirrors
# firmware/KilnFW/App/test/build_host_tests.ps1's pattern. TODO.md Phase 4.
#
# Usage: powershell -File test\build_host_tests.ps1
$ErrorActionPreference = "Stop"

$vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
if (-not (Test-Path $vcvars)) {
    throw "vcvarsall.bat not found at $vcvars -- update this path if MSVC Build Tools moved/were reinstalled."
}

$testDir = $PSScriptRoot
$srcDir = Join-Path $testDir "..\src"
$bootDir = Join-Path $testDir "..\bootloader"
$outDir = Join-Path $testDir "build"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$exe = Join-Path $outDir "saftyfw_host_tests.exe"

$sources = @(
    (Join-Path $testDir "test_main.c"),
    (Join-Path $testDir "test_safety_guards.c"),
    (Join-Path $testDir "test_link_frame.c"),
    (Join-Path $testDir "test_bootloader_metadata.c"),
    (Join-Path $srcDir "safety_guards.c"),
    (Join-Path $srcDir "tasks\link_frame.c"),
    (Join-Path $bootDir "crc32.c"),
    (Join-Path $bootDir "metadata.c")
)

$sourceArgs = ($sources | ForEach-Object { '"' + $_ + '"' }) -join " "
$cmd = "call `"$vcvars`" x64 >nul && cl /nologo /W4 /WX /EHsc /I `"$srcDir`" /I `"$bootDir`" /Fo:`"$outDir\\`" /Fe:`"$exe`" $sourceArgs"

cmd.exe /c $cmd
if ($LASTEXITCODE -ne 0) {
    throw "build failed"
}

& $exe
exit $LASTEXITCODE
