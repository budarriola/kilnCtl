# check_config_migration_steps.ps1 -- mechanical enforcement for
# docs/CONFIG_MIGRATION_CHAIN_PLAN.md section 5, "a config version bump must
# fail the build if it does not bring its step and its test."
#
# SCOPE: docs/CONFIG_MIGRATION_CHAIN_PLAN.md section 0.1 governs four
# stores (ESP zones config, ESP kiln-config slots, ESP fire profiles,
# RP2040 safety config). This script enforces all four, but NOT to the same
# depth -- each store's on-disk migration shape is different, and forcing
# an identical rule set onto a shape it doesn't fit would either be vacuous
# or require an unrelated refactor. See section 5 of the plan for exactly
# which rules apply to which store and why.
#
#   - ESP zones config (Test-ZonesMigrationSteps): the FULL D1 ("exactly
#     one step") + D2 (expiry floor) + frozen-input-assert + fixture rule
#     set, unchanged from the pass that landed this script. Still the sole
#     store with an empty, D1-shaped step table today (ZONES_CFG_VERSION
#     26, the monolithic pre-v26 tail handles everything, no
#     zones_cfg_step_* function exists yet).
#   - ESP kiln-config slots (Test-KilnCfgStoreMigrationStep): already
#     carries two pre-D1 GRANDFATHERED steps (migrate_store_v1_to_v2,
#     migrate_store_v2_to_v3) that a naive "exactly one total" rule would
#     immediately flag (the plan's own stated risk). Enforced instead: a
#     migrate_store_v<CURRENT-1>_to_v<CURRENT>(...) function must exist for
#     the CURRENT version. Frozen-struct/fixture/D2 rules are not enforced
#     here -- this store has no crc32-last-field or expiry-floor convention
#     of its own to check against.
#   - ESP fire profiles (Test-ProfilesMigrationStep): converters are named
#     convert_profile_v<N>(...) and convert DIRECTLY from historical
#     version N to current, not N -> N+1 -- a monolithic-tail shape, like
#     zones' pre-v26 converter, rather than a chain. Enforced: a
#     convert_profile_v<CURRENT-1>(...) converter must exist.
#   - RP2040 safety config (Test-SaftyConfigStoreMigrationStep): has no
#     per-transition function at all -- migration is inline `if (version ==
#     CONFIG_STORE_FORMAT_VERSION_V<N>)` branches in
#     config_store_unpack_ex(). Enforced: a CONFIG_STORE_FORMAT_VERSION_V
#     <CURRENT-1> macro must be defined and actually branched on.
#
# What is deliberately NOT enforced for the latter three stores -- D1's
# "exactly one NEW step per bump" defect-catching rule, the frozen-input
# _Static_assert/crc32-last-field discipline, the captured-fixture-must-be-
# referenced rule, and D2's expiry floor -- is follow-up work, not silently
# assumed done; see docs/CONFIG_MIGRATION_CHAIN_PLAN.md section 5 for why
# each is deferred rather than faked against a shape it doesn't fit.
#
# WHAT THIS CHECKS, against firmware/KilnFW/App/drivers/persist/
# zones_config_json.h (ZONES_CFG_VERSION) and zones_config_migrate.c (steps,
# the pre-v26 monolithic tail, and the expiry floor):
#
#   1. If ZONES_CFG_VERSION > $ZonesTailMax (26, the tail's fixed upper
#      bound), a step function `zones_cfg_step_v<N-1>_to_v<N>` must exist
#      whose <N> is the CURRENT version -- unless N-1 is still inside the
#      tail's own covered range, per the plan's own "input floor is v26"
#      rule (section 1's worked example only ever fires past 26).
#   2. Exactly one such step function exists -- a second is a defect under
#      D1, not a bonus (plan section 5, rule 2).
#   3. The step's frozen input type (zones_cfg_v<N-1>_t) is declared with a
#      `_Static_assert` pinning its `sizeof`, and `crc32` is its last field
#      (checked structurally, not just present anywhere in the type).
#   4. A captured fixture blob (`cfg_blobs/zones_v<N-1>.bin`) exists under
#      firmware/KilnFW/App/test/ and its filename is read by some test
#      source under that same tree, so it cannot sit unread (the failure
#      mode check_no_orphaned_checks.ps1 guards against for check/test
#      files generally, applied here to fixture blobs specifically).
#   5. An expiry floor constant (`ZONES_CFG_MIGRATION_EXPIRY_FLOOR`) exists
#      once a step is required, equals `ZONES_CFG_VERSION - 8` (plan section
#      4.2's proposed policy), and the tail's own oldest handled `case N:`
#      is not older than that floor.
#
# When ZONES_CFG_VERSION is still within the tail's range (today: 26), rules
# 1/2/5 instead assert the NEGATIVE -- no zones_cfg_step_* function and no
# expiry-floor constant exist yet -- so a step added early (skipping the
# monolithic tail before it is actually exhausted) is caught too, not just a
# step that never lands.
#
# NEGATIVE-TESTED, per this script's own header requirement (plan section
# 5's last paragraph: "bump the version in a scratch tree ... and confirm a
# red run"): a scratch copy of zones_config_json.h/zones_config_migrate.c is
# used to prove this script recognizes both an unmet requirement (a bumped
# version with no step) and a badly-shaped step (missing frozen-input
# assert, missing fixture, wrong expiry floor). Never mutates the real repo
# tree. See firmware/KilnFW/App/test/test_check_config_migration_steps.ps1.

