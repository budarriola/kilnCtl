# checkcache: ok
# check_unused_setters.ps1 -- every public *_set_*() function declared in
# firmware/SaftyFW/src/*.h must have at least one CALL site somewhere in
# src/ (outside its own declaration/definition), not just a definition.
#
# WHY THIS EXISTS. check_guard_input_producers.ps1 (this same directory)
# already catches "a safety_guard_input_t field is never assigned" -- but its
# own header comment lists a SECOND instance of the identical defect class
# that check cannot see at all: current_sense_set_cal() had no caller
# anywhere in src/ (found 2026-08-24), so amps[]/any_current_present stayed
# permanently zero/false even though the setter existed, compiled, and was
# perfectly correct C. That instance is a whole FUNCTION with no caller, not
# a struct field with no assignment -- check_guard_input_producers.ps1's
# regex-over-one-function approach cannot generalize to it, because there is
# no single "build" function whose body to scan; the missing call could be
# anywhere in src/, or nowhere.
#
# ROADMAP.md M13's own list of this class also names `i_normal_a` (a field,
# still open) and a documented heartbeat contract with no PC-side
# implementer -- this script covers neither of those (see LIMITATIONS
# below). What it DOES cover is the shape current_sense_set_cal() actually
# was: a *_set_*() function that exists, compiles, is exported via a header,
# and is never called by anything that would make it fire on real hardware.
#
# WHAT THIS CHECKS. Every function declared in a firmware/SaftyFW/src/*.h
# header whose name contains "_set_" (case-insensitive) -- the naming
# convention every producer function in this bug class has used so far
# (current_sense_set_cal, and the guard-input setters this repo already
# reasons about) -- must appear at least once in src/ as something OTHER
# than its own declaration and definition: i.e. a real call site.
#
# HOW A CALL IS DISTINGUISHED FROM A DECLARATION/DEFINITION. This does not
# attempt real C parsing. Instead: every source line containing
# `funcname(` is a "hit" of some kind. The declaration line (ends in `;`,
# in a .h -- possibly wrapped across several lines, see below) and the
# definition line (opens a `{` body, in a .c) are each exactly one hit;
# anything beyond those two possible hits is necessarily a CALL, PROVIDED a
# given name has exactly one declaration line and one definition line. This
# script does not just assume that: it ASSERTS a name is declared exactly
# once across all scanned headers (step 1 below) and throws loudly if not --
# a name declared twice (two headers, or a forward-declaration alongside its
# own definition) would otherwise silently pass with total hits >= 3 even
# with NO call site, since the heuristic has no way to tell "declared twice"
# apart from "declared once and called once".
#
# MULTI-LINE PROTOTYPES. A setter prototype CAN and DOES wrap across
# multiple lines in this codebase -- link_frame_apply_set_config() and
# link_frame_apply_set_ct_cal() (src/tasks/link_frame.h) both do, because
# their parameter lists are long. Step 1 therefore scans each header's
# comment-stripped text AS ONE BLOB (not line by line): `[^)]*` in the
# declaration regex already matches embedded newlines once the text isn't
# split into lines first, so a wrapped prototype is found the same as a
# single-line one. (An earlier version of this script scanned line-by-line
# and reported "all 3" setters while a real 5 existed -- two multi-line
# prototypes were invisible to it. Never re-introduce a line-by-line scan
# for the declaration regex.)
#
# LIMITATIONS (stated honestly, not fixed here):
#   - Only catches the "_set_" naming convention. `sample_counter_advancing`
#     (a struct field, not a function) and `i_normal_a` (a field with no
#     writer) are check_guard_input_producers.ps1's shape, not this one's --
#     that check already covers safety_guard_input_t specifically; a field
#     on some OTHER struct with no writer is covered by NEITHER script today.
#   - The documented-heartbeat-contract instance (uart_bridge.h) has no
#     implementer on the PC side at all -- a Python tool, outside this
#     repo's C source tree entirely. No static check over src/*.c/*.h can
#     see that; it needs a cross-repo/cross-language contract check this
#     script does not attempt.
#   - A function called only from App/test/ (a test double exercising it)
#     would still be flagged, since test/ is excluded from the scan (same
#     convention as check_relay_writes_through_owner.ps1: a test calling a
#     function is not evidence anything on real hardware ever will). If a
#     genuinely test-only setter is ever added, add it to $allowlist below
#     with a reason, the same pattern this repo's other guards use.
#   - Only checks firmware/SaftyFW/src, not firmware/KilnFW or firmware/
#     CommonFW -- narrower is honest; every instance of this exact bug
#     shape happened in SaftyFW so far (see check_guard_input_producers.ps1's
#     own header), and a script that actually fires on the tree it covers
#     beats one that claims broader coverage and cannot prove it.
#   - A call inside `#if 0` or a disabled `#ifdef` branch still counts as a
#     hit -- preprocessor conditionals are not evaluated or stripped, only
#     comments are. A dead call site would hide a real "no live caller" bug.
#   - A call in a .c file that exists under src/ but is not actually listed
#     in CMakeLists.txt (so never compiled into any real target) still
#     counts as a hit -- this scan never consults the build, only the
#     filesystem.
#   - A `static inline` setter defined entirely in a header, or a setter
#     implemented as a `#define` macro, is invisible to this script's
#     declaration regex (which requires a `;`-terminated prototype) and so
#     is skipped entirely -- neither shape exists in this codebase's setter
#     API today, but if one is added this script will not see it.
#
# Usage: powershell -File firmware\SaftyFW\tools\check_unused_setters.ps1
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$srcDir = Join-Path $root "src"
if (-not (Test-Path $srcDir)) {
    throw "check_unused_setters: $srcDir not found -- has it moved? This check is now blind."
}
$srcDirResolved = (Resolve-Path $srcDir).Path

