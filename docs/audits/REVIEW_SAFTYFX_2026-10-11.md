# Safety review: saftyfx, the TRIPWP2 + S8CLRFX2 follow-up fix (2026-10-11)

Reviewer: Opus. Read-only on firmware. No board access.

Scope: origin/dev commits 05c6832c1 (fix) and 150d14c6a (tests). They claim to
fix `REVIEW_TRIPWP2_2026-10-10.md` MED-1, MED-2, LOW-1..5 and
`REVIEW_S8CLRFX2_2026-10-10.md` LOW-1..3. The reviewed files are unchanged at
origin/dev 2d191c82f.

Files:

- `firmware/SaftyFW/src/safety_guards.c`
- `firmware/SaftyFW/src/tasks/link_task.c`
- `firmware/SaftyFW/test/test_safety_guards.c`
- `firmware/SaftyFW/test/test_test_trip.c`
- `firmware/SaftyFW/test/test_safety_core_host.c`
- `docs/TEST_TRIP_PLAN.md`

Owner rules checked:

- TEST_TRIP is admin-only, `SAFETY_TRIP_TEST = 4`, mask `0x0008`.
- A test trip clears only after `trip_verify_s`.
- At most one test trip per 10 s.
- No guard is ever disabled or blinded.

## Verdict

No HIGH and no MED findings. The fix closes TRIPWP2 MED-1: a test-trip clear
can no longer hide a real guard condition. It also closes S8CLRFX2 LOW-1..3,
and it keeps a legitimate S8 clear reachable.

Three LOW findings remain:

- LOW-1: a DUPLICATE retry after a real guard won the race replays ACCEPTED
  carrying the real trip's seq.
- LOW-2: the two source-scan tests that pin the TRIPWP2 LOW-1/LOW-2 fixes are
  vacuous (negtest MISSED).
- LOW-3: the TEST-clear S8 check is over-strict. This is an availability
  problem, not a safety one.

None of the three can clear or hide a real trip.

## Safety questions

### Can a TEST clear mask or blind a real guard?

No. `safety_guards_try_clear()` handles `SAFETY_TRIP_TEST`
(`safety_guards.c:244-258`) in this order:

1. It refuses until `s9_verify_elapsed_s >= trip_verify_s`.
2. It refuses while `test_trip_masks_real_condition()` (`:181-193`) holds.
   That function covers:
   - SENSOR_INVALID (S1)
   - OVERTEMP (S2)
   - OVER_SETPOINT (S3)
   - LOAD_STUCK_ON (S5)
   - FROZEN_SENSOR (S11)
   - ENCLOSURE_TEMP (S12)
   - BORROWED_STALE (S13)
   - LINK_DEAD (S6b)
   - the non-strict S8 rate check

After those checks pass, the existing clear-plus-retest tick runs every
unconditional guard again. S16, S6a (MAIN_FAULT) and S7 re-trip in that tick,
which refuses the clear.

S9 (INEFFECTIVE) is refused unconditionally before any of this. While any
latch is held, every other guard's debounce is frozen, so these immediate
checks are what stops the test latch from hiding a condition that was
mid-debounce.

### Is the guard list complete against SAFETY_TRIP_*?

Yes.

| Guard | How a TEST clear handles it |
|---|---|
| S1, S2, S3, S5, S8, S11, S12, S13, S6b | Explicit check |
| S6a, S7, S16 | Retest tick |
| S9 | Refused in the tripped path |
| S4, S10, S14 | WARN-only, nothing latches |
| SELF_TEST | Not part of this module |

Negtests M2, M3 and M4 below show that dropping any of the sampled entries is
caught.

### Same-tick precedence

`test_trip_requested` is evaluated last in the not-tripped tick
(`safety_guards.c:1355`), after S16, S6a, S6b, S7, S5, S1, S11, S12, S8, S2,
S3, S13 and S14. A real guard that fires in the same tick therefore latches
its own reason, and the test request is dropped. The poll path then reports
REFUSED_ALREADY_TRIPPED.

`test_test_trip_loses_to_every_real_guard` covers S6a, S7, S1, S12 and S13.
Negtest M8 (move the test-trip block in front of S16) is caught.

### The S8 changes: NaN, invalid TC, dt

The strict S8 clear and the TEST-clear S8 check both refuse when the TC is not
valid or reads NaN (`s8_clear_refuse()`, `:142-175`). The tripped-path post
measurement is gated `in->tc_valid && !isnan(in->tc_c)`, so a NaN sample can
no longer poison the post-window state.

`dt_s` comes from `tick_dt_compute_s()` in `safety_core.c`, which measures and
clamps it. A NaN or a huge `dt` cannot reach this code.

