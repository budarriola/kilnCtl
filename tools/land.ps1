# land.ps1 -- one-shot landing script: the sequence every agent otherwise
# repeats by hand (check-log gate, fetch, rebase, post-rebase re-check, push,
# push_verify, optional MCP restart, optional worktree removal).
#
# It composes the existing guards and does not reimplement them:
#   tools\push_verify.ps1  (LANDED verdict, direction- and $?-safe)
#   tools\worktree_mint.ps1 -Remove
#   tools\PcTools\scripts\mcp_servers.ps1 restart|status
# commit_guard.ps1 is a PRE-COMMIT guard (it compares a working copy to
# origin/main before `git commit`), so it has no place after the commit exists;
# this script's equivalent protection is its refusal of a tracked-dirty tree.
#
# USAGE (run from inside the worktree whose commits are ready)
#   powershell -ExecutionPolicy Bypass -File tools\land.ps1
#       [-WaitPid <pid>] [-CheckLog <file>] [-AllowFail <regex>[,<regex>]]
#       [-AllowKnownFailures]
#       [-PostRebaseChecks <regex>] [-RestartMcp] [-RemoveWorktree] [-Target dev|main] [-Coordinator] [-DryRun]
#
#   -WaitPid / -CheckLog  wait (bounded by -WaitTimeoutMin, default 120) for a
#       run_all_checks run to finish, then parse its summary. UTF-16 logs (what
#       PowerShell's `>`/Tee write) are decoded. A log with no summary line, or
#       any `FAIL <check>` not matched by -AllowFail, or a `FAILED:` line, is a
#       refusal. Allowed FAILs are echoed loudly and listed in the final JSON.
#   -AllowKnownFailures   additionally accept a `FAIL <check>` that is KNOWN on
#       origin/main: the check also failed in the main baseline (C:\wt\.mainbaseline,
#       tools\main_baseline.ps1) recorded for an origin/main commit that is an
#       ANCESTOR of HEAD (other lineages are ignored). Mode (fast/full) is taken
#       from the log's text. Any NEW failure (passed or absent on main) still
#       refuses. -AllowFail is unchanged and checked first. The final JSON lists
#       "new_fails" and "known_fails".
#   -PostRebaseChecks     extra regex ORed onto the fixed post-rebase set
#       (check_mcp_tool_count_doc, check_mcp_facade_coverage); run via
#       run_all_checks -Only. A NARROW re-confirmation only, not the full suite.
#   -RestartMcp           mcp_servers.ps1 restart then status, using the MAIN
#       tree's copy (servers must serve the shared tree, not a worktree that is
#       about to be removed). The main tree must itself hold the landed commit
#       (fast-forward it first) or the restart just reloads old code; warned.
#   -Target dev|main      branch to rebase onto and push to (default dev). Agents land on dev;
#                         the coordinator promotes dev to main with tools\dev_promote.ps1.
#                         -Target main is REFUSED unless -Coordinator AND -CheckLog are both given
#                         (review F1): main normally only moves through dev_promote.ps1.
#   -CheckLog binding     the log must carry the "Run tree: <hash> dirty=0 partial=0" line that
#                         run_all_checks.ps1 prints, and <hash> must equal HEAD^{tree} (review F9);
#                         when a file holds several runs, the LAST run is the one judged. The count
#                         of parsed FAIL lines must equal the summary's failed count (review F10).
#   -RemoveWorktree       after LANDED only: cd out, worktree_mint -Remove.
#   -DryRun               do the refusals + log gate + fetch, report what would
#       happen, change nothing (no rebase, no push).
#   -ChecksScript         override run_all_checks.ps1 path (tests).
#   -AllowStandaloneClone permit running in a non-linked checkout (tests /
#       private clones). Without it a main/shared tree is always refused.
#
# GUARANTEES: never force-pushes; never leaves a half-rebased tree (any failed
# rebase is aborted and the tree verified clean); push retried only on a
# non-fast-forward rejection, at most 3 attempts.
#
# EXIT: 0 landed (and requested optional steps ok), 1 refused/failed, 2 usage.
# The LAST stdout line is always a one-line JSON result:
#   {"sha":...,"landed":bool,"steps":[...],"allowed_fails":[...],"dry_run":bool,"error":...}

