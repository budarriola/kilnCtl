# Review: toolfx8 (de796cfa3) -- 2026-10-10

Adversarial review of origin/dev `de796cfa3`, which fixes T1-T4, I5 and I6 from
`REVIEW_TOOLFX7_2026-10-10.md`. Reviewed at dev tip `458533153`. No code changed.

## Verdict

No HIGH or MED findings. T1 and T4 do what they claim, and the new tests catch a real
regression of each fix. Four LOW items and three INFO items follow. Each LOW is either
rare or fails safe.

| ID | Grade | Area | Summary |
|----|-------|------|---------|
| R1 | LOW | push_verify.ps1 T1 | The creation-time filter is anchored to git's start time, not to each parent's start time, so a reused PID deeper in the tree can pull in an unrelated orphan |
| R2 | LOW | push_verify.ps1 T1 | PID-only kill after the snapshot, and killing leaves first makes parents exit on their own, which widens the PID-reuse window |
| R3 | LOW | check_push_verify.ps1 T1 | The no-job case cannot tell whether the hook took effect: if the hook is ignored, the job path runs and the case still passes |
| R4 | LOW | test_bench_test_start_jobs.py T4 | The synchronous gate's parameter tuple and its dry-run exemption are covered only in part |
| I1 | INFO | push_verify.ps1 | The fallback is close to unreachable in practice |
| I2 | INFO | push_verify.ps1 | Survivor check covers only git-remote-http |
| I3 | INFO | mcp_server_bench_test.py | Docstring now partly stale |

## Findings

### R1 (LOW) Creation-time filter is anchored to the root, not to each parent

`push_verify.ps1` keeps a child only if `CreationDate >= $gitStart.AddSeconds(-1)`. The
same bound applies at every depth. Windows never updates `ParentProcessId`, so a process
whose parent has exited still names its parent's PID. The PID-reuse guard has to compare
each child against its own parent's creation time.

Scenario: many sessions on this machine run short-lived git processes. One of them, U,
starts after the fetch's git wrapper W and gets PID 1234. U spawns a helper H and then
exits, and H keeps running as an orphan. W's inner `git.exe` later receives the freed
PID 1234. The fetch hangs, and the walk visits inner git (PID 1234). H has
`ParentProcessId == 1234` and was created after `$gitStart`, so it passes the filter.
H, another session's process, is killed. The `-1s` slack adds a second window at the
root. Both creation times come from the same kernel timestamp and CIM truncates only to
microseconds, so a full second is far more slack than needed.

Probability is low: it needs the fallback path (see I1), a PID reused inside a short
window, and an orphan. The cost is killing another session's git.

Fix: record each kept node's `CreationDate` and keep a child only if
`child.CreationDate >= parent.CreationDate`. Use `$fetchProc.StartTime` for the root and
shrink the slack to about 1 ms, or drop it.

### R2 (LOW) Kills by bare PID after the snapshot; killing leaves first lets parents exit early

The walk takes one `Win32_Process` snapshot and later calls `Stop-Process -Id <pid>` on
each PID in turn. Nothing pins a PID between the snapshot and the kill. Leaves go first,
so killing `git-remote-http` makes the inner `git.exe` see EOF and exit on its own just
before its own `Stop-Process`. Its PID is then free, and with many sessions spawning git,
a new unrelated process can take it in that gap. The killed-first order makes this gap
more likely, not less. A parent that is killed first cannot free its children's PIDs.

Fix: before killing anything, open a handle to each candidate
(`$p = Get-Process -Id $id; $null = $p.Handle`). An open handle stops the PID from being
reused. Then kill only if `$p.StartTime` equals the snapshot's `CreationDate`, and kill
through the handle (`$p.Kill()`). With handles pinned, the kill order no longer matters.

### R3 (LOW) The no-job case cannot prove the fallback ran

`PUSH_VERIFY_TEST_NO_JOB=1` skips `Assign`, so the case does exercise the fallback, and
negtests A and B below show it catches a one-level or missing walk. If the hook stops
working, though (mutation C, the hook condition removed), the job path kills the tree and
the case still passes. A later refactor that renames or drops the env check would leave
the fallback silently untested.

Fix: in the fallback branch, print one line such as
`fetch timeout: no job membership, killing N descendant(s) by walk`, and require it in the
no-job case (`$r.Out -match "no job membership"`). Require its absence in the job case.

