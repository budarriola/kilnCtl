# negtest.ps1 -- negative-test runner that NEVER edits the real source tree.
#
# WHY: negative tests used to be done by hand-editing real source, running the
# check, and restoring by hand. That left mutations behind (2026-10-07: two
# worktrees still held a `<` -> `<=` in update_fetch_heap.c and a 100 -> 200 in
# test_update_stage.c), let a poisoned prebuilt .exe survive a perfect
# hand-restore (ed854ac5 / ba230bca, see CLAUDE.md), and tripped the permission
# classifier on guard-removal edits. This script does the whole thing in a
# throwaway git worktree instead, and proves afterwards that the real tree is
# byte-for-byte where it was.
#
# WHAT IT DOES, per run
#   1. Validates every mutation BEFORE anything else happens: a -Find string
#      must match the base file exactly once (0 or 2+ matches is an error, exit
#      2, no copy is made and no command runs). A find == replace no-op is an
#      error too. If -Find has LF line ends and the file is CRLF, the LF->CRLF
#      form is tried (and must still match exactly once).
#   2. Snapshots the real tree: `git status --porcelain`, HEAD, and SHA-256 of
#      every file a mutation names, in the caller's tree AND the shared main
#      tree (if different).
#   3. Makes a throwaway copy: `git worktree add --detach` at HEAD (or -Rev)
#      into <CopyRoot>\negtest_<rand> (default C:\wt). With -IncludeDirty the
#      caller's uncommitted tracked diff (`git diff HEAD --binary`) and its
#      untracked, non-ignored files are snapshotted first and applied in the
#      copy, then committed THERE (local detached commit) so a reset between
#      runs returns exactly to the base.
#   4. Baseline (default on; -NoBaseline to skip): runs the command unmutated
#      in the copy and requires it to PASS (exit 0, and -ExpectPattern absent).
#      A failing baseline is an error (exit 2): a failure you cannot attribute
#      to the mutation proves nothing.
#   5. For each mutation: `git reset --hard <base>` + `git clean -fdx` (so no
#      build output of a previous run survives), apply the mutation, run the
#      command with a FRESH empty {OUT} dir, and judge it:
#        no -ExpectPattern : CAUGHT iff the command exits nonzero.
#        -ExpectPattern    : CAUGHT iff the pattern appears in the output
#                            (exit code ignored, so a harness that prints a
#                            FAIL verdict but exits 0 still counts). A nonzero
#                            exit WITHOUT the pattern is MISSED with a note
#                            ("failed for another reason", e.g. a compile
#                            error) -- that is not proof the check works.
#        timeout           : TIMEOUT (error, exit 2), process tree killed.
#   6. Always (finally block, which also runs on Ctrl-C): kill any child still
#      running, unlink every junction in the copy, `git worktree remove
#      --force`, Remove-TreeSafe (tools\lib_safe_remove.ps1), `git worktree
#      prune`. A copy left behind by a HARD kill is swept on the next run: each
#      copy has a sibling <copy>.owner.json naming its owner pid + start time,
#      and copies whose owner is gone are removed at startup.
#   7. Re-snapshots the real tree and FAILS LOUDLY (exit 2) if the caller's
#      tree status/HEAD changed or any mutated file's hash changed in either
#      tree. (A status change in the shared main tree alone, when it is not the
#      caller's tree, is only a warning: other sessions edit it concurrently.)
#
# DEFAULT VERDICT PATTERNS (used only when no -ExpectPattern is given; never pass a bare "FAIL" for the host
# presets: passing test titles contain it and the baseline then errors out):
#   kilnfw-host   'RUN FAILURES \(' (the script's own failure summary header)
#   saftyfw-host  'SAFTYFW HOST TESTS: FAILED' (the script's own verdict line; BUILD FAILED is a compile error, never CAUGHT)
#   pytest        '(?m)^FAILED \S+' (a failed assertion in the -rf summary; a collection/import crash prints ERROR
#                 and is therefore MISSED with a note, not CAUGHT)
# -RequireAssertion: for -Command runs (e.g. a node/JS test script) only; combining it with -ExpectPattern or a preset is refused.
# CAUGHT requires a case-sensitive assertion-failure line (AssertionError / ERR_ASSERTION / assertion failed /
# "FAIL:", or the KilnFW host-test form "  FAIL file:line: msg"), not merely a nonzero exit -- a mutation that makes the script crash (syntax error, ReferenceError)
# exits nonzero but proves nothing about the assertions.
#
# COMMAND: exactly one of -Preset or -Command. -Command is PowerShell text run
# with the copy as the current directory; `{OUT}` is replaced by a fresh empty
# per-run output dir and `{ROOT}` by the copy root. Presets:
#   kilnfw-host   firmware\KilnFW\App\test\build_host_tests.ps1 -OutDir {OUT}
#   saftyfw-host  firmware\SaftyFW\test\build_host_tests.ps1 -OutDir {OUT}
#                 (needs a short path: keep -CopyRoot at C:\wt)
#   check         powershell -File <-PresetArg, a repo-relative check_*.ps1>
#                 (only meaningful for checks that read the working tree; a
#                 check that builds origin/main itself cannot see a mutation)
#   pytest        <PcTools venv python> -m pytest <-PresetArg, default tests>
#                 from the copy's tools\PcTools, PYTHONPATH = the copy's src
#                 (so the copy's code is imported, not the editable install),
#                 no cache provider, --basetemp under {OUT}.
# Every run gets KILNCTL_NEGTEST=1, KILNCTL_CHECKCACHE=0 (never reuse a cached
# PASS) and PYTHONDONTWRITEBYTECODE=1.
#
# BUILD GATE: negtest itself never takes a tools\build_gate.ps1 slot. The
# commands it runs (build_host_tests.ps1, check_00_*.ps1, ...) take a slot only
# around their own compiles, which is exactly the owner rule; wrapping them in
# a slot here would hold one across their tests and lock waits. -Parallel N
# (clamped to 1..4, the heavy slot count) runs N copies at once; their compiles
# still queue on the gate.
#
# QUOTING (-Find/-Replace text with double quotes, $, backticks, backslashes):
#   ROOT CAUSE: Windows PowerShell 5.1 re-joins native-command arguments into one
#   command line WITHOUT escaping embedded double quotes, so
#   `& powershell.exe -File negtest.ps1 -Find 'printf("x")'` delivers printf(x) --
#   the quotes are gone before this script runs. This script cannot recover them.
#   * From Bash, single-quote the text: -Find 'printf("x")' passes through intact.
#   * From PowerShell, use -FindBase64/-ReplaceBase64 (UTF-8 base64; takes
#     precedence over -Find/-Replace; byte-exact for any text):
#       $b = { [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($args[0])) }
#       powershell -File tools\negtest.ps1 ... -File x.c -FindBase64 (& $b 'puts("a");') -ReplaceBase64 (& $b 'puts("b");')
#     or in Bash: -FindBase64 "$(printf '%s' 'puts("a");' | base64 -w0)"
#   * Or use -Mutations <json>, which never touches the command line.
#
# MUTATIONS: one of
#   -File <repo path> -Find <exact text> -Replace <text> [-Name <label>]
#   -Diff <unified diff file>  [-Name <label>]   (applied with `git apply`)
#   -Mutations <json file>: an array (or {"mutations":[...]}) of objects:
#       {"name":"...", "file":"...", "find":"...", "replace":"...", "expect":"regex"}
#       {"name":"...", "diff":"path\\to.diff"}
#       {"name":"...", "edits":[{"file","find","replace"}, ...]}
#     "expect" overrides -ExpectPattern for that mutation. Each mutation is
#     independent: sequentially in one copy with a full reset between them
#     (default), or spread over -Parallel N copies.
#
# OUTPUT: per mutation a CAUGHT / MISSED / TIMEOUT line with the matched
# failure lines; full logs under -LogDir (default %TEMP%\negtest_logs\<id>,
# kept). The LAST stdout line is one JSON object (land.ps1 / wait_for.ps1
# style):
#   {"verdict":"ALL_CAUGHT|MISSED|ERROR","baseline":{...},"mutations":[...],
#    "real_tree_unchanged":bool,"copies_removed":bool,"log_dir":...,"error":...}
# EXIT: 0 all caught, 1 any MISSED, 2 error (bad mutation, baseline failed,
# timeout, real tree changed, copy not removed, usage).
#
# EXAMPLES
#   powershell -ExecutionPolicy Bypass -File tools\negtest.ps1 -Preset kilnfw-host `
#     -File firmware\KilnFW\App\drivers\update\update_fetch_heap.c `
#     -Find "free_internal < FETCH_HEAP_PRECHECK_MIN" -Replace "free_internal <= FETCH_HEAP_PRECHECK_MIN" `
#     -ExpectPattern "FAIL .*test_update_fetch_heap"
#   powershell -ExecutionPolicy Bypass -File tools\negtest.ps1 -Preset check `
#     -PresetArg tools\check_uri_handler_cap.ps1 -Mutations my_mutations.json
#
# Tested by tools\check_negtest.ps1 (scratch git repo, never this repo).