$ErrorActionPreference = "Stop"

function Test-KilnCfgStoreMigrationStep {
    <#
      docs/CONFIG_MIGRATION_CHAIN_PLAN.md sec 5 follow-up: the ESP
      kiln-config slot store. Unlike zones this store already carries two
      pre-D1 GRANDFATHERED steps (migrate_store_v1_to_v2, migrate_store_v2_
      to_v3) -- the plan explicitly warns a naive "exactly one step total"
      rule would immediately flag that history. So this check enforces only
      the part of D1 that generalizes cleanly regardless of how many old
      steps already exist: a step function named
      migrate_store_v<CURRENT-1>_to_v<CURRENT>(...) must exist in
      kiln_cfg_store.c for the CURRENT version. Older steps are not
      re-inspected -- same "the tail keeps whatever coverage it has"
      posture the plan sec 3 states for zones' pre-v26 converter.

      Returns @{ Ok; Failures }.
    #>
    param(
        [Parameter(Mandatory = $true)][string]$VersionHeaderText,
        [Parameter(Mandatory = $true)][string]$SourceText
    )
    $failures = New-Object System.Collections.Generic.List[string]
    $verMatch = [regex]::Match($VersionHeaderText, '#define\s+KILN_CFG_STORE_VERSION\s+(\d+)')
    if (-not $verMatch.Success) {
        $failures.Add("kiln-config slot store: could not find '#define KILN_CFG_STORE_VERSION <N>'")
        return @{ Ok = $false; Failures = $failures }
    }
    $current = [int]$verMatch.Groups[1].Value
    $expectedFrom = $current - 1
    $stepPattern = "migrate_store_v${expectedFrom}_to_v${current}\s*\("
    if ($SourceText -notmatch $stepPattern) {
        $failures.Add("kiln-config slot store: KILN_CFG_STORE_VERSION is $current but no " +
            "migrate_store_v${expectedFrom}_to_v${current}(...) step function exists in kiln_cfg_store.c")
    }
    return @{ Ok = ($failures.Count -eq 0); Failures = $failures }
}

