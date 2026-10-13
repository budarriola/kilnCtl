# Review: toolfx9 (58be5c718..bddcc7269) -- 2026-10-10

Review of origin/dev commits `58be5c718`, `fcef5a93b`, `33a78968c`, `22625937a`, `f767abd94`
and `bddcc7269`. They fix R1-R4, I2 and I3 from `REVIEW_TOOLFX8_2026-10-10.md`. Reviewed at dev
tip `bddcc7269`. No code changed.

## Verdict

No HIGH or MED findings. push_verify cannot kill a process outside its own tree in any
scenario I could build. The worst case left is a narrow PID-reuse window (S1). Every clock or
time-zone mismatch makes the fallback skip a kill and leak a process. None makes it kill the
wrong process. The LANDED / NOT LANDED verdict paths are unchanged. R1, R3, R4 and I3 are
fixed and tested. R2 is fixed in code, but push_verify's use of the handle-checked kill is not
tested (S2). The survivor check flakes under load (S3). I2 adds no coverage a mutation can
measure, and it does not need a mutation of its own (I1 below).

| ID | Grade | Area | Summary |
|----|-------|------|---------|
| S1 | LOW | lib_push_verify_walk.ps1 | The `Test-PvStartMatch` tolerance (100 ms) is about 10^5 times the measured skew (under 1 us). That leaves a small PID-reuse window |
| S2 | LOW | push_verify.ps1 / check | Going back to a bare-PID `Stop-Process` in push_verify's kill loop is not caught (N5 MISSED). Only the lib functions are tested |
| S3 | LOW | check_push_verify.ps1 | The survivor assertions fail under concurrent load even on unmutated paths, so negtest can report a false CAUGHT |
| I1 | INFO | check_push_verify.ps1 (I2) | The wider survivor match cannot be told apart by mutation. It needs no mutation of its own |
| I2 | INFO | push_verify.ps1 | If the lib is missing, the fallback becomes a silent no-op. The verdict is unaffected |
| I3 | INFO | lib / check | Residual fail-open cases are inherent: a dead intermediate node, or a WMI failure. The root-slack test bounds the slack only between 0.5 ms and 500 ms |

## Analysis of the questions asked

**Killing outside its own tree.** The walk compares only snapshot `CreationDate` values with
each other: a child against its own parent. The root's PID is pinned by `$fetchProc.Handle`,
which is opened right after `Start-Process`. Any process whose `ParentProcessId` is the root
PID and that was created after the root is therefore a real child. A deeper parent is alive in
the snapshot, so a child with that `ParentProcessId` created at or after that parent cannot
belong to an earlier owner of the PID, because that owner exited before the parent was
created. The root time is taken from the snapshot when the root is in it. Otherwise it falls
back to `StartTime`, and the 1 ms slack covers CIM's truncation to microseconds. Only
`Open-PvTargets` compares two time sources, the CIM `CreationDate` and `Process.StartTime`.
Both are Local-kind times, and a DateTime comparison uses ticks only. I measured 235
accessible processes on this machine and the largest difference was 0.0009 ms. A DST or
time-zone mismatch would shift the CIM time by a whole hour, so the match fails and the target
is skipped (a leak). Mutation N8 shifted the root time by 1 h: the process leaked, the check
caught it, and nothing else was killed. A process the script cannot access throws on
`.Handle` or `StartTime` and is skipped.

**Failing open.** The walk drops no real child as long as the chain is alive in the snapshot.
The integration test checks this end to end (N7, N8 and N10 were CAUGHT). The remaining
fail-open cases are inherent (I3).

**The landing gate.** The diff touches only the timeout branch. Everything new runs inside
`try { } catch { }`, and that branch always ends with `VERDICT: UNKNOWN` and exit 1. All the
LANDED, NOT LANDED, UNKNOWN and usage cases in `check_push_verify.ps1` pass.

**Dot-sourcing and paths.** `$PSScriptRoot` is the directory of the script file being run, so
the lib always comes from the same checkout as `push_verify.ps1`, whether that is a worktree
or the main tree. `land.ps1` runs `Join-Path $PSScriptRoot "push_verify.ps1"`, so the pair
stays together. The verdict is taken against the repo of the current directory
(`git rev-parse --show-toplevel`), not the script's tree. That behavior is unchanged. See I2
for a lone copy of the script.

## Findings

### S1 (LOW) Start-time tolerance is far wider than needed

`Test-PvStartMatch` accepts a difference of up to 100 ms. The measured skew between the two
time sources is under 1 us.

Scenario: the snapshot is taken at T. Tree node X was created at C, between T - 100 ms and T.
X exits after T, before `Open-PvTargets` opens it. Its PID is reused by an unrelated process
Y, which started at S with S - C <= 100 ms. Y passes the match and is killed. This needs a
node spawned in the last 100 ms of a fetch that has already hung for `FetchTimeoutSec`, plus
quick PID reuse. That is rare, and the fallback itself is rare (toolfx8 I1).

Negtest N2 (tolerance 1000 ms) was MISSED when run serially. The check tests only 30 ms
(match) and 5 min (no match).

Fix: set the tolerance to about 2 ms. Add a check case asserting that a 5 ms difference does
not match.

