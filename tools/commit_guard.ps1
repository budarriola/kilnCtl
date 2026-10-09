# commit_guard.ps1 -- guard a commit against the stale-working-copy trap
# before it happens, not after.
#
# WHY THIS EXISTS. `git commit -o <path>` (and `git commit --amend` without
# a pathspec) commits the WORKING COPY of the named path, WHOLE -- not your
# edit specifically. In a tree several concurrent sessions are editing, a
# working copy that is stale relative to origin/main (e.g. read before
# someone else's commit landed, or a build/checkout tool silently restored
# an older version) silently REVERTS everything anyone else added to that
# file when committed this way. Real incidents in this project:
#   - a stale doc commit reverted 113 lines of another session's work in a
#     documentation file
#   - a `git commit --amend` without a pathspec pushed a 1067-line revert
#     of live work
#
# WHAT THIS DOES. For each path you are about to commit, compares the
# CONTENT you are about to commit (via `git hash-object <path>`, i.e. what
# is actually on disk right now) against `git rev-parse origin/main:<path>`
# (what origin/main currently has). If they differ, it is not automatically
# wrong -- that's what an edit looks like -- but the burden is on the
# caller to confirm every difference is deliberately theirs. It also runs
# `git diff --numstat origin/main -- <path>` (working tree vs origin/main)
# to report insertion/deletion counts, and flags any path whose counts
# exceed an expected size the caller must declare up front: the real 113-
# line incident's insertion/deletion counts were the available tell and
# were read past without anyone actually looking at them.
#
# USAGE
#   powershell -ExecutionPolicy Bypass -File tools\commit_guard.ps1 `
#       -Path CLAUDE.md -ExpectedMaxLines 40
#
#   Multiple paths, each may have its own expected size:
#   powershell ... -Path CLAUDE.md,docs\MCP_SERVERS.md -ExpectedMaxLines 40,120
#
#   -Confirm is the explicit override: without it, ANY difference from
#   origin/main is refused by default (even a small, correct one) until the
#   caller has looked at the diff and re-runs with -Confirm to proceed.
#
# EXIT CODES:
#   0 -- every path matches origin/main exactly (nothing to confirm), OR
#        differences exist and -Confirm was passed and no path exceeded its
#        declared -ExpectedMaxLines budget.
#   1 -- refused: differences exist and -Confirm was not passed, OR a path's
#        actual insertions+deletions exceed its declared -ExpectedMaxLines,
#        OR bad usage.
#
# This is a workflow guard invoked by hand immediately before a commit, not
# a repository-health check with a standing assertion about the tree in its
# current state -- it is not wired into run_all_checks.ps1 for the same
# reason worktree_mint.ps1 and push_verify.ps1 are not.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string[]]$Path,

    [int[]]$ExpectedMaxLines,

    [switch]$Confirm,

    [string]$Branch = "origin/dev"
)

$ErrorActionPreference = "Continue"

function Get-RepoRoot {
    $top = git rev-parse --show-toplevel 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $top) {
        Write-Host "ERROR: not inside a git repository." -ForegroundColor Red
        exit 1
    }
    return ($top -replace '/', '\')
}

$repoRoot = Get-RepoRoot

# F8: compare against the CURRENT remote branch, never a stale local tracking ref.
$gRemote = "origin"
$gBranchName = $Branch
$gRemoteList = @(git -C $repoRoot remote 2>$null)
if ($Branch -match '^([^/]+)/(.+)$' -and ($gRemoteList -contains $Matches[1])) { $gRemote = $Matches[1]; $gBranchName = $Matches[2] }
git -C $repoRoot fetch $gRemote $gBranchName *>$null
if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: git fetch $gRemote $gBranchName failed (exit $LASTEXITCODE); refusing to compare against a stale $Branch." -ForegroundColor Red
    exit 1
}

if ($null -ne $ExpectedMaxLines -and $ExpectedMaxLines.Count -ne $Path.Count) {
    Write-Host "ERROR: -ExpectedMaxLines, if given, must have one entry per -Path (got $($ExpectedMaxLines.Count) for $($Path.Count) paths)." -ForegroundColor Red
    exit 1
}

