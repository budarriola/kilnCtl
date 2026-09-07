# check_safety_link_status_producers.ps1 -- every field of
# safety_link_status_t must actually be assigned by SOME production
# (non-test) source file in App/drivers, not only by a test.
#
# WHY THIS EXISTS. safety_link_status_t (safety/safety_link.h) is the ESP's
# cache of everything the RP2040 safety processor reports over the hardened
# UART link -- Frame A status bytes, Frame E power telemetry, Frame B DIAG,
# Frame D trip events. Every field in it is the ESP-side half of a producer/
# consumer PAIR whose other half lives on the Pico: this repo's own
# CLAUDE.md names exactly this shape ("reset one side of a pair" bug class)
# as a recurring, silent source of real debugging time, and the sibling
# checks (check_thermal_guard_input_producers.ps1,
# check_thermal_guard_cfg_producers.ps1, check_profile_executor_wd_input_
# producers.ps1) cover the same "reader exists, writer does not" failure for
# other structs. Here the risk is sharper: a field that is read by dashboard_
# http.c / ui_page_diagnostics.c / a guard but never actually written by the
# frame-parsing code in safety/safety_link_frames.c stays at its safety_
# link_start()-time zero/NaN init FOREVER, on every board, and nothing
# distinguishes that from "the Pico legitimately hasn't sent this yet" --
# both read as the same falsy/NaN value, so a missing producer here is
# exactly as invisible as the thermal_guard_cfg_t case that motivated the
# original check.
#
# It cannot prove a producer is CORRECT (byte offset right, units right), or
# that the field is wired all the way from the wire frame rather than from
# an unrelated hardcoded value -- only that at least one production file
# assigns the field somewhere. Same limit the sibling checks accept. It also
# does not (and cannot, from this side of the link) prove the PICO side
# actually sends the field either -- that half is CommonFW/SaftyFW's
# responsibility and outside App/drivers.
#
# WHAT WOULD INVALIDATE THIS CHECK: it is keyed on the struct name
# safety_link_status_t wherever it is declared under App/drivers, not on a
# filename -- safety_link.h could be split (several oversized KilnFW files
# already have been) without breaking the parser, since it searches every
# header under drivers/ for the struct. It WOULD go blind if
# safety_link_status_t were renamed without updating the search below, or if
# the assignment style moved from `link->cached.<field> = ...` /
# `out-><field> = ...` field-by-field writes (safety_link_frames.c,
# safety_link.c) to a whole-struct designated-initializer literal or a
# memcpy of a wire-layout-compatible struct -- either would need the grep
# pattern extended to catch it. It also does not attempt to distinguish the
# handful of fields that are legitimately never "assigned" outside their
# NaN/zero init (there are none currently -- every field in this struct has
# at least one real production writer as of this check's introduction; if a
# genuinely producer-less field is ever added on purpose, it belongs in an
# explicit allowlist here, not a silent pass).
#
# Usage: powershell -ExecutionPolicy Bypass -File check_safety_link_status_producers.ps1
$ErrorActionPreference = "Stop"

$testDir = $PSScriptRoot
$driversDir = Split-Path -Parent $testDir
$driversDir = Join-Path $driversDir "drivers"

if (-not (Test-Path $driversDir)) {
    throw "check_safety_link_status_producers: $driversDir not found."
}

# --- 0. Find the header declaring safety_link_status_t, by SYMBOL, not by
# an assumed filename -- see header comment on invalidation. ---
$headerCandidates = Get-ChildItem -Path $driversDir -Filter "*.h" -File -Recurse |
    Where-Object { (Select-String -Path $_.FullName -Pattern 'safety_link_status_t' -SimpleMatch -Quiet) }

if ($headerCandidates.Count -eq 0) {
    throw "check_safety_link_status_producers: no header under $driversDir mentions safety_link_status_t -- has the struct been renamed? This check is now blind, which is worse than the bug it looks for."
}

# safety_link.h both declares the struct and mentions it in comments
# elsewhere in the same file -- find the file whose typedef actually parses
# the struct out, rather than requiring exactly one match.
$guardHeader = $null
foreach ($candidate in $headerCandidates) {
    $lines = Get-Content -Path $candidate.FullName
    $inStruct = $false
    $depth = 0
    foreach ($line in $lines) {
        if (-not $inStruct) {
            if ($line -match '^\s*typedef\s+struct\s*\{') { $inStruct = $true; $depth = 1 }
            continue
        }
        $depth += ([regex]::Matches($line, '\{')).Count
        $depth -= ([regex]::Matches($line, '\}')).Count
        if ($depth -le 0) {
            if ($line -match 'safety_link_status_t\s*;') { $guardHeader = $candidate.FullName }
            $inStruct = $false
        }
    }
    if ($guardHeader) { break }
}

if (-not $guardHeader) {
    throw "check_safety_link_status_producers: safety_link_status_t is mentioned under $driversDir but no typedef struct { ... } safety_link_status_t; was found -- has its declaration style changed? This check is now blind."
}

