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
#  19-20. Fire profiles' 2026-09-19 frozen-input-struct extension (plan sec
#      5.1's "check taught its existing scaffolding" follow-up): the
#      converter exists but its frozen input type profile_persisted_v<N>_t
#      either has no _Static_assert pinning its sizeof (19), or has one but
#      crc32 is not its last field (20) -> FAIL, named, in each case.
#   21. Fire profiles: the _Static_assert pinning sizeof exists but is
#      COMMENTED OUT (`// _Static_assert(...)`) -> FAIL, proving
#      Remove-CComments actually strips it before the regex runs rather than
#      a commented-out assert satisfying the rule.
#   22. Fire profiles: the true last struct member is followed by a trailing
#      `/* ... crc32 ... */` comment mentioning "crc32" -- the OLD
#      last-member-line check took the whole raw line (including the
#      comment) and matched the word "crc32" inside the comment text itself,
#      so a struct whose real last field is NOT crc32 still passed -> FAIL
#      after the fix, since the comment is stripped before the last member
#      is read.
#
# This does not touch the real repo tree; assertions 1-8, 10, 12, 14, 16-22
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

# ---------------------------------------------------------------------
# Assertions 19-20: fire profiles' frozen-input-struct extension
# (2026-09-19, docs/CONFIG_MIGRATION_CHAIN_PLAN.md sec 5.1). A correctly
# named convert_profile_v<N-1>(...) converter exists in both cases -- these
# prove the NEW struct-discipline checks fire independently of the
# pre-existing converter-existence check.
# ---------------------------------------------------------------------

# --- Assertion 19: converter exists, but profile_persisted_v4_t (its frozen
# input type) has no _Static_assert pinning its sizeof -> FAIL, named. ---
$profilesNoAssertSource = @"
typedef struct {
    uint8_t version;
    uint8_t data[300];
    uint32_t crc32;
} profile_persisted_v4_t;

static void convert_profile_v4(const profile_persisted_v4_t *src, profile_t *out) { }
"@
$r19 = Test-ProfilesMigrationStep -VersionHeaderText "#define PROFILE_VERSION 5`n" -SourceText $profilesNoAssertSource
if ($r19.Ok) {
    $failures += "Assertion 19 FAILED: expected FAIL when profile_persisted_v4_t has no _Static_assert pinning its sizeof, got PASS."
} elseif (($r19.Failures -join " ") -notmatch "no _Static_assert pinning sizeof\(profile_persisted_v4_t\)") {
    $failures += "Assertion 19 FAILED: failure did not cite the missing sizeof assert: $($r19.Failures -join '; ')"
} else {
    Write-Host "Assertion 19 OK: a frozen input struct with no sizeof _Static_assert is caught."
}

# --- Assertion 20: converter exists, sizeof is pinned, but crc32 is NOT the
# frozen input type's last field -> FAIL, named. ---
$profilesBadCrcSource = @"
typedef struct {
    uint8_t version;
    uint32_t crc32;
    uint8_t data[300];
} profile_persisted_v4_t;
_Static_assert(sizeof(profile_persisted_v4_t) == 308, "pin it");

static void convert_profile_v4(const profile_persisted_v4_t *src, profile_t *out) { }
"@
$r20 = Test-ProfilesMigrationStep -VersionHeaderText "#define PROFILE_VERSION 5`n" -SourceText $profilesBadCrcSource
if ($r20.Ok) {
    $failures += "Assertion 20 FAILED: expected FAIL when profile_persisted_v4_t's last field is not crc32, got PASS."
} elseif (($r20.Failures -join " ") -notmatch "last field is not crc32") {
    $failures += "Assertion 20 FAILED: failure did not cite the crc32-last-field rule: $($r20.Failures -join '; ')"
} else {
    Write-Host "Assertion 20 OK: a frozen input struct whose last field is not crc32 is caught."
}

# --- Assertion 21: the sizeof _Static_assert exists but is COMMENTED OUT ->
# FAIL, proving Remove-CComments strips it before the regex runs. ---
$profilesCommentedAssertSource = @"
typedef struct {
    uint8_t version;
    uint8_t data[300];
    uint32_t crc32;
} profile_persisted_v4_t;
// _Static_assert(sizeof(profile_persisted_v4_t) == 308, "pin it");

