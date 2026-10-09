# check_profile_executor_wd_input_producers.ps1 -- every field of
# profile_executor_wd_input_t must actually be assigned by SOME production
# (non-test) source file in App/drivers, not only by a test.
#
# WHY THIS EXISTS. profile_executor_wd_input_t (profile_executor.h) is the
# pure-classifier input struct profile_executor_wd_decide() consumes each
# tick -- the per-tick analog of thermal_guard_input_t, extracted so the
# watchdog decision could be host-tested without pulling in FreeRTOS/kiln_io/
# relay_authority. It is exactly the "consumer without producer" shape those
# checks were written for (see check_thermal_guard_input_producers.ps1 and
# check_thermal_guard_cfg_producers.ps1's header comments for the fuller
# rationale): a struct with fields a decision function reads, where a new
# field silently defaulting to its zero-initialized value (memset(&wd_in, 0,
# sizeof(wd_in)) in profile_executor.c) because nobody wired up the real
# assignment reads as "nothing wrong" -- e.g. a false tick_stale or a false
# safety_processor_tripped just means the watchdog never fires on that
# condition, with no compiler error and no obviously wrong number anywhere.
#
# It cannot prove a producer is CORRECT, or that the value it assigns is the
# real live state rather than another hardcoded placeholder -- only that at
# least one production file assigns the field somewhere. Same limit the
# sibling checks accept.
#
# WHAT WOULD INVALIDATE THIS CHECK: it is keyed on the struct name
# profile_executor_wd_input_t wherever it is declared under App/drivers, not
# on a filename or path -- profile_executor.h could be split (as several
# oversized KilnFW files have been) without breaking the parser, since it
# searches every header under drivers/ for the struct, not one specific file.
# It WOULD go blind if profile_executor_wd_input_t were renamed without
# renaming the field-list search below, or if the single call site building
# it (profile_executor.c, wd_in.<field> = ...) were rewritten to populate the
# struct via a helper that returns it by value with designated initializers
# instead of field-by-field assignment -- the `\.\s*FIELD\s*=` grep would
# then need push-current logic. It also cannot see a field wired to the
# WRONG source (e.g. tick_stale assigned from an unrelated bool) -- it only
# proves a field is written to by name somewhere in production code.
#
# Usage: powershell -ExecutionPolicy Bypass -File check_profile_executor_wd_input_producers.ps1
$ErrorActionPreference = "Stop"

$testDir = $PSScriptRoot
$driversDir = Split-Path -Parent $testDir
$driversDir = Join-Path $driversDir "drivers"

if (-not (Test-Path $driversDir)) {
    throw "check_profile_executor_wd_input_producers: $driversDir not found."
}

# --- 0. Find the header declaring profile_executor_wd_input_t, by SYMBOL,
# not by an assumed filename -- see header comment on invalidation. ---
$headerCandidates = Get-ChildItem -Path $driversDir -Filter "*.h" -File -Recurse |
    Where-Object { (Select-String -Path $_.FullName -Pattern 'profile_executor_wd_input_t' -SimpleMatch -Quiet) }

if ($headerCandidates.Count -eq 0) {
    throw "check_profile_executor_wd_input_producers: no header under $driversDir mentions profile_executor_wd_input_t -- has the struct been renamed? This check is now blind, which is worse than the bug it looks for."
}
if ($headerCandidates.Count -gt 1) {
    $paths = ($headerCandidates | ForEach-Object { $_.FullName }) -join ", "
    throw "check_profile_executor_wd_input_producers: profile_executor_wd_input_t is mentioned in more than one header ($paths) -- cannot tell which declares it."
}
$guardHeader = $headerCandidates[0].FullName

# --- 1. The field list, from profile_executor_wd_input_t's typedef ---
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
        if ($line -match 'profile_executor_wd_input_t\s*;') {
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
    throw "check_profile_executor_wd_input_producers: parsed ZERO fields out of profile_executor_wd_input_t in $guardHeader. The parser is broken or the struct moved -- failing loudly rather than reporting a vacuous pass."
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

# --- 2. What every PRODUCTION (non-test) .c file in App/drivers assigns ---
# Production spans the whole tree, not just control/: wd_in.<field> is set in
# profile_executor.c from values gathered earlier in the same tick (some
# originating in safety/safety_link.c), and excluding any subtree would risk
# a false failure someone "fixes" by weakening the check.
$sourceFiles = Get-ChildItem -Path $driversDir -Filter "*.c" -Recurse -File |
    Where-Object { $_.FullName -notmatch '\\test\\' }

if ($sourceFiles.Count -eq 0) {
    throw "check_profile_executor_wd_input_producers: found ZERO production .c files under $driversDir -- wrong directory, or the check would pass vacuously."
}

$scanText = ""
foreach ($f in $sourceFiles) {
    $scanText += ((Get-CodeOnlyLines -Path $f.FullName) -join "`n") + "`n"
}

if ($scanText -notmatch 'profile_executor_wd_input_t') {
    throw "check_profile_executor_wd_input_producers: no production file even references profile_executor_wd_input_t -- this check is looking at the wrong tree and would pass vacuously."
}

$missing = @()
foreach ($field in $fields) {
    $escaped = [regex]::Escape($field)
    $assigned = ($scanText -match "\.\s*$escaped\s*=") -or
                ($scanText -match "->\s*$escaped\s*=") -or
                ($scanText -match "\[\s*$escaped\s*\]") -or
                ($scanText -match "\.\s*$escaped\s*\[") -or
                ($scanText -match "->\s*$escaped\s*\[")
    if (-not $assigned) {
        $missing += $field
    }
}

if ($missing.Count -gt 0) {
    Write-Host "PROFILE EXECUTOR WD INPUT PRODUCER CHECK FAILED:" -ForegroundColor Red
    foreach ($m in $missing) {
        Write-Host "  profile_executor_wd_input_t.$m is never assigned by any production file in App/drivers" -ForegroundColor Red
    }
    Write-Host ""
    Write-Host "  A watchdog-input field with no producer is silently left at the" -ForegroundColor Red
    Write-Host "  memset(&wd_in, 0, sizeof(wd_in)) zero value on every tick --" -ForegroundColor Red
    Write-Host "  profile_executor_wd_decide() sees a permanently-false/zero condition" -ForegroundColor Red
    Write-Host "  and the corresponding fault path can never trigger, while host tests" -ForegroundColor Red
    Write-Host "  keep passing because the test supplies wd_in by hand." -ForegroundColor Red
    throw "$($missing.Count) profile_executor_wd_input_t field(s) have no production producer"
}

Write-Host "Profile executor wd input producer check passed: all $($fields.Count) profile_executor_wd_input_t fields are assigned somewhere in App/drivers (excluding test/)."
exit 0
