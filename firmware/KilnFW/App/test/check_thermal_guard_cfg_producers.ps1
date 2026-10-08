# check_thermal_guard_cfg_producers.ps1 -- every field of thermal_guard_cfg_t
# must actually be assigned by SOME production (non-test) source file in
# App/drivers, not only by a test.
#
# WHY THIS EXISTS. docs/audits/consumer_without_producer_2026-09-06.md finding
# 1: thermal_guard_cfg_t.progress_band_c was read by thermal_guard.c's
# effective_f() but never appeared in either production designated-initializer
# call site (profile_executor_run.c / autotune_engine.c) or in any
# zones_config accessor, so it was always the compound literal's implicit 0 --
# every zone silently ran the hardcoded firmware default with no way to
# override it, while nothing (compiler, test suite) could see the gap. Fixed
# 2026-09-0x by wiring zones_config_get_progress_band_c()/JSON parse/HTTP.
# This is the sibling struct to thermal_guard_input_t, which already has its
# own analogous check (check_thermal_guard_input_producers.ps1) -- see that
# script's header for the fuller rationale (shared with SaftyFW's
# check_guard_input_producers.ps1). thermal_guard_cfg_t is the *config* half
# (persisted, operator-tunable) rather than the per-tick *input* half, but the
# failure shape is identical: a struct field with a reader and zero writers.
#
# It cannot prove a producer is CORRECT, or that every zone's config is
# threaded through -- only that at least one production file assigns the
# field somewhere. Same limit the sibling check accepts.
#
# Usage: powershell -ExecutionPolicy Bypass -File check_thermal_guard_cfg_producers.ps1
$ErrorActionPreference = "Stop"

$testDir = $PSScriptRoot
$driversDir = Split-Path -Parent $testDir
$driversDir = Join-Path $driversDir "drivers"

if (-not (Test-Path $driversDir)) {
    throw "check_thermal_guard_cfg_producers: $driversDir not found."
}

function Resolve-DriverFile {
    param([string]$DriversDir, [string]$BaseName)
    $found = Get-ChildItem -Path $DriversDir -Filter $BaseName -File -Recurse
    if ($found.Count -eq 0) {
        throw "check_thermal_guard_cfg_producers: expected file '$BaseName' not found anywhere under $DriversDir -- has it moved or been renamed? This check is now blind, which is worse than the bug it looks for."
    }
    if ($found.Count -gt 1) {
        $paths = ($found | ForEach-Object { $_.FullName }) -join ", "
        throw "check_thermal_guard_cfg_producers: '$BaseName' matched more than one file under $DriversDir ($paths) -- cannot tell which one is the real file."
    }
    return $found[0].FullName
}

$guardHeader = Resolve-DriverFile -DriversDir $driversDir -BaseName "thermal_guard.h"

# --- 1. The field list, from thermal_guard_cfg_t in thermal_guard.h ---
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
        if ($line -match 'thermal_guard_cfg_t\s*;') {
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
    throw "check_thermal_guard_cfg_producers: parsed ZERO fields out of thermal_guard_cfg_t in $guardHeader. The parser is broken or the struct moved -- failing loudly rather than reporting a vacuous pass."
}

# Comment stripper -- same technique as the sibling input_t check.
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
# Production spans the whole tree (not just control/ and persist/): the
# fields flow from persist/zones_config_* accessors through http/ JSON
# parse/serialize into control/thermal_guard_cfg_t designated initializers,
# and excluding any of those subtrees would risk a false failure someone
# "fixes" by weakening the check.
$sourceFiles = Get-ChildItem -Path $driversDir -Filter "*.c" -Recurse -File |
    Where-Object { $_.FullName -notmatch '\\test\\' }

if ($sourceFiles.Count -eq 0) {
    throw "check_thermal_guard_cfg_producers: found ZERO production .c files under $driversDir -- wrong directory, or the check would pass vacuously."
}

. (Join-Path $PSScriptRoot "lib_typed_field_producers.ps1")
$scanText = ""
$typedInit = ""
$typedVars = @()
$typedCode = ""
foreach ($f in $sourceFiles) {
    $fileText = ((Get-CodeOnlyLines -Path $f.FullName) -join "`n") + "`n"
    $scanText += $fileText
    # Only files that reference the struct type can produce its fields; a
    # same-named field of an unrelated struct elsewhere must not count.
    if ($fileText -match 'thermal_guard_cfg_t') {
        $t = Get-TypedProducerText -CodeText $fileText -TypeName 'thermal_guard_cfg_t'
        $typedInit += $t.Init
        $typedCode += $fileText
        $typedVars += $t.Vars
    }
}
$typedVars = @($typedVars | Select-Object -Unique)

if ($scanText -notmatch 'thermal_guard_cfg_t') {
    throw "check_thermal_guard_cfg_producers: no production file even references thermal_guard_cfg_t -- this check is looking at the wrong tree and would pass vacuously."
}

$missing = @()
foreach ($field in $fields) {
    $escaped = [regex]::Escape($field)
    $assigned = Test-FieldProducedInText -InitText $typedInit -Vars $typedVars -Field $field -CodeText $typedCode -ChainMembers @()
    if (-not $assigned) {
        $missing += $field
    }
}