static void convert_profile_v4(const profile_persisted_v4_t *src, profile_t *out) { }
"@
$r21 = Test-ProfilesMigrationStep -VersionHeaderText "#define PROFILE_VERSION 5`n" -SourceText $profilesCommentedAssertSource
if ($r21.Ok) {
    $failures += "Assertion 21 FAILED: expected FAIL when profile_persisted_v4_t's sizeof _Static_assert is commented out, got PASS."
} elseif (($r21.Failures -join " ") -notmatch "no _Static_assert pinning sizeof\(profile_persisted_v4_t\)") {
    $failures += "Assertion 21 FAILED: failure did not cite the missing (commented-out) sizeof assert: $($r21.Failures -join '; ')"
} else {
    Write-Host "Assertion 21 OK: a commented-out sizeof _Static_assert does not satisfy the rule."
}

# --- Assertion 22: the true last struct member is NOT crc32, but is followed
# by a trailing comment that happens to mention "crc32" -> FAIL. Before the
# Remove-CComments fix, the last-member-line check read the raw line
# (including the comment) and matched the word "crc32" inside the comment
# text, so this exact shape wrongly passed. ---
$profilesTrailingCommentSource = @"
typedef struct {
    uint8_t version;
    uint32_t crc32;
    uint8_t data[300]; /* follows crc32 */
} profile_persisted_v4_t;
_Static_assert(sizeof(profile_persisted_v4_t) == 308, "pin it");

static void convert_profile_v4(const profile_persisted_v4_t *src, profile_t *out) { }
"@
$r22 = Test-ProfilesMigrationStep -VersionHeaderText "#define PROFILE_VERSION 5`n" -SourceText $profilesTrailingCommentSource
if ($r22.Ok) {
    $failures += "Assertion 22 FAILED: expected FAIL when the true last member is 'data' with only a trailing comment mentioning crc32, got PASS."
} elseif (($r22.Failures -join " ") -notmatch "last field is not crc32") {
    $failures += "Assertion 22 FAILED: failure did not cite the crc32-last-field rule: $($r22.Failures -join '; ')"
} else {
    Write-Host "Assertion 22 OK: a trailing comment mentioning crc32 after the true (non-crc32) last member does not satisfy the rule."
}

# ---------------------------------------------------------------------
# Assertions 23-40: chain-integrity rules (plan sec 5.1, 2026-10-03):
# one step per bump, no skipped version, version constant == last step,
# duplicates, forward declarations, and commented-out steps/branches.
# ---------------------------------------------------------------------
function Test-ChainCase {
    param([int]$N, $Result, [bool]$ExpectOk, [string]$Pattern, [string]$What)
    if ($ExpectOk) {
        if (-not $Result.Ok) { $script:failures += "Assertion ${N} FAILED: expected PASS ($What), got: $($Result.Failures -join '; ')" }
        else { Write-Host "Assertion $N OK: $What" }
    } elseif ($Result.Ok) {
        $script:failures += "Assertion ${N} FAILED: expected FAIL ($What), got PASS."
    } elseif (($Result.Failures -join " ") -notmatch $Pattern) {
        $script:failures += "Assertion ${N} FAILED: failure did not match /$Pattern/: $($Result.Failures -join '; ')"
    } else { Write-Host "Assertion $N OK: $What" }
}

$kilnHdr3 = "#define KILN_CFG_STORE_VERSION 3`n"
$kStep12 = "static void migrate_store_v1_to_v2(const a *s, b *d) { }`n"
$kStep23 = "static void migrate_store_v2_to_v3(const b *s, c *d) { }`n"
Test-ChainCase 23 (Test-KilnCfgStoreMigrationStep -VersionHeaderText $kilnHdr3 -SourceText ($kStep12 + $kStep23)) $true "" "kiln-config: contiguous v1->v2->v3 chain at version 3 passes."
Test-ChainCase 24 (Test-KilnCfgStoreMigrationStep -VersionHeaderText $kilnHdr3 -SourceText $kStep23) $false "kiln-config slot store.*v1_to_v2.*skipped" "kiln-config: a missing v1_to_v2 step (skipped version) is caught."
Test-ChainCase 25 (Test-KilnCfgStoreMigrationStep -VersionHeaderText $kilnHdr3 -SourceText ($kStep12 + $kStep23 + "static void migrate_store_v3_to_v5(const c *s, d *d) { }`n")) $false "migrate_store_v3_to_v5 is not a single-version step" "kiln-config: a step spanning two versions is caught."

