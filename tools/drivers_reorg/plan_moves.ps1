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
    [switch]$DryRun = $true,
    # Writes a rewritten copy of every file this script would edit into
    # $PreviewDir (mirroring its repo-relative path) so the rewrite logic can
    # be inspected/diffed without touching the real tree. Independent of
    # -DryRun/-Apply: it runs in addition to whatever else this invocation
    # does. Directories are created as needed; existing content at the same
    # path is overwritten.
    [string]$PreviewDir = $null,
    # -Apply refuses (see the cleanliness gate below) if the working tree
    # under the paths this script touches is dirty, unless this is set.
    [switch]$AllowDirty
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

if ($PreviewDir) {
    New-Item -ItemType Directory -Force -Path $PreviewDir | Out-Null
    Write-Host "Preview mode: rewritten copies will be written under $PreviewDir (real tree untouched by this)." -ForegroundColor Magenta
}

# Writes $Content to $RealPath's rewritten form. Always mirrors into
# -PreviewDir (repo-relative path) when set, regardless of -DryRun/-Apply --
# that is the whole point of -PreviewDir. Only writes the real file when this
# is an actual -Apply run.
function Write-RewrittenFile([string]$RealPath, [string]$Content, [System.Text.Encoding]$Encoding) {
    $rel = $RealPath.Substring($RepoRoot.Length + 1)
    if ($PreviewDir) {
        $dest = Join-Path $PreviewDir $rel
        $destDir = Split-Path $dest -Parent
        if (-not (Test-Path $destDir)) { New-Item -ItemType Directory -Force -Path $destDir | Out-Null }
        [System.IO.File]::WriteAllText($dest, $Content, $Encoding)
        Write-Host "  [preview] $rel"
    }
    if (-not $DryRun) {
        [System.IO.File]::WriteAllText($RealPath, $Content, $Encoding)
        Write-Host "  rewritten: $RealPath"
    }
}

# ---------------------------------------------------------------------------
# -Apply working-tree cleanliness gate (finding 11). Runs before anything
# else that could act on -Apply, so a dirty tree never gets git-mv'd or
# rewritten out from under other in-flight work.
# ---------------------------------------------------------------------------
if ($Apply -and -not $AllowDirty) {
    $cleanlinessPaths = @(
        "firmware/KilnFW/App/drivers",
        "firmware/KilnFW/App/test",
        "firmware/KilnFW/App/drivers/CMakeLists.txt",
        "tools/"
    )
    $dirty = git status --porcelain -- $cleanlinessPaths
    if ($dirty) {
        Write-Error "Refusing -Apply: working tree is dirty under $($cleanlinessPaths -join ', '). Commit/stash first, or pass -AllowDirty to override:`n$dirty"
        exit 1
    }
}

$rows = Import-Csv $MappingCsv

# ---------------------------------------------------------------------------
# 0a. mapping.csv integrity: duplicate old_path/new_path rows, and old_path
#     entries that don't exist on disk (finding 7). Duplicates are a pure
#     data-authoring bug independent of repo state, so they fail on every
#     invocation including -DryRun (same posture as the completeness check
#     below). A missing old_path is allowed to be a *pending* row (a file
#     another in-flight session is about to add -- see profiles_store.h in
#     mapping.csv) so it only hard-fails under -Apply; -DryRun reports it as
#     a warning instead.
# ---------------------------------------------------------------------------
Write-Host "=== 0a. mapping.csv integrity (duplicates, old_path existence) ===" -ForegroundColor Cyan
$dupOld = $rows | Group-Object old_path | Where-Object { $_.Count -gt 1 }
$dupNew = $rows | Group-Object new_path | Where-Object { $_.Count -gt 1 }
if ($dupOld -or $dupNew) {
    if ($dupOld) { $dupOld | ForEach-Object { Write-Error "mapping.csv: duplicate old_path '$($_.Name)' ($($_.Count) rows)" } }
    if ($dupNew) { $dupNew | ForEach-Object { Write-Error "mapping.csv: duplicate new_path '$($_.Name)' ($($_.Count) rows)" } }
    exit 1
}
Write-Host "No duplicate old_path/new_path rows."

