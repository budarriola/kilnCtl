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
#  10-15. The same FAIL-then-PASS-on-real-tree pattern for the three
#      follow-up stores' narrower rules: kiln-config slots
#      (Test-KilnCfgStoreMigrationStep), fire profiles
#      (Test-ProfilesMigrationStep), and RP2040 safety config
#      (Test-SaftyConfigStoreMigrationStep).
#  16-17. A comment-only mention of the expected step/converter name (not a
#      real definition) does NOT satisfy the kiln-config/profiles rules --
#      proves the patterns are anchored to a definition, not raw text.
#   18. RP2040 safety config: the expected macro is defined but never
#      branched on in config_store.c (only mentioned in a header comment)
#      -> FAIL, the orphaned-macro path distinct from assertion 14's
#      missing-macro path.
#
# This does not touch the real repo tree; assertions 1-8, 10, 12, 14, 16-18
# are entirely synthetic text, and assertions 9, 11, 13, 15 only READ the
# real tree (none of these functions ever writes anything).
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

# ---------------------------------------------------------------------
# Assertions 10-15: the follow-up rules for the other three governed
# stores (docs/CONFIG_MIGRATION_CHAIN_PLAN.md sec 5's later revision).
# Each store gets one FAIL (a version bump with no matching step, in
# synthetic scratch text) and one PASS (against the real production tree,
# which is already compliant with the narrower rule this script enforces
# for it) -- same "prove it can fail, then prove today's tree is not
# vacuously green" pattern as assertions 1-9.
# ---------------------------------------------------------------------

# --- Assertion 10: kiln-config slots, version bumped with no step -> FAIL, named. ---
$r10 = Test-KilnCfgStoreMigrationStep -VersionHeaderText "#define KILN_CFG_STORE_VERSION 4`n" `
    -SourceText "static void migrate_store_v2_to_v3(const kiln_cfg_store_blob_v2_t *src, kiln_cfg_store_blob_t *dst) { }`n"
if ($r10.Ok) {
    $failures += "Assertion 10 FAILED: expected FAIL when KILN_CFG_STORE_VERSION bumps to 4 with no migrate_store_v3_to_v4, got PASS."
} elseif (($r10.Failures -join " ") -notmatch "migrate_store_v3_to_v4") {
    $failures += "Assertion 10 FAILED: failure did not name the missing kiln-config step: $($r10.Failures -join '; ')"
} else {
    Write-Host "Assertion 10 OK: a kiln-config slot store version bump with no matching migrate_store_* step is caught."
}

# --- Assertion 11: kiln-config slots, the REAL production files -> PASS. ---
$realKilnCfgHeader = Get-Content -Raw (Join-Path $repoRoot "firmware\KilnFW\App\drivers\persist\kiln_cfg_store_internal.h")
$realKilnCfgSource = Get-Content -Raw (Join-Path $repoRoot "firmware\KilnFW\App\drivers\persist\kiln_cfg_store.c")
$r11 = Test-KilnCfgStoreMigrationStep -VersionHeaderText $realKilnCfgHeader -SourceText $realKilnCfgSource
if (-not $r11.Ok) {
    $failures += "Assertion 11 FAILED: expected PASS against the real kiln-config slot store, got failures: $($r11.Failures -join '; ')"
} else {
    Write-Host "Assertion 11 OK: the real kiln-config slot store (migrate_store_v2_to_v3 for KILN_CFG_STORE_VERSION 3) passes."
}

# --- Assertion 12: fire profiles, version bumped with no converter -> FAIL, named. ---
$r12 = Test-ProfilesMigrationStep -VersionHeaderText "#define PROFILE_VERSION 5`n" `
    -SourceText "static void convert_profile_v3(const profile_persisted_v3_t *src, profile_t *out) { }`n"
if ($r12.Ok) {
    $failures += "Assertion 12 FAILED: expected FAIL when PROFILE_VERSION bumps to 5 with no convert_profile_v4, got PASS."
} elseif (($r12.Failures -join " ") -notmatch "convert_profile_v4") {
    $failures += "Assertion 12 FAILED: failure did not name the missing profiles converter: $($r12.Failures -join '; ')"
} else {
    Write-Host "Assertion 12 OK: a fire-profiles version bump with no matching convert_profile_* converter is caught."
}

# --- Assertion 13: fire profiles, the REAL production file -> PASS. ---
$realProfilesSource = Get-Content -Raw (Join-Path $repoRoot "firmware\KilnFW\App\drivers\http\profiles_http.c")
$r13 = Test-ProfilesMigrationStep -VersionHeaderText $realProfilesSource -SourceText $realProfilesSource
if (-not $r13.Ok) {
    $failures += "Assertion 13 FAILED: expected PASS against the real fire profiles store, got failures: $($r13.Failures -join '; ')"
} else {
    Write-Host "Assertion 13 OK: the real fire profiles store (convert_profile_v3 for PROFILE_VERSION 4) passes."
}

# --- Assertion 14: RP2040 safety config, version bumped with no V<N-1> macro -> FAIL, named. ---
$r14 = Test-SaftyConfigStoreMigrationStep -VersionHeaderText "#define CONFIG_STORE_FORMAT_VERSION 4u`n#define CONFIG_STORE_FORMAT_VERSION_V1 1u`n#define CONFIG_STORE_FORMAT_VERSION_V2 2u`n" `
    -SourceText "if (version == CONFIG_STORE_FORMAT_VERSION_V1) { } if (version == CONFIG_STORE_FORMAT_VERSION_V2) { }`n"
