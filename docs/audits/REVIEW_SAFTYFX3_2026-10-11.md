# Safety review: saftyfx3, the REVIEW_SAFTYFX2 follow-up fix (2026-10-11)

Reviewer: Opus. Read-only on firmware. No board access.

Scope: origin/dev 76658651a..bdceee5a8 (5 commits), which claim to fix
`REVIEW_SAFTYFX2_2026-10-11.md` LOW-1, LOW-2, LOW-3, INFO-1, INFO-3 and INFO-5.

Files:

- `firmware/SaftyFW/src/safety_guards.c` (`s8_clear_refuse()`, non-strict branch)
- `firmware/SaftyFW/src/tasks/link_frame.c`/`.h` (`link_frame_test_trip_reply_on_latch/_on_timeout/_on_duplicate()`)
- `firmware/SaftyFW/src/tasks/link_task.c` (`s_tt_wait_request_id`, the three forwarding sites)
- `firmware/SaftyFW/test/test_safety_guards.c`, `test/test_test_trip.c`
- `firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md`, the REVIEW_SAFTYFX/SAFTYFX2 doc edits

## Verdict

No HIGH and no MED findings. The streak rule only adds refusals. The reply
glue is now pure and behaviourally tested, and each of the three reply sites
answers under the right request id. The forwarding scan is exact enough that
dead-code, commented-out and wrong-argument mutations of the forwarding lines
are all CAUGHT.

One LOW is new and comes from INFO-1's divisor. Dividing by the true time
since the frozen window opened makes the TEST-clear S8 check an average over
the whole latch hold. A fresh rise that starts late in a long hold is
diluted, where the saftyfx2 divisor (frozen elapsed, floored) refused it. One
LOW is test coverage: the line that sets `s_tt_wait_request_id` is not
pinned. REVIEW_SAFTYFX2 LOW-4 is still open, but an ESP-side precheck narrows
it more than that review said.

## Questions asked

### 1. Can a TEST clear now mask a real S8 (or any other) over-rate condition?

Not for any guard other than S8: the change touches only `s8_clear_refuse()`'s
non-strict branch, and the other guards' checks in
`test_trip_masks_real_condition()` are unchanged. The strict branch (a real
RATE latch) and S8's trip path are unchanged too.

For S8 the two changes pull in opposite directions:

- The streak rule (`s8_over_rate_streak >= 1` and any rise since the frozen
  window opened) only adds refusals. It closes REVIEW_SAFTYFX2 LOW-3: a
  zero-elapsed frozen window no longer skips the check.
- The divisor change loosens the check compared with saftyfx2, for windows
  with streak 0. See LOW-1.

After any granted clear `safety_guards_clear()` calls `safety_guards_reset()`,
so S8 starts from an empty window and re-trips after two full windows if the
rise continues. K4 is open for the whole TEST latch. S1, S2 and S9 are
independent of this check.

### 2. Can the streak rule wedge the latch forever on a real kiln?

No, not in practice. It refuses only while the reading is above
`s8_window_start_c`, the reading when the over-rate window last slid. With K4
open the kiln coasts and then cools, so the reading drops below that point.
Two edge cases are worth knowing (INFO-1): with no deadband, a kiln sitting
flat at the start value clears on roughly half of attempts because of noise,
and a cold kiln in a warming room can stay just above the start value for a
long time. The streak was set by a window that was really over the rate, so
both cases fail safe.

### 3. Is the divisor right for every `trip_verify_s`?

Yes, under its own claim. `s8_window_elapsed_s` grows only on valid-read
ticks before the latch, and it includes the latch tick, because the request
is evaluated after the S8 block. `s9_verify_elapsed_s` grows only on later
tripped ticks with K4 reported de-energized, and it is reset to 0 whenever K4
is not reported open. Their sum is therefore at most the true time since the
window opened, so the computed rate is at least the true average. The clear
needs `s9_verify_elapsed_s >= trip_verify_s`, where 0 means the 10 s default
and any value 1..65535 is allowed. So the 5 s floor binds only when
`trip_verify_s` is 1..4, and only then can the divisor be longer than the
true time. The test cases at `trip_verify_s = 1` (2 and 60 hold ticks) match
the arithmetic, with the latch tick's dt counted as frozen time.

The flaw is not in the arithmetic but in what is averaged (LOW-1).