### S2 (LOW) push_verify's handle-checked kill is not tested end to end

The lib functions are unit-tested, but nothing ties push_verify to them. Mutation N5 put back
`Stop-Process -Id $order[$i] -Force` in push_verify's kill loop, which is the exact R2 defect,
and it was MISSED. The integration test only checks that the tree dies, and a bare-PID kill
also kills it.

Scenario: a later refactor "simplifies" the loop back to `Stop-Process`. R2 returns silently
and every test stays green.

Fix: move the kill loop into the lib as `Stop-PvTargets $order $handles` and unit-test it.
Start a sacrificial process (for example `ping -n 30 127.0.0.1`), pass a `$Created` entry 30
minutes off, and assert the process survives. Make push_verify call `Stop-PvTargets`. A
cheaper option is a static assertion in the check that the fallback block in push_verify has
no `Stop-Process`.

### S3 (LOW) Survivor assertions flake under concurrent load

In the parallel negtest run (`-Parallel 2`), mutation N2 was reported CAUGHT, because both
"no git process ... survives" assertions failed. One of them was the job-path case, which the
mutation does not touch. Run serially, the same mutation was MISSED. A fixed 2500 ms settle
is not enough when the machine is loaded. `run_all_checks.ps1` phase 2 runs checks at
`-MaxParallel 8`, so this can give false FAILs there. In negtest, it can hide a real MISSED
behind a false CAUGHT.

Fix: poll the survivor query every 250 ms for up to about 10 s, and pass as soon as it is
empty. On failure, print each survivor's PID, name and command line, so a flake can be told
apart from a real leak.

## INFO

- **I1, I2 needs no mutation of its own.** N6 (push_verify skips `$order[0]`, the inner
  `git.exe`) and N9 (N6 with the survivor match narrowed back to port-only) were both MISSED.
  Once `git-remote-http` dies, the inner git sees EOF and exits by itself, so this fixture
  cannot show whether the script killed it. The wider match is harmless and does no harm to
  keep. Giving it teeth would need a fixture in which the inner git outlives its helper. That
  is not worth building, so recording it is enough. N10 (kill only the inner git, leaving
  `git-remote-http`) was CAUGHT by the port match.
- **I2, missing lib.** I ran a copy of `push_verify.ps1` with no lib beside it. It printed a
  `CommandNotFoundException`, then gave `VERDICT: LANDED`, exit 0, for a real ancestor. The
  verdict is safe. If a fetch then timed out without a job, the fallback would kill only the
  root, because the walk call would throw inside its `try`. In a git checkout the two files
  always travel together, so this is acceptable.
- **I3, residual fail-open cases.** If an intermediate node exits before the snapshot, its
  children cannot be seen. If `Get-CimInstance` fails, only the root is killed. Both are
  inherent to a parent-PID walk and both predate this change. The root-slack cases (kept at
  -0.5 ms, dropped at -500 ms) accept any slack between those two bounds.
- R4's optional suggestion was not done: `bench_test_start` and `bench_test_run` still keep
  two separate lists of argument names. With the tests now looping over all ten names, a
  dropped name is caught (P1, P2).

## Tests run (worktree `C:\wt\rvtoolfx9_l4u2hf`, dev `bddcc7269`)

| Test | Result |
|------|--------|
| `tools/check_push_verify.ps1` | all cases passed |
| `tools/check_negtest.ps1` | passed, 246 assertions, 11 groups |
| `uv run pytest tests/test_bench_test_start_jobs.py tests/test_mcp_server_aux.py` | 59 passed |
| Lone `push_verify.ps1` copy with no lib, `-Commit HEAD` | LANDED, exit 0, load error printed |
| CIM `CreationDate` vs `Process.StartTime`, 235 processes | max 0.0009 ms |

## Negative tests (tools\negtest.ps1)

| Mutation | Target | Verdict |
|----------|--------|---------|
| N1 filter anchored to the root, not the parent | check_push_verify | CAUGHT (R1 synthetic case) |
| N2 tolerance 100 -> 1000 ms | check_push_verify | MISSED serially (S1). The parallel run's CAUGHT was the S3 flake |
| N3 tolerance 100 -> 600000 ms | check_push_verify | CAUGHT |
| N4 Open-PvTargets skips the start match | check_push_verify | CAUGHT |
| N5 push_verify kills by bare PID | check_push_verify | MISSED (S2) |
| N6 push_verify skips the inner git | check_push_verify | MISSED (I1) |
| N7 push_verify opens no handles | check_push_verify | CAUGHT |
| N8 root time skewed by 1 h | check_push_verify | CAUGHT (leak, not a wrong kill) |
| N9 N6 plus a port-only survivor match | check_push_verify | MISSED (I1) |
| N10 kill only the inner git | check_push_verify | CAUGHT |
| P1 drop `update_downgrade_repo` from the gate | test_bench_test_start_jobs | CAUGHT |
| P2 drop `ota_image_build` from the gate | test_bench_test_start_jobs | CAUGHT |
| P3 dry run refused | test_bench_test_start_jobs | CAUGHT |
| P4 truthy `confirm` | test_bench_test_start_jobs | CAUGHT |
