# Review: toolfx7 (REVIEW_TOOLS3 M1, M2, L1-L7 fixes) -- 2026-10-10

Reviewed: `465379da4` (fix) and `ffbde85d6` (doc mark) on origin/dev, read at
`9c9600384`. Reviewer: Opus. No board access.

## Verdict

The M1/M2 fixes are sound and their tests are negative-tested. push_verify.ps1
cannot report LANDED falsely through the new code: the verdict logic is
untouched, `fetchOk` is set only on exit 0 within the timeout, and a timeout
still yields `VERDICT: UNKNOWN` with exit 1. It cannot hang on the new code
either (file redirects, bounded waits, `check_push_verify.ps1` now bounds each
child at 60 s; a mutation that sleeps 300 s in the timeout path was caught
as a failure, not a hang). No HIGH or MED findings. Five LOW, several INFO.

| ID | Sev | Area | Summary |
|----|-----|------|---------|
| T1 | LOW | push_verify.ps1 | Fallback kill (no job) misses grandchildren; confirmed by negtest |
| T2 | LOW | check_negtest.ps1 / negtest.ps1 | L2 check is vacuous and the L2 change is behaviorally a no-op |
| T3 | LOW | judgments.py / run_state.c | L1 read-back does not cover the run_state lock-failure case its comment claims |
| T4 | LOW | mcp_server_bench_test.py | `bench_test_start` refuses an unconfirmed OTA/update run only asynchronously |
| T5 | LOW | check_negtest.ps1 | Regex literals contain raw CR bytes |
| I1-I7 | INFO | various | see below |

## Findings

### T1 (LOW) push_verify fallback kill only reaches direct children

`tools/push_verify.ps1:139-146`. When `[PvJob]::Assign` fails (or the job
cannot be created), the timeout path kills processes whose
`ParentProcessId` is the git PID, then git. A hung HTTP fetch runs
`git.exe -> git-remote-http.exe` (and possibly a credential helper below
that), so a grandchild survives. Negtest mutation
`v1_assign_skipped_fallback_only` (force `$jobAssigned = $false`, fallback
intact) was **CAUGHT** by `check_push_verify.ps1`'s "no git process for the
hung fetch survives the timeout" assertion: the fallback does not do what the
comment says. Also no creation-time filter, so a reused PID's unrelated
children could be killed. Verdict is unaffected (still UNKNOWN); the cost is
a leaked process. The path is rare (nested jobs are allowed on Windows 8+).

Fix: walk descendants recursively from a single `Win32_Process` snapshot,
keeping only processes whose `CreationDate` is at or after git's
`StartTime`, kill leaves first, then git. Add a check case that forces the
no-job path (an env-var test hook) so the fallback is tested at all.

### T2 (LOW) L2 grep check is vacuous; the L2 change has no behavioral effect

`tools/check_negtest.ps1:330` asserts only that the word `assignFailed`
appears somewhere in negtest.ps1. Negtest mutation
`l2_assignfailed_flag_dropped` (remove `if ($assignFailed) { Stop-Tracked
$tracked }` from the timeout branch at `tools/negtest.ps1:528`) was
**MISSED**. The reason it does not matter at runtime: `negtest.ps1:537`
already calls `Stop-Tracked $tracked` unconditionally after `NegJob.Close`,
so the added timeout-branch call only moves the kill a few lines earlier.

Fix: either drop the L2 code and its grep (and say in the REVIEW_TOOLS3 doc
that line 537 already covered it), or replace the grep with a behavioral
test that fails if tracked descendants survive a timed-out run with job
assignment forced to fail.

### T3 (LOW) HP-05 read-back still passes on a run_state lock failure

`tools/PcTools/src/kilnctrl/bench_test/judgments.py:1813-1814` says the
`last_run.present` read-back distinguishes "acknowledged" from "a run_state
lock failure". It does not: `run_state_get_boot_record()`
(`firmware/KilnFW/App/drivers/*/run_state.c:481-488`) returns false when
`ensure_lock()` fails, so `/api/profile_exec` reports `present:false`, and
`card_clear` reads True. A lock failure therefore still PASSes HP-05.

Fix: correct the comment to say the read-back covers a stale or
unacknowledged record only, or (firmware) report a distinct
`last_run.error` when the lock cannot be taken and have the judge treat it
as INCONCLUSIVE.

### T4 (LOW) bench_test_start accepts an unconfirmed OTA/update run

