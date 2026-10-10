# push_verify.ps1 -- verify a commit actually landed on origin/main, and say
# so unambiguously.
#
# WHY THIS EXISTS. This project has produced FOUR false "landed" reports
# from the same two causes, each costing a round trip to discover:
#
#   1. DIRECTION-SENSITIVE ANCESTRY CHECK. The correct question is
#     `git merge-base --is-ancestor <mine> origin/main` -- "is my commit an
#     ancestor of origin/main", i.e. did origin/main actually pick it up.
#     The reversed form, `git merge-base --is-ancestor origin/main HEAD`,
#     asks the opposite question and succeeds whenever your branch merely
#     sits ON TOP of (rebased onto, or descended from) origin/main --
#     including the case where your own commit is stranded on an unpushed
#     local branch that origin/main has never seen. Reversing the two
#     arguments silently flips a "no" into a "yes".
#   2. `$?` AFTER A NATIVE GIT CALL. PowerShell 5.1 sets `$?` to $false
#     after ANY native command that wrote to stderr, independent of its
#     real exit code -- and `git push`'s ordinary progress banner
#     ("Enumerating objects...", "To https://...") goes to stderr by
#     design. `if ($?) { "pushed" } else { "failed" }` immediately after a
#     `git push` has reported a genuinely successful push as FAILED.
#
# This script never uses `$?` after a native call and never parses push
# output. It determines success from `$LASTEXITCODE` alone, on the
# correctly-ordered ancestry check, after a fresh `git fetch`.
#
# USAGE
#   powershell -ExecutionPolicy Bypass -File tools\push_verify.ps1 [-Commit <hash>=HEAD] [-Branch origin/dev] [-FetchTimeoutSec 90]
#   (-Branch is always resolved on the remote: "dev" and "origin/dev" both mean refs/remotes/origin/dev)
#
# OUTPUT: exactly one unambiguous verdict line, prefixed "VERDICT: ", plus
# supporting detail above it. On "no", also names the local branch(es) the
# commit IS reachable from, if any -- "stranded on an unpushed local
# branch" is the actual root cause behind these false reports far more
# often than a genuine push failure.
#
# EXIT CODES:
#   0 -- landed (commit is an ancestor of the target branch on origin, after fetch)
#   1 -- not landed (fetch failed, commit unknown, or ancestry check says no)
#   2 -- bad usage

[CmdletBinding()]
param(
    # Defaults to HEAD when omitted (agents kept failing on the old mandatory parameter).
    [string]$Commit = "HEAD",

    [string]$Branch = "origin/dev",

    # Bound on the fetch: a fetch that hangs (credential prompt, dead network) must not hang the caller.
    [int]$FetchTimeoutSec = 90
)

# See worktree_mint.ps1's header for why this is "Continue", not "Stop":
# `git fetch`/`git push`'s stderr progress banners would otherwise be
# treated as terminating errors under PS 5.1 even on exit code 0. Every
# call below is checked via $LASTEXITCODE explicitly.
$ErrorActionPreference = "Continue"

function Get-RepoRoot {
    $top = git rev-parse --show-toplevel 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $top) {
        Write-Host "ERROR: not inside a git repository." -ForegroundColor Red
        exit 2
    }
    return ($top -replace '/', '\')
}


$repoRoot = Get-RepoRoot

# Resolve the remote side of $Branch (e.g. "origin/main" -> remote "origin"). F7: the verdict is
# ALWAYS taken against refs/remotes/<remote>/<branch>, never a bare name that could resolve to a
# LOCAL branch ("-Branch dev" means origin/dev, and a local refs/heads/origin/dev cannot shadow it).
$remote = "origin"
$branchName = $Branch
$remoteList = @(git -C $repoRoot remote 2>$null)
if ($Branch -match '^([^/]+)/(.+)$' -and ($remoteList -contains $Matches[1])) {
    $remote = $Matches[1]
    $branchName = $Matches[2]
}
$Branch = "refs/remotes/$remote/$branchName"

Write-Host "Fetching $remote ..."
# Bounded fetch: run git as a child process, never prompt for credentials, kill the tree on timeout.
$env:GIT_TERMINAL_PROMPT = "0"
$fetchProc = Start-Process -FilePath "git" -ArgumentList @("-C", "`"$repoRoot`"", "fetch", $remote) -NoNewWindow -PassThru `
    -RedirectStandardOutput ([IO.Path]::GetTempFileName()) -RedirectStandardError ([IO.Path]::GetTempFileName())
$null = $fetchProc.Handle
if (-not $fetchProc.WaitForExit($FetchTimeoutSec * 1000)) {
    & taskkill /PID $fetchProc.Id /T /F *>$null
    Write-Host "VERDICT: UNKNOWN -- git fetch $remote timed out after ${FetchTimeoutSec}s; cannot verify against a stale view. NOT LANDED (unverified)." -ForegroundColor Red
    exit 1
}
if ($fetchProc.ExitCode -ne 0) {
    Write-Host "VERDICT: UNKNOWN -- git fetch $remote failed (exit $($fetchProc.ExitCode)); cannot verify against a stale view. NOT LANDED (unverified)." -ForegroundColor Red
    exit 1
}

# Resolve the commit to a full hash so partial hashes / refs work, and so we
# fail loudly if it doesn't even exist locally.
$fullHash = git -C $repoRoot rev-parse --verify "$Commit^{commit}" 2>$null
if ($LASTEXITCODE -ne 0 -or -not $fullHash) {
    Write-Host "VERDICT: NOT LANDED -- '$Commit' does not resolve to a commit in this repo." -ForegroundColor Red
    exit 1
}

git -C $repoRoot rev-parse --verify --quiet "$Branch^{commit}" *>$null
if ($LASTEXITCODE -ne 0) {
    Write-Host "VERDICT: UNKNOWN -- $Branch does not exist after fetching $remote." -ForegroundColor Red
    exit 1
}

# THE CORRECT DIRECTION: is $fullHash an ancestor of $Branch? Arguments in
# exactly this order -- reversing them asks "is $Branch an ancestor of
# HEAD", a different and commonly misleading question (see header).
git -C $repoRoot merge-base --is-ancestor $fullHash $Branch 2>$null
$isAncestor = ($LASTEXITCODE -eq 0)

if ($isAncestor) {
    Write-Host "VERDICT: LANDED -- $fullHash is an ancestor of $Branch." -ForegroundColor Green
    exit 0
}

# Not landed. Name which local branch(es), if any, the commit IS reachable
# from -- the actual real-world root cause behind these false reports has
# consistently been "stranded on an unpushed local branch", not a genuine
# push failure, so make that diagnosis available immediately rather than
# leaving the caller to re-derive it.
$containingBranches = git -C $repoRoot branch --contains $fullHash --format='%(refname:short)' 2>$null
$containingBranches = $containingBranches | Where-Object { $_ }

Write-Host "VERDICT: NOT LANDED -- $fullHash is NOT an ancestor of $Branch." -ForegroundColor Red
if ($containingBranches) {
    Write-Host "It IS reachable from the following local branch(es) (likely stranded/unpushed):" -ForegroundColor Yellow
    foreach ($b in $containingBranches) {
        Write-Host "  $b"
    }
} else {
    Write-Host "It is not reachable from any local branch either -- check the hash." -ForegroundColor Yellow
}
exit 1
