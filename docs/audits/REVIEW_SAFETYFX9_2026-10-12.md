# Review: safetyfx9 (580a848ac) -- REVIEW_SAFETYFX8 LOW-1..3, I-1, I-2

Reviewer: opus, 2026-10-12. Worktree `C:\wt\rvsafetyfx9_4hs3pk` at origin/dev `46b3d02cc` (contains `580a848ac`).

Scope: `580a848ac`, which touched
`firmware/KilnFW/App/drivers/control/profile_executor_relay_io.c`,
`firmware/KilnFW/App/drivers/owners/relay_authority.h`,
`firmware/KilnFW/App/test/test_profile_executor_prestart.c` and
`docs/audits/REVIEW_SAFETYFX8_2026-10-12.md`.

Verdict: **1 MED, 2 LOW, 6 INFO.** The fixes do what they claim and fail toward OFF. The MED is a new
spurious-trip path that the LOW-3 fix opened: it compares two millisecond ages that wrap every
2^32 ms (49.7 days).

## Evidence

- **Host suite on the dev tip** (the fixer did not re-run it after the rebase):
  `build_host_tests.ps1 -OutDir C:\wt\rvsafetyfx9_hostout` -> `Built: 99/99 executables`,
  `all 99 host test executables built and passed`. `test_profile_executor_prestart`: `6046/6046 checks passed`.
  All three new SAFETYFX8 sections ran.
- **Negative tests:** `tools\negtest.ps1 -Preset kilnfw-host -Mutations <8 mutants>
  -ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"`. The table is below.
  Mutants ran in 4 parallel copies at `46b3d02cc` with `-NoBaseline`. The unmutated baseline had already
  passed at the same commit (279 s) in an earlier run, which was killed externally during its second mutant.
  The full host suite also passed at this commit (99/99). The run reported `real_tree_unchanged: true` and
  `copies_removed: true`. Every CAUGHT mutant compiled and failed a real assertion. None failed to build.

  | Mutant | Change | Verdict | Failing assertion(s) (`test_profile_executor_prestart.c`) |
  |---|---|---|---|
  | M1 | `pico_diag_stale_for_on`: ESP_FAIL returns `false` | CAUGHT | 13985 aux ON refused; 13988 segment driven OFF, wants_on kept; 13991 retry skipped |
  | M2 | aux tick: `stale_hold = false` | CAUGHT | 14002 not commanded/actuated on; 14004 rule_reason names the denial |
  | M3 | `else if (authority_blocked)` (stale hold not in the rule_reason branch) | CAUGHT | 14004 rule_reason names the denial |
  | M4 | drop the TRIP_EVENT branch in `relay_authority_pico_tripped` | CAUGHT | 14022 event newer than DIAG; 14026 no DIAG ever; 14036 drops an on aux |
  | M5 | no-DIAG-ever arm forced false | CAUGHT | 14026 trip event with no DIAG ever: tripped |
  | M6 | diag age `+ 1000000u` (event sticks over a newer DIAG) | CAUGHT | 14024 newer ARMED DIAG supersedes the event |
  | M7 | `<=` -> `<` (equal ages) | MISSED | none; boundary untested (I-2) |
  | M8 | drop `s_stale_logged_aux_mask \|= mask;` | MISSED | none; log-once not asserted (expected for a log line) |
- **Stack:** not measured on a target ELF. No ELF built from `580a848ac` exists under `C:\wt`
  (no `KilnCtrl.elf` contains the new `s_stale_logged_seg` symbol), and this review may not run a target build.
  The static argument comes from the pre-fix ELF `C:\wt\checkbuild_280d180149` (53131caf1):
  - `pico_diag_stale_for_on` is out of line (local symbol `t`, `entry a1, 240`).
  - `aux_apply_relay_pico` is `entry a1, 80`.
  - `profile_executor_aux_tick_pico` is `entry a1, 176`.

  The chain that already existed was aux_tick_pico -> aux_apply_relay_pico -> pico_diag_stale_for_on,
  which is 176+80+240 = 496 B. The new direct call is aux_tick_pico -> pico_diag_stale_for_on, which is
  176+240 = 416 B, so it is shallower. The new locals (`stale_hold`, `blocked_i`) are two bools in a frame that
  already exists. `relay_authority_pico_tripped()` gained no locals. No added depth is expected, unless GCC newly
  inlines `pico_diag_stale_for_on` (now 4 call sites, was 3), which is unlikely for a 240-B frame.
  `check_executor_task_stack_budget.ps1` will measure the real ELF on the coordinator's next full run.

## Claim-by-claim

