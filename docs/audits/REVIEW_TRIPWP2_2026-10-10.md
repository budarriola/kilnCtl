# Review: TEST_TRIP WP2 (SaftyFW), a364d1703 / 103fbcec4 / 1c07873ee

Date: 2026-10-10 (written 2026-10-11). Reviewer: Claude (Opus), adversarial
review. No code was changed. Scope: the origin/dev commits a364d1703 (code and
tests), 103fbcec4 (docs) and 1c07873ee (link_task stack `*10 -> *11`, ceiling
re-pins), read at origin/dev 628fa7c1a against `docs/TEST_TRIP_PLAN.md` and
`docs/audits/REVIEW_TRIPWP1_2026-10-10.md`.

## Summary

The core wiring is sound:
- The test trip is the last check in `safety_guards_tick()`'s not-tripped
  path. Every earlier branch returns with a real trip, so on the same tick a
  real guard always wins. While a trip is latched, a test request is ignored.
- It goes through the real `trip()` latch. Nothing in WP2 clears a trip,
  masks a trip reason or skips a guard tick.
- The test-trip decision is pure and checks, in order: magic, peer >= 18,
  boot_id, dedup, update busy, already tripped, then the 10 s rate limit. The
  unsigned subtraction handles tick wrap.
- The F6 clear rule is pure and its order is correct: a frame carrying a
  boot_id must match; a peer >= 18 that sends no boot_id is refused; an
  unknown peer gets a 30 s grace.

Three things are wrong:
- **HIGH-1.** WP1-review HIGH-1 is now real on dev. A dev ESP announces 18
  but still builds only the 4-byte CLEAR_TRIP and parses only 30/31-byte DIAG.
  A WP2 Pico refuses every clear from it and sends it 32-byte DIAG it drops.
- **MED-1.** Clearing a test trip re-tests only the S9 window. A real guard
  that was mid-debounce when the test trip latched is never evaluated while
  latched. A clear can be granted while that real condition still holds, and
  its reason is never recorded.
- **MED-2.** Negtests show the new source scans miss four of five plausible
  regressions.

| ID | Grade | One line |
|---|---|---|
| HIGH-1 | HIGH | Dev ESP (announces 18, sends 4-byte clears, parses 30/31-byte DIAG) and WP2 Pico: every trip is unclearable, every DIAG is dropped |
| MED-1 | MED | TEST clear re-tests only `s9_verify_elapsed_s`; a real guard mid-debounce is masked, and a clear is granted while its condition holds |
| MED-2 | MED | Source-scan and guard tests miss 4 of 5 mutations: F6 grace refresh, reply poll, request-on-refusal, test trip placed before S6a |
| LOW-1 | LOW | ACCEPTED reply attributes any new trip_seq to the test trip without checking `reason == SAFETY_TRIP_TEST` |
| LOW-2 | LOW | DUPLICATE reply sends outcome DUPLICATE and the current seq, not the original outcome and seq (code comment says otherwise) |
| LOW-3 | LOW | Queue-full after an ACCEPTED decision replies REFUSED_ALREADY_TRIPPED but the dedup/rate state already counts it as accepted |
| LOW-4 | LOW | "K4 proven open" is the Pico's commanded relay state, not a readback |
| LOW-5 | LOW | Plan section 4.4's host test pinning the reboot classification of a pre-reboot test trip is missing |
| LOW-6 | LOW | +1 KB link_task stack comes out of the unchanged 56 KB FreeRTOS heap with no headroom check |
| INFO-1..7 | INFO | trip_verify_s unset/large, SET_CONFIG mid-trip, downgrade to 17, dedup across ESP reboot, ms wrap, v17 compatibility, DIAG size, stack arithmetic |

## HIGH-1: dev ESP plus WP2 Pico cannot clear any trip, and drops every DIAG

Files:
- `firmware/SaftyFW/src/tasks/link_frame.c:307-308`
- `firmware/KilnFW/App/drivers/safety/safety_link_commands.c:144-151`
- `firmware/KilnFW/App/drivers/safety/safety_link_frames.c:1092`
- `firmware/KilnFW/App/drivers/safety/safety_link_frames.c:176`

`KILNLINK_PROTOCOL_VERSION` is 18 since WP1, so a KilnFW built from dev
announces 18 (`safety_link_frames.c:176`). WP3 has not landed. The ESP's
CLEAR_TRIP builder fills only `trip_mask`, `has_trip_seq` and `trip_seq`, and
never sets `has_boot_id`, so it always sends the 4-byte form. The WP2 Pico's
`link_frame_decide_clear_trip_v3()` refuses a frame with no boot_id from a
peer >= 18: `LINK_CLEAR_TRIP_REFUSE_BOOT_ID_REQUIRED`, at `link_frame.c:308`.

