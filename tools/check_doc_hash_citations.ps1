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

$hashPattern = '`([0-9a-f]{7,40})`'
$failures = @()
$totalCitations = 0
$uniqueChecked = @{}

foreach ($file in $files) {
    if (-not (Test-Path $file)) { continue }
    $lineNum = 0
    Get-Content -LiteralPath $file -Encoding UTF8 | ForEach-Object {
        $lineNum++
        $line = $_
        $matches_ = [regex]::Matches($line, $hashPattern)
        foreach ($m in $matches_) {
            $hash = $m.Groups[1].Value
            # Skip tokens that are pure decimal-looking or too short to be a
            # meaningful abbreviated SHA (git's own minimum is 4, but this repo's
            # convention is >=7 per the pattern above already).
            $totalCitations++
            $uniqueChecked[$hash] = $true

            $isKnownFalsePositive = $KnownNonHashFalsePositives | Where-Object {
                $_.File -eq $file -and $_.Hash -eq $hash
            }
            if ($isKnownFalsePositive) { continue }

            & git cat-file -e "$hash^{commit}" 2>$null 1>$null
            if ($LASTEXITCODE -ne 0) {
                $failures += [PSCustomObject]@{
                    File = $file
                    Line = $lineNum
                    Hash = $hash
                    Text = $line.Trim()
                }
            }
        }
    }
}

Write-Host "check_doc_hash_citations: $totalCitations citations, $($uniqueChecked.Count) unique hashes checked."

if ($failures.Count -gt 0) {
    Write-Host "FAIL: $($failures.Count) cited hash(es) do not resolve to a commit:" -ForegroundColor Red
    foreach ($f in $failures) {
        Write-Host ("  {0}:{1}: `{2}` -- {3}" -f $f.File, $f.Line, $f.Hash, $f.Text) -ForegroundColor Red
    }
    exit 1
}

Write-Host "PASS: every cited hash resolves to a commit in this repository." -ForegroundColor Green
exit 0