$profHdr4 = "#define PROFILE_VERSION 4`n"
$profBase = "static void convert_profile_v1(const a *s, profile_t *o) { }`nstatic void convert_profile_v2(const b *s, profile_t *o) { }`n"
$profV3 = @"
typedef struct {
    uint8_t version;
    uint32_t crc32;
} profile_persisted_v3_t;
_Static_assert(sizeof(profile_persisted_v3_t) == 8, "pin");
static void convert_profile_v3(const profile_persisted_v3_t *s, profile_t *o) { }

"@
Test-ChainCase 26 (Test-ProfilesMigrationStep -VersionHeaderText $profHdr4 -SourceText ($profHdr4 + $profBase + $profV3)) $true "" "fire profiles: converters v1..v3 all present at version 4 passes."
Test-ChainCase 27 (Test-ProfilesMigrationStep -VersionHeaderText $profHdr4 -SourceText ($profHdr4 + "static void convert_profile_v1(const a *s, profile_t *o) { }`n" + $profV3)) $false "fire profiles store.*convert_profile_v2.*skipped" "fire profiles: a missing convert_profile_v2 (skipped version) is caught."
Test-ChainCase 28 (Test-ProfilesMigrationStep -VersionHeaderText $profHdr4 -SourceText ($profHdr4 + $profBase + $profV3 + "static void convert_profile_v4(const x *s, profile_t *o) { }`n")) $false "convert_profile_v4 exists but PROFILE_VERSION is only 4" "fire profiles: a converter at or above PROFILE_VERSION is caught."

$saftyHdr3 = "#define CONFIG_STORE_FORMAT_VERSION 3u`n#define CONFIG_STORE_FORMAT_VERSION_V1 1u`n#define CONFIG_STORE_FORMAT_VERSION_V2 2u`n"
$saftySrc = "if (version == CONFIG_STORE_FORMAT_VERSION_V2) { } if (version == CONFIG_STORE_FORMAT_VERSION_V1) { }`n"
Test-ChainCase 29 (Test-SaftyConfigStoreMigrationStep -VersionHeaderText $saftyHdr3 -SourceText $saftySrc) $true "" "RP2040: macros and branches for v1 and v2 at version 3 passes."
Test-ChainCase 30 (Test-SaftyConfigStoreMigrationStep -VersionHeaderText ($saftyHdr3 -replace "#define CONFIG_STORE_FORMAT_VERSION_V1 1u`n", "") -SourceText $saftySrc) $false "no CONFIG_STORE_FORMAT_VERSION_V1 macro.*skipped" "RP2040: a missing V1 macro (skipped version) is caught."
Test-ChainCase 31 (Test-SaftyConfigStoreMigrationStep -VersionHeaderText $saftyHdr3 -SourceText "if (version == CONFIG_STORE_FORMAT_VERSION_V2) { }`n") $false "CONFIG_STORE_FORMAT_VERSION_V1 is defined but.*orphaned macro" "RP2040: an older macro with no branch is caught as orphaned."
Test-ChainCase 32 (Test-SaftyConfigStoreMigrationStep -VersionHeaderText ($saftyHdr3 + "#define CONFIG_STORE_FORMAT_VERSION_V3 3u`n") -SourceText ($saftySrc + "if (version == CONFIG_STORE_FORMAT_VERSION_V3) { }`n")) $false "V3 is defined but CONFIG_STORE_FORMAT_VERSION is only 3" "RP2040: a macro at or above the current version is caught."

