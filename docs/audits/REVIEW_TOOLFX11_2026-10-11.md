# Review: toolfx11 (d1b5b728d) and landfx (295e50a9d) -- 2026-10-11

Read-only review of two origin/dev tooling commits, done at dev tip `295e50a9d`. No code changed.

- `d1b5b728d` (toolfx11) fixes M1, L1, L2 and I2 from `REVIEW_TOOLFX10_2026-10-11.md`, in
  `tools/push_verify.ps1` and `tools/check_push_verify.ps1`.
- `295e50a9d` (landfx) makes `tools/land.ps1` treat a "cannot lock ref" push rejection as a race
  and retry it. `tools/check_land.ps1` gains a pre-receive hook case for this.

## Verdict

**toolfx11 fixes M1.** git is now created suspended, added to the job and only then resumed, so
no descendant can start outside the job. The new 300 ms late-assign case is not vacuous: undoing
`CREATE_SUSPENDED` (`0x4` -> `0x0`) is CAUGHT (`FAIL: late job assign (300 ms): no git process
... survives the timeout`).

**L2 and I2 are fixed.** The I2 assertion is not vacuous, because `Get-PvDescendantOrder` excludes
the root, and a mutation that hard-codes the count to `0` is CAUGHT.

**L1 is fixed for PowerShell code only.** Two holes remain (LOW-3).

The new launcher adds one real regression (MED-1). `Launch` closes its own process handle before
PowerShell opens one. A child that exits in that gap then has a `$null` exit code. Today that
gives a spurious UNKNOWN, and in theory it reopens a PID-reuse kill. I demonstrated it outside the
repo. It never produces a false LANDED.

**landfx works for the race it tests.** Disabling the branch is CAUGHT. But the regex is checked
before the `[remote rejected]` hard refusal, and it matches anywhere in the push output. So a
genuine hook refusal, or a lock that never clears, that mentions one of the phrases is retried
`MaxPushTries` times and then reported without its message. I proved this with a mutation
(LOW-1).

Both checks pass unmodified in the worktree: `check_push_verify: all cases passed`,
`check_land: all cases passed`.

| ID | Grade | Area | Summary |
|----|-------|------|---------|
| MED-1 | MED | push_verify `Launch` | Process handle closed before `$fetchProc.Handle` is opened; a fast-exiting fetch reads `ExitCode=$null` -> spurious UNKNOWN; PID-reuse window reopened |
| LOW-1 | LOW | land.ps1 race regex | Unanchored match beats the `[remote rejected]` check: a genuine refusal (or persistent stale `.lock`) is retried 3x and its message dropped (proved by mutation) |
| LOW-2 | LOW | push_verify `Launch` | `ResumeThread` and `CreateFileW` results unchecked |
| LOW-3 | LOW | check_push_verify L1 | Static assertion misses a kill after `#` inside a string, and anything in the C# block (both MISSED under negtest) |
| LOW-4 | LOW | push_verify test hook | `PUSH_VERIFY_TEST_ASSIGN_DELAY_MS` is honored in production with no cap; the sleep runs before the fetch timeout starts |
| LOW-5 | LOW | check_push_verify | Leftover `Assert ($true) "placeholder"` (line 166): a vacuous ok line |
| INFO-1 | INFO | push_verify `Launch` | Handle hygiene and assign-failure path verified correct (no leak, no orphan) |
| INFO-2 | INFO | push_verify `Launch` | `bInheritHandles=TRUE` passes every inheritable handle to git |
| INFO-3 | INFO | push_verify `Launch` | `CreateProcessW(null, "git ...")` searches differently from `Start-Process` |
| INFO-4 | INFO | check_land | Test fakes the race with a hook; only the combined message is tested; alternative 2 likely never matches real git |
| INFO-5 | INFO | land.ps1 | Retry is bounded (`MaxPushTries`, default 3) |
| INFO-6 | INFO | 295e50a9d | Commit trailer is `Claude Sonnet 5.5`, not the `Claude Opus 5.5` that COMMON.md requires |