| Claim | Verified | Notes |
|---|---|---|
| LOW-1: link bound, status read ESP_FAIL -> aux ON refused, segment ON driven OFF with wants_on kept, retry skipped | Yes | New section 1 of `test_safetyfx9_...`. Mutant M1 (`return true` -> `return false`) CAUGHT. |
| LOW-2: stale DIAG blocks an aux not already on (actuated_on false, rule_reason AUTHORITY); already-on aux held | Yes | `stale_hold = !commanded_on && pico_diag_stale_for_on()` is ORed into the trigger's `failsafe_base`. An aux that is already on is never stale-held. The already-on hold is pinned by the existing SAFETYFX7 LOW-1 test. M2 and M3 CAUGHT. |
| LOW-3: TRIP_EVENT newer than the last DIAG (or no DIAG ever) -> tripped | Yes, with MED-1 and LOW-1 below | M4, M5 and M6 CAUGHT. The equality boundary (M7) is untested (INFO-2). |
| I-1: comment | Yes | Accurate. The new LOW-2 call ORs into `authority_blocked`, so a NULL link still blocks through `relay_authority_on_blocked(NULL)`. |
| I-2: log once per stale episode | Yes | It re-arms on a fresh read or on no link. It is not tested (M8 MISSED, as expected for a log). See INFO-3. |

## Callers of `relay_authority_pico_tripped()` (all fail toward OFF)

- `owners/relay_authority.h:395` `relay_authority_start_blocked()` refuses firing, autotune and resume starts
  (`profile_executor_run.c:415,1441`, `profile_executor_status.c:369`, `autotune_engine.c:1102`).
- `control/profile_executor.c:744,754` `pre_lock_pico_tripped` feeds `profile_executor_aux_fault_drop()`
  (aux dropped even while IDLE), `io_seg_start_pico()` and `profile_executor_aux_tick_pico()`.
- `control/profile_executor.c:2093` gates the segment retry.
- `control/profile_executor.c:2341`: the watchdog sets `safety_processor_tripped`. A RUNNING or PAUSED firing goes
  to FAULTED on a single sample (`profile_executor.h:223`).

Nothing in the trip-clear flow (`safety_link_send_clear_trip`, the LCD and web clear) reads this predicate, so it
cannot block a clear.

**Latching:** the result cannot latch permanently.
- A newer DIAG of any state supersedes the event. The Pico sends DIAG every `LINK_DIAG_TX_PERIOD_MS` = 2000 ms
  (`SaftyFW/src/tasks/link_task.c:214`).
- A Pico reboot clears `trip_event_ever_received` (`safety_link_frames.c:272`).
- With DIAG genuinely absent, the event keeps the predicate true. That is correct, because DIAG staleness already
  refuses every ON in that state.

## Findings

### MED-1: `trip_event_age_ms` wraps every 49.7 days, so a long-past trip periodically reads as a fresh one and aborts a healthy firing

- **Where:**
  - `owners/relay_authority.h:359-360`: `st->trip_event_age_ms <= st->diag_frame_age_ms`.
  - `safety/safety_link.c:210-217` `safety_elapsed_ms()`: `(uint32_t)delta * portTICK_PERIOD_MS`.
  - `safety/safety_link.c:700-702`.
- **What:** `safety_elapsed_ms()` returns `(true_age mod 2^32) ms`, because the multiply by `portTICK_PERIOD_MS`
  (10 at the default 100 Hz) is a 32-bit multiply. The function's own comment ("~497 days at the default 100 Hz")
  is about the tick counter wrapping. The millisecond product wraps 10x sooner, at 2^32 ms, about 49.7 days.
  - `trip_event_tick` is refreshed only by the 4-copy, 750 ms TRIP_EVENT burst (`link_task.c:231-243`). After a
    trip is cleared it is never refreshed again, until the next trip or a Pico reboot.
  - `diag_tick` is refreshed every 2 s.
  - So, 2^32 ms after the last TRIP_EVENT, the reported `trip_event_age_ms` drops to about 0 while
    `diag_frame_age_ms` is d0 in [0, 2000). The two ages then advance together until the next DIAG arrives. For
    2000-d0 ms (mean about 1 s) `relay_authority_pico_tripped()` returns true on a board whose DIAG says ARMED.
  - The watchdog samples every `WATCHDOG_CHECK_PERIOD_MS` = 2000 ms (`profile_executor.c:82`). That gives roughly
    a 50% chance per wrap that a RUNNING firing is FAULTED, with the reason "safety processor tripped: none, firing
    aborted". Every on aux is dropped (including while IDLE), and starts are refused with "the safety processor is
    tripped (clear the trip first)". The operator has nothing to clear.
  - This repeats every 49.7 days for as long as neither processor reboots. Before `580a848ac`,
    `trip_event_age_ms` was display only, so this path is new.
- **Preconditions:** at least one TRIP_EVENT since the last Pico boot (a single cleared S6a after a dual reset
  counts), then 49.7 days of continuous ESP and Pico uptime. A kiln controller left powered can reach this.
