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

  BLOB HASHES (added 2026-09-16): standing practice in this repo, after a
  negative test sabotages a tracked file and restores it by hand, is to run
  `git hash-object` on the restored file and compare it against
  `git rev-parse HEAD:<path>` to prove the restoration is byte-exact (see
  CLAUDE.md's negative-test guidance). That comparison produces a BLOB hash,
  not a commit hash -- `cat-file -e <hash>^{commit}` can never resolve it no
  matter how correct it is, because a blob is not a commit. Citing one this
  way is real, load-bearing evidence and should not be deleted or vaguely
  elided to dodge this check (that happened once, in
  docs/audits/release_gate_vacuity_audit_2026-09-16g.md, which is what
  prompted this fix -- the doc tried to avoid tripping this check by
  "eliding" the hash down to an 8-char prefix, but kept it backtick-quoted,
  so it still matched and still failed, as a commit, which it never was).
  Tag it explicitly with `blob:<path>` immediately before the backtick, no
  whitespace (matching the `sub:` convention), e.g.
  `blob:firmware/hwAbstraction/common/hal_status.c`\`cf1dbafb\``. This is
  graded MORE strictly than plain existence: the script recomputes
  `git rev-parse HEAD:<path>` itself and checks the cited hash is a prefix
  of it, so the citation is confirmed to match what HEAD actually has at
  that exact path -- not merely that some blob with that id exists
  somewhere in the object database (a coincidental collision at an unrelated
  path would still fail). A missing path, or a cited prefix that does not
  match HEAD's actual blob there, is a hard FAIL naming both.

  Parent-repo resolution is the default and requires no annotation -- this
  covers the overwhelming majority of the ~1,100 citations in this repo
  unchanged. A citation that refers to a SUBMODULE commit must say so
  explicitly with a `sub:<name>` tag immediately before the backtick, e.g.
  `sub:lvgl`85aa60d1``, where `<name>` is a submodule's leaf directory name
  (`lvgl`, `mykicadMcp`, `TFT35-SPI` as of this writing -- derived from
  .gitmodules, not hardcoded). Only that named submodule is tried; there is
  no other-submodule or parent fallback. An unknown `sub:` name, or a hash
  that does not resolve in an INITIALIZED named submodule, are reported as
  failures -- never silently swallowed.

  UNINITIALIZED SUBMODULES ARE UNGRADED, AND UNGRADED IS NOT A PASS
  (corrected 2026-09-16). A `sub:` tag naming a submodule that .gitmodules
  declares but that is not checked out in this worktree cannot be graded
  either way: there is no history present to resolve the hash against. That
  is an environment gap, not a documentation defect, and it fired on every
  clean-worktree verification run, where it was misreported as a real
  documentation regression more than once. Such citations are reported
  individually as SKIP lines naming the file, line, hash, submodule and the
  `git submodule update --init` command that would let them be graded.

  WHAT WAS WRONG, AND HOW. The revision that added this skip also asserted,
  immediately below, that it "never swallows a bad citation". It did. The
  skip branch did not affect the exit code at all, so the script printed its
  green PASS line -- "every cited hash resolves to a commit in the repository
  it names" -- and exited 0 while holding citations it had never checked.
  Demonstrated: a fabricated 7-hex-char hash tagged sub:mykicadMcp, planted
  in docs/REPO_LAYOUT.md in a worktree where that submodule is uninitialized,
  produced exit 0 and PASS, visible only as one SKIP line among the output.
  `tools/mykicadMcp` is uninitialized in every fresh clone and on every CI
  runner, so that was the NORMAL case, not an edge case.

  THE FIX, AND WHY THIS DIRECTION. Ungraded `sub:` citations now set the exit
  code to 3 -- the runner's "could not grade" status -- unless
  -AllowUngradedSubmodules is passed, which restores the old exit-0 behaviour
  for a caller that has deliberately accepted the gap. The alternative the
  review named, resolving `sub:` citations against the submodule's declared
  REMOTE instead of its on-disk clone, was rejected: `git ls-remote` cannot
  test an arbitrary commit's existence (it lists refs, and a cited commit is
  usually not itself a ref tip), so actually grading that way means fetching
  each submodule's history over the network from inside a check that is run
  dozens of times a day, turning a fast offline check into a slow, flaky,
  network-dependent one -- and still failing closed exactly when the network
  is down, which is the same exit-3 outcome by a much more expensive route.
  Exit 3, not exit 1: the citation is not known to be BAD, only unchecked,
  and exit 1's "these hashes do not resolve" is a claim this script has not
  earned. tools/run_all_checks.ps1 already fails the overall run on a SKIP by
  default (with -AllowSkips to opt out), so the two knobs compose: the
  default is honest at both levels, and either level can be relaxed
  deliberately.

  The earlier revision's argument against exit 3 -- that it "would both fail
  the run AND stop grading the ~2,300 citations that are perfectly gradeable"
  -- was half right and wholly the wrong conclusion. This script still grades
  every one of those citations and still reports every real failure as exit
  1; only the final status changes. Failing the run is the POINT when a
  citation went unchecked. And the cost is avoidable rather than endured: the
  single genuine sub: citation in this repo names `lvgl`, which a KilnFW
  worktree must initialize to build anyway, so a properly provisioned tree
  has zero ungraded citations and this check is green by default.

  One boundary is unchanged: an INITIALIZED submodule whose citation does not
  resolve is still a hard FAIL (exit 1), and a `sub:` name that is not
  declared in .gitmodules at all is still a hard FAIL -- the ungraded path is
  reachable only for a name .gitmodules itself declares.
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
  Exit code 0 = every citation was graded and all of them resolve.
  Exit code 1 = one or more cited hashes do not resolve.
  Exit code 3 = every graded citation resolved, but one or more `sub:`
                citations could not be graded because the named submodule is
                not checked out here. Pass -AllowUngradedSubmodules to treat
                that as exit 0 instead.

