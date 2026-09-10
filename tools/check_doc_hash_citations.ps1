<#
.SYNOPSIS
  Validates that every git hash cited in tracked documentation actually exists
  in this repository's history.

.DESCRIPTION
  Scans tracked *.md files (CLAUDE.md, ROADMAP.md, docs/**, firmware/*/docs/**,
  firmware/*/TODO.md, *_PLAN.md, docs/audits/**, etc.) for tokens that look like
  cited commit hashes and checks each one resolves via
  `git cat-file -e <hash>^{commit}`.

  MATCHING RULE (to avoid false positives from part numbers, CRCs, register
  values, and other incidental hex-looking strings that show up constantly in
  this codebase's docs): a candidate must be a run of 7-40 lowercase hex
  characters ENCLOSED IN BACKTICKS, e.g. `51e1ef5`. Bare hex tokens with no
  backticks are ignored. This convention was chosen because every hash
  citation found in this repo's docs during the 2026-09-07 audit was
  backtick-quoted; requiring the word "commit" on the same line was tried and
  rejected -- fewer than 15% of real citations in this repo actually say
  "commit" nearby (most just read like "Fixed by `51e1ef5` and flashed."), so
  that stricter rule silently dropped ~85% of real citations, which is worse
  than the false positives it would have avoided.

  Backtick-plus-hex is not perfectly precise: a backtick-quoted part number or
  numeric constant that happens to be all lowercase-hex-alphabet characters
  will still match and will fail this check if it does not happen to also be
  a valid git object. Two such known false positives were found in the
  2026-09-07 audit and are carried in $KnownNonHashFalsePositives below,
  keyed by (file, token), so this check stays green without weakening the
  backtick rule for everything else. Do not add to that list to silence a
  REAL wrong/typo'd hash -- fix the doc instead. Only add a genuinely
  non-hash token (an MPN, a register value, a CRC) that a human confirmed is
  not meant to be a commit reference.

  SUBMODULE HASHES: a citation can name a commit that lives in a submodule's
  own history (e.g. `firmware/KilnFW/components/lvgl`) rather than this
  repo's. Those hashes will never resolve against the parent repo no matter
  how correct they are, so before failing a hash this script also tries it
  against every submodule listed in .gitmodules (only submodules that are
  actually initialized on disk are tried; an uninitialized submodule is
  silently skipped for this purpose, not treated as a failure). This applies
  automatically to any doc, not just ones about lvgl -- no per-doc
  annotation is needed for a real submodule commit hash.

  ILLUSTRATIVE / NOT-A-CITATION HASHES: prose sometimes needs to quote a
  hash-shaped token that is NOT a citation -- e.g. discussing a fabricated
  hash another session introduced by mistake ("not the fabricated
  `abc1234`"). To avoid such a quote being flagged as a dangling citation
  while not opening a loophole that lets a genuinely bad citation hide, the
  ONLY recognised marker is the literal word "fabricated" immediately
  preceding the backtick-quoted token (case-insensitive, e.g. "the
  fabricated `abc1234`"). This is narrow by design: a real citation is never
  phrased as "the fabricated `<hash>`" -- that phrasing asserts the hash is
  fake, which is a claim a reviewer reading the prose would notice and
  challenge if it were being used to smuggle a real, wrong citation past
  this check. Tokens matched this way are reported separately as "excluded
  (marked fabricated)" and are never counted as citations or checked for
  existence.

  This script proves EXISTENCE only. It does not and cannot mechanically
  verify that a hash's commit actually supports the claim made about it
  (topical relevance) -- that requires reading the commit's diff/subject
  against the surrounding prose and is a human (or agent) judgment call. See
  e.g. `e584067f` having been cited in TODO.md for an unrelated
  firing-history-blob fix (wrong topic, but a real, existing commit) --
  fixed in c672a2c5, and NOT something an existence check could have caught.

  WHAT THIS CHECK DOES NOT DO: it only proves the hash EXISTS as a commit in
  this repository. It does NOT check that the commit's subject/diff actually
  supports the claim being made about it (e.g. "fixed by `abc1234`" where
  abc1234 is a real commit but touches something unrelated). That is a
  semantic/topical judgment call and is NOT something this script attempts --
  a wrong-but-existing hash citation (as found for `e584067f` in TODO.md,
  fixed in c672a2c5) requires a human (or an agent) to compare the commit's
  actual changed paths/subject against the surrounding prose's claim. This
  script is a floor (no dangling/typo'd/nonexistent hashes), not a ceiling.

.NOTES
  Registered for discovery by tools/run_all_checks.ps1 (matches check_*.ps1).
  Exit code 0 = all cited hashes resolve. Exit code 1 = one or more do not.
#>

$ErrorActionPreference = 'Continue'

$repoRoot = git rev-parse --show-toplevel
if (-not $repoRoot) {
    Write-Host "Not inside a git repository." -ForegroundColor Red
    exit 1
}
Set-Location $repoRoot

# Tracked markdown files only (respects .gitignore, avoids build output / worktrees).
$files = git ls-files -- '*.md' '*.MD' | Where-Object {
    $_ -notmatch '^\.claude/worktrees/'
}

# Known non-hash tokens that happen to be all lowercase-hex-alphabet and
# backtick-quoted (part numbers, MPNs, register/CRC values). See header.
$KnownNonHashFalsePositives = @(
    @{ File = 'docs/CONTACTOR_FEEDBACK_OPTIONS.md'; Hash = '1935161' }      # Phoenix connector part number
    @{ File = 'hardware/mainBoard/todo.md'; Hash = '74269244182' }          # ferrite bead MPN
)

$hashPattern = '((?i:fabricated)\s+)?`([0-9a-f]{7,40})`'
$failures = @()
$totalCitations = 0
$excludedFabricated = 0
$uniqueChecked = @{}

# Submodule paths (only ones actually initialized/checked out on disk are
# usable as a resolution target). See header, "SUBMODULE HASHES".
$submodulePaths = @()
if (Test-Path '.gitmodules') {
    $submodulePaths = (git config -f .gitmodules --get-regexp '\.path$') |
        ForEach-Object { ($_ -split '\s+', 2)[1] } |
        Where-Object { Test-Path (Join-Path $_ '.git') }
}

foreach ($file in $files) {
    if (-not (Test-Path $file)) { continue }
    $lineNum = 0
    Get-Content -LiteralPath $file -Encoding UTF8 | ForEach-Object {
        $lineNum++
        $line = $_
        $matches_ = [regex]::Matches($line, $hashPattern)
        foreach ($m in $matches_) {
            $isFabricatedMarker = $m.Groups[1].Success
            $hash = $m.Groups[2].Value
            if ($isFabricatedMarker) {
                # Marked as an illustrative/not-a-citation token -- see header.
                $excludedFabricated++
                continue
            }

            $totalCitations++
            $uniqueChecked[$hash] = $true

            $isKnownFalsePositive = $KnownNonHashFalsePositives | Where-Object {
                $_.File -eq $file -and $_.Hash -eq $hash
            }
            if ($isKnownFalsePositive) { continue }

            & git cat-file -e "$hash^{commit}" 2>$null 1>$null
            if ($LASTEXITCODE -eq 0) { continue }

            # Not in the parent repo -- try each initialized submodule before
            # declaring this a dangling citation. See header, "SUBMODULE HASHES".
            $resolvedInSubmodule = $false
            foreach ($subPath in $submodulePaths) {
                & git -C $subPath cat-file -e "$hash^{commit}" 2>$null 1>$null
                if ($LASTEXITCODE -eq 0) { $resolvedInSubmodule = $true; break }
            }
            if ($resolvedInSubmodule) { continue }

            $failures += [PSCustomObject]@{
                File = $file
                Line = $lineNum
                Hash = $hash
                Text = $line.Trim()
            }
        }
    }
}

Write-Host "check_doc_hash_citations: $totalCitations citations, $($uniqueChecked.Count) unique hashes checked, $excludedFabricated excluded (marked fabricated)."

if ($failures.Count -gt 0) {
    Write-Host "FAIL: $($failures.Count) cited hash(es) do not resolve to a commit:" -ForegroundColor Red
    foreach ($f in $failures) {
        Write-Host ("  {0}:{1}: `{2}` -- {3}" -f $f.File, $f.Line, $f.Hash, $f.Text) -ForegroundColor Red
    }
    exit 1
}

Write-Host "PASS: every cited hash resolves to a commit in this repository." -ForegroundColor Green
exit 0