### 4. Can the reply or request_id handling send the wrong answer for a request id?

No.

- ACCEPTED stores the id in `s_tt_wait_request_id`. The latch and timeout
  replies take their id from that variable through the helper, and refusals
  use `msg.request_id`. A refusal armed while the first request waits no
  longer re-addresses the first request's final answer (INFO-3 fixed).
- A DUPLICATE answers with the incoming id, and the helper replays the stored
  outcome and seq.
- The ESP drops any reply whose `request_id` differs from its own
  (`safety_link_commands.c:1118`). So the worst a stale id can cause is
  NO_REPLY, never a wrong outcome.

A residual pre-existing gap: there is one reply slot. If the first request's
latch is seen in the same poll as a refusal armed for a second request, the
refusal is overwritten before its first copy goes out, and the second caller
sees NO_REPLY (INFO-2). The answer is lost, never misaddressed.

### 5. Is the source scan robust or vacuous?

It is robust for what it pins. `normalized_body()` strips `/* */` and `//`
comments and collapses whitespace. Each check then needs the exact forwarding
statement, with the block's closing `}` right after the arm call. An
`if (0)`, a `#if 0`, a comment, a swapped argument or an extra statement
between the helper and the arm call all break the match. A missing function
body makes the check fail rather than skip. The scan does not pin the
ACCEPTED-path assignment `s_tt_wait_request_id = msg.request_id;` (LOW-2), or
the latch trigger `seq != s_tt_seq_base` (INFO-3).

## Findings

### LOW-1: the S8 TEST-clear divisor averages over the whole hold, so a late fresh rise is diluted

File: `firmware/SaftyFW/src/safety_guards.c`, `s8_clear_refuse()` non-strict
branch, `elapsed_s = s8_window_elapsed_s + s9_verify_elapsed_s`.