Test-ChainCase 33 (Test-KilnCfgStoreMigrationStep -VersionHeaderText $kilnHdr3 -SourceText ($kStep12 + $kStep23 + "static void migrate_store_v3_to_v4(const c *s, d *d) { }`n")) $false "last migrate_store_\* step ends at v4" "kiln-config: a last step ending past KILN_CFG_STORE_VERSION is caught."
Test-ChainCase 34 (Test-KilnCfgStoreMigrationStep -VersionHeaderText $kilnHdr3 -SourceText ($kStep12 + $kStep23 + $kStep23)) $false "migrate_store_v2_to_v3 is defined more than once" "kiln-config: a duplicate step definition is caught."
Test-ChainCase 35 (Test-KilnCfgStoreMigrationStep -VersionHeaderText $kilnHdr3 -SourceText ("static void migrate_store_v2_to_v3(const b *s, c *d);`n" + $kStep12 + $kStep23)) $true "" "kiln-config: a forward declaration plus its definition is not a false duplicate."
$kCommented23 = "// static void migrate_store_v2_to_v3(const b *s, c *d) { }`n"
Test-ChainCase 36 (Test-KilnCfgStoreMigrationStep -VersionHeaderText $kilnHdr3 -SourceText ($kStep12 + $kCommented23)) $false "no migrate_store_v2_to_v3" "kiln-config: a commented-out current step does not satisfy the existence rule."
Test-ChainCase 37 (Test-KilnCfgStoreMigrationStep -VersionHeaderText $kilnHdr3 -SourceText ("/* static void migrate_store_v1_to_v2(const a *s, b *d) { } */`n" + $kCommented23)) $false "no migrate_store_\* step function is defined at all" "kiln-config: only commented-out steps at version 3 fail instead of passing vacuously."
Test-ChainCase 38 (Test-SaftyConfigStoreMigrationStep -VersionHeaderText ($saftyHdr3 -replace "VERSION_V2 2u", "VERSION_V2 5u") -SourceText $saftySrc) $false "CONFIG_STORE_FORMAT_VERSION_V2 has value 5, expected 2" "RP2040: a macro whose value differs from its N is caught."
Test-ChainCase 39 (Test-SaftyConfigStoreMigrationStep -VersionHeaderText $saftyHdr3 -SourceText "// if (version == CONFIG_STORE_FORMAT_VERSION_V2) { }`nif (version == CONFIG_STORE_FORMAT_VERSION_V1) { }`n") $false "CONFIG_STORE_FORMAT_VERSION_V2 is defined but.*orphaned macro" "RP2040: a commented-out branch for the immediately preceding version does not satisfy the rule."
Test-ChainCase 40 (Test-SaftyConfigStoreMigrationStep -VersionHeaderText $saftyHdr3 -SourceText "if (version == CONFIG_STORE_FORMAT_VERSION_V2) { }`n/* if (version == CONFIG_STORE_FORMAT_VERSION_V1) { } */`n") $false "CONFIG_STORE_FORMAT_VERSION_V1 is defined but.*orphaned macro" "RP2040: a commented-out branch for an older version does not satisfy the rule."

# ---------------------------------------------------------------------
# Assertions 41-61: plan sec 5.1 follow-up rules (2026-10-08): D1 one NEW
# step per bump vs a baseline, fixture-must-be-referenced, D2 expiry floor.
# ---------------------------------------------------------------------
$kBase = Get-StoreStepModel -Store kiln -VersionHeaderText $kilnHdr3 -SourceText ($kStep12 + $kStep23)
$kHdr4 = "#define KILN_CFG_STORE_VERSION 4`n"
$kStep34 = "static void migrate_store_v3_to_v4(const c *s, d *d) { }`n"
$kCur4 = Get-StoreStepModel -Store kiln -VersionHeaderText $kHdr4 -SourceText ($kStep12 + $kStep23 + $kStep34)
Test-ChainCase 41 (Test-NewStepPerBump -StoreName "kiln-config" -Baseline $kBase -Current $kCur4) $true "" "D1 kiln: bump 3->4 with exactly one new step passes."
$kCur4None = Get-StoreStepModel -Store kiln -VersionHeaderText $kHdr4 -SourceText ($kStep12 + $kStep23)
Test-ChainCase 42 (Test-NewStepPerBump -StoreName "kiln-config" -Baseline $kBase -Current $kCur4None) $false "bump to v4 must add exactly one NEW step, found 0" "D1 kiln: bump with no new step is caught."
$kCur4Two = Get-StoreStepModel -Store kiln -VersionHeaderText $kHdr4 -SourceText ($kStep12 + $kStep23 + $kStep34 + "static void migrate_store_v4_to_v5(const d *s, e *d) { }`n")
Test-ChainCase 43 (Test-NewStepPerBump -StoreName "kiln-config" -Baseline $kBase -Current $kCur4Two) $false "new step for v5 was added but the version did not bump" "D1 kiln: an extra early step beyond the bump is caught."
$kSame = Get-StoreStepModel -Store kiln -VersionHeaderText $kilnHdr3 -SourceText ($kStep12 + $kStep23)
Test-ChainCase 44 (Test-NewStepPerBump -StoreName "kiln-config" -Baseline $kBase -Current $kSame) $true "" "D1 kiln: unchanged version with no new step passes."
$kDown = Get-StoreStepModel -Store kiln -VersionHeaderText "#define KILN_CFG_STORE_VERSION 2`n" -SourceText $kStep12
Test-ChainCase 45 (Test-NewStepPerBump -StoreName "kiln-config" -Baseline $kBase -Current $kDown) $false "went DOWN" "D1 kiln: a version downgrade is caught."