if ($missing.Count -gt 0) {
    Write-Host "THERMAL GUARD CFG PRODUCER CHECK FAILED:" -ForegroundColor Red
    foreach ($m in $missing) {
        Write-Host "  thermal_guard_cfg_t.$m is never assigned by any production file in App/drivers" -ForegroundColor Red
    }
    Write-Host ""
    Write-Host "  A guard-config field with no producer is silently 0 on every board --" -ForegroundColor Red
    Write-Host "  effective_f() substitutes the hardcoded firmware default and the field" -ForegroundColor Red
    Write-Host "  can never be tuned, while its host tests keep passing because the test" -ForegroundColor Red
    Write-Host "  supplies the value by hand. See docs/audits/consumer_without_producer_2026-09-06.md" -ForegroundColor Red
    Write-Host "  finding 1 for the instance that motivated this check." -ForegroundColor Red
    throw "$($missing.Count) thermal_guard_cfg_t field(s) have no production producer"
}

Write-Host "Thermal guard cfg producer check passed: all $($fields.Count) thermal_guard_cfg_t fields are assigned somewhere in App/drivers (excluding test/)."

# --- 3. Per-caller check: every file that calls thermal_guard_tick() must
# assign climb_window_floor_s ITSELF, not merely somewhere in the tree. ---
#
# WHY THIS EXTRA PHASE. 2026-09-10 opus review: cf3b5adb added
# thermal_guard_cfg_t.climb_window_floor_s and wired it up in
# profile_executor.c's per-tick guard_cfg_this_tick, which satisfied phase 1
# above (the field DOES have a producer somewhere) -- but autotune_engine.c
# builds its OWN separate per-tick thermal_guard_cfg_t local
# (guard_cfg_this_tick = s_at.guard_cfg, then a couple of per-tick field
# overrides) and never set this field there, so a STEP autotune run on a
# slow zone evaluated guard 1's climbing branch against an unfloored window
# and could false-trip HEATING_FAILED on a healthy zone -- silent, because
# phase 1's "assigned SOMEWHERE" check was already satisfied by the other
# caller. This is exactly this repo's "paired input left shared" bug class
# (docs, project_paired_input_left_shared.md): one field, two independent
# producers, only one kept current. This phase is deliberately narrow (only
# this one field, pinned to the two known call sites of thermal_guard_tick())
# rather than a general "every field at every call site" rule, per this
# check's own file-header precedent for when a narrow check is honest: a
# concrete, stable pair, not a speculative general rule that would also flag
# ordinary one-sided initialization patterns elsewhere in this codebase.
$FIELD_REQUIRING_PER_CALLER_PRODUCER = "climb_window_floor_s"

$tickCallSites = Get-ChildItem -Path $driversDir -Filter "*.c" -Recurse -File |
    Where-Object { $_.FullName -notmatch '\\test\\' } |
    Where-Object { $_.Name -ne 'thermal_guard.c' } | # defines thermal_guard_tick(), does not call it
    Where-Object {
        $codeOnly = (Get-CodeOnlyLines -Path $_.FullName) -join "`n"
        $codeOnly -match 'thermal_guard_tick\s*\('
    }

if ($tickCallSites.Count -lt 2) {
    throw "check_thermal_guard_cfg_producers (phase 3): expected at least 2 production callers of thermal_guard_tick() (profile_executor.c and autotune_engine.c) but found $($tickCallSites.Count) -- has a caller moved, been renamed, or been removed? This check is now blind to the case it exists for."
}

$missingPerCaller = @()
foreach ($f in $tickCallSites) {
    $codeOnly = (Get-CodeOnlyLines -Path $f.FullName) -join "`n"
    $escaped = [regex]::Escape($FIELD_REQUIRING_PER_CALLER_PRODUCER)
    $assignedHere = ($codeOnly -match "\.\s*$escaped\s*=") -or ($codeOnly -match "->\s*$escaped\s*=")
    if (-not $assignedHere) {
        $missingPerCaller += $f.FullName
    }
}

if ($missingPerCaller.Count -gt 0) {
    Write-Host "THERMAL GUARD CFG PER-CALLER PRODUCER CHECK FAILED:" -ForegroundColor Red
    foreach ($m in $missingPerCaller) {
        Write-Host "  $m calls thermal_guard_tick() but never assigns .$FIELD_REQUIRING_PER_CALLER_PRODUCER itself" -ForegroundColor Red
    }
    Write-Host ""
    Write-Host "  Every caller of thermal_guard_tick() builds its OWN per-tick thermal_guard_cfg_t" -ForegroundColor Red
    Write-Host "  copy -- a field set by only SOME callers silently reverts to whatever that" -ForegroundColor Red
    Write-Host "  caller's base config held (often 0, i.e. 'no floor') for every other caller," -ForegroundColor Red
    Write-Host "  with phase 1 above unable to see it because the field DOES have a producer" -ForegroundColor Red
    Write-Host "  elsewhere. See the 2026-09-10 opus review finding B this phase closes." -ForegroundColor Red
    throw "$($missingPerCaller.Count) caller(s) of thermal_guard_tick() never assign .$FIELD_REQUIRING_PER_CALLER_PRODUCER"
}

Write-Host "Thermal guard cfg per-caller producer check passed: all $($tickCallSites.Count) callers of thermal_guard_tick() assign .$FIELD_REQUIRING_PER_CALLER_PRODUCER themselves."
exit 0