.PARAMETER AllowUngradedSubmodules
  Restore the pre-2026-09-16 behaviour of exiting 0 when `sub:` citations
  naming an uninitialized submodule were skipped. Only pass this when the
  caller has deliberately accepted that those citations go unchecked -- the
  default refuses to report PASS over citations it never looked at.
#>

param(
    [switch]$AllowUngradedSubmodules
)

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
    @{ File = 'docs/audits/adaptive_fuzzy_evaluation_progress_2026-09-14.md'; Hash = 'de5743fda54bab70b61d32b7d6d06722' }  # md5 of a build artifact (factorial_of1.tsv), not a git commit
    @{ File = 'docs/audits/adaptive_fuzzy_evaluation_progress_2026-09-14.md'; Hash = '1be73e04757beb9c9499e05875057f8f' }  # md5 of a build artifact (factorial_of1.tsv, three-arm inertness proof), not a git commit
    @{ File = 'docs/audits/adaptive_fuzzy_evaluation_progress_2026-09-14.md'; Hash = 'e6f38410942ac28eb260864604168932' }  # md5 of a build artifact (factorial_of1.tsv, before/after poison comparison), not a git commit
)

$hashPattern = '(\b(?i:fabricated)\s+)?(?:\b(?i:sub):([A-Za-z0-9_.\-]+)|\b(?i:blob):([A-Za-z0-9_./\-]+))?`([0-9a-f]{7,40})`'
$failures = @()
$totalCitations = 0
$excludedFabricated = 0
$uniqueChecked = @{}
$resolvedInParent = 0
$resolvedInSubmodule = @{}
$resolvedInBlob = 0

# Citations that name a declared-but-not-checked-out submodule. Reported, but
# never counted as failures -- see header, "UNINITIALIZED SUBMODULES ARE
# SKIPPED, NOT FAILED".
$skippedUninitialized = @()

