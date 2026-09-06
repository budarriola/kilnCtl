# check_host_embed_symbols_defined.ps1 -- every ESP-IDF EMBED_FILES/
# EMBED_TXTFILES symbol (`extern ... asm("_binary_X_start")` /
# `_binary_X_end`) referenced from a firmware/KilnFW/App/drivers/*.c file
# that the HOST test build actually compiles must have a real, non-extern
# host-side definition of that same symbol name somewhere under
# firmware/KilnFW/App/test/*.c.
#
# WHY THIS EXISTS. Real instance, commit 333dd4e (2026-09-02): a new
# GET /api/tuning_recommendations handler was added to
# firmware/KilnFW/App/drivers/zones_http.c referencing
# tuning_recommendations_json_start/_end -- symbols the real ESP-IDF build
# produces via EMBED_FILES (see App/drivers/CMakeLists.txt) but that do not
# exist on the host toolchain. build_kilnfw (the real target) built fine;
# build_host_tests.ps1's zones_http executable failed to LINK, because
# nothing in test_zones_http.c defined those two symbols -- it only defines
# stand-ins for the embed symbols that existed when it was written. This
# broke silently: the host suite's own runner reports "Built: 18/19" and
# exits, so a coordinator only skimming a green run_all_checks.ps1 (which
# does not itself invoke build_host_tests.ps1 -- that is a separate gate)
# would miss it entirely. See test_zones_http.c's own header comment for the
# established convention this check enforces: `#define asm(x)` away the
# GCC-ism, then supply a real (often 1-byte placeholder) definition of every
# symbol the included driver file's `extern ... asm(...)` line names.
#
# WHAT THIS CHECKS. For every *.c file under firmware/KilnFW/App/drivers/
# that is reachable from the host build (determined by scanning
# firmware/KilnFW/App/test/*.c for `#include "../drivers/X.c"` -- the same
# source-inclusion convention test_zones_http.c's header documents; a driver
# file the host build never compiles, e.g. dashboard_http.c, diagnostics_http.c,
# is correctly out of scope and cannot break this gate), collect every
# `asm("_binary_<NAME>_start")` / `asm("_binary_<NAME>_end")` reference. For
# each referenced <NAME>_start/<NAME>_end, at least one *.c file under
# firmware/KilnFW/App/test/ must contain an actual DEFINITION of that exact
# symbol -- a line naming it as an array/pointer with no `extern` on that
# line -- not merely another `extern` re-declaration. A referenced symbol
# with no host-side definition anywhere in test/ is a failure: it will
# compile (the extern declaration is legal) and then fail exactly like
# zones_http did, at the LINK step, which this static check catches without
# needing to invoke the (slow, compiler-dependent) host build itself.
#
# Usage: powershell -File tools\check_host_embed_symbols_defined.ps1 [-DriversDir <path>] [-TestDir <path>]
# -DriversDir/-TestDir are for smoke-testing against a simulated
# post-layer-move tree (or other alternate roots); normal use omits them and
# gets the real repo paths.
param(
    [string]$DriversDir,
    [string]$TestDir
)
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
if ($DriversDir) { $driversDir = $DriversDir } else { $driversDir = Join-Path $root "firmware\KilnFW\App\drivers" }
if ($TestDir) { $testDir = $TestDir } else { $testDir = Join-Path $root "firmware\KilnFW\App\test" }

if (-not (Test-Path $driversDir)) {
    throw "check_host_embed_symbols_defined: $driversDir not found -- has it moved? This check is now blind."
}
if (-not (Test-Path $testDir)) {
    throw "check_host_embed_symbols_defined: $testDir not found -- has it moved? This check is now blind."
}

$testFiles = Get-ChildItem -Path $testDir -Filter "*.c" -File
if ($testFiles.Count -lt 5) {
    throw "check_host_embed_symbols_defined: found implausibly few test/*.c files ($($testFiles.Count)) -- this check has probably gone blind."
}

# --- 1. Which drivers/*.c files does the host build actually compile? ------
# Determined the same way build_host_tests.ps1 reaches static functions: a
# test file #includes the driver .c source directly. A driver file never
# #included by any test/*.c is not part of the host build and is out of
# scope here (its embed symbols, if any, are a real-target-only concern).
$includedDriverFiles = New-Object 'System.Collections.Generic.HashSet[string]'
foreach ($tf in $testFiles) {
    foreach ($line in (Get-Content -Path $tf.FullName)) {
        foreach ($m in [regex]::Matches($line, '#include\s+"\.\./drivers/(?:[A-Za-z0-9_]+/)?([A-Za-z0-9_]+\.c)"')) {
            [void]$includedDriverFiles.Add($m.Groups[1].Value)
        }
    }
}
if ($includedDriverFiles.Count -lt 10) {
    throw "check_host_embed_symbols_defined: found implausibly few host-included drivers/*.c files ($($includedDriverFiles.Count)) -- the #include convention this check parses has probably changed shape."
}

