# check_volatile_ceiling_write_callers.ps1 -- safety_cfg_write_set_and_confirm_f32_volatile()
# may be called ONLY from the call sites on the allowlist below.
#
# WHY THIS EXISTS. safety_cfg_write.h's own doc comment (the paragraph above
# the declaration) states the rule in prose: this is the RAM-only install
# sibling, it exists ONLY for kiln_cfg_swap.c's "raise-first" step, and it is
# "NEVER for the standing safety_ceiling_sync.c reconcile loop, which keeps
# calling the FLASH-writing sibling above unchanged." Nothing enforced that.
# A 2026-09-16 review found the rule documented in a header comment and
# checked by nothing at all -- `git grep -rln set_and_confirm_f32_volatile --
# tools` came back empty.
#
# WHY IT MATTERS (the failure this prevents). The volatile variant differs
# from the flash-writing sibling in two ways that are individually convenient
# and jointly dangerous:
#
#   1. It can never be refused for ARMED -- config_store_write_volatile() on
#      the Pico never calls config_store_decide_write(), so the ordinary
#      "config writes are refused while the relay is ARMED" rule does not
#      apply to it.
#   2. It never reaches flash, so the value it installs does NOT survive a
#      Pico reboot.
#
# The standing reconcile loop in safety_ceiling_sync.c writes the Pico's
# abs_max_temp_c -- the independent over-temperature ceiling whose whole
# purpose is to be a second set of eyes on the ESP. That loop legitimately
# hits ARMED refusals, because the Pico's ordinary running state IS ARMED.
# The tempting "fix" is to swap in the volatile variant, because it "just
# works" -- no refusal, the read-back confirms, the divergence alarm clears.
# What actually happens is that the ceiling then lives only in the Pico's
# RAM: the next Pico reboot silently restores the STALE flash ceiling, and
# nothing reports it until the next reconcile tick notices the divergence.
# Between those two moments the kiln runs with a ceiling nobody chose. That
# is precisely the standing invariant "the Pico's abs_max_temp_c must ALWAYS
# equal the ESP's" failing silently, which is the one failure mode the
# divergence machinery exists to make impossible.
#
# kiln_cfg_swap.c's two call sites are different, and are allowlisted, for
# the reason its own module comment gives: that module owns a two-processor
# transaction whose owner rule is "the Pico never leaves ARMED, ever," it
# drives the ceiling raise-first/lower-last itself, and it has a best-effort
# flash-fallback step after the whole swap verifies. It is the ONE caller the
# header licenses.
#
# WHAT THIS CHECKS. Every call to safety_cfg_write_set_and_confirm_f32_volatile(
# anywhere under firmware/KilnFW/App is a failure UNLESS the file is on the
# allowlist below with a stated reason. The DEFINITION (safety_cfg_write.c)
# and the DECLARATION (safety_cfg_write.h) are allowlisted as such; they are
# not callers. App/test/ is excluded from the scan entirely -- a host-test
# double DEFINING a stand-in for this function is not a production code path,
# matching how tools/check_relay_writes_through_owner.ps1 treats test doubles.
#
# Comments are stripped first (same Get-CodeOnlyLines helper as
# tools/check_relay_writes_through_owner.ps1) so a comment that merely
# DISCUSSES this function -- and several in this codebase do, at length,
# including the header rule this check enforces -- is not counted as a call.
#
# Usage: powershell -File tools\check_volatile_ceiling_write_callers.ps1
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$appDir = Join-Path $root "firmware\KilnFW\App"
if (-not (Test-Path $appDir)) {
    throw "check_volatile_ceiling_write_callers: $appDir not found -- has it moved? This check is now blind."
}
$appDirResolved = (Resolve-Path $appDir).Path

# Comment stripper -- duplicated from tools/check_relay_writes_through_owner.ps1
# (this project has no shared PowerShell module mechanism).
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

$fn = "safety_cfg_write_set_and_confirm_f32_volatile"

# --- Allowlist: {RelPath; Reason}. A file, not a line number -- line numbers
# in this repo move constantly and a check that goes stale on an unrelated
# edit gets disabled rather than fixed. MinCalls/MaxCalls bound how many
# calls that file may make, so the licence cannot quietly widen from the two
# documented raise-first/lower-last sites into a loop. ---
$allowlist = @(
    @{
        RelPath  = "firmware/KilnFW/App/drivers/persist/kiln_cfg_swap.c"
        MinCalls = 2
        MaxCalls = 2
        Reason   = "the ONE caller safety_cfg_write.h licenses: the two-processor apply transaction's raise-first (step 4) and lower-last ceiling drives, whose owner rule is 'the Pico never leaves ARMED, ever' and which has its own best-effort flash-fallback step after the swap verifies."
    },
    @{
        RelPath  = "firmware/KilnFW/App/drivers/safety/safety_cfg_write.c"
        MinCalls = 1
        MaxCalls = 1
        Reason   = "the DEFINITION site of the function itself -- not a caller."
    }
)