[CmdletBinding()]
param(
    [int]$WaitPid = 0,
    [string]$CheckLog,
    [string[]]$AllowFail,
    [switch]$AllowKnownFailures,
    [string]$PostRebaseChecks,
    [switch]$RestartMcp,
    [switch]$RemoveWorktree,
    [ValidateSet('dev','main')][string]$Target = 'dev',
    [switch]$DryRun,
    [switch]$Coordinator,
    [double]$WaitTimeoutMin = 120,
    [string]$ChecksScript,
    [switch]$AllowStandaloneClone,
    [int]$MaxPushTries = 3,
    [double]$McpTimeoutMin = 10
)

# "Continue", not "Stop": see worktree_mint.ps1 (PS 5.1 native stderr). Every
# native call is checked through $LASTEXITCODE.
$ErrorActionPreference = "Continue"

# `powershell -File` passes "a,b" as ONE string: split commas so several regexes work.
$AllowFail = @($AllowFail | ForEach-Object { $_ -split ',' } | Where-Object { $_ })

$script:steps = New-Object System.Collections.ArrayList
$script:allowedFails = @()
$script:knownFails = @()
$script:newFails = @()
$script:sha = $null

function Finish([int]$code, [string]$err, [bool]$landed = $false) {
    $o = [ordered]@{
        sha           = $script:sha
        landed        = $landed
        steps         = @($script:steps)
        allowed_fails = @($script:allowedFails)
        known_fails   = @($script:knownFails)
        new_fails     = @($script:newFails)
        dry_run       = [bool]$DryRun
        error         = $(if ($err) { $err } else { $null })
    }
    if ($err) { Write-Host "REFUSED/FAILED: $err" -ForegroundColor Red }
    Write-Output ($o | ConvertTo-Json -Compress)
    exit $code
}
function Test-WorktreeRegistered([string]$root, [string]$wt) {
    $want = ($wt -replace '\\', '/').TrimEnd('/')
    foreach ($l in @(git -C $root worktree list --porcelain 2>$null)) {
        if ($l -like 'worktree *' -and (($l.Substring(9) -replace '\\', '/').TrimEnd('/') -ieq $want)) { return $true }
    }
    return $false
}
function Run-Bounded([string]$script, [string]$verb, [double]$timeoutMin) {
    # Run `powershell -File <script> <verb>` with a wall-clock cap. Returns the exit code, or -1
    # after killing the whole process tree on timeout, or -2 when it could not be started.
    try {
        $psi = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"' + $script + '"'), $verb)
        $p = Start-Process -FilePath 'powershell.exe' -ArgumentList $psi -PassThru -NoNewWindow
    } catch { return -2 }
    if ($null -eq $p) { return -2 }
    [void]$p.Handle   # cache the handle so ExitCode is readable after exit (PS 5.1)
    if (-not $p.WaitForExit([int][Math]::Max(1000, $timeoutMin * 60000))) {
        & taskkill.exe /PID $p.Id /T /F *>$null
        return -1
    }
    return [int]$p.ExitCode
}
function Step([string]$s) { [void]$script:steps.Add($s); Write-Host "[step] $s" -ForegroundColor Cyan }

