# Review SAFTYFX6 (2026-10-12): SAFTYFX5 fixes

Scope: `b4fcac924`, `9ba778c99` and `78978db64` on origin/dev (tip `78978db64`). These
commits answer `REVIEW_SAFTYFX5_2026-10-12.md` LOW-3 and LOW-5, and add comment-only notes
for LOW-1, LOW-2 and LOW-4.

- `firmware/SaftyFW/src/safety_guards.c`: the gap comment in `s8_clear_refuse()`.
- `firmware/SaftyFW/test/test_safety_guards.c`: the new `test_test_clear_s8_partial_floor_and_divisor`.
- `docs/audits/REVIEW_SAFTYFX5_2026-10-12.md`: the fix-status table.

Reviewer worktree base: `78978db64`.

Name clash: `REVIEW_SAFTYFX6_2026-10-10.md` is a different, earlier review. The "saftyfx6 F2"
and "saftyfx6 F4" citations in `safety_guards.c` refer to that one, not to this one.

## Verdict

There are no HIGH or MED findings. The code change is comment-only. The new test pins the
5 s partial floor from both sides and pins the elapsed divisor. All seven mutations were
CAUGHT by a test assertion, and every mutant compiled.

There are two LOW findings:

- One comment overclaims when a clear is granted.
- `check_build_gate_reentrant` is flaky under load. That flake is the "NEW failure" the
  fixer landed with.

`land.ps1` did not let a NEW failure through by an override or a gap in its gate. The
fixer ran it with no `-CheckLog`, so the check-log gate never ran. For `-Target dev` that
is allowed by design.

## Checks requested

1. **Zero behavior change in safety_guards.c.** Confirmed. A script stripped comments and
   whitespace from the file at `b4fcac924^` and at `b4fcac924`. The resulting code streams
   are IDENTICAL, at 22340 characters each.
   - Every changed line in the diff is a ` *` comment line.
   - No later commit in the range touches `firmware/SaftyFW`.
2. **The new comment against the code.** Gaps (2), (3) and (4) are accurate. Gap (1)
   overclaims (LOW-1). Gap (2) omits one refusal path (INFO-1). Details:
   - (2): the partial check needs `s8_post_elapsed_s >= S8_POST_MIN_S`, so a rise of any
     rate is unseen by it for the first 5 s after each rollover. Accurate.
   - (3): the partial rate is `(tc - s8_post_start_c) / (elapsed/60)`, which includes the
     flat time before a mid-window rise. Accurate.
   - (4): the post window is clamped to 60..600 s at `safety_guards_tick()`. `rate_window_s`
     is an unvalidated U16: `config_params.c:496` stores it as is, and
     `safety_core.c:411` copies it. Accurate.
3. **The new test's numbers.** All correct (see the table below).
   - Setup: limit 10 C/min, `rate_window_s` 60, `dt_s` 0.5. The first post window
     completes flat, after 120 ticks. Then 9, 11 or 20 more ticks give an elapsed time of
     exactly 4.5, 5.5 or 10.0 s. The sums of 0.5 are exact in float.
   - The step is applied only to the clear input, so `s8_post_start_c` stays at 20 C. The
     completed-window rate is 0.
   - `s8_over_rate_streak` is 0, so the streak-1 refusal does not apply.
   - The whole-hold average stays under the limit: about 1.5 C over more than 1 min.
   - The k=0 case pins the floor from below: a 4 s floor refuses at 4.5 s. The k=1 case
     pins it from above. The k=2 case pins the divisor: the elapsed time gives 12 C/min,
     and the window length gives 2 C/min.

| Case | Elapsed | Rise | Partial rate | Expected |
|---|---|---|---|---|
| k=0 | 4.5 s | 1.5 C | skipped (< 5 s) | GRANT |
| k=1 | 5.5 s | 1.5 C | 16.4 C/min | REFUSE |
| k=2 | 10.0 s | 2.0 C | 12.0 C/min (2.0 if divided by W) | REFUSE |

4. **Negtest re-run.** All 7 mutations were CAUGHT.
   - Command: `tools\negtest.ps1 -Preset saftyfw-host -Mutations <scratch>\mut.json -Parallel 4`.
     The preset's default verdict pattern, `SAFTYFW HOST TESTS: FAILED`, never matches a
     build failure.
   - Run details: base `78978db64`, the baseline passed, `real_tree_unchanged: true` and
     `copies_removed: true`.
   - Every mutant log has zero `error:` or `BUILD FAILED` lines. Each one names a failing
     `TEST_CHECK` in `test_safety_guards.c`.

| Id | Mutation (`safety_guards.c`) | Verdict | Failing assertion(s) |
|---|---|---|---|
| F4 | non-strict partial floor `S8_POST_MIN_S` -> `10.0f` | CAUGHT | :5169 (k=1) |
| F5 | non-strict partial floor -> `30.0f` | CAUGHT | :5169 (k=1, k=2) |
| F4d | `#define S8_POST_MIN_S 10.0f` (all users) | CAUGHT | :5169, :4950, :3534 |
| F10 | partial divisor `s8_post_elapsed_s` -> `s8_post_win_s` | CAUGHT | :5169 (k=1, k=2) |
| R1 (own) | non-strict partial floor -> `4.0f` | CAUGHT | :5165 (k=0) |
| R2 (own) | partial limit `> 2.0f * max_rate` | CAUGHT | :5028, :5169 |
| R3 (own) | partial divisor `(elapsed + 5 s)` | CAUGHT | :5169 (k=1, k=2) |

