# dev_promote.ps1 -- coordinator-only: promote a tested origin/dev commit to origin/main as ONE
# single-parent commit (same shape as release_merge.ps1 builds for origin/release).
#
#   powershell -ExecutionPolicy Bypass -File tools\dev_promote.ps1 -Commit <dev commit> [-Push]
#
# Flow (docs/MCP_SERVERS.md "Git workflow guards"): agents push tested commits to origin/dev; the
# coordinator runs the full run_all_checks on the dev tip, then promotes it. M = commit-tree(tree of
# X, sole parent origin/main, message "Promote dev <X full sha>: <subjects of the dev commits since
# the previous promote>"). Pushed to main as a plain fast-forward; NO tag is created. Dry run unless
# -Push; touches neither working tree nor index; never force-pushes.
#
# Refusals (each names its rule):
#  - X is not an ancestor of origin/dev
#  - origin/main holds commits that are neither reachable from origin/dev nor earlier promote
#    commits (a direct push to main): names them and says to merge main into dev first
#  - the previous promote's dev commit is not an ancestor of X (promotes only move forward)
#  - X's tree equals origin/main's tree (nothing to promote)
#  - the built commit fails the tree-equals-X / single-parent verification
#
# -RepoPath points at another repository (the scratch-repo tests); default is this repo.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Commit,
    [switch]$Push,
    [string]$RepoPath = "",
    [string]$Trailer = "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
)
$ErrorActionPreference = 'Continue'
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $RepoPath) { $RepoPath = Split-Path -Parent $scriptRoot }
function Fail([string]$rule, [string]$msg) { Write-Host "REFUSED [$rule]: $msg" -ForegroundColor Red; exit 1 }
function G { $o = & git -C $RepoPath @args; $script:rc = $LASTEXITCODE; return $o }
function GOk { & git -C $RepoPath @args 2>$null | Out-Null; return ($LASTEXITCODE -eq 0) }
$PromoteRe = '^Promote dev ([0-9a-f]{40}):'

G fetch --quiet origin | Out-Null
if ($rc -ne 0) { Fail 'fetch' 'git fetch origin failed.' }
$devTip = (G rev-parse --verify --quiet 'refs/remotes/origin/dev^{commit}')
if ($rc -ne 0 -or -not $devTip) { Fail 'origin-dev' 'origin/dev does not exist.' }
$mainTip = (G rev-parse --verify --quiet 'refs/remotes/origin/main^{commit}')
if ($rc -ne 0 -or -not $mainTip) { Fail 'origin-main' 'origin/main does not exist.' }
$x = (G rev-parse --verify --quiet "$Commit^{commit}")
if ($rc -ne 0 -or -not $x) { Fail 'commit' "-Commit '$Commit' is not a commit." }

if (-not (GOk merge-base --is-ancestor $x $devTip)) { Fail 'commit-on-origin-dev' "$x is not an ancestor of origin/dev." }

# main commits that dev does not contain: only earlier promote commits are allowed there.
$offenders = New-Object System.Collections.Generic.List[string]
foreach ($c in @(G rev-list "refs/remotes/origin/dev..refs/remotes/origin/main")) {
    if (-not $c) { continue }
    $subj = (G log -1 --format=%s $c)
    if ($subj -match $PromoteRe -and @((G rev-list --parents -n 1 $c) -split ' ').Count -eq 2) { continue }
    $offenders.Add("$c $subj")
}
if ($offenders.Count -gt 0) {
    Write-Host "origin/main has commits that are not reachable from dev through a previous promote:" -ForegroundColor Red
    foreach ($o in $offenders) { Write-Host "    $o" -ForegroundColor Red }
    Fail 'main-ahead-of-dev' 'merge origin/main into dev first (git merge origin/main on a dev-based worktree, push to dev), then promote.'
}

# previous promote: newest first-parent main commit whose subject is "Promote dev <sha>:".
$prevDev = $null
foreach ($c in @(G rev-list --first-parent $mainTip)) {
    $pm = [regex]::Match((G log -1 --format=%s $c), $PromoteRe)
    if ($pm.Success) { $prevDev = $pm.Groups[1].Value; break }
}
if ($prevDev) {
    if (-not (GOk cat-file -e "$prevDev^{commit}")) { Fail 'prev-promote-missing' "the previous promote names dev commit $prevDev, which is not in this clone." }
    if ($prevDev -eq $x) { Fail 'already-promoted' "$x is the dev commit of the latest promote." }
    if (-not (GOk merge-base --is-ancestor $prevDev $x)) { Fail 'forward-only' "the previous promote's dev commit $prevDev is not an ancestor of $x." }
    $range = "$prevDev..$x"
} else {
    $mb = (G merge-base $mainTip $x)
    $range = if ($mb) { "$mb..$x" } else { $x }
}
if ((G rev-parse "$x^{tree}") -eq (G rev-parse "$mainTip^{tree}")) { Fail 'nothing-to-promote' "$x has the same tree as origin/main." }

$subjects = @(G log --no-merges --reverse --format=%s $range) | Where-Object { $_ }
$list = if ($subjects.Count -gt 0) { ($subjects -join '; ') } else { '(no new commits)' }
$body = "Promote dev ${x}: $list`n`n$Trailer`n"
$tmp = [IO.Path]::GetTempFileName()
[IO.File]::WriteAllText($tmp, $body, (New-Object Text.UTF8Encoding($false)))
$tree = (G rev-parse "$x^{tree}")
$m = (G commit-tree $tree -p $mainTip -F $tmp)
Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
if ($rc -ne 0 -or -not $m) { Fail 'commit-tree' 'git commit-tree failed.' }

if ((G rev-parse "$m^{tree}") -ne $tree) { Fail 'tree-equals-commit' 'promote tree differs from the dev commit tree.' }
if ((G rev-parse "$m^1") -ne $mainTip) { Fail 'first-parent' 'first parent is not origin/main.' }
if (@((G rev-list --parents -n 1 $m) -split ' ').Count -ne 2) { Fail 'single-parent' 'promote commit does not have exactly one parent.' }
Write-Host "promote commit $m  (tree == $x^{tree}, sole parent origin/main $mainTip)"

if (-not $Push) {
    Write-Host "DRY RUN: nothing pushed. -Push would fast-forward origin/main to $m (no tag)."
    exit 0
}
G push origin "${m}:refs/heads/main" | Out-Null
if ($rc -ne 0) { Fail 'push-main' 'plain fast-forward push of main was rejected (never forced); re-run after fetching.' }
G fetch --quiet origin | Out-Null
if ((G rev-parse 'refs/remotes/origin/main') -ne $m) { Fail 'verify-main' 'origin/main != the new promote commit after re-fetch.' }
Write-Host "pushed and verified: origin/main == $m"
exit 0