The strict clear needs `s8_post_windows_done >= 2`. The post window is latched
at trip time and clamped to [60, 600] s. **The earliest S8 clear is therefore
2 × clamp(rate_window_s, 60, 600): 120 s at the default 60 s window, at most
1200 s.** After that, a cooled or steady kiln still gets a clear.

### Are the source-scan tests vacuous?

Partly. The F6-proximity scan, the poll-call scan, the single-request-site
scan and the LOW-3 scans pin real structure. The LOW-1 and LOW-2 scans are
vacuous: see LOW-2 below.

## Findings

### LOW-1: DUPLICATE after a lost race replays ACCEPTED with the real trip's seq

Files:

- `firmware/SaftyFW/src/tasks/link_task.c:1702`
- `firmware/SaftyFW/src/tasks/link_task.c:1659`

The poll path computes
`o = (reason == SAFETY_TRIP_TEST) ? ACCEPTED : REFUSED_ALREADY_TRIPPED`, which
is correct. It then sets `s_tt_accepted_seq = seq` **unconditionally**, so
the seq is stored even when a real guard won the race. `link_frame_decide_test_trip()`
stamped the dedup state on its original ACCEPTED decision. A retry with the
same `request_id` inside 10 s therefore gets DUPLICATE. The DUPLICATE branch
(`:1659`) always replies ACCEPTED with `s_tt_accepted_seq`, which is the real
trip's seq.

`docs/TEST_TRIP_PLAN.md`'s DUPLICATE row says "the original outcome and
trip_seq are re-sent". The original outcome here was REFUSED_ALREADY_TRIPPED.
For retries, this reopens TRIPWP2 LOW-1 and LOW-2.

Impact: a misreport only. The real latch stays in place, and its own clear
checks still apply. The HTTP layer and the MCP tool both re-read status for
`trip_reason == 4`, so a caller that follows the plan's read-back sees the
real reason. Nothing auto-clears.

Fix:

- Store the outcome alongside the seq.
- Store the seq only when the reason is TEST, or else store the
  REFUSED_ALREADY_TRIPPED outcome.
- Replay the stored outcome on DUPLICATE.

### LOW-2: the LOW-1/LOW-2 source scans are vacuous

File: `firmware/SaftyFW/test/test_test_trip.c`

The LOW-1 scan checks only that the string `reason == SAFETY_TRIP_TEST`
appears in the poll block. The LOW-2 scan checks only that
`KILNLINK_TEST_TRIP_OUTCOME_ACCEPTED, s_tt_accepted_seq` appears. Two
mutations survived:

- M5: the ternary is changed to always produce ACCEPTED, and the compared
  string stays in the source.
- M6: `s_tt_accepted_seq = seq;` is deleted.

Fix: replace both scans with behavioural tests.

- Factor the poll outcome and the DUPLICATE replay into a pure helper in
  `link_frame.c`, as `link_frame_decide_test_trip()` already is.
- Test the real-guard-wins case and the DUPLICATE retry on top of that
  helper.
- The same test then pins the LOW-1 fix.

### LOW-3: the TEST-clear S8 check is over-strict

File: `firmware/SaftyFW/src/safety_guards.c:171`

The non-strict branch computes
`(tc - s8_window_start_c) / (s8_window_elapsed_s / 60)`. Both terms come from
the window that was frozen when the test trip latched, but `tc` keeps moving.

- If the latch lands moments after a window started, elapsed is tiny. Any
  rise refuses the clear. The test itself needs `tc_c` exactly 40 with
  elapsed 0.1 s to reach the "ok" case.
- On a slowly warming idle kiln, the rise keeps growing while the denominator
  stays frozen. The clear can then refuse until the temperature falls back
  below the window start.

This fails safe: the result is a stuck test latch, never a missed trip. S8
also ships disabled (`max_rate = 0`). It still contradicts the intent that a
test trip clears after `trip_verify_s`.

Suggestion: refuse only when `s8_over_rate_streak >= 1` at latch time, or
measure from a start temperature and time taken at the latch.

### INFO

- **S6a, S7 and S16 have no TEST-clear test.** The retest tick covers them by
  construction, but no test pins it. Add one each to
  `test_test_trip_clear_refuses_real_conditions`.
- **S11 is over-strict on a quiet bench.** A flat TC trips the S11 immediate
  check, which can refuse a TEST clear on a very stable fixture. It fails
  safe.
- **S1 and S5 refuse on a single bad read.** This is stricter than the real
  guards' debounce, but consistent with "never clear over a live condition".
- **MED-2(a), the F6 refresh, is pinned only by a source-proximity scan.**
  Negtest of TRIPWP2 showed that scan catches removal. A behavioural test
  would be better.
- **The clear retest reuses the input struct.** `safety_guards_try_clear()`
  retests with the same `safety_guard_input_t`, which may carry
  `test_trip_requested`. That is unreachable today: the request is drained
  in `safety_core_build_input()`, and a clear is never processed in a tick
  that also carries a request. Clearing the flag on the retest copy would
  make this robust.