function Test-ProfilesMigrationStep {
    <#
      docs/CONFIG_MIGRATION_CHAIN_PLAN.md sec 5 follow-up: the ESP fire
      profiles store. Its converters are named convert_profile_v<N>(...) and
      each converts DIRECTLY from historical version N to the current
      in-memory profile_t (not N -> N+1) -- a "monolithic tail of typed
      converters" shape, closer to zones' pre-v26 switch-based tail than to
      a chained step. Enforces only that the converter for the immediately
      preceding version exists, which is the part of D1 ("the nearest prior
      version must be consumable, freshly, by the current release") this
      shape can express without a redesign.

      Returns @{ Ok; Failures }.
    #>
    param(
        [Parameter(Mandatory = $true)][string]$VersionHeaderText,
        [Parameter(Mandatory = $true)][string]$SourceText
    )
    $failures = New-Object System.Collections.Generic.List[string]
    $verMatch = [regex]::Match($VersionHeaderText, '#define\s+PROFILE_VERSION\s+(\d+)')
    if (-not $verMatch.Success) {
        $failures.Add("fire profiles store: could not find '#define PROFILE_VERSION <N>'")
        return @{ Ok = $false; Failures = $failures }
    }
    $current = [int]$verMatch.Groups[1].Value
    $expectedFrom = $current - 1
    $stepPattern = "convert_profile_v${expectedFrom}\s*\("
    if ($SourceText -notmatch $stepPattern) {
        $failures.Add("fire profiles store: PROFILE_VERSION is $current but no " +
            "convert_profile_v${expectedFrom}(...) converter exists in profiles_http.c")
    }
    return @{ Ok = ($failures.Count -eq 0); Failures = $failures }
}

function Test-SaftyConfigStoreMigrationStep {
    <#
      docs/CONFIG_MIGRATION_CHAIN_PLAN.md sec 5 follow-up: the RP2040 safety
      config store. It has no per-transition function at all -- migration is
      a couple of inline `if (version == CONFIG_STORE_FORMAT_VERSION_V<N>)`
      branches in config_store_unpack_ex() -- so what this checks is the
      store's own actual convention: a named
      CONFIG_STORE_FORMAT_VERSION_V<CURRENT-1> macro must exist (naming the
      immediately preceding format this build still knows how to read), and
      config_store.c must actually branch on it (so the macro cannot be
      declared and then silently orphaned).

      Returns @{ Ok; Failures }.
    #>
    param(
        [Parameter(Mandatory = $true)][string]$VersionHeaderText,
        [Parameter(Mandatory = $true)][string]$SourceText
    )
    $failures = New-Object System.Collections.Generic.List[string]
    $verMatch = [regex]::Match($VersionHeaderText, '#define\s+CONFIG_STORE_FORMAT_VERSION\s+(\d+)u?')
    if (-not $verMatch.Success) {
        $failures.Add("RP2040 safety config store: could not find '#define CONFIG_STORE_FORMAT_VERSION <N>'")
        return @{ Ok = $false; Failures = $failures }
    }
    $current = [int]$verMatch.Groups[1].Value
    $expectedFrom = $current - 1
    $macroName = "CONFIG_STORE_FORMAT_VERSION_V${expectedFrom}"
    $macroPattern = "#define\s+${macroName}\s+\d+u?"
    if ($VersionHeaderText -notmatch $macroPattern) {
        $failures.Add("RP2040 safety config store: CONFIG_STORE_FORMAT_VERSION is $current but no " +
            "$macroName macro naming the immediately preceding format is defined in config_store.h")
    } else {
        $branchPattern = "==\s*${macroName}\b"
        if ($SourceText -notmatch $branchPattern -and $VersionHeaderText -notmatch $branchPattern) {
            $failures.Add("RP2040 safety config store: $macroName is defined but config_store.c has no " +
                "'== $macroName' branch actually handling it -- an orphaned macro")
        }
    }
    return @{ Ok = ($failures.Count -eq 0); Failures = $failures }
}

function Test-ZonesMigrationSteps {
    <#
      Pure function over explicit file contents, so both the real check
      below and the scratch-tree negative test in
      firmware/KilnFW/App/test/test_check_config_migration_steps.ps1 can call the identical
      logic against different input text -- the negative test must exercise
      THIS function, not a reimplementation of it.

      Returns a hashtable: @{ Ok = [bool]; Failures = [string[]] }
    #>
    param(
        [Parameter(Mandatory = $true)][string]$VersionHeaderText,
        [Parameter(Mandatory = $true)][string]$MigrateFileText,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][string[]]$TestTreeFileNames,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][AllowEmptyString()][string[]]$TestTreeFileContents,
        [int]$ZonesTailMax = 26
    )

    $failures = New-Object System.Collections.Generic.List[string]

    $verMatch = [regex]::Match($VersionHeaderText, '#define\s+ZONES_CFG_VERSION\s+(\d+)')
    if (-not $verMatch.Success) {
        $failures.Add("could not find '#define ZONES_CFG_VERSION <N>' in the version header")
        return @{ Ok = $false; Failures = $failures }
    }
    $current = [int]$verMatch.Groups[1].Value

    # Every zones_cfg_step_vA_to_vB(...) function declared or defined in the
    # migrate file.
    $stepMatches = [regex]::Matches($MigrateFileText, 'zones_cfg_step_v(\d+)_to_v(\d+)\s*\(')
    $stepPairs = @()
    foreach ($m in $stepMatches) {
        $stepPairs += , @([int]$m.Groups[1].Value, [int]$m.Groups[2].Value)
    }
    # De-duplicate (a function has both a forward declaration and a
    # definition in real code; that is one step, not two).
    $uniqueSteps = $stepPairs | Sort-Object -Unique -Property { "$($_[0])_$($_[1])" }
    # PowerShell's Sort-Object -Unique on arrays-of-arrays does not dedupe
    # reliably across ISE/host versions -- do it by string key instead.
    $seen = @{}
    $uniqueSteps = @()
    foreach ($p in $stepPairs) {
        $key = "$($p[0])_$($p[1])"
        if (-not $seen.ContainsKey($key)) {
            $seen[$key] = $true
            $uniqueSteps += , $p
        }
    }

    $needsStep = ($current -gt $ZonesTailMax) -and (($current - 1) -ge $ZonesTailMax)

    if ($needsStep) {
        $expectedFrom = $current - 1
        $expectedTo = $current
        $matchingStep = $uniqueSteps | Where-Object { $_[0] -eq $expectedFrom -and $_[1] -eq $expectedTo }

        if (-not $matchingStep) {
            $failures.Add("ZONES_CFG_VERSION is $current (> tail max $ZonesTailMax) but no " +
                "zones_cfg_step_v${expectedFrom}_to_v${expectedTo}(...) step function exists")
        }
        if ($uniqueSteps.Count -ne 1) {
            $failures.Add("expected exactly one zones_cfg_step_* function (D1), found $($uniqueSteps.Count): " +
                (($uniqueSteps | ForEach-Object { "v$($_[0])_to_v$($_[1])" }) -join ", "))
        }

        if ($matchingStep) {
            # Rule 3: frozen input type's sizeof is pinned, and crc32 is its
            # last field.
            $inputType = "zones_cfg_v${expectedFrom}_t"
            $sizeofAssert = [regex]::Match($MigrateFileText + $VersionHeaderText,
                "_Static_assert\s*\(\s*sizeof\s*\(\s*$inputType\s*\)")
            if (-not $sizeofAssert.Success) {
                $failures.Add("no _Static_assert pinning sizeof($inputType) -- the frozen input type must be sized")
            }

            $structBody = [regex]::Match($MigrateFileText + $VersionHeaderText,
                "typedef\s+struct\s*\{(?<body>(?:[^{}]|\{[^{}]*\})*)\}\s*$inputType\s*;", "Singleline")
            if (-not $structBody.Success) {
                $failures.Add("could not find 'typedef struct { ... } $inputType;' to check crc32 field position")
            } else {
                $bodyLines = $structBody.Groups["body"].Value -split "`n" |
                    Where-Object { $_.Trim() -ne "" -and -not ($_.Trim().StartsWith("/*")) -and -not ($_.Trim().StartsWith("//")) -and -not ($_.Trim().StartsWith("*")) }
                $lastMemberLine = $bodyLines | Select-Object -Last 1
                if (-not $lastMemberLine -or $lastMemberLine -notmatch '\bcrc32\b') {
                    $failures.Add("$inputType's last field is not crc32 (found: '$lastMemberLine')")
                }
            }

            # Rule 4: fixture blob exists and is read by some test source.
            $fixtureName = "zones_v${expectedFrom}.bin"
            $fixturePresent = $TestTreeFileNames -contains "cfg_blobs/$fixtureName" -or
                $TestTreeFileNames -contains "cfg_blobs\$fixtureName"
            if (-not $fixturePresent) {
                $failures.Add("no fixture cfg_blobs/$fixtureName found under the test tree")
            }
            $referenced = $false
            for ($i = 0; $i -lt $TestTreeFileContents.Count; $i++) {
                if ($TestTreeFileContents[$i] -match [regex]::Escape($fixtureName)) {
                    $referenced = $true
                    break
                }
            }
            if (-not $referenced) {
                $failures.Add("fixture $fixtureName is not referenced by name in any test source -- it would sit unread")
            }
        }

        # Rule 5: expiry floor.
        $floorMatch = [regex]::Match($VersionHeaderText + $MigrateFileText,
            '#define\s+ZONES_CFG_MIGRATION_EXPIRY_FLOOR\s+\(?\s*ZONES_CFG_VERSION\s*-\s*(\d+)\s*\)?')
        if (-not $floorMatch.Success) {
            $failures.Add("no ZONES_CFG_MIGRATION_EXPIRY_FLOOR constant defined once a step is required (plan sec 4.2: ZONES_CFG_VERSION - 8)")
        } else {
            $offset = [int]$floorMatch.Groups[1].Value
            if ($offset -ne 8) {
                $failures.Add("ZONES_CFG_MIGRATION_EXPIRY_FLOOR is ZONES_CFG_VERSION - $offset, plan sec 4.2 specifies ZONES_CFG_VERSION - 8")
            }
            $floorValue = $current - $offset
            $caseMatches = [regex]::Matches($MigrateFileText, '(?m)^\s*case\s+(\d+)\s*:')
            if ($caseMatches.Count -gt 0) {
                $oldestCase = ($caseMatches | ForEach-Object { [int]$_.Groups[1].Value } | Measure-Object -Minimum).Minimum
                if ($oldestCase -lt $floorValue) {
                    $failures.Add("tail's oldest handled case ($oldestCase) is older than the expiry floor ($floorValue) -- delete cases below the floor (plan sec 4.2)")
                }
            }
        }
    } else {
        # Still inside the tail's range: nothing should have jumped ahead.
        if ($uniqueSteps.Count -ne 0) {
            $failures.Add("ZONES_CFG_VERSION is $current (<= tail max $ZonesTailMax) but $($uniqueSteps.Count) zones_cfg_step_* function(s) already exist -- the tail has not been exhausted yet")
        }
        $floorMatch = [regex]::Match($VersionHeaderText + $MigrateFileText, '#define\s+ZONES_CFG_MIGRATION_EXPIRY_FLOOR')
        if ($floorMatch.Success) {
            $failures.Add("ZONES_CFG_MIGRATION_EXPIRY_FLOOR is defined before any step exists -- premature (plan sec 4.2 only applies once the tail is being trimmed)")
        }
    }

    return @{ Ok = ($failures.Count -eq 0); Failures = $failures }
}