On reachability: the hook is a plain env var read only at `Assign` time. Setting it only
switches to the fallback kill path and never skips a kill or changes a verdict, so a
leaked value would be harmless, apart from R1 and R2. No other script sets it, and the
check clears it in `finally`. Acceptable.

### R4 (LOW) T4 gate only partly covered by tests

The gate itself is correct. It names all ten `ota_*`/`update_*` image arguments, the same
set `bench_test_run` puts in `ctx` and gates on. `ota_allow_heat` is excluded in both, it
uses `confirm is not True`, and it exempts `dry_run`. A dry run with image arguments
still starts a job, and the job's `bench_test_run` skips the OTA gate. A confirmed run
still starts a job (`test_ota_image_arguments_reach_the_runner_ctx`). Negtests:

- D, `ota_corrupt_image_path` dropped from the tuple: MISSED. This fails safe, because
  the job's `bench_test_run` still refuses. The refusal just comes back as a FAILED job
  instead of synchronously, which is the T4 regression itself.
- E, `not dry_run` removed, so a dry run with image arguments is refused: MISSED. No
  test calls `bench_test_start(dry_run=True, ota_image_path=...)`.

The tuple duplicates the ctx loop's list, so a future eleventh image argument would fall
back to the job-level refusal unnoticed.

Fix: loop the new test over all ten keyword arguments, not three. Add one
`bench_test_start(suite="ota", dry_run=True, ota_image_path=...)` case that asserts
`STARTED`. Optionally, build both lists from one module-level tuple of names.

### INFO

- I1: the fallback runs only when `CreateJobObject`/`SetInformationJobObject` or
  `AssignProcessToJobObject` fails. Windows 8 and later support nested jobs, so this is
  rare on this machine. R1 and R2 are graded LOW partly for that reason.
- I2: the survivor check matches `127.0.0.1:<port>` in the command line, which only
  `git-remote-http` carries. The inner `git.exe fetch` and the `cmd\git.exe` wrapper are
  not checked. The final `$fetchProc.Kill()` covers the wrapper. A mutation that skipped
  only inner git would pass, but inner git exits on EOF once its helper dies, so this is
  low value. A process created after the snapshot (none in the hung-fetch case) would
  also be missed. A dead intermediate node breaks the chain, so orphaned grandchildren
  are invisible. That is inherent to a parent-PID walk.
- I3: `bench_test_start`'s docstring still says "a refusal (`error: refused -- ...`)
  comes back as the job's FAILED report". That is no longer true for the unconfirmed-OTA
  refusal. Add "except an unconfirmed ota_*/update_* run, refused synchronously".

T2, a sanity check: removing `assignFailed` is behavior-neutral. The post-loop
`Stop-Tracked $tracked` at `negtest.ps1:535` runs unconditionally, and the only grep that
was dropped is the vacuous one toolfx7 T2 named. T3 is a comment-only change and is now
accurate. I5 and I6 are test-layout changes, and the suites pass.

## Tests run (worktree `C:\wt\rvtoolfx8_7gwalc`, dev `458533153`)

| Test | Result |
|------|--------|
| `uv run pytest tests/test_bench_test_start_jobs.py tests/test_mcp_server_aux.py tests/test_pico_write_resume_and_armed_gate.py` | 73 passed |
| `tools/check_push_verify.ps1` | all cases passed, including both no-job assertions |
| `tools/check_negtest.ps1` | passed, 246 assertions, 11 groups |

## Negative tests (tools\negtest.ps1)

| Mutation | Target | Verdict |
|----------|--------|---------|
| A_one_level_walk (`$frontier = @()`) | check_push_verify | CAUGHT |
| B_no_walk_at_all (kill loop never runs) | check_push_verify | CAUGHT |
| C_hook_ignored (env hook removed) | check_push_verify | MISSED (R3) |
| D_drop_corrupt_from_tuple | test_bench_test_start_jobs | MISSED (R4, fails safe) |
| E_dry_run_refused | test_bench_test_start_jobs | MISSED (R4) |
| F_truthy_confirm (`not confirm`) | test_bench_test_start_jobs | CAUGHT |
| G_refuse_confirmed_too | test_bench_test_start_jobs | CAUGHT |