- **TRIPWP2 LOW-3, queue busy, is fixed.** A busy queue restores
  `s_test_trip_state` and replies REFUSED_RATE_LIMIT.
- **TRIPWP2 LOW-5, the reboot classification, is fixed.**
  `test_admin_test_trip_boot_reason_latch` pins it.
- **TRIPWP2 LOW-6, the heap report after the stack bump, is not addressed.**
  It is out of scope here.

## Documentation that is now stale

Not edited here. Per instructions, `GUARD_TEST_MATRIX.md` is not edited in
the main tree.

- `firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md`, S1/S8 row (line ~126). It
  still says "until the first one completes the frozen tripping window
  decides and refuses while it exceeds the rate". It should say:
  - The clear is refused unconditionally until two full post windows
    complete. The earliest clear is 2 × clamp(rate_window_s, 60, 600) s, which
    is 120 s by default.
  - A NaN TC flagged valid is refused and never enters the post state.
  - Cite `test_s8_clear_followups` (P2, the LOW-2 floor, LOW-3 NaN modes 0-4)
    and negtests M7 and M9 of this review.
- `GUARD_TEST_MATRIX.md`, TEST row (line ~20). Add:
  - The clear also refuses while any real guard's immediate condition holds:
    S1, S2, S3, S5, S8 (non-strict), S11, S12, S13, S6b.
  - S6a, S7 and S16 are caught by the retest tick.
  - A real guard wins same-tick precedence.
  - Cite `test_test_trip_clear_refuses_real_conditions`,
    `test_test_trip_loses_to_every_real_guard`, and negtests M1-M4 and M8.
- `REVIEW_SAFTYFX7_2026-10-10.md` Fix status ("until then the frozen trip
  window refuses") and `REVIEW_SAFTYFX6_2026-10-10.md` Fix status
  ("frozen-window fallback only before any post-trip time") are superseded.
  There is no frozen-window fallback for a real RATE trip any more: the clear
  is refused outright before two post windows complete.

## Testing

Host tests at 150d14c6a, in short-path worktree `C:\wt\rvsfx_lsq8fd`: **SAFTYFW
HOST TESTS: all passed.**

Negtest: `tools\negtest.ps1 -Preset saftyfw-host`, with
`-ExpectPattern "FAIL .*\.c:\d+|SAFTYFW HOST TESTS: FAILED"`, nine mutations
and `-Parallel 3`. The baseline passed, the real tree was unchanged, and every
copy was removed. Logs: `%TEMP%\negtest_logs\20261011_224620_i1dv`.

| Mutation | Result |
|---|---|
| M1: non-strict S8 check in TEST clear disabled | CAUGHT `:4859` |
| M2: LINK_DEAD dropped from the TEST real-condition list | CAUGHT |
| M3: OVERTEMP dropped | CAUGHT |
| M4: OVER_SETPOINT dropped | CAUGHT |
| M5: poll outcome ternary always ACCEPTED | **MISSED** (LOW-2); CAUGHT after the fix, see Fix status |
| M6: `s_tt_accepted_seq = seq` removed | **MISSED** (LOW-2); CAUGHT after the fix, see Fix status |
| M7: S8 previous-post-window check disabled | CAUGHT `:3512` |
| M8: test-trip request evaluated before S16 | CAUGHT `:4705`, `:4802` x5 |
| M9: S8 strict clear granted after one post window | CAUGHT `:3422`, `:3599` |

## Fix status

| Item | Status |
|---|---|
| LOW-1 | FIXED. The answered outcome and seq are stored in `link_test_trip_state_t` (`answer_outcome`/`answer_seq`, recorded when the reply is finalised) and a DUPLICATE replays them. |
| LOW-2 | FIXED. `link_frame_test_trip_latched_outcome()`, `link_frame_test_trip_record_answer()` and `link_frame_test_trip_duplicate_replay()` are pure helpers in `link_frame.c`; `test_answer_replay` tests them. The vacuous source scans are replaced by a scan that link_task calls the helpers. |
| LOW-3 | FIXED. The non-strict S8 check floors the frozen-window divisor at `S8_POST_MIN_S` (5 s); `test_test_clear_s8_tiny_window`: +0.5 C clears, +20 C still refuses. The slow-warming case (rise growing against a frozen window) still refuses, fail-safe. |
| INFO S6a/S7/S16 | Tests added (`test_test_trip_clear_refuses_real_conditions`, k=9..11: the retest tick re-trips them). |
| Stale docs | GUARD_TEST_MATRIX.md S1/S8 and TEST rows updated; REVIEW_SAFTYFX6/7 carry a correction note. |
