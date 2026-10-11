# push_verify.ps1 -- verify a commit actually landed on origin/main, and say
# so unambiguously.
#
# WHY THIS EXISTS. This project has produced FOUR false "landed" reports
# from the same two causes, each costing a round trip to discover:
#
#   1. DIRECTION-SENSITIVE ANCESTRY CHECK. The correct question is
#     `git merge-base --is-ancestor <mine> origin/main` -- "is my commit an
#     ancestor of origin/main", i.e. did origin/main actually pick it up.
#     The reversed form, `git merge-base --is-ancestor origin/main HEAD`,
#     asks the opposite question and succeeds whenever your branch merely
#     sits ON TOP of (rebased onto, or descended from) origin/main --
#     including the case where your own commit is stranded on an unpushed
#     local branch that origin/main has never seen. Reversing the two
#     arguments silently flips a "no" into a "yes".
#   2. `$?` AFTER A NATIVE GIT CALL. PowerShell 5.1 sets `$?` to $false
#     after ANY native command that wrote to stderr, independent of its
#     real exit code -- and `git push`'s ordinary progress banner
#     ("Enumerating objects...", "To https://...") goes to stderr by
#     design. `if ($?) { "pushed" } else { "failed" }` immediately after a
#     `git push` has reported a genuinely successful push as FAILED.
#
# This script never uses `$?` after a native call and never parses push
# output. It determines success from `$LASTEXITCODE` alone, on the
# correctly-ordered ancestry check, after a fresh `git fetch`.
#
# USAGE
#   powershell -ExecutionPolicy Bypass -File tools\push_verify.ps1 -Commit <hash>   (required; no HEAD default) [-Branch origin/dev] [-FetchTimeoutSec 90]
#   (-Branch is always resolved on the remote: "dev" and "origin/dev" both mean refs/remotes/origin/dev)
#
# OUTPUT: exactly one unambiguous verdict line, prefixed "VERDICT: ", plus
# supporting detail above it. On "no", also names the local branch(es) the
# commit IS reachable from, if any -- "stranded on an unpushed local
# branch" is the actual root cause behind these false reports far more
# often than a genuine push failure.
#
# EXIT CODES:
#   0 -- landed (commit is an ancestor of the target branch on origin, after fetch)
#   1 -- not landed (fetch failed, commit unknown, or ancestry check says no)
#   2 -- bad usage

[CmdletBinding()]
param(
    # REQUIRED (P2, review 2026-10-10): a HEAD default reported LANDED for the base commit when the
    # caller forgot to commit or ran in the wrong tree. Name the sha you mean. Checked in the body
    # (not Mandatory=$true) so a missing value exits 2 instead of prompting.
    [string]$Commit = "",

    [string]$Branch = "origin/dev",

    # Bound on the fetch: a fetch that hangs (credential prompt, dead network) must not hang the caller.
    [int]$FetchTimeoutSec = 90
)

# See worktree_mint.ps1's header for why this is "Continue", not "Stop":
# `git fetch`/`git push`'s stderr progress banners would otherwise be
# treated as terminating errors under PS 5.1 even on exit code 0. Every
# call below is checked via $LASTEXITCODE explicitly.
$ErrorActionPreference = "Continue"

function Get-RepoRoot {
    $top = git rev-parse --show-toplevel 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $top) {
        Write-Host "ERROR: not inside a git repository." -ForegroundColor Red
        exit 2
    }
    return ($top -replace '/', '\')
}


if (-not $Commit) {
    Write-Host "ERROR: -Commit <sha> is required (there is no HEAD default: a forgotten commit would report the base commit LANDED)." -ForegroundColor Red
    exit 2
}
if ($FetchTimeoutSec -le 0) {
    Write-Host "ERROR: -FetchTimeoutSec must be > 0 (got $FetchTimeoutSec)." -ForegroundColor Red
    exit 2
}

$repoRoot = Get-RepoRoot

# Resolve the remote side of $Branch (e.g. "origin/main" -> remote "origin"). F7: the verdict is
# ALWAYS taken against refs/remotes/<remote>/<branch>, never a bare name that could resolve to a
# LOCAL branch ("-Branch dev" means origin/dev, and a local refs/heads/origin/dev cannot shadow it).
$remote = "origin"
$branchName = $Branch
$remoteList = @(git -C $repoRoot remote 2>$null)
if ($Branch -match '^([^/]+)/(.+)$' -and ($remoteList -contains $Matches[1])) {
    $remote = $Matches[1]
    $branchName = $Matches[2]
}
$Branch = "refs/remotes/$remote/$branchName"