The F4 and F5 used here mutate only the non-strict branch. Only the new test catches
them, which shows that the new test is what pins the TEST-clear floor. The SAFTYFX5 run's
":4950, :3534 also" matches F4d, the `#define` variant, which also changes the strict
branch and the frozen-window floor.

5. **How the NEW failure (`check_build_gate_reentrant`) landed.** See INFO-3 and LOW-2.

## Findings

### LOW-1: gap (1) says "the clear is granted only after the burst has ended", which (2) and (3) contradict

Gap (1) says the partial window "still refuses while it sees" the burst, and that the
clear is granted only after the burst ends. That holds only after the first 5 s of a
window, and only when the burst outruns the dilution. Gaps (2) and (3), in the same
comment, describe two ways a burst still in progress is granted:

- in the first `S8_POST_MIN_S` after a rollover;
- while the net rise is no more than max_rate x elapsed.

A reader of (1) alone gets a stronger guarantee than the code gives. This is a comment in
safety code, with no behavior impact.

Fix: in (1), write "granted once the burst has left the partial window, or earlier under
(2) and (3)".

### LOW-2: check_build_gate_reentrant case 5 is a fixed-sleep race, flaky under load

Case 5 starts a separate `powershell` child to hold the slot mutex. It then sleeps a fixed
`Start-Sleep -Seconds 2` and expects `Enter-KilnBuildGate` to time out
(`tools/check_build_gate_reentrant.ps1:124-130`).

Under load, a cold `powershell` start takes longer than 2 s. The parent then takes the
still-free slot and gets no timeout message. The log shows "acquired light slot 0 for
't_timeout'" and "FAIL: slot timeout message matches the BUSY pattern".

Reproduced on the dev tip: 3 of 4 standalone runs failed at about 100% CPU (the negtest
was running), and 1 passed. The SAFTYFX5 commits do not touch this script or
`build_gate.ps1`. So the fixer's "passes when re-run alone" is true only some of the time.

This is the "boot order beats bounded waits" class. Fix: the hold child writes a ready
file (or a named event) after `WaitOne` succeeds. The parent waits for that file, bounded,
before it calls `Enter-KilnBuildGate`.

### INFO-1: gap (2) says "only the whole-hold average sees it"

The `s8_over_rate_streak >= 1` refusal also sees the rise: any rise above the frozen
window start refuses, and that check is stronger. The whole-hold average applies only when
`s8_window_active`. Neither point weakens the clear. This is a precision note only.

### INFO-2: test wording, and the k=0 pin

- The test header says "15 C/min over 6 s", but the case clears at 5.5 s, which is
  16.4 C/min. The k=1 assertion text also says 15 C/min. The numbers in the code are right.
- The k=2 assertion text names the divisor, but a floor mutant at or above 10 s also
  fails it (F5).
- k=0 asserts that gap (2) exists, as a GRANT. If the owner later decides to close gap
  (2), this assertion must flip. That is intended: the assertion pins the floor from below.

### INFO-3: land.ps1 was run without -CheckLog; no override, no gate gap

The fixer's transcript has exactly one invocation:
`powershell -NoProfile -ExecutionPolicy Bypass -File tools\land.ps1 -Target dev`.

- No `-CheckLog`, `-WaitPid`, `-AllowFail` or `-AllowKnownFailures` was passed.
- In `land.ps1`, the check-log gate runs only `if ($WaitPid -gt 0 -or $CheckLog)` (line 215 at `78978db64`).
- With neither flag, the only checks land runs are the narrow post-rebase set
  (`check_mcp_tool_count_doc`, `check_mcp_facade_coverage`) and the submodule pin check.
  Both passed. So land never saw the `-Fast` log that listed
  `NEW tools\check_build_gate_reentrant.ps1`.

This is by design. The owner decision of 2026-10-09 says agents push to dev without
`run_all_checks`, and `-Target main` requires `-CheckLog`.

When a log IS given, the gate logic is sound:

- It binds the log to HEAD's tree, refuses dirty or partial runs, and requires the
  FAIL-line count to equal the summary count.
- A FAIL is excused only by an explicit `-AllowFail` regex, or by `-AllowKnownFailures`
  against an ancestor main baseline.

The fixer disclosed the NEW failure in its report.

Optional hardening: when `-Target dev` runs without `-CheckLog`, print one line saying no
suite result was checked. That way a NEW failure an agent has seen cannot look
gate-checked.

### INFO-4: the SAFTYFX5 fix-status "base" sha

`78978db64` rewrote the negtest "base" from `5870895e4` to `b4fcac924`. The negtest ran
against the pre-rebase commit, so `5870895e4` was the true base. The patch content is
unchanged by the rebase, so the evidence still holds. The sha now names the post-rebase
commit rather than the tree that was tested.

## Fix status

| Finding | Status |
|---|---|
| LOW-1 | FIXED in `aba482d76` (gap (1) reworded; reviewed in `REVIEW_FLAKEFX4_2026-10-12.md` INFO-5) |
| LOW-2 | FIXED in `032909fa1` (case 5 ready-file handshake; follow-ups in `a6e5bd18f`) |
| INFO-1..4 | no action required |