# Submodule paths, keyed by leaf directory name (what a `sub:<name>` tag
# names), not full path. TWO maps, because "names a submodule that is not
# checked out here" and "names something that is not a submodule at all" are
# different outcomes -- see header, "SUBMODULE HASHES".
#   $declaredSubmodulesByName -- every submodule .gitmodules declares, checked
#     out or not. A sub: tag naming one of these in a worktree where it is
#     absent is SKIPPED (an environment gap, not a doc defect).
#   $submodulesByName -- only those actually present on disk, i.e. the ones a
#     hash can genuinely be resolved against. A sub: tag naming one of THESE
#     that does not resolve is still a hard FAIL.
# A sub: tag in neither map is an unknown name and is still a hard FAIL.
$declaredSubmodulesByName = @{}
$submodulesByName = @{}
if (Test-Path '.gitmodules') {
    (git config -f .gitmodules --get-regexp '\.path$') |
        ForEach-Object { ($_ -split '\s+', 2)[1] } |
        ForEach-Object {
            $leaf = (Split-Path -Leaf $_).ToLowerInvariant()
            $declaredSubmodulesByName[$leaf] = $_
            if (Test-Path (Join-Path $_ '.git')) {
                $submodulesByName[$leaf] = $_
            }
        }
}

# PASS 1: collect every candidate citation, in document order, with no git
# calls. (Previously this loop spawned one `git cat-file` process per citation
# -- ~2,900 spawns -- which ran for 30+ minutes on a loaded machine.)
$citations = New-Object System.Collections.Generic.List[object]
foreach ($file in $files) {
    if (-not (Test-Path $file)) { continue }
    $lineNum = 0
    foreach ($line in [System.IO.File]::ReadAllLines((Join-Path $repoRoot $file), [System.Text.Encoding]::UTF8)) {
        $lineNum++
        foreach ($m in [regex]::Matches($line, $hashPattern)) {
            $citations.Add([PSCustomObject]@{
                File = $file
                Line = $lineNum
                Text = $line
                Fabricated = $m.Groups[1].Success
                SubName = $m.Groups[2].Value
                BlobPath = $m.Groups[3].Value
                Hash = $m.Groups[4].Value
                Result = $null
            })
        }
    }
}

# Resolves a list of revision expressions in ONE git process. Returns, per
# input and in order, the `git cat-file --batch-check` output line.
function Invoke-BatchCheck {
    param([string[]]$Exprs, [string]$RepoDir)
    if (-not $Exprs -or $Exprs.Count -eq 0) { return @() }
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = 'git'
    $repoArg = ''
    if ($RepoDir) { $repoArg = '-C "' + $RepoDir + '" ' }
    $psi.Arguments = $repoArg + 'cat-file --batch-check'
    $psi.UseShellExecute = $false
    # Set-Location does not move the .NET process cwd; pin it so relative -C
    # paths and the repo git sees are the ones this script resolved.
    $psi.WorkingDirectory = (Get-Location).ProviderPath
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = New-Object System.Text.UTF8Encoding($false)
    $p = [System.Diagnostics.Process]::Start($psi)
    # Raw UTF-8 bytes, no BOM (PowerShell's pipe to a native exe prepends one,
    # which corrupts the first line). Write stdin from a thread-free path by
    # draining stdout asynchronously so a large batch cannot deadlock.
    $stdoutTask = $p.StandardOutput.ReadToEndAsync()
    $stderrTask = $p.StandardError.ReadToEndAsync()
    # The process's stdin writer may emit a UTF-8 BOM, which git reads as part
    # of the FIRST line only. Lead with a sacrificial "HEAD" line and drop its
    # answer, so every real expression is read exactly as written.
    $p.StandardInput.Write(('HEAD' + [char]10 + ($Exprs -join [string][char]10) + [char]10))
    $p.StandardInput.Close()
    $p.WaitForExit()
    $out = @($stdoutTask.Result -split "`r?`n" | Where-Object { $_ -ne '' })
    $out = @($out | Select-Object -Skip 1)
    if ($out.Count -ne $Exprs.Count) {
        throw "git cat-file --batch-check returned $($out.Count) lines for $($Exprs.Count) inputs (repo: '$RepoDir')"
    }
    return $out
}
function Test-IsCommitLine { param([string]$L) return ($L -match ' commit \d+$') }

