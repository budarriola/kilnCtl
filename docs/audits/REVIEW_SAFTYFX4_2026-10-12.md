# REVIEW_SAFTYFX4 (2026-10-12): adversarial review of 9ba887351 (SaftyFW TEST_TRIP leftovers)

Scope: `origin/dev` 9ba887351, which fixes REVIEW_SAFTYFX3 LOW-1, LOW-2, INFO-3,
INFO-4, INFO-5 and TRIPWP2 LOW-6. Files: `firmware/SaftyFW/src/safety_guards.c`
(the TEST latch now tracks the `s8_post_*` windows, and the non-strict `s8_clear_refuse`
uses them), `firmware/SaftyFW/test/test_safety_guards.c`,
`firmware/SaftyFW/test/test_test_trip.c` (scan pins), `firmware/SaftyFW/src/main.c`
(boot heap log), `firmware/KilnFW/App/test/test_safety_link_compile.c` (request_id
seed test), and the REVIEW_SAFTYFX3, REVIEW_TRIPWP2 and GUARD_TEST_MATRIX docs.

Verdict: **no HIGH or MED findings.** The change only adds refusals to the TEST
clear, and the RATE latch path is unchanged. The refusal cannot block a legitimate
clear for longer than one post window. The gaps are in the tests and in the fix
record: the new completed-window refusal and the "a cooling kiln still clears" claim
are not pinned by any test (4 mutations MISSED), and one cited mutation cannot be
reproduced as written.

## Findings

### LOW-1: the completed-post-window refusal is never the sole refuser in any test

`s8_clear_refuse()` non-strict branch, `s8_post_windows_done >= 1u &&
s8_post_last_rate_c_per_min > max_rate` (safety_guards.c:186-189).

In `test_test_clear_s8_late_rise`, the rise in k=1 starts on the tick that rolls a
window over (tick 541: elapsed reaches 60 s, and `last_rate` = 0.33 C/min). The
refusal therefore comes from the *partial* window (19.7 C over 59 s, about 20 C/min).
In k=3 both checks refuse. Removing the completed-window check (R1) or requiring two
completed windows (R2) passes every test.

This check is the only refuser in the first `S8_POST_MIN_S` seconds after a rollover,
when the partial check is skipped and the rise is in the window that just completed.
Without it, a clear in that 5 s gap falls back to the diluted whole-hold average,
which is the original REVIEW_SAFTYFX3 LOW-1 hole.

Fix: add a case that puts the rise inside one window, clears it 1-4 s after the
rollover with a flat reading, and expects a refusal. Add a second case that clears
it again one quiet window later and expects a grant.

### LOW-2: nothing tests that a kiln cooling faster than the limit can clear

The fix-status text and the code comment both say that a kiln which warmed and then
cools "still clears". In k=1 the "cooling" is a single step from 40 C to 30 C. That
step lands at the 601 rollover, so the completed window reads +9.67 C/min, and every
later window reads 0. No window or partial window in the test has a negative rate.
Making either check use `fabsf` (R3 on the partial check, R4 on the completed check)
would refuse every kiln cooling at more than `max_rate` (15 C/min at the config
floor). That is a plausible deadlock while a hot kiln cools quickly with the lid
cracked, and the suite passes with it.

Fix: in a late-rise case, ramp down at more than 15 C/min over a full window and
part of the next, and assert that the clear is granted in both the completed and the
partial state.

### LOW-3: the fix record for LOW-3 is not reproducible as written, and the work it describes is not in this commit

- The ESP seeding (`s_test_trip_request_id_seeded`, `hal_sysinfo_random_u32()` under
  `xact_lock`) landed in 57c4e0454 (TEST_TRIP WP3 review fixes, another agent).
  9ba887351 changes no KilnFW driver source. REVIEW_SAFTYFX3's Fix status says "FIXED
  on the ESP side only: `safety_link_send_test_trip` lazily seeds...", which reads as
  if this change made the fix. It only added the test. The seeding itself was kept
  intact through the rebase: the code at 9ba887351 matches 57c4e0454.
