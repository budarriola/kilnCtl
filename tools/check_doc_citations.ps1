# check_doc_citations.ps1 -- every `path/to/file.c:NNN`-style citation in a
# checked-in *.md doc must point at a line that still exists in that file.
#
# WHY THIS EXISTS. The fifth stale-claim-audit pass (2026-09-04) found that
# firmware/KilnFW/docs/ARCHITECTURE.md's task-inventory table had gone stale
# in nearly every row after uart_bridge.c/uart_bridge_ext.c/profile_executor.c
# were split into per-command-family files: every `file:line` citation still
# named the ORIGINAL file, at a line number past that file's new, much
# shorter, end-of-file. The table's own citations were corrected once
# (2026-09-04) -- and by the end of the SAME day, section 2's citations to
# uart_bridge.c/profile_executor.c/main.c (never touched by that correction
# pass) and the table's own `ota_confirm` row (main.c split again, out from
# under a citation that had just been "fixed") were freshly stale again. This
# repo already has source_path_drift_check.py for path-keyed CODE checks
# surviving a file split; nothing covered path-keyed CITATIONS IN DOCS. This
# is that check.
#
# WHAT THIS CHECKS, AND WHY IT STOPS THERE. A citation of the form
# `<path ending in .c/.h/.cpp/.hpp>:<line>` (optionally with a second
# `:<line>` or a `,<line>`/`-<line>`/`/<line>` list of further lines in the
# same citation, e.g. `foo.c:1548,1569,1583` or `foo.c:62-76`) must resolve to
# an EXISTING file at that path (tried relative to the repo root, then
# relative to the doc's own directory) with AT LEAST that many lines. That is
# the entire check: file exists, line exists. It deliberately does NOT try to
# confirm the named symbol is still at that line -- this repo's docs cite
# function bodies, struct fields, comment blocks, and multi-line ranges in no
# consistent shape, and a symbol-proximity heuristic strict enough to catch
# real drift would also flag every legitimate rename/refactor the doc hasn't
# caught up to yet in the exact same breath as a citation that just moved
# fifty lines during a split -- indistinguishable without reading the prose,
# which is a human job. A checker that cries wolf on every rename gets turned
# off (see this script's own header precedent: check_uri_handler_cap.ps1 and
# check_c_files_in_cmakelists.ps1 both learned this the hard way and stayed
# narrow on purpose). File-exists-and-line-exists is cheap, mechanical, and
# has zero false positives against a codebase that only ever adds lines or
# splits files -- exactly the two things that provoked this check's creation.
#
# WHAT THIS DOES NOT CATCH. A citation that points at a real, in-bounds line
# of the RIGHT file but the WRONG line within it (the symbol moved 20 lines
# within the same file, or the file grew past the cited line without ever
# shrinking below it) is invisible to this check. That is a real gap, judged
# acceptable: closing it needs a symbol-aware citation format this repo does
# not have today, and a best-effort keyword match would be exactly the noisy,
# switched-off-in-a-month checker described above.
#
# SCOPE. Every *.md file in the repo (excluding build/ output, vendored
# submodules, and stray per-agent worktree copies), matched the same way
# run_all_checks.ps1 excludes them.
#
# Usage: powershell -File tools\check_doc_citations.ps1
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot

$docs = Get-ChildItem -Path $root -Filter "*.md" -Recurse -File |
    Where-Object {
        $_.FullName -notmatch '[\\/]build[\\/]' -and
        $_.FullName -notmatch '[\\/]node_modules[\\/]' -and
        $_.FullName -notmatch '[\\/]mykicadMcp[\\/]' -and
        $_.FullName -notmatch '[\\/]pdfMcp[\\/]' -and
        $_.FullName -notmatch '[\\/]\.[^\\/]+[\\/]'
    } |
    Sort-Object FullName

# Matches a citation like:
#   App/drivers/foo.c:123
#   `foo.c:123,456`
#   foo.h:62-76
#   foo.c:1108/1156/1202/1340   (slash-separated line list, seen in prose)
# Group 1: path (must contain a '/' or start with a bare filename+extension,
# and end in a recognized source/header extension). Group 2: the first line
# number, which is all this check verifies -- a citation naming several lines
# is checked on its first (most commonly the one actually meant as "look
# here"); this keeps the regex simple and avoids over-claiming precision on
# the trailing numbers, which are frequently a related-but-different line in
# the same function rather than a strict list.
$pattern = '(?<path>[A-Za-z0-9_./\\-]+\.(?:c|h|cpp|hpp))(?::(?<line>\d+))'

$checked = 0
$stale = @()
$skippedNoFile = 0
$skippedAmbiguous = 0
$lineCountCache = @{}

# Index of bare-filename -> list of full paths, built ONCE up front (not per
# citation -- a per-citation repo-wide Get-ChildItem was measured to make
# this check take minutes on this tree). Only source/header extensions are
# indexed, matching the citation pattern below. A name with more than one
# hit is left ambiguous on purpose (see Resolve-ByBasename) rather than
# guessed at.
Write-Host "Indexing source/header files for bare-filename citation lookups..."
$basenameIndex = @{}
Get-ChildItem -Path $root -Recurse -File -Include *.c,*.h,*.cpp,*.hpp -ErrorAction SilentlyContinue |
    Where-Object {
        $_.FullName -notmatch '[\\/]build[\\/]' -and
        $_.FullName -notmatch '[\\/]node_modules[\\/]' -and
        $_.FullName -notmatch '[\\/]mykicadMcp[\\/]' -and
        $_.FullName -notmatch '[\\/]pdfMcp[\\/]' -and
        $_.FullName -notmatch '[\\/]\.[^\\/]+[\\/]'
    } |
    ForEach-Object {
        if (-not $basenameIndex.ContainsKey($_.Name)) { $basenameIndex[$_.Name] = New-Object System.Collections.Generic.List[string] }
        $basenameIndex[$_.Name].Add($_.FullName)
    }

