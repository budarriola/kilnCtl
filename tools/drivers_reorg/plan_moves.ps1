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

# ---------------------------------------------------------------------------
# 0. Mapping completeness check -- FAIL if any real file under
#    firmware/KilnFW/App/drivers/** is missing a mapping.csv row (excluding
#    the 4 STAY rows, which legitimately have old_path == new_path and are
#    covered separately). This must run before anything else touches disk.
# ---------------------------------------------------------------------------
Write-Host "=== 0. Mapping completeness check ===" -ForegroundColor Cyan
$mappedOld = @{}
foreach ($r in $rows) { $mappedOld[$r.old_path] = $true }

$driversDirAll = Join-Path $RepoRoot "firmware/KilnFW/App/drivers"
$actualFiles = Get-ChildItem -Path $driversDirAll -Recurse -File |
    ForEach-Object { $_.FullName.Substring($RepoRoot.Length + 1) -replace '\\','/' }

$missing = $actualFiles | Where-Object { -not $mappedOld.ContainsKey($_) }
if ($missing) {
    Write-Error "mapping.csv is missing $($missing.Count) file(s) present under firmware/KilnFW/App/drivers/**:`n$($missing -join "`n")"
    exit 1
}
Write-Host "All $($actualFiles.Count) files under firmware/KilnFW/App/drivers/** have a mapping.csv row."

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

if (-not $DryRun) {
    # Create all eleven layer dirs up front so `git mv` never has to create a
    # target directory itself (git mv fails/behaves oddly moving a file into
    # a not-yet-existent directory in some situations, and doing it up front
    # makes the loop below purely mechanical).
    $layerDirs = $moveRows | ForEach-Object { Split-Path $_.new_path -Parent } | Sort-Object -Unique
    foreach ($d in $layerDirs) {
        $full = Join-Path $RepoRoot $d
        if (-not (Test-Path $full)) {
            New-Item -ItemType Directory -Force -Path $full | Out-Null
            Write-Host "  mkdir $d"
        }
    }
}

foreach ($r in $moveRows) {
    if ($DryRun) {
        Write-Host "[DRYRUN] git mv `"$($r.old_path)`" `"$($r.new_path)`""
    } else {
        Write-Host "git mv `"$($r.old_path)`" `"$($r.new_path)`""
        git mv -- $r.old_path $r.new_path
        if ($LASTEXITCODE -ne 0) {
            Write-Error "git mv failed for $($r.old_path) -> $($r.new_path)"
            exit 1
        }
    }
}

# ---------------------------------------------------------------------------
# 2. Rewrite CMakeLists literal SRCS paths
# ---------------------------------------------------------------------------
Write-Host "`n=== 2. CMakeLists literal SRCS rewrite ===" -ForegroundColor Cyan

$cmakeHits = @{}
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
    $cmakeHits[$cmakeFile] = $hits
    if (-not $DryRun) {
        foreach ($old in $literalMap.Keys) {
            $newRel = $literalMap[$old]
            $text = $text -replace ('"' + [regex]::Escape($old) + '"'), ('"' + ($newRel -replace '\\','/') + '"')
        }
        # Preserve the file's original trailing-newline convention instead of
        # always stripping it (finding 4): Set-Content -NoNewline with no
        # -Encoding writes the system ANSI codepage and drops the final
        # newline. Write UTF-8 without a BOM (matches how this repo's other
        # generated scripts write text -- checked build_host_tests.ps1 and
        # gen_build_info.cmake, neither emits a BOM) and keep whatever
        # trailing newline the original had.
        $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
        [System.IO.File]::WriteAllText($cmakeFile, $text, $utf8NoBom)
        Write-Host "  rewritten: $cmakeFile"
    }
}