# Stage which citations need which lookup. Fabricated-marked and known
# false-positive tokens are never looked up.
$parentIdx = New-Object System.Collections.Generic.List[int]
$blobIdx = New-Object System.Collections.Generic.List[int]
$subIdx = @{}   # sub path -> list of citation indexes
for ($i = 0; $i -lt $citations.Count; $i++) {
    $c = $citations[$i]
    if ($c.Fabricated) { continue }
    $f = $c.File; $h = $c.Hash
    if ($KnownNonHashFalsePositives | Where-Object { $_.File -eq $f -and $_.Hash -eq $h }) { continue }
    if ($c.SubName) {
        $p = $submodulesByName[$c.SubName.ToLowerInvariant()]
        if ($p) {
            if (-not $subIdx.ContainsKey($p)) { $subIdx[$p] = New-Object System.Collections.Generic.List[int] }
            $subIdx[$p].Add($i)
        }
    } elseif ($c.BlobPath) {
        $blobIdx.Add($i)
    } else {
        $parentIdx.Add($i)
    }
}

# PASS 2: one git process per repository (parent: commits and blob paths; one
# per initialized submodule). Order is preserved, so line N answers input N.
$lines = @(Invoke-BatchCheck -Exprs @($parentIdx | ForEach-Object { "$($citations[$_].Hash)^{commit}" }))
for ($k = 0; $k -lt $parentIdx.Count; $k++) { $citations[$parentIdx[$k]].Result = (Test-IsCommitLine $lines[$k]) }

$lines = @(Invoke-BatchCheck -Exprs @($blobIdx | ForEach-Object { "HEAD:$($citations[$_].BlobPath)" }))
for ($k = 0; $k -lt $blobIdx.Count; $k++) {
    # "<oid> <type> <size>" when the path exists at HEAD; the old
    # per-citation code only required existence, then compared against
    # rev-parse HEAD:<path>.
    if ($lines[$k] -match '^([0-9a-f]{40}) \w+ \d+$') { $citations[$blobIdx[$k]].Result = $Matches[1] }
    else { $citations[$blobIdx[$k]].Result = '' }
}

foreach ($p in $subIdx.Keys) {
    $idxs = $subIdx[$p]
    $lines = @(Invoke-BatchCheck -Exprs @($idxs | ForEach-Object { "$($citations[$_].Hash)^{commit}" }) -RepoDir $p)
    for ($k = 0; $k -lt $idxs.Count; $k++) { $citations[$idxs[$k]].Result = (Test-IsCommitLine $lines[$k]) }
}

