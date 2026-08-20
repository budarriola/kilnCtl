# Builds virtual_simfw.exe with MSVC `cl` directly (mirrors
# firmware/SimFW/test/build_host_tests.ps1's pattern -- cmake/ninja are not
# reliably on PATH in this environment, see that script's own header).
#
# Usage: powershell -File build_host.ps1
$ErrorActionPreference = "Stop"

$vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
if (-not (Test-Path $vcvars)) {
    throw "vcvarsall.bat not found at $vcvars -- update this path if MSVC Build Tools moved/were reinstalled."
}

$root = $PSScriptRoot
$simDir = Join-Path $root "..\..\src\sim"
$tasksDir = Join-Path $root "..\..\src\tasks"
$simfwSrcDir = Join-Path $root "..\..\src"
$commonDir = Join-Path $root "..\..\..\CommonFW"
$commonInc = Join-Path $commonDir "include"
$commonSrc = Join-Path $commonDir "src"
$outDir = Join-Path $root "build"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$exe = Join-Path $outDir "virtual_simfw.exe"

$sources = @(
    (Join-Path $root "src\virtual_simfw.c"),
    (Join-Path $simDir "thermal_model.c"),
    (Join-Path $simDir "max31856_regs.c"),
    (Join-Path $simDir "fault_engine.c"),
    (Join-Path $simDir "tc_fault_state.c"),
    (Join-Path $simDir "sine_synth.c"),
    (Join-Path $commonSrc "benchproto_crc.c"),
    (Join-Path $commonSrc "benchproto_frame.c"),
    (Join-Path $commonSrc "benchproto_link.c")
)

$sourceArgs = ($sources | ForEach-Object { '"' + $_ + '"' }) -join " "
$cmd = "call `"$vcvars`" x64 >nul && cl /nologo /W3 /EHsc " +
    "/I `"$root\src`" /I `"$simDir`" /I `"$tasksDir`" /I `"$simfwSrcDir`" /I `"$commonInc`" " +
    "/Fo:`"$outDir\\`" /Fe:`"$exe`" $sourceArgs ws2_32.lib"

cmd.exe /c $cmd
if ($LASTEXITCODE -ne 0) {
    throw "build failed"
}

Write-Host "built: $exe"