- The LOW-3b mutation in the appendix searches for
  `static bool s_test_trip_request_id_seeded; /* guarded by xact_lock */`. The source
  line has no trailing comment (safety_link_commands.c:1015), so negtest would refuse
  that mutation as not found. The cited CAUGHT line, `test_safety_link_compile.c:3150`,
  is `tt_set_reply()`'s opening brace, not an assertion. The real assertion is line
  3197. Both details look like they came from a pre-rebase copy of the fixer's own
  seeding.
- Re-run with a corrected search string (K1), the mutation is CAUGHT at :3197, so the
  test is meaningful against the landed code. K3 (seed removed) is also CAUGHT.

Fix: correct the Fix status row and the appendix JSON in REVIEW_SAFTYFX3.

### INFO-1: once-per-boot seeding is not pinned (K2 MISSED)

Replacing `if (!s_test_trip_request_id_seeded)` with `if (1)` reseeds from the RNG on
every call, and the test still passes, because it checks only the first id. With a
reseed on every call, consecutive ids collide 1 time in 256. The Pico then answers
DUPLICATE and replays the earlier outcome. Fix: also assert that the second call's id
is `0x42` (the scripted seed is consumed only once).

### INFO-2: phase dependence and boundary behaviour of the post windows (questions 1 and 2)

- **Bounded refusal.** The completed-window refusal lasts at most one window,
  `s8_post_win_s`, which is clamped to 60-600 s at the first tripped tick. The partial
  check refuses only while the current window's own rate is over the limit. A kiln
  warming below `max_rate` is never refused. A kiln warming above it with K4 open is a
  real condition (S8 itself would trip on it), so refusing indefinitely is correct.
- **Noise.** The partial check is most sensitive at exactly `S8_POST_MIN_S`. The
  divisor is 1/12 min, so at the 15 C/min config floor a 1.25 C difference between two
  single samples refuses. That is well above MAX31856 read noise, and a retry a few
  seconds later dilutes it. One glitch sample at a rollover can make a completed window
  read over the rate. That refuses for at most one window, which is stricter than S8
  itself (S8 needs two windows). The strict RATE branch is stricter still: it requires
  two quiet windows. This is consistent with the existing design, not a deadlock.
- **Window alignment.** The windows are tumbling, not sliding, despite the comment's
  "Windows slide". A burst of up to 2x `max_rate` that lasts one window and straddles a
  boundary evenly reads at or under the limit in both windows, so it is granted. S8's
  own trip path has the same resolution and would not trip on such a burst either: it
  needs two consecutive windows. So this does not weaken the TEST clear relative to the
  guard it stands in for. A 4.9 s burst right after a rollover is seen only by the
  whole-hold average. That is the same `S8_POST_MIN_S` floor the strict branch has.
- **Timing a clear.** The post-window phase starts on the first tripped tick after the
  latch, so the ESP can roughly predict it. Exploiting either gap needs a temperature
  rise with K4 open, the same precondition as REVIEW_SAFTYFX3 LOW-1.

### INFO-3: answers to the remaining questions

- **Q3 (only refusals?).** Yes. In the tick, the `SAFETY_TRIP_TEST` term only adds post
  tracking under a TEST latch. The RATE behaviour is byte-identical, and the strict
  branch is untouched. The new block can only `return true`.
- **Q4 (interactions).** TEST latches only on the not-tripped path, and the tripped path
  never re-trips as RATE. Any grant goes through `safety_guards_clear()` (a memset)
  before the retest tick. So TEST-latch `s8_post_*` state can never reach a strict RATE
  check, and the reverse is also true. S9 escalation rewrites the reason to INEFFECTIVE,
  which stops tracking, and INEFFECTIVE is never cleared. The other guards in
  `test_trip_masks_real_condition()` run before `s8_clear_refuse`, and none of them
  reads `s8_post_*`. Only `safety_guards.c` reads it.
- **Q5 (hold tests moved to 40 C).** No coverage is lost overall. The hold reading
  feeds only the new post windows, because S8 is frozen while latched. Without the
  move, the 20-to-40 C jump would have made the post check refuse the "condition gone"
  grants. Redundancy did shrink. In `test_test_trip_clear_refuses_real_conditions`
  k=4, `test_test_clear_s8_tiny_window` k=1 and `test_test_clear_s8_divisor` case 4,
  the refusal is now also produced by the post partial window. Disabling the frozen
  whole-hold check (R7) is caught only by `test_test_clear_s8_divisor` case 2
  (:4950). Disabling the streak-1 check (R6) is still caught by the streak test
  (:4981). Both are CAUGHT, but each now has a single sentinel.