$anyDiff = $false
$anyBudgetExceeded = $false
$rows = @()

for ($i = 0; $i -lt $Path.Count; $i++) {
    $p = $Path[$i]
    $budget = if ($null -ne $ExpectedMaxLines -and $ExpectedMaxLines.Count -gt $i) { $ExpectedMaxLines[$i] } else { $null }

    # Repo-relative path is what git wants for both hash-object and the
    # origin/main:<path> object form.
    $full = Join-Path $repoRoot $p
    if (-not (Test-Path $full)) {
        Write-Host "ERROR: '$p' does not exist in the working tree." -ForegroundColor Red
        exit 1
    }

    $workingHash = git -C $repoRoot hash-object -- $p 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $workingHash) {
        Write-Host "ERROR: git hash-object failed for '$p'." -ForegroundColor Red
        exit 1
    }

    $originHash = git -C $repoRoot rev-parse "${Branch}:${p}" 2>$null
    $originExists = ($LASTEXITCODE -eq 0)

    if (-not $originExists) {
        Write-Host ("{0}: NEW to {1} (no origin version to compare against)." -f $p, $Branch) -ForegroundColor Yellow
        $rows += [pscustomobject]@{ Path = $p; Status = "new"; Ins = "-"; Del = "-" }
        continue
    }

    if ($workingHash -eq $originHash) {
        Write-Host ("{0}: UNCHANGED from {1} -- nothing to confirm." -f $p, $Branch) -ForegroundColor Green
        $rows += [pscustomobject]@{ Path = $p; Status = "unchanged"; Ins = 0; Del = 0 }
        continue
    }

    $anyDiff = $true

    # Show the actual diff for a human to read.
    Write-Host ""
    Write-Host ("=== DIFF vs {0}: {1} ===" -f $Branch, $p) -ForegroundColor Cyan
    git -C $repoRoot diff --color "${Branch}" -- $p 2>$null | Write-Host

    # Insertion/deletion counts -- the tell that was read past in the real
    # incident. --numstat prints "<ins>\t<del>\t<path>".
    $numstat = git -C $repoRoot diff --numstat "${Branch}" -- $p 2>$null
    $ins = 0; $del = 0
    if ($numstat) {
        $fields = ($numstat -split "`t")
        if ($fields.Count -ge 2) {
            [void][int]::TryParse($fields[0], [ref]$ins)
            [void][int]::TryParse($fields[1], [ref]$del)
        }
    }
    Write-Host ("{0}: {1} insertion(s), {2} deletion(s) vs {3}." -f $p, $ins, $del, $Branch) -ForegroundColor Yellow

    $status = "changed"
    if ($null -ne $budget -and ($ins + $del) -gt $budget) {
        $anyBudgetExceeded = $true
        $status = "over-budget"
        Write-Host ("{0}: EXCEEDS declared budget of {1} line(s) (actual {2}) -- this is exactly the shape of the stale-working-copy incidents (a much larger change than the caller intended). Re-check this file before proceeding." -f $p, $budget, ($ins + $del)) -ForegroundColor Red
    }

    $rows += [pscustomobject]@{ Path = $p; Status = $status; Ins = $ins; Del = $del }
}

Write-Host ""
Write-Host "=== Summary ===" -ForegroundColor Cyan
$rows | Format-Table -AutoSize | Out-String | Write-Host

if ($anyBudgetExceeded) {
    Write-Host "REFUSED: at least one path exceeds its declared -ExpectedMaxLines budget. Re-read the diff above; do not override without understanding why it is larger than expected." -ForegroundColor Red
    exit 1
}

if ($anyDiff -and -not $Confirm) {
    Write-Host "REFUSED: differences from $Branch exist above and -Confirm was not passed." -ForegroundColor Red
    Write-Host "Re-run with -Confirm only after you have reviewed every diff above and are certain each difference is your own intended edit (not a stale working copy)." -ForegroundColor Red
    exit 1
}

if ($anyDiff) {
    Write-Host "CONFIRMED by caller: proceeding is safe to attempt (this script does not itself commit -- run your git commit next)." -ForegroundColor Green
} else {
    Write-Host "All paths unchanged or new vs $Branch -- safe." -ForegroundColor Green
}
exit 0
