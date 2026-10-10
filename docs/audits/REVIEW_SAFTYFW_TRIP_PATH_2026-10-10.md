# SaftyFW trip path review, 2026-10-10

Scope: the RP2040 trip path end to end on `origin/dev` at `8eba89bd2`.
This covers the guards (`safety_guards.c`), the input build and trip/clear
plumbing in `safety_core.c`, the relay cut path (`relay_owner.c`,
`relay_grace.c`), the latch and `CLEAR_TRIP` decision (`link_frame.c`), the
`config_store` read and write paths, the watchdog (`watchdog_task.c`,
`watchdog_gate.c`), the boot reason that feeds the ESP's benign/fatal
Pico-reboot decision (`boot_reason.c`, `link_task.c` DIAG,
`heat_enable.c`), and the context frame parser. The review was read-only;
no code was changed.

It does not repeat findings that the reviews earlier today already own:
`SAFTYFW_GUARD_REVIEW_2026-10-09` (F1-F7, fixed),
`REVIEW_SAFTYFW_GUARD_FIXES`/`FIX2`/`FIX3`,
`REVIEW_SAFTYFW_SINCE_PRE2_2026-10-10`, `SAFETY_LINK_REVIEW_2026-10-09`
(F6, CLEAR_TRIP has no boot identity, is still deferred to the owner) and
`REVIEW_SAFETY_LINK_FIX2_2026-10-10`.

Owner rule respected: no finding proposes disabling or blinding a guard.
Every recommendation makes a guard stricter or keeps its state.

## Summary

| ID | Severity | Finding |
|----|----------|---------|
| T1 | MED | An S1 (absolute overtemp) latch, and an S8 (rate) latch, can be cleared while the condition still holds. `GUARD_TEST_MATRIX.md` says such a clear is refused, and a host test asserts that it is accepted. |
| T2 | LOW | One failed `config_store_get_full_record()` swaps the default record into the guard config for that tick. That resets the S1 streak and the S2, S8 and S14 accumulators. |
| T3 | LOW | The Pico's trip latch does not survive a Pico reboot. The scratch-register trip reason is read at boot and then discarded. |
| T4 | INFO | S12 (enclosure) is skipped on every bad TC read, even when the cold junction is still valid. |
| T5 | INFO | `CLAUDE.md` still calls the RP2040 `config_store` atomicity defect "unfixed". It was fixed and bench-verified on 2026-09-14. |
| T6 | INFO | Paths checked and found sound: the relay cut, the watchdog, timer wrap, the trip mask, the context parser, and the fatal boot classification. |

## T1 (MED): an S1/S8 latch can be cleared while the condition still holds

`safety_guards_try_clear()` (`safety_guards.c:249`) first asks
`guard_condition_still_immediate()` (`:184`). That check covers S5, S2, S3,
S11, S12, S13 and S6b. Next, `safety_guards_clear()` zeroes all guard state,
and then the function re-ticks once. S1 and S8 are not in the immediate
check. The comment at `:172` gives the reason: "it is a 3-tick debounce with
no single-tick ... test short of the trip condition itself". After the
zeroing, the single re-tick only brings the S1 streak to 1, so the clear is
**ACCEPTED** while `tc_c` is still above `abs_max_temp_c`. `relay_owner`
returns to ARMED, and S1 re-trips about 300 ms later. This happens again on
every clear. S8 behaves the same way, except that its window state is zeroed
too, so it needs two full `rate_window_s` windows (120 s by default) before
it can re-trip.

The rationale is weak. S2 sits in the same list and uses its own
single-tick level condition. For S1, the single-tick test is simply
`cfg->abs_max_temp_c > 0 && in->tc_valid && in->tc_c > cfg->abs_max_temp_c`.
That is the level check that S1's debounce counts. Refusing a clear while
the check holds makes the guard stricter and blinds nothing.

There is a documentation and test contradiction:

- `firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md:119` says "All | `CLEAR_TRIP` while condition is still true | **Refused**".
- `GUARD_TEST_MATRIX.md:583-584` says that every guard's "CLEAR_TRIP-refused-while-still-true" behaviour is covered by `test_try_clear`.
- `test/test_safety_guards.c:3371` asserts the opposite for S1: "scope limit: a single retick does not rebuild S1's 3-tick streak, clear holds".

Impact is bounded today on the ESP side. A fresh TRIPPED diag puts the run
into FAULTED and releases the relay claim (`profile_executor.c`, the
`safety_processor_tripped` watchdog path). So an accepted clear does not by
itself re-request heat. The window is still a real ARMED period granted
against an ongoing overtemp. It cycles the trip/clear state machine. It also
shows operators a "cleared" state while the kiln is still above its absolute
ceiling. An S1 trip is the case where this matters most, because a kiln
above its ceiling cools slowly, so a clear attempt while still hot is the
expected operator action.

