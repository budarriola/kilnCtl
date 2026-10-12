# Safety review: saftyfx2, the REVIEW_SAFTYFX follow-up fix (2026-10-11)

Reviewer: Opus. Read-only on firmware. No board access.

Scope: origin/dev commits 96a033797 (fix, tests, docs) and 8e9784cd2
(initializer completion for target `-Werror`). Together they claim to fix
`REVIEW_SAFTYFX_2026-10-11.md` LOW-1, LOW-2, LOW-3, the S6a/S7/S16 INFO and the
stale docs. No later origin/dev commit touches `firmware/SaftyFW`; the files
reviewed are as of origin/dev 2a4aa2db3.

Files:

- `firmware/SaftyFW/src/safety_guards.c` (`s8_clear_refuse()`)
- `firmware/SaftyFW/src/tasks/link_frame.c`, `link_frame.h` (new pure helpers)
- `firmware/SaftyFW/src/tasks/link_task.c` (`link_task_handle_test_trip()`,
  `link_task_poll_test_trip_reply()`)
- `firmware/SaftyFW/test/test_safety_guards.c`, `test/test_test_trip.c`
- `firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md`, the REVIEW_SAFTYFX6/7
  correction notes, the REVIEW_SAFTYFX Fix status table

## Verdict

No HIGH and no MED findings. The LOW-3 floor is safe: it touches only the
TEST-clear check and leaves S8's own trip path as it was. It cannot let a TEST
clear through while S8's real trip condition holds. The LOW-1 fix stores the
answered outcome and seq. A DUPLICATE replays them, including
REFUSED_ALREADY_TRIPPED when a real guard won the race. The stored answer is
reset where it should be. The S6a/S7/S16 tests are real.

Two LOW findings are about tests and docs. The new helper tests are real, but
the link_task wiring is still pinned only by string scans. Four wiring
mutations were MISSED, and the Fix status claim "M5/M6 CAUGHT after the fix"
does not hold. One LOW is a gap in the TEST-clear S8 check, which is older
than this fix. One LOW is a reset-one-side instance across the link. None of
these can clear, hide or blind a real trip.

## Questions asked

### 1. LOW-3: can the floor pass a TEST clear over a live S8 condition, or blind S8?

No, with one corner case (INFO-1).

- The floor sits only in the `strict == false` branch of `s8_clear_refuse()`.
  `test_trip_masks_real_condition()` is the only caller of that branch, and
  it runs only for a latched `SAFETY_TRIP_TEST`. The strict branch, used for
  a real RATE clear, is unchanged. So is S8's trip logic in
  `safety_guards_tick()`, which keeps its own window, streak and divisor.
  Nothing in S8's trip path reads the floor.
- S8's real condition is two consecutive full `rate_window_s` windows over the
  rate. A TEST clear runs `safety_guards_clear()` and then a retest tick, as
  every clear does. S8 then starts from an empty window and re-trips after two
  windows if the rise continues. The floor does not change that.
- The floor makes the check looser only when 5 s is longer than the true time
  since the frozen window opened. The TEST latch is held for at least
  `trip_verify_s` before the check runs (`s9_verify_elapsed_s` grows only
  while K4 is de-energized). The true elapsed time is therefore at least
  `frozen + trip_verify_s`. At `trip_verify_s >= 5` s (the default is 10 s),
  the floored divisor never exceeds the true elapsed time, so the computed
  rate is never below the real average rate. See INFO-1 for
  `trip_verify_s < 5`.
- Negtests N5 (floor removed) and N6 (floor raised to 100 s) are both CAUGHT.
  N6 is caught by the existing "S8 mid-debounce rate" case at
  `test_safety_guards.c:4863`.

### 2. LOW-1: is the stored outcome and seq reset on every path?

Yes, for every path that should reset it.

| Event | What happens to `answer_outcome`/`answer_seq` | Correct? |
|---|---|---|
| New accepted request | `link_frame_decide_test_trip()` sets ACCEPTED/0 together with `last_request_id`/`last_accept_ms` | Yes (negtest N7 CAUGHT) |
| Request refused for queue busy | the whole struct is restored from `tt_state_before`, answer fields included | Yes |
| Latch observed | `record_answer(o, seq)` with o from the reason | Yes |
| Seq wait times out | `record_answer(ACCEPTED, 0)`; redundant after the decide reset (N11 is an equivalent mutant) | Yes |
| Pico reboot | `link_task_start()` zero-inits the struct, and RAM is fresh anyway | Yes |
| CLEAR_TRIP / a later real trip | not reset, on purpose. A DUPLICATE must answer the original request (TEST_TRIP_PLAN DUPLICATE row, TRIPWP2 LOW-2: never the current seq) | Yes |

