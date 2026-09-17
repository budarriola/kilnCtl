# test_check_config_migration_steps.ps1 -- negative test for
# tools/check_config_migration_steps.ps1 (docs/CONFIG_MIGRATION_CHAIN_PLAN.md
# section 5).
#
# Proves Test-ZonesMigrationSteps (the PRODUCTION function, the same one
# check_config_migration_steps.ps1's main body calls) can actually detect a
# version bump with no matching step, and every other rule 1-5 shape it
# claims to enforce -- not just that it reports PASS on a tree that happens
# to already be compliant. Same dot-source pattern as
# test_check_hal_include_boundary.ps1 / test_check_route_tier_coverage.ps1:
# dot-source the check (which, per its own guard, only defines the function
# when dot-sourced) and call it directly against synthetic text, never the
# real repo's zones_config_json.h/zones_config_migrate.c.
#
#   1. A version still inside the tail (26) with no step function -> PASS.
#   2. The same version, but with a stray step function already present
#      (jumping ahead before the tail is exhausted) -> FAIL, named.
#   3. A version past the tail (27) with the matching step, a correctly
#      frozen+asserted input type, a referenced fixture, and a correct
#      expiry floor -> PASS.
#   4. The same v27 case with the step function MISSING entirely -> FAIL,
#      naming the missing zones_cfg_step_v26_to_v27.
#   5. The same v27 case with the step present but TWO step functions total
#      (a D1 violation) -> FAIL, naming the count.
#   6. The same v27 case with the step present but its frozen input type's
#      last field is NOT crc32 -> FAIL.
#   7. The same v27 case with the step present but no fixture referenced by
#      any test file -> FAIL.
#   8. The same v27 case with the step present but the expiry floor constant
#      uses the wrong offset (ZONES_CFG_VERSION - 5 instead of - 8) -> FAIL.
#   9. The REAL production zones_config_json.h/zones_config_migrate.c/test
#      tree, dot-sourced and scanned directly (not a copy) -> PASS, proving
#      today's real check is not vacuous on the actual tree.
#
# This does not touch the real repo tree; assertions 1-8 are entirely
# synthetic text, and assertion 9 only READS the real tree
# (Test-ZonesMigrationSteps never writes anything).
#
# Usage: powershell -ExecutionPolicy Bypass -File App\test\test_check_config_migration_steps.ps1

$ErrorActionPreference = "Stop"

$testDir = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $testDir "..\..\..\..")).Path
$checkScript = Join-Path $repoRoot "tools\check_config_migration_steps.ps1"

if (-not (Test-Path $checkScript)) {
    throw "test_check_config_migration_steps: expected $checkScript not found -- has it moved?"
}

# Dot-source: per check_config_migration_steps.ps1's own guard, this defines
# Test-ZonesMigrationSteps and runs nothing else ($MyInvocation.InvocationName
# is '.' during dot-sourcing).
. $checkScript

$failures = @()

# ---------------------------------------------------------------------
# Building blocks for a synthetic, correctly-shaped v27 step (assertion 3),
# then each subsequent assertion mutates one piece of it to prove that
# piece is actually load-bearing.
# ---------------------------------------------------------------------

$headerV26 = "#define ZONES_CFG_VERSION 26`n"
$headerV27 = "#define ZONES_CFG_VERSION 27`n" +
    "#define ZONES_CFG_MIGRATION_EXPIRY_FLOOR (ZONES_CFG_VERSION - 8)`n"

$goodInputType = @"
typedef struct {
    uint8_t name[16];
    uint8_t thermo_count;
    uint32_t crc32;
} zones_cfg_v26_t;
"@

$badInputType = @"
typedef struct {
    uint8_t name[16];
    uint32_t crc32;
    uint8_t thermo_count;
} zones_cfg_v26_t;
"@

$goodStepFn = @"
static void zones_cfg_step_v26_to_v27(const zones_cfg_v26_t *src, zones_cfg_t *dst)
{
    memset(dst, 0, sizeof(*dst));
}
"@

$staticAssert = "_Static_assert(sizeof(zones_cfg_v26_t) == 21, `"pin it`");`n"
$oldestCase = "    case 1: {`n        break;`n    }`n"

