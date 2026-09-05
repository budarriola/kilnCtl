# compile_headers.ps1 -- Phase 0 host-compile check for
# firmware/hwAbstraction/interface/*.h.
#
# WHY THIS EXISTS. The interface headers use the C11 `_Alignas` keyword and
# a fixed-size opaque-storage pattern whose whole point is that a backend
# growing its impl struct past the reserved size is a BUILD ERROR, not a
# runtime memory bug (docs/HW_ABSTRACTION_PLAN.md "Opaque handles"). That
# only holds if the headers actually compile under MSVC with /std:c11 (the
# std flag Phase 0 mandates on every host cl invocation that includes an
# interface header) and if a dummy backend's _Static_assert genuinely fires
# when it should. This script is the negative-test proof for both, run
# standalone before any of these headers is wired into build_host_tests.ps1.
#
# WHAT THIS CHECKS, per interface header:
#   1. The header alone compiles under `cl /std:c11 /c` (syntax only).
#   2. hal_status.c compiles and the header's declarations are consistent
#      with it (both built into one .obj set).
#   3. A dummy backend .c demonstrates the _Static_assert opaque-storage
#      pattern: an impl struct sized to fit compiles clean; oversized fails
#      the build. This proves the ABI guarantee is real, not decorative.
#
# Usage: powershell -ExecutionPolicy Bypass -File compile_headers.ps1
#
# Manual-only for now: this script is not yet wired into run_all_checks.ps1
# (Phase 0's commit notes this belongs in Phase 1a). Run it by hand after
# touching any interface header.
$ErrorActionPreference = "Stop"

$here = $PSScriptRoot
$ifaceDir = Join-Path (Split-Path -Parent $here) "interface"
$workDir = Join-Path $here "_work"
if (Test-Path $workDir) { Remove-Item -Recurse -Force $workDir }
New-Item -ItemType Directory -Path $workDir | Out-Null

# Discover vcvarsall.bat via vswhere rather than a hardcoded VS-version path
# (opus review on c626727: the original literal named "18" and would break
# on a different BuildTools install/machine). App/test/build_host_tests.ps1
# still hardcodes that same "18" path today -- this is a deliberate
# improvement over that script, not a copy of its (also-hardcoded) approach.
$vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
$vcvars = $null
if (Test-Path $vswhere) {
    $installPath = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath 2>$null
    if ($installPath) {
        $candidate = Join-Path $installPath "VC\Auxiliary\Build\vcvarsall.bat"
        if (Test-Path $candidate) { $vcvars = $candidate }
    }
}
if (-not $vcvars) {
    # Fallback: the path this repo's other MSVC-invoking script
    # (build_host_tests.ps1) hardcodes today, in case vswhere is absent or
    # found no VC-Tools-bearing install.
    $fallback = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
    if (Test-Path $fallback) { $vcvars = $fallback }
}
if (-not $vcvars) {
    throw "vcvarsall.bat not found via vswhere or the known fallback path -- install MSVC Build Tools with the C++ workload, or update this script's fallback path."
}
Write-Host "Using vcvarsall.bat: $vcvars"

$headers = @("hal_status.h", "hal_spi.h", "hal_i2c.h", "hal_uart.h", "hal_gpio.h", "hal_adc.h")
$failures = @()

function Invoke-Cl {
    param([string]$SourceFile, [string]$OutObj)
    $cmd = "set `"PATH=%PATH%;C:\Program Files (x86)\Microsoft Visual Studio\Installer`" && call `"$vcvars`" x64 >nul && cl /nologo /W3 /std:c11 /I`"$ifaceDir`" /c `"$SourceFile`" /Fo:`"$OutObj`""
    $out = cmd /c $cmd 2>&1
    return @{ ExitCode = $LASTEXITCODE; Output = ($out -join "`n") }
}

# 1) Each header compiles standalone (syntax-only .c that just includes it).
foreach ($h in $headers) {
    $src = Join-Path $workDir "syntax_$($h -replace '\.h$','').c"
    @"
#include "$h"
int hal_test_unused_$($h -replace '[^a-zA-Z0-9]','_');
"@ | Set-Content -Path $src -Encoding ASCII

    $obj = Join-Path $workDir "syntax_$($h -replace '\.h$','').obj"
    $r = Invoke-Cl -SourceFile $src -OutObj $obj
    if ($r.ExitCode -ne 0) {
        $failures += "Header $h failed to compile standalone:`n$($r.Output)"
    } else {
        Write-Host "OK   $h compiles standalone under /std:c11"
    }
}

# 2) hal_status.c compiles.
$statusObj = Join-Path $workDir "hal_status.obj"
$r = Invoke-Cl -SourceFile (Join-Path $ifaceDir "hal_status.c") -OutObj $statusObj
if ($r.ExitCode -ne 0) {
    $failures += "hal_status.c failed to compile:`n$($r.Output)"
} else {
    Write-Host "OK   hal_status.c compiles under /std:c11"
}

# 3a) Dummy backend, impl struct fits its reservation -- must compile clean.
$goodSrc = Join-Path $workDir "dummy_backend_fits.c"
@"
#include "hal_spi.h"
struct dummy_spi_bus_impl { uint8_t a[32]; void *p; };
_Static_assert(sizeof(struct dummy_spi_bus_impl) <= sizeof(((hal_spi_bus_t*)0)->storage),
               "dummy_spi_bus_impl must fit HAL_SPI_BUS_STORAGE_BYTES");
"@ | Set-Content -Path $goodSrc -Encoding ASCII
$goodObj = Join-Path $workDir "dummy_backend_fits.obj"
$r = Invoke-Cl -SourceFile $goodSrc -OutObj $goodObj
if ($r.ExitCode -ne 0) {
    $failures += "Dummy backend that FITS its reservation unexpectedly failed to compile:`n$($r.Output)"
} else {
    Write-Host "OK   dummy backend within HAL_SPI_BUS_STORAGE_BYTES compiles"
}

# 3b) NEGATIVE TEST -- dummy backend, impl struct deliberately oversized.
# This proves the _Static_assert pattern actually fires; a check with no
# proof it can fail is exactly the vacuous-check class this repo's other
# checks (and CLAUDE.md's "negative-test every check") guard against.
$badSrc = Join-Path $workDir "dummy_backend_oversized.c"
@"
#include "hal_spi.h"
struct dummy_spi_bus_impl_oversized { uint8_t a[HAL_SPI_BUS_STORAGE_BYTES + 8]; };
_Static_assert(sizeof(struct dummy_spi_bus_impl_oversized) <= sizeof(((hal_spi_bus_t*)0)->storage),
               "dummy_spi_bus_impl_oversized must fit HAL_SPI_BUS_STORAGE_BYTES");
"@ | Set-Content -Path $badSrc -Encoding ASCII
$badObj = Join-Path $workDir "dummy_backend_oversized.obj"
$r = Invoke-Cl -SourceFile $badSrc -OutObj $badObj
if ($r.ExitCode -eq 0) {
    $failures += "NEGATIVE TEST FAILED: an oversized backend struct compiled clean -- the _Static_assert opaque-storage guarantee is not actually enforced."
} else {
    Write-Host "OK   oversized dummy backend correctly fails the build (_Static_assert fired)"
}

Remove-Item -Recurse -Force $workDir -ErrorAction SilentlyContinue

if ($failures.Count -gt 0) {
    Write-Host "`n=== FAILURES ==="
    foreach ($f in $failures) { Write-Host $f }
    exit 1
}

Write-Host "`nAll hwAbstraction/interface header compile checks passed."
exit 0