In the other direction, `send_diag` now sets `has_boot_id` for a peer >= 18
and sends 32 bytes. The ESP accepts only `SAFETY_LINK_DIAG_FRAME_LEN` or
`_V2` (`safety_link_frames.c:1092`), so it logs "unexpected DIAG frame" and
drops every one. It then loses the trip_seq and the link counters DIAG
carries.

Failure scenario:
1. Flash dev KilnFW. It embeds the dev SaftyFW slot images and auto-updates
   the Pico at boot.
2. The expected dual-reflash S6a latches.
3. `safety_clear_trip()` is refused every time with BOOT_ID_REQUIRED.
4. The bench stays tripped until WP3 lands.

This fails safe (heat stays denied), but it blocks the bench. It also applies
to any trip latched after such a flash, including real trips that the
operator has resolved.

Recommendation:
- Land WP3 (the 5-byte clear and the 32-byte DIAG parse) before any bench
  flash of a dev tip that contains WP2.
- Until then, record "dev = WP2 without WP3: trips unclearable, do not
  flash" on the ROADMAP TEST_TRIP row.
- Do not loosen the Pico rule to accept 4-byte clears from peers >= 18;
  that rule is F6's point.

## MED-1: a test trip can mask a real guard that is mid-debounce

Files:
- `firmware/SaftyFW/src/safety_guards.c:187-194` (`guard_condition_still_immediate`, `case SAFETY_TRIP_TEST`)
- `firmware/SaftyFW/src/safety_guards.c:1326-1334`

Once the test trip latches, the tick runs only the tripped path (S8-post and
S9). S1's over-ceiling streak, the S8 window, S3's `stuck_on_time_s`, S5 and
the other debounced guards are not evaluated. The clear test for
`SAFETY_TRIP_TEST` returns only
`s9_verify_elapsed_s < effective_f(trip_verify_s, 10 s)`. It does not ask
whether any other guard's immediate condition holds.

Failure scenario:
1. The kiln sits just over `abs_max_temp_c`. S1 has a streak of 2 of 3
   ticks.
2. An admin fires a test trip. The test trip latches with reason TEST, and
   the S1 streak is no longer advanced.
3. After 10 s with K4 commanded off, the operator clears. The clear is
   granted because the TEST hold has elapsed.
4. The accumulators restart from zero, so S1 needs a full fresh debounce
   before it trips. The over-ceiling condition existed the whole time but
   was never recorded as the trip reason, and the event log shows only
   "admin test trip".

With relays off for at least 10 s, temperature has usually dropped. But the
same shape applies to conditions that stay true with heat off: S12 (if the
CT reads current with K4 open), S5 sensor-invalid, and S16. This violates the
owner rule that a test trip may "never mask/blind" a real one.

Recommendation: for `SAFETY_TRIP_TEST`, also OR in the immediate conditions
the other `guard_condition_still_immediate()` cases already compute (S1 over
ceiling, S5 sensor invalid, S12 current with K4 open, and so on). Then a clear
is refused while any real condition is live, and the next tick after the clear
latches the real reason. Add a guard test with S1 over the ceiling at
streak 2 and a TEST latch, which must refuse the clear.

## MED-2: test coverage gaps, confirmed by negtest

Each mutation was run with `tools/negtest.ps1 -Preset saftyfw-host`, with
`-ExpectPattern "FAIL .*\.c:\d+|SAFTYFW HOST TESTS: FAILED"`, in a throwaway
worktree at 628fa7c1a. The baseline passed.

| # | Mutation | Verdict |
|---|---|---|
| a | delete the `s_last_peer_known_ms = link_task_now_ms();` refresh in the `link_task_fn` loop (`link_task.c:3554`) | MISSED |
| b | delete the `link_task_poll_test_trip_reply();` call in the loop (`link_task.c:3557`) | MISSED |
| c | add `(void)safety_core_request_test_trip();` on the refusal path (`link_task.c:1677`) | MISSED |
| d | duplicate the test-trip block right after S16, before S6a (`safety_guards.c:609`) | MISSED |
| e | control: the TEST case of `guard_condition_still_immediate` returns `false` | CAUGHT (`test_safety_guards.c:4728`, `:4735`, `:4756`) |

negtest's final verdict line reads ERROR with `real_tree_unchanged:false`.
That is only because this review doc was written into the worktree while
the run was going. `git status` afterward showed this file and nothing else.
The baseline passed, and each per-mutation verdict above is valid. Logs:
`%TEMP%\negtest_logs\20261011_220635_q9oo`.

What each MISSED mutation means in practice:

