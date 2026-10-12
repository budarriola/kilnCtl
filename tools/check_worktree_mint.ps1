# check_worktree_mint.ps1 -- worktree_mint.ps1 defaults to origin/dev and -Base overrides it;
# commit_guard.ps1, push_verify.ps1 and wt_status.ps1 default to origin/dev. Scratch repo only.
$ErrorActionPreference = "Continue"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$script:fails = 0
function Assert([bool]$c, [string]$w) { if ($c) { Write-Host "  ok: $w" } else { Write-Host "  FAIL: $w" -ForegroundColor Red; $script:fails++ } }
$env:GIT_AUTHOR_NAME = "t"; $env:GIT_AUTHOR_EMAIL = "t@example.invalid"
$env:GIT_COMMITTER_NAME = "t"; $env:GIT_COMMITTER_EMAIL = "t@example.invalid"
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("wtmint_chk_" + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Path $tmp | Out-Null
$origin = Join-Path $tmp "origin.git"; $work = Join-Path $tmp "work"; $wtroot = Join-Path $tmp "wt"
New-Item -ItemType Directory -Path $wtroot | Out-Null
try {
    git init --bare -b main $origin *>$null
    git clone $origin $work *>$null
    git -C $work checkout -b main *>$null
    Set-Content -LiteralPath (Join-Path $work "a.txt") -Value "a" -Encoding ascii
    git -C $work add a.txt *>$null; git -C $work commit -m base *>$null; git -C $work push origin main *>$null
    git -C $work checkout -b dev *>$null
    Set-Content -LiteralPath (Join-Path $work "d.txt") -Value "d" -Encoding ascii
    git -C $work add d.txt *>$null; git -C $work commit -m devonly *>$null; git -C $work push origin dev *>$null
    $devSha = (git -C $work rev-parse HEAD).Trim(); $mainSha = (git -C $work rev-parse main).Trim()
    function Mint([string[]]$more) {
        Push-Location $work
        try { $o = & powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $here "worktree_mint.ps1") -Label t -WtRoot $wtroot -NoSubmodules @more 2>&1 | Out-String } finally { Pop-Location }
        $m = [regex]::Match($o, '(?m)^WORKTREE:\s*(.+?)\s*$')
        if (-not $m.Success) { Write-Host $o; return $null }
        return (git -C $m.Groups[1].Value rev-parse HEAD).Trim()
    }
    Write-Host "case: default mint is origin/dev"
    Assert ((Mint @()) -eq $devSha) "default worktree HEAD == origin/dev"
    Write-Host "case: -Base origin/main"
    Assert ((Mint @("-Base", "origin/main")) -eq $mainSha) "-Base origin/main worktree HEAD == origin/main"
    Write-Host "case: dev advances in origin after the clone"
    $other = Join-Path $tmp "other"
    git clone $origin $other *>$null
    git -C $other checkout dev *>$null
    Set-Content -LiteralPath (Join-Path $other "e.txt") -Value "e" -Encoding ascii
    git -C $other add e.txt *>$null; git -C $other commit -m devadvance *>$null; git -C $other push origin dev *>$null
    $newDev = (git -C $other rev-parse HEAD).Trim()
    Assert ($newDev -ne $devSha) "scratch origin dev advanced"
    Assert ((Mint @()) -eq $newDev) "mint lands on the advanced origin/dev tip"
    Write-Host "case: -Remove (F4/F14)"
    function MintPath() {
        Push-Location $work
        try { $o = & powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $here "worktree_mint.ps1") -Label r -WtRoot $wtroot -NoSubmodules 2>&1 | Out-String } finally { Pop-Location }
        return ([regex]::Match($o, '(?m)^WORKTREE:\s*(.+?)\s*$')).Groups[1].Value
    }
    function Run-MintRemove([string[]]$more, [string]$cwd = $work) {
        Push-Location $cwd
        try { $o = & powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $here "worktree_mint.ps1") -Remove @more 2>&1 | Out-String; $rc = $LASTEXITCODE } finally { Pop-Location }
        return [pscustomobject]@{ Rc = $rc; Out = $o }
    }
    # clean worktree at origin/dev: removed
    $w1 = MintPath
    $r = Run-MintRemove @("-Path", $w1)
    Assert ($r.Rc -eq 0 -and -not (Test-Path -LiteralPath $w1)) "clean worktree is removed"
    # unlanded detached commit: refused, kept; -Force removes
    $w2 = MintPath
    Set-Content -LiteralPath (Join-Path $w2 "u.txt") -Value "u" -Encoding ascii
    git -C $w2 add u.txt *>$null; git -C $w2 commit -m unlanded *>$null
    $r = Run-MintRemove @("-Path", $w2)
    Assert ($r.Rc -ne 0 -and (Test-Path -LiteralPath $w2) -and $r.Out -match "REFUSED") "unlanded detached commit: -Remove refused and worktree kept"
    $r = Run-MintRemove @("-Path", $w2, "-Force")
    Assert ($r.Rc -eq 0 -and -not (Test-Path -LiteralPath $w2)) "unlanded detached commit: -Remove -Force removes"
    # commit that is on a branch: allowed
    $w3 = MintPath
    git -C $w3 checkout -b keepme *>$null
    Set-Content -LiteralPath (Join-Path $w3 "k.txt") -Value "k" -Encoding ascii
    git -C $w3 add k.txt *>$null; git -C $w3 commit -m onbranch *>$null
    $r = Run-MintRemove @("-Path", $w3)
    Assert ($r.Rc -eq 0 -and -not (Test-Path -LiteralPath $w3)) "commit reachable from a branch: -Remove allowed"
    # rev-list error (a branch ref pointing at a missing object): fail closed, even with -Force
    $w5 = MintPath
    $gc = (git -C $work rev-parse --git-common-dir).Trim()
    if (-not [System.IO.Path]::IsPathRooted($gc)) { $gc = Join-Path $work $gc }
    $badRef = Join-Path $gc "refs/heads/brokenref"
    Set-Content -LiteralPath $badRef -Value "1111111111111111111111111111111111111111" -Encoding ascii
    $r = Run-MintRemove @("-Path", $w5, "-Force")
    Assert ($r.Rc -ne 0 -and (Test-Path -LiteralPath $w5) -and $r.Out -match "REFUSED") "rev-list error: -Remove -Force refused (fail closed) and worktree kept"
    Remove-Item -LiteralPath $badRef -Force
    $r = Run-MintRemove @("-Path", $w5)
    Assert ($r.Rc -eq 0 -and -not (Test-Path -LiteralPath $w5)) "after the bad ref is gone: -Remove allowed"
    # zero-byte loose ref: rev-list fails; REFUSED shows git's message; -ForceUnverifiable proceeds
    $w6 = MintPath
    $zeroRef = Join-Path $gc "refs/heads/zerobyte"
    New-Item -ItemType File -Path $zeroRef -Force | Out-Null
    $r = Run-MintRemove @("-Path", $w6, "-Force")
    Assert ($r.Rc -ne 0 -and (Test-Path -LiteralPath $w6) -and $r.Out -match "REFUSED" -and $r.Out -match "git said: \S") "zero-byte ref: refused with git's stderr shown"
    $r = Run-MintRemove @("-Path", $w6, "-Force", "-ForceUnverifiable")
    Assert ($r.Rc -eq 0 -and -not (Test-Path -LiteralPath $w6) -and $r.Out -match "WARNING.*-ForceUnverifiable") "zero-byte ref: -ForceUnverifiable removes with a warning"
    Remove-Item -LiteralPath $zeroRef -Force
    # unborn-looking worktree (HEAD -> absent-looking branch, empty reflog) whose branch ref is a
    # zero-byte LOOSE file: show-ref says "not found" (rc 1) but the file exists, so it must NOT be
    # classed unborn (sweepfx3 LOW-C; the `-not $looseExists` clause). Refused, worktree kept.
    $w6b = MintPath
    git -C $w6b symbolic-ref HEAD refs/heads/zerounborn *>$null
    $w6bGit = (git -C $w6b rev-parse --absolute-git-dir).Trim()
    Set-Content -LiteralPath (Join-Path $w6bGit "logs/HEAD") -Value $null -NoNewline
    $zeroRef2 = Join-Path $gc "refs/heads/zerounborn"
    New-Item -ItemType File -Path $zeroRef2 -Force | Out-Null
    $r = Run-MintRemove @("-Path", $w6b, "-Force")
    Assert ($r.Rc -ne 0 -and (Test-Path -LiteralPath $w6b) -and $r.Out -match "REFUSED" -and $r.Out -notmatch "unborn HEAD") "zero-byte loose ref + empty reflog: not unborn, refused"
    Remove-Item -LiteralPath $zeroRef2 -Force
    $r = Run-MintRemove @("-Path", $w6b, "-Force")
    Assert ($r.Rc -eq 0 -and -not (Test-Path -LiteralPath $w6b)) "same worktree with the ref file gone: genuinely unborn, removed"
    # remote ref pointing at a missing object
    $w7 = MintPath
    $remRef = Join-Path $gc "refs/remotes/origin/ghost"
    New-Item -ItemType Directory -Path (Split-Path -Parent $remRef) -Force | Out-Null
    Set-Content -LiteralPath $remRef -Value "2222222222222222222222222222222222222222" -Encoding ascii
    $r = Run-MintRemove @("-Path", $w7, "-Force")
    Assert ($r.Rc -ne 0 -and (Test-Path -LiteralPath $w7) -and $r.Out -match "REFUSED" -and $r.Out -match "git said: \S") "remote ref at missing object: refused with git's stderr shown"
    $r = Run-MintRemove @("-Path", $w7, "-Force", "-ForceUnverifiable")
    Assert ($r.Rc -eq 0 -and -not (Test-Path -LiteralPath $w7)) "remote ref at missing object: -ForceUnverifiable removes"
    Remove-Item -LiteralPath $remRef -Force
    # orphan worktree (unborn HEAD): nothing to lose, removable without any override
    $w8 = Join-Path $wtroot "orphan_wt"
    git -C $work worktree add --orphan -b orphanbr $w8 *>$null
    Assert (Test-Path -LiteralPath $w8) "orphan worktree created"
    $r = Run-MintRemove @("-Path", $w8)
    Assert ($r.Rc -eq 0 -and -not (Test-Path -LiteralPath $w8)) "orphan worktree (unborn HEAD): -Remove allowed"
    # deleted branch ref with an unlanded commit (sweepfx2 LOW-B): HEAD does not resolve but is NOT
    # a genuinely unborn branch -> -Force alone must refuse; -ForceUnverifiable proceeds
    $w9 = MintPath
    git -C $w9 checkout -b delme *>$null
    Set-Content -LiteralPath (Join-Path $w9 "d.txt") -Value "d" -Encoding ascii
    git -C $w9 add d.txt *>$null; git -C $w9 commit -m unlanded_delme *>$null
    git -C $work update-ref -d refs/heads/delme *>$null
    $r = Run-MintRemove @("-Path", $w9, "-Force")
    Assert ($r.Rc -ne 0 -and (Test-Path -LiteralPath $w9) -and $r.Out -match "REFUSED" -and $r.Out -match "not a genuinely unborn") "deleted branch ref: -Remove -Force refused (not treated as unborn)"
    $r = Run-MintRemove @("-Path", $w9, "-Force", "-ForceUnverifiable")
    Assert ($r.Rc -eq 0 -and -not (Test-Path -LiteralPath $w9)) "deleted branch ref: -ForceUnverifiable removes"
    # main tree refused, and nothing in it touched
    $r = Run-MintRemove @("-Path", $work)
    Assert ($r.Rc -ne 0 -and (Test-Path -LiteralPath (Join-Path $work "a.txt")) -and $r.Out -match "not a linked worktree") "main tree is refused"
    # relative path resolves against PowerShell location
    $w4 = MintPath
    $r = Run-MintRemove @("-Path", ("..\wt\" + (Split-Path -Leaf $w4)))
    Assert ($r.Rc -eq 0 -and -not (Test-Path -LiteralPath $w4)) "relative -Path resolves against the caller's location"
    # nonexistent / wildcard path
    $r = Run-MintRemove @("-Path", (Join-Path $wtroot "no[x]such"))
    Assert ($r.Rc -ne 0) "nonexistent path refused"
    Write-Host "case: guard defaults"
    foreach ($f in "commit_guard.ps1", "push_verify.ps1", "wt_status.ps1") {
        $t = Get-Content -Raw -LiteralPath (Join-Path $here $f)
        Assert ($t -match '\[string\]\$(Branch|Base)\s*=\s*"origin/dev"') "$f defaults to origin/dev"
    }
} finally { Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue }
if ($script:fails -gt 0) { Write-Host "check_worktree_mint: $script:fails FAILED" -ForegroundColor Red; exit 1 }
Write-Host "check_worktree_mint: all cases passed"; exit 0