- **Q6 (boot heap log).** Safe. It runs in `main()` before `vTaskStartScheduler()`,
  after `console_uart_init()` (line 215), and the very next line already uses the same
  blocking console. `snprintf` was already used on this path (line 425). The 96 B buffer
  is on the main stack and the formatted text is at most about 93 bytes; `snprintf`
  truncates anyway. The added blocking write takes about 8 ms at 115200 baud, inside
  the 1 s watchdog that was enabled at line 320. Two caveats, both INFO: at this point
  `min-ever free` always equals `free` (nothing has been freed yet), and with
  `configKERNEL_PROVIDED_STATIC_MEMORY` the idle and timer tasks do not come from the
  heap, so the number does cover every task. The console header GP16/GP17 is not
  fitted on the bench (main.c comment), so the line is unreadable there without a
  probe.
- **Stale comment.** The `s8_clear_refuse()` header still says "strict=false ... no
  post state exists". After this change it does exist.

## Fix status

| Finding | Status |
|---|---|
| LOW-1 | FIXED. `test_test_clear_s8_post_window_checks` (test_safety_guards.c): 590 s flat pre-latch dilutes the whole-hold average, +15 C inside the first 600 s post window, clear 2 s after the rollover refuses (only the completed-window check can refuse), and one quiet window later grants. R1 CAUGHT :5083, R2 CAUGHT :5083. |
| LOW-2 | FIXED. Same function: cooling at -20 C/min (limit 10) clears in the completed state (2 s after rollover) and the partial state (12 s after). R3 CAUGHT :5125 (partial), R4 CAUGHT :5125 (completed and partial). |
| LOW-3 | FIXED (doc only). REVIEW_SAFTYFX3's LOW-3 row now says the seeding landed in 57c4e0454; its LOW-3b mutation uses the real find string and cites :3197. |
| INFO-1 | FIXED. `test_send_test_trip` asserts the second `request_id` is 0x42. K2 CAUGHT test_safety_link_compile.c:3209. |
| INFO-2 | FIXED (comment only). The `safety_guards.c` comment now says the windows are tumbling and names the two gaps (2x burst straddling a boundary; first `S8_POST_MIN_S` after a rollover). Algorithm unchanged. **Owner question:** should the TEST clear use sliding or overlapping post windows (for example a half-window stride) to close the boundary gap? Recommendation: no. S8's own trip has the same resolution (two consecutive windows), so the clear is no weaker than the guard it stands in for, and a change would add state to a safety path for no gain in protection. |
| Stale comment | FIXED. `s8_clear_refuse()` header no longer says "no post state exists". |

Negative tests, all CAUGHT, `real_tree_unchanged: true`: R1-R4 via `-Preset saftyfw-host` (base aae788e6b), K2 via `-Preset kilnfw-host`. R1/R4 search strings carry trailing context to be unique.

## Appendix: negative tests (this review)

Base 9ba887351, worktree `C:\wt\sfx4r_xy45nc`. Both runs reported
`real_tree_unchanged: true` and `copies_removed: true`.

SaftyFW, `tools\negtest.ps1 -Preset saftyfw-host -ExpectPattern 'FAIL .*\.c:\d+|SAFTYFW HOST TESTS: FAILED'`, baseline PASS:

```json
[
{"name":"R0a (re-run L1-a): post-window refusal disabled","file":"firmware/SaftyFW/src/safety_guards.c","find":"if (state->s8_post_active) {","replace":"if (0 && state->s8_post_active) {"},
{"name":"R0b (re-run L1-b): TEST latch no longer tracks post windows","file":"firmware/SaftyFW/src/safety_guards.c","find":"state->reason == SAFETY_TRIP_RATE || state->reason == SAFETY_TRIP_TEST","replace":"state->reason == SAFETY_TRIP_RATE"},
{"name":"R1: completed post-window check removed (partial only)","file":"firmware/SaftyFW/src/safety_guards.c","find":"state->s8_post_last_rate_c_per_min > cfg->max_rate_c_per_min) {","replace":"0) {"},
{"name":"R2: completed check needs 2 windows done","file":"firmware/SaftyFW/src/safety_guards.c","find":"state->s8_post_windows_done >= 1u &&","replace":"state->s8_post_windows_done >= 2u &&"},
{"name":"R3: partial check on |delta| (cooling refuses)","file":"firmware/SaftyFW/src/safety_guards.c","find":"if (((in->tc_c - state->s8_post_start_c) / pm) > cfg->max_rate_c_per_min) {","replace":"if ((fabsf(in->tc_c - state->s8_post_start_c) / pm) > cfg->max_rate_c_per_min) {"},
{"name":"R4: completed check on |rate| (cooling refuses)","file":"firmware/SaftyFW/src/safety_guards.c","find":"state->s8_post_last_rate_c_per_min > cfg->max_rate_c_per_min) {","replace":"fabsf(state->s8_post_last_rate_c_per_min) > cfg->max_rate_c_per_min) {"},
{"name":"R5: partial divisor diluted by one extra minute","file":"firmware/SaftyFW/src/safety_guards.c","find":"float pm = state->s8_post_elapsed_s / 60.0f;","replace":"float pm = (state->s8_post_elapsed_s + 60.0f) / 60.0f;"},
{"name":"R6: non-strict streak-1 rise check removed","file":"firmware/SaftyFW/src/safety_guards.c","find":"if (state->s8_over_rate_streak >= 1u && state->s8_window_active &&","replace":"if (0 && state->s8_over_rate_streak >= 1u && state->s8_window_active &&"},
{"name":"R7: frozen whole-hold window never refuses","file":"firmware/SaftyFW/src/safety_guards.c","find":"return ((in->tc_c - state->s8_window_start_c) / elapsed_min) > cfg->max_rate_c_per_min;","replace":"return ((in->tc_c - state->s8_window_start_c) / elapsed_min) > 1.0e9f;"}
]
```

| Mutation | Verdict | First failing assertion |
|---|---|---|
| R0a | CAUGHT | test_safety_guards.c:5028 late 20 C/min rise refuses |
| R0b | CAUGHT | test_safety_guards.c:5028 |
| R1 | **MISSED** | (LOW-1) |
| R2 | **MISSED** | (LOW-1) |
| R3 | **MISSED** | (LOW-2) |
| R4 | **MISSED** | (LOW-2) |
| R5 | CAUGHT | test_safety_guards.c:5028 |
| R6 | CAUGHT | test_safety_guards.c:4981 any rise since the slide with streak 1 refuses |
| R7 | CAUGHT | test_safety_guards.c:4950 1 C over the 5 s floor is 12 C/min: refuse |

Logs: `%TEMP%\negtest_logs\` (copy `negtest_w3tna5`).

KilnFW, `tools\negtest.ps1 -Command 'powershell -NoProfile -ExecutionPolicy Bypass -File firmware\KilnFW\App\test\build_host_tests.ps1 -OutDir "{OUT}" -Only test_safety_link_compile' -ExpectPattern '(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES'`, baseline PASS:

```json
[
{"name":"K1 (corrected LOW-3b): seeded flag preset true","file":"firmware/KilnFW/App/drivers/safety/safety_link_commands.c","find":"static bool s_test_trip_request_id_seeded;","replace":"static bool s_test_trip_request_id_seeded = true;"},
{"name":"K2: reseed from RNG on every call","file":"firmware/KilnFW/App/drivers/safety/safety_link_commands.c","find":"if (!s_test_trip_request_id_seeded) {","replace":"if (1) {"},
{"name":"K3 (re-run LOW-3): seed removed","file":"firmware/KilnFW/App/drivers/safety/safety_link_commands.c","find":"s_test_trip_request_id = (uint8_t)hal_sysinfo_random_u32();","replace":"s_test_trip_request_id = 0u;"}
]
```

| Mutation | Verdict | First failing assertion |
|---|---|---|
| K1 | CAUGHT | test_safety_link_compile.c:3197 first request_id is the RNG seed + 1 |
| K2 | **MISSED** | (INFO-1) |
| K3 | CAUGHT | test_safety_link_compile.c:3197 |

Logs: `%TEMP%\negtest_logs\20261012_013219_2je7`.

Not re-run: the fixer's L1-c, L1-d, LOW-2 P1 and INFO P2. R5 covers the partial-window
divisor that L1-c targets.