- **(a) F6 grace refresh.** `link_task.c:3554` refreshes
  `s_last_peer_known_ms` every loop while the peer version is known. Without
  it, the timer holds its boot value. After a known-to-unknown transition
  late in uptime (an ESP reboot, a session change), `peer_unknown_ms` is
  already far above 30 s. A legacy unbound 4-byte clear from the unknown peer
  is then accepted at once, with no ANNOUNCE wait. This defeats F6. The source
  scan checks only that `link_task_peer_unknown_ms()` reads `version != 0u`;
  it does not check the refresh in the loop or the init in
  `link_task_start` (`:3595`).
- **(b) reply poll.** `link_task.c:3557`. Without the call, the Pico never
  sends TEST_TRIP_RESULT. The trip still latches, but the ESP sees only a
  timeout.
- **(c) request on refusal.** The scan checks only that decide runs before
  request (strstr order) and that the ACCEPTED `if` string exists. A request
  added on the refusal path (rate-limited, updating, wrong boot_id) passes
  every test. That path would latch a test trip the decision refused, which
  bypasses the rate limit and the boot_id binding.
- **(d) placement before S6a.** `test_admin_test_trip`'s same-tick check uses
  only `config_integrity_trip`, the very first check in the tick. Moving the
  test-trip block to just after S16 passes, even though a same-tick S6a/S7,
  S1 or S2 would then lose its reason to TEST. Same-tick cases with S6a, S1
  and one late guard would pin "last".

Recommendation:
- Make (a) a behavioural test: run `link_task_peer_unknown_ms()` across a
  known-to-unknown transition at t >> 30 s, or factor the refresh into a pure
  helper.
- Pin (b) with a scan of `link_task_fn`'s loop body, not the whole file.
- For (c), assert that `safety_core_request_test_trip` appears exactly once
  in the handler, and only inside the ACCEPTED block.
- For (d), add same-tick cases with a late guard.

## LOW-1: ACCEPTED reply attributes any trip_seq change to the test trip

File: `firmware/SaftyFW/src/tasks/link_task.c:1683-1696`

`link_task_poll_test_trip_reply()` reads `reason` and then ignores it. It
replies ACCEPTED with the first `seq != base`.

Failure scenario:
1. A real guard trips between the handler's decision and safety_core's next
   drain. The queued request is then dropped, because the tick is already
   tripped.
2. The reply still reports ACCEPTED with the real trip's seq.
3. The ESP/MCP records the real trip as "your test trip".

A later clear by that seq clears the real trip under a test label. The clear
is still subject to the real reason's own hold, so this does not mask the
trip, but the record is misleading. Recommendation: require
`reason == SAFETY_TRIP_TEST` for ACCEPTED. Otherwise reply
REFUSED_ALREADY_TRIPPED with the seq.

## LOW-2: DUPLICATE reply differs from the plan and from its own comment

File: `firmware/SaftyFW/src/tasks/link_task.c:1651-1658`

The comment says "answer the SAME way again (ACCEPTED, with the seq if
known)". The code arms outcome DUPLICATE with the current trip_seq. If
`trip_verify_s` is under 10 s, the test trip can be cleared and a real trip
latched inside the dedup window. A retried request then gets DUPLICATE plus
the real trip's seq. Recommendation: store the original outcome and seq with
the dedup state and replay them, or fix the comment and the plan.

## LOW-3: queue-busy after ACCEPTED leaves dedup and rate state charged

File: `firmware/SaftyFW/src/tasks/link_task.c:1660-1668`

`link_frame_decide_test_trip()` records `last_accept_ms` and
`last_request_id` before the handler tries `safety_core_request_test_trip()`.
If the queue send fails, the reply is REFUSED_ALREADY_TRIPPED, which is
false. A retry with the same id within 10 s gets DUPLICATE, and a new id gets
RATE_LIMIT. No trip happens either way, so this fails safe, but the operator
sees misleading outcomes. Recommendation: commit the dedup and rate state only
after a successful enqueue, or use a distinct BUSY outcome.

## LOW-4: "K4 proven open" is the commanded state

Files:
- `firmware/SaftyFW/src/tasks/safety_core.c:1424` (`.relay_deenergized = !relay_owner_is_energized()`)
- `firmware/SaftyFW/src/tasks/relay_owner.c:283`

`s9_verify_elapsed_s` advances on the Pico's own `s_energized` mirror, not on
contact feedback. If the K4 contacts are welded, the TEST hold releases after
`trip_verify_s` anyway. Only S9's current check could catch it, and on a board
without a CT, or uncommissioned, S9 only warns. This matches how S9 already
behaves, so it is not a regression, but the plan's "K4 proven open" overstates
it. Recommendation: reword the plan and the code comment to "K4 commanded open
for trip_verify_s; S9 had its chance to escalate".

