# check_push_verify.ps1 -- push_verify.ps1 against a scratch bare origin (never a real remote).
# Covers F7: -Branch is ALWAYS resolved as refs/remotes/<remote>/<branch> after a fetch, so a commit
# stranded on a LOCAL branch named dev (or origin/dev) is never reported LANDED; a pushed commit is;
# a commit pushed by another clone is seen because the script fetches first.
$ErrorActionPreference = "Continue"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$script:fails = 0
. (Join-Path $here "lib_push_verify_walk.ps1")
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
# S3: survivors are polled (up to 10 s, every 250 ms) so a loaded machine does not give a false FAIL; on failure each survivor is printed.
function Get-PvSurvivors([int]$port, [string]$tmpLeaf) {
    @(Get-CimInstance Win32_Process -Filter "Name like 'git%'" -ErrorAction SilentlyContinue | Where-Object { $_.CommandLine -match "127\.0\.0\.1:$port" -or $_.CommandLine -match [regex]::Escape($tmpLeaf) })
}
function Assert-NoSurvivors([int]$port, [string]$tmpLeaf, [string]$w) {
    $deadline = [DateTime]::UtcNow.AddSeconds(10); $left = @(Get-PvSurvivors $port $tmpLeaf)
    while ($left.Count -gt 0 -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 250; $left = @(Get-PvSurvivors $port $tmpLeaf) }
    foreach ($s in $left) { Write-Host "  survivor: PID $($s.ProcessId) $($s.Name) $($s.CommandLine)" }
    Assert ($left.Count -eq 0) $w
}
function Commit([string]$repo, [string]$f, [string]$msg) {
    Set-Content -LiteralPath (Join-Path $repo $f) -Value $msg -Encoding ascii
    git -C $repo add $f *>$null; git -C $repo commit -m $msg *>$null
    return (git -C $repo rev-parse HEAD).Trim()
}
# R1: per-parent creation filter, tested on a synthetic snapshot (no process needed).
$t0 = [datetime]'2026-01-01T10:00:00'
function P($id, $ppid, $sec) { [pscustomobject]@{ ProcessId = $id; ParentProcessId = $ppid; CreationDate = $t0.AddSeconds($sec) } }
$snapR1 = @((P 18 12 1.5), (P 10 1 0), (P 11 10 1), (P 12 11 2), (P 13 11 -50), (P 14 12 -40), (P 15 10 0.0005), (P 16 10 -0.0005))
$w = @(Get-PvDescendantOrder -Snap $snapR1 -RootId 10 -RootCreated $t0 | ForEach-Object { $_.Id })
Assert (($w -contains 11) -and ($w -contains 12)) "R1: genuine chain kept"
Assert (-not ($w -contains 13)) "R1: child older than its parent (reused PID) dropped"
Assert (-not ($w -contains 18)) "R1: grandchild newer than the root but older than its own parent dropped"
Assert (-not ($w -contains 14)) "R1: grandchild older than its own parent dropped even though newer than the root"
Assert (($w -contains 15) -and ($w -contains 16)) "R1: root slack is about 1 ms (a child within 1 ms before the root start is kept)"
Assert ($w.IndexOf(11) -lt $w.IndexOf(12)) "R1: parent-first order"
$snapR1b = @((P 10 1 0), (P 17 10 -0.5))
Assert ((Get-PvDescendantOrder -Snap $snapR1b -RootId 10 -RootCreated $t0).Count -eq 0) "R1: child 0.5 s older than the root dropped (no 1 s slack)"
Assert (Test-PvStartMatch -Actual $t0 -Expected $t0.AddMilliseconds(1)) "R2: StartTime within tolerance of the snapshot matches"
Assert (-not (Test-PvStartMatch -Actual $t0.AddMinutes(5) -Expected $t0)) "R2: a reused PID (later StartTime) is skipped, not killed"
Assert (Test-PvStartMatch -Actual $t0 -Expected $t0.AddMilliseconds(1)) "S1: 1 ms difference matches"
Assert (-not (Test-PvStartMatch -Actual $t0 -Expected $t0.AddMilliseconds(5))) "S1: 5 ms difference does not match (tolerance ~2 ms)"
Assert (-not (Test-PvStartMatch -Actual $t0 -Expected $t0.AddMilliseconds(-5))) "S1: -5 ms difference does not match"
# S2: Stop-PvTargets kills only through a checked handle. Sacrificial process: one with a handle is killed, one without (reused-PID stand-in) survives.
$sacA = Start-Process -FilePath "ping" -ArgumentList "-n","40","127.0.0.1" -WindowStyle Hidden -PassThru
$sacB = Start-Process -FilePath "ping" -ArgumentList "-n","40","127.0.0.1" -WindowStyle Hidden -PassThru
try {
    $null = $sacA.Handle; $null = $sacB.Handle
    $hs = Open-PvTargets -Ids @($sacA.Id, $sacB.Id) -Created @{ $sacA.Id = $sacA.StartTime; $sacB.Id = $sacB.StartTime.AddMinutes(-30) }
    Assert ($hs.ContainsKey($sacA.Id) -and -not $hs.ContainsKey($sacB.Id)) "S2: precondition: only the matching-StartTime process gets a handle"
    Stop-PvTargets -Order ([int[]]@($sacA.Id, $sacB.Id)) -Handles $hs
    Assert ($sacA.WaitForExit(5000)) "S2: process with a verified handle is killed"
    Assert (-not $sacB.WaitForExit(1000)) "S2: process whose StartTime differs from the snapshot is NOT killed by bare PID"
} finally { foreach ($sp in @($sacA, $sacB)) { try { if (-not $sp.HasExited) { $sp.Kill() } } catch { } } }
$pvSrc = Get-Content -LiteralPath (Join-Path $here "push_verify.ps1") -Raw
# L1 (LOW-3): parse push_verify.ps1 with the PowerShell AST (no regex comment stripping, so a '#' inside a string cannot hide a kill),
# forbid every bare-PID kill command / member call, and scan the C# here-string with its own rules.
$pvTok = $null; $pvErr = $null
$pvAst = [System.Management.Automation.Language.Parser]::ParseInput($pvSrc, [ref]$pvTok, [ref]$pvErr)
$pvBadCmds = @('stop-process', 'kill', 'spps', 'taskkill', 'taskkill.exe')
$pvCmdHits = @($pvAst.FindAll({ param($n) $n -is [System.Management.Automation.Language.CommandAst] -and ($pvBadCmds -contains ([string]$n.GetCommandName()).ToLowerInvariant()) }, $true))
$pvMemHits = @($pvAst.FindAll({ param($n)
    $n -is [System.Management.Automation.Language.InvokeMemberExpressionAst] -and $n.Expression.Extent.Text -ne '[PvJob]' -and
    ([string]$n.Member.Extent.Text) -match '^(Kill|GetProcessById|GetProcessesByName|GetProcesses|GetProcess)$' }, $true))