## MED-1: process handle closed before PowerShell opens one

`tools/push_verify.ps1:153-158`:

```
System.Diagnostics.Process p = System.Diagnostics.Process.GetProcessById(pi.pid); // handle still held: PID cannot be reused
ResumeThread(pi.ht);
return p;
...
finally { CloseHandle(pi.ht); CloseHandle(pi.hp); }
```

`Process.GetProcessById` opens no handle. The handle is opened later, on the first `.Handle`,
`WaitForExit` or `ExitCode` access. Here that is `$null = $fetchProc.Handle` (line 174), which
runs after the `finally` has closed `pi.hp`. So the comment "handle still held" is true only until
`Launch` returns. During the gap from `ResumeThread` to line 174, no handle is held on git.
The old `Start-Process` code had no such gap, because `Process.Start` keeps its creation handle.

Demonstration: I copied the C# class verbatim into a scratch script, as `PvJobDemo`. I launched
`cmd.exe /c exit 0` and slept 300 ms before the first access, to simulate an exit inside the gap:

```
delay=0   handle=2700  WaitForExit=True HasExited=True ExitCode=[0]  (ExitCode -ne 0) = False
delay=300 handle=      WaitForExit=True HasExited=True ExitCode=[]   (ExitCode -ne 0) = True
```

What follows if the child exits before line 174:

- `.Handle` silently yields `$null`, and `ExitCode` is `$null`. Line 202,
  `$fetchProc.ExitCode -ne 0`, is then true. A fetch that succeeded is reported as
  `failed (exit )`, giving `VERDICT: UNKNOWN`. This fails closed, but it is spurious, and the
  existing "failed fetch -> UNKNOWN" case accepts any failure text, so it would hide it.
- If the PID is freed and reused before line 174, the Process object binds to an unrelated
  process. `WaitForExit` then waits on that process, and the timeout path runs
  `$fetchProc.Kill()` against it. This is the bare-PID kill class that toolfx8-10 removed. It is
  unlikely, but the code comment claims it is impossible.

In practice a real network fetch rarely exits within the few milliseconds of the gap. A local
`file://` remote, or a fetch that fails at once, can.

Fix direction: open the handle while `pi.hp` is still held, for example `IntPtr _h = p.Handle;`
before `ResumeThread`. Or return a `SafeProcessHandle` / duplicated handle instead of a by-PID
Process. Add a case where the fetch exits at once with success, and assert `LANDED`, not merely
not-UNKNOWN.

## LOW-1: race regex can swallow a genuine refusal

`tools/land.ps1:378`:

```
$refRace = $pout -match 'cannot lock ref|failed to update ref[^\r\n]*expected|is at \S+ but expected'
```

This runs before the `[remote rejected]` hard-refusal check (line 383), and it matches anywhere in
the combined push output, including `remote:` lines that a hook prints. Two effects:

- A hook refusal whose text contains one of the phrases is classified as a race.
- A lock that never clears is also classified as a race. Example: a stale
  `refs/heads/dev.lock` on the server, where git says `cannot lock ref ... Unable to create
  '...dev.lock': File exists`.

Either case retries `MaxPushTries` times, and every attempt re-runs fetch, rebase and the
post-rebase checks. It then ends with `push still rejected after 3 attempts` (line 390), and
`$pout` is not included anywhere. The operator loses the reason.

Proof: one negtest mutation changed only the existing F11 hook text to
`no pushes today: signed commit expected; cannot lock ref until review`. It was CAUGHT:
`FAIL: remote rejection fails with its message`, and
`FAIL: no retry loop on remote rejection (rebases: 3)`.

This fails closed, so there is no wrong push. Fix direction:

- Match only git's own rejection shapes. For a remote rejection that is the
  `[remote rejected] <ref> (cannot lock ref ...)` reason. For receive-pack stderr it is the
  `error: cannot lock ref '<ref>': is at <sha> but expected <sha>` line.
- Exclude `File exists`.
- Include the last `$pout` in the final "still rejected" message.

## LOW-2: unchecked Win32 results in `Launch`

- `ResumeThread(pi.ht)` (line 154): its return is ignored. On failure (`0xFFFFFFFF`) git stays
  suspended until `FetchTimeoutSec` expires. It is then killed, and push_verify reports
  "timed out", which hides the real cause.
- `CreateFileW` (lines 142-144): the result is never compared with `INVALID_HANDLE_VALUE`. A
  failed open passes -1 as a std handle, so git's output is lost without notice. (The later
  `CloseHandle(-1)` is harmless: -1 is the current-process pseudo-handle.) The temp files come
  from `GetTempFileName`, so failure is unlikely.

## LOW-3: holes in the L1 static assertion

`tools/check_push_verify.ps1:81-86`. Two negtest mutations of push_verify.ps1 were MISSED:

1. `if ($false) { Write-Host "x #y"; Stop-Process -Id 1 }`. The comment stripper
   `(?m)(^|\s)#[^\r\n]*` deletes from any whitespace-preceded `#` to end of line, even inside a
   string. That deletes the kill too.
2. `...GetProcessById(1).Kill();` inside the C# block. Every here-string `@"..."@` is stripped
   whole. The C# block now contains `GetProcessById` and `TerminateProcess`, and the
   `GetProcessById` is the root of MED-1. So the most kill-capable code in the file is exactly the
   part the check exempts.

Also, `saps` is the alias for `Start-Process`, not a kill. Forbidding it is harmless but
over-broad. The positive and alias cases are real: adding `kill 1` is CAUGHT.

Fix direction: strip comments with the PowerShell tokenizer (`[System.Management.Automation.PSParser]::Tokenize`),
not a regex. Give the C# block its own allowlist: `TerminateProcess` only on `pi.hp`, and no
`.Kill(` or `GetProcessById` without a held handle.

## LOW-4: production-reachable test hook has no cap

`tools/push_verify.ps1:169` takes any all-digit `PUSH_VERIFY_TEST_ASSIGN_DELAY_MS` and passes it
to `Thread.Sleep`. The sleep happens inside `Launch`, before `WaitForExit($FetchTimeoutSec*1000)`
starts, so `-FetchTimeoutSec` does not bound it. A stray value in a user or CI environment
suspends git and stalls push_verify, and therefore land.ps1, for up to `Int32.MaxValue` ms. A
value beyond `Int32` throws an uncaught `[int]` cast error, and the run ends with no VERDICT
line. A name collision is unlikely because the name is specific. The hook only delays and never
weakens the kill path. Fix direction: clamp the value (e.g. <= 5000), or honor it only with a
second marker variable.

## LOW-5: placeholder assertion

`tools/check_push_verify.ps1:166`:
`# I2: ...` / `Assert ($true) "placeholder"    } finally { $lis.Stop() }`.
The real I2 assertion was folded into the no-job assertion at line 158. This line always prints
`ok: placeholder` and inflates the pass count. The missing newline before `} finally` is also an
edit artifact. The same artifact appears in push_verify.ps1 at line 159 (`}    public static void
Kill`). Remove both.

## INFO

- **INFO-1, handles and orphans: verified correct.**
  - `pi.hp` and `pi.ht` are closed in a `finally` on every path. The three std file handles are
    closed in an outer `finally`.
  - If anything throws between create and resume (assign, `GetProcessById`), the `catch` calls
    `TerminateProcess` on the still-suspended child. No orphan is left.
  - If `AssignProcessToJobObject` returns false, the child is resumed outside the job
    (`assigned=false`). A timeout then takes the documented snapshot-walk fallback, so nothing is
    left suspended.
  - If `CreateProcessW` fails, it throws. push_verify ends without a VERDICT, with a non-zero exit,
    and land.ps1 treats that as not LANDED.
  - The only object not disposed is the returned Process. It is reclaimed at script exit.
