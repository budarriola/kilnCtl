# Review: toolfx10 (fa6d0265a) -- 2026-10-11

Review of origin/dev commit `fa6d0265a`. It fixes S1, S2 and S3 from
`REVIEW_TOOLFX9_2026-10-10.md` in `tools/lib_push_verify_walk.ps1`, `tools/push_verify.ps1` and
`tools/check_push_verify.ps1`. Reviewed at dev tip `0b2d24b19`. No code changed.

## Verdict

S1, S2 and S3 are fixed as asked. The 2 ms tolerance is safe: the skew is load-independent and
measures 0.0009 ms. `Stop-PvTargets` kills only through handles, and a handle cannot reach a
reused PID. The LANDED / NOT LANDED paths are untouched.

The fixer's unexplained survivors are a **real, pre-existing defect** (M1 below). It is not in
the walk and not in the tolerance. It is a race in the **job path**, the normal path: `git` is
started, and only then assigned to the job object. If the wrapper has already spawned its child
by then, the job kill takes only the wrapper. On this machine that leaves exactly three git
processes alive. I reproduced it every time with 10-30 ms of extra latency before the assign.

| ID | Grade | Area | Summary |
|----|-------|------|---------|
| M1 | MED | push_verify.ps1 (job path) | Start-then-assign race: under load, a timed-out fetch leaks inner `git`, `git remote-http` and `git-remote-http.exe`. This explains the three survivors |
| L1 | LOW | check_push_verify.ps1 | The static no-`Stop-Process` assertion is bypassable (`kill` alias, `GetProcessById(pid).Kill()`). Both MISSED |
| L2 | LOW | check_push_verify.ps1 | The S2 "not killed" assertion reads `HasExited` immediately. A bare-PID kill of sacB could still be in flight |
| I1 | INFO | lib | A tolerance between 2 and 5 ms cannot be told apart (2 -> 4 ms MISSED). That is fine |
| I2 | INFO | push_verify.ps1 | A WMI failure in the no-job path kills only the root. That leaves the same three survivors as M1 (toolfx9 I3), so the output cannot tell the two apart |

## Questions asked

**1. Is 2 ms too tight?** No. Both times come from the same kernel value, the process
CreateTime. WMI `CreationDate` truncates it to microseconds, and `Process.StartTime` reads it
through `GetProcessTimes`. Neither depends on scheduling, so load cannot widen the gap. I
measured every process I could open, plus 40 `ping` processes spawned during the measurement:

| Condition | Processes | Max skew | Over 2 ms |
|-----------|-----------|----------|-----------|
| Unloaded (machine already at 91% CPU from other sessions) | 394 | 0.0009 ms | 0 |
| Two busy loops per logical CPU, CPU 100% | 469 | 0.0009 ms | 0 |
| Same, second run | 456 | 0.0009 ms | 0 |

The margin is about 2000x. The only mismatch that can happen is a whole-hour DST or time-zone
shift. That makes the target be skipped (a leak), never killed wrongly. A legitimate tree child
cannot miss the match because of load.

**2. Stop-PvTargets.** It kills only `$Handles[$id].Kill()` for an id in the map, and in
push_verify that map comes only from `Open-PvTargets`. An id with no handle is skipped. In .NET
Framework (PowerShell 5.1), `$ph.Handle` caches a process handle. `StartTime` and `Kill()` both
reuse that cached handle and never reopen by PID. If the process has exited, `Kill()` throws
`InvalidOperationException` (caught) instead of terminating anything. While the handle is open,
the PID cannot be reused, so a kill through it can only reach the original process. Mutation M3
(kill by bare PID in the lib) was CAUGHT. M5 (no-op) was CAUGHT.

**3. The three survivors.** This is M1, a real defect. Detail is below. The tolerance cannot
cause it (point 1), and a slow kill would not last 10 s. A WMI failure in the no-job path (I2)
gives the same three survivors. That is the second candidate, but it is far less likely than a
race I can reproduce every time.

**4. Bypassing the S2 tests.** The sacrificial test calls the lib directly, so it guards
`Stop-PvTargets` itself. It does not guard whether push_verify calls it. The static assertion
only greps the whole file for the literal `Stop-Process` and requires `Stop-PvTargets` anywhere,
comments included. See L1.

**5. Verdict path.** It is untouched. The diff changes only the timeout branch. There, the old
loop became one `Stop-PvTargets` call inside the existing `try { } catch { }`, and that branch
still always ends in `VERDICT: UNKNOWN`, exit 1. All of the LANDED, NOT LANDED, UNKNOWN and
usage cases pass at baseline.

## Findings

### M1 (MED) Job-path start-then-assign race leaks the fetch tree

`push_verify.ps1` runs `Start-Process git ...`, then reads `$fetchProc.Handle`, and only then
calls `[PvJob]::Assign`. A process joins its parent's job only if it is created after the
parent was assigned. Here, `git` is `C:\Program Files\Git\cmd\git.exe`, a launcher. The tree
is:

```
cmd\git.exe -> git.exe fetch -> git.exe remote-http -> git-remote-http.exe
```

The launcher spawns its child within about 25-40 ms. If the assign comes later than that,
`TerminateJobObject` kills only the launcher, and `$fetchProc.Kill()` hits the same launcher.
The other three hang on the dead HTTP server until it closes the socket. Against a real hung
remote, they can hang much longer. A leaked `git fetch` can also hold ref locks, so the next
fetch can fail.