# ---------------------------------------------------------------------------
# 2b. Rewrite INCLUDE_DIRS in drivers/CMakeLists.txt to list all eleven layer
#     subdirs (finding 2) -- idempotent, and -Apply refuses if it cannot be
#     applied cleanly.
# ---------------------------------------------------------------------------
Write-Host "`n=== 2b. INCLUDE_DIRS rewrite ===" -ForegroundColor Cyan
$layerNames = $tierOf.Keys | Sort-Object
$includeDirsList = ($layerNames | ForEach-Object { "`"$_`"" }) -join ' '
$includeDirsPattern = 'INCLUDE_DIRS\s+((?:"[^"]*"\s*)+)'
if (-not (Test-Path $DriversCmake)) {
    Write-Error "$DriversCmake not found -- cannot rewrite INCLUDE_DIRS"
    if (-not $DryRun) { exit 1 }
} else {
    $cmakeText = Get-Content $DriversCmake -Raw
    $m = [regex]::Match($cmakeText, $includeDirsPattern)
    if (-not $m.Success) {
        Write-Error "Could not locate an INCLUDE_DIRS clause in $DriversCmake -- refusing to apply (finding 2: -Apply must refuse rather than silently skip this edit)."
        if (-not $DryRun) { exit 1 }
    } else {
        $already = $layerNames | Where-Object { $m.Groups[1].Value -notmatch [regex]::Escape("`"$_`"") }
        if (-not $already) {
            Write-Host "INCLUDE_DIRS already lists all eleven layer subdirs -- no change needed (idempotent)."
        } else {
            Write-Host "INCLUDE_DIRS will become: INCLUDE_DIRS $includeDirsList"
            if (-not $DryRun) {
                $newCmakeText = $cmakeText.Substring(0, $m.Index) + "INCLUDE_DIRS $includeDirsList" + $cmakeText.Substring($m.Index + $m.Length)
                $verify = [regex]::Match($newCmakeText, $includeDirsPattern)
                $stillMissing = $layerNames | Where-Object { $verify.Groups[1].Value -notmatch [regex]::Escape("`"$_`"") }
                if ($stillMissing) {
                    Write-Error "INCLUDE_DIRS rewrite failed verification (still missing: $($stillMissing -join ', ')) -- refusing to write $DriversCmake"
                    exit 1
                }
                $utf8NoBom2 = New-Object System.Text.UTF8Encoding($false)
                [System.IO.File]::WriteAllText($DriversCmake, $newCmakeText, $utf8NoBom2)
                Write-Host "  rewritten: $DriversCmake (INCLUDE_DIRS)"
            }
        }
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
# 4b. Mechanically rewrite path-keyed sites (finding 3): host-test mirrors
#     under App/test that `#include "../drivers/<file>"`, and check_*.ps1 /
#     *.py scripts that hardcode `App/drivers/<file>` -- both rewritten from
#     the same literal map used for the CMakeLists SRCS rewrite above.
#     Literals matching a "<prefix>drivers/<basename>" shape whose basename
#     is NOT in the map are reported and, under -Apply, are a hard failure
#     (this covers both a real gap in mapping.csv and an unrelated
#     firmware/UnitTestFw/... drivers/ literal that must NOT be touched --
#     see tools/check_no_duplicate_crc.ps1 in DRYRUN.md section 4b).
# ---------------------------------------------------------------------------
Write-Host "`n=== 4b. Rewrite path-keyed sites ===" -ForegroundColor Cyan

# basename -> new layer, for every moved file (STAY rows have no layer and
# are intentionally excluded -- their basenames never appear path-qualified
# under a layer subdir since they stay at the drivers/ root).
$basenameLayer = @{}
foreach ($r in $moveRows) {
    $basenameLayer[(Split-Path $r.old_path -Leaf)] = Get-Layer $r.new_path
}

# Only rewrite an occurrence when it is unambiguously scoped to THIS reorg's
# drivers/ tree, i.e. immediately preceded by ".../App/drivers/" or
# "../drivers/" (not e.g. "UnitTestFw/.../drivers/", which is explicitly
# out of scope).
# The optional "espInterfaces/" segment is consumed and dropped on rewrite
# (finding 3 / DRYRUN.md 4a): that subfolder is flattened into `owners/` by
# the move, so "App/drivers/espInterfaces/uart_protocol.h" becomes
# "App/drivers/owners/uart_protocol.h", not "App/drivers/owners/espInterfaces/...".
$siteRewritePattern = '((?:App/drivers/|\.\./drivers/))(?:espInterfaces/)?([A-Za-z0-9_\-]+(?:\.[A-Za-z0-9_\-]+)*)'
$unmapped = New-Object System.Collections.Generic.HashSet[string]
$rewriteFileCount = 0
$rewriteLineCount = 0

foreach ($file in $candidateFiles) {
    $raw = Get-Content -Path $file.FullName -Raw -ErrorAction SilentlyContinue
    if ($null -eq $raw) { continue }
    if ($raw -notmatch [regex]::Escape('App/drivers/') -and $raw -notmatch [regex]::Escape('../drivers/')) { continue }

    $fileLineHits = 0
    $rxMatches = [regex]::Matches($raw, $siteRewritePattern)
    $sb = New-Object System.Text.StringBuilder
    $lastEnd = 0
    foreach ($m in $rxMatches) {
        $prefix = $m.Groups[1].Value
        $bn = $m.Groups[2].Value
        [void]$sb.Append($raw.Substring($lastEnd, $m.Index - $lastEnd))
        # UnitTestFw is explicitly out of scope (DRYRUN.md 4b,
        # check_no_duplicate_crc.ps1's firmware/UnitTestFw/.../drivers/...
        # literal must NOT be touched even though its basename matches a
        # moved file) -- checked per-occurrence via the enclosing line.
        $lineStart = $raw.LastIndexOf("`n", [Math]::Max($m.Index - 1, 0)) + 1
        $lineEndIdx = $raw.IndexOf("`n", $m.Index)
        if ($lineEndIdx -lt 0) { $lineEndIdx = $raw.Length }
        $enclosingLine = $raw.Substring($lineStart, $lineEndIdx - $lineStart)
        if ($enclosingLine -match 'UnitTestFw') {
            [void]$sb.Append($m.Value)
            $lastEnd = $m.Index + $m.Length
            continue
        }
        if ($basenameLayer.ContainsKey($bn)) {
            [void]$sb.Append("$prefix$($basenameLayer[$bn])/$bn")
            $fileLineHits++
        } else {
            # Basename not one we're moving (e.g. UnitTestFw's own files
            # caught by an overly broad match, or a STAY-row basename like
            # CMakeLists.txt/Kconfig/README.md which never gets a layer
            # prefix) -- leave untouched, but track it for reporting.
            [void]$sb.Append($m.Value)
            [void]$unmapped.Add("$prefix$bn")
        }
        $lastEnd = $m.Index + $m.Length
    }
    [void]$sb.Append($raw.Substring($lastEnd))
    $newRaw = $sb.ToString()

    if ($fileLineHits -gt 0) {
        $relPath = $file.FullName.Substring($RepoRoot.Length + 1) -replace '\\','/'
        Write-Host "$relPath : $fileLineHits site(s) to rewrite"
        $rewriteFileCount++
        $rewriteLineCount += $fileLineHits
        if (-not $DryRun) {
            $utf8NoBom3 = New-Object System.Text.UTF8Encoding($false)
            [System.IO.File]::WriteAllText($file.FullName, $newRaw, $utf8NoBom3)
        }
    }
}

# build_host_tests.ps1's own include-path handling, if any (finding 3, last
# sentence) -- it is one of the $candidateFiles above (App/test), so it was
# already scanned/rewritten by the loop; call it out explicitly here since
# the coordinator asked for it by name.
$buildHostTests = Join-Path $RepoRoot "firmware/KilnFW/App/test/build_host_tests.ps1"
if (Test-Path $buildHostTests) {
    $bhtHits = (Select-String -Path $buildHostTests -Pattern 'App/drivers/|\.\./drivers/' -AllMatches).Count
    Write-Host "build_host_tests.ps1: $bhtHits drivers/-path reference(s) (covered by the loop above, called out per coordinator instruction)."
}

Write-Host "`nTotal: $rewriteLineCount site(s) across $rewriteFileCount file(s) $(if ($DryRun) {'would be rewritten'} else {'rewritten'})."

$stayBasenames = $stayRows | ForEach-Object { Split-Path $_.old_path -Leaf }
# Known documentation placeholders / globs, not real filenames -- e.g.
# check_host_embed_symbols_defined.ps1 documents the "../drivers/X.c"
# convention by name, check_c_files_in_cmakelists.ps1/check_doc_citations.ps1
# use "foo.c" as a made-up example, check_duplicate_symbols.ps1 says
# "App/drivers/kilnlink" meaning the kilnlink *library* (not a file in
# drivers/ at all), check_thermal_guard_input_producers.ps1 says
# "App/drivers/test" meaning the test *directory*, and
# check_bridge_reject_reason.ps1's "uart_bridge*.c" glob has its `*`
# stripped by the basename regex, leaving the bare prefix "uart_bridge".
$knownPlaceholders = @('X.c', 'foo.c', 'kilnlink', 'test', 'uart_bridge', 'uart_protocol')
$genuinelyUnmapped = $unmapped | Where-Object {
    $bn = $_ -replace '^.*/', ''
    -not ($stayBasenames -contains $bn) -and -not ($knownPlaceholders -contains $bn)
}

if ($unmapped.Count -gt 0) {
    $unmappedList = $unmapped | Sort-Object
    Write-Host "NOTE: $($unmapped.Count) 'drivers/<name>' literal(s) left untouched (STAY rows, or an out-of-scope tree like UnitTestFw):" -ForegroundColor Yellow
    $unmappedList | ForEach-Object { Write-Host "  $_" }
}
if ($genuinelyUnmapped.Count -gt 0 -and -not $DryRun) {
    Write-Error "Refusing to -Apply: $($genuinelyUnmapped.Count) 'drivers/<name>' literal(s) matched a path-keyed site but have no mapping.csv row and are not a known STAY/out-of-scope basename:`n$($genuinelyUnmapped -join "`n")"
    exit 1
}

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
        if ($lines[$i] -match '^\s*#\s*include\s*"([^"]+)"') {
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

if ($DryRun) {
    Write-Host "`n=== 6. Dry-run diff preview (counts) ===" -ForegroundColor Cyan
    Write-Host "  git mv                         : $($moveRows.Count) file(s), $($stayRows.Count) STAY (no-op)"
    Write-Host "  CMakeLists SRCS rewrite        : $($cmakeHits[$DriversCmake]) literal(s) in drivers/CMakeLists.txt, $($cmakeHits[$AppCmake]) in App/CMakeLists.txt"
    Write-Host "  INCLUDE_DIRS rewrite           : $(if ($already) { $already.Count } else { 0 }) layer dir(s) to add (0 = already up to date)"
    Write-Host "  Path-keyed site rewrite        : $rewriteLineCount line(s) across $rewriteFileCount file(s)"
    Write-Host "  Unmapped 'drivers/<name>' hits : $($unmapped.Count) (of which $($genuinelyUnmapped.Count) would hard-fail -Apply)"
    Write-Host "  Upward includes remaining      : $($violations.Count)"
}

Write-Host "`n=== Done ($(if ($DryRun) {'DRY RUN -- nothing changed'} else {'APPLIED'})) ===" -ForegroundColor Cyan