## LOW-5: missing reboot-classification test

`docs/TEST_TRIP_PLAN.md` section 4.4 says WP2 adds a host test pinning how a
test trip latched before a Pico reboot is classified on the next boot (the
boot_reason latch). No such test exists: there is no `SAFETY_TRIP_TEST`
reference outside `safety_guards.c`, `link_task.c`, `safety_core.c`,
`test_test_trip.c` and `test_safety_guards.c`. Under the owner decision that
"benign reboot auto-resumes heat", the case to pin is whether a test-trip
latch counts as a fault or as benign.

## LOW-6: stack bump against an unchanged heap

Files:
- `firmware/SaftyFW/src/tasks/link_task.c:174`
- `firmware/SaftyFW/FreeRTOSConfig.h:113`

`LINK_TASK_STACK_WORDS` is now `configMINIMAL_STACK_SIZE * 11`, which is
11264 B. That covers the new 10304 B ceiling (5152 B measured × 2). The extra
1024 B comes out of the 56 KB `configTOTAL_HEAP_SIZE`, which was not changed.
No check reports free heap after task creation. The measurement is static
objdump with unresolved `regsp` frames, so the 2x margin is the only
protection. Recommendation: have the stack-budget check, or a boot log line,
report `xPortGetMinimumEverFreeHeapSize()` so the next bump is not blind.

## INFO

- **INFO-1, trip_verify_s unset or 0.** `effective_f` falls back to 10 s, so
  the hold is never zero. The maximum stored value is 65535 s (a u16 param),
  which holds a test trip for up to 18 h. That fails safe and only an admin
  can set it.
- **INFO-2, SET_CONFIG mid-trip.** Lowering `trip_verify_s` while a test trip
  is latched shortens the hold. This matches S9 and needs admin access.
- **INFO-3, downgrade by announcing 17.** This needs an ANNOUNCE frame that
  an 18 ESP never sends. The link is unauthenticated by owner decision, so
  anyone who can inject frames can already do worse.
- **INFO-4, dedup across an ESP reboot.** Dedup state is not reset on an ESP
  session change. A request_id reused within 10 s after an ESP reboot gets
  DUPLICATE and no trip. That fails safe, and the MCP read-back catches it.
  This is a "reset one side of a pair" shape, but harmless here.
- **INFO-5, ms wrap.** The 32-bit ms counter wraps at 49.7 days. The
  unsigned-difference comparisons handle the wrap; the worst case is one
  misjudged 10 s window.
- **INFO-6, v17 ESP compatibility.**
  - A peer at 17 gets the 31-byte DIAG.
  - A 4-byte clear from a known peer at 17 is accepted.
  - After a Pico reboot the version is unknown until the ESP re-announces.
    During that time a 4-byte clear is refused for up to 30 s. That is
    stricter than the plan text, which names only the 3-byte form for this
    rule; fine, but the plan should say so.
  - The recovery image announces 16 and never sends CLEAR_TRIP.
- **INFO-7, sizes.** DIAG V3 is 32 B, well under the 253 B maximum payload.
  Stack arithmetic: 11264 B ≥ 10304 B. The safety_core ceiling of 2216 B
  (from 2208 B) is consistent with the added 0-tick `xQueueReceive`.

## Fix status

Fixed in 05c6832c1 (fix) and 150d14c6a (tests), reviewed in
`REVIEW_SAFTYFX_2026-10-11.md`.

| Finding | Status |
|---|---|
| HIGH-1 | Tracked by TEST_TRIP WP3 (KilnFW side); not part of this fix. |
| MED-1 | Fixed. A TEST clear refuses while any real guard's immediate condition holds; S6a/S7/S16 re-trip in the retest tick. |
| MED-2 | Fixed. New behavioural tests plus source scans; F6 refresh still pinned by a proximity scan only. |
| LOW-1 | Partly fixed. The first reply is correct; a DUPLICATE retry after a real guard won replays ACCEPTED with the real seq (REVIEW_SAFTYFX LOW-1). |
| LOW-2 | Partly fixed. DUPLICATE now re-sends a seq, but always as ACCEPTED, and its source scan is vacuous (REVIEW_SAFTYFX LOW-1, LOW-2). |
| LOW-3 | Fixed. Queue busy restores the dedup/rate state and replies REFUSED_RATE_LIMIT. |
| LOW-4 | Fixed (wording in `docs/TEST_TRIP_PLAN.md`). |
| LOW-5 | Fixed. `test_admin_test_trip_boot_reason_latch`. |
| LOW-6 | Not addressed. |