Recommendation (never a weakening):

- Add `SAFETY_TRIP_OVERTEMP` to `guard_condition_still_immediate()` with the
  level test above.
- For S8, refuse while the pre-clear state shows the last completed window
  over rate, or while the current partial window already extrapolates over
  rate.
- Change the `test_safety_guards.c:3347-3383` block to assert refusal.
- Correct the matrix line 583 claim.

Negative test (below, mutation A): adding the S1 level test is CAUGHT by the
suite, which shows that the current suite pins the accept behaviour. A fix
must update that test deliberately.

## T2 (LOW): a failed config read swaps defaults into the guard config for that tick

`safety_core.c:1067` gets `cfg_read_ok = config_store_get_full_record(&cfg_rec)`.
On failure (the primary seqlock and the fallback double buffer are both
exhausted), `cfg_rec` is `config_store_default()`, with `fields_set == 0`.
Then `safety_core_load_guard_cfg(&cfg_rec)` at `:1114` still runs
unconditionally. For that tick:

- `abs_max_temp_c = 0`, so S1 is inert and its streak is reset to 0 (`safety_guards.c:645` block).
- `max_rate_c_per_min = 0`, so the S8 window and streak are reset.
- `tc_placement_valid = false`, so the S2 `s2_over_elapsed_s` is zeroed. That can be up to `overshoot_time_s` (120 s default) of accumulation, lost.
- `i_normal_valid[]` is false, so the S14 accumulators reset. `tc_source` reverts to OWN_J7, so S13 goes inactive.
- `safety_tc_installed = 0`, so the S5 promotion to TRIP is suppressed for that tick (its accumulators keep running).

The 2026-09-14 review's Finding C already established that a failed read
means "could not confirm", not "uncommissioned". That finding was applied
only to the item-16 backstop (`:1144-1180`), not to the guards. The failure
needs a config writer burst that outlasts `CONFIG_STORE_SEQLOCK_MAX_RETRIES`
(4) plus the fallback, so it is rare. Each occurrence restarts the S2, S8
and S14 windows, which delays those trips. The direction is fail-late, not
fail-safe.

Recommendation: on `!cfg_read_ok`, keep the last good `s_guard_cfg` (do not
reload). A board that never had a good read still starts from the defaults
that `safety_core_init` loads. Negative test (mutation B): this change is
MISSED by the whole SaftyFW host suite, so no test pins how the guards
behave after a failed config read.

## T3 (LOW): the trip latch does not survive a Pico reboot

`boot_reason_latch_trip()` writes the trip reason to watchdog scratch[0]/[1]
on every trip. `main.c:335-348` reads it at boot, then discards it
(`(void)boot_reason;`, a Phase 8 TODO), and clears it. Guard and relay state
live in RAM, so any Pico reset comes up untripped: GRACE for 60 s, then
ARMED. That includes S9 `TRIP_INEFFECTIVE`, which is otherwise never
operator-clearable. Guards that need longer than the 60 s grace to re-arm do
not catch a still-present condition before ARMED: S2 needs 120 s and S8
needs 2 windows.

Mitigations in place today:

- A watchdog reset sets the DIAG `BOOT_WATCHDOG` bit, which `heat_enable.c`
  classifies as fatal (hold).
- A trip seen by the ESP while a firing runs puts that firing into FAULTED
  and releases the claim, so the owner's "benign reboot auto-resumes heat"
  path (`heat_enable_note_pico_boot`) has no claim left to resume.

The residual window is a non-watchdog reset (debugger, SYSRESETREQ, RUN)
that lands between a trip and the ESP's next fresh DIAG.

Recommendation: report `trip_reason_valid`/`trip_reason` in DIAG, or make
the ESP treat "rebooted while my last DIAG said TRIPPED" as fatal. Do not
re-latch blindly on the Pico. The magic word survives only watchdog/soft
resets, so a re-latch would turn every soft reset into an
operator-clearable trip.

## T4 (INFO): S12 is skipped on every bad TC read

A bad read runs `goto context_guards` (`safety_guards.c:637`), which skips
S1, S11, S12 and S8. S12 (`:699`) checks the cold junction and has its own
`cj_invalid`/`isnan(cj_c)` gate. On the most common fault, an open TC, the
MAX31856 cold junction is still valid, so the enclosure guard is blind for
no reason. The blindness is bounded: an installed TC trips S5 after
`blind_grace_s` (60 s), and a declared-not-installed board cannot get
heating enabled (`safety_core_request_enable` refuses). Recommendation for
later: run S12 before the S5 early exit whenever `cj_valid`. That would
un-blind S12 without touching S5.

## T5 (INFO): documentation drift about config_store atomicity