function Read-TextAuto([string]$path) {
    # UTF-8 / UTF-16 LE / BE (BOM or NUL-heavy heuristic) tolerant read.
    # FileShare.ReadWrite|Delete so a log still held open by the writer (run_all_checks
    # tee, an editor) can be read; ReadAllBytes uses FileShare.Read and fails on it.
    $fs = New-Object System.IO.FileStream($path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, ([System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete))
    try { $b = New-Object byte[] ([int]$fs.Length); $off = 0; while ($off -lt $b.Length) { $n0 = $fs.Read($b, $off, $b.Length - $off); if ($n0 -le 0) { break }; $off += $n0 } }
    finally { $fs.Dispose() }
    if ($b.Length -ge 2 -and $b[0] -eq 0xFF -and $b[1] -eq 0xFE) { return [Text.Encoding]::Unicode.GetString($b, 2, $b.Length - 2) }
    if ($b.Length -ge 2 -and $b[0] -eq 0xFE -and $b[1] -eq 0xFF) { return [Text.Encoding]::BigEndianUnicode.GetString($b, 2, $b.Length - 2) }
    if ($b.Length -ge 3 -and $b[0] -eq 0xEF -and $b[1] -eq 0xBB -and $b[2] -eq 0xBF) { return [Text.Encoding]::UTF8.GetString($b, 3, $b.Length - 3) }
    $n = [Math]::Min($b.Length, 4096); $nul = 0
    for ($i = 1; $i -lt $n; $i += 2) { if ($b[$i] -eq 0) { $nul++ } }
    if ($n -gt 8 -and $nul -gt ($n / 4)) { return [Text.Encoding]::Unicode.GetString($b) }
    return [Text.Encoding]::UTF8.GetString($b)
}

$SummaryRe = '(?m)^\s*\d+ passed, \d+ skipped[^\r\n]*?, (\d+) failed\.'

function Git-Clean-Or-Die([string]$why) {
    $p = git status --porcelain --untracked-files=no 2>$null
    if ($p) { Finish 1 "$why; tree not clean: $($p -join '; ')" }
}

function Abort-Rebase {
    git rebase --abort *>$null
    $rm = git rev-parse --git-path rebase-merge 2>$null
    $ra = git rev-parse --git-path rebase-apply 2>$null
    if ((Test-Path $rm) -or (Test-Path $ra)) { Finish 1 "git rebase --abort did not clear the rebase state; fix by hand" }
    $p = git status --porcelain --untracked-files=no 2>$null
    if ($p) { Finish 1 "tree not clean after rebase --abort: $($p -join '; ')" }
}

# F1: main only moves through dev_promote.ps1; the direct path is a coordinator escape hatch.
if ($Target -eq 'main' -and (-not $Coordinator -or -not $CheckLog)) {
    Finish 1 "-Target main is refused: main moves only through tools\dev_promote.ps1. The direct path needs BOTH -Coordinator and -CheckLog <full run_all_checks log of this tree>."
}

# ---------------------------------------------------------------- step 1
$top = git rev-parse --show-toplevel 2>$null
if ($LASTEXITCODE -ne 0 -or -not $top) { Finish 2 "not inside a git repository" }
$top = $top -replace '/', '\'
Set-Location $top

$gitDir = (git rev-parse --absolute-git-dir 2>$null) -replace '/', '\'
$commonDir = (git rev-parse --git-common-dir 2>$null)
$commonDir = ($commonDir -replace '/', '\')
if (-not [System.IO.Path]::IsPathRooted($commonDir)) { $commonDir = Join-Path $top $commonDir }
$commonDir = [System.IO.Path]::GetFullPath($commonDir).TrimEnd('\')
$isLinked = ($gitDir.TrimEnd('\') -ne $commonDir)
if (-not $isLinked -and -not $AllowStandaloneClone) {
    Finish 1 "this is the main/shared tree (not a linked worktree); mint one with tools\worktree_mint.ps1 and land from there"
}
$mainRoot = if ($isLinked) { Split-Path -Parent $commonDir } else { $top }

# Fetch BEFORE anything reads origin/main: the -AllowKnownFailures gate below computes a
# merge-base and selects a baseline against it, and must see the current origin/main.
git fetch origin *>$null
if ($LASTEXITCODE -ne 0) { Finish 1 "git fetch origin failed" }

Step "preflight"
Git-Clean-Or-Die "tracked modifications present"
$ahead = git rev-list --count origin/$Target..HEAD 2>$null
if ($LASTEXITCODE -ne 0) { Finish 1 "cannot resolve origin/$Target; run git fetch origin" }
if ([int]$ahead -le 0) { Finish 1 "HEAD has no commits ahead of origin/$Target; nothing to land" }
$script:sha = (git rev-parse HEAD).Trim()

# ---------------------------------------------------------------- step 2
if ($WaitPid -gt 0 -or $CheckLog) {
    Step "check-log gate"
    $deadline = (Get-Date).AddMinutes($WaitTimeoutMin)
    if ($WaitPid -gt 0) {
        Write-Host "waiting for pid $WaitPid (up to $WaitTimeoutMin min) ..."
        while ((Get-Process -Id $WaitPid -ErrorAction SilentlyContinue) -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 5 }
        if (Get-Process -Id $WaitPid -ErrorAction SilentlyContinue) { Finish 1 "timed out waiting for pid $WaitPid" }
    }
    if (-not $CheckLog) { Finish 1 "-WaitPid given without -CheckLog; there is no summary to parse" }
    $text = $null
    $readFailSince = $null
    while ($true) {
        if (Test-Path -LiteralPath $CheckLog) {
            try { $text = Read-TextAuto $CheckLog; $readFailSince = $null }
            catch {
                $text = $null
                # Fail fast on a persistent read error instead of retrying to the deadline.
                if (-not $readFailSince) { $readFailSince = Get-Date }
                if (((Get-Date) - $readFailSince).TotalSeconds -ge 30) {
                    Finish 1 "cannot read check log '$CheckLog' for 30 s: $($_.Exception.Message)"
                }
            }
            if ($text -and $text -match $SummaryRe) { break }
        }
        if ($WaitPid -gt 0) { break }   # process already exited: no summary will appear
        if ((Get-Date) -ge $deadline) { break }
        Start-Sleep -Seconds 5
    }
    if (-not $text) { Finish 1 "check log '$CheckLog' missing or empty" }
    # F9: judge only the LAST run in the file (a log with several appended runs), and bind it to
    # the tree being landed. run_all_checks.ps1 prints "Run tree: <hash> dirty=<0|1> partial=<0|1>".
    $treeMs = @([regex]::Matches($text, '(?m)^Run tree: ([0-9a-f]{40}) dirty=(\d) partial=(\d)'))
    if ($treeMs.Count -eq 0) { Finish 1 "check log '$CheckLog' has no 'Run tree:' line; it cannot be tied to this tree (re-run run_all_checks.ps1 from this checkout)" }
    $lastRun = $treeMs[$treeMs.Count - 1]
    $text = $text.Substring($lastRun.Index)
    $headTree = (git rev-parse 'HEAD^{tree}').Trim()
    if ($lastRun.Groups[1].Value -ne $headTree) { Finish 1 "check log '$CheckLog' was run on tree $($lastRun.Groups[1].Value), but HEAD's tree is $headTree; run the checks on exactly what you are landing" }
    if ($lastRun.Groups[2].Value -ne '0') { Finish 1 "check log '$CheckLog' was run on a dirty working tree; its result does not describe a commit" }
    if ($lastRun.Groups[3].Value -ne '0') { Finish 1 "check log '$CheckLog' is a partial (-Only/-Skip) run" }
    $sms = @([regex]::Matches($text, $SummaryRe))
    if ($sms.Count -eq 0) { Finish 1 "check log '$CheckLog' has no run_all_checks summary line (run unfinished or died)" }
    $sm = $sms[$sms.Count - 1]
    $failedCount = [int]$sm.Groups[1].Value
    if ($text -match '(?m)^\s*\d+ passed,[^\r\n]*?, [1-9]\d* BUSY \(not run\)') { Finish 1 "check log reports BUSY checks that never ran" }
    $failNames = @([regex]::Matches($text, '(?m)^\s*FAIL\s+(\S+)\s+\((?:exit|killed)') | ForEach-Object { $_.Groups[1].Value } | Select-Object -Unique)
    $failedLines = @([regex]::Matches($text, '(?m)^FAILED:.*$') | ForEach-Object { $_.Value.Trim() })
    $blocked = @()
    foreach ($n in $failNames) {
        $ok = $false
        foreach ($re in $AllowFail) { if ($re -and $n -match $re) { $ok = $true; break } }
        if ($ok) { $script:allowedFails += $n } else { $blocked += $n }
    }
    foreach ($l in $failedLines) {
        $ok = $false
        foreach ($re in $AllowFail) { if ($re -and $l -match $re) { $ok = $true; break } }
        if ($ok) { $script:allowedFails += $l } else { $blocked += $l }
    }
    if ($AllowKnownFailures -and $blocked.Count -gt 0) {
        . (Join-Path $PSScriptRoot "checkcache_lib.ps1")
        . (Join-Path $PSScriptRoot "main_baseline_lib.ps1")
        $bmode = "fast"   # the standing run; a full run prints "Run mode: full"
        $mm = [regex]::Match($text, '(?m)^Run mode: (fast|full)')
        if ($mm.Success) { $bmode = $mm.Groups[1].Value }
        $sel = Select-MainBaseline -Dir (Get-MainBaselineDir) -Mode $bmode -RepoRoot $top
        if ($null -eq $sel.Baseline) {
            Write-Host "-AllowKnownFailures: no usable $bmode baseline: $($sel.Reason)" -ForegroundColor Yellow
        } else {
            if ($sel.Warning) { Write-Host "WARNING: $($sel.Warning)" -ForegroundColor Yellow }
            if (-not $sel.Exact) { Write-Host "-AllowKnownFailures: baseline is not at the merge-base with origin/main, so nothing counts as KNOWN. Record one there: tools\main_baseline.ps1 -Record" -ForegroundColor Yellow }
            $outs = Get-LogFailureOutputs -Text $text
            $cur = @($blocked | Where-Object { $failNames -contains $_ } | ForEach-Object {
                $o = if ($outs.ContainsKey($_)) { [string]$outs[$_] } else { "" }
                $ec = [regex]::Match($text, '(?m)^\s*FAIL\s+' + [regex]::Escape($_) + '\s+\(exit ([^)]*)\)')
                $code = if ($ec.Success) { $ec.Groups[1].Value } else { $null }
                [pscustomobject]@{ Path = $_; Status = "FAIL"; Signature = (Get-MainFailureSignature -Output $o -ExitCode $code) } })
            $cmp = Compare-MainBaseline -Current $cur -Baseline $sel.Baseline -Exact:$sel.Exact
            foreach ($w in $cmp.Warnings) { Write-Host "WARNING: $w" -ForegroundColor Yellow }
            $script:knownFails = @($cmp.Known)
            foreach ($k in $script:knownFails) { Write-Host "!!! KNOWN FAIL on origin/main (-AllowKnownFailures): $k" -ForegroundColor Yellow }
            $blocked = @($blocked | Where-Object { $script:knownFails -notcontains $_ })
        }
        $script:newFails = @($blocked)
    } elseif ($blocked.Count -gt 0) { $script:newFails = @($blocked) }
    # F10: the parsed FAIL lines must account for exactly the summary's failed count.
    if ($failNames.Count -ne $failedCount) { $blocked += "summary reports $failedCount failed but $($failNames.Count) FAIL line(s) were parsed; cannot classify them" }
    foreach ($a in $script:allowedFails) { Write-Host "!!! ALLOWED FAIL (-AllowFail): $a" -ForegroundColor Yellow }
    if ($blocked.Count -gt 0) { Finish 1 ("check log has unallowed FAIL: " + ($blocked -join '; ')) }
    Write-Host "check log OK ($failedCount failed, all allowed)" -ForegroundColor Green
}

# ---------------------------------------------------------------- fetch
git fetch origin *>$null
if ($LASTEXITCODE -ne 0) { Finish 1 "git fetch origin failed" }

if ($DryRun) {
    $behind = (git rev-list --count HEAD..origin/$Target).Trim()
    $ahead = (git rev-list --count origin/$Target..HEAD).Trim()
    Step "dry-run: would rebase onto origin/$Target (ahead $ahead, behind $behind)"
    Step "dry-run: would run post-rebase checks (check_mcp_tool_count_doc, check_mcp_facade_coverage$(if ($PostRebaseChecks) { ', ' + $PostRebaseChecks }))"
    Step "dry-run: would git push origin HEAD:$Target (no force) and push_verify"
    if ($RestartMcp) { Step "dry-run: would restart MCP servers" }
    if ($RemoveWorktree) { Step "dry-run: would remove worktree $top" }
    Write-Host "DRY RUN: nothing changed." -ForegroundColor Green
    Finish 0 $null $false
}

# ---------------------------------------------------------------- 3-5 loop
if (-not $ChecksScript) { $ChecksScript = Join-Path $top "tools\run_all_checks.ps1" }
$pushed = $false
for ($try = 1; $try -le $MaxPushTries; $try++) {
    if ($try -gt 1) { git fetch origin *>$null; if ($LASTEXITCODE -ne 0) { Finish 1 "git fetch origin failed" } }
    Step "rebase onto origin/$Target (attempt $try)"
    $out = (& git rebase "origin/$Target" 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) {
        $conf = @(git diff --name-only --diff-filter=U 2>$null)
        Abort-Rebase
        if ($conf.Count -gt 0) { Finish 1 ("rebase conflict (aborted, tree clean) in: " + ($conf -join ', ')) }
        if ($out -match 'unable to unlink|Invalid argument') {
            Finish 1 "rebase hit 'unable to unlink ... Invalid argument' (a process holds files in this worktree; close editors/builds/Search indexer, check with Sysinternals handle.exe or Resource Monitor, then re-run). Rebase aborted, tree clean."
        }
        Finish 1 "rebase failed (aborted, tree clean): $($out.Trim())"
    }
    $script:sha = (git rev-parse HEAD).Trim()

    $re = 'check_mcp_tool_count_doc|check_mcp_facade_coverage'
    if ($PostRebaseChecks) { $re += "|($PostRebaseChecks)" }
    Step "post-rebase checks (NARROW re-confirmation only, not the full suite): -Only '$re'"
    if (-not (Test-Path -LiteralPath $ChecksScript)) { Finish 1 "checks script not found: $ChecksScript" }
    # A check excused as KNOWN against the PRE-rebase merge-base is re-run on the rebased tree
    # and re-judged against the baseline at the NEW merge-base (-FailOnlyOnNew exits 0 only
    # when every failure is KNOWN there with an unchanged signature).
    $extra = @()
    if ($script:knownFails.Count -gt 0) {
        $kre = ($script:knownFails | ForEach-Object { ([regex]::Escape(($_ -replace '\\', '/')) -replace '/', '[\\/]') }) -join '|'
        $re += "|($kre)"
        $extra = @('-FailOnlyOnNew')
        Step "re-running $($script:knownFails.Count) known-failing check(s) after the rebase"
    }
    & powershell -NoProfile -ExecutionPolicy Bypass -File $ChecksScript -Only $re -AllowFewerChecks @extra
    if ($LASTEXITCODE -ne 0) { Finish 1 "post-rebase checks failed (exit $LASTEXITCODE); rebased commits remain local, nothing pushed" }

    Step "push origin HEAD:$Target (attempt $try)"
    $pout = (& git push origin HEAD:$Target 2>&1 | Out-String)
    if ($LASTEXITCODE -eq 0) { $pushed = $true; break }
    if ($pout -match '\[remote rejected\]') { Finish 1 "git push was refused by the remote (hook / protected branch), not a race: $($pout.Trim())" }
    if ($pout -match 'non-fast-forward|fetch first|\[rejected\]') {
        Write-Host "push rejected as non-fast-forward; re-fetching and rebasing" -ForegroundColor Yellow
        continue
    }
    Finish 1 "git push failed (not a non-fast-forward rejection): $($pout.Trim())"
}
if (-not $pushed) { Finish 1 "push still rejected after $MaxPushTries attempts" }

# ---------------------------------------------------------------- 6
Step "push_verify"
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "push_verify.ps1") -Commit $script:sha -Branch "origin/$Target"
if ($LASTEXITCODE -ne 0) { Finish 1 "push_verify did not report LANDED for $($script:sha)" }
Step "LANDED"

# ---------------------------------------------------------------- 7
$optFail = $null
if ($RestartMcp) {
    Step "restart MCP servers"
    $mcp = Join-Path $mainRoot "tools\PcTools\scripts\mcp_servers.ps1"
    if (-not (Test-Path -LiteralPath $mcp)) { $optFail = "mcp_servers.ps1 not found at $mcp" }
    else {
        git -C $mainRoot merge-base --is-ancestor $script:sha HEAD *>$null
        if ($LASTEXITCODE -ne 0) { Write-Host "WARNING: main tree HEAD does not contain $($script:sha); fast-forward it, otherwise the restart reloads OLD code." -ForegroundColor Yellow }
        $rc = Run-Bounded $mcp "restart" $McpTimeoutMin
        if ($rc -eq -1) { $optFail = "mcp_servers.ps1 restart exceeded $McpTimeoutMin min and was stopped" }
        elseif ($rc -ne 0) { $optFail = "mcp_servers.ps1 restart exited $rc" }
        $rc = Run-Bounded $mcp "status" 2
        if ($rc -ne 0 -and -not $optFail) { $optFail = "mcp_servers.ps1 status exited $rc" }
    }
}

# ---------------------------------------------------------------- 8
if ($RemoveWorktree) {
    if (-not $isLinked) { Write-Host "NOTE: not a linked worktree; -RemoveWorktree ignored." -ForegroundColor Yellow }
    else {
        Step "remove worktree"
        # ROOT CAUSE (2026-10-08): Set-Location only changes PowerShell's location, NOT the
        # process working directory ([Environment]::CurrentDirectory). land.ps1 is started
        # with the worktree as its cwd, so the process (and every child it spawns, incl. the
        # worktree_mint -Remove child) kept an OS directory handle on the worktree: git
        # deleted the contents and unregistered it, then RemoveDirectory failed, leaving an
        # empty unregistered dir (and a second -Remove then failed "not a git worktree").
        if (((Get-Location).Path.TrimEnd('\') + '\') -like (($top.TrimEnd('\')) + '\*') -or ([Environment]::CurrentDirectory.TrimEnd('\') + '\') -like (($top.TrimEnd('\')) + '\*')) {
            Write-Host "NOTE: the current directory is inside the worktree being removed ($top); moving to $mainRoot. A *calling* shell whose cwd is inside it will still hold the directory: cd out of it." -ForegroundColor Yellow
        }
        Set-Location $mainRoot
        [Environment]::CurrentDirectory = $mainRoot
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "worktree_mint.ps1") -Remove -Path $top
        $rmRc = $LASTEXITCODE
        $stillReg = Test-WorktreeRegistered $mainRoot $top
        if ($rmRc -ne 0 -or (Test-Path -LiteralPath $top) -or $stillReg) {
            Write-Host "worktree_mint -Remove left '$top' behind (exit $rmRc); falling back" -ForegroundColor Yellow
            . (Join-Path $PSScriptRoot "lib_safe_remove.ps1")
            if (Test-Path -LiteralPath $top) { [void](Remove-ReparsePointsUnder -Path $top) }
            git -C $mainRoot worktree remove --force $top *>$null
            if (Test-Path -LiteralPath $top) { Remove-TreeSafe -Path $top }
            git -C $mainRoot worktree prune *>$null
            $stillReg = Test-WorktreeRegistered $mainRoot $top
            if ((Test-Path -LiteralPath $top) -or $stillReg) {
                if (-not $optFail) { $optFail = "worktree removal failed for $top even after fallback (landed anyway): a process still holds it (shell cwd inside it?)" }
            } else { Write-Host "fallback removal succeeded: $top" -ForegroundColor Green }
        }
    }
}

if ($optFail) { Write-Host "WARNING: $optFail" -ForegroundColor Yellow; Finish 1 $optFail $true }
Finish 0 $null $true
