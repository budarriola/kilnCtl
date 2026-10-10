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
function MakeLog([string]$x, [string[]]$lines, [string]$treeOverride = "") {
    $t = if ($treeOverride) { $treeOverride } else { (git -C $work rev-parse "$x^{tree}").Trim() }
    $p = Join-Path $tmp ("log_" + [guid]::NewGuid().ToString("N").Substring(0, 6) + ".log")
    @("Run tree: $t dirty=0 partial=0", "Run mode: full") + $lines | Set-Content -LiteralPath $p -Encoding Unicode
    return $p
}
function Run([string[]]$more) {
    if (($more -contains "-Push") -and -not ($more -contains "-CheckLog") -and -not ($more -contains "-NoAutoLog")) {
        $xi = [array]::IndexOf($more, "-Commit")
        $more = $more + @("-CheckLog", (MakeLog $more[$xi + 1] @("5 passed, 0 skipped (0 due to -Fast), 0 failed.")))
    }
    $more = @($more | Where-Object { $_ -ne "-NoAutoLog" })
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File $promote -RepoPath $work @more 2>&1 | Out-String
    return [pscustomobject]@{ Code = $LASTEXITCODE; Out = $out }
}
function PinStub([int]$code) {
    $p = Join-Path $tmp ("pin_" + [guid]::NewGuid().ToString("N").Substring(0, 6) + ".ps1")
    Set-Content -LiteralPath $p -Value ("param([string]`$RepoPath,[string]`$Commit)`nexit $code") -Encoding ascii
    return $p
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

    Write-Host "case: -Push evidence gate (F2)"
    CommitFile $work "g.txt" "g" "dev change G"
    git -C $work push origin dev *>$null
    $xg = (git -C $work rev-parse HEAD).Trim()
    $mainBefore = Rev main
    $r = Run @("-Commit", $xg, "-Push", "-NoAutoLog")
    Assert ($r.Code -eq 1 -and $r.Out -match 'requires -CheckLog' -and (Rev main) -eq $mainBefore) "no -CheckLog refused, main untouched"
    $bad = MakeLog $xg @("5 passed, 0 skipped (0 due to -Fast), 0 failed.") ((git -C $work rev-parse "$x1^{tree}").Trim())
    $r = Run @("-Commit", $xg, "-Push", "-CheckLog", $bad)
    Assert ($r.Code -eq 1 -and (Rev main) -eq $mainBefore) "log for another tree refused"
    $bad = MakeLog $xg @("  FAIL  tools\check_x.ps1 (exit 1)", "vs main baseline", "  NEW (fails here, passed on main): 1", "0 passed, 0 skipped (0 due to -Fast), 1 failed.")
    $r = Run @("-Commit", $xg, "-Push", "-CheckLog", $bad)
    Assert ($r.Code -eq 1 -and (Rev main) -eq $mainBefore) "log with NEW failure refused"
    $bad = MakeLog $xg @("FAILED: 1 of 3 checks", "5 passed, 0 skipped (0 due to -Fast), 0 failed.")
    $r = Run @("-Commit", $xg, "-Push", "-CheckLog", $bad)
    Assert ($r.Code -eq 1 -and (Rev main) -eq $mainBefore) "log with FAILED line refused"
    $bad = MakeLog $xg @("no summary here")
    $r = Run @("-Commit", $xg, "-Push", "-CheckLog", $bad)
    Assert ($r.Code -eq 1 -and (Rev main) -eq $mainBefore) "log without summary refused"
    $r = Run @("-Commit", $xg)
    Assert ($r.Code -eq 0 -and $r.Out -match 'DRY RUN') "dry run needs no log"
    $r = Run @("-Commit", $xg, "-Push", "-PinCheckScript", (PinStub 2))
    Assert ($r.Code -eq 1 -and $r.Out -match 'submodule-pins' -and (Rev main) -eq $mainBefore) "pin check exit 2 refused, main untouched (T-5)"

    Write-Host "case: -PinCheckScript refused when -RepoPath is this repo (L-1)"
    $own = & powershell -NoProfile -ExecutionPolicy Bypass -File $promote -Commit $xg -PinCheckScript (PinStub 0) 2>&1 | Out-String
    Assert ($LASTEXITCODE -eq 1 -and $own -match 'pin-check-script') "stub refused without a foreign -RepoPath"
    $r = Run @("-Commit", $xg, "-Push", "-PinCheckScript", (PinStub 3))
    Assert ($r.Out -match 'OVERRIDE: submodule_pins=pass\(stub:') "foreign -RepoPath stub is announced as OVERRIDE"
    git -C $origin update-ref refs/heads/main $mainBefore
    Write-Host "case: second promote lists only new subjects"
    CommitFile $work "c.txt" "c" "dev change C"
    git -C $work push origin dev *>$null
    $x2 = (git -C $work rev-parse HEAD).Trim()
    $r = Run @("-Commit", $x2, "-Push", "-PinCheckScript", (PinStub 3))
    Assert ($r.Code -eq 0 -and $r.Out -match 'WARNING: submodule pin check could not run') "second promote exits 0; pin exit 3 only warns (T-5)"
    $subj = (git -C $origin log -1 --format=%s main)
    Assert ($subj -match ('^Promote dev ' + $x2 + ': dev change G; dev change C$')) "only new subject listed (got: $subj)"

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

    Write-Host "case: forged promote subject on main is an offender"
    $forge = Join-Path $tmp "forge"
    git clone $origin $forge *>$null
    git -C $forge checkout main *>$null
    # (a) subject names a dev sha, single parent, but tree differs from that sha
    CommitFile $forge "forged.txt" "f" "Promote dev ${x4}: forged wrong tree"
    git -C $forge push origin main *>$null
    $forgedA = (git -C $forge rev-parse HEAD).Trim()
    CommitFile $work "e.txt" "e" "dev change E"
    git -C $work push origin dev *>$null
    $x5 = (git -C $work rev-parse HEAD).Trim()
    $r = Run @("-Commit", $x5, "-Push")
    Assert ($r.Code -eq 1 -and $r.Out -match 'main-ahead-of-dev' -and $r.Out -match $forgedA) "forged promote (tree mismatch) refused and named"
    # (b) subject names a sha that is not on origin/dev
    git -C $forge reset --hard HEAD~1 *>$null
    $bogus = "0123456789abcdef0123456789abcdef01234567"
    git -C $forge commit --allow-empty -m "Promote dev ${bogus}: forged unknown sha" *>$null
    git -C $forge push --force origin main *>$null
    $r = Run @("-Commit", $x5, "-Push")
    Assert ($r.Code -eq 1 -and $r.Out -match 'main-ahead-of-dev') "forged promote (sha not on dev) refused"
    git -C $forge reset --hard HEAD~1 *>$null
    # (c) names a real commit with an equal tree, but that commit is not on origin/dev
    $mainSave = (git -C $forge rev-parse HEAD).Trim()
    git -C $forge checkout -b side *>$null
    CommitFile $forge "side.txt" "s" "side only"
    git -C $forge push origin side *>$null
    $side = (git -C $forge rev-parse HEAD).Trim()
    git -C $forge checkout main *>$null
    $fc = (git -C $forge commit-tree "$side^{tree}" -p $mainSave -m "Promote dev ${side}: forged off-dev").Trim()
    git -C $forge push --force origin "${fc}:refs/heads/main" *>$null
    $r = Run @("-Commit", $x5, "-Push")
    Assert ($r.Code -eq 1 -and $r.Out -match 'main-ahead-of-dev') "forged promote (named sha not on dev) refused"
    git -C $forge push --force origin "${mainSave}:refs/heads/main" *>$null

    Write-Host "case: concurrent promote loses cleanly (main moves between verify and push)"
    CommitFile $work "h.txt" "h" "dev change H"
    git -C $work push origin dev *>$null
    $xh = (git -C $work rev-parse HEAD).Trim()
    $hook = Join-Path $origin "hooks/pre-receive"
    $racerMark = (Join-Path $tmp "racer.marker") -replace '\\', '/'
    [IO.File]::WriteAllText($hook, "#!/bin/sh`nunset GIT_OBJECT_DIRECTORY GIT_ALTERNATE_OBJECT_DIRECTORIES GIT_QUARANTINE_PATH`nif [ ! -e '$racerMark' ]; then touch '$racerMark'; m=`$(git rev-parse main); n=`$(git commit-tree -p `$m -m 'racing promote' `$m^{tree}); git update-ref refs/heads/main `$n; fi`nexit 0`n")
    $r = Run @("-Commit", $xh, "-Push")
    Remove-Item -LiteralPath $hook -Force
    $raced = (git -C $origin log -1 --format=%s main)
    Assert (Test-Path -LiteralPath $racerMark) "fixture: racing hook ran"
    Assert ($r.Code -eq 1) "concurrent promote fails loudly (code $($r.Code))"
    Assert ($raced -eq 'racing promote') "winner's commit kept on main, nothing overwritten (got: $raced)"

    Write-Host "case: nothing to promote"
    $r = Run @("-Commit", $x4, "-Push")
    Assert ($r.Code -eq 1) "re-promoting the same commit refused"
} finally {
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}
if ($script:fails -gt 0) { Write-Host "check_dev_promote: $script:fails FAILED" -ForegroundColor Red; exit 1 }
Write-Host "check_dev_promote: all cases passed"
exit 0
