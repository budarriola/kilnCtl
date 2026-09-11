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
  how correct they are.

  Parent-repo resolution is the default and requires no annotation -- this
  covers the overwhelming majority of the ~1,100 citations in this repo
  unchanged. A citation that refers to a SUBMODULE commit must say so
  explicitly with a `sub:<name>` tag immediately before the backtick, e.g.
  `sub:lvgl`85aa60d1``, where `<name>` is a submodule's leaf directory name
  (`lvgl`, `mykicadMcp`, `TFT35-SPI` as of this writing -- derived from
  .gitmodules, not hardcoded). Only that named submodule is tried; there is
  no other-submodule or parent fallback. An unknown `sub:` name, an
  uninitialized/missing submodule, or a hash that does not resolve in the
  named submodule are all reported as failures -- never silently swallowed.
  The `sub:` token requires a `\b` word boundary before it (so "notsub:lvgl"
  does NOT trigger it -- same widening class as the fabricated-marker fix
  below) and must sit with NO whitespace between the name and the backtick
  (`sub:lvgl`85aa60d1`` -- not `sub:lvgl \`85aa60d1\``); the two were
  mismatched in an earlier revision of this fix (pattern allowed whitespace,
  this comment said "immediately") and are now both the stricter, no-space
  form.

  KNOWN LIMITATION shared with the fabricated marker below: prose that
  quotes the `sub:<name>`\`hash\` or `fabricated`\`hash\` syntax ITSELF
  (e.g. a doc explaining this convention, as this comment block does) would
  be parsed as a real citation/exclusion if it lived in a scanned *.md file.
  This script does not parse fenced/inline code spans specially. Not
  believed to bite in practice -- doc prose describing these markers has so
  far always done so without the fully-formed backtick-hash token in
  scannable *.md prose (this very explanation lives in a .ps1 comment, which
  is not scanned) -- but a future doc author demonstrating the syntax in a
  *.md file should break the example (e.g. insert a space or use a
  non-hex/short placeholder) rather than write a literal working token.

  This replaced an earlier design (added in 79d93233) that tried EVERY
  initialized submodule automatically before failing any unresolved hash,
  with no way to tell from the output which repository (if any) actually
  matched. That silent, unscoped fallback meant a mistyped PARENT-repo hash
  that happened to collide with a real commit in some large submodule's
  history (lvgl's history is large) would pass this check with no hint that
  it resolved anywhere other than the intended repo -- worse than an
  unresolved citation, since it reads as verified. Confirmed 2026-09-10: the
  short hash `85aa60d` (a truncation of the real, single genuine submodule
  citation in this repo, `85aa60d1`, see docs/audits/esp_bring_up_to_head_2026-09-10.md)
  resolves in `firmware/KilnFW/components/lvgl` but nowhere in the parent
  repo, and the old code accepted it with output identical to a normal
  parent-repo pass -- no repository named anywhere.

  ILLUSTRATIVE / NOT-A-CITATION HASHES: prose sometimes needs to quote a
  hash-shaped token that is NOT a citation -- e.g. discussing a fabricated
  hash another session introduced by mistake ("not the fabricated
  `abc1234`"). To avoid such a quote being flagged as a dangling citation
  while not opening a loophole that lets a genuinely bad citation hide, the
  ONLY recognised marker is the literal WORD "fabricated" immediately
  preceding the backtick-quoted token (case-insensitive, e.g. "the
  fabricated `abc1234`"), matched with a leading `\b` word boundary so a
  larger word merely ending in "fabricated" (e.g. "notfabricated
  `<hash>`") does NOT trigger it -- confirmed by negative test 2026-09-10,
  which found the pre-`\b` regex DID wrongly exclude "notfabricated
  `<hash>`" as though it were marked, a real widening of the loophole this
  design otherwise closes. This is narrow by design: a real citation is
  never phrased as "the fabricated `<hash>`" -- that phrasing asserts the
  hash is fake, which is a claim a reviewer reading the prose would notice
  and challenge if it were being used to smuggle a real, wrong citation past
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

$hashPattern = '(\b(?i:fabricated)\s+)?(?:\b(?i:sub):([A-Za-z0-9_.\-]+))?`([0-9a-f]{7,40})`'
$failures = @()
$totalCitations = 0
$excludedFabricated = 0
$uniqueChecked = @{}
$resolvedInParent = 0
$resolvedInSubmodule = @{}

# Submodule paths (only ones actually initialized/checked out on disk are
# usable as a resolution target). See header, "SUBMODULE HASHES". Keyed by
# leaf directory name (what a `sub:<name>` tag names), not full path.
$submodulesByName = @{}
if (Test-Path '.gitmodules') {
    (git config -f .gitmodules --get-regexp '\.path$') |
        ForEach-Object { ($_ -split '\s+', 2)[1] } |
        Where-Object { Test-Path (Join-Path $_ '.git') } |
        ForEach-Object {
            $leaf = Split-Path -Leaf $_
            $submodulesByName[$leaf.ToLowerInvariant()] = $_
        }
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
            $subName = $m.Groups[2].Value
            $hash = $m.Groups[3].Value
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

            if ($subName) {
                # Explicitly declared as a submodule citation -- see header,
                # "SUBMODULE HASHES". Only the named submodule is tried; no
                # fallback to the parent repo or any other submodule.
                $subPath = $submodulesByName[$subName.ToLowerInvariant()]
                if (-not $subPath) {
                    $known = ($submodulesByName.Keys | Sort-Object) -join ', '
                    $failures += [PSCustomObject]@{
                        File = $file
                        Line = $lineNum
                        Hash = $hash
                        Text = $line.Trim()
                        Reason = "unknown sub: name '$subName' (known submodules: $known)"
                    }
                    continue
                }
                & git -C $subPath cat-file -e "$hash^{commit}" 2>$null 1>$null
                if ($LASTEXITCODE -eq 0) {
                    if (-not $resolvedInSubmodule.ContainsKey($subName)) { $resolvedInSubmodule[$subName] = 0 }
                    $resolvedInSubmodule[$subName]++
                    continue
                }
                $failures += [PSCustomObject]@{
                    File = $file
                    Line = $lineNum
                    Hash = $hash
                    Text = $line.Trim()
                    Reason = "declared sub:$subName but does not resolve in $subPath"
                }
                continue
            }

            # No sub: tag -- parent-repo resolution only (the default case,
            # covering the overwhelming majority of citations). No implicit
            # submodule fallback: see header, "SUBMODULE HASHES".
            & git cat-file -e "$hash^{commit}" 2>$null 1>$null
            if ($LASTEXITCODE -eq 0) { $resolvedInParent++; continue }

            $failures += [PSCustomObject]@{
                File = $file
                Line = $lineNum
                Hash = $hash
                Text = $line.Trim()
                Reason = 'does not resolve in the parent repo (add sub:<name> if this cites a submodule commit)'
            }
        }
    }
}

$subSummary = if ($resolvedInSubmodule.Count -gt 0) {
    ($resolvedInSubmodule.GetEnumerator() | Sort-Object Name | ForEach-Object { "$($_.Value) in $($_.Name)" }) -join ', '
} else {
    'none'
}
Write-Host "check_doc_hash_citations: $totalCitations citations, $($uniqueChecked.Count) unique hashes checked, $excludedFabricated excluded (marked fabricated)."
Write-Host "  resolved: $resolvedInParent in parent repo; via sub: tag: $subSummary."

if ($failures.Count -gt 0) {
    Write-Host "FAIL: $($failures.Count) cited hash(es) do not resolve to a commit:" -ForegroundColor Red
    foreach ($f in $failures) {
        Write-Host ("  {0}:{1}: `{2}` -- {3}" -f $f.File, $f.Line, $f.Hash, $f.Reason) -ForegroundColor Red
        Write-Host ("      {0}" -f $f.Text) -ForegroundColor Red
    }
    exit 1
}

Write-Host "PASS: every cited hash resolves to a commit in the repository it names (parent by default, or the declared sub: submodule)." -ForegroundColor Green
exit 0