[CmdletBinding()]
param(
    [string]$Command,
    [ValidateSet('', 'kilnfw-host', 'saftyfw-host', 'check', 'pytest')][string]$Preset = '',
    [string]$PresetArg,
    [string]$File,
    [string]$Find,
    [string]$Replace,
    [string]$FindBase64,
    [string]$ReplaceBase64,
    [string]$Diff,
    [string]$Name,
    [string]$Mutations,
    [string]$ExpectPattern,
    [switch]$RequireAssertion,
    [switch]$NoBaseline,
    [switch]$IncludeDirty,
    [string]$Rev = 'HEAD',
    [int]$Parallel = 1,
    [double]$TimeoutMin = 60,
    [string]$RepoRoot,
    [string]$CopyRoot = 'C:\wt',
    [switch]$Submodules,
    [string]$LogDir,
    # internal: run one chunk of mutations in its own copy (used by -Parallel)
    [string]$WorkerSpec
)

# "Continue", not "Stop": PS 5.1 turns native stderr into errors. Every native
# call is checked through $LASTEXITCODE.
$ErrorActionPreference = 'Continue'
. (Join-Path $PSScriptRoot 'lib_safe_remove.ps1')

$script:FailRegex = '(?i)(\bFAIL|\bFAILED\b|\berror\b|assert|exception|MISMATCH)'

function Write-Line([string]$m, [string]$color) {
    if ($color) { Write-Host $m -ForegroundColor $color } else { Write-Host $m }
}

function Invoke-Git {
    # Returns output lines as strings; sets $script:gitExit.
    $out = & git @args 2>&1 | ForEach-Object { "$_" }
    $script:gitExit = $LASTEXITCODE
    return , @($out)
}

function New-RandId([int]$n = 6) {
    $chars = 'abcdefghijklmnopqrstuvwxyz0123456789'.ToCharArray()
    return -join (1..$n | ForEach-Object { $chars[(Get-Random -Maximum $chars.Length)] })
}

function Get-MyStartTicks {
    try { return (Get-Process -Id $PID).StartTime.ToUniversalTime().Ticks } catch { return 0 }
}

function Test-OwnerAlive($owner) {
    try {
        $p = Get-Process -Id ([int]$owner.pid) -ErrorAction Stop
        return ($p.StartTime.ToUniversalTime().Ticks -eq [long]$owner.start_ticks)
    } catch { return $false }
}

# ---------------------------------------------------------------- text edits

function Get-OrdinalCount([string]$text, [string]$needle) {
    if ([string]::IsNullOrEmpty($needle)) { return 0 }
    $n = 0; $i = 0
    while (($i = $text.IndexOf($needle, $i, [StringComparison]::Ordinal)) -ge 0) { $n++; $i += $needle.Length }
    return $n
}

function Read-TextFile([byte[]]$bytes) {
    $bom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
    $off = if ($bom) { 3 } else { 0 }
    $text = (New-Object System.Text.UTF8Encoding($false)).GetString($bytes, $off, $bytes.Length - $off)
    return @{ Text = $text; Bom = $bom }
}

function Resolve-Edit([string]$text, [string]$find, [string]$repl) {
    # Returns @{ Count; Find; Replace } -- the effective strings to use.
    $n = Get-OrdinalCount $text $find
    if ($n -eq 0 -and $find.Contains("`n") -and -not $find.Contains("`r`n") -and $text.Contains("`r`n")) {
        $f2 = $find.Replace("`n", "`r`n"); $r2 = $repl.Replace("`r`n", "`n").Replace("`n", "`r`n")
        $n2 = Get-OrdinalCount $text $f2
        if ($n2 -gt 0) { return @{ Count = $n2; Find = $f2; Replace = $r2 } }
    }
    return @{ Count = $n; Find = $find; Replace = $repl }
}

function Set-EditInFile([string]$path, $edit) {
    $bytes = [IO.File]::ReadAllBytes($path)
    $t = Read-TextFile $bytes
    $r = Resolve-Edit $t.Text $edit.find $edit.replace
    if ($r.Count -ne 1) { throw "mutation find string matches $($r.Count) time(s) in $($edit.file) inside the copy (must be exactly 1)" }
    $i = $t.Text.IndexOf($r.Find, [StringComparison]::Ordinal)
    $new = $t.Text.Substring(0, $i) + $r.Replace + $t.Text.Substring($i + $r.Find.Length)
    $enc = New-Object System.Text.UTF8Encoding($t.Bom)
    $pre = $enc.GetPreamble()
    $body = $enc.GetBytes($new)
    $all = New-Object byte[] ($pre.Length + $body.Length)
    [Array]::Copy($pre, 0, $all, 0, $pre.Length); [Array]::Copy($body, 0, $all, $pre.Length, $body.Length)
    [IO.File]::WriteAllBytes($path, $all)
}

# ---------------------------------------------------------------- copies

function Remove-Copy([string]$repo, [string]$copy) {
    $ok = $false
    $maxTry = 4
    if ($script:lastTimedOut) { $maxTry = 12 }   # ~30 s+ budget after a TIMEOUT kill
    for ($try = 1; $try -le $maxTry -and -not $ok; $try++) {
        try { Remove-ReparsePointsUnder -Path $copy | Out-Null } catch { Write-Line "negtest: unlink pass: $($_.Exception.Message)" Yellow }
        if (Test-Path -LiteralPath $copy) { Invoke-Git -C $repo worktree remove --force --force $copy | Out-Null }
        if (Test-Path -LiteralPath $copy) { try { Remove-TreeSafe -Path $copy } catch { } }
        $ok = -not (Test-Path -LiteralPath $copy)
        if (-not $ok) { Start-Sleep -Seconds ([math]::Min($try, 4)) }
    }
    Invoke-Git -C $repo worktree prune | Out-Null
    if ($ok) { Remove-Item -LiteralPath "$copy.owner.json" -Force -ErrorAction SilentlyContinue }
    return $ok
}