function Resolve-ByBasename {
    param([string]$Name)
    if (-not $basenameIndex.ContainsKey($Name)) { return $null }
    $hits = $basenameIndex[$Name]
    if ($hits.Count -eq 1) { return $hits[0] }
    return $null
}

foreach ($doc in $docs) {
    $text = Get-Content -Path $doc.FullName -Raw
    if ([string]::IsNullOrEmpty($text)) { continue }

    $lineStarts = $null # lazy per-doc line index for reporting the doc line number
    foreach ($m in [regex]::Matches($text, $pattern)) {
        $relPath = $m.Groups['path'].Value -replace '\\', '/'
        $citedLine = [int]$m.Groups['line'].Value

        # Resolve: repo-root-relative first, then walk up from the doc's own
        # directory to the repo root trying each ancestor as a base -- this
        # repo's docs cite paths like `App/drivers/foo.c` from
        # `firmware/KilnFW/docs/ARCHITECTURE.md`, i.e. relative to the
        # firmware component root (the doc's grandparent), not the repo root
        # and not the doc's own directory. Walking ancestors generically
        # covers that without hardcoding `firmware/KilnFW`/`SaftyFW`/etc.
        $target = $null
        $winPath = $relPath -replace '/', '\'
        $candidateRoot = Join-Path $root $winPath
        if (Test-Path $candidateRoot -PathType Leaf) {
            $target = $candidateRoot
        } else {
            $dir = $doc.DirectoryName
            while ($true) {
                $candidate = Join-Path $dir $winPath
                if (Test-Path $candidate -PathType Leaf) {
                    $target = $candidate
                    break
                }
                $parent = Split-Path -Parent $dir
                if (-not $parent -or $parent -eq $dir -or $dir.Length -le $root.Length) { break }
                $dir = $parent
            }
        }

        if (-not $target -and ($relPath -notmatch '/')) {
            # Bare filename, no directory prefix (common in this repo's prose
            # once a path has already been given in surrounding text, e.g.
            # "`uart_bridge.c:1771`"). Resolve by a repo-wide basename search
            # -- but only if the name is unique repo-wide; an ambiguous name
            # is reported separately rather than guessed at, since guessing
            # wrong would produce a false "stale" against the wrong file.
            $byName = Resolve-ByBasename -Name $relPath
            if ($byName) {
                $target = $byName
            } else {
                $skippedAmbiguous++
                continue
            }
        }

        if (-not $target) {
            # Path doesn't resolve to a real file anywhere sensible. Could be
            # a citation into a third-party/vendored path this repo doesn't
            # carry, or a genuinely wrong path. Either way this check can't
            # safely call it "stale" (that implies the file used to match and
            # drifted) so it's counted separately, not as a failure -- keeps
            # the check's false-positive rate at the low bar this repo needs
            # for a check_*.ps1 to survive being run automatically.
            $skippedNoFile++
            continue
        }

        $checked++
        if (-not $lineCountCache.ContainsKey($target)) {
            $lineCountCache[$target] = (Get-Content -Path $target).Count
        }
        $lineCount = $lineCountCache[$target]
        if ($citedLine -gt $lineCount) {
            # Compute the doc's own line number for a useful error message.
            if ($null -eq $lineStarts) {
                $lineStarts = @()
                $acc = 0
                foreach ($l in ($text -split "`n")) {
                    $lineStarts += $acc
                    $acc += $l.Length + 1
                }
            }
            $docLine = 1
            for ($i = 0; $i -lt $lineStarts.Count; $i++) {
                if ($lineStarts[$i] -gt $m.Index) { break }
                $docLine = $i + 1
            }
            $relDoc = $doc.FullName.Substring($root.Length + 1) -replace '\\', '/'
            $stale += "${relDoc}:${docLine}: cites ${relPath}:${citedLine}, but ${relPath} has only ${lineCount} line(s)"
        }
    }
}

if ($checked -lt 10) {
    throw "check_doc_citations: only resolved $checked citation(s) against real files across $($docs.Count) doc(s) -- the citation regex or path resolution has probably gone blind. Update the pattern before trusting this result."
}

if ($stale.Count -gt 0) {
    Write-Host "DOC CITATION CHECK FAILED:" -ForegroundColor Red
    foreach ($s in $stale) {
        Write-Host "  $s" -ForegroundColor Red
    }
    Write-Host ""
    Write-Host "  A file:line citation past its target's current end-of-file is exactly the" -ForegroundColor Red
    Write-Host "  class of staleness the 2026-09-04 stale-claim audit found nearly everywhere" -ForegroundColor Red
    Write-Host "  in ARCHITECTURE.md after uart_bridge.c/profile_executor.c were split. Fix" -ForegroundColor Red
    Write-Host "  the citation to point at the symbol's current location." -ForegroundColor Red
    throw "$($stale.Count) doc citation(s) point past their target file's current end-of-file"
}

Write-Host "Doc citation check passed: $checked citation(s) verified in-bounds across $($docs.Count) doc(s) ($skippedNoFile unresolved path(s), $skippedAmbiguous ambiguous bare filename(s) skipped, not failed)."
exit 0