Pairs on the Pico side: `answer_*` and `last_request_id` sit in one struct
and are written by one function. `s_tt_waiting_seq` is set on the same
ACCEPTED path that resets `answer_*`, and both are reset together at boot.
The cross-processor pair is LOW-4.

### 3. Is the DUPLICATE replay correct when a real guard won the race?

Yes. The poll path computes
`link_frame_test_trip_latched_outcome(reason == SAFETY_TRIP_TEST)`, which
gives REFUSED_ALREADY_TRIPPED for a real reason, and records it with the real
seq. A later DUPLICATE replays exactly that pair. While the original is still
waiting for its seq, no reply is armed (negtest N10 CAUGHT). `test_answer_replay`
covers both the race-lost and the TEST cases on the helpers. One caveat: no
test pins that link_task passes the helper's result to the reply (LOW-1).

### 4. Is the call-presence scan for M6 adequate?

No. The scan checks that a text string appears inside the function body.
It does not strip comments and does not check reachability. It also does not
check that the reply uses the helper's result. See LOW-1.

### 5. Are the new S6a/S7/S16 tests real?

Yes. `main_fault_asserted`, `estop_pressed` and `config_integrity_trip` are
unconditional, immediate trips in `safety_guards_tick()`. They are not in the
immediate-condition list, so the refusal really comes from the retest tick.
Negtest N9 removed the S7 trip from the tick and was CAUGHT. The check asserts
`reason != SAFETY_TRIP_TEST` rather than the specific reason, which is weaker
but adequate.

### 6. Are the docs accurate?

Mostly. The GUARD_TEST_MATRIX TEST row has the right guard list (S1, S2, S3,
S5, S8, S11, S12, S13, S6b by explicit check; S6a, S7, S16 by retest; S9
refused outright), names the right tests and describes the floor correctly.
The S1/S8 row matches the strict code: no frozen-window fallback, refusal
until two full post windows, and the NaN gate on the post state. The
REVIEW_SAFTYFX6/7 correction notes are accurate. Two exceptions are LOW-2 and
INFO-5.

## Findings

### LOW-1: link_task wiring of the new helpers is still pinned only by string scans; four mutations MISSED