Write-Host "Fetching $remote $branchName ..."
# Bounded fetch: run git as a child process, never prompt for credentials, kill the fetch process on timeout.
# P1: fetch with an EXPLICIT refspec so refs/remotes/<remote>/<branch> is updated even when the
# remote's configured fetch refspec does not cover this branch (a plain `git fetch` then exits 0 and
# leaves the ref stale, which would report LANDED off old data).
$env:GIT_TERMINAL_PROMPT = "0"
# Job object: a timed-out fetch is killed with everything it spawned (git-remote-https etc.) by job
# membership, never `taskkill /T` (which walks parent PIDs that can be stale and hit an unrelated process).
if (-not ('PvJob' -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class PvJob {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern IntPtr CreateJobObject(IntPtr a, string name);
    [DllImport("kernel32.dll")] static extern bool AssignProcessToJobObject(IntPtr job, IntPtr proc);
    [DllImport("kernel32.dll")] static extern bool TerminateJobObject(IntPtr job, uint code);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll")] static extern bool SetInformationJobObject(IntPtr h, int cls, IntPtr info, int len);
    public static IntPtr Create() {
        IntPtr job = CreateJobObject(IntPtr.Zero, null);
        if (job == IntPtr.Zero) return IntPtr.Zero;
        // JOBOBJECT_EXTENDED_LIMIT_INFORMATION: LimitFlags at offset 16, size 144 (x64) / 112 (x86)
        int size = IntPtr.Size == 8 ? 144 : 112;
        IntPtr buf = Marshal.AllocHGlobal(size);
        try {
            for (int i = 0; i < size; i++) Marshal.WriteByte(buf, i, 0);
            Marshal.WriteInt32(buf, 16, 0x2000); // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
            if (!SetInformationJobObject(job, 9, buf, size)) { CloseHandle(job); return IntPtr.Zero; }
        } finally { Marshal.FreeHGlobal(buf); }
        return job;
    }
    public static bool Assign(IntPtr j, IntPtr p) { return AssignProcessToJobObject(j, p); }
    public static void Kill(IntPtr j) { TerminateJobObject(j, 1); }
    public static void Close(IntPtr j) { CloseHandle(j); }
}
"@
}
$fetchJob = [PvJob]::Create()
$outTmp = [IO.Path]::GetTempFileName(); $errTmp = [IO.Path]::GetTempFileName()
$fetchOk = $false; $fetchMsg = ""
try {
    $fetchProc = Start-Process -FilePath "git" -NoNewWindow -PassThru -RedirectStandardOutput $outTmp -RedirectStandardError $errTmp `
        -ArgumentList @("-C", "`"$repoRoot`"", "fetch", "--no-tags", $remote, "+refs/heads/${branchName}:refs/remotes/$remote/$branchName")
    $null = $fetchProc.Handle
    # Assign result is checked: if the job cannot hold the process, a timeout falls back to killing git directly.
    $jobAssigned = $false
    if ($fetchJob -ne [IntPtr]::Zero) { $jobAssigned = [PvJob]::Assign($fetchJob, $fetchProc.Handle) }
    if (-not $fetchProc.WaitForExit($FetchTimeoutSec * 1000)) {
        if ($jobAssigned) { [PvJob]::Kill($fetchJob) }
        else {
            # no job membership: kill git's direct children (git-remote-*) by parent pid, then git itself
            try {
                Get-CimInstance Win32_Process -Filter "ParentProcessId=$($fetchProc.Id)" -ErrorAction SilentlyContinue |
                    ForEach-Object { try { Stop-Process -Id $_.ProcessId -Force -ErrorAction Stop } catch { } }
            } catch { }
        }
        try { if (-not $fetchProc.HasExited) { $fetchProc.Kill() } } catch { }
        $null = $fetchProc.WaitForExit(5000)
        $fetchMsg = "timed out after ${FetchTimeoutSec}s"
    } elseif ($fetchProc.ExitCode -ne 0) {
        $fetchMsg = "failed (exit $($fetchProc.ExitCode))"
    } else { $fetchOk = $true }
} finally {
    if ($fetchJob -ne [IntPtr]::Zero) { [PvJob]::Close($fetchJob) }
    foreach ($tf in @($outTmp, $errTmp)) {
        # a killed fetch child can still hold the file for a moment: best-effort retry
        for ($i = 0; $i -lt 5; $i++) { try { [IO.File]::Delete($tf); break } catch { Start-Sleep -Milliseconds 300 } }
    }
}
if (-not $fetchOk) {
    Write-Host "VERDICT: UNKNOWN -- git fetch $remote $fetchMsg; cannot verify against a stale view. NOT LANDED (unverified)." -ForegroundColor Red
    exit 1
}

# Resolve the commit to a full hash so partial hashes / refs work, and so we
# fail loudly if it doesn't even exist locally.
$fullHash = git -C $repoRoot rev-parse --verify "$Commit^{commit}" 2>$null
if ($LASTEXITCODE -ne 0 -or -not $fullHash) {
    Write-Host "VERDICT: NOT LANDED -- '$Commit' does not resolve to a commit in this repo." -ForegroundColor Red
    exit 1
}

git -C $repoRoot rev-parse --verify --quiet "$Branch^{commit}" *>$null
if ($LASTEXITCODE -ne 0) {
    Write-Host "VERDICT: UNKNOWN -- $Branch does not exist after fetching $remote." -ForegroundColor Red
    exit 1
}

# THE CORRECT DIRECTION: is $fullHash an ancestor of $Branch? Arguments in
# exactly this order -- reversing them asks "is $Branch an ancestor of
# HEAD", a different and commonly misleading question (see header).
git -C $repoRoot merge-base --is-ancestor $fullHash $Branch 2>$null
$isAncestor = ($LASTEXITCODE -eq 0)

if ($isAncestor) {
    Write-Host "VERDICT: LANDED -- $fullHash is an ancestor of $Branch." -ForegroundColor Green
    exit 0
}

# Not landed. Name which local branch(es), if any, the commit IS reachable
# from -- the actual real-world root cause behind these false reports has
# consistently been "stranded on an unpushed local branch", not a genuine
# push failure, so make that diagnosis available immediately rather than
# leaving the caller to re-derive it.
$containingBranches = git -C $repoRoot branch --contains $fullHash --format='%(refname:short)' 2>$null
$containingBranches = $containingBranches | Where-Object { $_ }

Write-Host "VERDICT: NOT LANDED -- $fullHash is NOT an ancestor of $Branch." -ForegroundColor Red
if ($containingBranches) {
    Write-Host "It IS reachable from the following local branch(es) (likely stranded/unpushed):" -ForegroundColor Yellow
    foreach ($b in $containingBranches) {
        Write-Host "  $b"
    }
} else {
    Write-Host "It is not reachable from any local branch either -- check the hash." -ForegroundColor Yellow
}
exit 1
