# wt_status.ps1 -- report on, and safely prune, the worktrees/dirs under C:\wt.
#
# WHY THIS EXISTS. C:\wt is a flat namespace shared by every concurrent
# session (several hundred entries by 2026-10-07). Cleanup passes recur, and
# CLAUDE.md's "What is safe to delete during cleanup" rules are easy to get
# wrong by hand: remove a dir another session is still using, or one holding
# unpushed commits, or recurse through a junction into the main tree (the
# 2026-10-07 .venv incident, tools\lib_safe_remove.ps1). This tool applies
# those rules mechanically.
#
# USAGE
#   powershell -ExecutionPolicy Bypass -File tools\wt_status.ps1            # report
#   powershell -ExecutionPolicy Bypass -File tools\wt_status.ps1 -Size      # + sizes (slow)
#   powershell -ExecutionPolicy Bypass -File tools\wt_status.ps1 -Prune -WhatIf   # dry run
#   powershell -ExecutionPolicy Bypass -File tools\wt_status.ps1 -Prune     # remove
#
# REPORT, per directory under -Root: registered in `git worktree list`?,
# branch or HEAD sha, commits ahead of -Base (default origin/dev), tracked-modified and
# untracked counts (gitignored excluded), idle time (newest file mtime,
# skipping build\ and .git internals and never entering a junction), live
# processes whose command line / executable path is under the directory,
# and a class:
#   ACTIVE       a live process references it, or a file changed < 15 min ago
#   HAS_WORK     registered, and has commits ahead of -Base, is dirty, holds gitignored
#                data under logs/ or elf_archive/, or is mid-rebase/cherry-pick/merge/revert
#   STALE_CLEAN  registered, nothing ahead, clean, idle > 2 h, no process
#   ORPHAN_DIR   not registered, no process, idle > 2 h
#   UNKNOWN      anything else (locked, git query failed, a .git inside an
#                unregistered dir, .kicad_* inside, a reparse point itself,
#                the directory running this tool, a clean tree idle 15 min-2 h)
# Helpers (not classified, never pruned): dot-dirs such as .buildgate and
# .checkcache, unregistered *_logs dirs, and loose files.
#
# PRUNE removes only STALE_CLEAN and ORPHAN_DIR, each re-evaluated just before
# its removal. Registered worktrees go through worktree_mint.ps1 -Remove
# (-Force: wt_status itself has proven nothing unlanded via git cherry and a clean tree; unlinks junctions first); orphan dirs
# go through lib_safe_remove.ps1's Remove-TreeSafe (reparse points are deleted
# as links, never followed). Then `git worktree prune`. -WhatIf lists and
# removes nothing. Unsure means skip. Exit 1 only if a removal failed.
#
# PARAMETERS beyond the above are for tests: -Root, -Repo (a repo whose
# `git worktree list` is authoritative), -Base (default origin/dev),
# -ActiveMinutes / -StaleHours thresholds, -PassThru (emit objects, never
# `exit`), -Quiet.
# LIMIT: another process's cwd is not readable from PowerShell 5.1, so a session whose
# cwd (but no command line) is in a worktree is only protected by the idle thresholds.

[CmdletBinding()]
param(
    [string]$Root = "C:\wt",
    [string]$Repo,
    [string]$Base = "origin/dev",
    [switch]$Size,
    [switch]$Prune,
    [switch]$WhatIf,
    [int]$ActiveMinutes = 15,
    [double]$StaleHours = 2,
    [int]$MaxParallel = 8,
    [switch]$PassThru,
    [switch]$Quiet
)

$ErrorActionPreference = "Continue"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $here "lib_safe_remove.ps1")