`tools/PcTools/src/kilnctrl/mcp_server_bench_test.py:268-310`.
`bench_test_start` forwards `confirm` into the background job, so an
unconfirmed call with `ota_*`/`update_*` parameters returns a job id and
the refusal arrives only as the job's FAILED report. `ota_matrix_start`
answers unconfirmed calls synchronously. No board action happens (the gate
at `:140-144` runs inside the job before the runner), so this is UX/
consistency, not safety.

Fix: run the same `confirm is not True` test in `bench_test_start` before
creating the job and return the refusal string directly.

### T5 (LOW) Raw CR bytes inside check_negtest.ps1 regexes

`tools/check_negtest.ps1:304` and `:331-333` contain literal CR characters
inside single-quoted regex strings (they act as `\r?\n`). Any line-ending
normalisation or editor save that strips stray CRs changes the regex
silently; `Assert-True $mS.Success` at `:306` would then fail loudly, but
the `:331` liveJob assertion would just fail with a misleading message.

Fix: write `\r?\n` escapes instead of raw bytes.

### INFO

- I1 `tools/push_verify.ps1:142`: with KILL_ON_JOB_CLOSE set (`:120`), the
  explicit `[PvJob]::Kill` is redundant. Negtest `v4a_no_job_kill_killonclose_covers`
  MISSED as expected; `v4b` (no Kill and no kill-on-close) CAUGHT. Fine as
  defence in depth.
- I2 `tools/negtest.ps1` `Stop-JobMembers`: the `InJob` re-check only guards a
  PID-reuse race and cannot be caught by the check (`l3_injob_recheck_removed`
  MISSED, expected). The Kill itself (`l3_kill_removed`) is CAUGHT.
- I3 `tools/check_negtest.ps1:311-322`: if an assertion throws before cleanup,
  the three 120 s test processes leak until they exit. Wrap in try/finally.
- I4 `tools/PcTools/src/kilnctrl/debug_probe.py:888-893`: L5 is partial. The
  `KCTL_HALT_ERR` case now reports success with a warning; `ok=False` with
  clean markers and no halt error still reports failure.
- I5 `tools/PcTools/tests/test_pico_write_resume_and_armed_gate.py:112`: the
  new `WriteWrapperAndHaltErrTest` sits after the `if __name__ == "__main__"`
  block at `:108`, so it does not run when the file is executed directly
  (pytest collects it). Move it above.
- I6 `tools/PcTools/tests/test_mcp_server_aux.py:224,234`: two `with A, B:`
  statements collapsed onto one line with a long run of spaces (lost line
  continuation). Valid Python; reformat.
- I7 `mcp_server_bench_test.py:144`: the run-level preflight runs before
  `BenchTestRunner` takes the board lock (same TOCTOU as `ota_matrix_run`);
  per-case gates still re-check. The aux POST (`mcp_server_aux.py:290`) still
  honours proxy environment variables through urllib; with the pinned IP
  this is the only remaining way the POST can reach a different host.

## Negative tests run by this review

All through `tools\negtest.ps1` in throwaway worktrees; real tree unchanged
in every run.

pytest preset (test_bench_test_start_jobs.py, test_mcp_server_aux.py,
test_pico_write_resume_and_armed_gate.py, test_bench_test_judgments_heat.py):

| Mutation | Fix | Result |
|---|---|---|
| m2_wrapper_passes_leave_halted | M2 | CAUGHT |
| m1_start_forces_confirm | M1 | CAUGHT |
| m1_preflight_ignored | M1 | CAUGHT |
| l1_card_false_ignored | L1 | CAUGHT |
| l6_post_unpinned | L6 | CAUGHT |
| l5_herr_ignored | L5 | CAUGHT |

check preset, `tools\check_push_verify.ps1` (baseline PASS 70 s):

| Mutation | Expected | Result |
|---|---|---|
| v1_assign_skipped_fallback_only | MISSED (fallback works) | CAUGHT (T1) |
| v2_assign_skipped_no_fallback | CAUGHT | CAUGHT |
| v3_hang_in_timeout_path (sleep 300) | CAUGHT, no hang | CAUGHT in 130 s |
| v4a_no_job_kill_killonclose_covers | MISSED | MISSED (I1) |
| v4b_no_job_kill_no_killonclose | CAUGHT | CAUGHT |

check preset, `tools\check_negtest.ps1` (baseline PASS 271 s):

| Mutation | Fix | Result |
|---|---|---|
| l3_kill_removed | L3 | CAUGHT |
| l3_injob_recheck_removed | L3 | MISSED (I2, race-only) |
| l7_clear_after_close | L7 | CAUGHT |
| l2_assignfailed_flag_dropped | L2 | MISSED (T2) |
