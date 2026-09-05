<#
.SYNOPSIS
  Prepares (and, with -Apply, executes) the KilnFW App/drivers/ layering reorg
  described in docs/HW_ABSTRACTION_PLAN.md ("drivers/ layering (KilnFW only)").

.DESCRIPTION
  Reads tools/drivers_reorg/mapping.csv (old_path,new_path,rationale) and:
    1. Emits `git mv` for every row (skipped/reported only in -DryRun, the default).
    2. Rewrites literal SRCS paths in
       firmware/KilnFW/App/drivers/CMakeLists.txt and firmware/KilnFW/App/CMakeLists.txt.
    3. Reports whether bare #include "x.h" can be kept (INCLUDE_DIRS listing all
       ten new layer dirs) instead of path-qualifying every include.
    4. Lists every check_*.ps1 / *.py under tools/ and firmware/KilnFW/tools, and every
       host-test build script/stub under firmware/KilnFW/App/test, that contains the
       literal "drivers/" or a filename that appears in mapping.csv -- these are the
       path-keyed sites that must be rewritten in the same commit as the move
       (CLAUDE.md: "splits break filename-keyed checks").
    5. Verifies include direction: ui/http/bridge -> control/safety/persist/net ->
       owners/hw/sim, and reports any #include that points the wrong way (upward).

  This script performs NO firmware file moves or edits by default. Only -Apply
  executes `git mv` and rewrites CMakeLists.txt. Coordinator must review the
  22 AMBIGUOUS rows in mapping.csv (and the 4 BUILD_META rows, which have no
  single-layer home and are NOT git-mv'd even under -Apply) before ever
  passing -Apply.

.PARAMETER DryRun
  Default. Report only; no filesystem/git changes.

.PARAMETER Apply
  Execute the git mv / CMakeLists rewrites. NOT invoked as part of this
  preparation task -- left here for a later, separate pass.
#>

[CmdletBinding()]
param(
    [switch]$Apply,
    [switch]$DryRun = $true
)

if ($Apply) { $DryRun = $false }

$RepoRoot = (git rev-parse --show-toplevel).Trim()
Set-Location $RepoRoot

$MappingCsv   = Join-Path $RepoRoot "tools/drivers_reorg/mapping.csv"
$DriversCmake = Join-Path $RepoRoot "firmware/KilnFW/App/drivers/CMakeLists.txt"
$AppCmake     = Join-Path $RepoRoot "firmware/KilnFW/App/CMakeLists.txt"

if (-not (Test-Path $MappingCsv)) {
    Write-Error "Missing $MappingCsv"
    exit 1
}

$rows = Import-Csv $MappingCsv

# Layer tiers per the plan's allowed include direction (decision A: layers
# are subdirectories of the single `drivers` component,
# firmware/KilnFW/App/drivers/<layer>/, not sibling App/<layer> components --
# see HW_ABSTRACTION_PLAN.md's reorg section for the rationale).
#   ui/http/bridge (tier 0, top)
#     -> control/safety/persist/net (tier 1, mid)
#       -> owners/hw/sim (tier 2, bottom -- decision D REVERTED: sim is a
#         hardware substitute, same tier as the real hw drivers it stands in
#         for, not a top-tier orchestrator; control legitimately consumes
#         sim_backend.h the same way it consumes a real driver header)
#         -> common (tier 3, bottom-most -- decision B: pure leaf
#           headers/utilities with no includes above this tier)
$tierOf = @{
    "ui" = 0; "http" = 0; "bridge" = 0
    "control" = 1; "safety" = 1; "persist" = 1; "net" = 1
    "owners" = 2; "hw" = 2; "sim" = 2
    "common" = 3
}

function Get-Layer([string]$path) {
    # firmware/KilnFW/App/drivers/<layer>/<file> (STAY rows -- CMakeLists.txt,
    # Kconfig, README.md, gen_build_info.cmake -- have old_path == new_path
    # sitting directly in drivers/, so they never match a <layer> subdir and
    # fall out of $moveRows below without any special-casing).
    if ($path -match '^firmware/KilnFW/App/drivers/([^/]+)/') { return $matches[1] }
    return $null
}

# ---------------------------------------------------------------------------
# 1. git mv plan
# ---------------------------------------------------------------------------
Write-Host "=== 1. git mv plan ($($rows.Count) rows) ===" -ForegroundColor Cyan
$moveRows = $rows | Where-Object { (Get-Layer $_.new_path) -and ($tierOf.ContainsKey((Get-Layer $_.new_path))) }
$stayRows = $rows | Where-Object { -not ((Get-Layer $_.new_path) -and $tierOf.ContainsKey((Get-Layer $_.new_path))) }

Write-Host "$($moveRows.Count) rows map into the eleven layer subdirs; $($stayRows.Count) are STAY rows (CMakeLists.txt/Kconfig/README.md/gen_build_info.cmake -- component root files, no git mv needed since old_path == new_path)."

foreach ($r in $moveRows) {
    $cmd = "git mv `"$($r.old_path)`" `"$($r.new_path)`""
    if ($DryRun) {
        Write-Host "[DRYRUN] $cmd"
    } else {
        Write-Host $cmd
        Invoke-Expression $cmd
    }
}

# ---------------------------------------------------------------------------
# 2. Rewrite CMakeLists literal SRCS paths
# ---------------------------------------------------------------------------
Write-Host "`n=== 2. CMakeLists literal SRCS rewrite ===" -ForegroundColor Cyan

$literalMap = @{}
foreach ($r in $moveRows) {
    $oldRel = $r.old_path -replace '^firmware/KilnFW/App/drivers/', ''
    $literalMap[$oldRel] = $r.new_path -replace '^firmware/KilnFW/App/drivers/', ''
}

foreach ($cmakeFile in @($DriversCmake, $AppCmake)) {
    if (-not (Test-Path $cmakeFile)) { continue }
    $text = Get-Content $cmakeFile -Raw
    $hits = 0
    foreach ($old in $literalMap.Keys) {
        # drivers/CMakeLists.txt's SRCS entries are bare quoted filenames
        # ("MAX31856.c"), not path-qualified -- match the quoted literal.
        $pattern1 = '"' + [regex]::Escape($old) + '"'
        $m1 = [regex]::Matches($text, $pattern1)
        $hits += $m1.Count
    }
    Write-Host "$cmakeFile : $hits literal quoted-filename occurrence(s) found"
    if (-not $DryRun) {
        foreach ($old in $literalMap.Keys) {
            $newRel = $literalMap[$old]
            $text = $text -replace ('"' + [regex]::Escape($old) + '"'), ('"' + ($newRel -replace '\\','/') + '"')
        }
        Set-Content -Path $cmakeFile -Value $text -NoNewline
        Write-Host "  rewritten: $cmakeFile"
    }
}
Write-Host "NOTE (decision A): the single `drivers` component is retained -- CMakeLists.txt/Kconfig/README.md/gen_build_info.cmake all STAY at firmware/KilnFW/App/drivers/ (mapping.csv STAY rows, old_path == new_path). firmware/KilnFW/App/CMakeLists.txt still has no literal drivers/<file> paths (it only has REQUIRES drivers); the ~201 literal SRCS entries live entirely inside drivers/CMakeLists.txt and are rewritten in place above to their new '<layer>/<file>' relative path -- no new per-layer CMakeLists.txt fragments and no App/CMakeLists.txt restructuring needed."

# ---------------------------------------------------------------------------
# 3. Bare-include feasibility (INCLUDE_DIRS covering all ten dirs)
# ---------------------------------------------------------------------------
Write-Host "`n=== 3. Bare-include feasibility ===" -ForegroundColor Cyan
Write-Host @"
PROPOSAL (preferred): list all eleven new subdirectories
(firmware/KilnFW/App/drivers/{hw,owners,control,safety,persist,net,http,ui,
bridge,sim,common}) in the drivers component's own INCLUDE_DIRS
(idf_component_register in firmware/KilnFW/App/drivers/CMakeLists.txt, which
stays put per decision A), the same way drivers/ itself is a single
INCLUDE_DIRS entry today -- this is unchanged from before decision A except
the dirs are now subdirectories of drivers/ instead of siblings of it.
#include "x.h" lines stay BARE -- no path-qualified rewrite needed --
because ESP-IDF's build resolves an unqualified include against every
directory on the include path, and mapping.csv's collision check (see below)
found zero basename collisions across the eleven target dirs, so there is no
ambiguity a bare include could hit.
"@
$dupCheck = $rows | Group-Object { Split-Path $_.new_path -Leaf } | Where-Object { $_.Count -gt 1 }
if ($dupCheck) {
    Write-Host "WARNING: basename collisions found across target dirs -- bare includes would be ambiguous for these:" -ForegroundColor Yellow
    $dupCheck | ForEach-Object { Write-Host "  $($_.Name) : $($_.Count) occurrences" }
} else {
    Write-Host "No basename collisions across the eleven target directories -- bare-include plan holds."
}

# ---------------------------------------------------------------------------
# 4. Path-keyed check/test sites
# ---------------------------------------------------------------------------
Write-Host "`n=== 4. Path-keyed check/test sites (must be rewritten in the same commit) ===" -ForegroundColor Cyan

$basenames = $rows | ForEach-Object { Split-Path $_.old_path -Leaf } | Sort-Object -Unique
$searchDirs = @(
    "tools",
    "firmware/KilnFW/tools",
    "firmware/KilnFW/App/test"
) | Where-Object { Test-Path (Join-Path $RepoRoot $_) }

$candidateFiles = foreach ($d in $searchDirs) {
    Get-ChildItem -Path (Join-Path $RepoRoot $d) -Recurse -File -Include *.ps1,*.py |
        Where-Object {
            $_.FullName -notmatch '\\drivers_reorg\\' -and
            $_.FullName -notmatch '\\\.venv\\' -and
            $_.FullName -notmatch '\\node_modules\\' -and
            $_.FullName -notmatch '\\site-packages\\' -and
            $_.FullName -notmatch '\\\.git\\'
        }
}

$hitCount = 0
# Single combined regex (alternation) instead of an O(files*lines*basenames)
# triple loop -- Select-String does one pass per file.
$escaped = $basenames | ForEach-Object { [regex]::Escape($_) }
$pattern = 'drivers/|(' + ($escaped -join '|') + ')'
$rx = [regex]::new($pattern)

foreach ($file in $candidateFiles) {
    $matches_ = Select-String -Path $file.FullName -Pattern $pattern -AllMatches
    foreach ($m in $matches_) {
        $relPath = $file.FullName.Substring($RepoRoot.Length + 1) -replace '\\','/'
        Write-Host "$relPath`:$($m.LineNumber): $($m.Line.Trim())"
        $hitCount++
    }
}
Write-Host "`nTotal path-keyed lines found: $hitCount across $($candidateFiles.Count) scanned files"

# ---------------------------------------------------------------------------
# 5. Include-direction verifier
# ---------------------------------------------------------------------------
Write-Host "`n=== 5. Include-direction verifier (hypothetical post-move) ===" -ForegroundColor Cyan

# old basename -> new layer
$layerOf = @{}
foreach ($r in $moveRows) {
    $bn = Split-Path $r.old_path -Leaf
    $layerOf[$bn] = Get-Layer $r.new_path
}

# Deliberate exceptions (coordinator-reviewed, not architecture violations):
# the owner modules that arbitrate direct relay/GPIO access reach *up* to
# consult the safety/control state that gates whether a write is allowed at
# all -- CLAUDE.md's "Bypassed owner module bug class" note is explicit that
# every relay write must route through these owners with interlocks
# consulted, so an owner checking danger_mode/heat_interlock/ota_state/
# safety_link before acting is the safety property, not a layering bug.
# Keyed by "<includer basename>|<included basename>" so only these exact
# pairs are suppressed -- any other upward include from these same files
# still reports.
$allowedUpwardIncludes = @{
    "kiln_io_owner.c|danger_mode.h"   = $true
    "kiln_io_owner.c|heat_interlock.h" = $true
    "kiln_io_owner.c|ota_state.h"     = $true
    "kiln_io_owner.h|safety_link.h"   = $true
    "relay_authority.h|safety_link.h" = $true
}

$violations = @()
$driversDir = Join-Path $RepoRoot "firmware/KilnFW/App/drivers"
$srcFiles = Get-ChildItem -Path $driversDir -Recurse -File -Include *.c,*.h
foreach ($file in $srcFiles) {
    $bn = $file.Name
    if (-not $layerOf.ContainsKey($bn)) { continue }
    $includerLayer = $layerOf[$bn]
    if (-not $tierOf.ContainsKey($includerLayer)) { continue }
    $includerTier = $tierOf[$includerLayer]

    $lines = Get-Content $file.FullName
    for ($i = 0; $i -lt $lines.Count; $i++) {
        # Anchor to an actual preprocessor directive (optional leading
        # whitespace only) so a comment that merely mentions #include "x.h"
        # as prose -- e.g. backlight_pwm.h's "Deliberately NOT #include
        # ..." and safety_cfg_store.c's "Declared here by hand rather than
        # via #include ..." -- is not misreported as a real upward include.
        if ($lines[$i] -match '^\s*#include\s*"([^"]+)"') {
            $incName = $matches[1] -replace '^.*/', ''
            if ($layerOf.ContainsKey($incName)) {
                $includedLayer = $layerOf[$incName]
                if ($tierOf.ContainsKey($includedLayer)) {
                    $includedTier = $tierOf[$includedLayer]
                    if ($includerTier -gt $includedTier) {
                        $allowKey = "$bn|$incName"
                        if (-not $allowedUpwardIncludes.ContainsKey($allowKey)) {
                            $violations += [pscustomobject]@{
                                File = $bn; Layer = $includerLayer; Line = $i + 1
                                Include = $incName; IncludedLayer = $includedLayer
                            }
                        }
                    }
                }
            }
        }
    }
}

if ($violations.Count -eq 0) {
    Write-Host "No upward includes found (expected -- items 1-6 in the plan already untangled the six known patterns)."
} else {
    Write-Host "$($violations.Count) upward include(s) remain:" -ForegroundColor Yellow
    $violations | ForEach-Object {
        Write-Host "  $($_.File) [$($_.Layer)]:$($_.Line) includes `"$($_.Include)`" [$($_.IncludedLayer)] -- bottom-tier file including a higher-tier header"
    }
}

Write-Host "`n=== Done ($(if ($DryRun) {'DRY RUN -- nothing changed'} else {'APPLIED'})) ===" -ForegroundColor Cyan