# PASS 3: grade in document order (same verdicts and message text as the
# original per-citation loop).
foreach ($c in $citations) {
    $file = $c.File; $lineNum = $c.Line; $line = $c.Text
    $subName = $c.SubName; $blobPath = $c.BlobPath; $hash = $c.Hash
    if ($c.Fabricated) {
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
        # "SUBMODULE HASHES". Only the named submodule is tried; no fallback
        # to the parent repo or any other submodule.
        $subPath = $submodulesByName[$subName.ToLowerInvariant()]
        if (-not $subPath) {
            $declaredPath = $declaredSubmodulesByName[$subName.ToLowerInvariant()]
            if ($declaredPath) {
                # Declared in .gitmodules but not checked out: this ONE
                # citation cannot be graded either way (see header). An
                # unknown name still falls through to the failure below.
                $skippedUninitialized += [PSCustomObject]@{
                    File = $file
                    Line = $lineNum
                    Hash = $hash
                    Sub  = $subName
                    Path = $declaredPath
                }
                continue
            }
            $known = ($declaredSubmodulesByName.Keys | Sort-Object) -join ', '
            if (-not $known) { $known = '(none declared in .gitmodules)' }
            $failures += [PSCustomObject]@{
                File = $file
                Line = $lineNum
                Hash = $hash
                Text = $line.Trim()
                Reason = "unknown sub: name '$subName' (declared submodules: $known)"
            }
            continue
        }
        if ($c.Result) {
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

    if ($blobPath) {
        # Explicitly declared as a BLOB citation -- see header, "BLOB HASHES".
        # Graded by checking the cited hash is a prefix of HEAD's blob id at
        # that exact path (stricter than mere existence).
        if ($null -eq $c.Result -or $c.Result -eq '') {
            $failures += [PSCustomObject]@{
                File = $file
                Line = $lineNum
                Hash = $hash
                Text = $line.Trim()
                Reason = "declared blob:$blobPath but that path does not exist at HEAD"
            }
            continue
        }
        $actualBlob = $c.Result
        if ($actualBlob.StartsWith($hash, [System.StringComparison]::Ordinal)) {
            $resolvedInBlob++
            continue
        }
        $failures += [PSCustomObject]@{
            File = $file
            Line = $lineNum
            Hash = $hash
            Text = $line.Trim()
            Reason = "declared blob:$blobPath but HEAD's blob there is $actualBlob, which does not start with the cited hash"
        }
        continue
    }

    # No sub: or blob: tag -- parent-repo resolution only. No implicit
    # submodule fallback: see header, "SUBMODULE HASHES".
    if ($c.Result) { $resolvedInParent++; continue }

    $failures += [PSCustomObject]@{
        File = $file
        Line = $lineNum
        Hash = $hash
        Text = $line.Trim()
        Reason = 'does not resolve in the parent repo (add sub:<name> if this cites a submodule commit)'
    }
}

$subSummary = if ($resolvedInSubmodule.Count -gt 0) {
    ($resolvedInSubmodule.GetEnumerator() | Sort-Object Name | ForEach-Object { "$($_.Value) in $($_.Name)" }) -join ', '
} else {
    'none'
}
Write-Host "check_doc_hash_citations: $totalCitations citations, $($uniqueChecked.Count) unique hashes checked, $excludedFabricated excluded (marked fabricated)."
Write-Host "  resolved: $resolvedInParent in parent repo; via sub: tag: $subSummary; via blob: tag: $resolvedInBlob."

if ($skippedUninitialized.Count -gt 0) {
    # These citations were NEVER CHECKED. Reporting PASS/exit 0 over them is
    # what let a fabricated sub:mykicadMcp hash sail through -- see header,
    # "UNINITIALIZED SUBMODULES ARE UNGRADED, AND UNGRADED IS NOT A PASS".
    $bySub = ($skippedUninitialized | Group-Object Sub | Sort-Object Name | ForEach-Object {
        "$($_.Count) naming sub:$($_.Name)"
    }) -join ', '
    Write-Host ("SKIP: {0} submodule citation(s) not graded ({1}) -- the named submodule is declared in .gitmodules but is not checked out in this worktree, so there is no history here to resolve them against. This is an environment gap, not a documentation defect." -f $skippedUninitialized.Count, $bySub) -ForegroundColor Yellow
    foreach ($s in $skippedUninitialized) {
        Write-Host ("  SKIP {0}:{1}: hash {2} declared sub:{3} -- submodule not initialized at {4}; run 'git submodule update --init -- {4}' to grade this citation." -f $s.File, $s.Line, $s.Hash, $s.Sub, $s.Path) -ForegroundColor Yellow
    }
}

if ($failures.Count -gt 0) {
    Write-Host "FAIL: $($failures.Count) cited hash(es) do not resolve to a commit:" -ForegroundColor Red
    foreach ($f in $failures) {
        Write-Host ("  {0}:{1}: `{2}` -- {3}" -f $f.File, $f.Line, $f.Hash, $f.Reason) -ForegroundColor Red
        Write-Host ("      {0}" -f $f.Text) -ForegroundColor Red
    }
    exit 1
}

if ($skippedUninitialized.Count -gt 0) {
    if ($AllowUngradedSubmodules) {
        Write-Host ("PASS (with {0} ungraded submodule citation(s), allowed by -AllowUngradedSubmodules): every citation this run actually graded resolves to a commit in the repository it names." -f $skippedUninitialized.Count) -ForegroundColor Green
        exit 0
    }
    Write-Host ("SKIP: {0} citation(s) were NOT graded (listed above). Every citation that was graded resolves, but this check will not report PASS over citations it never checked -- an ungraded citation is exactly how a fabricated hash passes unnoticed. Initialize the named submodule(s) to grade them, or pass -AllowUngradedSubmodules to accept the gap deliberately." -f $skippedUninitialized.Count) -ForegroundColor Yellow
    exit 3
}

Write-Host "PASS: every cited hash resolves to a commit in the repository it names (parent by default, or the declared sub: submodule)." -ForegroundColor Green
exit 0
