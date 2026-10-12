# Review: safetyfx10 (e02630df4, 61a68d24f) -- REVIEW_SAFETYFX9 MED-1, LOW-1, LOW-2, I-1..I-3

Reviewer: opus, 2026-10-12. Worktree `C:\wt\rvsafetyfx10_mvbjhd` at origin/dev `61a68d24f`.

Scope:
- `e02630df4` orders TRIP_EVENT against DIAG by arrival, using the new `trip_event_after_diag` flag, instead of comparing two wrapping ages. It also puts the event reason into the watchdog fault, makes the stale-DIAG log fire once per segment and once per run, and adds tests.
- `61a68d24f` adds a test that run start re-arms the stale-DIAG log.

Verdict: **0 HIGH, 0 MED, 2 LOW, 7 INFO.**
- The arrival-order fix is sound and fails toward OFF.
- The 49.7-day wrap from REVIEW_SAFETYFX9 MED-1 is gone.
- No path was found where a real trip is suppressed or misattributed because of this change.
- One LOW is a pre-existing reboot-ordering window that this design inherits. The other LOW is an untested reason substitution.

## Evidence

**Negative tests.** The command was `tools\negtest.ps1 -Preset kilnfw-host -Mutations <12 mutants> -Parallel 4 -ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"`, run at base `61a68d24f`.
- The unmutated baseline passed (exit 0, 1227 s).
- The run reported `real_tree_unchanged: true` and `copies_removed: true`.
- All 13 runs log `Built: 99/99 executables` and none contains `BUILD FAILURES`. Every CAUGHT mutant compiled and failed a real assertion.
- Logs: `%TEMP%\negtest_logs\20261012_130754_6gq3`.

| Mutant | Change | Verdict | Failing assertion(s) |
|---|---|---|---|
| M1 | `safety_apply_trip_event` no longer sets `trip_event_after_diag` (`safety_link_frames.c:1403`) | CAUGHT | `test_safety_link_compile.c:3880` event after DIAG sets flag; `:3882` resend keeps it; `:3892` same tick DIAG then event |
| M2 | `safety_apply_diag` no longer clears it (`safety_link_frames.c:1177`) | CAUGHT | `test_safety_link_compile.c:3884` DIAG clears; `:3889` event then DIAG; `:3911` aliased ages |
| M3 | reboot note no longer clears it (`safety_link_frames.c:273`) | CAUGHT | `test_safety_link_compile.c:3897` reboot clears flag with `trip_event_ever_received` |
| M4 | predicate back to `trip_event_age_ms <= diag_frame_age_ms` (`relay_authority.h:362`) | CAUGHT | `test_profile_executor_prestart.c:14055`, `:14060`, `:14063`, `:14070` |
| M5 | predicate drops the `trip_event_after_diag` branch | CAUGHT | `test_profile_executor_prestart.c:14053`, `:14067`, `:14070`, `:14084` (aux dropped on event) |
| M6 | watchdog reason not substituted (`profile_executor.c:2350`) | **MISSED** | none (LOW-2) |
| M7 | `safety_elapsed_ms` back to a 32-bit product (`safety_link.c:210`) | **MISSED** | none (INFO-1) |
| M8 | run start does not re-arm the stale log (`profile_executor_run.c:734`) | CAUGHT | `test_profile_executor_prestart.c:13993` run start re-armed the warnings |
| M9 | segment mask uses one global bit (`profile_executor_relay_io.c:699`) | CAUGHT | `test_profile_executor_prestart.c:13977`, `:13983`, `:13990` |
| M10 | segment call site passes `0` instead of `idx` (`profile_executor_relay_io.c:1298`) | **MISSED** | none (INFO-4) |
| M11 | fresh read does not re-arm the segment mask (`profile_executor_relay_io.c:731`) | CAUGHT | `test_profile_executor_prestart.c:13987` after a fresh read segment 0 logs again |
| M12 | `stale_log_once_aux` does not record the bit (`profile_executor_relay_io.c:693`) | CAUGHT | `test_profile_executor_prestart.c:13980` aux 0 does not log twice |

M6, M7 and M10 were expected to be MISSED; they are reported as findings below. The three missed mutants cover a fault label, a 49.7-day saturation and a log line. None of them controls whether a relay is driven.

## Arrival-order correctness

**Single consumer.** Every frame goes through one FIFO inbox drained by one consumer (`safety_link_inbox.c:280-317`). The order in which the frame handlers run is therefore the order in which the frames arrived. A boolean set by TRIP_EVENT (`safety_link_frames.c:1403`, on every copy, resends included) and cleared by DIAG (`:1177`, unconditionally after the lock, with no early return in between) correctly answers "did a TRIP_EVENT arrive after the last DIAG?". No age arithmetic is involved, so nothing wraps.

**Pico reboot.** Each of the three reboot detectors calls `safety_note_pico_reboot_locked()`:
- DIAG uptime regression (`safety_link_frames.c:1120-1170`)
- V3 DIAG boot_id (`safety_link_frames.c:1120-1170`)
- FW_VERSION boot_id (`safety_link_frames.c:405-440`)

That function clears `trip_event_after_diag` together with `trip_event_ever_received`, `trip_last_seq` and `diag_since_reboot` (`:270-273`). The pair is reset on both sides, so this is not an instance of the "reset one side of a pair" class. The new field is covered by M3.

**ESP link reset.** Link-down handling (`safety_link.c:304-330`) touches neither the cached trip flags nor the DIAG flags. This is consistent: the flag and both `*_ever_received` bits keep describing the same last-known frames.

**boot_id change.** This takes the same `safety_note_pico_reboot_locked()` path as above. The FW_VERSION path skips the note inside the uptime-path note window (`:428-437`), so one reboot is never counted twice. That leaves the flag as the DIAG path left it, which was already cleared.

