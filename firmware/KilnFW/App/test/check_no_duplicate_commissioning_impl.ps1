# check_no_duplicate_commissioning_impl.ps1 -- guards against the exact
# drift a 2026-09-08 review (5d03f8c2) found: commissioning_shared.js
# extracted the safety-processor commissioning "confirm a critical change,
# refuse while busy, POST, distinguish a REJECTED commit from a real
# success, then do THIS PAGE'S OWN independent read-back" contract, but for
# a while safety_commissioning_page.html was deliberately left with its own
# second copy of the same three functions (findCriticalChanges(),
# checkFiringOrAutotuneRunning(), and the commit-and-read-back logic
# itself). Two implementations of one safety-critical contract, silently
# divergeable -- exactly this codebase's most-repeated defect class (trip
# words, zones field tables, MCP tool counts all drifted the same way).
#
# Keyed on SYMBOLS, not file paths -- per the task's own instruction, since
# a file split has broken every path-keyed check in this repo at least once
# (project_splits_break_path_keyed_checks). Three string literals are each
# unique to the REAL implementation of one of the three functions (a page
# that merely calls window.kcCommissioningCheckBusy()/etc and displays the
# result never needs to contain these strings itself):
#
#   BUSY_MARKER    -- checkBusy()'s own busy-reason text (a page that only
#                     calls the shared function and prints its return value
#                     never needs to spell out the reason text itself).
#   READBACK_OK    -- commitAndVerify()'s named, per-field success message
#                     (only the function that actually re-fetches and
#                     compares needs to build this exact string).
#   READBACK_FAIL  -- commitAndVerify()'s mismatch message (same reasoning).
#
# Each marker is expected in EXACTLY ONE served page under
# firmware/KilnFW/App/drivers/http (commissioning_shared.js itself). If a
# second implementation reappears -- in a third page, or hand-copied back
# into safety_commissioning_page.html/setup_wizard_page.html -- one of these
# three markers will appear in a second file and this check goes RED, naming
# the offending file and marker.
#
# Negative-tested by hand (see the report for this task): a duplicate
# READBACK_OK string was pasted into a second file, this check turned RED
# naming both files, the duplicate was removed, and `git diff` was confirmed
# empty before this pass committed.
#
# Usage: powershell -File firmware\KilnFW\App\test\check_no_duplicate_commissioning_impl.ps1
$ErrorActionPreference = "Stop"

$testDir = $PSScriptRoot
# test/ lives at firmware/KilnFW/App/test -- drivers/ is a sibling under App/.
$driversDir = (Resolve-Path (Join-Path $testDir "..\drivers")).Path

$servedFiles = Get-ChildItem -Path $driversDir -Recurse -Include "*.html", "*.js" -File

$markers = [ordered]@{
    "BUSY_MARKER (checkBusy() reason text)"          = "a profile is currently firing (state: running)"
    "READBACK_OK (commitAndVerify() named success)"  = "Committed and confirmed by read-back: "
    "READBACK_FAIL (commitAndVerify() mismatch text)" = "does NOT match what was just written"
}

$failures = @()

foreach ($markerName in $markers.Keys) {
    $needle = $markers[$markerName]
    $hits = @()
    foreach ($f in $servedFiles) {
        $content = Get-Content -Path $f.FullName -Raw
        if ($content.Contains($needle)) {
            $hits += $f.FullName
        }
    }
    if ($hits.Count -eq 0) {
        $failures += "$markerName -- NOT FOUND in any served page under $driversDir. " +
            "This marker text moved or was deleted; update this check's marker string " +
            "rather than letting it pass vacuously with zero coverage."
    } elseif ($hits.Count -gt 1) {
        $failures += "$markerName -- found in $($hits.Count) files (expected exactly 1): " +
            ($hits -join ", ") +
            ". A second confirm-and-read-back implementation has reappeared -- retrofit the " +
            "extra file onto commissioning_shared.js instead of hand-copying this logic."
    } else {
        Write-Host "  OK    $markerName -- only in $($hits[0])" -ForegroundColor Green
    }
}

if ($failures.Count -gt 0) {
    Write-Host ""
    Write-Host "NO-DUPLICATE-COMMISSIONING-IMPL CHECK FAILED:" -ForegroundColor Red
    foreach ($f in $failures) {
        Write-Host "  - $f" -ForegroundColor Red
    }
    exit 1
}

Write-Host ""
Write-Host "no-duplicate-commissioning-impl check passed: each marker lives in exactly one served page."
exit 0
