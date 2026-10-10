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
#  - -Push without -CheckLog <path>, or with a log that is not the LAST-run evidence for X: a full
#    (not -Fast), unfiltered, clean-tree run_all_checks log whose "Run tree:" hash equals
#    X^{tree}, with 0 failures or 0 NEW failures versus the main baseline (review F2). A dry run
#    needs no log. Make the log from a clean detached worktree at X:
#      tools\run_all_checks.ps1 *>&1 | Tee-Object C:\wt\devbatch_<id>.log
#  - after the push, origin/main must CONTAIN the promote commit (an ancestor check after a
#    checked fetch, review F12), so a promote that raced in behind ours is not a false failure
#
# -RepoPath points at another repository (the scratch-repo tests); default is this repo.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Commit,
    [switch]$Push,
    [string]$CheckLog = "",
    [string]$PinCheckScript = "",
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

if ($Push -and -not $CheckLog) { Fail 'check-log' '-Push requires -CheckLog <path> (a full run_all_checks log of exactly this commit''s tree).' }
G fetch --quiet origin | Out-Null
if ($rc -ne 0) { Fail 'fetch' 'git fetch origin failed.' }
$devTip = (G rev-parse --verify --quiet 'refs/remotes/origin/dev^{commit}')
if ($rc -ne 0 -or -not $devTip) { Fail 'origin-dev' 'origin/dev does not exist.' }
$mainTip = (G rev-parse --verify --quiet 'refs/remotes/origin/main^{commit}')
if ($rc -ne 0 -or -not $mainTip) { Fail 'origin-main' 'origin/main does not exist.' }
$x = (G rev-parse --verify --quiet "$Commit^{commit}")
if ($rc -ne 0 -or -not $x) { Fail 'commit' "-Commit '$Commit' is not a commit." }

if (-not (GOk merge-base --is-ancestor $x $devTip)) { Fail 'commit-on-origin-dev' "$x is not an ancestor of origin/dev." }

# A real promote: single parent, subject names dev SHA D, D is on origin/dev, tree(commit) == tree(D).
function Test-RealPromote([string]$c) {
    $pm = [regex]::Match((G log -1 --format=%s $c), $PromoteRe)
    if (-not $pm.Success) { return $false }
    if (@((G rev-list --parents -n 1 $c) -split ' ').Count -ne 2) { return $false }
    $d = $pm.Groups[1].Value
    if (-not (GOk cat-file -e "$d^{commit}")) { return $false }
    if (-not (GOk merge-base --is-ancestor $d $devTip)) { return $false }
    return ((G rev-parse "$c^{tree}") -eq (G rev-parse "$d^{tree}"))
}

