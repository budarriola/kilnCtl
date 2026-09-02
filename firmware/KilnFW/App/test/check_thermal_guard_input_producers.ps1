# check_thermal_guard_input_producers.ps1 -- every field of
# thermal_guard_input_t must actually be assigned by SOME production
# (non-test) source file in App/drivers, not only by a test.
#
# WHY THIS EXISTS. SaftyFW's check_guard_input_producers.ps1 (see that
# script's own header) documents three shipped instances of the same defect:
# a guard input field exists, the guard code that reads it is correct, the
# host tests that cover the guard pass -- because the TEST supplies the value
# by hand -- and the field is never written anywhere on the real board. A
# designated initializer that omits a field is legal C; the omitted field is
# simply zero, which for a bool almost always means "this guard input is
# off", silently, with nothing in the type system, the compiler, or a
# passing test suite able to see it.
#
# thermal_guard_input_t (thermal_guard.h) is KilnFW's analogous struct: guard
# 1-8's per-tick input, built fresh at TWO known production call sites
# (autotune_engine.c and profile_executor.c) rather than SaftyFW's single
# safety_core_build_input(). That is why this check scans every non-test .c
# file in App/drivers for an assignment, instead of one named function --
# SaftyFW's narrower per-function scan does not fit a struct with more than
# one legitimate producer, and scanning only one of the two real call sites
# would flag fields that the OTHER site correctly supplies (a false failure
# someone would "fix" by weakening the check, which is worse than not having
# it).
#
# It cannot prove a producer is CORRECT, or that a field is wired at every
# call site that needs it -- only that at least one production file assigns
# it somewhere. That is the same limit SaftyFW's check accepts, and it is
# still worth having: every real instance so far has been a field with ZERO
# producers, not a subtly wrong one.
#
# Usage: powershell -ExecutionPolicy Bypass -File check_thermal_guard_input_producers.ps1
$ErrorActionPreference = "Stop"

$testDir = $PSScriptRoot
$driversDir = Split-Path -Parent $testDir
$driversDir = Join-Path $driversDir "drivers"
$guardHeader = Join-Path $driversDir "thermal_guard.h"

if (-not (Test-Path $guardHeader)) {
    throw "check_thermal_guard_input_producers: $guardHeader not found -- did the file move? This check is now blind, which is worse than the bug it looks for."
}
if (-not (Test-Path $driversDir)) {
    throw "check_thermal_guard_input_producers: $driversDir not found."
}

# --- 1. The field list, from thermal_guard_input_t in thermal_guard.h ---
$headerLines = Get-Content -Path $guardHeader
$inStruct = $false
$fields = @()
$depth = 0
foreach ($line in $headerLines) {
    if (-not $inStruct) {
        if ($line -match '^\s*typedef\s+struct\s*\{') {
            $inStruct = $true
            $depth = 1
            $fields = @()
        }
        continue
    }

    $depth += ([regex]::Matches($line, '\{')).Count
    $depth -= ([regex]::Matches($line, '\}')).Count

    if ($depth -le 0) {
        $inStruct = $false
        if ($line -match 'thermal_guard_input_t\s*;') {
            break   # this was the struct we wanted; $fields holds its members
        }
        $fields = @()   # some other struct in this header -- discard, keep looking
        continue
    }

    $code = $line
    if ($code -match '^\s*(\*|/\*|//)') { continue }
    if ($code -match '^\s*(const\s+)?[A-Za-z_][A-Za-z0-9_ ]*\s+\**([A-Za-z_][A-Za-z0-9_]*)\s*(\[[^\]]*\])?\s*;') {
        $fields += $Matches[2]
    }
}

if ($fields.Count -eq 0) {
    throw "check_thermal_guard_input_producers: parsed ZERO fields out of thermal_guard_input_t in $guardHeader. The parser is broken or the struct moved -- failing loudly rather than reporting a vacuous pass."
}

# Comment stripper -- same technique as SaftyFW's check_guard_input_producers.ps1.
function Get-CodeOnlyLines {
    param([string]$Path)
    $inBlockComment = $false
    $result = @()
    foreach ($line in (Get-Content -Path $Path)) {
        $code = $line
        if ($inBlockComment) {
            $endIdx = $code.IndexOf("*/")
            if ($endIdx -ge 0) { $code = $code.Substring($endIdx + 2); $inBlockComment = $false }
            else { $result += ""; continue }
        }
        $lineCommentIdx = $code.IndexOf("//")
        if ($lineCommentIdx -ge 0) { $code = $code.Substring(0, $lineCommentIdx) }
        while ($true) {
            $startIdx = $code.IndexOf("/*")
            if ($startIdx -lt 0) { break }
            $endIdx = $code.IndexOf("*/", $startIdx)
            if ($endIdx -ge 0) { $code = $code.Substring(0, $startIdx) + $code.Substring($endIdx + 2) }
            else { $code = $code.Substring(0, $startIdx); $inBlockComment = $true; break }
        }
        $result += $code
    }
    return $result
}

# --- 2. What every PRODUCTION (non-test) .c file in App/drivers assigns ---
# "Production" excludes App/drivers/test entirely -- that is exactly the
# directory a test would supply the value by hand from, which is the failure
# mode this check exists to catch, not paper over.
$sourceFiles = Get-ChildItem -Path $driversDir -Filter "*.c" -Recurse -File |
    Where-Object { $_.FullName -notmatch '\\test\\' }

if ($sourceFiles.Count -eq 0) {
    throw "check_thermal_guard_input_producers: found ZERO production .c files under $driversDir -- wrong directory, or the check would pass vacuously."
}

$scanText = ""
foreach ($f in $sourceFiles) {
    $scanText += ((Get-CodeOnlyLines -Path $f.FullName) -join "`n") + "`n"
}

if ($scanText -notmatch 'thermal_guard_input_t') {
    throw "check_thermal_guard_input_producers: no production file even references thermal_guard_input_t -- this check is looking at the wrong tree and would pass vacuously."
}

$missing = @()
foreach ($field in $fields) {
    $escaped = [regex]::Escape($field)
    $assigned = ($scanText -match "\.\s*$escaped\s*=") -or
                ($scanText -match "->\s*$escaped\s*=")
    if (-not $assigned) {
        $missing += $field
    }
}

if ($missing.Count -gt 0) {
    Write-Host "THERMAL GUARD INPUT PRODUCER CHECK FAILED:" -ForegroundColor Red
    foreach ($m in $missing) {
        Write-Host "  thermal_guard_input_t.$m is never assigned by any production file in App/drivers" -ForegroundColor Red
    }
    Write-Host ""
    Write-Host "  A guard input with no producer is silently false/0 on every board." -ForegroundColor Red
    Write-Host "  For a bool that almost always means the guard reading it is OFF," -ForegroundColor Red
    Write-Host "  while its own host tests keep passing because the test supplies" -ForegroundColor Red
    Write-Host "  the value by hand. See SaftyFW/tools/check_guard_input_producers.ps1's" -ForegroundColor Red
    Write-Host "  header for the three times this exact shape has already happened." -ForegroundColor Red
    throw "$($missing.Count) thermal_guard_input_t field(s) have no production producer"
}

Write-Host "Thermal guard input producer check passed: all $($fields.Count) thermal_guard_input_t fields are assigned somewhere in App/drivers (excluding test/)."
exit 0