function Get-AllowEntry {
    param([string]$RelPath)
    foreach ($entry in $allowlist) {
        if ($entry.RelPath -ieq $RelPath) { return $entry }
    }
    return $null
}

$sourceFiles = Get-ChildItem -Path $appDirResolved -Recurse -File -Include "*.c", "*.h" |
    Where-Object { $_.FullName -notmatch '[\\/]test[\\/]' -and $_.FullName -notmatch '[\\/]build[\\/]' }

if ($sourceFiles.Count -lt 50) {
    throw "check_volatile_ceiling_write_callers: only $($sourceFiles.Count) source file(s) found under $appDirResolved (excluding test/) -- implausibly low, has the tree moved? This check would pass vacuously."
}

$violations = @()
$seenCounts = @{}

foreach ($f in $sourceFiles) {
    $codeLines = Get-CodeOnlyLines -Path $f.FullName
    $rel = "firmware/KilnFW/App/" + ($f.FullName.Substring($appDirResolved.Length + 1) -replace '\\', '/')
    $entry = Get-AllowEntry -RelPath $rel

    for ($i = 0; $i -lt $codeLines.Count; $i++) {
        if ($codeLines[$i] -match "\b$fn\s*\(") {
            # The declaration in the header is not a call; count it so the
            # blindness guard below can see the symbol still exists, but never
            # flag it.
            if ($rel -eq "firmware/KilnFW/App/drivers/safety/safety_cfg_write.h") {
                $seenCounts[$rel] = [int]$seenCounts[$rel] + 1
                continue
            }
            if ($entry) {
                $seenCounts[$rel] = [int]$seenCounts[$rel] + 1
                continue
            }
            $violations += "${rel}:$($i + 1): call to $fn() outside the allowlist"
        }
    }
}

# --- Blindness guards. If the function were renamed, deleted, or moved out
# from under this scan, every loop above would find nothing and this check
# would report a cheerful PASS while enforcing nothing. Both of the following
# must hold. ---
$declSeen = [int]$seenCounts["firmware/KilnFW/App/drivers/safety/safety_cfg_write.h"]
if ($declSeen -lt 1) {
    throw "check_volatile_ceiling_write_callers: $fn() was not found in safety_cfg_write.h at all -- renamed, moved, or deleted. This check has gone blind; update it before trusting this result."
}

foreach ($entry in $allowlist) {
    $n = [int]$seenCounts[$entry.RelPath]
    if ($n -lt $entry.MinCalls) {
        throw "check_volatile_ceiling_write_callers: expected at least $($entry.MinCalls) occurrence(s) of $fn() in $($entry.RelPath) but found $n -- the allowlist no longer matches reality, so this check is enforcing something other than what it claims. Update the allowlist deliberately."
    }
    if ($n -gt $entry.MaxCalls) {
        $violations += "$($entry.RelPath): $n occurrence(s) of $fn(), allowlist permits at most $($entry.MaxCalls) -- a licensed file grew an EXTRA call site. The licence covers the two documented raise-first/lower-last drives, not a third caller and not a loop."
    }
}

if ($violations.Count -gt 0) {
    Write-Host "VOLATILE CEILING WRITE CALLER CHECK FAILED:" -ForegroundColor Red
    foreach ($v in $violations) {
        Write-Host "  $v" -ForegroundColor Red
    }
    Write-Host ""
    Write-Host "  $fn() installs the Pico's" -ForegroundColor Red
    Write-Host "  abs_max_temp_c in RAM ONLY. It is never ARMED-refused and never reaches" -ForegroundColor Red
    Write-Host "  flash, so the next Pico reboot silently restores the STALE ceiling with no" -ForegroundColor Red
    Write-Host "  alarm until the next reconcile tick -- breaking the standing invariant that" -ForegroundColor Red
    Write-Host "  the Pico's abs_max_temp_c ALWAYS equals the ESP's. If you reached for this" -ForegroundColor Red
    Write-Host "  because the standing safety_ceiling_sync.c reconcile loop hit an ARMED" -ForegroundColor Red
    Write-Host "  refusal, that refusal is the design working: surface it to the operator." -ForegroundColor Red
    Write-Host "  Use safety_cfg_write_set_and_confirm_f32() -- see safety_cfg_write.h's" -ForegroundColor Red
    Write-Host "  doc comment above the declaration." -ForegroundColor Red
    throw "$($violations.Count) unlicensed call site(s) / count violation(s) for $fn()"
}

$total = ($seenCounts.Values | Measure-Object -Sum).Sum
Write-Host "Volatile ceiling write caller check passed: $fn() appears only at its declaration, its definition, and $($allowlist.Count - 1) allowlisted caller file ($total occurrence(s) total, $($sourceFiles.Count) source files scanned)."
exit 0
