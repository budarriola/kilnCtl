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
    # Bounded: a hung push_verify child is killed after 60 s and reported as rc 99 (FAIL), never hangs the check.
    try {
        $outF = [IO.Path]::GetTempFileName(); $errF = [IO.Path]::GetTempFileName()
        $argl = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$(Join-Path $here 'push_verify.ps1')`"") + ($a | ForEach-Object { if ($_ -match '\s') { "`"$_`"" } else { $_ } })
        $p = Start-Process -FilePath "powershell" -ArgumentList $argl -NoNewWindow -PassThru -RedirectStandardOutput $outF -RedirectStandardError $errF
        $null = $p.Handle
        if (-not $p.WaitForExit(60000)) {
            try { $p.Kill() } catch { }
            $null = $p.WaitForExit(5000)
            $rc = 99; $o = "RUN-PV TIMEOUT after 60s"
        } else {
            $p.WaitForExit()
            $rc = $p.ExitCode
            $o = (Get-Content -LiteralPath $outF -Raw) + (Get-Content -LiteralPath $errF -Raw)
        }
    }
    finally { Pop-Location; Remove-Item -LiteralPath $outF, $errF -Force -ErrorAction SilentlyContinue }
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
    Write-Host "case: -Commit is required (P2); -FetchTimeoutSec validated (P3)"
    $r = Run-PV @()
    Assert ($r.Rc -eq 2 -and $r.Out -match "-Commit <sha> is required" -and $r.Out -notmatch "LANDED --") "no -Commit -> usage error exit 2, never LANDED for HEAD"
    git -C $work fetch -q origin *>$null
    $r = Run-PV @("-Commit", $c1, "-FetchTimeoutSec", "0")
    Assert ($r.Rc -eq 2 -and $r.Out -match "must be > 0") "-FetchTimeoutSec 0 -> usage error"
    $r = Run-PV @("-Commit", $c1, "-FetchTimeoutSec", "-5")
    Assert ($r.Rc -eq 2 -and $r.Out -match "must be > 0") "negative -FetchTimeoutSec -> usage error"

    Write-Host "case: failed fetch -> UNKNOWN, never LANDED (P3)"
    $goodUrl = (git -C $work remote get-url origin).Trim()
    git -C $work remote set-url origin (Join-Path $tmp "no_such_remote.git") *>$null
    $r = Run-PV @("-Commit", $c1)
    Assert ($r.Rc -eq 1 -and $r.Out -match "VERDICT: UNKNOWN" -and $r.Out -match "failed" -and $r.Out -notmatch "LANDED --") "unreachable remote -> UNKNOWN (a stale ref would have said LANDED)"

    Write-Host "case: hung fetch times out and the git process tree is killed (P3)"
    $lis = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, 0)
    $lis.Start(); $port = $lis.LocalEndpoint.Port
    try {
        git -C $work remote set-url origin "http://127.0.0.1:$port/x.git" *>$null
        $r = Run-PV @("-Commit", $c1, "-FetchTimeoutSec", "2")
        Assert ($r.Rc -eq 1 -and $r.Out -match "VERDICT: UNKNOWN" -and $r.Out -match "timed out") "a fetch that exceeds the timeout -> UNKNOWN, never LANDED"
        Start-Sleep -Milliseconds 500
        $left = @(Get-CimInstance Win32_Process -Filter "Name like 'git%'" -ErrorAction SilentlyContinue | Where-Object { $_.CommandLine -match "127\.0\.0\.1:$port" })
        Assert ($left.Count -eq 0) "no git process for the hung fetch survives the timeout"
    } finally { $lis.Stop() }
    git -C $work remote set-url origin $goodUrl *>$null

    Write-Host "case: explicit refspec beats a narrowed fetch refspec (P1)"
    git -C $other push -q origin HEAD:refs/heads/side *>$null                          # a branch that exists, so the narrowed fetch itself succeeds
    git -C $work config remote.origin.fetch "+refs/heads/side:refs/remotes/origin/side"
    $cx = Commit $other "x.txt" "cx doomed"
    git -C $other push origin dev *>$null
    git -C $work fetch -q origin "+refs/heads/dev:refs/remotes/origin/dev" *>$null      # work now knows cx on origin/dev
    git -C $other reset -q --hard HEAD~1 *>$null
    git -C $other push -q --force origin dev *>$null                                    # remote dev no longer has cx
    $r = Run-PV @("-Commit", $cx)
    Assert ($r.Rc -eq 1 -and $r.Out -match "NOT LANDED") "narrowed remote.origin.fetch cannot leave a stale ref that reports LANDED"
    git -C $work config remote.origin.fetch "+refs/heads/*:refs/remotes/origin/*"
} finally { Set-Location $here; Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue }
if ($script:fails -gt 0) { Write-Host "check_push_verify: $script:fails FAILED" -ForegroundColor Red; exit 1 }
Write-Host "check_push_verify: all cases passed"; exit 0