# --- 2. Collect every _binary_ embed symbol referenced from those files ----
# Matches both `extern ... X[] asm("_binary_X_start");` (the convention every
# current call site uses) and a bare `asm("_binary_X_start")` anywhere else,
# so a future reference written slightly differently is still caught.
# Resolve a bare basename to a file anywhere under $driversDir. This makes the
# check agnostic to an upcoming move of every drivers/*.c file into layer
# subdirectories (drivers/<layer>/<file>) -- $includedDriverFiles is parsed
# from #include lines as bare basenames either way. Fails loud (not silently
# picks one) if a name is missing or ambiguous.
function Resolve-DriverFile {
    param([string]$DriversDir, [string]$BaseName)
    $found = Get-ChildItem -Path $DriversDir -Filter $BaseName -File -Recurse
    if ($found.Count -eq 0) {
        throw "check_host_embed_symbols_defined: expected driver file '$BaseName' not found anywhere under $DriversDir -- has it moved or been renamed? Update this script or the #include line it comes from."
    }
    if ($found.Count -gt 1) {
        $paths = ($found | ForEach-Object { $_.FullName }) -join ", "
        throw "check_host_embed_symbols_defined: '$BaseName' matched more than one file under $DriversDir ($paths) -- this script cannot tell which one is the referenced driver file. Disambiguate."
    }
    return $found[0].FullName
}

$refPattern = 'asm\(\s*"_binary_([A-Za-z0-9_]+)_(start|end)"\s*\)'
$referencedSymbols = New-Object 'System.Collections.Generic.HashSet[string]'
$symbolSource = @{}
$resolvedFileCount = 0
foreach ($driverFileName in $includedDriverFiles) {
    $path = Resolve-DriverFile -DriversDir $driversDir -BaseName $driverFileName
    $resolvedFileCount++
    foreach ($line in (Get-Content -Path $path)) {
        foreach ($m in [regex]::Matches($line, $refPattern)) {
            $sym = "$($m.Groups[1].Value)_$($m.Groups[2].Value)"
            [void]$referencedSymbols.Add($sym)
            if (-not $symbolSource.ContainsKey($sym)) {
                $symbolSource[$sym] = $driverFileName
            }
        }
    }
}

# --- 3. Every referenced symbol needs a real (non-extern) definition -------
# somewhere under test/*.c. "Real definition" = a line naming the symbol
# followed by `[` (array) with no `extern` keyword on that same line -- the
# exact shape test_zones_http.c/test_backup_import.c/test_ota_http.c/
# test_profiles_http.c already use for their 1-byte placeholder arrays.
$testFileText = @{}
foreach ($tf in $testFiles) {
    $testFileText[$tf.FullName] = Get-Content -Path $tf.FullName -Raw
}

$violations = @()
foreach ($sym in ($referencedSymbols | Sort-Object)) {
    $defPattern = "(?m)^(?!.*\bextern\b).*\b$([regex]::Escape($sym))\s*\["
    $found = $false
    foreach ($path in $testFileText.Keys) {
        if ([regex]::IsMatch($testFileText[$path], $defPattern)) {
            $found = $true
            break
        }
    }
    if (-not $found) {
        $violations += "$sym (referenced by drivers/$($symbolSource[$sym])): no non-extern definition found under firmware/KilnFW/App/test/*.c"
    }
}

if ($violations.Count -gt 0) {
    Write-Host "HOST EMBED SYMBOLS CHECK FAILED:" -ForegroundColor Red
    foreach ($v in $violations) {
        Write-Host "  $v" -ForegroundColor Red
    }
    Write-Host ""
    Write-Host "  A drivers/*.c file the host build compiles (via a test/*.c #include) references" -ForegroundColor Red
    Write-Host "  an ESP-IDF EMBED_FILES symbol with no host-side definition. It will compile (the" -ForegroundColor Red
    Write-Host "  extern declaration is legal C) and then fail to LINK -- exactly the zones_http" -ForegroundColor Red
    Write-Host "  break commit 333dd4e introduced (tuning_recommendations_json_start/_end). Add a" -ForegroundColor Red
    Write-Host "  1-byte placeholder definition to the relevant test/*.c file, same convention" -ForegroundColor Red
    Write-Host "  test_zones_http.c's header comment documents." -ForegroundColor Red
    throw "$($violations.Count) embed symbol(s) referenced from the host build have no host-side definition"
}

if ($resolvedFileCount -lt $includedDriverFiles.Count) {
    throw "check_host_embed_symbols_defined: only resolved $resolvedFileCount of $($includedDriverFiles.Count) host-included drivers/*.c file(s) -- see errors above."
}
if ($resolvedFileCount -eq 0) {
    throw "check_host_embed_symbols_defined: resolved 0 drivers/*.c files -- this check has gone blind (hollow run) and cannot be trusted to have checked anything."
}
if ($referencedSymbols.Count -eq 0) {
    throw "check_host_embed_symbols_defined: found 0 referenced _binary_* embed symbols across $resolvedFileCount host-included drivers/*.c file(s) -- implausible given known EMBED_FILES usage (e.g. tuning_recommendations_json). The reference pattern has probably gone blind rather than there genuinely being none."
}

Write-Host "Host embed symbols check passed: $($referencedSymbols.Count) _binary_* symbol(s) referenced from $($includedDriverFiles.Count) host-included drivers/*.c file(s), all defined under test/*.c."
exit 0