$pvStopCalls = @($pvAst.FindAll({ param($n) $n -is [System.Management.Automation.Language.CommandAst] -and $n.GetCommandName() -eq 'Stop-PvTargets' }, $true))
$pvCs = ''
foreach ($hs in @($pvAst.FindAll({ param($n) $n -is [System.Management.Automation.Language.StringConstantExpressionAst] -and $n.StringConstantType -eq 'DoubleQuotedHereString' }, $true))) { $pvCs += $hs.Value + "`n" }
$pvCs = [regex]::Replace($pvCs, '//[^\r\n]*', '')
$pvCsBad = '(?i)GetProcessById|Process\.GetProcess\w*|\.Kill\s*\(|taskkill|Process\.Start\b|TerminateProcess\s*\((?!\s*(IntPtr\s+p\b|pi\.hp\s*,|h\s*,))'
Assert ($pvErr.Count -eq 0 -and $pvCmdHits.Count -eq 0 -and $pvMemHits.Count -eq 0 -and $pvStopCalls.Count -ge 1) "S2/L1: push_verify.ps1 (AST) has no bare-PID kill command or member call and kills via Stop-PvTargets"
Assert (($pvCs -match 'ResumeThread\(pi\.ht\)\s*==\s*0xFFFFFFFF') -and ($pvCs -match 'hin\s*==\s*bad') -and ($pvCs -match 'hout\s*==\s*bad') -and ($pvCs -match 'herr\s*==\s*bad')) "LOW-2: ResumeThread and every CreateFileW result is checked (INVALID_HANDLE_VALUE / 0xFFFFFFFF)"
Assert (($pvCs -match 'CreateProcessW\(gitExe,') -and ($pvSrc -match 'Get-Command git -CommandType Application')) "INFO-3: CreateProcessW gets the resolved git.exe path as lpApplicationName"
Assert ($pvCs.Length -gt 200 -and $pvCs -notmatch $pvCsBad) "L1: C# block has no GetProcessById / Process.GetProcess* / .Kill( / taskkill, and TerminateProcess only on held handles"
$me = Get-Process -Id $PID
$hm = Open-PvTargets -Ids @($PID) -Created @{ $PID = $me.StartTime }
Assert ($hm.ContainsKey($PID)) "R2: a live process whose StartTime matches the snapshot is opened"
$hx = Open-PvTargets -Ids @($PID) -Created @{ $PID = $me.StartTime.AddMinutes(-30) }
Assert (-not $hx.ContainsKey($PID)) "R2: a live process whose StartTime differs from the snapshot (reused PID) is skipped"
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
        Assert-NoSurvivors $port (Split-Path -Leaf $tmp) "no git process for the hung fetch survives the timeout"
        # I2: survivors are matched by the URL (git-remote-http) OR this run's unique scratch dir (wrapper git and inner git carry it in -C).
        # toolfx8 T1: force the no-job fallback (test hook) and require the whole tree, grandchild included, to die.
        $env:PUSH_VERIFY_TEST_NO_JOB = "1"
        try { $r = Run-PV @("-Commit", $c1, "-FetchTimeoutSec", "2") } finally { Remove-Item Env:\PUSH_VERIFY_TEST_NO_JOB -ErrorAction SilentlyContinue }
        Assert ($r.Rc -eq 1 -and $r.Out -match "VERDICT: UNKNOWN" -and $r.Out -match "timed out" -and $r.Out -match "no job membership" -and $r.Out -match "walk found [1-9]\d* descendant") "no-job fallback: hung fetch -> UNKNOWN, and the fallback path (not the job) ran"
        Assert-NoSurvivors $port (Split-Path -Leaf $tmp) "no-job fallback: no git process (incl. git-remote-http grandchild) survives the timeout"
        # M1 (toolfx10): job path with the create->assign window widened by 300 ms. The child is created suspended, so no descendant can escape the job.
        $env:PUSH_VERIFY_TEST_MODE = "1"; $env:PUSH_VERIFY_TEST_ASSIGN_DELAY_MS = "300"
        try { $r = Run-PV @("-Commit", $c1, "-FetchTimeoutSec", "2") } finally { Remove-Item Env:\PUSH_VERIFY_TEST_ASSIGN_DELAY_MS, Env:\PUSH_VERIFY_TEST_MODE -ErrorAction SilentlyContinue }
        Assert ($r.Rc -eq 1 -and $r.Out -match "VERDICT: UNKNOWN" -and $r.Out -match "timed out" -and $r.Out -notmatch "no job membership") "late job assign (300 ms): job path still used, hung fetch -> UNKNOWN"
        Assert-NoSurvivors $port (Split-Path -Leaf $tmp) "late job assign (300 ms): no git process (wrapper, git, remote-http) survives the timeout"
        # LOW-4: the delay hook is ignored without the test-mode flag (production env var stray), and capped with it.
        $env:PUSH_VERIFY_TEST_ASSIGN_DELAY_MS = "90000"
        try { $r = Run-PV @("-Commit", $c1, "-FetchTimeoutSec", "2") } finally { Remove-Item Env:\PUSH_VERIFY_TEST_ASSIGN_DELAY_MS -ErrorAction SilentlyContinue }
        Assert ($r.Rc -eq 1 -and $r.Out -match "timed out" -and $r.Out -notmatch "TEST HOOK") "stray PUSH_VERIFY_TEST_ASSIGN_DELAY_MS without PUSH_VERIFY_TEST_MODE: ignored (no stall)"
        $env:PUSH_VERIFY_TEST_MODE = "1"; $env:PUSH_VERIFY_TEST_ASSIGN_DELAY_MS = "99999999999"
        try { $r = Run-PV @("-Commit", $c1, "-FetchTimeoutSec", "2") } finally { Remove-Item Env:\PUSH_VERIFY_TEST_ASSIGN_DELAY_MS, Env:\PUSH_VERIFY_TEST_MODE -ErrorAction SilentlyContinue }
        Assert ($r.Rc -eq 1 -and $r.Out -match "timed out" -and $r.Out -notmatch "TEST HOOK") "oversized delay value parsed safely (ignored, no cast crash)"
        $env:PUSH_VERIFY_TEST_MODE = "1"; $env:PUSH_VERIFY_TEST_ASSIGN_DELAY_MS = "99999"
        try { $r = Run-PV @("-Commit", $c1, "-FetchTimeoutSec", "2") } finally { Remove-Item Env:\PUSH_VERIFY_TEST_ASSIGN_DELAY_MS, Env:\PUSH_VERIFY_TEST_MODE -ErrorAction SilentlyContinue }
        Assert ($r.Out -match "assign delay 5000 ms") "delay hook with test mode is capped at 5000 ms"
    } finally { $lis.Stop() }
    git -C $work remote set-url origin $goodUrl *>$null

    Write-Host "case: fetch exits before the first handle read (MED-1)"
    $env:PUSH_VERIFY_TEST_MODE = "1"; $env:PUSH_VERIFY_TEST_POST_LAUNCH_SLEEP_MS = "1500"
    try { $r = Run-PV @("-Commit", $c1) } finally { Remove-Item Env:\PUSH_VERIFY_TEST_POST_LAUNCH_SLEEP_MS, Env:\PUSH_VERIFY_TEST_MODE -ErrorAction SilentlyContinue }
    Assert ($r.Rc -eq 0 -and $r.Out -match "VERDICT: LANDED") "git exits 0 long before the exit code is read -> still LANDED (native handle held, no null ExitCode)"

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

