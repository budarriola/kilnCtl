# check_dev_promote.ps1 -- unit test for tools/dev_promote.ps1 against a throwaway bare origin
# under the temp dir (never the real origin).
# Cases: happy promote (single parent, tree == X, subjects listed, no tag); second promote lists only
# new subjects; X not on dev refused; direct commit on main refused naming it, then accepted after
# main is merged into dev; nothing-to-promote refused; dry run pushes nothing.
$ErrorActionPreference = "Continue"
$promote = Join-Path $PSScriptRoot "dev_promote.ps1"
$script:fails = 0
function Assert([bool]$cond, [string]$what) {
    if ($cond) { Write-Host "  ok: $what" } else { Write-Host "  FAIL: $what" -ForegroundColor Red; $script:fails++ }
}
$env:GIT_AUTHOR_NAME = "t"; $env:GIT_AUTHOR_EMAIL = "t@example.invalid"
$env:GIT_COMMITTER_NAME = "t"; $env:GIT_COMMITTER_EMAIL = "t@example.invalid"
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("devpromote_test_" + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Path $tmp | Out-Null
$origin = Join-Path $tmp "origin.git"
function CommitFile([string]$dir, [string]$name, [string]$text, [string]$msg) {
    Set-Content -LiteralPath (Join-Path $dir $name) -Value $text -Encoding ascii
    git -C $dir add -- $name *>$null
    git -C $dir commit -m $msg *>$null
}
function Run([string[]]$more) {
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File $promote -RepoPath $work @more 2>&1 | Out-String
    return [pscustomobject]@{ Code = $LASTEXITCODE; Out = $out }
}
function Rev([string]$ref) { (git -C $origin rev-parse $ref).Trim() }
try {
    git init --bare -b main $origin *>$null
    $work = Join-Path $tmp "work"
    git clone $origin $work *>$null
    git -C $work checkout -b main *>$null
    CommitFile $work "base.txt" "base" "base"
    git -C $work push origin main *>$null
    git -C $work checkout -b dev *>$null
    CommitFile $work "a.txt" "a" "dev change A"
    CommitFile $work "b.txt" "b" "dev change B"
    git -C $work push origin dev *>$null
    $x1 = (git -C $work rev-parse HEAD).Trim()

    Write-Host "case: dry run pushes nothing"
    $mainBefore = Rev main
    $r = Run @("-Commit", $x1)
    Assert ($r.Code -eq 0 -and $r.Out -match 'DRY RUN') "dry run exits 0"
    Assert ((Rev main) -eq $mainBefore) "dry run left main alone"

    Write-Host "case: happy promote"
    $r = Run @("-Commit", $x1, "-Push")
    Assert ($r.Code -eq 0) "promote exits 0"
    $m1 = Rev main
    Assert ((git -C $origin rev-parse "$m1^{tree}").Trim() -eq (git -C $origin rev-parse "$x1^{tree}").Trim()) "tree equals X's tree"
    Assert ((git -C $origin rev-parse "$m1^").Trim() -eq $mainBefore) "sole parent is old main"
    Assert (@((git -C $origin rev-list --parents -n 1 $m1) -split ' ').Count -eq 2) "exactly one parent"
    $subj = (git -C $origin log -1 --format=%s $m1)
    Assert ($subj -match ('^Promote dev ' + $x1 + ': dev change A; dev change B$')) "subject names full sha and dev subjects (got: $subj)"
    Assert (@(git -C $origin tag --list).Count -eq 0) "no tag created"

    Write-Host "case: second promote lists only new subjects"
    CommitFile $work "c.txt" "c" "dev change C"
    git -C $work push origin dev *>$null
    $x2 = (git -C $work rev-parse HEAD).Trim()
    $r = Run @("-Commit", $x2, "-Push")
    Assert ($r.Code -eq 0) "second promote exits 0"
    $subj = (git -C $origin log -1 --format=%s main)
    Assert ($subj -match ('^Promote dev ' + $x2 + ': dev change C$')) "only new subject listed (got: $subj)"

    Write-Host "case: X not on dev refused"
    git -C $work checkout -b off *>$null
    CommitFile $work "off.txt" "o" "off dev"
    $xo = (git -C $work rev-parse HEAD).Trim()
    git -C $work checkout dev *>$null
    $mainBefore = Rev main
    $r = Run @("-Commit", $xo, "-Push")
    Assert ($r.Code -eq 1 -and $r.Out -match 'commit-on-origin-dev') "refused: not an ancestor of origin/dev"
    Assert ((Rev main) -eq $mainBefore) "main untouched"

    Write-Host "case: direct commit on main refused, then accepted after merging main into dev"
    $direct = Join-Path $tmp "direct"
    git clone $origin $direct *>$null
    git -C $direct checkout main *>$null
    CommitFile $direct "hot.txt" "h" "hotfix pushed straight to main"
    git -C $direct push origin main *>$null
    $hot = (git -C $direct rev-parse HEAD).Trim()
    CommitFile $work "d.txt" "d" "dev change D"
    git -C $work push origin dev *>$null
    $x3 = (git -C $work rev-parse HEAD).Trim()
    $mainBefore = Rev main
    $r = Run @("-Commit", $x3, "-Push")
    Assert ($r.Code -eq 1 -and $r.Out -match 'main-ahead-of-dev') "refused: main ahead of dev"
    Assert ($r.Out -match $hot) "offending commit named"
    Assert ($r.Out -match 'merge origin/main into dev') "says to merge main into dev first"
    Assert ((Rev main) -eq $mainBefore) "main untouched on refusal"
    git -C $work fetch origin *>$null
    git -C $work merge --no-edit origin/main *>$null
    git -C $work push origin dev *>$null
    $x4 = (git -C $work rev-parse HEAD).Trim()
    $r = Run @("-Commit", $x4, "-Push")
    Assert ($r.Code -eq 0) "accepted after main was merged into dev"
    Assert ((git -C $origin rev-parse "$(Rev main)^{tree}").Trim() -eq (git -C $origin rev-parse "$x4^{tree}").Trim()) "main tree == merged dev tree"

    Write-Host "case: nothing to promote"
    $r = Run @("-Commit", $x4, "-Push")
    Assert ($r.Code -eq 1) "re-promoting the same commit refused"
} finally {
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}
if ($script:fails -gt 0) { Write-Host "check_dev_promote: $script:fails FAILED" -ForegroundColor Red; exit 1 }
Write-Host "check_dev_promote: all cases passed"
exit 0