# --- 1. The field list, from safety_link_status_t's typedef ---
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
        if ($line -match 'safety_link_status_t\s*;') {
            break   # this was the struct we wanted; $fields holds its members
        }
        $fields = @()   # some other struct in this header -- discard, keep looking
        continue
    }

    $code = $line
    if ($code -match '^\s*(\*|/\*|//)') { continue }
    if ($code -match '^\s*(const\s+)?(unsigned\s+|signed\s+)?[A-Za-z_][A-Za-z0-9_]*\s+\**([A-Za-z_][A-Za-z0-9_]*)\s*(\[[^\]]*\])?\s*;') {
        $fields += $Matches[3]
    }
}

if ($fields.Count -eq 0) {
    throw "check_safety_link_status_producers: parsed ZERO fields out of safety_link_status_t in $guardHeader. The parser is broken or the struct moved -- failing loudly rather than reporting a vacuous pass."
}

# Comment stripper -- same technique as the sibling thermal_guard checks.
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

# --- 2. What every PRODUCTION (non-test) .c file under drivers/safety
# assigns --- Deliberately scoped to the safety/ subtree ONLY, not the whole
# drivers tree like the sibling checks: safety_link_status_t is filled
# exclusively by safety_link_frames.c/safety_link.c (link->cached.<field> =
# ..., or safety_link_get_status()'s out-><field> = ... overrides after the
# whole-struct *out = link->cached; copy). Scanning the whole tree was tried
# first and produced a FALSE PASS: dashboard_http.c has its own unrelated
# local variable also named "out" (a different struct) with a field also
# named diag_ever_received ("out->diag_ever_received = sl.diag_ever_received;"),
# which is a READ of the real field into a copy, not a producer -- a bare
# "\bout\s*->\s*field\s*=" search anywhere in App/drivers matched that copy
# and hid a genuinely missing safety_link_frames.c producer during this
# check's own negative-testing. Scoping to safety/ is what makes the check
# honest for this specific struct. This WOULD need revisiting (and is exactly
# the kind of thing that would invalidate this check) if the frame-parsing
# code ever moved out of drivers/safety/.
$safetyDir = Join-Path $driversDir "safety"
if (-not (Test-Path $safetyDir)) {
    throw "check_safety_link_status_producers: $safetyDir not found -- has the safety-link code moved out of drivers/safety? This check is now blind."
}
$sourceFiles = Get-ChildItem -Path $safetyDir -Filter "*.c" -Recurse -File |
    Where-Object { $_.FullName -notmatch '\\test\\' }

if ($sourceFiles.Count -eq 0) {
    throw "check_safety_link_status_producers: found ZERO production .c files under $safetyDir -- wrong directory, or the check would pass vacuously."
}

$scanText = ""
foreach ($f in $sourceFiles) {
    $scanText += ((Get-CodeOnlyLines -Path $f.FullName) -join "`n") + "`n"
}

if ($scanText -notmatch 'safety_link_status_t') {
    throw "check_safety_link_status_producers: no production file even references safety_link_status_t -- this check is looking at the wrong tree and would pass vacuously."
}

$missing = @()
foreach ($field in $fields) {
    $escaped = [regex]::Escape($field)
    # Scoped tighter than the sibling checks: safety_link_status_t is copied
    # whole (*out = link->cached;) in safety_link_get_status(), so a bare
    # "->field =" or ".field =" anywhere in App/drivers would also match
    # unrelated structs that happen to reuse a field name (e.g. dashboard_
    # http.c's "out->diag_ever_received = sl.diag_ever_received;", which
    # copies an ALREADY-READ value into a different, unrelated output
    # struct and would let a missing real producer hide behind it). The
    # only two places that actually populate safety_link_status_t are
    # safety_link_frames.c/safety_link.c writing through "link->cached.<field>"
    # and safety_link.c's safety_link_get_status() overriding a few fields
    # directly on "out-><field>" (age_ms, link_up, fault_asserted,
    # trip_event_age_ms) after the whole-struct copy -- so the search is
    # restricted to those two spellings.
    $assigned = ($scanText -match "cached\s*\.\s*$escaped\s*=") -or
                ($scanText -match "cached\s*\.\s*$escaped\s*\[") -or
                ($scanText -match "\bout\s*->\s*$escaped\s*=") -or
                ($scanText -match "\bout\s*->\s*$escaped\s*\[")
    if (-not $assigned) {
        $missing += $field
    }
}

if ($missing.Count -gt 0) {
    Write-Host "SAFETY LINK STATUS PRODUCER CHECK FAILED:" -ForegroundColor Red
    foreach ($m in $missing) {
        Write-Host "  safety_link_status_t.$m is never assigned by any production file in App/drivers" -ForegroundColor Red
    }
    Write-Host ""
    Write-Host "  A status field with no producer is stuck at its safety_link_start()" -ForegroundColor Red
    Write-Host "  zero/NaN init forever, indistinguishable from 'the Pico hasn't sent" -ForegroundColor Red
    Write-Host "  this yet' -- exactly the failure shape CLAUDE.md's 'reset one side of" -ForegroundColor Red
    Write-Host "  a pair' bug class describes, on the field-producer axis instead of a" -ForegroundColor Red
    Write-Host "  reset axis. Host tests keep passing because the test fills the struct" -ForegroundColor Red
    Write-Host "  by hand." -ForegroundColor Red
    throw "$($missing.Count) safety_link_status_t field(s) have no production producer"
}

Write-Host "Safety link status producer check passed: all $($fields.Count) safety_link_status_t fields are assigned somewhere in App/drivers (excluding test/)."
exit 0