function New-MigrateFileText {
    param(
        [string]$InputType = $goodInputType,
        [string]$StepFn = $goodStepFn,
        [bool]$IncludeAssert = $true,
        [bool]$IncludeSecondStep = $false,
        [string]$CaseBlock = $oldestCase
    )
    $text = $CaseBlock + "`n" + $InputType + "`n"
    if ($IncludeAssert) { $text += $staticAssert }
    $text += "`n" + $StepFn + "`n"
    if ($IncludeSecondStep) {
        $text += "`nstatic void zones_cfg_step_v25_to_v26(const zones_cfg_v25_t *src, zones_cfg_v26_t *dst) { }`n"
    }
    return $text
}

# --- Assertion 1: still inside the tail, no step -> PASS. ---
$r1 = Test-ZonesMigrationSteps -VersionHeaderText $headerV26 -MigrateFileText $oldestCase `
    -TestTreeFileNames @() -TestTreeFileContents @()
if (-not $r1.Ok) {
    $failures += "Assertion 1 FAILED: expected PASS for v26 (in-tail, no step), got failures: $($r1.Failures -join '; ')"
} else {
    Write-Host "Assertion 1 OK: v26 in-tail with no step function passes."
}

# --- Assertion 2: still inside the tail, but a stray step already exists -> FAIL. ---
$r2 = Test-ZonesMigrationSteps -VersionHeaderText $headerV26 -MigrateFileText (New-MigrateFileText) `
    -TestTreeFileNames @() -TestTreeFileContents @()
if ($r2.Ok) {
    $failures += "Assertion 2 FAILED: expected FAIL for v26 with a stray zones_cfg_step_* already present, got PASS."
} elseif (($r2.Failures -join " ") -notmatch "tail has not been exhausted") {
    $failures += "Assertion 2 FAILED: failed for the wrong reason: $($r2.Failures -join '; ')"
} else {
    Write-Host "Assertion 2 OK: a step present before the tail is exhausted is caught."
}

# --- Assertion 3: v27, everything correct -> PASS. ---
$fixtureName = "zones_v26.bin"
$testNames3 = @("cfg_blobs/$fixtureName")
$testContents3 = @("// references $fixtureName in a fixture table`n")
# v27's expiry floor is ZONES_CFG_VERSION - 8 = 19, so the tail's oldest
# case must be >= 19 here for the "everything correct" scenario -- case 1
# (used elsewhere, where no floor check gates the outcome) would trip rule 5.
$caseAtFloor = "    case 19: {`n        break;`n    }`n"
$r3 = Test-ZonesMigrationSteps -VersionHeaderText $headerV27 -MigrateFileText (New-MigrateFileText -CaseBlock $caseAtFloor) `
    -TestTreeFileNames $testNames3 -TestTreeFileContents $testContents3
if (-not $r3.Ok) {
    $failures += "Assertion 3 FAILED: expected PASS for a fully correct v27 step, got failures: $($r3.Failures -join '; ')"
} else {
    Write-Host "Assertion 3 OK: a fully correct v27 step (step fn, frozen+asserted input, referenced fixture, correct floor) passes."
}

# --- Assertion 4: v27, step function missing entirely -> FAIL, named. ---
$r4 = Test-ZonesMigrationSteps -VersionHeaderText $headerV27 -MigrateFileText $oldestCase `
    -TestTreeFileNames @() -TestTreeFileContents @()
if ($r4.Ok) {
    $failures += "Assertion 4 FAILED: expected FAIL when zones_cfg_step_v26_to_v27 is missing, got PASS."
} elseif (($r4.Failures -join " ") -notmatch "zones_cfg_step_v26_to_v27") {
    $failures += "Assertion 4 FAILED: failure did not name the missing step: $($r4.Failures -join '; ')"
} else {
    Write-Host "Assertion 4 OK: a missing step function is caught and named."
}

# --- Assertion 5: v27, two step functions total -> FAIL, count named. ---
$r5 = Test-ZonesMigrationSteps -VersionHeaderText $headerV27 -MigrateFileText (New-MigrateFileText -IncludeSecondStep $true) `
    -TestTreeFileNames $testNames3 -TestTreeFileContents $testContents3
if ($r5.Ok) {
    $failures += "Assertion 5 FAILED: expected FAIL when two zones_cfg_step_* functions exist, got PASS."
} elseif (($r5.Failures -join " ") -notmatch "exactly one") {
    $failures += "Assertion 5 FAILED: failure did not cite the D1 'exactly one' rule: $($r5.Failures -join '; ')"
} else {
    Write-Host "Assertion 5 OK: a second, accumulated step function (D1 violation) is caught."
}

# --- Assertion 6: v27, frozen input type's last field is not crc32 -> FAIL. ---
$r6 = Test-ZonesMigrationSteps -VersionHeaderText $headerV27 -MigrateFileText (New-MigrateFileText -InputType $badInputType) `
    -TestTreeFileNames $testNames3 -TestTreeFileContents $testContents3
