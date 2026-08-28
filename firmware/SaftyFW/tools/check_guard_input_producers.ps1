# check_guard_input_producers.ps1 -- every field of safety_guard_input_t must
# actually be assigned by safety_core_build_input().
#
# WHY THIS EXISTS. This project has now shipped the same defect three times,
# and each instance was invisible for weeks or months because the code READ
# correctly at every site anyone thought to look at:
#
#   1. S13's `sample_counter_advancing` was hardcoded false. The guard was
#      written, host-tested and wired -- and could never fire, because nothing
#      ever produced its input.
#   2. `current_sense_set_cal()` had no caller anywhere in src/, so `amps[]`
#      was permanently 0 and `any_current_present` permanently false. That
#      silently disabled S3, S9, S11 and S6b's current-gated trip while S4
#      warned forever. Found 2026-08-24, long after the guards were "done".
#   3. `current_sensing_commissioned` (2026-08-27) arrived from a fix with no
#      producer, which would have left it false on EVERY board -- permanently
#      downgrading S9 to a warning even after commissioning. Caught the same
#      day, by reading, not by any test.
#
# The shape is always identical: a guard input field exists, the guard that
# reads it is correct, the tests that cover the guard pass, and the field is
# never written on the real board. Nothing in the type system, the compiler or
# the host tests can see it, because a designated initializer that omits a
# field is perfectly legal C -- the omitted field is simply zero, which for a
# bool means "false", which for a guard input almost always means "this guard
# is off".
#
# So this check reads safety_guards.h for the field list, reads
# safety_core_build_input() for what it assigns, and fails on the difference.
# It cannot prove a producer is CORRECT -- only that one exists. That is worth
# having anyway: all three defects above were missing producers, not wrong
# ones.
#
# Usage: powershell -File tools\check_guard_input_producers.ps1
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$guardsHeader = Join-Path $root "src\safety_guards.h"
$coreSource = Join-Path $root "src\tasks\safety_core.c"

foreach ($f in @($guardsHeader, $coreSource)) {
    if (-not (Test-Path $f)) {
        throw "check_guard_input_producers: $f not found -- did the file move? This check is now blind, which is worse than the bug it looks for."
    }
}

# --- 1. The field list, from safety_guard_input_t in safety_guards.h ---
$headerLines = Get-Content -Path $guardsHeader
$inStruct = $false
$fields = @()
$depth = 0
foreach ($line in $headerLines) {
    if (-not $inStruct) {
        # The struct is declared `typedef struct { ... } safety_guard_input_t;`
        # -- find its opening by looking for the typedef's own comment-free
        # start. Matching the CLOSING name is what makes this robust to the
        # struct being renamed or moved: we scan forward from a `typedef
        # struct` and only keep the block that closes as safety_guard_input_t.
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
        if ($line -match 'safety_guard_input_t\s*;') {
            break   # this was the struct we wanted; $fields holds its members
        }
        $fields = @()   # some other struct -- discard and keep looking
        continue
    }

    # A member declaration: a type, then a name, then `;`. Skip comment lines
    # (this header's comments are long and full of field names in prose, which
    # is exactly how a naive grep would produce a wrong field list).
    $code = $line
    if ($code -match '^\s*(\*|/\*|//)') { continue }
    if ($code -match '^\s*[A-Za-z_][A-Za-z0-9_ ]*\s+\**([A-Za-z_][A-Za-z0-9_]*)\s*(\[[^\]]*\])?\s*;') {
        $fields += $Matches[1]
    }
}

if ($fields.Count -eq 0) {
    throw "check_guard_input_producers: parsed ZERO fields out of safety_guard_input_t in $guardsHeader. The parser is broken or the struct moved -- failing loudly rather than reporting a vacuous pass."
}

# Comment stripper, same as check_isolation.ps1 and
# check_link_impl_isolation.ps1 carry (duplicated rather than imported --
# this project has no shared PowerShell module mechanism).
#
# It is precautionary rather than load-bearing today, and the distinction is
# worth stating honestly: safety_core.c currently carries a comment reading
# "in->current_sensing_commissioned so an uncommissioned board cannot latch",
# which does NOT satisfy the assignment regex only because no `=` follows the
# field name. One reworded sentence would change that. The prose in this
# header and in safety_guards.h names these fields constantly, and a comment
# is written at the moment the code is thought about -- so it long outlives
# the code being deleted, which is exactly when this check needs to fire.
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

# --- 2. What safety_core_build_input() assigns ---
$sourceText = (Get-CodeOnlyLines -Path $coreSource) -join "`n"
if ($sourceText -notmatch 'safety_core_build_input') {
    throw "check_guard_input_producers: safety_core_build_input() not found in $coreSource -- this check is looking at the wrong function and would pass vacuously."
}

# Everything from the function's start to the end of file is scanned for
# designated initializers (`.field = ...`) and plain assignments
# (`in.field = ...` / `in->field = ...`). Scanning to EOF rather than trying to
# find the function's closing brace is deliberate: over-scanning can only
# produce a false PASS if some OTHER function assigns the same field name,
# which is itself a producer -- while under-scanning produces a false FAILURE
# that someone would "fix" by weakening this check.
$buildIdx = $sourceText.IndexOf('safety_core_build_input')
$scanText = $sourceText.Substring($buildIdx)

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
    Write-Host "GUARD INPUT PRODUCER CHECK FAILED:" -ForegroundColor Red
    foreach ($m in $missing) {
        Write-Host "  safety_guard_input_t.$m is never assigned in safety_core_build_input()" -ForegroundColor Red
    }
    Write-Host ""
    Write-Host "  A guard input with no producer is silently false on every board." -ForegroundColor Red
    Write-Host "  For a bool that almost always means the guard reading it is OFF," -ForegroundColor Red
    Write-Host "  while its own host tests keep passing. See this script's header" -ForegroundColor Red
    Write-Host "  for the three times this has already happened here." -ForegroundColor Red
    throw "$($missing.Count) guard input field(s) have no producer"
}

Write-Host "Guard input producer check passed: all $($fields.Count) safety_guard_input_t fields are assigned in safety_core_build_input()."
exit 0
