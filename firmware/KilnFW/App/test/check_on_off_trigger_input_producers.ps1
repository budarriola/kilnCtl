# check_on_off_trigger_input_producers.ps1 -- every field of
# on_off_trigger_input_t (control/on_off_trigger_decide.h) must be assigned
# by some PRODUCTION (non-test) source file in App/drivers, not only by a
# test.
#
# WHY THIS EXISTS. Same rationale as check_thermal_guard_cfg_producers.ps1
# and check_thermal_guard_input_producers.ps1 (docs/audits/consumer_without_
# producer_2026-09-06.md): a struct field read by production code but never
# assigned by any production code is silently 0/false on the real board
# forever, while its host tests keep passing because the test supplies the
# value by hand -- see project_consumer_without_producer_class.md. This
# module's input struct is new (docs/ON_OFF_ZONE_PLAN.md sec 3/4/7's trigger
# evaluation core) and, unlike thermal_guard_input_t, several of its fields
# (rule.*) currently have only a CONSTANT production producer in
# profile_executor.c (rule.enable = false, etc.) because profile_on_off_
# rule_t storage (plan step 5) has not landed yet -- that is still a real
# assignment site by this check's own stated limit ("only that at least one
# production file assigns the field somewhere", never that the assignment is
# meaningful or varies), and is intentional: see profile_executor.c's own
# comment at the on_off_trigger_input_t construction site.
#
# It cannot prove a producer is CORRECT, or that the value assigned is ever
# anything but a placeholder -- only that at least one production file
# assigns the field somewhere. Same limit the sibling checks accept.
#
# Usage: powershell -ExecutionPolicy Bypass -File check_on_off_trigger_input_producers.ps1
$ErrorActionPreference = "Stop"

$testDir = $PSScriptRoot
$driversDir = Split-Path -Parent $testDir
$driversDir = Join-Path $driversDir "drivers"

if (-not (Test-Path $driversDir)) {
    throw "check_on_off_trigger_input_producers: $driversDir not found."
}

function Resolve-DriverFile {
    param([string]$DriversDir, [string]$BaseName)
    $found = Get-ChildItem -Path $DriversDir -Filter $BaseName -File -Recurse
    if ($found.Count -eq 0) {
        throw "check_on_off_trigger_input_producers: expected file '$BaseName' not found anywhere under $DriversDir -- has it moved or been renamed? This check is now blind, which is worse than the bug it looks for."
    }
    if ($found.Count -gt 1) {
        $paths = ($found | ForEach-Object { $_.FullName }) -join ", "
        throw "check_on_off_trigger_input_producers: '$BaseName' matched more than one file under $DriversDir ($paths) -- cannot tell which one is the real file."
    }
    return $found[0].FullName
}

$header = Resolve-DriverFile -DriversDir $driversDir -BaseName "on_off_trigger_decide.h"

# --- 1. The field lists, from on_off_trigger_input_t AND on_off_trigger_rule_t
# (the rule struct is embedded by value as on_off_trigger_input_t.rule, and
# its members are what profile_executor.c actually assigns via `.rule = {
# .enable = ..., ... }`, not a field literally named "rule.enable") ---
$headerLines = Get-Content -Path $header

function Get-StructFields {
    param([string[]]$Lines, [string]$StructTypedefSuffix)
    $inStruct = $false
    $depth = 0
    $fields = @()
    foreach ($line in $Lines) {
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
            if ($line -match [regex]::Escape($StructTypedefSuffix)) {
                return $fields
            }
            $fields = @()
            continue
        }
        $code = $line
        if ($code -match '^\s*(\*|/\*|//)') { continue }
        if ($code -match '^\s*(const\s+)?[A-Za-z_][A-Za-z0-9_ ]*\s+\**([A-Za-z_][A-Za-z0-9_]*)\s*(\[[^\]]*\])?\s*;') {
            $fields += $Matches[2]
        }
    }
    return $fields
}

$inputFields = Get-StructFields -Lines $headerLines -StructTypedefSuffix "on_off_trigger_input_t;"
$ruleFields = Get-StructFields -Lines $headerLines -StructTypedefSuffix "on_off_trigger_rule_t;"

if ($inputFields.Count -eq 0) {
    throw "check_on_off_trigger_input_producers: parsed ZERO fields out of on_off_trigger_input_t in $header. The parser is broken or the struct moved -- failing loudly rather than reporting a vacuous pass."
}
if ($ruleFields.Count -eq 0) {
    throw "check_on_off_trigger_input_producers: parsed ZERO fields out of on_off_trigger_rule_t in $header. The parser is broken or the struct moved -- failing loudly rather than reporting a vacuous pass."
}

# The embedded struct field itself ("rule") is not separately assigned by
# name (its members are, via the nested designated initializer) -- drop it
# from the input-struct list and check its members via $ruleFields instead.
$fields = @($inputFields | Where-Object { $_ -ne "rule" }) + $ruleFields
$fields = $fields | Select-Object -Unique

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

# --- 2. What every PRODUCTION (non-test) .c file in App/drivers assigns ---
$sourceFiles = Get-ChildItem -Path $driversDir -Filter "*.c" -Recurse -File |
    Where-Object { $_.FullName -notmatch '\\test\\' }

if ($sourceFiles.Count -eq 0) {
    throw "check_on_off_trigger_input_producers: found ZERO production .c files under $driversDir -- wrong directory, or the check would pass vacuously."
}

$scanText = ""
foreach ($f in $sourceFiles) {
    $scanText += ((Get-CodeOnlyLines -Path $f.FullName) -join "`n") + "`n"
}

if ($scanText -notmatch 'on_off_trigger_input_t') {
    throw "check_on_off_trigger_input_producers: no production file even references on_off_trigger_input_t -- this check is looking at the wrong tree and would pass vacuously."
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
    Write-Host "ON_OFF_TRIGGER_INPUT PRODUCER CHECK FAILED:" -ForegroundColor Red
    foreach ($m in $missing) {
        Write-Host "  on_off_trigger_input_t (or its embedded rule).$m is never assigned by any production file in App/drivers" -ForegroundColor Red
    }
    Write-Host ""
    Write-Host "  A trigger-input field with no producer is silently 0/false on every board" -ForegroundColor Red
    Write-Host "  forever, while its host tests keep passing because the test supplies the" -ForegroundColor Red
    Write-Host "  value by hand. See docs/audits/consumer_without_producer_2026-09-06.md" -ForegroundColor Red
    Write-Host "  and docs/ON_OFF_ZONE_PLAN.md sec 3/4/7." -ForegroundColor Red
    throw "$($missing.Count) on_off_trigger_input_t/rule field(s) have no production producer"
}

Write-Host "on_off_trigger_input producer check passed: all $($fields.Count) fields (input struct + embedded rule) are assigned somewhere in App/drivers (excluding test/)."
exit 0