if ($r6.Ok) {
    $failures += "Assertion 6 FAILED: expected FAIL when crc32 is not the frozen input type's last field, got PASS."
} elseif (($r6.Failures -join " ") -notmatch "last field is not crc32") {
    $failures += "Assertion 6 FAILED: failure did not cite the crc32-last-field rule: $($r6.Failures -join '; ')"
} else {
    Write-Host "Assertion 6 OK: crc32 not being the last field of the frozen input type is caught."
}

# --- Assertion 7: v27, fixture exists but is referenced by no test source -> FAIL. ---
$r7 = Test-ZonesMigrationSteps -VersionHeaderText $headerV27 -MigrateFileText (New-MigrateFileText) `
    -TestTreeFileNames @("cfg_blobs/$fixtureName") -TestTreeFileContents @("// no mention of the fixture here`n")
if ($r7.Ok) {
    $failures += "Assertion 7 FAILED: expected FAIL when the fixture blob is unreferenced by any test source, got PASS."
} elseif (($r7.Failures -join " ") -notmatch "sit unread") {
    $failures += "Assertion 7 FAILED: failure did not cite the unreferenced-fixture rule: $($r7.Failures -join '; ')"
} else {
    Write-Host "Assertion 7 OK: a fixture blob referenced by no test source is caught."
}

# --- Assertion 8: v27, expiry floor uses the wrong offset -> FAIL. ---
$badFloorHeader = "#define ZONES_CFG_VERSION 27`n#define ZONES_CFG_MIGRATION_EXPIRY_FLOOR (ZONES_CFG_VERSION - 5)`n"
$r8 = Test-ZonesMigrationSteps -VersionHeaderText $badFloorHeader -MigrateFileText (New-MigrateFileText) `
    -TestTreeFileNames $testNames3 -TestTreeFileContents $testContents3
if ($r8.Ok) {
    $failures += "Assertion 8 FAILED: expected FAIL when the expiry floor offset is wrong (5 instead of 8), got PASS."
} elseif (($r8.Failures -join " ") -notmatch "ZONES_CFG_VERSION - 8") {
    $failures += "Assertion 8 FAILED: failure did not cite the plan's ZONES_CFG_VERSION - 8 floor: $($r8.Failures -join '; ')"
} else {
    Write-Host "Assertion 8 OK: a wrong expiry-floor offset is caught."
}

# --- Assertion 9: the REAL production files, read directly -> PASS. ---
$realVersionHeader = Join-Path $repoRoot "firmware\KilnFW\App\drivers\persist\zones_config_json.h"
$realMigrateFile = Join-Path $repoRoot "firmware\KilnFW\App\drivers\persist\zones_config_migrate.c"
$realTestTreeRoot = Join-Path $repoRoot "firmware\KilnFW\App\test"
$realVersionText = Get-Content -Raw $realVersionHeader
$realMigrateText = Get-Content -Raw $realMigrateFile
$realFiles = Get-ChildItem -Path $realTestTreeRoot -Recurse -File
$realNames = @()
$realContents = @()
foreach ($f in $realFiles) {
    $realNames += ($f.FullName.Substring($realTestTreeRoot.Length + 1) -replace '\\', '/')
    if ($f.Extension -in @(".c", ".h", ".py", ".ps1", ".json")) {
        $realContents += (Get-Content -Raw $f.FullName -ErrorAction SilentlyContinue)
    } else {
        $realContents += ""
    }
}
$r9 = Test-ZonesMigrationSteps -VersionHeaderText $realVersionText -MigrateFileText $realMigrateText `
    -TestTreeFileNames $realNames -TestTreeFileContents $realContents
if (-not $r9.Ok) {
    $failures += "Assertion 9 FAILED: expected PASS against the real production tree, got failures: $($r9.Failures -join '; ')"
} else {
    Write-Host "Assertion 9 OK: the real production tree (ZONES_CFG_VERSION still within the tail) passes."
}

if ($failures.Count -gt 0) {
    Write-Host ""
    Write-Host "test_check_config_migration_steps: $($failures.Count) assertion(s) FAILED:" -ForegroundColor Red
    foreach ($f in $failures) { Write-Host "  - $f" -ForegroundColor Red }
    exit 1
}

Write-Host ""
Write-Host "test_check_config_migration_steps: all 9 assertions passed." -ForegroundColor Green
exit 0