Measurements (an uncommitted probe script: a hung TcpListener, a 2 s wait, the job kill, then a
survivor query):

| Extra delay before Assign | Assign at (ms after start) | Runs that leaked 3 processes |
|---------------------------|----------------------------|------------------------------|
| 0 (natural), idle | 10-35 | 0/5 |
| 0 (natural), one busy loop per logical CPU | 8-50 | 0/12 |
| 10 ms | 26-51 | 3/4 |
| 20 ms | 37-68 | 2/4 |
| 30 ms | 55-82 | 3/3 |
| 100 ms / 500 ms | 110-990 | 5/5 |

The natural latency already reaches 50 ms under CPU load, which is right at the threshold. A
stall of tens of milliseconds while the machine is busy with memory or disk work (a heavy
serial negtest) is enough. Negtest mutation M6 (a 300 ms sleep before the assign) was CAUGHT
by both survivor assertions. So the check detects the leak, which is how the fixer saw it. It
is not a flaky check.

The race predates `fa6d0265a`. It came in with the job object.

Fix, in order of preference:

- Create the process suspended (`CreateProcess` with `CREATE_SUSPENDED` via P/Invoke), assign
  it to the job, then `ResumeThread`. This closes the race completely.
- Cheaper: on timeout, also run the snapshot walk on the job path. Take the snapshot and
  `Open-PvTargets` before `TerminateJobObject`, then `Stop-PvTargets`. `$fetchProc.Handle`
  keeps the root PID pinned, so the walk stays valid after the launcher dies.

Either way, add a test hook (for example `PUSH_VERIFY_TEST_ASSIGN_DELAY_MS`, like
`PUSH_VERIFY_TEST_NO_JOB`) so the check runs the late-assign case on every run and does not
depend on luck.

### L1 (LOW) The static no-Stop-Process assertion is bypassable

The assertion is `($pvSrc -notmatch 'Stop-Process') -and ($pvSrc -match 'Stop-PvTargets')`
over the whole file. Two mutations replaced the `Stop-PvTargets` call with a bare-PID kill and
kept the name in a trailing comment:

- `kill -Id $kid -Force`: MISSED. `kill` is an alias of `Stop-Process`.
- `[Diagnostics.Process]::GetProcessById($kid).Kill()`: MISSED.

Both reintroduce R2. The integration test cannot see the difference, because a bare-PID kill
also kills the tree.

Fix: extract the no-job fallback block and match it with comments stripped. Assert that it
contains a `Stop-PvTargets -Order ... -Handles $handles` call, and contains none of
`Stop-Process`, `kill `, `spps`, `taskkill`, `GetProcessById` or `.Kill(`. The single
`$fetchProc.Kill()` that follows is outside the block. Alternatively, require that the block
contains no kill primitive other than `Stop-PvTargets`.

### L2 (LOW) The S2 "not killed" assertion does not wait

`Assert (-not $sacB.HasExited)` runs right after `Stop-PvTargets` and after `sacA.WaitForExit`.
`TerminateProcess` returns before the target has exited. A bare-PID kill of sacB is issued
first (leaves-first order), and sacA's wait normally gives it time to finish, which is why M3
was CAUGHT. That is timing, not design. Fix: `Assert (-not $sacB.WaitForExit(1000)) ...`.

## INFO

- **I1.** Tolerances from 2 ms up to just under 5 ms pass every assertion (mutation M7, 2 -> 4,
  MISSED). This has no consequence, given the 0.0009 ms skew.
- **I2.** In the no-job path, if `Get-CimInstance` fails or returns nothing, the walk is empty
  and only the root dies. That gives the same three survivors as M1. The no-job path prints
  nothing to say the walk found zero descendants. A one-line note (`walk found N descendants`)
  would make the two causes distinguishable in a check log.

## Tests run (worktree `C:\wt\rvtfx10_hv9v91`, dev `0b2d24b19`)

| Test | Result |
|------|--------|
| `tools/check_push_verify.ps1` | all cases passed |
| Skew, CIM `CreationDate` vs `Process.StartTime`, idle and at 100% CPU (3 runs, 394-469 processes) | max 0.0009 ms, none over 2 ms |
| Job-assign race probe, natural and with 10/20/30/100/500 ms delay | leaks 3 processes once the assign comes after about 25-40 ms (table in M1) |

## Negative tests (`tools\negtest.ps1 -Preset check -PresetArg tools/check_push_verify.ps1`, serial, baseline PASS)

| Mutation | Verdict |
|----------|---------|
| M1 push_verify: `kill -Id` (alias) in place of `Stop-PvTargets` | MISSED (L1) |
| M2 push_verify: `GetProcessById(pid).Kill()` in place of `Stop-PvTargets` | MISSED (L1) |
| M3 lib: `Stop-PvTargets` kills by bare PID | CAUGHT (S2 sacB) |
| M4 lib: tolerance 2 -> 100 ms (S1 revert) | CAUGHT (S1 5 ms cases) |
| M5 lib: `Stop-PvTargets` no-op | CAUGHT (S2 sacA, no-job survivors) |
| M6 push_verify: 300 ms sleep before the job assign | CAUGHT (both survivor assertions; reproduces M1) |
| M7 lib: tolerance 2 -> 4 ms | MISSED (I1, expected) |