The check divides the rise since the frozen window opened by the total time
since then. That time includes the entire hold. Example with a 10 C/min limit
and a 60 s window: the reading is flat for 10 minutes after the latch, then
rises 20 C in the last minute (a 2x-limit rise by S8's own window measure).
The divisor is about 660 s, so the check reads 1.8 C/min and grants the clear.
The saftyfx2 divisor (frozen elapsed floored at 5 s) refused this. In general,
any clear tried more than about `rate_window_s * (R/limit)` after the latch
misses a rise of R over the last window.

Why it is LOW: with K4 open, a rise like this needs a heat source that is not
the elements, a K4 failure that S9 cannot see (no CT fitted), or a drifting
sensor. S1 and S2 still apply. After the clear, S8 restarts and trips after
two windows if the rise continues. S8 ships disabled. The change does fix the
"slowly warming kiln" refusal noted as saftyfx2 INFO-2, which was its intent.

Fix suggestion: for a TEST latch, also run the post-trip window tracking that
the tripped path already runs for `SAFETY_TRIP_RATE` (`s8_post_*`). Then refuse
in the non-strict branch while the latest completed post window, or the
current partial window once it is at least `S8_POST_MIN_S` long, is over the
rate. Keep the present whole-span average as a second test. That bounds the
measure to `rate_window_s`, as S8's own trip path is, and the post window
slides, so the slow-warming case still clears.

### LOW-2: nothing pins `s_tt_wait_request_id = msg.request_id;` on the ACCEPTED path

File: `firmware/SaftyFW/test/test_test_trip.c` (`scan_wiring()`).

The forwarding scan pins that the latch and timeout replies read
`s_tt_wait_request_id`. It does not pin that ACCEPTED writes that variable.
With the write removed (negtest P1), every final reply goes out under 0 or a
previous request's id. The ESP drops it, and every accepted test trip is
reported as NO_REPLY. That is a misreport only: the latch is real. It is the
same kind of wiring regression that REVIEW_SAFTYFX2 LOW-1 was about.

Fix: add `s_tt_wait_request_id = msg.request_id;` to the normalized-body
checks of `link_task_handle_test_trip()`, next to `s_tt_waiting_seq = true;`.

### LOW-3 (REVIEW_SAFTYFX2 LOW-4, still open): ESP request_id restarts at 0 on ESP reboot; Pico dedup does not

Still open: `safety_link_commands.c:1011` is a zero-initialised static that is
incremented per call, and the Pico still checks DUPLICATE before
already-tripped. It is narrower than REVIEW_SAFTYFX2 said, because the ESP
refuses locally with LOCAL_ALREADY_TRIPPED whenever its cached DIAG shows the
Pico tripped (`safety_link_commands.c:1053`). It also refuses with
LOCAL_STALE when it has no fresh DIAG. So the replay needs all of these
inside the Pico's 10 s dedup interval: the first request accepted, its latch
cleared (possible only with `trip_verify_s` well under 10 s), the ESP
rebooted, and a new request sent. In that case the caller is told ACCEPTED
with the old seq, but no new trip latched. The status read-back shows the
Pico not tripped, so the error is visible. Fix options are unchanged.

### INFO

- **INFO-1: the streak rule has no deadband.** `in->tc_c > s8_window_start_c`
  refuses a rise of 0.01 C. At a flat temperature, noise makes some clears
  refuse, so the operator may need to retry. A deadband of about the read
  noise (0.25 C) would remove this without masking a real rise. Optional.
- **INFO-2: one reply slot.** A refusal armed for a second request can be
  overwritten by the first request's final reply before any copy is sent.
  The second caller gets NO_REPLY. This is pre-existing and never misaddresses
  an answer.
- **INFO-3: the latch trigger is unpinned.** If `seq != s_tt_seq_base` is
  inverted (negtest P2), every accepted request times out and answers
  ACCEPTED with seq 0, even when a real guard won the race. This is
  pre-existing wiring, and the result is a misreport only.
- **INFO-4: `test_test_clear_s8_streak_zero_elapsed` does not reach a frozen
  elapsed of 0.** `latch_test_trip_and_hold()` latches on a separate
  tick, after the slide, which adds one dt. The streak rule does not depend on
  elapsed, and L3a shows the test discriminates, so this affects only the
  test's title.
- **INFO-5: the Fix status says "frozen elapsed is a lower bound".** It is a lower
  bound: bad-read ticks pause the window, and verify time only counts while K4
  is reported open. That is the conservative direction, so the conclusion
  stands.

## Testing

Worktree `C:\wt\sfx3rv_1yry1x` at bdceee5a8. `tools\negtest.ps1 -Preset
saftyfw-host -ExpectPattern "FAIL .*\.c:\d+|SAFTYFW HOST TESTS: FAILED"
-Parallel 3`, baseline passed, real tree unchanged, copies removed.

The baseline passed. All copies were removed and no tracked file changed.
negtest reported ERROR "real tree changed" only because this review file was
created, untracked, in the worktree during the run. Logs:
`%TEMP%
egtest_logs61012_000014_9d0m`.

| Mutation | Result |
|---|---|
| P1: `s_tt_wait_request_id = msg.request_id;` removed | **MISSED** (LOW-2) |
| P2: latch trigger `seq != s_tt_seq_base` inverted | **MISSED** (INFO-3, pre-existing) |
| P3: divisor verify term doubled (longer than the true time) | CAUGHT `test_safety_guards.c:4944` |
| P4: streak rule requires `!s8_window_active` | CAUGHT `test_safety_guards.c:4975` |
| P5: timeout reply forwards `s_tt_reply_request_id` | CAUGHT `test_test_trip.c:301` |

Not re-run: the fixer's N1-N4, N1b, N4b, L3a-c, N5 and N6, which are cited in
the REVIEW_SAFTYFX2 Fix status. P5 independently confirms that the
forwarding scan catches a wrong id argument.

## Fix status

| Item | Status |
|---|---|
| LOW-1 | FIXED. TEST latches now run the `s8_post_*` window tracking in `safety_guards_tick`; the non-strict `s8_clear_refuse` also refuses when the latest completed post window, or a partial one of at least `S8_POST_MIN_S`, is over the rate limit. Only adds refusals; a slowly warming kiln still clears (`test_test_clear_s8_late_rise`, k=0..3). Existing hold tests now hold at the clear reading to avoid a phantom jump. |
| LOW-2 | FIXED. `test_test_trip.c` scan pins `s_tt_waiting_seq = true; s_tt_seq_base = base; s_tt_wait_request_id = msg.request_id;`. Mutation: store removed, CAUGHT `test_test_trip.c:318`. |
| INFO-3 (P2) | FIXED. Scan pins `if (have && seq != s_tt_seq_base) {`. Mutation: inverted, CAUGHT `test_test_trip.c:304`. |
| LOW-3 | FIXED on the ESP side only (the seeding itself landed in 57c4e0454, TEST_TRIP WP3 review fixes; 9ba887351 added only the test, per REVIEW_SAFTYFX4 LOW-3): `safety_link_send_test_trip` lazily seeds `s_test_trip_request_id` from `hal_sysinfo_random_u32()` once per boot (under xact_lock). Pico-side reorder not done: it changes decision semantics. Residual 1/256 chance of a post-reboot collision with the Pico dedup memory. Test: `test_send_test_trip` scripts the RNG to 0x40 and asserts the first id is 0x41. |
| INFO-4, INFO-5 | FIXED (wording): streak test title says near-zero frozen window; "exact" changed to "a lower bound". |
| TRIPWP2 LOW-6 | FIXED: boot heap log in `main.c`. |
| Pooled: bound on 0x0205 `rate_window_s` | NOT DONE. No precedent for a U16 time-field bound in `config_params.c` (only the F32 rate has a range), and adding one to `config_params_validate_ranges` could invalidate stored records at load. Needs an owner decision. |

Negative tests (SaftyFW `-Preset saftyfw-host`, KilnFW `-Preset kilnfw-host`), all CAUGHT:
- L1-a post-window refusal disabled; L1-b TEST latch not tracking post windows; L1-c partial window ignored; L1-d wrong comparison sense; LOW-2 request id store removed; P2 latch trigger inverted.
- LOW-3: seed removed (`s_test_trip_request_id = 0u;`); LOW-3b: seeded flag preset true (CAUGHT `test_safety_link_compile.c:3197`; corrected in REVIEW_SAFTYFX4 LOW-3, the earlier :3150 was `tt_set_reply()`'s opening brace).

Mutation definitions (negtest `-Mutations` JSON):

SaftyFW:

```json
[
{"name":"L1-a: post-window refusal disabled","file":"firmware/SaftyFW/src/safety_guards.c","find":"    if (state->s8_post_active) {\n        if (state->s8_post_windows_done >= 1u &&","replace":"    if (0 && state->s8_post_active) {\n        if (state->s8_post_windows_done >= 1u &&"},
{"name":"L1-b: TEST latch no longer tracks post windows","file":"firmware/SaftyFW/src/safety_guards.c","find":"state->reason == SAFETY_TRIP_RATE || state->reason == SAFETY_TRIP_TEST","replace":"state->reason == SAFETY_TRIP_RATE"},
{"name":"L1-c: partial post window ignored","file":"firmware/SaftyFW/src/safety_guards.c","find":"        if (state->s8_post_elapsed_s >= S8_POST_MIN_S) {\n            float pm","replace":"        if (state->s8_post_elapsed_s >= 1.0e9f) {\n            float pm"},
{"name":"L1-d: completed window compares against wrong sense","file":"firmware/SaftyFW/src/safety_guards.c","find":"state->s8_post_last_rate_c_per_min > cfg->max_rate_c_per_min) {\n            return true;\n        }\n        if (state->s8_post_elapsed_s >= S8_POST_MIN_S) {\n            float pm","replace":"state->s8_post_last_rate_c_per_min < cfg->max_rate_c_per_min) {\n            return true;\n        }\n        if (state->s8_post_elapsed_s >= S8_POST_MIN_S) {\n            float pm"},
{"name":"LOW-2 P1: ACCEPTED no longer stores request id","file":"firmware/SaftyFW/src/tasks/link_task.c","find":"        s_tt_wait_request_id = msg.request_id;\n","replace":""},
{"name":"INFO P2: latch trigger inverted","file":"firmware/SaftyFW/src/tasks/link_task.c","find":"if (have && seq != s_tt_seq_base) {","replace":"if (have && seq == s_tt_seq_base) {"}
]
```

KilnFW:

```json
[
{"name":"LOW-3: request id seed removed","file":"firmware/KilnFW/App/drivers/safety/safety_link_commands.c","find":"        s_test_trip_request_id = (uint8_t)hal_sysinfo_random_u32();\n","replace":"        s_test_trip_request_id = 0u;\n"},
{"name":"LOW-3b: seed never applied (flag preset)","file":"firmware/KilnFW/App/drivers/safety/safety_link_commands.c","find":"static bool s_test_trip_request_id_seeded;","replace":"static bool s_test_trip_request_id_seeded = true;"}
]
```
