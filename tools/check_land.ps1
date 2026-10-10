# check_land.ps1 -- unit test for tools/land.ps1 against a throwaway bare
# origin plus clones under the temp dir (never the real origin).
# Cases: dirty-tree refusal, nothing-ahead refusal, main-tree refusal,
# conflict abort leaving a clean tree, non-fast-forward retry, FAIL-in-log
# refusal (UTF-16) and -AllowFail, no-summary log refusal, dry run, happy path.
# Post-rebase checks use a stub run_all_checks (-ChecksScript).
#
# Negative-tested by hand 2026-10-07: each case's guard in land.ps1 was broken
# in turn (see the commit message) and this check went RED, then restored.

$ErrorActionPreference = "Continue"
$land = Join-Path $PSScriptRoot "land.ps1"
$script:fails = 0
function Assert([bool]$cond, [string]$what) {
    if ($cond) { Write-Host "  ok: $what" } else { Write-Host "  FAIL: $what" -ForegroundColor Red; $script:fails++ }
}

$env:GIT_AUTHOR_NAME = "t"; $env:GIT_AUTHOR_EMAIL = "t@example.invalid"
$env:GIT_COMMITTER_NAME = "t"; $env:GIT_COMMITTER_EMAIL = "t@example.invalid"

$tmp = Join-Path ([IO.Path]::GetTempPath()) ("land_test_" + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Path $tmp | Out-Null
$origin = Join-Path $tmp "origin.git"

function G { param([string]$dir) git -C $dir @args *>$null }
function Commit-File([string]$dir, [string]$name, [string]$text, [string]$msg) {
    Set-Content -LiteralPath (Join-Path $dir $name) -Value $text -Encoding ascii
    git -C $dir add -- $name *>$null
    git -C $dir commit -m $msg *>$null
}
function New-Clone([string]$name) {
    $d = Join-Path $tmp $name
    git clone $origin $d *>$null
    return $d
}
function OriginHead { (git -C $origin rev-parse dev).Trim() }
function Write-Log([string]$dir, [string[]]$lines, [string]$tree = "", [switch]$Append) {
    $t = if ($tree) { $tree } else { (git -C $dir rev-parse "HEAD^{tree}").Trim() }
    $all = @("Run tree: $t dirty=0 partial=0", "Run mode: full") + $lines
    if ($Append) { Add-Content -LiteralPath $script:log -Value $all -Encoding Unicode } else { $all | Set-Content -LiteralPath $script:log -Encoding Unicode }
}
function Run-Land([string]$dir, [string[]]$more) {
    Push-Location $dir
    try {
        $out = & powershell -NoProfile -ExecutionPolicy Bypass -File $land -AllowStandaloneClone @more 2>&1 | Out-String
        $code = $LASTEXITCODE
    } finally { Pop-Location }
    $line = ($out -split "`r?`n" | Where-Object { $_.Trim().StartsWith("{") } | Select-Object -Last 1)
    $json = $null
    if ($line) { try { $json = $line | ConvertFrom-Json } catch {} }
    return [pscustomobject]@{ Code = $code; Json = $json; Out = $out }
}
function Stub([string]$body) {
    $p = Join-Path $tmp ("stub_" + [guid]::NewGuid().ToString("N").Substring(0, 6) + ".ps1")
    Set-Content -LiteralPath $p -Value ("param([string]`$Only,[switch]`$AllowFewerChecks)`n" + $body) -Encoding ascii
    return $p
}
$okStub = Stub "exit 0"

try {
    git init --bare -b main $origin *>$null
    $seed = New-Clone "seed"
    git -C $seed checkout -b main *>$null
    Commit-File $seed "f.txt" "base" "seed"
    Commit-File $seed "g.txt" "base" "seed2"
    git -C $seed push origin HEAD:main *>$null
    git -C $seed push origin HEAD:dev *>$null
    $log = Join-Path $tmp "run.log"

    Write-Host "case: nothing ahead"
    $c = New-Clone "c_ahead"
    $r = Run-Land $c @("-ChecksScript", $okStub)
    Assert ($r.Code -eq 1 -and $r.Json.error -match 'no commits ahead') "refused with nothing ahead"

    Write-Host "case: dirty tracked tree"
    $c = New-Clone "c_dirty"
    Commit-File $c "mine1.txt" "x" "mine"
    Add-Content -LiteralPath (Join-Path $c "f.txt") -Value "dirty"
    $before = OriginHead
    $r = Run-Land $c @("-ChecksScript", $okStub)
    Assert ($r.Code -eq 1 -and $r.Json.error -match 'tracked modifications') "refused on tracked modification"
    Assert ((OriginHead) -eq $before) "origin untouched"

    Write-Host "case: main/shared tree refused without the test switch"
    $c = New-Clone "c_main"
    Commit-File $c "mine2.txt" "x" "mine"
    Push-Location $c
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File $land -ChecksScript $okStub 2>&1 | Out-String
    $code = $LASTEXITCODE
    Pop-Location
    Assert ($code -eq 1 -and $out -match 'main/shared tree') "non-linked checkout refused"

    Write-Host "case: dry run"
    $c = New-Clone "c_dry"
    Commit-File $c "dry.txt" "x" "dry"
    $before = OriginHead
    $r = Run-Land $c @("-DryRun", "-ChecksScript", $okStub)
    Assert ($r.Code -eq 0 -and $r.Json.dry_run -eq $true -and $r.Json.landed -eq $false) "dry run exits 0, landed false"
    Assert ((OriginHead) -eq $before) "dry run pushed nothing"

    Write-Host "case: conflict aborts and leaves clean tree"
    $c = New-Clone "c_conf"
    Set-Content -LiteralPath (Join-Path $c "f.txt") -Value "mine" -Encoding ascii
    git -C $c commit -am "mine conflicting" *>$null
    $mine = (git -C $c rev-parse HEAD).Trim()
    $other = New-Clone "c_other"
    Commit-File $other "f.txt" "theirs" "theirs"
    git -C $other push origin HEAD:dev *>$null
    $before = OriginHead
    $r = Run-Land $c @("-ChecksScript", $okStub)
    Assert ($r.Code -eq 1 -and $r.Json.error -match 'conflict' -and $r.Json.error -match 'f\.txt') "conflict reported with file"
    Assert (-not (git -C $c status --porcelain --untracked-files=no)) "tree clean after abort"
    Assert (-not (Test-Path (Join-Path $c ".git\rebase-merge"))) "no rebase in progress"
    Assert (((git -C $c rev-parse HEAD).Trim()) -eq $mine) "HEAD back at local commit"
    Assert ((OriginHead) -eq $before) "nothing pushed"

    Write-Host "case: non-fast-forward retry"
    $c = New-Clone "c_nff"
    Commit-File $c "nff.txt" "x" "nff"
    $racer = New-Clone "c_racer"; git -C $racer checkout -b r origin/dev *>$null
    $marker = Join-Path $tmp "racer.marker"
    $racerStub = Stub ("if (-not (Test-Path '$marker')) { New-Item '$marker' -ItemType File | Out-Null; Set-Content -LiteralPath '$racer\race.txt' -Value r; git -C '$racer' add race.txt; git -C '$racer' commit -m race; git -C '$racer' push origin HEAD:dev }`nexit 0")
    $r = Run-Land $c @("-ChecksScript", $racerStub)
    $rebases = @($r.Json.steps | Where-Object { $_ -like 'rebase onto*' }).Count
    Assert ($r.Code -eq 0 -and $r.Json.landed -eq $true) "landed after retry"
    Assert ($rebases -eq 2) "rebased twice (got $rebases)"
    git -C $origin merge-base --is-ancestor $r.Json.sha dev *>$null
    Assert ($LASTEXITCODE -eq 0) "sha is on origin/dev"

    Write-Host "case: FAIL in UTF-16 log refused; -AllowFail permits"
    $c = New-Clone "c_log"
    Commit-File $c "log.txt" "x" "log"
    Write-Log $c @("  PASS  tools/check_a.ps1", "  FAIL  tools/check_x.ps1 (exit 1)", "1 of 3 checks FAILED:", "1 passed, 0 skipped (0 due to -Fast), 1 failed.")
    $before = OriginHead
    $r = Run-Land $c @("-CheckLog", $log, "-WaitTimeoutMin", "0.05", "-ChecksScript", $okStub)
    Assert ($r.Code -eq 1 -and $r.Json.error -match 'check_x') "FAIL refused"
    Assert ((OriginHead) -eq $before) "nothing pushed on refusal"
    $r = Run-Land $c @("-CheckLog", $log, "-AllowFail", "check_x", "-ChecksScript", $okStub)
    Assert ($r.Code -eq 0 -and $r.Json.landed -eq $true -and ($r.Json.allowed_fails -contains 'tools/check_x.ps1')) "allowed FAIL lands and is listed"
    Assert ($r.Out -match 'ALLOWED FAIL') "allowed FAIL echoed loudly"

    Write-Host "case: log without summary refused; green log accepted"
    $c = New-Clone "c_log2"
    Commit-File $c "log2.txt" "x" "log2"
    Write-Log $c @("  PASS  tools/check_a.ps1")
    $r = Run-Land $c @("-CheckLog", $log, "-WaitTimeoutMin", "0.05", "-ChecksScript", $okStub)
    Assert ($r.Code -eq 1 -and $r.Json.error -match 'no run_all_checks summary') "summary-less log refused"
    Write-Log $c @("5 passed, 0 skipped (0 due to -Fast), 0 failed.")
    $r = Run-Land $c @("-CheckLog", $log, "-WaitTimeoutMin", "0.05", "-ChecksScript", $okStub)
    Assert ($r.Code -eq 0 -and $r.Json.landed -eq $true) "green log lands"

    Write-Host "case: log held open by a writer (FileShare.ReadWrite) is still read; exclusive lock fails fast"
    $c = New-Clone "c_log3"
    Commit-File $c "log3.txt" "x" "log3"
    Write-Log $c @("5 passed, 0 skipped (0 due to -Fast), 0 failed.")
    $hold = New-Object System.IO.FileStream($log, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::ReadWrite)
    try {
        $r = Run-Land $c @("-CheckLog", $log, "-WaitTimeoutMin", "0.05", "-ChecksScript", $okStub)
        Assert ($r.Code -eq 0 -and $r.Json.landed -eq $true) "log held open with ReadWrite share lands"
    } finally { $hold.Dispose() }
    $c = New-Clone "c_log4"
    Commit-File $c "log4.txt" "x" "log4"
    Write-Log $c @("5 passed, 0 skipped (0 due to -Fast), 0 failed.")
    $hold = New-Object System.IO.FileStream($log, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
    try {
        $t0 = Get-Date
        $r = Run-Land $c @("-CheckLog", $log, "-WaitTimeoutMin", "5", "-ChecksScript", $okStub)
        Assert ($r.Code -eq 1 -and $r.Json.error -match 'cannot read check log' -and $r.Json.error -match 'run\.log') "exclusively locked log fails fast naming file"
        Assert (((Get-Date) - $t0).TotalSeconds -lt 120) "fail-fast well before the wait deadline"
    } finally { $hold.Dispose() }

    Write-Host "case: default target is dev; main untouched"
    $c = New-Clone "c_dev"
    git -C $c checkout -b work origin/dev *>$null
    Commit-File $c "dev.txt" "x" "devcommit"
    $mainBefore = (git -C $origin rev-parse main).Trim()
    $r = Run-Land $c @("-Target", "dev", "-ChecksScript", $okStub)
    Assert ($r.Code -eq 0 -and $r.Json.landed -eq $true) "land to dev succeeds (no check log needed)"
    Assert (((git -C $origin rev-parse main).Trim()) -eq $mainBefore) "origin/main not advanced by a dev land"
    Assert ((git -C $origin rev-parse dev).Trim() -eq $r.Json.sha) "origin/dev advanced to the landed sha"
    $c = New-Clone "c_dev2"
    git -C $c checkout -b work origin/dev *>$null
    Commit-File $c "dev2.txt" "x" "dev2"
    Push-Location $c
    try { $dout = & powershell -NoProfile -ExecutionPolicy Bypass -File $land -AllowStandaloneClone -DryRun 2>&1 | Out-String } finally { Pop-Location }
    Assert ($dout -match 'rebase onto origin/dev') "default target (no -Target) is dev"

    Write-Host "case: -Target main needs -Coordinator AND -CheckLog (F1)"
    $c = New-Clone "c_tm"
    Commit-File $c "tm.txt" "x" "tm"
    Write-Log $c @("5 passed, 0 skipped (0 due to -Fast), 0 failed.")
    $mainBefore = (git -C $origin rev-parse main).Trim()
    foreach ($variant in @(@("-Target", "main"), @("-Target", "main", "-Coordinator"), @("-Target", "main", "-CheckLog", $log))) {
        $r = Run-Land $c ($variant + @("-ChecksScript", $okStub))
        Assert ($r.Code -eq 1 -and $r.Out -match 'refused') "refused: $($variant -join ' ')"
    }
    Assert (((git -C $origin rev-parse main).Trim()) -eq $mainBefore) "main untouched by refusals"
    $r = Run-Land $c @("-Target", "main", "-Coordinator", "-CheckLog", $log, "-DryRun", "-ChecksScript", $okStub)
    Assert ($r.Code -eq 0 -and $r.Json.dry_run -eq $true) "main with -Coordinator and -CheckLog passes the gate (dry run)"

    Write-Host "case: check log evidence (F9, F10)"
    $c = New-Clone "c_ev"
    Commit-File $c "ev.txt" "x" "ev"
    $green = "5 passed, 0 skipped (0 due to -Fast), 0 failed."
    Write-Log $c @($green) "0123456789abcdef0123456789abcdef01234567"
    $r = Run-Land $c @("-CheckLog", $log, "-DryRun", "-ChecksScript", $okStub)
    Assert ($r.Code -eq 1 -and $r.Json.error -match 'tree') "log for a different tree refused"
    Write-Log $c @($green)
    Write-Log $c @("  FAIL  tools/check_x.ps1 (exit 1)", "1 passed, 0 skipped (0 due to -Fast), 1 failed.") -Append
    $r = Run-Land $c @("-CheckLog", $log, "-DryRun", "-ChecksScript", $okStub)
    Assert ($r.Code -eq 1) "appended run: the LAST (red) run decides"
    Write-Log $c @("  FAIL  tools/check_x.ps1 (exit 1)", "1 passed, 0 skipped (0 due to -Fast), 1 failed.")
    Write-Log $c @($green) -Append
    $r = Run-Land $c @("-CheckLog", $log, "-DryRun", "-ChecksScript", $okStub)
    Assert ($r.Code -eq 0) "appended run: the LAST (green) run decides"
    Write-Log $c @("  FAIL  tools/check_x.ps1 (exit 1)", "2 passed, 0 skipped (0 due to -Fast), 2 failed.")
    $r = Run-Land $c @("-CheckLog", $log, "-AllowFail", "check_x", "-DryRun", "-ChecksScript", $okStub)
    Assert ($r.Code -eq 1 -and $r.Out -match 'FAIL line') "FAIL-count mismatch refused even with -AllowFail"

    Write-Host "case: [remote rejected] is not retried (F11)"
    $c = New-Clone "c_rej"
    Commit-File $c "rej.txt" "x" "rej"
    $hook = Join-Path $origin "hooks/pre-receive"
    [IO.File]::WriteAllText($hook, "#!/bin/sh`necho 'no pushes today' >&2`nexit 1`n")
    $r = Run-Land $c @("-ChecksScript", $okStub)
    Remove-Item -LiteralPath $hook -Force
    $rebases = @($r.Json.steps | Where-Object { $_ -like 'rebase onto*' }).Count
    Assert ($r.Code -eq 1 -and $r.Out -match 'refused by the remote') "remote rejection fails with its message"
    Assert ($rebases -le 1) "no retry loop on remote rejection (rebases: $rebases)"

    Write-Host "case: -RestartMcp really runs mcp_servers.ps1 restart then status (F3)"
    $c = New-Clone "c_mcp"
    Commit-File $c "mcp.txt" "x" "mcp"
    $mdir = Join-Path $c "tools/PcTools/scripts"
    New-Item -ItemType Directory -Path $mdir -Force | Out-Null
    $mark = Join-Path $tmp "mcp.marker"
    Set-Content -LiteralPath (Join-Path $mdir "mcp_servers.ps1") -Encoding ascii -Value "param([string]`$Verb)`nAdd-Content -LiteralPath '$mark' -Value `$Verb`nexit 0"
    $r = Run-Land $c @("-RestartMcp", "-ChecksScript", $okStub)
    $mk = if (Test-Path $mark) { @(Get-Content $mark) } else { @() }
    Assert ($r.Code -eq 0 -and $mk -contains 'restart' -and $mk -contains 'status') "restart and status both ran (marker: $($mk -join ','))"

    Write-Host "case: failing post-rebase check blocks push"
    $c = New-Clone "c_post"
    Commit-File $c "post.txt" "x" "post"
    $before = OriginHead
    $r = Run-Land $c @("-ChecksScript", (Stub "exit 1"))
    Assert ($r.Code -eq 1 -and $r.Json.error -match 'post-rebase checks failed') "refused"
    Assert ((OriginHead) -eq $before) "nothing pushed"

    Write-Host "case: unpushed submodule pin blocks push; submodule_pins=fail"
    $c = New-Clone "c_pin"
    $om = $origin -replace '\\','/'
    Set-Content -LiteralPath (Join-Path $c ".gitmodules") -Value "[submodule `"m`"]`n`tpath = m`n`turl = $om`n"
    git -C $c add .gitmodules *>$null
    git -C $c update-index --add --cacheinfo "160000,1111111111111111111111111111111111111111,m" *>$null
    git -C $c commit -q -m "pin" *>$null
    New-Item -ItemType Directory -Path (Join-Path $c 'm') -Force | Out-Null
    $before = OriginHead
    $r = Run-Land $c @("-ChecksScript", $okStub)
    Assert ($r.Code -eq 1 -and $r.Json.submodule_pins -eq 'fail') "refused with submodule_pins=fail (out: $($r.Out.Trim() -replace '\s+',' '))"
    Assert ((OriginHead) -eq $before) "nothing pushed"
    Write-Host "case: -RemoveWorktree from a linked worktree whose process cwd is inside it"
    $mainc = New-Clone "c_wtmain"
    $wt = Join-Path $tmp "c_wt_linked"
    git -C $mainc worktree add -b wtbranch $wt *>$null
    Commit-File $wt "wt.txt" "x" "wt"
    $r = Run-Land $wt @("-RemoveWorktree", "-ChecksScript", $okStub)
    Assert ($r.Code -eq 0 -and $r.Json.landed -eq $true -and -not $r.Json.error) "landed and removal reported ok (out: $($r.Out.Trim() -replace '\s+',' '))"
    Assert (-not (Test-Path -LiteralPath $wt)) "worktree directory is gone (no empty remnant)"
    Assert (-not ((git -C $mainc worktree list --porcelain) -match 'c_wt_linked')) "worktree unregistered"

    Write-Host "case: two comma-separated -AllowFail regexes; no GetFullPath noise"
    $c = New-Clone "c_two"
    Commit-File $c "two.txt" "x" "two"
    Write-Log $c @("  FAIL  tools/check_x.ps1 (exit 1)", "  FAIL  tools/check_y.ps1 (exit 1)", "2 of 4 checks FAILED:", "2 passed, 0 skipped (0 due to -Fast), 2 failed.")
    $r = Run-Land $c @("-CheckLog", $log, "-AllowFail", "check_x,check_y", "-ChecksScript", $okStub)
    Assert ($r.Code -eq 0 -and $r.Json.landed -eq $true -and $r.Json.allowed_fails.Count -eq 2) "comma-separated -AllowFail accepts both"
    Assert ($r.Out -notmatch 'GetFullPath') "no GetFullPath exception in output"
    $wt2 = Join-Path $tmp "c_wt_two"
    git -C $c worktree add -b wtb2 $wt2 *>$null
    Commit-File $wt2 "wt2.txt" "x" "wt2"
    $r = Run-Land $wt2 @("-ChecksScript", $okStub)
    Assert ($r.Out -notmatch 'GetFullPath') "linked worktree: no GetFullPath exception"
} finally {
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}

if ($script:fails -gt 0) { Write-Host "check_land: $($script:fails) FAILED" -ForegroundColor Red; exit 1 }
Write-Host "check_land: all cases passed" -ForegroundColor Green
exit 0
