# check_thermo_snapshot_producers.ps1 -- every field of thermo_snapshot_t
# must actually be assigned by SOME production (non-test) source file under
# src/, not only by a test.
#
# WHY THIS EXISTS. docs/audits/unreviewed_changes_review_2026-09-08.md finding
# D1: d6b643a4 added `cj_valid` to thermo_snapshot_t and updated the two
# live-hardware producer branches in thermo_task_fn() but missed the third
# producer, thermo_task_inject_reading() -- its file-static
# `s_inject_snapshot.cj_valid` stayed static-zeroed (false) forever, so every
# injected reading reported the cold junction invalid on the wire regardless
# of what was injected. This is the "consumer without producer" / "field
# added to a struct, one producer left behind" class this project keeps
# hitting (see check_guard_input_producers.ps1's header for three earlier
# instances) -- except here there are multiple PRODUCERS of the same struct
# rather than one, so a field can be wired into most of them and still be
# silently wrong from the one left out.
#
# This check is keyed on symbols (the struct name, the field names, and
# assignment syntax), not on which function or line they live at, so it does
# not need to know how many producers exist or enumerate them by name -- it
# only requires that EVERY field of thermo_snapshot_t is assigned somewhere
# in production code. It cannot prove a producer is CORRECT, or that a
# specific producer (e.g. injection) sets a field consistently with its own
# documented contract -- only that at least one production assignment site
# exists for each field. Same limit the sibling checks accept.
#
# Usage: powershell -ExecutionPolicy Bypass -File check_thermo_snapshot_producers.ps1
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$srcDir = Join-Path $root "src"
$snapshotsHeader = Join-Path $srcDir "snapshots.h"

if (-not (Test-Path $snapshotsHeader)) {
    throw "check_thermo_snapshot_producers: $snapshotsHeader not found -- did the file move? This check is now blind, which is worse than the bug it looks for."
}

# --- 1. The field list, from thermo_snapshot_t in snapshots.h ---
$headerLines = Get-Content -Path $snapshotsHeader
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
        if ($line -match 'thermo_snapshot_t\s*;') {
            break   # this was the struct we wanted; $fields holds its members
        }
        $fields = @()   # some other struct in this header -- discard, keep looking
        continue
    }

    $code = $line
    if ($code -match '^\s*(\*|/\*|//)') { continue }
    # Strip a same-line trailing comment before extracting member names, so
    # `float tc_c, cj_c;      /* NaN when !valid */` doesn't have "NaN" etc
    # mistaken for a second declaration.
    $codeIdx = $code.IndexOf("//")
    if ($codeIdx -ge 0) { $code = $code.Substring(0, $codeIdx) }
    $codeIdx = $code.IndexOf("/*")
    if ($codeIdx -ge 0) { $code = $code.Substring(0, $codeIdx) }
    if ($code -notmatch ';') { continue }
    if ($code -match '^\s*[A-Za-z_][A-Za-z0-9_ ]*\s+(\**[A-Za-z_][A-Za-z0-9_]*(\[[^\]]*\])?\s*(,\s*\**[A-Za-z_][A-Za-z0-9_]*(\[[^\]]*\])?\s*)*)\s*;') {
        $declList = $Matches[1]
        foreach ($decl in ($declList -split ',')) {
            if ($decl -match '\**\s*([A-Za-z_][A-Za-z0-9_]*)\s*(\[[^\]]*\])?\s*$') {
                $fields += $Matches[1]
            }
        }
    }
}

if ($fields.Count -eq 0) {
    throw "check_thermo_snapshot_producers: parsed ZERO fields out of thermo_snapshot_t in $snapshotsHeader. The parser is broken or the struct moved -- failing loudly rather than reporting a vacuous pass."
}

# Comment stripper -- same technique as the sibling checks.
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

# --- 2. What every PRODUCTION (non-test) .c/.h file under src/ assigns ---
# Scanned across the whole tree, not just thermo_task.c, deliberately: this
# check must not assume how many producers exist or where they live, only
# that a field is assigned somewhere in production code. Excluding a
# subtree by name would risk a false failure someone "fixes" by weakening
# the check instead of fixing the real gap.
$sourceFiles = Get-ChildItem -Path $srcDir -Include "*.c","*.h" -Recurse -File |
    Where-Object { $_.FullName -notmatch '\\test\\' -and $_.FullName -notmatch '\\tests\\' }

if ($sourceFiles.Count -eq 0) {
    throw "check_thermo_snapshot_producers: found ZERO production source files under $srcDir -- wrong directory, or the check would pass vacuously."
}

$scanText = ""
foreach ($f in $sourceFiles) {
    $scanText += ((Get-CodeOnlyLines -Path $f.FullName) -join "`n") + "`n"
}

if ($scanText -notmatch 'thermo_snapshot_t') {
    throw "check_thermo_snapshot_producers: no production file even references thermo_snapshot_t -- this check is looking at the wrong tree and would pass vacuously."
}

$missing = @()
foreach ($field in $fields) {
    $escaped = [regex]::Escape($field)
    $assigned = ($scanText -match "\.\s*$escaped\s*=") -or
                ($scanText -match "->\s*$escaped\s*=") -or
                ($scanText -match "\[\s*$escaped\s*\]")
    if (-not $assigned) {
        $missing += $field
    }
}

if ($missing.Count -gt 0) {
    Write-Host "THERMO SNAPSHOT PRODUCER CHECK FAILED:" -ForegroundColor Red
    foreach ($m in $missing) {
        Write-Host "  thermo_snapshot_t.$m is never assigned by any production file in src/" -ForegroundColor Red
    }
    Write-Host ""
    Write-Host "  A thermo_snapshot_t field with no producer is silently zero/false on" -ForegroundColor Red
    Write-Host "  every board -- see docs/audits/unreviewed_changes_review_2026-09-08.md" -ForegroundColor Red
    Write-Host "  finding D1 for the instance (cj_valid, missed in the injection" -ForegroundColor Red
    Write-Host "  producer) that motivated this check." -ForegroundColor Red
    throw "$($missing.Count) thermo_snapshot_t field(s) have no production producer"
}

Write-Host "Thermo snapshot producer check passed: all $($fields.Count) thermo_snapshot_t fields are assigned somewhere in src/ (excluding test/)."
exit 0