# ---------------------------------------------------------------------------
# Real-tree run starts here. Everything above is reusable by the negative
# test (dot-sourced), same pattern as check_hal_include_boundary.ps1 /
# check_route_tier_coverage.ps1.
# ---------------------------------------------------------------------------

if ($MyInvocation.InvocationName -eq '.') {
    # Dot-sourced by the negative test -- define the function only, run
    # nothing else.
    return
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$versionHeader = Join-Path $repoRoot "firmware\KilnFW\App\drivers\persist\zones_config_json.h"
$migrateFile = Join-Path $repoRoot "firmware\KilnFW\App\drivers\persist\zones_config_migrate.c"
$testTreeRoot = Join-Path $repoRoot "firmware\KilnFW\App\test"

$kilnCfgVersionHeader = Join-Path $repoRoot "firmware\KilnFW\App\drivers\persist\kiln_cfg_store_internal.h"
$kilnCfgSource = Join-Path $repoRoot "firmware\KilnFW\App\drivers\persist\kiln_cfg_store.c"

$profilesSource = Join-Path $repoRoot "firmware\KilnFW\App\drivers\http\profiles_http.c"

$saftyVersionHeader = Join-Path $repoRoot "firmware\SaftyFW\src\config_store.h"
$saftySource = Join-Path $repoRoot "firmware\SaftyFW\src\config_store.c"

foreach ($p in @($versionHeader, $migrateFile, $testTreeRoot, $kilnCfgVersionHeader, $kilnCfgSource,
        $profilesSource, $saftyVersionHeader, $saftySource)) {
    if (-not (Test-Path $p)) {
        Write-Host "check_config_migration_steps: FAIL -- expected path not found: $p"
        exit 1
    }
}

$versionHeaderText = Get-Content -Raw $versionHeader
$migrateFileText = Get-Content -Raw $migrateFile

$testFiles = Get-ChildItem -Path $testTreeRoot -Recurse -File
$testTreeFileNames = @()
$testTreeFileContents = @()
foreach ($f in $testFiles) {
    $rel = $f.FullName.Substring($testTreeRoot.Length + 1) -replace '\\', '/'
    $testTreeFileNames += $rel
    # Only bother reading small text-ish sources for the "referenced by
    # name" check -- skip the fixture blobs themselves and anything
    # large/binary.
    if ($f.Extension -in @(".c", ".h", ".py", ".ps1", ".json")) {
        $testTreeFileContents += (Get-Content -Raw $f.FullName -ErrorAction SilentlyContinue)
    } else {
        $testTreeFileContents += ""
    }
}

$zonesResult = Test-ZonesMigrationSteps -VersionHeaderText $versionHeaderText `
    -MigrateFileText $migrateFileText `
    -TestTreeFileNames $testTreeFileNames `
    -TestTreeFileContents $testTreeFileContents

$kilnCfgResult = Test-KilnCfgStoreMigrationStep -VersionHeaderText (Get-Content -Raw $kilnCfgVersionHeader) `
    -SourceText (Get-Content -Raw $kilnCfgSource)

$profilesSourceText = Get-Content -Raw $profilesSource
$profilesResult = Test-ProfilesMigrationStep -VersionHeaderText $profilesSourceText `
    -SourceText $profilesSourceText

$saftyResult = Test-SaftyConfigStoreMigrationStep -VersionHeaderText (Get-Content -Raw $saftyVersionHeader) `
    -SourceText (Get-Content -Raw $saftySource)

$allFailures = @()
$allFailures += $zonesResult.Failures
$allFailures += $kilnCfgResult.Failures
$allFailures += $profilesResult.Failures
$allFailures += $saftyResult.Failures

if ($allFailures.Count -gt 0) {
    Write-Host ""
    Write-Host "FAILED: check_config_migration_steps found $($allFailures.Count) problem(s)" -ForegroundColor Red
    Write-Host "        across the governed config stores (docs/CONFIG_MIGRATION_CHAIN_PLAN.md sec 5):" -ForegroundColor Red
    foreach ($f in $allFailures) {
        Write-Host "  - $f" -ForegroundColor Red
    }
    Write-Host ""
    exit 1
}

Write-Host ("check_config_migration_steps: PASS -- zones (full D1/D2 rule set, still within the tail), " +
    "kiln-config slots, fire profiles, and RP2040 safety config (existence-of-current-step rule) all satisfied")
exit 0
