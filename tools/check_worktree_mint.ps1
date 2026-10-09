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
        try { $o = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $here "worktree_mint.ps1") -Label t -WtRoot $wtroot -NoSubmodules @more 2>&1 | Out-String } finally { Pop-Location }
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
        try { $o = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $here "worktree_mint.ps1") -Label r -WtRoot $wtroot -NoSubmodules 2>&1 | Out-String } finally { Pop-Location }
        return ([regex]::Match($o, '(?m)^WORKTREE:\s*(.+?)\s*$')).Groups[1].Value
    }
    function Run-MintRemove([string[]]$more, [string]$cwd = $work) {
        Push-Location $cwd
        try { $o = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $here "worktree_mint.ps1") -Remove @more 2>&1 | Out-String; $rc = $LASTEXITCODE } finally { Pop-Location }
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