function Remove-StaleCopies([string]$root) {
    if (-not (Test-Path -LiteralPath $root)) { return }
    foreach ($f in @(Get-ChildItem -LiteralPath $root -Filter 'negtest_*.owner.json' -File -ErrorAction SilentlyContinue)) {
        $o = $null
        try { $o = Get-Content -LiteralPath $f.FullName -Raw | ConvertFrom-Json } catch { continue }
        if (-not $o -or -not $o.copy) { continue }
        if (Test-OwnerAlive $o) { continue }
        if ((Split-Path -Parent ([IO.Path]::GetFullPath($o.copy))) -ne [IO.Path]::GetFullPath($root).TrimEnd('\')) { continue }
        Write-Line "negtest: sweeping copy left by dead owner pid $($o.pid): $($o.copy)" Yellow
        if (Remove-Copy $o.repo $o.copy) { Remove-Item -LiteralPath $f.FullName -Force -ErrorAction SilentlyContinue }
        else { Write-Line "negtest: could not remove stale copy $($o.copy)" Red }
    }
}

function New-Copy($spec) {
    $script:lastTimedOut = $false   # 4d: per copy, not sticky for the rest of the run
    if (-not (Test-Path -LiteralPath $spec.copy_root)) { New-Item -ItemType Directory -Force -Path $spec.copy_root | Out-Null }
    $copy = $null
    for ($i = 0; $i -lt 20; $i++) {
        $c = Join-Path $spec.copy_root ("negtest_" + (New-RandId))
        if (-not (Test-Path -LiteralPath $c) -and -not (Test-Path -LiteralPath "$c.owner.json")) { $copy = $c; break }
    }
    if (-not $copy) { throw "could not pick a free copy name under $($spec.copy_root)" }
    $owner = [ordered]@{ pid = $PID; start_ticks = (Get-MyStartTicks); repo = $spec.repo; copy = $copy; created = (Get-Date).ToString('o') }
    Set-Content -LiteralPath "$copy.owner.json" -Value ($owner | ConvertTo-Json -Compress) -Encoding ASCII
    $script:liveCopy = $copy
    $o = Invoke-Git -C $spec.repo worktree add --detach $copy $spec.base_sha
    if ($script:gitExit -ne 0) { throw "git worktree add failed: $($o -join ' | ')" }
    if ($spec.submodules) {
        $o = Invoke-Git -C $copy submodule update --init --recursive
        if ($script:gitExit -ne 0) { throw "submodule init failed: $($o -join ' | ')" }
    }
    $applied = $false
    if ($spec.patch -and (Get-Item -LiteralPath $spec.patch).Length -gt 0) {
        $o = Invoke-Git -C $copy apply --binary --whitespace=nowarn $spec.patch
        if ($script:gitExit -ne 0) { throw "could not apply the caller's uncommitted diff in the copy: $($o -join ' | ')" }
        $applied = $true
    }
    if ($spec.untracked_dir -and (Test-Path -LiteralPath $spec.untracked_dir)) {
        $items = @(Get-ChildItem -LiteralPath $spec.untracked_dir -Force)
        foreach ($it in $items) { Copy-Item -LiteralPath $it.FullName -Destination $copy -Recurse -Force }
        if ($items.Count -gt 0) { $applied = $true }
    }
    $base = $spec.base_sha
    if ($applied) {
        Invoke-Git -C $copy add -A | Out-Null
        $o = Invoke-Git -C $copy -c user.name=negtest -c user.email=negtest@invalid -c commit.gpgsign=false commit -q --no-verify -m "negtest: caller's uncommitted changes"
        if ($script:gitExit -ne 0) { throw "could not commit the base in the copy: $($o -join ' | ')" }
        $base = ((Invoke-Git -C $copy rev-parse HEAD) | Select-Object -First 1).Trim()
    }
    return @{ Path = $copy; Base = $base }
}

function Reset-Copy([string]$copy, [string]$base) {
    $o = Invoke-Git -C $copy reset -q --hard $base
    if ($script:gitExit -ne 0) { throw "reset of the copy failed: $($o -join ' | ')" }
    $o = Invoke-Git -C $copy clean -fdxq
    if ($script:gitExit -ne 0) { throw "clean of the copy failed: $($o -join ' | ')" }
    $st = Invoke-Git -C $copy status --porcelain
    if (@($st | Where-Object { $_ }).Count -gt 0) { throw "copy not pristine after reset: $($st -join ' | ')" }
}

function Set-Mutation([string]$copy, $mut) {
    if ($mut.diff) {
        $o = Invoke-Git -C $copy apply --check --binary $mut.diff
        if ($script:gitExit -ne 0) { throw "diff for '$($mut.name)' does not apply: $($o -join ' | ')" }
        $o = Invoke-Git -C $copy apply --binary $mut.diff
        if ($script:gitExit -ne 0) { throw "git apply failed for '$($mut.name)': $($o -join ' | ')" }
    }
    foreach ($e in @($mut.edits)) {
        if ($null -eq $e) { continue }
        Set-EditInFile (Join-Path $copy $e.file) $e
    }
    $st = Invoke-Git -C $copy status --porcelain
    if (@($st | Where-Object { $_ }).Count -eq 0) { throw "mutation '$($mut.name)' changed nothing in the copy" }
}

# ---------------------------------------------------------------- running


# Win32 Job Object: kill is atomic for every descendant (taskkill /T only kills a
# snapshot of the tree; a grandchild spawned after the snapshot escapes and keeps
# its cwd inside the copy, so the copy cannot be removed).
if (-not ('NegJob' -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class NegJob {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern IntPtr CreateJobObject(IntPtr a, string name);
    [DllImport("kernel32.dll")] static extern bool SetInformationJobObject(IntPtr h, int cls, IntPtr info, int len);
    [DllImport("kernel32.dll")] static extern bool AssignProcessToJobObject(IntPtr job, IntPtr proc);
    [DllImport("kernel32.dll")] static extern bool TerminateJobObject(IntPtr job, uint code);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll")] static extern bool IsProcessInJob(IntPtr proc, IntPtr job, out bool result);
    [DllImport("kernel32.dll")] static extern bool QueryInformationJobObject(IntPtr job, int cls, IntPtr info, int len, IntPtr retLen);
    // Live members of the job (JobObjectBasicProcessIdList = 3). Membership is a property of the
    // process object, so a reused PID or a stale parent PID can never add a stranger.
    public static int[] Members(IntPtr job) {
        int max = 4096;
        int size = 8 + max * IntPtr.Size;
        IntPtr buf = Marshal.AllocHGlobal(size);
        try {
            for (int i = 0; i < size; i += 8) Marshal.WriteInt64(buf, i, 0);
            if (!QueryInformationJobObject(job, 3, buf, size, IntPtr.Zero)) return new int[0];
            int n = Marshal.ReadInt32(buf, 4);
            int[] r = new int[n];
            for (int i = 0; i < n; i++) r[i] = (int)Marshal.ReadIntPtr(buf, 8 + i * IntPtr.Size).ToInt64();
            return r;
        } finally { Marshal.FreeHGlobal(buf); }
    }
    // True only when the OPEN process handle is still a member of the job (re-check before a kill,
    // so a member that exited and whose PID was reused is never killed).
    public static bool InJob(IntPtr proc, IntPtr job) {
        bool r;
        return IsProcessInJob(proc, job, out r) && r;
    }
    public static IntPtr Create() {
        IntPtr job = CreateJobObject(IntPtr.Zero, null);
        if (job == IntPtr.Zero) return IntPtr.Zero;
        // JOBOBJECT_EXTENDED_LIMIT_INFORMATION: LimitFlags at offset 16, total size 144 (x64) / 112 (x86)
        int size = IntPtr.Size == 8 ? 144 : 112;
        IntPtr buf = Marshal.AllocHGlobal(size);
        try {
            for (int i = 0; i < size; i++) Marshal.WriteByte(buf, i, 0);
            Marshal.WriteInt32(buf, 16, 0x2000); // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
            if (!SetInformationJobObject(job, 9, buf, size)) { CloseHandle(job); return IntPtr.Zero; }
        } finally { Marshal.FreeHGlobal(buf); }
        return job;
    }
    public static bool Assign(IntPtr job, IntPtr proc) { return AssignProcessToJobObject(job, proc); }
    public static bool Kill(IntPtr job) { return TerminateJobObject(job, 1); }
    public static void Close(IntPtr job) { CloseHandle(job); }
    // Clear KILL_ON_JOB_CLOSE so closing the handle after a NORMAL exit does not kill survivors
    // (shared per-user daemons such as mspdbsrv.exe that another session's cl build depends on).
    public static bool Disarm(IntPtr job) {
        int size = IntPtr.Size == 8 ? 144 : 112;
        IntPtr buf = Marshal.AllocHGlobal(size);
        try {
            for (int i = 0; i < size; i++) Marshal.WriteByte(buf, i, 0);
            return SetInformationJobObject(job, 9, buf, size);
        } finally { Marshal.FreeHGlobal(buf); }
    }
}
"@
}

$script:SpareNames = @('mspdbsrv.exe', 'vctip.exe', 'conhost.exe', 'ccache.exe')
# Kill every live member of the job except shared daemons, through a handle (Process.Kill), no taskkill /T.
function Stop-JobMembers($job) {
    if ($job -eq [IntPtr]::Zero) { return }
    foreach ($id in [NegJob]::Members($job)) {
        if ($id -eq $PID) { continue }
        try {
            $mp = [Diagnostics.Process]::GetProcessById($id)
            $null = $mp.Handle   # open once; membership and Kill both go through THIS handle
            if (-not [NegJob]::InJob($mp.Handle, $job)) { continue }
            if ($script:SpareNames -contains ("$($mp.ProcessName).exe").ToLowerInvariant()) { continue }
            $mp.Kill()
        } catch { }
    }
}
# Replacement for `taskkill /T`: job members when a job exists, else just the root (never a PPID walk).
function Stop-Tree($proc, $job) {
    if ($job -and $job -ne [IntPtr]::Zero) { [NegJob]::Kill($job) | Out-Null }
    try { if (-not $proc.HasExited) { $proc.Kill() } } catch { }
}
function Stop-CopyProcesses([string]$copy) {
    # Belt and braces after the job kill: anything whose command line names the copy.
    $deadline = (Get-Date).AddSeconds(15)
    do {
        $left = @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | Where-Object {
                $_.ProcessId -ne $PID -and $_.CommandLine -and $_.CommandLine.IndexOf($copy, [StringComparison]::OrdinalIgnoreCase) -ge 0 })
        foreach ($l in $left) { if ($script:SpareNames -contains "$($l.Name)".ToLowerInvariant()) { continue }; & taskkill.exe /F /PID $l.ProcessId 2>&1 | Out-Null }
        $left = @($left | Where-Object { $script:SpareNames -notcontains "$($_.Name)".ToLowerInvariant() })
        if ($left.Count -gt 0) { Start-Sleep -Milliseconds 500 }
    } while ($left.Count -gt 0 -and (Get-Date) -lt $deadline)
}

# Orphan tracking: host_build_worker.ps1 / kilnctl_host_tests_*.exe detach from the cmd tree and keep the
# copy's directories busy. While the command runs we remember every descendant (pid + creation time); after
# the run (any exit path) the ones still alive are killed. Shared per-user daemons are spared.
# A real child is never older than its parent. Win32_Process.ParentProcessId is not updated when the parent
# exits and PIDs get reused, so a stale parent PID can make an unrelated long-lived process look like a child.
function Test-ChildAdoptable($child, $parentCreated) {
    if ($null -eq $child.CreationDate -or $null -eq $parentCreated) { return $false }
    return ([datetime]$child.CreationDate -ge [datetime]$parentCreated)
}
function Add-Descendants([int]$rootId, $tracked, $rootCreated = $null) {
    $all = @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue)
    $kids = @{}
    foreach ($q in $all) { if (-not $kids.ContainsKey([int]$q.ParentProcessId)) { $kids[[int]$q.ParentProcessId] = @() }; $kids[[int]$q.ParentProcessId] += $q }
    $stack = New-Object System.Collections.Stack
    $byId = @{}
    foreach ($q in $all) { $byId[[int]$q.ProcessId] = $q }
    # The root may already have exited (final scan): walk its direct children using the creation time
    # the caller captured while holding the process handle (the PID cannot be reused meanwhile).
    if (-not $byId.ContainsKey($rootId) -and $null -eq $rootCreated) { return }
    $stack.Push($rootId)
    while ($stack.Count -gt 0) {
        $id = [int]$stack.Pop()
        $parentCreated = if ($byId.ContainsKey($id)) { $byId[$id].CreationDate } elseif ($id -eq $rootId) { $rootCreated } else { $null }
        foreach ($c in @($kids[$id])) {
            if (-not $c -or $c.ProcessId -eq $PID) { continue }
            if (-not (Test-ChildAdoptable $c $parentCreated)) { continue }
            if (-not $tracked.ContainsKey([int]$c.ProcessId)) { $tracked[[int]$c.ProcessId] = "$($c.CreationDate)|$($c.Name)" }
            $stack.Push([int]$c.ProcessId)
        }
    }
}
function Stop-Tracked($tracked) {
    foreach ($k in @($tracked.Keys)) {
        $cp, $nm = "$($tracked[$k])".Split('|', 2)
        if ($script:SpareNames -contains $nm.ToLowerInvariant()) { continue }
        $cur = Get-CimInstance Win32_Process -Filter "ProcessId = $k" -ErrorAction SilentlyContinue
        if ($cur -and "$($cur.CreationDate)" -eq $cp) { & taskkill.exe /F /PID $k 2>&1 | Out-Null }
    }
}

function Read-SharedText([string]$path) {
    # A killed child (timeout) can hold the log open for a moment; read with
    # full sharing and retry instead of letting an IOException abort the run.
    for ($i = 1; $i -le 10; $i++) {
        try {
            $fs = New-Object IO.FileStream($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
            try { $sr = New-Object IO.StreamReader($fs, [Text.Encoding]::Default, $true); return $sr.ReadToEnd() } finally { $fs.Dispose() }
        } catch { if ($i -eq 10) { return "NEGTEST: could not read log ${path}: $_" }; Start-Sleep -Milliseconds 500 }
    }
}

function Invoke-Command-InCopy($spec, [string]$copy, [string]$tag) {
    $out = Join-Path $copy ("_nt\" + $tag)
    if (Test-Path -LiteralPath $out) { throw "fresh out dir already exists: $out" }
    New-Item -ItemType Directory -Force -Path $out | Out-Null
    $cmd = $spec.command.Replace('{OUT}', $out).Replace('{ROOT}', $copy)
    $wrapper = Join-Path $spec.state_dir ("run_" + (New-RandId 8) + ".ps1")
    $log = Join-Path $spec.log_dir ("$tag.log")
    $q = $copy.Replace("'", "''")
    $body = @"
`$ErrorActionPreference = 'Continue'
`$env:KILNCTL_NEGTEST = '1'
`$env:KILNCTL_CHECKCACHE = '0'
`$env:PYTHONDONTWRITEBYTECODE = '1'
Set-Location -LiteralPath '$q'
`$global:LASTEXITCODE = 0
try {
$cmd
} catch { Write-Output ('NEGTEST: command threw: ' + `$_); exit 97 }
`$ok = `$?
if (`$LASTEXITCODE -ne 0) { exit `$LASTEXITCODE }
if (-not `$ok) { exit 1 }
exit 0
"@
    Set-Content -LiteralPath $wrapper -Value $body -Encoding UTF8
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process -FilePath "$env:SystemRoot\System32\cmd.exe" -NoNewWindow -PassThru -WorkingDirectory $copy `
        -ArgumentList "/d /s /c `"powershell -NoProfile -ExecutionPolicy Bypass -File `"$wrapper`" > `"$log`" 2>&1`""
    $null = $p.Handle   # PS 5.1: cache the handle or ExitCode reads back null
    # 4b (documented residual gap): the child is created by Start-Process and assigned to the job AFTER
    # it starts, so a grandchild spawned in that window escapes the job. Stop-CopyProcesses (command-line
    # match on the copy path) is the backstop; a suspended-create + resume needs CreateProcess P/Invoke.
    $assignFailed = $false
    $job = [NegJob]::Create()
    if ($job -eq [IntPtr]::Zero) { $assignFailed = $true }
    if ($job -ne [IntPtr]::Zero) { if (-not [NegJob]::Assign($job, $p.Handle)) { $assignFailed = $true; Write-Line "negtest: could not assign child to job object; job kill is a no-op, falling back to killing the tracked descendants + command-line matches" Yellow } }
    $script:liveChild = $p
    $script:liveJob = $job
    $tracked = @{}
    $script:liveTracked = $tracked
    $nextScan = 0
    $limitMs = [long]($spec.timeout_min * 60000)
    $timedOut = $false
    while (-not $p.WaitForExit(500)) {
        if ($sw.ElapsedMilliseconds -ge $nextScan) { try { Add-Descendants $p.Id $tracked $p.StartTime } catch { }; $nextScan = $sw.ElapsedMilliseconds + 1500 }
        if ($sw.ElapsedMilliseconds -gt $limitMs) { $timedOut = $true; if ($job -ne [IntPtr]::Zero) { [NegJob]::Kill($job) | Out-Null }; Stop-Tree $p $job; $p.WaitForExit(10000) | Out-Null; break }
    }
    try { Add-Descendants $p.Id $tracked $p.StartTime } catch { }
    if ($timedOut) { if ($assignFailed) { Stop-Tracked $tracked }; Stop-CopyProcesses $copy }
    # 4c: kill the job ONLY on timeout. After a normal exit, disarm kill-on-close and just close the
    # handle: killing the whole job would take down a shared mspdbsrv.exe (and ccache etc.) that other
    # sessions' builds use. Stragglers are handled by the targeted Stop-CopyProcesses below.
    if ($job -ne [IntPtr]::Zero) {
        if ($timedOut) { [NegJob]::Kill($job) | Out-Null } else { Stop-JobMembers $job; [NegJob]::Disarm($job) | Out-Null }
        $script:liveJob = $null   # before Close: a throw after Close must not leave a stale handle value for the finally
        [NegJob]::Close($job)
    }
    Stop-Tracked $tracked
    if (-not $timedOut) { Stop-CopyProcesses $copy }
    $script:liveChild = $null
    $script:liveJob = $null
    $script:liveTracked = $null
    if ($timedOut) { $script:lastTimedOut = $true }
    $exit = if ($timedOut) { -1 } else { $p.ExitCode }
    $text = ''
    if (Test-Path -LiteralPath $log) { $text = Read-SharedText $log }
    return @{ Exit = $exit; TimedOut = $timedOut; Text = $text; Log = $log; Seconds = [math]::Round($sw.Elapsed.TotalSeconds, 1) }
}

function Get-MatchLines([string]$text, [string]$pattern) {
    $lines = @($text -split "`r?`n" | Where-Object { $_ -match $pattern } | ForEach-Object { $_.Trim() })
    return , @($lines | Select-Object -First 12)
}

function Invoke-Chunk($spec) {
    # One copy: optional baseline, then each mutation with a full reset.
    $res = [ordered]@{ baseline = $null; mutations = @(); copy = $null; copy_removed = $true; error = $null }
    $copy = $null
    try {
        $c = New-Copy $spec
        $copy = $c.Path; $res.copy = $copy
        Write-Line "negtest: copy $copy at $($c.Base.Substring(0, 10))"
        # validate every mutation in the copy before any command runs
        foreach ($m in $spec.mutations) {
            if ($m.diff) {
                $o = Invoke-Git -C $copy apply --check --binary $m.diff
                if ($script:gitExit -ne 0) { throw "diff for '$($m.name)' does not apply: $($o -join ' | ')" }
            }
            foreach ($e in @($m.edits)) {
                if ($null -eq $e) { continue }
                $t = Read-TextFile ([IO.File]::ReadAllBytes((Join-Path $copy $e.file)))
                $n = (Resolve-Edit $t.Text $e.find $e.replace).Count
                if ($n -ne 1) { throw "mutation '$($m.name)': find matches $n time(s) in $($e.file) in the copy (must be exactly 1)" }
            }
        }
        if ($spec.baseline) {
            Write-Line "negtest: baseline (unmutated) ..."
            $r = Invoke-Command-InCopy $spec $copy "baseline"
            $pat = $spec.expect
            $hit = if ($pat) { Get-MatchLines $r.Text $pat } else { @() }
            $pass = (-not $r.TimedOut) -and $r.Exit -eq 0 -and $hit.Count -eq 0
            $res.baseline = [ordered]@{ passed = $pass; exit = $r.Exit; timed_out = $r.TimedOut; seconds = $r.Seconds; log = $r.Log
                lines = $(if ($hit.Count) { $hit } elseif (-not $pass) { Get-MatchLines $r.Text $script:FailRegex } else { @() }) }
            if (-not $pass) {
                # B6: still list every mutation of this chunk (never run), so the report is complete.
                foreach ($m in $spec.mutations) {
                    $res.mutations += [ordered]@{ index = $m.index; name = $m.name; verdict = 'BASELINE-FAILED'; exit = $null
                        note = "not run: baseline failed"; matched = @(); seconds = 0; log = $null }
                }
                $why = if ($r.TimedOut) { "timed out" } elseif ($r.Exit -ne 0) { "exit $($r.Exit)" } else { "expect pattern already present" }
                throw "BASELINE FAILED ($why): the unmutated command does not pass, so no failure can be attributed to a mutation. Log: $($r.Log)"
            }
            Write-Line "negtest: baseline PASS ($($r.Seconds)s)" Green
        }
        $k = 0
        foreach ($m in $spec.mutations) {
            $k++
            Reset-Copy $copy $c.Base
            Set-Mutation $copy $m
            Write-Line "negtest: mutation '$($m.name)' ..."
            $r = Invoke-Command-InCopy $spec $copy ("m{0:D2}_{1}" -f $m.index, (New-RandId 4))
            $pat = if ($m.expect) { $m.expect } else { $spec.expect }
            $verdict = 'MISSED'; $note = $null; $lines = @()
            if ($r.TimedOut) {
                $verdict = 'TIMEOUT'; $note = "no result within $($spec.timeout_min) min; process tree killed"
            } elseif ($pat) {
                $lines = Get-MatchLines $r.Text $pat
                if ($lines.Count -gt 0 -and (-not $RequireAssertion -or $r.Exit -ne 0)) { $verdict = 'CAUGHT' }
                elseif ($lines.Count -gt 0) { $note = "-RequireAssertion: assertion text matched but the run exited 0 (non-gating output)" }
                elseif ($r.Exit -ne 0) { $note = "exit $($r.Exit) but the expect pattern never appeared: failed for another reason (compile error?)"; $lines = Get-MatchLines $r.Text $script:FailRegex }
                else { $note = "exit 0 and no expect-pattern match" }
            } else {
                $lines = Get-MatchLines $r.Text $script:FailRegex
                if ($r.Exit -ne 0) { $verdict = 'CAUGHT' } else { $note = "command exited 0 with the mutation applied" }
            }
            $res.mutations += [ordered]@{ index = $m.index; name = $m.name; verdict = $verdict; exit = $r.Exit; note = $note
                matched = @($lines); seconds = $r.Seconds; log = $r.Log }
        }
    } catch {
        $res.error = "$($_.Exception.Message)"
    } finally {
        if ($script:liveChild) { try { Stop-Tree $script:liveChild $script:liveJob } catch { } }
        if ($script:liveTracked) { try { Stop-Tracked $script:liveTracked } catch { } }
        if ($script:liveCopy) {
            try { Stop-CopyProcesses $script:liveCopy } catch { }
            $res.copy_removed = Remove-Copy $spec.repo $script:liveCopy
            if (-not $res.copy_removed) { Write-Line "negtest: COPY NOT REMOVED: $($script:liveCopy)" Red }
            $script:liveCopy = $null
        }
    }
    return $res
}

# ---------------------------------------------------------------- worker mode

if ($WorkerSpec) {
    $spec = Get-Content -LiteralPath $WorkerSpec -Raw | ConvertFrom-Json
    $r = Invoke-Chunk $spec
    Write-Output ($r | ConvertTo-Json -Compress -Depth 8)
    exit 0
}

# ---------------------------------------------------------------- main

$script:result = [ordered]@{ verdict = 'ERROR'; baseline = $null; mutations = @(); real_tree_unchanged = $null
    copies_removed = $null; log_dir = $null; repo = $null; base_sha = $null; error = $null }

function Finish([int]$code, [string]$err) {
    if ($err) { $script:result.error = $err; Write-Line "NEGTEST ERROR: $err" Red }
    Write-Output ($script:result | ConvertTo-Json -Compress -Depth 8)
    exit $code
}

# ---- decode base64 find/replace (PS 5.1 strips embedded double quotes from native args)
foreach ($pair in @(@('FindBase64','Find'), @('ReplaceBase64','Replace'))) {
    $bv = (Get-Variable -Name $pair[0] -ValueOnly)
    if ($PSBoundParameters.ContainsKey($pair[0])) {
        try { Set-Variable -Name $pair[1] -Value ([Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($bv))) }
        catch { Finish 2 "-$($pair[0]) is not valid base64" }
        $PSBoundParameters[$pair[1]] = (Get-Variable -Name $pair[1] -ValueOnly)
    }
}

# ---- usage
if ([bool]$Preset -eq [bool]$Command) { Finish 2 "give exactly one of -Preset or -Command" }
$single = [bool]($File -or $PSBoundParameters.ContainsKey('Find') -or $PSBoundParameters.ContainsKey('Replace'))
$nModes = @($single, [bool]$Diff, [bool]$Mutations) | Where-Object { $_ }
if (@($nModes).Count -ne 1) { Finish 2 "give exactly one mutation source: -File/-Find/-Replace, -Diff, or -Mutations" }
if ($single -and (-not $File -or -not $PSBoundParameters.ContainsKey('Find') -or -not $PSBoundParameters.ContainsKey('Replace'))) {
    Finish 2 "-File, -Find and -Replace go together (-Replace may be empty but must be given)"
}
if ($IncludeDirty -and $Rev -ne 'HEAD') { Finish 2 "-IncludeDirty only makes sense with -Rev HEAD" }
if ($Parallel -lt 1) { $Parallel = 1 }
if ($Parallel -gt 4) { Write-Line "negtest: -Parallel clamped to 4 (heavy build slot count)" Yellow; $Parallel = 4 }

$callerDir = (Get-Location).ProviderPath
if (-not $RepoRoot) {
    $t = Invoke-Git -C $callerDir rev-parse --show-toplevel
    if ($script:gitExit -ne 0) { $t = Invoke-Git -C $PSScriptRoot rev-parse --show-toplevel }
    if ($script:gitExit -ne 0) { Finish 2 "not inside a git repository; pass -RepoRoot" }
    $RepoRoot = ($t | Select-Object -First 1).Trim()
}
$RepoRoot = [IO.Path]::GetFullPath($RepoRoot).TrimEnd('\')
$script:result.repo = $RepoRoot
$baseSha = ((Invoke-Git -C $RepoRoot rev-parse --verify "$Rev^{commit}") | Select-Object -First 1)
if ($script:gitExit -ne 0) { Finish 2 "cannot resolve -Rev '$Rev' in $RepoRoot" }
$baseSha = $baseSha.Trim()
$script:result.base_sha = $baseSha
$mainTree = $null
$wl = Invoke-Git -C $RepoRoot worktree list --porcelain
$first = $wl | Where-Object { $_ -like 'worktree *' } | Select-Object -First 1
if ($first) { $mainTree = [IO.Path]::GetFullPath(($first.Substring(9)).Replace('/', '\')).TrimEnd('\') }

$CopyRoot = [IO.Path]::GetFullPath($CopyRoot).TrimEnd('\')
foreach ($t in @($RepoRoot, $mainTree) | Where-Object { $_ }) {
    if ($CopyRoot -eq $t -or $CopyRoot.StartsWith($t + '\', [StringComparison]::OrdinalIgnoreCase)) {
        Finish 2 "-CopyRoot $CopyRoot is inside the real tree $t"
    }
}

# ---- command
$cmdText = $Command
switch ($Preset) {
    'kilnfw-host' { $presetExpect = 'RUN FAILURES \('; $cmdText = 'powershell -NoProfile -ExecutionPolicy Bypass -File firmware\KilnFW\App\test\build_host_tests.ps1 -OutDir "{OUT}"' }
    'saftyfw-host' {
        $presetExpect = 'SAFTYFW HOST TESTS: FAILED'
        if ($CopyRoot.Length -gt 8) { Write-Line "negtest: WARNING SaftyFW host tests need a short path; -CopyRoot $CopyRoot may overflow the MSVC command line" Yellow }
        $cmdText = 'powershell -NoProfile -ExecutionPolicy Bypass -File firmware\SaftyFW\test\build_host_tests.ps1 -OutDir "{OUT}"'
    }
    'check' {
        if (-not $PresetArg) { Finish 2 "-Preset check needs -PresetArg <repo-relative check script>" }
        if ([IO.Path]::IsPathRooted($PresetArg)) { Finish 2 "-PresetArg for -Preset check must be repo-relative (it runs the COPY's script)" }
        if (-not (Test-Path -LiteralPath (Join-Path $RepoRoot $PresetArg))) {
            # Accept a bare name (check_x.ps1) the way run_all_checks discovers checks: any
            # check_*.ps1 in the tree, excluding build output, node_modules and dotted dirs.
            $bare = [IO.Path]::GetFileName($PresetArg)
            # TRACKED files only (git ls-files): untracked archived copies (logs/wt_archive_*) must not make a name ambiguous.
            $tracked = @(& git -C $RepoRoot ls-files -- "*$bare" 2>$null)
            $hits = @($tracked | Where-Object { ($_ -split '/')[-1] -eq $bare -and $_ -like '*check_*.ps1' -and $_ -notmatch '(^|/)(build|node_modules)/' -and $_ -notmatch '(^|/)\.[^/]+/' } |
                ForEach-Object { Get-Item -LiteralPath (Join-Path $RepoRoot $_) -ErrorAction SilentlyContinue } | Where-Object { $_ })
            if ($hits.Count -eq 0) { Finish 2 "no such check: $PresetArg" }
            if ($hits.Count -gt 1) { Finish 2 "ambiguous check name $PresetArg matches: $(($hits | ForEach-Object { $_.FullName.Substring($RepoRoot.Length).TrimStart('\') }) -join ', ')" }
            $PresetArg = $hits[0].FullName.Substring($RepoRoot.Length).TrimStart('\')
        }
        $cmdText = "powershell -NoProfile -ExecutionPolicy Bypass -File `"$PresetArg`""
    }
    'pytest' {
        $presetExpect = '(?m)^FAILED \S+'
        $py = $null
        foreach ($t in @($RepoRoot, $mainTree) | Where-Object { $_ }) {
            $c = Join-Path $t 'tools\PcTools\.venv\Scripts\python.exe'
            if (Test-Path -LiteralPath $c) { $py = $c; break }
        }
        if (-not $py) { Finish 2 "no tools\PcTools\.venv python found in $RepoRoot or the main tree" }
        $pa = if ($PresetArg) { $PresetArg } else { 'tests' }
        $cmdText = "`$env:PYTHONPATH = (Join-Path '{ROOT}' 'tools\PcTools\src'); Set-Location -LiteralPath (Join-Path '{ROOT}' 'tools\PcTools'); & '$py' -m pytest $pa -p no:cacheprovider -rfE --basetemp `"{OUT}\pt`""
    }
}
if ($RequireAssertion -and ($ExpectPattern -or $presetExpect)) { Finish 2 "-RequireAssertion cannot be combined with -ExpectPattern or a -Preset (it would be silently ignored); drop one" }
if (-not $ExpectPattern) {
    if ($presetExpect) { $ExpectPattern = $presetExpect }
    elseif ($RequireAssertion) { $ExpectPattern = '(?-i)(AssertionError|ERR_ASSERTION|assertion failed|\bFAIL:|(?m)^\s+FAIL \S+:\d+:)' }
}
if ($Command -and $Command -notmatch '\{OUT\}') {
    Write-Line "negtest: note: -Command has no {OUT}; every run still starts from a pristine copy (reset + clean -fdx), so no build output is reused" DarkGray
}

# ---- mutations
$muts = New-Object System.Collections.ArrayList
function Add-Mut($name, $edits, $diff, $expect) {
    [void]$script:muts.Add([ordered]@{ index = $script:muts.Count + 1; name = $name; edits = @($edits); diff = $diff; expect = $expect })
}
function Resolve-RepoRel([string]$p) {
    if ([IO.Path]::IsPathRooted($p)) {
        $full = [IO.Path]::GetFullPath($p)
        if (-not $full.StartsWith($RepoRoot + '\', [StringComparison]::OrdinalIgnoreCase)) { throw "file '$p' is outside the repo $RepoRoot" }
        $p = $full.Substring($RepoRoot.Length + 1)
    }
    $p = $p.Replace('/', '\')
    if ($p -match '(^|\\)\.\.(\\|$)') { throw "file '$p' escapes the repo" }
    return $p
}
function Resolve-DiffPath([string]$p) {
    $c = if ([IO.Path]::IsPathRooted($p)) { $p } elseif (Test-Path -LiteralPath (Join-Path $callerDir $p)) { Join-Path $callerDir $p } else { Join-Path $RepoRoot $p }
    if (-not (Test-Path -LiteralPath $c)) { throw "diff file not found: $p" }
    return [IO.Path]::GetFullPath($c)
}
try {
    if ($single) {
        $nm = if ($Name) { $Name } else { "${File}: '$Find' -> '$Replace'" }
        Add-Mut $nm @(@{ file = (Resolve-RepoRel $File); find = $Find; replace = $Replace }) $null $null
    } elseif ($Diff) {
        $nm = if ($Name) { $Name } else { [IO.Path]::GetFileName($Diff) }
        Add-Mut $nm @() (Resolve-DiffPath $Diff) $null
    } else {
        $mp = if ([IO.Path]::IsPathRooted($Mutations)) { $Mutations } else { Join-Path $callerDir $Mutations }
        if (-not (Test-Path -LiteralPath $mp)) { throw "mutations file not found: $Mutations" }
        $j = Get-Content -LiteralPath $mp -Raw | ConvertFrom-Json
        $list = if ($j.PSObject.Properties.Name -contains 'mutations') { @($j.mutations) } else { @($j) }
        if ($list.Count -eq 0) { throw "mutations file has no mutations" }
        foreach ($m in $list) {
            $edits = @()
            if ($m.file) { $edits += @{ file = (Resolve-RepoRel $m.file); find = "$($m.find)"; replace = "$($m.replace)" } }
            foreach ($e in @($m.edits)) { if ($e) { $edits += @{ file = (Resolve-RepoRel $e.file); find = "$($e.find)"; replace = "$($e.replace)" } } }
            $d = if ($m.diff) { Resolve-DiffPath $m.diff } else { $null }
            if ($edits.Count -eq 0 -and -not $d) { throw "mutation '$($m.name)' has neither file/find/replace, edits, nor diff" }
            $nm = if ($m.name) { "$($m.name)" } else { "mutation $($muts.Count + 1)" }
            Add-Mut $nm $edits $d $(if ($m.expect) { "$($m.expect)" } else { $null })
        }
    }
} catch { Finish 2 "$($_.Exception.Message)" }

# ---- validate find strings against the base content, before anything else happens
function Get-BaseBytes([string]$rel) {
    if ($IncludeDirty) {
        $p = Join-Path $RepoRoot $rel
        if (-not (Test-Path -LiteralPath $p)) { throw "mutation file not found: $rel" }
        return [IO.File]::ReadAllBytes($p)
    }
    $tmp = [IO.Path]::GetTempFileName()
    try {
        & cmd.exe /d /c "git -C `"$RepoRoot`" cat-file blob `"${baseSha}:$($rel.Replace('\','/'))`" > `"$tmp`" 2>nul"
        if ($LASTEXITCODE -ne 0) { throw "mutation file $rel does not exist at $($baseSha.Substring(0,10)) (uncommitted? use -IncludeDirty)" }
        return [IO.File]::ReadAllBytes($tmp)
    } finally { Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue }
}
$guardFiles = New-Object System.Collections.Generic.List[string]
try {
    foreach ($m in $muts) {
        foreach ($e in $m.edits) {
            if ($e.find -ceq $e.replace) { throw "mutation '$($m.name)': find and replace are identical (a no-op is not a mutation)" }
            if ([string]::IsNullOrEmpty($e.find)) { throw "mutation '$($m.name)': empty find string" }
            $t = Read-TextFile (Get-BaseBytes $e.file)
            $n = (Resolve-Edit $t.Text $e.find $e.replace).Count
            if ($n -ne 1) { throw "mutation '$($m.name)': find string matches $n time(s) in $($e.file) (must be exactly 1): '$($e.find)'" }
            if (-not $guardFiles.Contains($e.file)) { $guardFiles.Add($e.file) }
        }
        if ($m.diff) {
            foreach ($l in (Get-Content -LiteralPath $m.diff)) {
                if ($l -match '^\+\+\+ (?:b/)?(.+?)\s*$' -and $Matches[1] -ne '/dev/null') {
                    $f = $Matches[1].Replace('/', '\'); if (-not $guardFiles.Contains($f)) { $guardFiles.Add($f) }
                }
            }
        }
    }
} catch { Finish 2 "$($_.Exception.Message)" }

# ---- real-tree snapshot
function Get-TreeState([string]$root) {
    $st = (Invoke-Git -C $root status --porcelain) -join "`n"
    $head = ((Invoke-Git -C $root rev-parse HEAD) | Select-Object -First 1)
    $h = [ordered]@{}
    foreach ($f in $guardFiles) {
        $p = Join-Path $root $f
        $h[$f] = if (Test-Path -LiteralPath $p) { (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash } else { 'absent' }
    }
    return @{ Status = $st; Head = $head; Hashes = $h }
}
$guardRoots = @($RepoRoot)
if ($mainTree -and $mainTree -ne $RepoRoot -and (Test-Path -LiteralPath $mainTree)) { $guardRoots += $mainTree }
$before = @{}
foreach ($g in $guardRoots) { $before[$g] = Get-TreeState $g }

# ---- state / logs
$runId = (Get-Date -Format 'yyyyMMdd_HHmmss') + '_' + (New-RandId 4)
if (-not $LogDir) { $LogDir = Join-Path ([IO.Path]::GetTempPath()) "negtest_logs\$runId" }
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
$LogDir = [IO.Path]::GetFullPath($LogDir)
$script:result.log_dir = $LogDir
$stateDir = Join-Path ([IO.Path]::GetTempPath()) "negtest_state_$runId"
New-Item -ItemType Directory -Force -Path $stateDir | Out-Null

$exitCode = 2
$workers = @()
try {
    Remove-StaleCopies $CopyRoot

    $patch = $null; $untrackedDir = $null
    if ($IncludeDirty) {
        $patch = Join-Path $stateDir 'dirty.patch'
        & cmd.exe /d /c "git -C `"$RepoRoot`" diff HEAD --binary > `"$patch`""
        if ($LASTEXITCODE -ne 0) { throw "git diff HEAD failed in $RepoRoot" }
        $untrackedDir = Join-Path $stateDir 'untracked'
        New-Item -ItemType Directory -Force -Path $untrackedDir | Out-Null
        foreach ($u in (Invoke-Git -C $RepoRoot ls-files --others --exclude-standard)) {
            if (-not $u) { continue }
            $src = Join-Path $RepoRoot $u
            if (-not (Test-Path -LiteralPath $src -PathType Leaf)) { continue }
            $dst = Join-Path $untrackedDir $u
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dst) | Out-Null
            Copy-Item -LiteralPath $src -Destination $dst -Force
        }
        Write-Line ("negtest: including caller's uncommitted changes ({0} bytes of diff, {1} untracked file(s))" -f (Get-Item $patch).Length, @(Get-ChildItem -LiteralPath $untrackedDir -Recurse -File).Count)
    }

    $baseSpec = [ordered]@{ repo = $RepoRoot; copy_root = $CopyRoot; base_sha = $baseSha; patch = $patch; untracked_dir = $untrackedDir
        submodules = [bool]$Submodules; command = $cmdText; expect = $ExpectPattern; timeout_min = $TimeoutMin
        state_dir = $stateDir; log_dir = $LogDir; baseline = (-not $NoBaseline); mutations = @() }

    $chunks = @()
    $nw = [Math]::Min($Parallel, $muts.Count)
    if ($nw -le 1) {
        $s = [pscustomobject]$baseSpec; $s.mutations = @($muts | ForEach-Object { [pscustomobject]$_ })
        $chunks += , (Invoke-Chunk $s)
    } else {
        for ($w = 0; $w -lt $nw; $w++) {
            $s = [ordered]@{}; foreach ($k in $baseSpec.Keys) { $s[$k] = $baseSpec[$k] }
            $s.baseline = ($w -eq 0) -and (-not $NoBaseline)
            $s.mutations = @(for ($i = $w; $i -lt $muts.Count; $i += $nw) { $muts[$i] })
            $sf = Join-Path $stateDir "worker_$w.json"
            Set-Content -LiteralPath $sf -Value ($s | ConvertTo-Json -Depth 8) -Encoding UTF8
            $outF = Join-Path $LogDir "worker_$w.out"; $errF = Join-Path $LogDir "worker_$w.err"
            $p = Start-Process -FilePath powershell.exe -NoNewWindow -PassThru -RedirectStandardOutput $outF -RedirectStandardError $errF `
                -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-WorkerSpec', "`"$sf`"")
            $null = $p.Handle
            $workers += @{ P = $p; Out = $outF; Err = $errF }
            Write-Line "negtest: worker $w (pid $($p.Id)) has $($s.mutations.Count) mutation(s)$(if ($s.baseline) { ' + baseline' })"
        }
        foreach ($wk in $workers) { while (-not $wk.P.WaitForExit(500)) { } }
        foreach ($wk in $workers) {
            $last = @(Get-Content -LiteralPath $wk.Out -ErrorAction SilentlyContinue | Where-Object { $_ -match '^\{' }) | Select-Object -Last 1
            if (-not $last) {
                $chunks += , ([ordered]@{ baseline = $null; mutations = @(); copy_removed = $false; error = "worker pid $($wk.P.Id) produced no result (see $($wk.Out), $($wk.Err))" })
            } else {
                $chunks += , ($last | ConvertFrom-Json)
            }
        }
        $workers = @()
    }

    $errors = @(); $allRemoved = $true
    foreach ($c in $chunks) {
        if ($c.baseline) { $script:result.baseline = $c.baseline }
        foreach ($m in @($c.mutations)) { if ($m) { $script:result.mutations += $m } }
        if ($c.error) { $errors += $c.error }
        if (-not $c.copy_removed) { $allRemoved = $false }
    }
    $script:result.mutations = @($script:result.mutations | Sort-Object { [int]$_.index })
    $script:result.copies_removed = $allRemoved
    if (-not $allRemoved) { $errors += "a throwaway copy could not be removed (see above); it will be swept on the next run once its owner is gone" }

    # ---- report
    Write-Line ""
    # A failed baseline invalidates every per-mutation verdict (other -Parallel workers still ran theirs):
    # report SKIPPED/BASELINE-FAILED, never CAUGHT/MISSED.
    $baselineFailed = ($script:result.baseline -and (-not $script:result.baseline.passed))
    if ($baselineFailed) {
        foreach ($m in $script:result.mutations) { $m.verdict = 'BASELINE-FAILED' }
        Write-Line "SKIPPED: baseline failed; per-mutation results below are not attributable (BASELINE-FAILED)" Red
    }
    foreach ($m in $script:result.mutations) {
        $col = switch ($m.verdict) { 'CAUGHT' { 'Green' } 'MISSED' { 'Red' } default { 'Yellow' } }
        Write-Line ("{0,-7} {1}  (exit {2}, {3}s)" -f $m.verdict, $m.name, $m.exit, $m.seconds) $col
        if ($m.note) { Write-Line "        note: $($m.note)" $col }
        foreach ($l in @($m.matched)) { if ($l) { Write-Line "        | $l" } }
        Write-Line "        log: $($m.log)" DarkGray
    }
    if (@($script:result.mutations | Where-Object { $_.verdict -eq 'TIMEOUT' }).Count -gt 0) { $errors += "at least one mutation run timed out" }
    $expected = $muts.Count
    if ($errors.Count -eq 0 -and @($script:result.mutations).Count -ne $expected) { $errors += "only $(@($script:result.mutations).Count) of $expected mutation(s) produced a verdict" }

    if ($errors.Count -gt 0) {
        $script:result.error = ($errors -join ' ; ')
        $exitCode = 2
    } elseif (@($script:result.mutations | Where-Object { $_.verdict -eq 'MISSED' }).Count -gt 0) {
        $script:result.verdict = 'MISSED'; $exitCode = 1
    } else {
        $script:result.verdict = 'ALL_CAUGHT'; $exitCode = 0
    }
} catch {
    $script:result.error = "$($_.Exception.Message)"
    $exitCode = 2
} finally {
    foreach ($wk in $workers) { try { if (-not $wk.P.HasExited) { Stop-Tree $wk.P $null } } catch { } }
    if ($script:liveChild) { try { Stop-Tree $script:liveChild $script:liveJob } catch { } }
    if ($script:liveTracked) { try { Stop-Tracked $script:liveTracked } catch { } }
    if ($script:liveCopy) { Remove-Copy $RepoRoot $script:liveCopy | Out-Null; $script:liveCopy = $null }
    if ($workers.Count -gt 0) { Start-Sleep -Seconds 1; Remove-StaleCopies $CopyRoot }
    try { Remove-TreeSafe -Path $stateDir } catch { }
}

# ---- real-tree guard (always)
$guardOk = $true
foreach ($g in $guardRoots) {
    $a = Get-TreeState $g; $b = $before[$g]
    $strict = ($g -eq $RepoRoot)
    foreach ($f in $b.Hashes.Keys) {
        if ($a.Hashes[$f] -ne $b.Hashes[$f]) { $guardOk = $false; Write-Line "REAL TREE CHANGED: $g\$f content changed during the run" Red }
    }
    if ($a.Status -ne $b.Status) {
        if ($strict) { $guardOk = $false; Write-Line "REAL TREE CHANGED: git status --porcelain of $g differs from before the run (any file written in this tree during the run counts, tracked or not -- do not write notes into the worktree while negtest runs)" Red }
        else { Write-Line "negtest: warning: shared main tree $g status changed during the run (other sessions edit it; mutated files are unchanged)" Yellow }
    }
    if ($strict -and $a.Head -ne $b.Head) { $guardOk = $false; Write-Line "REAL TREE CHANGED: HEAD of $g moved during the run" Red }
}
$script:result.real_tree_unchanged = $guardOk
if (-not $guardOk) {
    $msg = "REAL TREE CHANGED during a negative-test run -- inspect it by hand; never restore with git checkout"
    $script:result.verdict = 'ERROR'
    $script:result.error = if ($script:result.error) { "$($script:result.error) ; $msg" } else { $msg }
    $exitCode = 2
}
if ($exitCode -eq 2) { $script:result.verdict = 'ERROR'; Write-Line "NEGTEST ERROR: $($script:result.error)" Red }
elseif ($exitCode -eq 1) { Write-Line "NEGTEST: at least one mutation was MISSED" Red }
else { Write-Line "NEGTEST: all $($muts.Count) mutation(s) CAUGHT; baseline $(if ($NoBaseline) { 'skipped' } else { 'passed' }); real tree unchanged" Green }
Write-Output ($script:result | ConvertTo-Json -Compress -Depth 8)
exit $exitCode