- **Safety:** none. It fails toward OFF. The impact is a ruined firing and a misleading reason.
- **Fix (pick one):**
  - Decide event-vs-DIAG order by arrival, not by subtracting ages. Keep a cached bool
    `trip_event_after_diag`: `safety_apply_trip_event()` sets it, `safety_apply_diag()` clears it, and a Pico reboot
    clears it. Expose it in `safety_link_status_t`. This is close to SAFETYFX8 LOW-3's second suggestion (set
    `diag_state = TRIPPED` from `safety_apply_trip_event()`), but it does not overwrite DIAG data.
  - Or compare the raw `TickType_t` stamps under the link lock (`(TickType_t)(trip_event_tick - diag_tick)` as a
    signed delta), which is wrap-safe as long as the two are less than 2^31 ticks apart. That bound is also crossed
    after a long quiet period, so the bool is the robust choice.
  - Either way, also fix `safety_elapsed_ms()`'s comment (or widen the product to 64 bit and saturate), because
    every other `*_age_ms` consumer inherits the same 49.7-day wrap.
  - Add a host test that sets `trip_event_age_ms` small with an ARMED DIAG received after the event. Today that case
    cannot be told apart from a genuine fresh event, which is the defect.

### LOW-1: a trip inferred from TRIP_EVENT reports reason "none" in the watchdog fault

- **Where:** `control/profile_executor.c:2345,2438` passes `safety_status.diag_trip_reason` as the watchdog's
  `safety_trip_reason`; `profile_executor.h:229-232` renders it.
- **What:** when the event branch fires, the DIAG is by definition older and usually ARMED, so `diag_trip_reason` is
  0 ("0 = none", `safety_link.h:1036`) or a stale earlier reason. `fault_reason` becomes "safety processor tripped: none, firing aborted". That is exactly the LOW-3 scenario
  (DIAG lost, TRIP_EVENT received), where the event's own `st.trip_reason` (`safety_link.h:1085`) carries the real
  guard.
- **Fix:** when `diag_state != TRIPPED` but the predicate is true, use `st.trip_reason`. Better, have the predicate
  (or a sibling) return the reason it acted on.

### LOW-2: the fixer's comment claims the Pico repeats TRIP_EVENT "while latched"; it sends one 750 ms burst

- **Where:** `owners/relay_authority.h:356-358`.
- **What:** the Pico sends `LINK_TRIP_REPEAT_COUNT` = 4 copies, 250 ms apart, once per new `trip_seq`
  (`SaftyFW/src/tasks/link_task.c:231-243`). It does not repeat while latched. This matters for the reasoning:
  - **No auto-clear:** this follows from "no newer DIAG", not from repetition.
  - **Lost burst:** if all four copies are lost, the event branch never fires.
  - **Latch duration:** the event branch holds only until the next DIAG, at most about 2 s. After that only
    `diag_state` matters. If that DIAG is lost too, the predicate stays true, which is correct.
- **Fix:** correct the comment.

### INFO

- **I-1. Doc line endings.** `580a848ac` rewrote `REVIEW_SAFETYFX8_2026-10-12.md` from LF to CRLF (344-line diff
  for 5 changed lines), and it left 3 bare CRs where " -- FIXED (safetyfx9)" was appended to the LOW-1/2/3
  headings. The file now has 169 CRLF, 3 bare CR and 3 LF line ends. A renderer that treats a bare CR as a line
  break splits those headings. Normalise to LF.
- **I-2. Equality boundary untested.** Mutant M7 (`<=` -> `<`) survives. Equal ages happen when a TRIP_EVENT and a
  DIAG land in the same 10 ms tick. `<=` (tripped) is the fail-toward-OFF choice. A one-line check at equal ages
  would pin it.
- **I-3. Log-once is global for segments and not reset per run.**
  - `s_stale_logged_seg` is one flag for all segments, so a second segment refused in the same stale episode is
    silent.
  - Neither flag is reset at run start, so a stale episode that spans a run boundary hides the next run's first
    refusal until a fresh read re-arms it.
  - The re-arm happens only inside `pico_diag_stale_for_on()`, which every aux tick calls for each not-on aux, so it
    re-arms promptly in practice.
  - Logs only.
- **I-4. Second status read per aux per tick.** The aux tick now calls `pico_diag_stale_for_on()` (one
  `safety_link_get_status()` lock round) for every not-commanded-on aux, then `aux_apply_relay_pico()` reads it
  again on an ON request. The two reads can disagree across a DIAG arrival.
  - Stale then fresh: the trigger says OFF and the write is OFF.
  - Fresh then stale: the trigger says ON, the write is refused, and the old accounting mismatch reappears for one
    tick.

  Both outcomes fail toward OFF. Passing the first result into `aux_apply_relay_pico()` would make them consistent.
- **I-5. Stale-hold resets the aux trigger's hold timing.** Feeding staleness through `failsafe_base` takes the
  fail-safe override path (OFF, hold bypassed). After a stale episode, the aux may also wait out `min_off_s` before
  its first ON. The test settles twice and does not pin this. That matches how `authority_blocked` already behaves,
  so it is noted only.
- **I-6. Stack unmeasured.** See Evidence. There is no target ELF of `580a848ac`. The static call-graph argument
  says no added depth.