if ($r14.Ok) {
    $failures += "Assertion 14 FAILED: expected FAIL when CONFIG_STORE_FORMAT_VERSION bumps to 4 with no _V3 macro, got PASS."
} elseif (($r14.Failures -join " ") -notmatch "CONFIG_STORE_FORMAT_VERSION_V3") {
    $failures += "Assertion 14 FAILED: failure did not name the missing safety-config macro: $($r14.Failures -join '; ')"
} else {
    Write-Host "Assertion 14 OK: a safety-config version bump with no matching CONFIG_STORE_FORMAT_VERSION_V<N-1> macro is caught."
}

# --- Assertion 15: RP2040 safety config, the REAL production files -> PASS. ---
$realSaftyHeader = Get-Content -Raw (Join-Path $repoRoot "firmware\SaftyFW\src\config_store.h")
$realSaftySource = Get-Content -Raw (Join-Path $repoRoot "firmware\SaftyFW\src\config_store.c")
$r15 = Test-SaftyConfigStoreMigrationStep -VersionHeaderText $realSaftyHeader -SourceText $realSaftySource
if (-not $r15.Ok) {
    $failures += "Assertion 15 FAILED: expected PASS against the real RP2040 safety config store, got failures: $($r15.Failures -join '; ')"
} else {
    Write-Host "Assertion 15 OK: the real RP2040 safety config store (CONFIG_STORE_FORMAT_VERSION_V2 for version 3) passes."
}

# --- Assertion 16: kiln-config slots, a COMMENT naming the step text does
# NOT satisfy the rule -- must be anchored to a real definition. ---
$r16 = Test-KilnCfgStoreMigrationStep -VersionHeaderText "#define KILN_CFG_STORE_VERSION 4`n" `
    -SourceText "/* see migrate_store_v3_to_v4( ) for the shape */`nstatic void migrate_store_v2_to_v3(const kiln_cfg_store_blob_v2_t *src, kiln_cfg_store_blob_t *dst) { }`n"
if ($r16.Ok) {
    $failures += "Assertion 16 FAILED: a comment-only mention of migrate_store_v3_to_v4 satisfied the rule -- pattern is not anchored to a definition."
} elseif (($r16.Failures -join " ") -notmatch "migrate_store_v3_to_v4") {
    $failures += "Assertion 16 FAILED: failure did not name the missing kiln-config step: $($r16.Failures -join '; ')"
} else {
    Write-Host "Assertion 16 OK: a comment-only mention of the kiln-config step does not satisfy the rule."
}

# --- Assertion 17: fire profiles, a COMMENT naming the converter does NOT
# satisfy the rule -- must be anchored to a real definition. ---
$r17 = Test-ProfilesMigrationStep -VersionHeaderText "#define PROFILE_VERSION 5`n" `
    -SourceText "/* v5 will need convert_profile_v4() eventually */`nstatic void convert_profile_v3(const profile_persisted_v3_t *src, profile_t *out) { }`n"
if ($r17.Ok) {
    $failures += "Assertion 17 FAILED: a comment-only mention of convert_profile_v4 satisfied the rule -- pattern is not anchored to a definition."
} elseif (($r17.Failures -join " ") -notmatch "convert_profile_v4") {
    $failures += "Assertion 17 FAILED: failure did not name the missing profiles converter: $($r17.Failures -join '; ')"
} else {
    Write-Host "Assertion 17 OK: a comment-only mention of the profiles converter does not satisfy the rule."
}

# --- Assertion 18: RP2040 safety config, macro defined but NEVER branched on
# in the .c file (only mentioned in a header comment) -> FAIL, orphaned-macro
# path, distinct from assertion 14's missing-macro path. ---
$r18 = Test-SaftyConfigStoreMigrationStep -VersionHeaderText "#define CONFIG_STORE_FORMAT_VERSION 4u`n#define CONFIG_STORE_FORMAT_VERSION_V1 1u`n#define CONFIG_STORE_FORMAT_VERSION_V2 2u`n#define CONFIG_STORE_FORMAT_VERSION_V3 3u`n// format_version == CONFIG_STORE_FORMAT_VERSION_V3 (legacy note only)`n" `
    -SourceText "if (version == CONFIG_STORE_FORMAT_VERSION_V1) { } if (version == CONFIG_STORE_FORMAT_VERSION_V2) { }`n"
if ($r18.Ok) {
    $failures += "Assertion 18 FAILED: expected FAIL when CONFIG_STORE_FORMAT_VERSION_V3 is defined (and only mentioned in a header comment) but never branched on in config_store.c, got PASS."
} elseif (($r18.Failures -join " ") -notmatch "orphaned macro") {
    $failures += "Assertion 18 FAILED: failure did not name the orphaned-macro condition: $($r18.Failures -join '; ')"
} else {
    Write-Host "Assertion 18 OK: a macro defined but only mentioned in a header comment (never branched on in the .c file) is caught as orphaned."
}

if ($failures.Count -gt 0) {
    Write-Host ""
    Write-Host "test_check_config_migration_steps: $($failures.Count) assertion(s) FAILED:" -ForegroundColor Red
    foreach ($f in $failures) { Write-Host "  - $f" -ForegroundColor Red }
    exit 1
}

Write-Host ""
Write-Host "test_check_config_migration_steps: all 18 assertions passed." -ForegroundColor Green
exit 0