- **INFO-2, handle inheritance.** `bInheritHandles=TRUE` hands git every inheritable handle of
  the PowerShell process, not only the three std handles, which may include a caller's pipe. The
  old `Process.Start` path did the same, so this is not a regression. The job kill makes it
  moot on the normal path. On the no-job path, a survivor could keep a caller's pipe open.
  `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` would limit inheritance to the three handles.
  - Redirect setup is correct: `STARTF_USESTDHANDLES` (0x100); stdin is `NUL` with read access;
    stdout and stderr use `CREATE_ALWAYS` with write access and share 7, so they can be deleted
    during cleanup.
- **INFO-3, executable search.** With `lpApplicationName = null`, CreateProcess resolves `git`
  from the application directory, then the current directory, then the system directories, then
  PATH. It appends only `.exe`. `Start-Process` resolved the name through PATH. Two consequences:
  a `git.exe` in the .NET current directory would win, and a PATH holding only a `git.cmd` shim
  would fail. Neither occurs on this machine.
- **INFO-4, check_land coverage.**
  - The race is faked by a pre-receive hook that echoes git's wording. So the test proves the
    classifier and the retry, not that real receive-pack output matches.
  - The fake message contains all three alternatives at once. Dropping alternative 1
    (`cannot lock ref`) or alternative 3 (`is at ... but expected`) alone is MISSED, because the
    other one still matches.
  - Alternative 2 (`failed to update ref[^\r\n]*expected`) is probably dead. git's own reason line
    is `[remote rejected] <ref> (failed to update ref)`, which has no `expected` on it.
  - The hook is removed without try/finally. If `Run-Land` throws, later cases inherit the hook.
- **INFO-5, retry bound.** `MaxPushTries` (default 3) bounds the loop, and the race branch only
  `continue`s. Each attempt re-fetches, re-rebases and re-runs the narrow post-rebase checks,
  which is a natural back-off. The retry line does not print `$pout`, and neither does the final
  failure (see LOW-1).
- **INFO-6, attribution.** `295e50a9d` carries `Co-Authored-By: Claude Sonnet 5.5`. COMMON.md
  "Attribution" requires `Claude Opus 5.5` for every subagent commit. Do not rewrite it, because
  dev has been pushed.

## Evidence

All runs were in worktree `C:\wt\rvtfx11_hczcu7` at `295e50a9d`.

| Run | Result |
|-----|--------|
| `check_push_verify.ps1` (unmodified) | exit 0, `all cases passed` |
| `check_land.ps1` (unmodified) | exit 0, `all cases passed` |
| negtest check_push_verify: `CREATE_SUSPENDED` removed | CAUGHT (late job assign survivors) |
| negtest check_push_verify: `kill 1` added | CAUGHT (S2/L1) |
| negtest check_push_verify: `Write-Host "x #y"; Stop-Process -Id 1` | MISSED (LOW-3) |
| negtest check_push_verify: `GetProcessById(1).Kill()` in C# | MISSED (LOW-3) |
| negtest check_push_verify: walk count hard-coded 0 | CAUGHT (no-job fallback) |
| negtest check_land: race branch disabled | CAUGHT |
| negtest check_land: alternative 1 dropped | MISSED (INFO-4) |
| negtest check_land: alternative 3 dropped | MISSED (INFO-4) |
| negtest check_land: F11 hook text mentions `cannot lock ref` / `expected` | CAUGHT: land retried (rebases 3), message lost (LOW-1) |
| scratch demo of `Launch` with a 300 ms gap before first access | `ExitCode=[]`, `(ExitCode -ne 0) = True` (MED-1) |

negtest reported `real_tree_unchanged: true` and `copies_removed: true` for both runs.