$pBase = Get-StoreStepModel -Store profiles -VersionHeaderText $profHdr4 -SourceText ($profBase + $profV3)
$pHdr5 = "#define PROFILE_VERSION 5`n"
$pCur5 = Get-StoreStepModel -Store profiles -VersionHeaderText $pHdr5 -SourceText ($profBase + $profV3 + "static void convert_profile_v4(const x *s, profile_t *o) { }`n")
Test-ChainCase 46 (Test-NewStepPerBump -StoreName "fire profiles" -Baseline $pBase -Current $pCur5) $true "" "D1 profiles: bump 4->5 adding convert_profile_v4 passes."
$pCur5None = Get-StoreStepModel -Store profiles -VersionHeaderText $pHdr5 -SourceText ($profBase + $profV3)
Test-ChainCase 47 (Test-NewStepPerBump -StoreName "fire profiles" -Baseline $pBase -Current $pCur5None) $false "bump to v5 must add exactly one NEW step, found 0" "D1 profiles: bump with no new converter is caught."

$sBase = Get-StoreStepModel -Store safty -VersionHeaderText $saftyHdr3 -SourceText $saftySrc
$sHdr4 = ($saftyHdr3 -replace "VERSION 3u", "VERSION 4u")
$sCur4 = Get-StoreStepModel -Store safty -VersionHeaderText ($sHdr4 + "#define CONFIG_STORE_FORMAT_VERSION_V3 3u`n") -SourceText $saftySrc
Test-ChainCase 48 (Test-NewStepPerBump -StoreName "RP2040" -Baseline $sBase -Current $sCur4) $true "" "D1 RP2040: bump 3->4 adding the V3 macro passes."
$sCur4None = Get-StoreStepModel -Store safty -VersionHeaderText $sHdr4 -SourceText $saftySrc
Test-ChainCase 49 (Test-NewStepPerBump -StoreName "RP2040" -Baseline $sBase -Current $sCur4None) $false "bump to v4 must add exactly one NEW step, found 0" "D1 RP2040: bump with no new format macro is caught."

$zBase = Get-StoreStepModel -Store zones -VersionHeaderText "#define ZONES_CFG_VERSION 26`n" -SourceText "int x;`n"
$zCur27 = Get-StoreStepModel -Store zones -VersionHeaderText "#define ZONES_CFG_VERSION 27`n" -SourceText "void zones_cfg_step_v26_to_v27(const a *s, b *d);`n"
Test-ChainCase 50 (Test-NewStepPerBump -StoreName "zones" -Baseline $zBase -Current $zCur27 -MinToVersion 27) $true "" "D1 zones: bump 26->27 with the first step passes."
$zCur27None = Get-StoreStepModel -Store zones -VersionHeaderText "#define ZONES_CFG_VERSION 27`n" -SourceText "int x;`n"
Test-ChainCase 51 (Test-NewStepPerBump -StoreName "zones" -Baseline $zBase -Current $zCur27None -MinToVersion 27) $false "bump to v27 must add exactly one NEW step, found 0" "D1 zones: bump past the tail with no step is caught."