**Predicate** (`relay_authority.h:350-366`). It returns tripped if the latest DIAG says TRIPPED, or if a TRIP_EVENT exists and either no DIAG has ever arrived or the event arrived after the last DIAG. A newer ARMED DIAG supersedes an older event. That matches the Pico's own send order: DIAG is sent before `link_task_poll_trip_event` in the same loop iteration (`SaftyFW/src/tasks/link_task.c` ~3518/3559), so a DIAG that reports TRIPPED reaches the ESP first and then the event lands after it. Neither order loses a trip.

## Findings

### LOW-1 (pre-existing, inherited): a late FW_VERSION boot_id note erases a new boot's TRIP_EVENT

`safety_link_frames.c:405-440` and `:270-273`.

**Sequence.** The Pico reboots and copy 1 of the FW_VERSION burst is lost. The new boot trips early and sends a TRIP_EVENT. Copy 2 of FW_VERSION arrives after that TRIP_EVENT and before the new boot's first DIAG. The uptime-path note window does not apply, because no new-boot DIAG has arrived yet.

**Result.** `safety_note_pico_reboot_locked()` clears `trip_event_ever_received` and `trip_event_after_diag`. Until the next DIAG (at most about 2 s), the predicate reads the old boot's `diag_state`, which is likely ARMED, so it reports not tripped.
- The watchdog (`profile_executor.c:2341`) does not fault for that window.
- New ONs are still refused, because `diag_since_reboot` is false and `relay_authority_pico_diag_fresh` fails.
- K4 is opened by the Pico in hardware either way.

The next DIAG reports TRIPPED and restores the verdict.

**Impact.** This is a short delay in fault attribution, with no unsafe relay state. It predates safetyfx10, which cleared `trip_event_ever_received` on this path already.

**Suggested fix.** Only a frame known to come from the new boot should clear a TRIP_EVENT on reboot. One option: tag the cached event with the boot_id it arrived under, and have the reboot note keep an event whose boot_id matches the new boot.

### LOW-2: the LOW-1 reason substitution in the watchdog fault is untested

`profile_executor.c:2344-2351`.

When the predicate trips on a TRIP_EVENT while `diag_state` is not TRIPPED, the watchdog input now carries `safety_status.trip_reason` (`:2350`, used at `:2443` and `:2495`) instead of the DIAG reason, which is "none". Mutant M6 (`(void)0;` in place of the assignment) was **MISSED**, so no test pins this.

**Impact.** The label only, not whether the fault fires.

**Suggested fix.** Add a prestart test: set `trip_event_ever_received`, `trip_event_after_diag` and `trip_reason = <non-zero>` with `diag_state` ARMED, run a tick, and assert that the watchdog fault reason equals `trip_reason`.

### INFO

1. **`safety_elapsed_ms` saturation is untested.** The function (`safety_link.c:210-220`) now computes a 64-bit product and saturates at `UINT32_MAX`. M7 (back to 32-bit) was MISSED. Every consumer is a freshness or staleness comparison, so saturating is the correct direction. A unit test with `since` far in the past would pin it.

2. **The "aliasing" test does not model a real wrap.** The aliasing section of `test_safetyfx10_trip_event_after_diag_is_arrival_order` (`test_safety_link_compile.c:~3900-3911`) stamps both frames at tick 0 and reads at tick 5. That shows equal ages, not ages wrapped past 2^32 ms.
   - This is harmless, because the predicate no longer reads ages and M4 is caught by the prestart tests.
   - The test name overstates what it covers.

3. **The segment mask has no compile-time bound.** `s_stale_logged_seg_mask` is a `uint16_t` (`profile_executor_relay_io.c:685`), and the only bound is a comment. `PROFILE_MAX_SEGMENTS` is 12 today (`persist/profiles_types.h:32`).
   - Raising it past 16 would make `1u << idx` overflow the mask silently. The result would be a missed log only.
   - Suggested fix: `_Static_assert(PROFILE_MAX_SEGMENTS <= 16, ...)`.

4. **The segment call site's use of `idx` is untested.** At `profile_executor_relay_io.c:1298`, M10 (`stale_log_once_seg(0)`) was MISSED. The helper is tested directly and the call site is not. This affects logging only.

5. **A late TRIP_EVENT resend sets the flag again after a clear.**
   - Cause: the Pico's `s_trip_repeats_pending` is not cleared when a trip is cleared, so a resend copy that arrives after a clearing DIAG sets the flag again.
   - Effect: the predicate reads tripped until the next DIAG, at most about 2 s.
   - This fails toward OFF and matches the behaviour before the fix. It is noted for completeness.

6. **The stale-log masks are read and written from two tasks without a lock.** The run-start task (`profile_executor_run.c:734`) and the executor tick (`profile_executor_relay_io.c:720-731`) both touch them. The worst case is one log line too many or too few.

7. **A comment is stale.** `safety_link.c:700-701` still says `trip_event_age_ms` is "left at whatever safety_elapsed_ms(0) works out to". The next line now sets it to 0 explicitly when no event was received.

REVIEW_SAFETYFX9 I-4 remains deferred, as the fix commit states. I-5 was a note and needs no action.

## Are the tests behavioural?

**Yes, for the parts that decide relay state:**
- `test_safetyfx10_trip_event_after_diag_is_arrival_order` drives the real `safety_apply_trip_event`, `safety_apply_diag` and `safety_note_pico_reboot_locked`.
- The prestart predicate tests exercise `relay_authority_pico_tripped` and the aux fault-drop path.
- The stale-log tests call the real helpers, plus `aux_test_start_run` for the run-start re-arm.

**Gaps:** the watchdog reason (LOW-2), saturation (INFO-1) and the call-site index (INFO-4).