# Comment stripper, same shape as check_guard_input_producers.ps1 and
# check_relay_writes_through_owner.ps1 carry (duplicated rather than
# imported -- this project has no shared PowerShell module mechanism).
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

# --- Allowlist: {Function; Reason}. ---
$allowlist = @(
)

function Test-Allowlisted {
    param([string]$Function)
    foreach ($entry in $allowlist) {
        if ($entry.Function -ieq $Function) { return $true }
    }
    return $false
}

# --- 1. Collect every *_set_* function NAME declared in a src/*.h header. ---
$headerFiles = Get-ChildItem -Path $srcDirResolved -Recurse -File -Include "*.h" |
    Where-Object { $_.FullName -notmatch '[\\/]test[\\/]' -and $_.FullName -notmatch '[\\/]build[\\/]' }

if ($headerFiles.Count -lt 5) {
    throw "check_unused_setters: only $($headerFiles.Count) header(s) found under $srcDirResolved (excluding test/) -- implausibly low, has the tree moved? This check would pass vacuously."
}

$setterNames = New-Object System.Collections.Generic.HashSet[string]
$declCounts = @{}
foreach ($f in $headerFiles) {
    $codeLines = Get-CodeOnlyLines -Path $f.FullName
    # Scan the WHOLE comment-stripped file as one blob, not line by line --
    # `[^)]*` already matches embedded newlines once the text isn't split
    # into lines first, so a prototype wrapped across several lines (e.g.
    # link_frame_apply_set_config()) is found the same as a single-line one.
    $text = $codeLines -join "`n"
    $matches = [regex]::Matches($text, '\b([A-Za-z_][A-Za-z0-9_]*_set_[A-Za-z0-9_]*)\s*\([^)]*\)\s*;')
    foreach ($m in $matches) {
        $name = $m.Groups[1].Value
        [void]$setterNames.Add($name)
        if (-not $declCounts.ContainsKey($name)) { $declCounts[$name] = 0 }
        $declCounts[$name] += 1
    }
}

if ($setterNames.Count -eq 0) {
    throw "check_unused_setters: parsed ZERO '*_set_*' function declarations out of $($headerFiles.Count) header(s). The naming convention or the parser has drifted -- failing loudly rather than reporting a vacuous pass."
}

# The hit-counting heuristic below (step 2) assumes exactly ONE declaration
# line per name -- "<=2 hits means no call" only holds if the two possible
# non-call hits are one declaration and one definition. Assert that
# assumption instead of silently trusting it: a name declared twice (two
# headers, or a duplicate forward-declaration) would otherwise let a
# genuinely uncalled setter pass with 3+ hits and zero real callers.
$dupDecls = $declCounts.GetEnumerator() | Where-Object { $_.Value -gt 1 }
if ($dupDecls) {
    Write-Host "UNUSED SETTER CHECK CANNOT RUN -- heuristic assumption violated:" -ForegroundColor Red
    foreach ($d in $dupDecls) {
        Write-Host "  $($d.Key)() has $($d.Value) declaration lines (expected exactly 1) -- the '<=2 hits = no call' heuristic cannot distinguish this from a real call site." -ForegroundColor Red
    }
    throw "check_unused_setters: duplicate declaration(s) found -- fix the heuristic or the duplicate declaration before trusting this check's result"
}

# --- 2. Count every hit of each name (as `name(`) across all of src/
#        (excluding test/), and flag any with <= 2 (declaration + definition,
#        no call). ---
$sourceFiles = Get-ChildItem -Path $srcDirResolved -Recurse -File -Include "*.c", "*.h" |
    Where-Object { $_.FullName -notmatch '[\\/]test[\\/]' -and $_.FullName -notmatch '[\\/]build[\\/]' }

$hitCounts = @{}
foreach ($name in $setterNames) { $hitCounts[$name] = 0 }

foreach ($f in $sourceFiles) {
    $codeLines = Get-CodeOnlyLines -Path $f.FullName
    $text = $codeLines -join "`n"
    foreach ($name in $setterNames) {
        $escaped = [regex]::Escape($name)
        $hitCounts[$name] += ([regex]::Matches($text, "\b$escaped\s*\(")).Count
    }
}

$violations = @()
foreach ($name in ($setterNames | Sort-Object)) {
    if (Test-Allowlisted -Function $name) { continue }
    if ($hitCounts[$name] -le 2) {
        $violations += "${name}(): $($hitCounts[$name]) hit(s) in src/ (declaration + definition only, or fewer) -- no call site found anywhere"
    }
}

if ($violations.Count -gt 0) {
    Write-Host "UNUSED SETTER CHECK FAILED:" -ForegroundColor Red
    foreach ($v in $violations) {
        Write-Host "  $v" -ForegroundColor Red
    }
    Write-Host ""
    Write-Host "  A *_set_*() function with no caller compiles clean, host-tests clean, and" -ForegroundColor Red
    Write-Host "  silently never runs on real hardware -- whatever it was supposed to feed" -ForegroundColor Red
    Write-Host "  stays at its zero-value default forever. current_sense_set_cal() shipped" -ForegroundColor Red
    Write-Host "  exactly this way (found 2026-08-24, this script's own header has the story)." -ForegroundColor Red
    Write-Host "  Wire a real caller, or add a reasoned allowlist entry if this genuinely has" -ForegroundColor Red
    Write-Host "  none yet (e.g. a producer landing in a later, separate commit)." -ForegroundColor Red
    throw "$($violations.Count) unused setter function(s) found"
}

Write-Host "Unused setter check passed: all $($setterNames.Count) '*_set_*' function(s) declared in firmware/SaftyFW/src/*.h (including multi-line prototypes) -- the full set found -- have at least one call site."
exit 0