`CLAUDE.md` ("Zones config ... dual-write" paragraph) still says "a real
unfixed atomicity defect in the RP2040's own config store". The defect (D2,
torn-slot reuse) was fixed on 2026-09-14 and bench-verified
(`docs/CONFIG_FILESYSTEM.md:403-450`,
`docs/audits/rp2040_config_store_write_atomicity_2026-09-14.md`). The code
on dev confirms the fix:

- `config_store_flash.c:1560` refuses a non-erased target slot.
- `:1348-1366` re-reads and compares after every program.
- `seqlock_read` has a bounded primary plus a fallback double buffer.

The `CLAUDE.md` sentence should be updated. This review does not edit
`CLAUDE.md`.

## T6 (INFO): paths checked and found sound

- **Relay cut.** `SAFTYFW_PIN_RELAY` is written only in `main.c:205-207` (output latch low before the pin becomes an output) and in `relay_owner.c`. ENERGIZE is honoured only in ARMED; every other state drives low. TRIP drives low before it latches TRIPPED. Trip and clear go through owed-retry latches, and a trip wins over a pending clear.
- **Timer wrap.** The relay grace uses 32-bit tick subtraction. At the 49.7-day wrap, a clear inside the 60 s window after the wrap goes to GRACE instead of ARMED, which is the safe direction. The watchdog deadline math uses unsigned tick differences and is wrap-safe.
- **Watchdog.** The feed is gated on every check-in being within its deadline (`watchdog_gate.c`). Deadlines are 3x the task period, capped at 700 ms. The task runs on the trip-path core.
- **Trip mask.** `link_frame_trip_mask_for_reason()` is `1 << (reason - 1)`, and NONE is 0. `link_frame_decide_clear_trip()` refuses NONE, refuses INEFFECTIVE, refuses a mask mismatch, and refuses a frame without `trip_seq` from a peer whose protocol version supports it. Mutation C shows the mask check is covered.
- **Context parser.** It enforces the length and `zone_count <= CONTEXT_SNAPSHOT_MAX_ZONES`, with an exact length match. Non-finite setpoints and measurements are filtered in `context_reduce_zones()` (snapshots.h, F5). `borrowed_zone_index` is bounded 0..2 at config-param validation before it indexes the S13 arrays.
- **Boot classification.** Every deliberate Pico reboot path (`hal_wdt_reboot` = `watchdog_reboot`) and the fatal hooks (stack overflow, malloc, assert) land as WATCHDOG or fatal-kind bits. The ESP holds on those. A hard fault locks up and is reset by the watchdog, so it also reports WATCHDOG. POWERON means "not a watchdog reset", which the ESP treats as benign per the owner decision of 2026-10-10. See T3 for the trip-latch residual.

## Negative tests

All runs used `tools\negtest.ps1 -Preset saftyfw-host -Mutations <json>`,
from a throwaway copy at `8eba89bd2`. `-RequireAssertion` could not be used:
the script refuses it together with any preset ("-RequireAssertion cannot be
combined with -ExpectPattern or a -Preset"). It also would not match
SaftyFW's `FAIL <file>:<line>` lines. The preset's own verdict pattern
(`SAFTYFW HOST TESTS: FAILED`, never `BUILD FAILED`) was therefore the
assertion criterion.

| Mutation | File | Change | Verdict | Meaning |
|----------|------|--------|---------|---------|
| A | `safety_guards.c` | Add `SAFETY_TRIP_OVERTEMP` level test to `guard_condition_still_immediate()` | CAUGHT (`saftyfw_host_tests`, `safety_core_host_tests` fail) | The suite pins the T1 accept-while-over-ceiling behaviour. A T1 fix must change those tests on purpose. |
| B | `safety_core.c` | `if (cfg_read_ok) { safety_core_load_guard_cfg(&cfg_rec); }` | MISSED (suite passes) | No test covers guard behaviour after a failed config read (T2 coverage gap). |
| C | `link_frame.c` | Mask check narrowed to `wire_trip_mask != current_mask && wire_trip_mask == 0xFFFFu` | CAUGHT (`saftyfw_host_tests` fails) | The CLEAR_TRIP mask check is covered. |

The first try at mutation C (`if (false && ...)`) did not compile: MSVC
C4100/C4189 are warnings-as-errors. It was replaced by the form above, and
that run passed its baseline and left the real tree unchanged. In the first
run, the "real tree changed" error came from this audit doc being created in
the worktree while the run was in progress. It was not caused by a mutation.

## Coverage gaps against GUARD_TEST_MATRIX.md

- Matrix row 119 and the claim at lines 583-584 are wrong for S1 and S8 (T1).
- No test covers a failed `config_store_get_full_record()` during a guard tick (T2, mutation B).
- No test covers a Pico reboot while TRIPPED, as seen by the ESP's benign/fatal classification (T3).
