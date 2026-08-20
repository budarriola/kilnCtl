# Builds dut_core.exe: the REAL SaftyFW safety_guards.c + relay_grace.c,
# compiled verbatim for the host with MSVC (no pico-sdk, no FreeRTOS, no
# hardware) plus this directory's own stdio harness (main.c) and register
# decode (max31856_decode.c). Mirrors firmware/SaftyFW/test/
# build_host_tests.ps1's pattern and toolchain path exactly -- see this
# directory's README.md for why safety_guards.c/relay_grace.c are the two
# (and only two) SaftyFW files compiled here.
#
# Usage: powershell -File build_host.ps1
$ErrorActionPreference = "Stop"

$vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
if (-not (Test-Path $vcvars)) {
    throw "vcvarsall.bat not found at $vcvars -- update this path if MSVC Build Tools moved/were reinstalled."
}

$thisDir = $PSScriptRoot
$saftyfwSrc = Join-Path $thisDir "..\..\..\..\SaftyFW\src"
$saftyfwTasks = Join-Path $saftyfwSrc "tasks"
$outDir = Join-Path $thisDir "build"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$exe = Join-Path $outDir "dut_core.exe"

# Exactly two SaftyFW sources, both already proven host-portable by
# firmware/SaftyFW/test/build_host_tests.ps1 (safety_guards.c has its own
# dedicated test_safety_guards.c there; relay_grace.c has test_relay_grace.c)
# -- neither is modified, wrapped, or copied here, only compiled.
$sources = @(
    (Join-Path $thisDir "main.c"),
    (Join-Path $thisDir "max31856_decode.c"),
    (Join-Path $saftyfwSrc "safety_guards.c"),
    (Join-Path $saftyfwTasks "relay_grace.c")
)

$sourceArgs = ($sources | ForEach-Object { '"' + $_ + '"' }) -join " "
$cmd = "call `"$vcvars`" x64 >nul && cl /nologo /W4 /WX /EHsc " +
       "/I `"$thisDir`" /I `"$saftyfwSrc`" /I `"$saftyfwTasks`" " +
       "/Fo:`"$outDir\\`" /Fe:`"$exe`" $sourceArgs"

cmd.exe /c $cmd
if ($LASTEXITCODE -ne 0) {
    throw "build failed"
}

Write-Host "Built $exe"