# main commits that dev does not contain: only earlier promote commits are allowed there.
$offenders = New-Object System.Collections.Generic.List[string]
foreach ($c in @(G rev-list "refs/remotes/origin/dev..refs/remotes/origin/main")) {
    if (-not $c) { continue }
    $subj = (G log -1 --format=%s $c)
    if (Test-RealPromote $c) { continue }
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
    if (Test-RealPromote $c) { $prevDev = [regex]::Match((G log -1 --format=%s $c), $PromoteRe).Groups[1].Value; break }
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
# F2: test evidence. The log must describe exactly X's tree.
function Read-LogText([string]$path) {
    $fs = New-Object System.IO.FileStream($path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, ([System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete))
    try { $b = New-Object byte[] ([int]$fs.Length); $off = 0; while ($off -lt $b.Length) { $n0 = $fs.Read($b, $off, $b.Length - $off); if ($n0 -le 0) { break }; $off += $n0 } }
    finally { $fs.Dispose() }
    if ($b.Length -ge 2 -and $b[0] -eq 0xFF -and $b[1] -eq 0xFE) { return [Text.Encoding]::Unicode.GetString($b, 2, $b.Length - 2) }
    if ($b.Length -ge 3 -and $b[0] -eq 0xEF -and $b[1] -eq 0xBB -and $b[2] -eq 0xBF) { return [Text.Encoding]::UTF8.GetString($b, 3, $b.Length - 3) }
    $n = [Math]::Min($b.Length, 4096); $nul = 0
    for ($i = 1; $i -lt $n; $i += 2) { if ($b[$i] -eq 0) { $nul++ } }
    if ($n -gt 8 -and $nul -gt ($n / 4)) { return [Text.Encoding]::Unicode.GetString($b) }
    return [Text.Encoding]::UTF8.GetString($b)
}
if (-not (Test-Path -LiteralPath $CheckLog)) { Fail 'check-log' "check log '$CheckLog' does not exist." }
try { $logText = Read-LogText $CheckLog } catch { Fail 'check-log' "cannot read check log '$CheckLog': $($_.Exception.Message)" }
$tms = @([regex]::Matches($logText, '(?m)^Run tree: ([0-9a-f]{40}) dirty=(\d) partial=(\d)'))
if ($tms.Count -eq 0) { Fail 'check-log' "check log '$CheckLog' has no 'Run tree:' line (not a current run_all_checks log)." }
$lastRun = $tms[$tms.Count - 1]
$logText = $logText.Substring($lastRun.Index)
if ($lastRun.Groups[1].Value -ne (G rev-parse "$x^{tree}")) { Fail 'check-log-tree' "check log is for tree $($lastRun.Groups[1].Value), not $x's tree $(G rev-parse "$x^{tree}"); run the full suite on exactly that commit." }
if ($lastRun.Groups[2].Value -ne '0') { Fail 'check-log-dirty' 'the check run was on a dirty working tree.' }
if ($lastRun.Groups[3].Value -ne '0') { Fail 'check-log-partial' 'the check run was filtered (-Only/-Skip).' }
if ($logText -notmatch '(?m)^Run mode: full\s*$') { Fail 'check-log-mode' 'the check run was not a full run (-Fast); promote needs a full run.' }
if ($logText -match '(?m)^FAILED:') { Fail 'check-log-failed' 'the check log carries a FAILED: line (skipped/busy checks or a run-level failure).' }
$sums = @([regex]::Matches($logText, '(?m)^\s*\d+ passed, \d+ skipped[^

]*?, (\d+) failed\.'))
if ($sums.Count -eq 0) { Fail 'check-log-summary' 'the check log has no run_all_checks summary line (run unfinished or died).' }
$nFail = [int]$sums[$sums.Count - 1].Groups[1].Value
if ($nFail -gt 0) {
    $nFailLines = @([regex]::Matches($logText, '(?m)^\s*FAIL\s+(\S+)\s+\((?:exit|killed)') | ForEach-Object { $_.Groups[1].Value } | Select-Object -Unique).Count
    if ($nFailLines -ne $nFail) { Fail 'check-log-failcount' "summary reports $nFail failed but $nFailLines FAIL line(s) parsed." }
    $nm = [regex]::Match($logText, '(?m)^NEW \(fails here, passed on main\): (\d+)\s*$')
    if (-not $nm.Success) { Fail 'check-log-baseline' "$nFail check(s) failed and the log has no 'vs main baseline' NEW count (no usable baseline); promote needs 0 failures or 0 NEW." }
    if ([int]$nm.Groups[1].Value -ne 0 -or $logText -match '(?m)^\s*CHANGED ') { Fail 'check-log-new' "the check log has NEW (or CHANGED) failures versus the main baseline." }
    Write-Host "check log: $nFail failure(s), all KNOWN on main (0 NEW)" -ForegroundColor Yellow
} else { Write-Host "check log OK: full run on tree of $x, 0 failed" -ForegroundColor Green }

if (-not $PinCheckScript) { $PinCheckScript = Join-Path $scriptRoot 'check_submodule_pins_pushed.ps1' }
& powershell -NoProfile -ExecutionPolicy Bypass -File $PinCheckScript -RepoPath $RepoPath -Commit $m
if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 3) { Fail 'submodule-pins' "submodule pin check refused (exit $LASTEXITCODE): a pin is not on its remote or the check errored; push the submodule commit first." }
if ($LASTEXITCODE -eq 3) { Write-Host "WARNING: submodule pin check could not run (exit $LASTEXITCODE); not a PASS" -ForegroundColor Yellow }
G push origin "${m}:refs/heads/main" | Out-Null
if ($rc -ne 0) { Fail 'push-main' 'plain fast-forward push of main was rejected (never forced); re-run after fetching.' }
G fetch --quiet origin | Out-Null
if ($rc -ne 0) { Fail 'verify-fetch' 'git fetch origin failed after the push; cannot verify.' }
if (-not (GOk merge-base --is-ancestor $m refs/remotes/origin/main)) { Fail 'verify-main' 'origin/main does not contain the new promote commit after re-fetch.' }
Write-Host "pushed and verified: origin/main contains $m"
exit 0
