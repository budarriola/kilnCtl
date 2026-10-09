# test_check_route_tier_coverage.ps1 -- negative test for
# tools/check_route_tier_coverage.ps1 (docs/WEB_AUTH_PLAN.md section 1).
#
# Proves Invoke-RouteTierCoverageScan (the PRODUCTION function, the same one
# check_route_tier_coverage.ps1's main body calls) can actually detect a
# route registered with no tier -- fail-closed: unclassified must fail the
# check, never quietly pass as OPEN. Same dot-source pattern as
# test_check_hal_include_boundary.ps1: dot-source the check (which, per its
# own guard, only defines functions when dot-sourced) and call its exported
# function directly against synthetic scratch files.
#
#   1. A synthetic drivers tree with one route file (all tiered) and a
#      matching table -> zero missing.
#   2. The same drivers tree with ONE MORE route added, with no matching
#      ROUTE_TIER row -> exactly one missing, naming that route.
#   3. Delete the blindness floor's precondition (too few routes / too few
#      table rows) -> the scan itself still throws rather than passing
#      vacuously.
#   4. The REAL production route table and REAL production drivers tree,
#      dot-sourced and scanned directly (not a copy) -> zero missing, proving
#      today's real check is not vacuous on the actual tree.
#
# This does not touch the real repo tree; steps 1-3 are entirely synthetic
# scratch files, and step 4 only READS the real tree (Invoke-RouteTierCoverageScan
# never writes).
#
# Usage: powershell -ExecutionPolicy Bypass -File App\test\test_check_route_tier_coverage.ps1

$ErrorActionPreference = "Stop"

$testDir = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $testDir "..\..\..\..")).Path
$checkScript = Join-Path $repoRoot "tools\check_route_tier_coverage.ps1"

if (-not (Test-Path $checkScript)) {
    throw "test_check_route_tier_coverage: expected $checkScript not found -- has it moved?"
}

# Dot-source: per check_route_tier_coverage.ps1's own guard, this defines
# Invoke-RouteTierCoverageScan/Get-RegisteredRoutes/Get-TieredKeys and runs
# nothing else (no $DriversDir/$TableFile were bound, and $MyInvocation
# .InvocationName is '.' during dot-sourcing).
. $checkScript

$scratchDir = Join-Path $env:TEMP "route_tier_test_$PID"
if (-not (Test-Path $scratchDir)) {
    New-Item -ItemType Directory -Force -Path $scratchDir | Out-Null
}

$failures = @()