File: `firmware/SaftyFW/test/test_test_trip.c` (`scan_wiring()`, the
"poll path uses the pure outcome helper" and "DUPLICATE replays through the
pure helper" checks).

The helpers are tested properly, and N7, N8b and N10 are CAUGHT. The
link_task side is not. `has()` is a plain `strstr` inside the function's text,
with no comment stripping and no reachability check, and nothing checks the
arguments passed to `link_task_arm_test_trip_reply()`. These mutations pass
the whole suite:

- N1: the poll reply arms `KILNLINK_TEST_TRIP_OUTCOME_ACCEPTED` instead of `o`.
  This is TRIPWP2 LOW-1 coming back at the wiring level: a real guard that won
  the race is answered ACCEPTED.
- N2: the DUPLICATE reply arms ACCEPTED instead of `dup_outcome`. This brings
  back saftyfx LOW-1.
- N3: `if (0) link_frame_test_trip_record_answer(...)`. This is M6 as dead code.
- N4: the `record_answer` call commented out. This is M6 as a comment.

Impact: a misreport only, the same as the original LOW-1/LOW-2. The latch is
real, and the HTTP/MCP layers re-read `trip_reason`. But the regression these
scans were meant to stop is still undetected.

Fix suggestion: move the wiring itself into a pure step. For example, add
`link_frame_test_trip_on_latch(state, reason, seq, *out_outcome, *out_seq)`,
which records the answer and returns what to send. Then link_task only
forwards the outputs, and the scan only needs to pin a single forwarding line.
At minimum, scan for the exact arm calls
`link_task_arm_test_trip_reply(s_tt_reply_request_id, o, seq)` and
`(msg.request_id, dup_outcome, dup_seq)`, and strip `/* */` and `//` comments
before scanning.

### LOW-2: the Fix status claim "M5/M6 CAUGHT after the fix" is not supported

Files: `docs/audits/REVIEW_SAFTYFX_2026-10-11.md` (M5/M6 rows),
`firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md` (TEST row: "negtests ... and the
LOW-1..3 mutations").

No negtest run or log is recorded for the fix. M6 as originally written
(`s_tt_accepted_seq = seq` deleted) no longer exists. Its equivalents, N3 and
N4, are MISSED. M5's helper form (N8b) is CAUGHT, but its wiring form (N1) is
MISSED. Fix: change both rows to the results in this review, or to the results
of a re-run after LOW-1 is fixed.

### LOW-3: the TEST-clear S8 check is skipped when the frozen window has 0 s elapsed, and it ignores the over-rate streak (pre-existing)

File: `firmware/SaftyFW/src/safety_guards.c`, `s8_clear_refuse()` non-strict
branch, the `state->s8_window_elapsed_s > 0.0f` term.

Consider a tick where S8 completes a window over the rate (streak 1, not yet a
trip) and slides the window (elapsed = 0). The test-trip request is evaluated
last in that same tick and latches TEST. The frozen window then has elapsed
exactly 0. The non-strict check returns false whatever the rise, and
`s8_over_rate_streak` is never consulted. This is the clearest "mid-debounce"
S8 state, the one TRIPWP2 MED-1 meant to protect, and the check cannot see it.

This is not introduced by the fix: the `> 0.0f` term predates it. But the
floor now makes it safe to drop that term, since the divisor is at least 5 s.
REVIEW_SAFTYFX LOW-3 also suggested keying on the streak.

Bounded impact: after the clear, S8 needs two fresh windows to re-trip, so a
continuing runaway is delayed by at most one window (60 s by default). S1 and
S2 are independent of this. With K4 open during the latch, the kiln has no
power, and S8 ships disabled (`max_rate_c_per_min = 0`).

Fix: drop the `> 0.0f` term now that the divisor is floored, and refuse while
`s8_over_rate_streak >= 1`.

### LOW-4: the ESP's request_id restarts at 0 on an ESP reboot, but the Pico's dedup memory does not (reset-one-side, pre-existing)

Files: `firmware/KilnFW/App/drivers/safety/safety_link_commands.c`
(`s_test_trip_request_id`, a static that starts at 0 each boot, `++` per
call); `link_frame_decide_test_trip()` (DUPLICATE checked before
already-tripped).

Sequence: the ESP sends test trip id 1, and it is accepted and latched. The
ESP reboots and sends a new test trip within 10 s. The Pico's boot_id is
unchanged, so the frame passes the boot_id check, and its id is again 1. It
matches `last_request_id` inside the interval and is answered DUPLICATE, with
the old ACCEPTED outcome and old seq replayed, not REFUSED_ALREADY_TRIPPED.
The status read-back shows `trip_reason == 4` from the old latch, so the
caller believes its new test trip happened. Nothing is cleared or masked: the
latch is real and still held.

This is the CLAUDE.md "reset one side of a pair" class: the ESP counter and
the Pico dedup key share an equality contract, and only one side resets on an
ESP reboot. The LOW-1 fix makes the replay faithful to the original request,
but it cannot tell the requests apart.

Fix options: seed the ESP counter from a random or boot-unique value, or carry
an ESP boot nonce in the request. Alternatively, check already-tripped before
DUPLICATE for any request whose stored answer is not for this latch.

### INFO

- **INFO-1: the floor can exceed the true elapsed time when `trip_verify_s` < 5 s.**
  `trip_verify_s` is a u16 with no lower bound besides 0 (which means the 10 s
  default). At 1..4 s, the floored 5 s divisor can be longer than the time
  actually elapsed since the window opened, so the check reads a lower rate
  than the true average over that short span. This is not S8's real
  condition, which needs two full windows, and S8 re-trips after the clear if
  the rise continues. A tighter divisor would be `frozen elapsed +
  s9_verify_elapsed_s`. That is always at most the true time, so it stays
  conservative, and it would also fix the second half of saftyfx LOW-3 (INFO-2).
- **INFO-2: the "slowly warming kiln" half of saftyfx LOW-3 is not addressed.**
  The rise keeps growing against a frozen divisor. The Fix status says so. It
  fails safe: the TEST latch stays until the kiln cools.
- **INFO-3: `s_tt_reply_request_id` is shared between a pending ACCEPTED reply
  and any refusal reply (pre-existing).** A second request that arrives while
  the first waits for its seq is refused, and arming that refusal overwrites
  the id. The first request's final answer then goes out under the second
  request's id. The ESP holds a transaction lock and waits about 345 ms, longer
  than the Pico's 300 ms seq wait, so this needs an ESP timeout followed by an
  immediate retry. Misreport only.
- **INFO-4: the shipped ESP client never produces a DUPLICATE.** It uses a
  fresh `request_id` for each call (except across a reboot, LOW-4), so the
  replay path guards against retransmission rather than a path exercised
  today.
- **INFO-5: REVIEW_SAFTYFX's own guard labels are wrong.** Its "Can a TEST
  clear mask" list says SENSOR_INVALID (S1), OVERTEMP (S2), OVER_SETPOINT (S3),
  LOAD_STUCK_ON (S5). The code says S5, S1, S2 and S3 respectively. Its guard
  table and the GUARD_TEST_MATRIX row use the right numbers.
- **INFO-6: N11 is an equivalent mutant.** The timeout path's
  `record_answer(ACCEPTED, 0)` repeats what the ACCEPTED decision already
  stored, so removing it cannot be observed.

## Testing

Worktree `C:\wt\rvsfx2_7849x3` at origin/dev 2a4aa2db3. Ran
`tools\negtest.ps1 -Preset saftyfw-host` with the default verdict
`SAFTYFW HOST TESTS: FAILED` and `-Parallel 3`. The baseline passed (SaftyFW
host tests all passed), the real tree was unchanged and every copy was
removed. Logs: `%TEMP%\negtest_logs\20261011_232229_ib02` and
`20261011_232722_ps97`.

| Mutation | Result |
|---|---|
| N1: poll reply arms ACCEPTED instead of `o` | **MISSED** (LOW-1) |
| N2: DUPLICATE reply arms ACCEPTED instead of `dup_outcome` | **MISSED** (LOW-1) |
| N3: `record_answer` under `if (0)` | **MISSED** (LOW-1) |
| N4: `record_answer` commented out | **MISSED** (LOW-1) |
| N5: LOW-3 floor removed | CAUGHT `test_safety_guards.c` (tiny window) |
| N6: LOW-3 floor raised to 100 s | CAUGHT `:4863` S8 mid-debounce |
| N7: decide does not reset `answer_seq` | CAUGHT |
| N8: `latched_outcome` constant (unused param) | build error, invalid; re-run as N8b |
| N8b: `latched_outcome` always ACCEPTED | CAUGHT |
| N9: S7 trip removed from tick | CAUGHT (k=10 retest) |
| N10: `duplicate_replay` ignores `original_waiting` | CAUGHT |
| N11: timeout-path `record_answer` removed | MISSED, equivalent mutant (INFO-6) |

## Fix status (saftyfx3)

All mutations run with `tools
egtest.ps1 -Preset saftyfw-host -ExpectPattern "FAIL .*\.c:\d+|SAFTYFW HOST TESTS: FAILED"`, by the fixer, on the fix commits (final run on the tip; N4b, N5 re-run after test tweaks). Wiring mutations N1-N4 are re-expressed against the new glue (`link_task` forwards `link_frame_test_trip_reply_on_*()` results).

| Item | Status |
|---|---|
| LOW-1 | FIXED. Reply decision moved into pure `link_frame_test_trip_reply_on_latch/_timeout/_duplicate()` (they also record the answer); `link_task` only forwards. `test_reply_glue` tests them behaviourally; the scan now strips comments, normalises whitespace and pins the exact three forwarding sites. N1 (arms ACCEPTED) CAUGHT, N2 (DUPLICATE arms ACCEPTED) CAUGHT, N3 (`if (0)`) CAUGHT, N4 (commented out) CAUGHT, plus N4b (record_answer removed in the helper) CAUGHT and N1b (stale reply id) CAUGHT. |
| LOW-2 | FIXED. REVIEW_SAFTYFX M5/M6 rows and GUARD_TEST_MATRIX cite the runs above. |
| LOW-3 | FIXED. The `> 0` term is gone; a window with streak >= 1 refuses on any rise since it opened (a flat or falling reading clears; refusing on the streak alone would deadlock, since S8 is frozen by the TEST latch). Tests `test_test_clear_s8_streak_zero_elapsed`. L3a (streak check disabled) CAUGHT, L3b (rise tolerance +100 C) CAUGHT. Direction is refuse-only. |
| LOW-4 | Not touched (ESP side, tripwp4/tripfx3). |
| INFO-1 | FIXED. Divisor is frozen elapsed + `s9_verify_elapsed_s`, floored at 5 s. The latch is held at least `trip_verify_s` before the check, so this is the true time since the window opened (frozen elapsed is exact; verify time only grows while K4 is open); the 5 s floor is the only term that can exceed truth, and only when `trip_verify_s` < 5 s. `test_test_clear_s8_divisor`. N5 (floor removed) CAUGHT, N6 (floor 100 s) CAUGHT, L3c (verify term dropped) CAUGHT. |
| INFO-3 | FIXED. `s_tt_wait_request_id` holds the waiting request's id; `s_tt_reply_request_id` is only set by the arm call, so a refusal reply can no longer re-address the pending final answer. N1b CAUGHT. |
| INFO-5 | FIXED in REVIEW_SAFTYFX. |
| Re-runs | N7, N8b, N9, N10 CAUGHT. N5/N6 CAUGHT. |