$fxNames = @("cfg_blobs/kiln_cfg_v3.bin", "x.c")
Test-ChainCase 52 (Test-FixtureRule -StoreName "kiln-config" -Prefix kiln_cfg -CurrentVersion 4 -RequireForBump $true -TestTreeFileNames $fxNames -TestTreeFileContents @("", "reads kiln_cfg_v3.bin")) $true "" "Fixture: present and referenced on a bump passes."
Test-ChainCase 53 (Test-FixtureRule -StoreName "kiln-config" -Prefix kiln_cfg -CurrentVersion 4 -RequireForBump $true -TestTreeFileNames @("x.c") -TestTreeFileContents @("")) $false "fixture cfg_blobs/kiln_cfg_v3.bin is missing" "Fixture: a bump without its fixture is caught."
Test-ChainCase 54 (Test-FixtureRule -StoreName "kiln-config" -Prefix kiln_cfg -CurrentVersion 4 -RequireForBump $true -TestTreeFileNames $fxNames -TestTreeFileContents @("", "nothing")) $false "kiln_cfg_v3.bin is not referenced" "Fixture: an unreferenced fixture is caught."
Test-ChainCase 55 (Test-FixtureRule -StoreName "kiln-config" -Prefix kiln_cfg -CurrentVersion 3 -RequireForBump $false -TestTreeFileNames @("x.c") -TestTreeFileContents @("")) $true "" "Fixture: no bump and no fixtures passes."
Test-ChainCase 56 (Test-FixtureRule -StoreName "zones" -Prefix zones -CurrentVersion 26 -RequireForBump $false -TestTreeFileNames @("cfg_blobs/zones_v25.bin") -TestTreeFileContents @("")) $false "zones_v25.bin is not referenced" "Fixture: an orphan fixture is caught even without a bump."

$d2Ok = @{ Version = 12; Steps = @(5, 6, 7, 8, 9, 10, 11, 12) }
Test-ChainCase 57 (Test-ExpiryFloor -StoreName "kiln-config" -Model $d2Ok) $true "" "D2: oldest handled v4 at version 12 (floor 4) passes."
$d2Bad = @{ Version = 13; Steps = @(5, 6, 7, 8, 9, 10, 11, 12, 13) }
Test-ChainCase 58 (Test-ExpiryFloor -StoreName "kiln-config" -Model $d2Bad) $false "older than the expiry floor v5" "D2: a step older than CURRENT-8 is caught."
$d2Prof = Get-StoreStepModel -Store profiles -VersionHeaderText "#define PROFILE_VERSION 12`n" -SourceText "static void convert_profile_v1(const a *s, profile_t *o) { }`n"
Test-ChainCase 59 (Test-ExpiryFloor -StoreName "fire profiles" -Model $d2Prof) $false "still migrates from v1" "D2: an expired profile converter is caught."
Test-ChainCase 60 (Test-ExpiryFloor -StoreName "RP2040" -Model $sBase) $true "" "D2: the real-shaped RP2040 model passes."

# 61: the real sources must pass the whole production script (all new rules
# run). Baseline HEAD so the test does not depend on origin being fetched.
$env:KILNCTL_MIGCHK_BASELINE = "HEAD"
$realOut = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "..\..\..\..\tools\check_config_migration_steps.ps1") 2>&1
$realExit = $LASTEXITCODE
Remove-Item Env:\KILNCTL_MIGCHK_BASELINE -ErrorAction SilentlyContinue
if ($realExit -ne 0) { $failures += "Assertion 61 FAILED: real sources vs baseline HEAD did not PASS: $($realOut -join ' ')" }
else { Write-Host "Assertion 61 OK: real sources PASS the new D1/fixture/D2 rules." }

if ($failures.Count -gt 0) {
    Write-Host ""
    Write-Host "test_check_config_migration_steps: $($failures.Count) assertion(s) FAILED:" -ForegroundColor Red
    foreach ($f in $failures) { Write-Host "  - $f" -ForegroundColor Red }
    exit 1
}

Write-Host ""
Write-Host "test_check_config_migration_steps: all 61 assertions passed." -ForegroundColor Green
exit 0