try {

# --- Assertion 1: a small synthetic tree, fully tiered -> 0 missing. ---
$synthDriversDir = Join-Path $scratchDir "drivers_ok"
New-Item -ItemType Directory -Force -Path $synthDriversDir | Out-Null
# Need >=5 .c files for the "only N .c files found" precondition and enough
# routes for the blindness floor -- pad with filler files carrying no routes
# at all (they must not count toward the route total).
1..4 | ForEach-Object {
    Set-Content -Path (Join-Path $synthDriversDir "filler_$_.c") -Value "/* no routes here */`nint filler_$_(void) { return $_; }`n" -Encoding utf8
}
$routeFileContent = @'
static const httpd_uri_t r1 = { .uri = "/a", .method = HTTP_GET, .handler = h1 };
static const httpd_uri_t r2 = { .uri = "/b", .method = HTTP_POST, .handler = h2 };
'@
Set-Content -Path (Join-Path $synthDriversDir "routes.c") -Value $routeFileContent -Encoding utf8

$tableOkContent = @'
#define ROUTE_TIER(uri, method, tier) { (uri), (method), (tier) }
static const route_tier_entry_t kRouteTierTable[] = {
    ROUTE_TIER("/a", HTTP_GET, ROUTE_TIER_OPEN),
    ROUTE_TIER("/b", HTTP_POST, ROUTE_TIER_ADMIN),
};
'@
$tableOkFile = Join-Path $scratchDir "table_ok.h"
Set-Content -Path $tableOkFile -Value $tableOkContent -Encoding utf8

$resultOk = Invoke-RouteTierCoverageScan -DriversDir $synthDriversDir -TableFile $tableOkFile -BlindnessFloor 2
if ($resultOk.Missing.Count -ne 0) {
    $missingDesc = ($resultOk.Missing | ForEach-Object { "$($_.Method) $($_.Uri)" }) -join ", "
    $failures += "Assertion 1 FAILED: fully-tiered synthetic tree scored $($resultOk.Missing.Count) missing route(s), expected 0: $missingDesc"
} else {
    Write-Host "Assertion 1 OK: fully-tiered synthetic tree scored 0 missing routes ($($resultOk.TotalRoutes) routes, $($resultOk.TotalTiered) tiered)."
}

# --- Assertion 2: add ONE untiered route -> exactly 1 missing, naming it. ---
$synthDriversDirBad = Join-Path $scratchDir "drivers_bad"
Copy-Item -Path $synthDriversDir -Destination $synthDriversDirBad -Recurse -Force
$badRouteContent = $routeFileContent + "`nstatic const httpd_uri_t r3 = { .uri = `"/c/new_admin_only_route`", .method = HTTP_POST, .handler = h3 };`n"
Set-Content -Path (Join-Path $synthDriversDirBad "routes.c") -Value $badRouteContent -Encoding utf8

$resultBad = Invoke-RouteTierCoverageScan -DriversDir $synthDriversDirBad -TableFile $tableOkFile -BlindnessFloor 2
if ($resultBad.Missing.Count -ne 1) {
    $failures += "Assertion 2 FAILED: expected exactly 1 missing route after adding an untiered one, got $($resultBad.Missing.Count)."
} elseif ($resultBad.Missing[0].Uri -ne "/c/new_admin_only_route" -or $resultBad.Missing[0].Method -ne "HTTP_POST") {
    $failures += "Assertion 2 FAILED: the single missing route did not name the injected route: $($resultBad.Missing[0].Method) $($resultBad.Missing[0].Uri)"
} else {
    Write-Host "Assertion 2 OK: untiered route detected and named -- $($resultBad.Missing[0].Method) $($resultBad.Missing[0].Uri)"
}

# --- Assertion 2b: a table row with no registered route -> Stale names it. ---
$tableStaleFile = Join-Path $scratchDir "table_stale.h"
Set-Content -Path $tableStaleFile -Value ($tableOkContent.Replace("};", "    ROUTE_TIER(`"/gone`", HTTP_GET, ROUTE_TIER_ADMIN),`n};")) -Encoding utf8
$resultStale = Invoke-RouteTierCoverageScan -DriversDir $synthDriversDir -TableFile $tableStaleFile -BlindnessFloor 2
if ($resultStale.Stale.Count -ne 1 -or $resultStale.Stale[0] -ne "HTTP_GET /gone" -or $resultOk.Stale.Count -ne 0) {
    $failures += "Assertion 2b FAILED: stale row not detected exactly (stale=$($resultStale.Stale -join ',') okStale=$($resultOk.Stale.Count))."
} else {
    Write-Host "Assertion 2b OK: stale table row detected and named."
}

# --- Assertion 2c (DEV_TOOLS_REVIEW_2026-10-09 LOW-4): a route whose method cannot be parsed is Missing
# ("<unparsed>") but its table row must NOT also be reported stale. ---
$synthDriversDirNull = Join-Path $scratchDir "drivers_null"
Copy-Item -Path $synthDriversDir -Destination $synthDriversDirNull -Recurse -Force
Set-Content -Path (Join-Path $synthDriversDirNull "routes.c") -Value ($routeFileContent + "`nstatic const httpd_uri_t r3 = { .uri = `"/z`", .handler = h3 };`n") -Encoding utf8
$tableNullFile = Join-Path $scratchDir "table_null.h"
Set-Content -Path $tableNullFile -Value ($tableOkContent.Replace("};", "    ROUTE_TIER(`"/z`", HTTP_GET, ROUTE_TIER_ADMIN),`n};")) -Encoding utf8
$resultNull = Invoke-RouteTierCoverageScan -DriversDir $synthDriversDirNull -TableFile $tableNullFile -BlindnessFloor 2
if ($resultNull.Missing.Count -lt 1 -or $resultNull.Missing[0].Method -ne "<unparsed>") {
    $failures += "Assertion 2c FAILED: unparsed-method route was not reported Missing as <unparsed>."
} elseif ($resultNull.Stale.Count -ne 0) {
    $failures += "Assertion 2c FAILED: unparsed-method route made its table row stale: $($resultNull.Stale -join ',')"
} else {
    Write-Host "Assertion 2c OK: unparsed-method route is Missing and does not make its row stale."
}

# --- Assertion 3: blindness floor still trips on an implausibly small tree,
# proving the guard itself has teeth (this is the same class of gap the
# HAL boundary test's assertion 3/3b covers for its own ratchet function). ---
$floorTripped = $false
try {
    Invoke-RouteTierCoverageScan -DriversDir $synthDriversDir -TableFile $tableOkFile -BlindnessFloor 999
} catch {
    $floorTripped = $true
}
if (-not $floorTripped) {
    $failures += "Assertion 3 FAILED: BlindnessFloor=999 against a 2-route synthetic tree should have thrown, but did not."
} else {
    Write-Host "Assertion 3 OK: blindness floor throws when the counted route total falls below it."
}

# --- Assertion 3b (2026-09-17 audit finding 7): a table with the SAME
# (method, uri) key listed twice must make Get-TieredKeys throw, naming the
# duplicated key -- never silently pick a winner (last-wins was the actual
# pre-fix behaviour: a plain hashtable assignment overwrites the earlier
# entry with no error). Uses two DIFFERENT tiers for the duplicate rows on
# purpose: a same-tier duplicate would pass unnoticed either way, and the
# real defect this guards against is exactly the case where the duplicate
# rows disagree. ---
$dupTableContent = @'
#define ROUTE_TIER(uri, method, tier) { (uri), (method), (tier) }
static const route_tier_entry_t kRouteTierTable[] = {
    ROUTE_TIER("/a", HTTP_GET, ROUTE_TIER_OPEN),
    ROUTE_TIER("/b", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/a", HTTP_GET, ROUTE_TIER_USER),
};
'@
$dupTableFile = Join-Path $scratchDir "table_dup.h"
Set-Content -Path $dupTableFile -Value $dupTableContent -Encoding utf8

$dupThrew = $false
$dupMessage = ""
try {
    Get-TieredKeys -Path $dupTableFile | Out-Null
} catch {
    $dupThrew = $true
    $dupMessage = $_.Exception.Message
}
if (-not $dupThrew) {
    $failures += "Assertion 3b FAILED: a table with 'HTTP_GET /a' listed twice (OPEN then USER) did not throw -- a duplicate key can silently pick a winner."
} elseif ($dupMessage -notmatch [regex]::Escape("HTTP_GET /a")) {
    $failures += "Assertion 3b FAILED: the duplicate-key error did not name the offending key 'HTTP_GET /a': $dupMessage"
} else {
    Write-Host "Assertion 3b OK: a duplicated (method, uri) key throws and names the key, instead of one reader silently taking the last row."
}

# Also prove Invoke-RouteTierCoverageScan itself (not just the inner
# helper) surfaces this -- the production entry point every caller
# (including the real check_route_tier_coverage.ps1 main body) goes
# through.
$dupThroughScan = $false
try {
    Invoke-RouteTierCoverageScan -DriversDir $synthDriversDir -TableFile $dupTableFile -BlindnessFloor 2
} catch {
    $dupThroughScan = $true
}
if (-not $dupThroughScan) {
    $failures += "Assertion 3c FAILED: Invoke-RouteTierCoverageScan did not propagate the duplicate-key failure from Get-TieredKeys."
} else {
    Write-Host "Assertion 3c OK: Invoke-RouteTierCoverageScan (the real entry point) also fails on a duplicate key, not just the inner helper."
}

# --- Assertion 4: run the SAME production function against the REAL
# production drivers tree and REAL route_tier_table.h (read-only -- this
# does not modify either). Proves today's real table is not vacuously
# passing because the scan function never actually reaches real code. ---
$realDriversDir = (Resolve-Path (Join-Path $repoRoot "firmware\KilnFW\App\drivers")).Path
$realTableFile = (Get-ChildItem -Path $realDriversDir -Filter "route_tier_table.h" -File -Recurse | Select-Object -First 1).FullName
if (-not $realTableFile) {
    $failures += "Assertion 4 FAILED: route_tier_table.h not found under $realDriversDir."
} else {
    $realResult = Invoke-RouteTierCoverageScan -DriversDir $realDriversDir -TableFile $realTableFile
    if ($realResult.Missing.Count -ne 0) {
        $realMissingDesc = ($realResult.Missing | ForEach-Object { "$($_.Method) $($_.Uri) ($($_.File))" }) -join ", "
        $failures += "Assertion 4 FAILED: real production tree has $($realResult.Missing.Count) route(s) with no tier: $realMissingDesc"
    } else {
        Write-Host "Assertion 4 OK: real production tree ($($realResult.TotalRoutes) routes) fully covered by $realTableFile ($($realResult.TotalTiered) tiered rows)."
    }
}

# --- Assertion 5 (plan section 12, point 5, "none orphaned" half): every
# row in the REAL route_tier_table.h corresponds to a route actually
# registered somewhere in the real drivers tree. This is the reverse
# direction from assertion 4 (which proves every registered route has a
# table row) -- a table row with no matching real route is a stale entry
# that route_tier_table.h's own header comment says "should still be deleted
# when noticed" but which nothing mechanically catches today. Reuses the
# SAME Get-RegisteredRoutes/Get-TieredKeys the production check itself uses
# (dot-sourced above), not a second parser. ---
if ($realTableFile) {
    $realRegisteredKeys = New-Object System.Collections.Generic.HashSet[string]
    foreach ($f in (Get-ChildItem -Path $realDriversDir -Filter "*.c" -File -Recurse)) {
        foreach ($r in (Get-RegisteredRoutes -Path $f.FullName)) {
            if ($null -ne $r.Method) {
                [void]$realRegisteredKeys.Add("$($r.Method) $($r.Uri)")
            }
        }
    }
    $realTieredKeys = Get-TieredKeys -Path $realTableFile
    $orphaned = @()
    foreach ($key in $realTieredKeys.Keys) {
        if (-not $realRegisteredKeys.Contains($key)) {
            $orphaned += $key
        }
    }
    if ($orphaned.Count -gt 0) {
        $failures += "Assertion 5 FAILED: $($orphaned.Count) row(s) in $realTableFile have no matching registered route (orphaned): $($orphaned -join ', ')"
    } else {
        Write-Host "Assertion 5 OK: every row in the real route_tier_table.h ($($realTieredKeys.Count) rows) corresponds to an actually-registered route -- none orphaned."
    }
}

} finally {
    Remove-Item -Path $scratchDir -Recurse -Force -ErrorAction SilentlyContinue
}

if ($failures.Count -gt 0) {
    Write-Host "TEST_CHECK_ROUTE_TIER_COVERAGE FAILED:" -ForegroundColor Red
    foreach ($f in $failures) { Write-Host "  $f" -ForegroundColor Red }
    throw "$($failures.Count) assertion(s) failed."
}

Write-Host "test_check_route_tier_coverage: all assertions passed (fully-tiered synthetic tree=0 missing, injected untiered route detected and named, blindness floor has teeth, real production tree fully covered in both directions -- none missing, none orphaned)." -ForegroundColor Green
exit 0