if (-not ('WtStatusWalk' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
public class WtGitResult {
    public int ExitCode;
    public List<string> Lines = new List<string>();
}
public class WtWalkResult {
    public long NewestTicksUtc;
    public long Bytes;
    public int Files;
    public bool HasKicad;
    public int Errors;
}
public static class WtStatusWalk {
    // Runs git with stdin closed (a pooled runspace has no usable console stdin,
    // and a native call from one hung on it) and stderr discarded.
    public static WtGitResult Git(string dir, string args) {
        WtGitResult r = new WtGitResult();
        System.Diagnostics.ProcessStartInfo psi = new System.Diagnostics.ProcessStartInfo("git", "-C \"" + dir + "\" " + args);
        psi.UseShellExecute = false; psi.CreateNoWindow = true;
        psi.RedirectStandardInput = true; psi.RedirectStandardOutput = true; psi.RedirectStandardError = true;
        try {
            using (System.Diagnostics.Process p = System.Diagnostics.Process.Start(psi)) {
                p.StandardInput.Close();
                System.Threading.Tasks.Task<string> err = p.StandardError.ReadToEndAsync();
                string line;
                while ((line = p.StandardOutput.ReadLine()) != null) r.Lines.Add(line);
                p.WaitForExit();
                err.Wait();
                r.ExitCode = p.ExitCode;
            }
        } catch (Exception) { r.ExitCode = -1; }
        return r;
    }

    // Junction-safe: a reparse point is never entered. Without computeSize,
    // build\ and .git directories are not entered at all; with it they are
    // entered for byte counting only (they never feed the mtime or kicad scan).
    public static WtWalkResult Walk(string root, bool computeSize) {
        WtWalkResult r = new WtWalkResult();
        Stack<KeyValuePair<string, bool>> stack = new Stack<KeyValuePair<string, bool>>();
        stack.Push(new KeyValuePair<string, bool>(root, false));
        while (stack.Count > 0) {
            KeyValuePair<string, bool> cur = stack.Pop();
            IEnumerable<FileSystemInfo> en;
            try { en = new DirectoryInfo(cur.Key).EnumerateFileSystemInfos(); }
            catch (Exception) { r.Errors++; continue; }
            try {
                foreach (FileSystemInfo fi in en) {
                    FileAttributes a = fi.Attributes;
                    if ((a & FileAttributes.ReparsePoint) != 0) continue;
                    if ((a & FileAttributes.Directory) != 0) {
                        bool skip = cur.Value;
                        if (!skip && (string.Equals(fi.Name, "build", StringComparison.OrdinalIgnoreCase)
                                      || string.Equals(fi.Name, ".git", StringComparison.OrdinalIgnoreCase))) {
                            if (!computeSize) continue;
                            skip = true;
                        }
                        stack.Push(new KeyValuePair<string, bool>(fi.FullName, skip));
                    } else {
                        if (computeSize) { try { r.Bytes += ((FileInfo)fi).Length; } catch (Exception) { r.Errors++; } }
                        if (cur.Value) continue;
                        r.Files++;
                        long t = fi.LastWriteTimeUtc.Ticks;
                        if (t > r.NewestTicksUtc) r.NewestTicksUtc = t;
                        if (fi.Name.IndexOf(".kicad_", StringComparison.OrdinalIgnoreCase) >= 0) r.HasKicad = true;
                    }
                }
            } catch (Exception) { r.Errors++; }
        }
        return r;
    }
}
'@
}

function Normalize-Path([string]$p) {
    if (-not $p) { return '' }
    return ([System.IO.Path]::GetFullPath(($p -replace '/', '\'))).TrimEnd('\').ToLowerInvariant()
}

if (-not $Repo) {
    $top = git -C $here rev-parse --show-toplevel 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $top) { Write-Host "ERROR: cannot find a git repo from $here (pass -Repo)." -ForegroundColor Red; if ($PassThru) { return } else { exit 1 } }
    $Repo = $top -replace '/', '\'
}
$rootFull = [System.IO.Path]::GetFullPath($Root).TrimEnd('\')
if (-not (Test-Path -LiteralPath $rootFull)) { Write-Host "ERROR: root '$rootFull' does not exist." -ForegroundColor Red; if ($PassThru) { return } else { exit 1 } }
$rootNorm = Normalize-Path $rootFull
if ($rootNorm -match '^[a-z]:$') { Write-Host "ERROR: refusing a drive root as -Root." -ForegroundColor Red; if ($PassThru) { return } else { exit 1 } }
$mint = Join-Path $here "worktree_mint.ps1"
$selfDirs = @((Normalize-Path $here), (Normalize-Path (Get-Location).Path))

# ---- registered worktrees ----
function Get-Registered {
    $map = @{}
    $lines = @(git -C $Repo worktree list --porcelain 2>$null)
    $cur = $null; $first = $true
    foreach ($ln in $lines) {
        if ($ln -like 'worktree *') {
            $cur = [pscustomobject]@{ Path = $ln.Substring(9); Locked = $false; Main = $first }
            $first = $false
            $map[(Normalize-Path $cur.Path)] = $cur
        } elseif ($ln -like 'locked*' -and $cur) { $cur.Locked = $true }
    }
    return $map
}

function Get-ProcessTexts {
    $texts = @()
    try {
        foreach ($p in (Get-CimInstance Win32_Process -ErrorAction Stop)) {
            $t = (("" + $p.CommandLine) + " " + ("" + $p.ExecutablePath)) -replace '/', '\'
            if ($t.Trim().Length -gt 0) { $texts += $t }
        }
    } catch { return $null }
    return ,$texts
}

function Get-ProcCount([string]$dirFull, $texts) {
    # Boundary-aware so C:\wt\foo does not match C:\wt\foo_bar.
    $rx = [regex]::new([regex]::Escape($dirFull) + '(?=$|[\\\s"''])', 'IgnoreCase')
    $n = 0
    foreach ($t in $texts) { if ($rx.IsMatch($t)) { $n++ } }
    return $n
}

$gather = {
    param($dir, $registered, $base, $wantSize)
    $ErrorActionPreference = 'Continue'
    $f = @{ Dir = $dir; GitOk = $true; Head = ''; Ahead = @(); Unlanded = 0; Ignored = 0; GitState = ''; Mod = 0; Untr = 0; Newest = 0; Bytes = 0; Kicad = $false; Errors = 0; HasGit = $false }
    $w = [WtStatusWalk]::Walk($dir, [bool]$wantSize)
    $f.Newest = $w.NewestTicksUtc; $f.Bytes = $w.Bytes; $f.Kicad = $w.HasKicad; $f.Errors = $w.Errors
    $topM = (Get-Item -LiteralPath $dir -Force).LastWriteTimeUtc.Ticks
    if ($topM -gt $f.Newest) { $f.Newest = $topM }
    $f.HasGit = Test-Path -LiteralPath (Join-Path $dir '.git')
    if ($registered) {
        $g2 = [WtStatusWalk]::Git($dir, "log --format=%H%x20%s $base..HEAD"); $rc2 = $g2.ExitCode; $ah = @($g2.Lines)
        # git cherry compares patch-ids, so a commit landed on main under a rebased sha reads "-".
        $gc = [WtStatusWalk]::Git($dir, "cherry $base HEAD"); $landed = @{}
        if ($gc.ExitCode -eq 0) { foreach ($cl in $gc.Lines) { if ($cl.Length -gt 2) { $landed[$cl.Substring(2)] = ($cl[0] -eq '-') } } }
        $unl = 0; $ah2 = @()
        foreach ($al in $ah) {
            $sp = $al.IndexOf(' '); $full = $(if ($sp -gt 0) { $al.Substring(0, $sp) } else { $al }); $subj = $(if ($sp -gt 0) { $al.Substring($sp + 1) } else { '' })
            if ($gc.ExitCode -eq 0 -and $landed[$full]) { $mk = '[on-main] ' } else { $mk = '[unlanded] '; $unl++ }
            $ah2 += ($mk + $full.Substring(0, [Math]::Min(8, $full.Length)) + ' ' + $subj)
        }
        $ah = $ah2; $f.Unlanded = $unl
        # Gitignored captured data under logs/ or elf_archive/ must never be pruned (CLAUDE.md).
        $gi = [WtStatusWalk]::Git($dir, 'status --porcelain=v1 --ignored')
        if ($gi.ExitCode -eq 0) { foreach ($il in $gi.Lines) { if ($il.StartsWith('!! ') -and $il.Substring(3) -match '(^|/)(logs|elf_archive)(/|$)') { $f.Ignored++ } } } else { $f.GitOk = $false }
        # Mid-operation worktrees keep their only copy of commits in rebase state / ORIG_HEAD.
        $gd = [WtStatusWalk]::Git($dir, 'rev-parse --absolute-git-dir')
        if ($gd.ExitCode -eq 0 -and $gd.Lines.Count -gt 0) {
            $gdir = $gd.Lines[0]
            foreach ($mk in 'rebase-merge', 'rebase-apply', 'CHERRY_PICK_HEAD', 'MERGE_HEAD', 'REVERT_HEAD') { if (Test-Path -LiteralPath (Join-Path $gdir $mk)) { $f.GitState = $mk; break } }
        } else { $f.GitOk = $false }
        $g3 = [WtStatusWalk]::Git($dir, 'status --porcelain=v2 --branch --untracked-files=all'); $rc3 = $g3.ExitCode
        if ($rc2 -ne 0 -or $rc3 -ne 0) { $f.GitOk = $false }
        else {
            $oid = ''; $hd = ''
            foreach ($s in $g3.Lines) {
                if ($s.StartsWith('# branch.oid ')) { $oid = $s.Substring(13) }
                elseif ($s.StartsWith('# branch.head ')) { $hd = $s.Substring(14) }
                elseif ($s.StartsWith('? ')) { $f.Untr++ }
                elseif ($s.StartsWith('#') -or -not $s) { }
                else { $f.Mod++ }
            }
            if ($hd -and $hd -ne '(detached)') { $f.Head = $hd } else { $f.Head = $oid.Substring(0, [Math]::Min(8, $oid.Length)) }
            $f.Ahead = $ah
        }
    }
    return $f
}

function Get-Idle($facts) {
    if ($facts.Newest -le 0) { return [double]::PositiveInfinity }
    $newest = New-Object DateTime ($facts.Newest, [DateTimeKind]::Utc)
    return ([DateTime]::UtcNow - $newest).TotalMinutes
}

function Get-Class($name, $dirFull, $facts, $reg, $isReparse, $procCount) {
    $note = ''
    if ($isReparse) { return @('UNKNOWN', 'is a reparse point') }
    $norm = Normalize-Path $dirFull
    foreach ($s in $selfDirs) { if ($s -eq $norm -or $s.StartsWith($norm + '\')) { return @('UNKNOWN', 'contains the directory running this tool') } }
    if ($reg -and $reg.Main) { return @('UNKNOWN', 'is the main worktree') }
    $idle = Get-Idle $facts
    if ($procCount -gt 0) { return @('ACTIVE', "$procCount live process(es)") }
    if ($idle -lt $ActiveMinutes) { return @('ACTIVE', 'file modified recently') }
    if ($reg) {
        if ($reg.Locked) { return @('UNKNOWN', 'worktree is locked') }
        if (-not $facts.GitOk) { return @('UNKNOWN', 'git query failed') }
        if ($facts.GitState) { return @('HAS_WORK', "mid-operation ($($facts.GitState))") }
        if ($facts.Ignored -gt 0) { return @('HAS_WORK', 'gitignored data under logs/ or elf_archive/') }
        if ($facts.Unlanded -gt 0 -or $facts.Mod -gt 0 -or $facts.Untr -gt 0) { return @('HAS_WORK', '') }
        if ($idle -gt ($StaleHours * 60)) { return @('STALE_CLEAN', '') }
        return @('UNKNOWN', 'clean but idle less than the stale threshold')
    }
    # Registered worktrees legitimately hold tracked .kicad_* files (a change shows up as Mod/Untr);
    # only an unregistered dir holding board files is left for owner review.
    if ($facts.Kicad) { return @('UNKNOWN', 'unregistered dir contains .kicad_* files') }
    if ($facts.HasGit) { return @('UNKNOWN', 'unregistered but has a .git') }
    if ($idle -gt ($StaleHours * 60)) { return @('ORPHAN_DIR', '') }
    return @('UNKNOWN', 'unregistered, idle less than the stale threshold')
}

function Test-IsReparse([string]$p) {
    return [bool]([System.IO.File]::GetAttributes($p) -band [System.IO.FileAttributes]::ReparsePoint)
}

function Get-IdleText([double]$min) {
    if ([double]::IsInfinity($min)) { return 'n/a' }
    if ($min -lt 120) { return ('{0:N0}m' -f $min) }
    if ($min -lt 2880) { return ('{0:N1}h' -f ($min / 60)) }
    return ('{0:N1}d' -f ($min / 1440))
}

# ---- gather ----
$registeredMap = Get-Registered
$procTexts = Get-ProcessTexts
$procKnown = ($null -ne $procTexts)
if (-not $procKnown) { $procTexts = @() }
$all = @(Get-ChildItem -LiteralPath $rootFull -Force)
$dirs = @($all | Where-Object { $_.PSIsContainer })
$files = @($all | Where-Object { -not $_.PSIsContainer })

$entries = New-Object System.Collections.Generic.List[object]
$helpers = New-Object System.Collections.Generic.List[object]
$jobs = @()
$pool = [runspacefactory]::CreateRunspacePool(1, [Math]::Max(1, $MaxParallel))
$pool.Open()
try {
    foreach ($d in $dirs) {
        $norm = Normalize-Path $d.FullName
        $reg = $registeredMap[$norm]
        if (-not $reg -and ($d.Name.StartsWith('.') -or $d.Name -like '*_logs')) {
            $helpers.Add([pscustomobject]@{ Name = $d.Name; Kind = $(if ($d.Name.StartsWith('.')) { 'helper dir' } else { 'log dir' }); Idle = (Get-IdleText (([DateTime]::UtcNow - $d.LastWriteTimeUtc).TotalMinutes)) })
            continue
        }
        $isReparse = Test-IsReparse $d.FullName
        if ($isReparse) {
            $entries.Add([pscustomobject]@{ Name = $d.Name; Full = $d.FullName; Reg = $reg; Reparse = $true; Facts = $null; Job = $null })
            continue
        }
        $ps = [powershell]::Create()
        $ps.RunspacePool = $pool
        [void]$ps.AddScript($gather.ToString()).AddArgument($d.FullName).AddArgument([bool]$reg).AddArgument($Base).AddArgument([bool]$Size)
        $entries.Add([pscustomobject]@{ Name = $d.Name; Full = $d.FullName; Reg = $reg; Reparse = $false; Facts = $null; Job = @{ Ps = $ps; Handle = $ps.BeginInvoke() } })
    }
    foreach ($e in $entries) {
        if ($e.Job) {
            $res = $e.Job.Ps.EndInvoke($e.Job.Handle)
            $e.Facts = $res | Where-Object { $_ -is [hashtable] } | Select-Object -First 1
            $e.Job.Ps.Dispose()
        }
    }
} finally { $pool.Close(); $pool.Dispose() }

foreach ($f in $files) {
    $helpers.Add([pscustomobject]@{ Name = $f.Name; Kind = 'file'; Idle = (Get-IdleText (([DateTime]::UtcNow - $f.LastWriteTimeUtc).TotalMinutes)) })
}

$rows = New-Object System.Collections.Generic.List[object]
foreach ($e in $entries) {
    if ($e.Reparse -or -not $e.Facts) {
        $cls = @('UNKNOWN', $(if ($e.Reparse) { 'is a reparse point' } else { 'could not be inspected' }))
        $facts = @{ Head = ''; Ahead = @(); Unlanded = 0; Ignored = 0; GitState = ''; Mod = 0; Untr = 0; Newest = 0; Bytes = 0 }
        $pc = 0
    } else {
        $facts = $e.Facts
        $pc = Get-ProcCount $e.Full $procTexts
        $cls = Get-Class $e.Name $e.Full $facts $e.Reg $false $pc
    }
    $note = $cls[1]
    if (-not $procKnown -and $cls[0] -in @('STALE_CLEAN', 'ORPHAN_DIR')) { $cls = @('UNKNOWN', 'process list unavailable'); $note = $cls[1] }
    $idle = if ($e.Facts) { Get-Idle $facts } else { [double]::PositiveInfinity }
    $rows.Add([pscustomobject]@{
        Name = $e.Name; Full = $e.Full; Class = $cls[0]; Note = $note
        Registered = [bool]$e.Reg; Head = $facts.Head; Ahead = @($facts.Ahead).Count; Unlanded = [int]$facts.Unlanded; AheadCommits = @($facts.Ahead)
        Modified = $facts.Mod; Untracked = $facts.Untr; IdleMinutes = $idle; Procs = $pc
        SizeMB = $(if ($Size) { [Math]::Round($facts.Bytes / 1MB, 1) } else { $null })
    })
}

# ---- report ----
$order = @{ ACTIVE = 0; HAS_WORK = 1; UNKNOWN = 2; STALE_CLEAN = 3; ORPHAN_DIR = 4 }
$sorted = @($rows | Sort-Object @{ Expression = { $order[$_.Class] } }, Name)
$counts = [ordered]@{}
foreach ($c in 'ACTIVE', 'HAS_WORK', 'STALE_CLEAN', 'ORPHAN_DIR', 'UNKNOWN') { $counts[$c] = @($rows | Where-Object { $_.Class -eq $c }).Count }
$summary = 'SUMMARY: ' + (($counts.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ' ') + " total=$($rows.Count) helpers=$($helpers.Count)"

if (-not $Quiet) {
    $baseSha = (git -C $Repo rev-parse --short $Base 2>$null)
    Write-Host "wt_status: root=$rootFull base=$Base ($baseSha) active<${ActiveMinutes}m stale>${StaleHours}h"
    if (-not $procKnown) { Write-Host "WARNING: process list unavailable; nothing is classified STALE_CLEAN/ORPHAN_DIR." -ForegroundColor Yellow }
    $cols = @('Name', 'Class', @{ N = 'Reg'; E = { if ($_.Registered) { 'Y' } else { 'N' } } }, 'Head', 'Ahead', 'Unlanded', @{ N = 'Mod'; E = { $_.Modified } }, @{ N = 'Untr'; E = { $_.Untracked } }, @{ N = 'Idle'; E = { Get-IdleText $_.IdleMinutes } }, 'Procs')
    if ($Size) { $cols += @{ N = 'SizeMB'; E = { $_.SizeMB } } }
    $cols += 'Note'
    ($sorted | Format-Table -Property $cols -AutoSize | Out-String -Width 250) | Write-Host
    foreach ($r in ($sorted | Where-Object { $_.Ahead -gt 0 })) {
        Write-Host "  ahead of ${Base}: $($r.Name)"
        foreach ($c in ($r.AheadCommits | Select-Object -First 10)) { Write-Host "    $c" }
        if ($r.Ahead -gt 10) { Write-Host "    ... and $($r.Ahead - 10) more (display truncated at 10; counts above are complete)" }
    }
    Write-Host ""
    Write-Host "Helper files and dirs (never pruned): $($helpers.Count)"
    if ($helpers.Count -gt 0) { ($helpers | Sort-Object Kind, Name | Format-Table Name, Kind, Idle -AutoSize | Out-String -Width 250) | Write-Host }
    Write-Host $summary
}

# ---- prune ----
$pruneResults = New-Object System.Collections.Generic.List[object]
$failed = 0
if ($Prune) {
    foreach ($r in ($rows | Where-Object { $_.Class -in @('STALE_CLEAN', 'ORPHAN_DIR') })) {
        $dirFull = $r.Full
        $res = [pscustomobject]@{ Name = $r.Name; Class = $r.Class; Action = ''; Result = '' }
        # Hard guards: direct child of Root, not a helper-style name, not a reparse point.
        $parentNorm = Normalize-Path (Split-Path -Parent $dirFull)
        if ($parentNorm -ne $rootNorm -or $r.Name.StartsWith('.') -or (Test-IsReparse $dirFull)) {
            $res.Action = 'skip'; $res.Result = 'guard: not a plain direct child of the root'; $pruneResults.Add($res); continue
        }
        # Re-evaluate just before acting; a session may have started using it.
        $reg2 = (Get-Registered)[(Normalize-Path $dirFull)]
        $f2 = & $gather $dirFull ([bool]$reg2) $Base $false
        $t2 = Get-ProcessTexts
        if ($null -eq $t2) { $res.Action = 'skip'; $res.Result = 'process list unavailable'; $pruneResults.Add($res); continue }
        $c2 = Get-Class $r.Name $dirFull $f2 $reg2 $false (Get-ProcCount $dirFull $t2)
        if ($c2[0] -ne $r.Class) { $res.Action = 'skip'; $res.Result = "re-check says $($c2[0]); left alone"; $pruneResults.Add($res); continue }
        if ($WhatIf) { $res.Action = 'would remove'; $res.Result = 'WhatIf'; $pruneResults.Add($res); continue }
        $res.Action = 'remove'
        try {
            if ($r.Class -eq 'STALE_CLEAN') {
                Push-Location -LiteralPath $Repo
                try { $out = & powershell -NoProfile -ExecutionPolicy Bypass -File $mint -Remove -Force -Path $dirFull -WtRoot $rootFull 2>&1; $rc = $LASTEXITCODE } finally { Pop-Location }
                if ($rc -ne 0) { $res.Result = "FAILED: worktree_mint -Remove exit $rc"; $failed++ }
                elseif (Test-Path -LiteralPath $dirFull) { $res.Result = 'PARTIAL: unregistered but directory remnant left'; $failed++ }
                else { $res.Result = 'removed' }
            } else {
                Remove-TreeSafe -Path $dirFull
                if (Test-Path -LiteralPath $dirFull) { $res.Result = 'PARTIAL: some content could not be deleted'; $failed++ }
                else { $res.Result = 'removed' }
            }
        } catch { $res.Result = "FAILED: $($_.Exception.Message)"; $failed++ }
        $pruneResults.Add($res)
    }
    if ($WhatIf) { $gp = @(git -C $Repo worktree prune -n -v 2>&1) } else { $gp = @(git -C $Repo worktree prune -v 2>&1) }
    if (-not $Quiet) {
        Write-Host ""
        Write-Host $(if ($WhatIf) { "PRUNE (WhatIf: nothing removed):" } else { "PRUNE:" })
        if ($pruneResults.Count -gt 0) { ($pruneResults | Format-Table -AutoSize | Out-String -Width 250) | Write-Host } else { Write-Host "  nothing to prune" }
        Write-Host "git worktree prune$(if ($WhatIf) { ' -n' }): $(@($gp).Count) line(s)"
        foreach ($g in $gp) { Write-Host "  $g" }
    }
}

if ($PassThru) {
    [pscustomobject]@{ Rows = $rows.ToArray(); Helpers = $helpers.ToArray(); Counts = $counts; Prune = $pruneResults.ToArray(); Failed = $failed }
    return
}
if ($failed -gt 0) { exit 1 }
exit 0