$missingOldPath = $rows | Where-Object { -not (Test-Path (Join-Path $RepoRoot $_.old_path)) }
if ($missingOldPath) {
    $names = ($missingOldPath | ForEach-Object { $_.old_path }) -join "`n"
    if ($Apply) {
        Write-Error "Refusing -Apply: mapping.csv has $($missingOldPath.Count) old_path row(s) that do not exist on disk:`n$names"
        exit 1
    } else {
        Write-Host "WARNING: $($missingOldPath.Count) old_path row(s) do not exist on disk yet (pending, e.g. profiles_store.h -- not a failure in -DryRun, but -Apply would refuse until the file lands or the row is removed):" -ForegroundColor Yellow
        $missingOldPath | ForEach-Object { Write-Host "  $($_.old_path)" }
    }
} else {
    Write-Host "All old_path rows exist on disk."
}

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
#     -> control/safety/persist/net/sim (tier 1, mid -- coordinator decision
#       2026-09-05, item 9: sim moves back to MID, alongside
#       control/safety/persist/net, superseding the earlier "decision D
#       reverted" placement in owners/hw/sim. Rationale: a sim backend
#       legitimately reads config the way control/persist/net do -- it is a
#       system-level stand-in, not a bottom-tier device driver -- and the
#       plan's own residual list (DRYRUN.md Fix 4) already showed control
#       consumes sim_backend.h the same way it consumes a config/status
#       header from this same tier, not the way it consumes a raw hw driver)
#       -> owners/hw (tier 2, bottom -- bus/IO arbitration and real device
#         drivers)
#         -> common (tier 3, bottom-most -- decision B: pure leaf
#           headers/utilities with no includes above this tier)
$tierOf = @{
    "ui" = 0; "http" = 0; "bridge" = 0
    "control" = 1; "safety" = 1; "persist" = 1; "net" = 1; "sim" = 1
    "owners" = 2; "hw" = 2
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

# finding 4: the KILNCTL_GZIP_ASSETS quoted literals (drivers/CMakeLists.txt)
# must NOT be touched by the generic per-literal rewrite below -- they get a
# dedicated structural rewrite instead (see "2c" below) since kilnctl_gz_out
# must stay flat (unqualified basename) so the EMBED_TXTFILES
# _binary_<name>_gz_start/_end symbol names don't change. Carve that block
# out of $text before the generic pass runs, and splice it back in after.
$gzipBlockPattern = '(?s)(set\(KILNCTL_GZIP_ASSETS.*?\r?\n\s*\))'
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

foreach ($cmakeFile in @($DriversCmake, $AppCmake)) {
    if (-not (Test-Path $cmakeFile)) { continue }
    $text = Get-Content $cmakeFile -Raw

    $gzipBlockMatch = [regex]::Match($text, $gzipBlockPattern)
    $gzipPlaceholder = "@@KILNCTL_GZIP_ASSETS_BLOCK_PLACEHOLDER@@"
    $gzipBlockText = $null
    if ($gzipBlockMatch.Success) {
        $gzipBlockText = $gzipBlockMatch.Value
        $text = $text.Substring(0, $gzipBlockMatch.Index) + $gzipPlaceholder + $text.Substring($gzipBlockMatch.Index + $gzipBlockMatch.Length)
    }

    $hits = 0
    foreach ($old in $literalMap.Keys) {
        # drivers/CMakeLists.txt's SRCS entries are bare quoted filenames
        # ("MAX31856.c"); finding 5: a basename can ALSO appear embedded in
        # a longer quoted path, e.g. "${CMAKE_CURRENT_SOURCE_DIR}/tuning_
        # recommendations_fallback.json". Both shapes are matched by ONE
        # regex -- the character immediately before the basename is either
        # `"` (bare) or `/` (embedded), captured so it can be echoed back
        # unchanged. This single combined pattern is essential, not just
        # tidier: two separate sequential -replace passes (bare-quote first,
        # then embedded-path) would have the second pass re-match the `/`
        # the first pass just inserted before the new "<layer>/<basename>"
        # text and double-prefix it (e.g. "hw/hw/SX1509.c") -- caught by a
        # -PreviewDir run while developing this fix.
        $pattern = '(["/])' + [regex]::Escape($old) + '"'
        $hits += [regex]::Matches($text, $pattern).Count
    }
    if ($gzipBlockText) {
        Write-Host "$cmakeFile : KILNCTL_GZIP_ASSETS block excluded from the generic rewrite (handled by 2c)"
    }
    Write-Host "$cmakeFile : $hits literal quoted-filename occurrence(s) found (outside KILNCTL_GZIP_ASSETS)"
    $cmakeHits[$cmakeFile] = $hits

    foreach ($old in $literalMap.Keys) {
        $newRel = ($literalMap[$old] -replace '\\','/')
        $pattern = '(["/])' + [regex]::Escape($old) + '"'
        $text = [regex]::Replace($text, $pattern, { param($mm) $mm.Groups[1].Value + $newRel + '"' })
    }

    if ($gzipBlockText) {
        $text = $text.Replace($gzipPlaceholder, $gzipBlockText)
    }

    if ($cmakeFile -eq $DriversCmake) {
        # Don't write yet -- section 2c below still needs to apply the
        # KILNCTL_GZIP_ASSETS structural rewrite on top of this same text;
        # writing here and re-reading from disk in 2c would silently drop
        # this generic-literal pass in -PreviewDir/-Apply (the two rewrites
        # would clobber each other instead of composing).
        $driversCmakeRewrittenText = $text
        continue
    }

    # Preserve the file's original trailing-newline convention instead of
    # always stripping it: Set-Content -NoNewline with no -Encoding writes
    # the system ANSI codepage and drops the final newline. Write UTF-8
    # without a BOM (matches how this repo's other generated scripts write
    # text -- checked build_host_tests.ps1 and gen_build_info.cmake, neither
    # emits a BOM) and keep whatever trailing newline the original had.
    Write-RewrittenFile -RealPath $cmakeFile -Content $text -Encoding $utf8NoBom
}

# ---------------------------------------------------------------------------
# 2c. KILNCTL_GZIP_ASSETS structural rewrite (finding 4). The 16 assets span
#     more than one target layer (most are http, but ota_page.html and
#     wifi_provision_page.html are net per mapping.csv), so a single
#     "${CMAKE_CURRENT_SOURCE_DIR}/<layer>/" prefix constant can't work --
#     each list entry becomes "<layer>/<basename>" instead, kilnctl_gz_src
#     is built straight from that (picks up the layer), and kilnctl_gz_out
#     is rebuilt from get_filename_component(... NAME) so it stays the flat
#     basename -- EMBED_TXTFILES' _binary_<name>_gz_start/_end symbols are
#     therefore unchanged by this move.
# ---------------------------------------------------------------------------
Write-Host "`n=== 2c. KILNCTL_GZIP_ASSETS structural rewrite ===" -ForegroundColor Cyan
if (Test-Path $DriversCmake) {
    # Chain onto the generic-literal-rewritten text from section 2 above
    # (not a fresh read from disk) so the two rewrites compose instead of
    # one clobbering the other.
    $text2c = if ($driversCmakeRewrittenText) { $driversCmakeRewrittenText } else { Get-Content $DriversCmake -Raw }
    $assetBlockMatch = [regex]::Match($text2c, $gzipBlockPattern)
    if (-not $assetBlockMatch.Success) {
        Write-Error "Could not locate the KILNCTL_GZIP_ASSETS block in $DriversCmake -- refusing to apply this rewrite."
        if (-not $DryRun) { exit 1 }
    } else {
        $assetListText = $assetBlockMatch.Value
        $assetLiterals = [regex]::Matches($assetListText, '"([^"]+)"') | ForEach-Object { $_.Groups[1].Value }
        $newAssetListText = $assetListText
        $gzAssetHits = 0
        foreach ($asset in $assetLiterals) {
            if ($literalMap.ContainsKey($asset)) {
                $newRel = ($literalMap[$asset] -replace '\\','/')
                $newAssetListText = $newAssetListText -replace ('"' + [regex]::Escape($asset) + '"'), ('"' + $newRel + '"')
                $gzAssetHits++
            }
        }
        Write-Host "$($assetLiterals.Count) KILNCTL_GZIP_ASSETS entries, $gzAssetHits will gain a layer prefix"

        $gzSrcOldLine = 'set(kilnctl_gz_src "${CMAKE_CURRENT_SOURCE_DIR}/${asset}")'
        $gzOutOldLine = 'set(kilnctl_gz_out "${CMAKE_CURRENT_BINARY_DIR}/${asset}.gz")'
        $gzNewLines = 'get_filename_component(kilnctl_gz_basename "${asset}" NAME)' + "`r`n" +
            '        set(kilnctl_gz_src "${CMAKE_CURRENT_SOURCE_DIR}/${asset}")' + "`r`n" +
            '        set(kilnctl_gz_out "${CMAKE_CURRENT_BINARY_DIR}/${kilnctl_gz_basename}.gz")'

        if ($text2c -notmatch [regex]::Escape($gzSrcOldLine) -or $text2c -notmatch [regex]::Escape($gzOutOldLine)) {
            Write-Error "Could not locate the kilnctl_gz_src/kilnctl_gz_out lines verbatim in $DriversCmake -- refusing to apply this rewrite (the file may have changed shape since this script was written)."
            if (-not $DryRun) { exit 1 }
        } else {
            $newText2c = $text2c.Replace($assetListText, $newAssetListText)
            $newText2c = $newText2c.Replace($gzSrcOldLine, $gzNewLines).Replace($gzOutOldLine, '')
            # The .Replace($gzOutOldLine, '') above just deletes the old
            # out-line text since $gzNewLines already supplies the
            # replacement kilnctl_gz_out line; strip the now-empty line it
            # leaves behind so indentation stays tidy.
            $newText2c = $newText2c -replace '(\r?\n)[ \t]*(\r?\n)', '$1'
            Write-Host "kilnctl_gz_src will read the asset's own '<layer>/<basename>' entry; kilnctl_gz_out stays flat via get_filename_component(NAME)."
            # Update the running text, don't write yet -- 2b (INCLUDE_DIRS)
            # below still needs to chain onto this same text; a single write
            # happens after 2b so all three drivers/CMakeLists.txt rewrites
            # (2, 2c, 2b) compose into one file instead of clobbering.
            $driversCmakeRewrittenText = $newText2c
        }
    }
}

# ---------------------------------------------------------------------------
# 2b. Rewrite INCLUDE_DIRS in drivers/CMakeLists.txt to list all eleven layer
#     subdirs (finding 2) -- idempotent, and -Apply refuses if it cannot be
#     applied cleanly.
# ---------------------------------------------------------------------------
Write-Host "`n=== 2b. INCLUDE_DIRS rewrite ===" -ForegroundColor Cyan
$layerNames = $tierOf.Keys | Sort-Object
# finding 6: KEEP "." in the list -- it is the existing bare-include root
# ("." resolves the huge majority of same-directory bare includes today) --
# dropping it would be an unstated behavior change, not just a reformat.
# "espInterfaces" is dropped: that subfolder is flattened into owners/ by
# the move, so it no longer exists as a name on the include path.
$includeDirsEntries = @(".") + $layerNames
$includeDirsList = ($includeDirsEntries | ForEach-Object { "`"$_`"" }) -join ' '
$includeDirsPattern = 'INCLUDE_DIRS\s+((?:"[^"]*"\s*)+)'
$script:includeDirsAlreadyCount = $null
$script:includeDirsClauseMissing = $false
if (-not (Test-Path $DriversCmake)) {
    Write-Error "$DriversCmake not found -- cannot rewrite INCLUDE_DIRS"
    $script:includeDirsClauseMissing = $true
    if (-not $DryRun) { exit 1 }
} else {
    # Chain onto whatever section 2/2c already produced for this same file
    # rather than re-reading from disk, so all three rewrites compose.
    $cmakeText = if ($driversCmakeRewrittenText) { $driversCmakeRewrittenText } else { Get-Content $DriversCmake -Raw }
    $m = [regex]::Match($cmakeText, $includeDirsPattern)
    if (-not $m.Success) {
        Write-Error "Could not locate an INCLUDE_DIRS clause in $DriversCmake -- refusing to apply (finding 2: -Apply must refuse rather than silently skip this edit)."
        $script:includeDirsClauseMissing = $true
        if (-not $DryRun) { exit 1 }
    } else {
        $already = $includeDirsEntries | Where-Object { $m.Groups[1].Value -match [regex]::Escape("`"$_`"") }
        $missingEntries = $includeDirsEntries | Where-Object { $m.Groups[1].Value -notmatch [regex]::Escape("`"$_`"") }
        $script:includeDirsAlreadyCount = $already.Count
        if (-not $missingEntries) {
            Write-Host "INCLUDE_DIRS already lists '.' and all eleven layer subdirs -- no change needed (idempotent)."
        } else {
            Write-Host "INCLUDE_DIRS will become: INCLUDE_DIRS $includeDirsList"
            # finding 6: replace ONLY Groups[1]'s span (the captured dir-list
            # text), not the whole match -- the whole match's trailing `\s*`
            # can swallow the newline+indentation before the next keyword
            # (e.g. PRIV_INCLUDE_DIRS), which previously produced
            # `"ui"PRIV_INCLUDE_DIRS` (a CMake syntax error) because that
            # whitespace was deleted along with the old dir list.
            $g = $m.Groups[1]
            # $g.Value's own greedy `(?:"[^"]*"\s*)+` includes the trailing
            # whitespace/newline after the LAST quoted entry as part of the
            # capture (that's exactly how the original bug happened: the
            # first version of this fix replaced "Groups[1]" verbatim,
            # which still deleted that trailing whitespace along with the
            # dir list). Trim the replacement span to end right after the
            # last '"' in the group so the whitespace before the next
            # keyword (PRIV_INCLUDE_DIRS) is left completely untouched.
            $lastQuoteIdx = $g.Value.LastIndexOf('"')
            $effectiveLen = $lastQuoteIdx + 1
            $newCmakeText = $cmakeText.Substring(0, $g.Index) + $includeDirsList + $cmakeText.Substring($g.Index + $effectiveLen)
            $verify = [regex]::Match($newCmakeText, $includeDirsPattern)
            $stillMissing = $includeDirsEntries | Where-Object { $verify.Groups[1].Value -notmatch [regex]::Escape("`"$_`"") }
            if ($stillMissing) {
                Write-Error "INCLUDE_DIRS rewrite failed verification (still missing: $($stillMissing -join ', ')) -- refusing to write $DriversCmake"
                if (-not $DryRun) { exit 1 }
            } else {
                # Also verify the whitespace immediately after the new list
                # was preserved (the exact regression this fix targets):
                # whatever directly follows the old dir list in the original
                # text must still directly follow the new one.
                $tailAfterOld = $cmakeText.Substring($g.Index + $effectiveLen, [Math]::Min(40, $cmakeText.Length - ($g.Index + $effectiveLen)))
                $tailAfterNew = $newCmakeText.Substring($g.Index + $includeDirsList.Length, [Math]::Min(40, $newCmakeText.Length - ($g.Index + $includeDirsList.Length)))
                if ($tailAfterOld -ne $tailAfterNew) {
                    Write-Error "INCLUDE_DIRS rewrite changed the whitespace/text following the dir list (expected '$tailAfterOld', got '$tailAfterNew') -- refusing to write $DriversCmake"
                    if (-not $DryRun) { exit 1 }
                } else {
                    $driversCmakeRewrittenText = $newCmakeText
                }
            }
        }
    }
}

# Single write for drivers/CMakeLists.txt, after sections 2, 2c and 2b have
# all had a chance to chain their edits onto $driversCmakeRewrittenText.
if ($driversCmakeRewrittenText -and (Test-Path $DriversCmake)) {
    Write-RewrittenFile -RealPath $DriversCmake -Content $driversCmakeRewrittenText -Encoding $utf8NoBom
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

# finding 12: check_uri_handler_cap.ps1, check_nvs_write_guard_coverage.ps1
# and check_host_embed_symbols_defined.ps1 are being made layout-agnostic by
# another agent in parallel -- excluded here by name so this rewriter never
# edits them (avoiding a conflicting edit), noted again in DRYRUN.md.
$otherAgentOwnedScripts = @(
    'check_uri_handler_cap.ps1',
    'check_nvs_write_guard_coverage.ps1',
    'check_host_embed_symbols_defined.ps1'
)

$candidateFiles = foreach ($d in $searchDirs) {
    # finding 2: App/test's host-test mirror sources (`#include "../drivers/
    # X.c"`) are *.c/*.h, not *.ps1/*.py -- the original -Include list never
    # matched any of the 153 such lines. Every other search dir stays
    # script-only (tools/**, firmware/KilnFW/tools/** have no .c/.h that
    # legitimately reference drivers/ this way).
    $includePatterns = if ($d -eq "firmware/KilnFW/App/test") { @('*.ps1', '*.py', '*.c', '*.h') } else { @('*.ps1', '*.py') }
    Get-ChildItem -Path (Join-Path $RepoRoot $d) -Recurse -File -Include $includePatterns |
        Where-Object {
            $_.FullName -notmatch '\\drivers_reorg\\' -and
            $_.FullName -notmatch '\\\.venv\\' -and
            $_.FullName -notmatch '\\node_modules\\' -and
            $_.FullName -notmatch '\\site-packages\\' -and
            $_.FullName -notmatch '\\\.git\\' -and
            -not ($otherAgentOwnedScripts -contains $_.Name)
        }
}

# finding 12: markdown citations of "App/drivers/<file>[:line]" -- docs/,
# firmware/**/docs, any *.md under firmware/**, and the root-level
# ROADMAP.md/CLAUDE.md/TODO.md family. Folded into the same rewrite pass as
# the check/test scripts below (same $siteRewritePattern, same basename ->
# layer map) rather than a separate pass.
$mdRoots = @(
    "docs",
    "firmware"
) | Where-Object { Test-Path (Join-Path $RepoRoot $_) }
$mdFiles = foreach ($d in $mdRoots) {
    Get-ChildItem -Path (Join-Path $RepoRoot $d) -Recurse -File -Include *.md |
        Where-Object { $_.FullName -notmatch '\\\.git\\' }
}
$rootMdFiles = @('ROADMAP.md', 'CLAUDE.md', 'TODO.md') | ForEach-Object {
    $p = Join-Path $RepoRoot $_
    if (Test-Path $p) { Get-Item $p }
}
$mdFiles = @($mdFiles) + @($rootMdFiles) | Where-Object { $_ }
Write-Host "Markdown citation scan: $($mdFiles.Count) .md file(s) under docs/, firmware/**, and the root TODO/ROADMAP/CLAUDE family."

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

# finding 1: build_host_tests.ps1's `Join-Path $driversDir "<name>"` bare-
# basename form (no "App/drivers/"/"../drivers/" prefix at all -- $driversDir
# is a PowerShell variable holding that path) -- rewrite the quoted basename
# argument to "<layer>/<name>" so Join-Path resolves into the new subdir.
# Matches both quote styles used in the file ("pid.c" and 'zones_config_json.c').
$joinPathPattern = 'Join-Path\s+\$driversDir\s+([\x22\x27])([A-Za-z0-9_\-]+\.[A-Za-z0-9_\-]+)\1'

$allRewriteTargets = @($candidateFiles) + @($mdFiles)
foreach ($file in $allRewriteTargets) {
    $raw = Get-Content -Path $file.FullName -Raw -ErrorAction SilentlyContinue
    if ($null -eq $raw) { continue }
    $hasSiteHits = ($raw -match [regex]::Escape('App/drivers/')) -or ($raw -match [regex]::Escape('../drivers/'))
    $hasJoinPathHits = $raw -match 'Join-Path\s+\$driversDir'
    if (-not $hasSiteHits -and -not $hasJoinPathHits) { continue }

    $fileLineHits = 0

    if ($hasJoinPathHits) {
        $jpMatches = [regex]::Matches($raw, $joinPathPattern)
        $jsb = New-Object System.Text.StringBuilder
        $jLastEnd = 0
        foreach ($jm in $jpMatches) {
            $bn = $jm.Groups[2].Value
            [void]$jsb.Append($raw.Substring($jLastEnd, $jm.Index - $jLastEnd))
            if ($basenameLayer.ContainsKey($bn)) {
                $q = $jm.Groups[1].Value
                [void]$jsb.Append("Join-Path `$driversDir $q$($basenameLayer[$bn])/$bn$q")
                $fileLineHits++
            } else {
                [void]$jsb.Append($jm.Value)
                # Tracked as "Join-Path/<basename>" (not the literal
                # PowerShell call text) so the genuinely-unmapped filter's
                # `-replace '^.*/', ''` extraction below finds the same
                # basename it would for a "prefix/basename" site hit -- e.g.
                # check_c_files_in_cmakelists.ps1's `Join-Path $driversDir
                # "CMakeLists.txt"` correctly falls out via $stayBasenames
                # (CMakeLists.txt is a STAY row, never gets a layer prefix)
                # instead of spuriously hard-failing -Apply.
                [void]$unmapped.Add("Join-Path/$bn")
            }
            $jLastEnd = $jm.Index + $jm.Length
        }
        [void]$jsb.Append($raw.Substring($jLastEnd))
        $raw = $jsb.ToString()

        # Same file's `cl ... /I"$driversDir"` invocations need /I for every
        # layer subdir too -- a bare #include that used to resolve via
        # $driversDir's own directory (single flat dir) now needs one /I per
        # layer, since the including and included file can land in
        # different layer subdirs. Expand in place, once per literal
        # occurrence of the flag.
        $layerIncludeFlags = ($layerNames | ForEach-Object { '/I`"$driversDir\' + $_ + '`"' }) -join ' '
        $raw = $raw -replace '/I`"\$driversDir`"', $layerIncludeFlags
    }

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
        Write-RewrittenFile -RealPath $file.FullName -Content $newRaw -Encoding $utf8NoBom
    }
}

# ---------------------------------------------------------------------------
# 4c. espInterfaces/ bare-include prefix strip inside drivers/ itself
#     (finding 3). Unlike the check/test-script sites above, these are real
#     production #include lines with NO "App/drivers/"/"../drivers/" prefix
#     at all -- gpio_probe.h:5, uart_bridge.h:6, uart_bridge_internal.h:30,
#     uart_log_bridge.h:5 each say `#include "espInterfaces/uart_protocol.h"`
#     bare, relative to the old flat drivers/ dir. Once espInterfaces/ is
#     flattened into owners/ (mapping.csv), the segment must be dropped so
#     the include becomes bare "uart_protocol.h", resolved via the new
#     eleven-dir INCLUDE_DIRS (section 3) the same way every other
#     same-tier bare include already works.
# ---------------------------------------------------------------------------
Write-Host "`n=== 4c. espInterfaces/ bare-include prefix strip ===" -ForegroundColor Cyan
$espIncludePattern = '(#\s*include\s*")espInterfaces/([A-Za-z0-9_\-]+\.[A-Za-z0-9_\-]+)(")'
$espHitFiles = 0
$espHitLines = 0
$driversSrcFilesForEsp = Get-ChildItem -Path $driversDirAll -Recurse -File -Include *.c,*.h
foreach ($file in $driversSrcFilesForEsp) {
    $raw = Get-Content -Path $file.FullName -Raw -ErrorAction SilentlyContinue
    if ($null -eq $raw) { continue }
    if ($raw -notmatch 'espInterfaces/') { continue }
    $espMatches = [regex]::Matches($raw, $espIncludePattern)
    if ($espMatches.Count -eq 0) { continue }
    $relPath = $file.FullName.Substring($RepoRoot.Length + 1) -replace '\\','/'
    Write-Host "$relPath : $($espMatches.Count) espInterfaces/ include(s) to strip"
    $espHitFiles++
    $espHitLines += $espMatches.Count
    $newRaw = [regex]::Replace($raw, $espIncludePattern, '$1$2$3')
    Write-RewrittenFile -RealPath $file.FullName -Content $newRaw -Encoding $utf8NoBom
}
Write-Host "Total: $espHitLines espInterfaces/ include(s) across $espHitFiles file(s) $(if ($DryRun) {'would be rewritten'} else {'rewritten'})."

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
# finding 12's markdown scan surfaced a second wave of the same class: prose
# that mentions a module by name without a file extension ("the board_temps
# module", "see profiles_http's handler", etc. -- the basename regex's
# extension group is optional so it still matches), and prose that names a
# UnitTestFw hardware-component file (AD9833.c, DcDac.c, ILI9488.c,
# PCF8575.c, SSD1306.c) whose enclosing line didn't happen to also say
# "UnitTestFw" (the per-occurrence UnitTestFw exclusion in the rewrite loop
# above is line-scoped, not file-scoped, so a citation on its own line in a
# UnitTestFw doc without a same-line "UnitTestFw" mention still surfaces
# here). rules_task.c/rules_http are prose references to the deleted
# "Relays & Rules" engine (CLAUDE.md/CMakeLists.txt comment), not a file
# that exists to be moved.
$knownPlaceholders = @(
    'X.c', 'foo.c', 'kilnlink', 'test', 'uart_bridge', 'uart_protocol',
    'AD9833.c', 'AD9833.h', 'DcDac.c', 'DcDac.h', 'ILI9488.c',
    'PCF8575.c', 'PCF8575.h', 'SSD1306.c', 'SSD1306.h',
    'rules_http', 'rules_task.c',
    'board_temps', 'dashboard_http', 'heater_output', 'nvs_report',
    'ota_auth', 'ota_pico_relay', 'pid', 'profiles_http',
    'relay_authority', 'sim_backend', 'stack_margin', 'thermal_guard',
    'thermo_combine', 'ui_topbar', 'web_encoding', 'wifi_prov',
    'wifi_provision_http', 'zones_http', 'espInterfaces'
)
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
    # finding 8: a non-allowlisted upward include is an architecture
    # violation the reorg is supposed to remove, not just a thing to note --
    # -Apply must refuse rather than complete a move that leaves it in
    # place. -DryRun (including a -PreviewDir run, which is still a dry
    # run) only reports it, since that's exactly what dry runs are for.
    if ($Apply) {
        Write-Error "Refusing -Apply: $($violations.Count) non-allowlisted upward include(s) remain (listed above). Either resolve them (see DRYRUN.md section 5's proposed fixes) or add a reviewed entry to `$allowedUpwardIncludes."
        exit 1
    }
}

if ($DryRun) {
    Write-Host "`n=== 6. Dry-run diff preview (counts) ===" -ForegroundColor Cyan
    Write-Host "  git mv                         : $($moveRows.Count) file(s), $($stayRows.Count) STAY (no-op)"
    Write-Host "  CMakeLists SRCS rewrite        : $($cmakeHits[$DriversCmake]) literal(s) in drivers/CMakeLists.txt, $($cmakeHits[$AppCmake]) in App/CMakeLists.txt"
    if ($script:includeDirsClauseMissing) {
        Write-Host "  INCLUDE_DIRS rewrite           : INCLUDE_DIRS clause not found (see section 2b error above)"
    } else {
        $includeDirsMissingCount = $includeDirsEntries.Count - $script:includeDirsAlreadyCount
        Write-Host "  INCLUDE_DIRS rewrite           : $includeDirsMissingCount of $($includeDirsEntries.Count) '.'+layer dir(s) to add (0 = already up to date)"
    }
    Write-Host "  Path-keyed site rewrite        : $rewriteLineCount line(s) across $rewriteFileCount file(s) (incl. markdown + Join-Path `$driversDir sites)"
    Write-Host "  espInterfaces/ include strip   : $espHitLines line(s) across $espHitFiles file(s)"
    Write-Host "  Unmapped 'drivers/<name>' hits : $($unmapped.Count) (of which $($genuinelyUnmapped.Count) would hard-fail -Apply)"
    Write-Host "  Upward includes remaining      : $($violations.Count) $(if ($violations.Count -gt 0) {'(would hard-fail -Apply)'})"
    Write-Host "  Missing old_path row(s)        : $($missingOldPath.Count) (would hard-fail -Apply; benign in -DryRun -- see section 0a)"
}
if ($PreviewDir) {
    Write-Host "`nPreview copies written under: $PreviewDir"
}

Write-Host "`n=== Done ($(if ($DryRun) {'DRY RUN -- nothing changed'} else {'APPLIED'})) ===" -ForegroundColor Cyan
