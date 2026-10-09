# Optional AddressSanitizer/UBSan soak run for test_fuzz_payloads.c, the
# fuzz harness over every kilnlink payload decoder (GUARD_TEST_MATRIX.md
# "Fuzz over every decoder"). NOT part of the default CI build
# (build_host_tests.ps1) -- that build uses plain `cl` with no sanitizer, to
# match every other host test binary in this repo and avoid depending on
# copying the ASan runtime DLL next to a monolithic multi-file test binary
# that also links SaftyFW/KilnFW code never built with /fsanitize elsewhere.
# This script confirms sanitizer coverage is available and clean on this
# toolchain (MSVC Build Tools ships clang-cl's ASan since VS2019 16.9) --
# run it by hand for a deeper soak or after touching any kilnlink_*.c file.
#
# Usage: powershell -File test\run_fuzz_payloads_asan.ps1
#   KILNLINK_FUZZ_SEED / KILNLINK_FUZZ_ITERS env vars work the same as the
#   plain build (see test_fuzz_payloads.c's own header comment).
param([string]$OutDir = "")
$ErrorActionPreference = "Stop"

$vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
if (-not (Test-Path $vcvars)) {
    throw "vcvarsall.bat not found at $vcvars -- update this path if MSVC Build Tools moved/were reinstalled."
}

$commonDir = Split-Path -Parent $PSScriptRoot
$srcDir = Join-Path $commonDir "src"
$incDir = Join-Path $commonDir "include"
$testDir = Join-Path $commonDir "test"
$outDir = if ($OutDir) { $OutDir } else { Join-Path $testDir "build_asan" }
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$exe = Join-Path $outDir "test_fuzz_payloads_asan.exe"

$sources = Get-ChildItem -Path $srcDir -Filter "kilnlink_*.c" | ForEach-Object { $_.FullName }
$sources = @((Join-Path $testDir "test_fuzz_payloads.c")) + $sources
$sourceArgs = ($sources | ForEach-Object { '"' + $_ + '"' }) -join " "

$cmd = "call `"$vcvars`" x64 >nul && cl /nologo /fsanitize=address /Zi /std:c17 /I `"$incDir`" " +
       "/Fo:`"$outDir\\`" /Fe:`"$exe`" $sourceArgs"
cmd.exe /c $cmd
if ($LASTEXITCODE -ne 0) {
    throw "ASan build failed"
}

# The ASan-instrumented exe needs clang_rt.asan_dynamic-x86_64.dll next to
# it; locate it under the same Build Tools install and copy it in.
$asanDll = Get-ChildItem -Path "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Tools\MSVC" `
    -Recurse -Filter "clang_rt.asan_dynamic-x86_64.dll" -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -like "*Hostx64\x64*" } | Select-Object -First 1
if (-not $asanDll) {
    throw "clang_rt.asan_dynamic-x86_64.dll not found under the MSVC Build Tools install -- this VS install may not ship clang-cl's ASan runtime."
}
Copy-Item $asanDll.FullName -Destination $outDir -Force

& $exe
exit $LASTEXITCODE
