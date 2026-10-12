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
. (Join-Path $PSScriptRoot "lib_push_verify_walk.ps1")

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
    // M1 (toolfx10): the child is created SUSPENDED, joined to the job, then resumed, so it (and every
    // descendant) is a job member from its first instruction; a late assign can no longer leak the tree.
    // assignDelayMs is a test hook only (PUSH_VERIFY_TEST_ASSIGN_DELAY_MS): it widens the create->assign window.
    [StructLayout(LayoutKind.Sequential)] struct SA { public int n; public IntPtr d; public int inh; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] struct SI {
        public int cb; public string r, desk, title; public int x, y, xs, ys, xc, yc, fill, flags; public short show, res2;
        public IntPtr res2p, hin, hout, herr; }
    [StructLayout(LayoutKind.Sequential)] struct PI { public IntPtr hp, ht; public int pid, tid; }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern bool CreateProcessW(string app, System.Text.StringBuilder cmd, IntPtr pa, IntPtr ta, bool inh, uint fl, IntPtr env, string dir, ref SI si, out PI pi);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern IntPtr CreateFileW(string f, uint acc, uint share, ref SA sa, uint disp, uint fl, IntPtr t);
    [DllImport("kernel32.dll")] static extern uint ResumeThread(IntPtr t);
    [DllImport("kernel32.dll")] static extern bool TerminateProcess(IntPtr p, uint c);
    [DllImport("kernel32.dll")] static extern uint WaitForSingleObject(IntPtr h, uint ms);
    [DllImport("kernel32.dll")] static extern bool GetExitCodeProcess(IntPtr h, out uint code);
    [DllImport("kernel32.dll")] static extern bool GetProcessTimes(IntPtr h, out long c, out long e, out long k, out long u);
    // The caller owns the returned native process handle (hp) and uses it for the wait, the exit code and the
    // kill, so the exit code is never lost to a child that exits early and a PID is never re-opened.
    // INFO-2: bInheritHandles stays TRUE without PROC_THREAD_ATTRIBUTE_HANDLE_LIST (needs STARTUPINFOEX plus
    // attribute-list marshalling); the job kill covers the normal path, so inheritance is left as documented.
    public static IntPtr Launch(string gitExe, string cmdLine, string outPath, string errPath, IntPtr job, int assignDelayMs, out bool assigned, out int pid) {
        assigned = false; pid = 0;
        SA sa = new SA(); sa.n = Marshal.SizeOf(typeof(SA)); sa.inh = 1;
        IntPtr bad = new IntPtr(-1);
        IntPtr hin = CreateFileW("NUL", 0x80000000, 3, ref sa, 3, 0x80, IntPtr.Zero);
        int e1 = Marshal.GetLastWin32Error();
        IntPtr hout = CreateFileW(outPath, 0x40000000, 7, ref sa, 2, 0x80, IntPtr.Zero);
        int e2 = Marshal.GetLastWin32Error();
        IntPtr herr = CreateFileW(errPath, 0x40000000, 7, ref sa, 2, 0x80, IntPtr.Zero);
        int e3 = Marshal.GetLastWin32Error();
        try {
            if (hin == bad) throw new System.ComponentModel.Win32Exception(e1, "CreateFileW(NUL) failed");
            if (hout == bad) throw new System.ComponentModel.Win32Exception(e2, "CreateFileW(stdout temp) failed");
            if (herr == bad) throw new System.ComponentModel.Win32Exception(e3, "CreateFileW(stderr temp) failed");
            SI si = new SI(); si.cb = Marshal.SizeOf(typeof(SI)); si.flags = 0x100; si.hin = hin; si.hout = hout; si.herr = herr;
            PI pi;
            if (!CreateProcessW(gitExe, new System.Text.StringBuilder(cmdLine), IntPtr.Zero, IntPtr.Zero, true, 0x4, IntPtr.Zero, null, ref si, out pi))
                throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(), "CreateProcessW failed");
            bool keep = false;
            try {
                if (assignDelayMs > 0) System.Threading.Thread.Sleep(assignDelayMs);
                if (job != IntPtr.Zero) assigned = AssignProcessToJobObject(job, pi.hp);
                if (ResumeThread(pi.ht) == 0xFFFFFFFF) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(), "ResumeThread failed");
                pid = pi.pid; keep = true;
                return pi.hp;
            } catch { TerminateProcess(pi.hp, 1); throw; }
            finally { CloseHandle(pi.ht); if (!keep) CloseHandle(pi.hp); }
        } finally {
            if (hin != bad) CloseHandle(hin);
            if (hout != bad) CloseHandle(hout);
            if (herr != bad) CloseHandle(herr);
        }
    }
    public static bool WaitExit(IntPtr h, int ms) { return WaitForSingleObject(h, (uint)ms) == 0; }
    public static long ExitCode(IntPtr h) { uint c; if (!GetExitCodeProcess(h, out c)) return -1; return c; }
    public static void KillProc(IntPtr h) { TerminateProcess(h, 1); }
    public static long StartFileTime(IntPtr h) { long c, e, k, u; return GetProcessTimes(h, out c, out e, out k, out u) ? c : 0; }
    public static void CloseProc(IntPtr h) { CloseHandle(h); }
    public static void Kill(IntPtr j) { TerminateJobObject(j, 1); }
    public static void Close(IntPtr j) { CloseHandle(j); }
}
"@
}
$fetchJob = [PvJob]::Create()
$outTmp = [IO.Path]::GetTempFileName(); $errTmp = [IO.Path]::GetTempFileName()
$fetchOk = $false; $fetchMsg = ""; $fetchH = [IntPtr]::Zero
try {
    $fetchCmd = "git -C `"$repoRoot`" fetch --no-tags $remote +refs/heads/${branchName}:refs/remotes/$remote/$branchName"
    # env hook: check_push_verify widens the create->assign window. Honored only with PUSH_VERIFY_TEST_MODE=1, capped at 5000 ms, parsed safely.
    $assignDelay = 0
    if ($env:PUSH_VERIFY_TEST_MODE -eq '1' -and $env:PUSH_VERIFY_TEST_ASSIGN_DELAY_MS -match '^\d{1,5}$') { $assignDelay = [math]::Min(5000, [int]$env:PUSH_VERIFY_TEST_ASSIGN_DELAY_MS) }
    $gitExe = (Get-Command git -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
    if ($assignDelay -gt 0) { Write-Host "push_verify: TEST HOOK assign delay ${assignDelay} ms" }
    $jobAssigned = $false
    $useJob = ($fetchJob -ne [IntPtr]::Zero -and $env:PUSH_VERIFY_TEST_NO_JOB -ne '1')  # env hook: check_push_verify forces the no-job fallback
    # Created suspended, assigned to the job, then resumed (M1). Assign result is checked: if the job cannot hold the process, a timeout falls back to the snapshot walk.
    $fetchPid = 0
    $fetchH = [PvJob]::Launch($gitExe, $fetchCmd, $outTmp, $errTmp, $(if ($useJob) { $fetchJob } else { [IntPtr]::Zero }), $assignDelay, [ref]$jobAssigned, [ref]$fetchPid)
    # test seam: let git exit before the first read (MED-1 regression case)
    if ($env:PUSH_VERIFY_TEST_MODE -eq '1' -and $env:PUSH_VERIFY_TEST_POST_LAUNCH_SLEEP_MS -match '^\d{1,4}$') { Start-Sleep -Milliseconds ([int]$env:PUSH_VERIFY_TEST_POST_LAUNCH_SLEEP_MS) }
    if (-not [PvJob]::WaitExit($fetchH, $FetchTimeoutSec * 1000)) {
        if ($jobAssigned) { [PvJob]::Kill($fetchJob) }
        else {
            # no job membership: walk ALL descendants (git -> git-remote-http -> helpers) from one
            # Win32_Process snapshot. A child is kept only if it was created at/after ITS OWN parent
            # (a reused PID's older children fail that test at every depth; the root gets ~1 ms slack for
            # clock rounding). Before any kill, open a Process handle per target (an open handle blocks PID
            # reuse) and skip any whose StartTime differs from the snapshot's CreationDate; kill through the
            # handle, leaves first, then git itself.
            Write-Host "push_verify: no job membership; killing the fetch process tree by snapshot (handle-checked)."
            try {
                $snap = @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue)
                $rootCreated = [DateTime]::FromFileTime([PvJob]::StartFileTime($fetchH))
                $rootSnap = $snap | Where-Object { [int]$_.ProcessId -eq $fetchPid } | Select-Object -First 1
                if ($rootSnap -and $rootSnap.CreationDate) { $rootCreated = $rootSnap.CreationDate }
                $walk = Get-PvDescendantOrder -Snap $snap -RootId ([int]$fetchPid) -RootCreated $rootCreated
                $order = New-Object System.Collections.ArrayList; $keep = @{}
                foreach ($w in $walk) { [void]$order.Add($w.Id); $keep[$w.Id] = $w.Created }
                $handles = Open-PvTargets -Ids ([int[]]@($order)) -Created $keep
                # discovered parent-first (breadth-first); kill in reverse so leaves go first
                Write-Host "push_verify: snapshot walk found $($order.Count) descendant(s) of the fetch process."  # I2: 0 means the walk failed or was empty, so only the root dies
                Stop-PvTargets -Order ([int[]]@($order)) -Handles $handles   # lib: kills only via the handles opened above, never by bare PID
            } catch { }
        }
        [PvJob]::KillProc($fetchH)   # via the held native handle, never by PID
        $null = [PvJob]::WaitExit($fetchH, 5000)
        $fetchMsg = "timed out after ${FetchTimeoutSec}s"
    } elseif ([PvJob]::ExitCode($fetchH) -ne 0) {
        $fetchMsg = "failed (exit $([PvJob]::ExitCode($fetchH)))"
    } else { $fetchOk = $true }
} finally {
    if ($fetchH -ne [IntPtr]::Zero) { [PvJob]::CloseProc($fetchH) }
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
