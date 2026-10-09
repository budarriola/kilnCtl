# check_push_verify.ps1 -- push_verify.ps1 against a scratch bare origin (never a real remote).
# Covers F7: -Branch is ALWAYS resolved as refs/remotes/<remote>/<branch> after a fetch, so a commit
# stranded on a LOCAL branch named dev (or origin/dev) is never reported LANDED; a pushed commit is;
# a commit pushed by another clone is seen because the script fetches first.
$ErrorActionPreference = "Continue"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$script:fails = 0
function Assert([bool]$c, [string]$w) { if ($c) { Write-Host "  ok: $w" } else { Write-Host "  FAIL: $w" -ForegroundColor Red; $script:fails++ } }
$env:GIT_AUTHOR_NAME = "t"; $env:GIT_AUTHOR_EMAIL = "t@example.invalid"
$env:GIT_COMMITTER_NAME = "t"; $env:GIT_COMMITTER_EMAIL = "t@example.invalid"
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("pushver_chk_" + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Path $tmp | Out-Null
$origin = Join-Path $tmp "origin.git"; $work = Join-Path $tmp "work"; $other = Join-Path $tmp "other"
function Run-PV([string[]]$a) {
    Push-Location $work
    try { $o = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $here "push_verify.ps1") @a 2>&1 | Out-String; $rc = $LASTEXITCODE }
    finally { Pop-Location }
    return [pscustomobject]@{ Rc = $rc; Out = $o }
}
function Commit([string]$repo, [string]$f, [string]$msg) {
    Set-Content -LiteralPath (Join-Path $repo $f) -Value $msg -Encoding ascii
    git -C $repo add $f *>$null; git -C $repo commit -m $msg *>$null
    return (git -C $repo rev-parse HEAD).Trim()
}
try {
    git init --bare -b dev $origin *>$null
    git clone $origin $work *>$null
    git -C $work checkout -b dev *>$null
    $c1 = Commit $work "a.txt" "c1"
    git -C $work push origin dev *>$null

    Write-Host "case: pushed commit is LANDED (default and bare branch name)"
    $r = Run-PV @("-Commit", $c1)
    Assert ($r.Rc -eq 0 -and $r.Out -match "VERDICT: LANDED") "default -Branch origin/dev -> LANDED"
    $r = Run-PV @("-Commit", $c1, "-Branch", "dev")
    Assert ($r.Rc -eq 0 -and $r.Out -match "VERDICT: LANDED") "-Branch dev resolves as origin/dev -> LANDED"

    Write-Host "case: unpushed commit on LOCAL dev is NOT LANDED (F7)"
    $c2 = Commit $work "b.txt" "c2 unpushed"
    $r = Run-PV @("-Commit", $c2, "-Branch", "dev")
    Assert ($r.Rc -eq 1 -and $r.Out -match "NOT LANDED") "-Branch dev does not trust local dev"
    $r = Run-PV @("-Commit", $c2)
    Assert ($r.Rc -eq 1 -and $r.Out -match "NOT LANDED" -and $r.Out -match "stranded") "default branch: NOT LANDED, names the stranded local branch"
    git -C $work branch origin/dev $c2 *>$null
    $r = Run-PV @("-Commit", $c2, "-Branch", "origin/dev")
    Assert ($r.Rc -eq 1 -and $r.Out -match "NOT LANDED") "a local branch named origin/dev cannot shadow the remote ref"
    git -C $work branch -D origin/dev *>$null

    Write-Host "case: commit pushed by another clone is seen (fetch first)"
    git clone $origin $other *>$null
    git -C $other checkout dev *>$null
    $c3 = Commit $other "c.txt" "c3 from other"
    git -C $other push origin dev *>$null
    git -C $work cat-file -e $c3 2>$null
    Assert ($LASTEXITCODE -ne 0) "precondition: work has not fetched c3"
    $r = Run-PV @("-Commit", $c3)
    Assert ($r.Rc -eq 0 -and $r.Out -match "VERDICT: LANDED") "verify fetches, so c3 is LANDED"

    Write-Host "case: refusals"
    $r = Run-PV @("-Commit", "deadbeefdeadbeefdeadbeefdeadbeefdeadbeef")
    Assert ($r.Rc -eq 1 -and $r.Out -match "does not resolve") "unknown commit -> NOT LANDED"
    $r = Run-PV @("-Commit", $c1, "-Branch", "origin/nosuch")
    Assert ($r.Rc -eq 1 -and $r.Out -match "UNKNOWN") "missing remote branch -> UNKNOWN, not LANDED"
} finally { Set-Location $here; Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue }
if ($script:fails -gt 0) { Write-Host "check_push_verify: $script:fails FAILED" -ForegroundColor Red; exit 1 }
Write-Host "check_push_verify: all cases passed"; exit 0
