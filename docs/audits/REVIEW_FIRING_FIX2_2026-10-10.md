# Review: firing-path fix batch (2026-10-10, second pass)

Reviewer: Opus safety review, read-only. No code was changed.

Commits reviewed on origin/dev: `1fe83614a`, `6de0fec2f`, `603f062c9`, `7c4624cc3`.
The scope comes from `docs/audits/REVIEW_FIRING_PATH_FIX_2026-10-10.md`: F1, F2, F3, F4, NaN/inf duty, MED-A, MED-B and the `profile_exec_wdt` stack ceiling.
Owner decision applied: a benign Pico reboot auto-resumes heat, and a fatal reboot reason pauses the run (`pico_fatal_reboot`).

Review tree: dev tip `c2121889b`. All line numbers refer to that tree.

## Verdict

- No new heat-on path was found.
- No new lock-order or deadlock risk was found. The lock order is s_exec, then the link lock, and s_exec, then the heat_enable lock. No reverse acquisition exists.
- The timeout paths fail safe: a guard 9 lock timeout leaves APP asserted and the relays cut, and a bounded-pause timeout leaves the next loop to retry.
- Two of the fixes have caveats: F4's run-start clear removes a safety retry, and MED-A does not cover an in-flight enable.
- Three of the four call sites the author listed as untested are still uncaught by any test. See the negative-test table below.

## Fix status (batch firefx3)

- MEDIUM-1 fixed in 462fe5623: run-start clear reverted; unowned pending OFF bits are retried while RUNNING every 1 s, never for relays owned by run zones or aux claims.
- LOW-1 fixed in 462fe5623: send_enable queues a release when reboot_hold is set while the send is in flight.
- LOW-2 fixed in 462fe5623: guard 9 merge and assert now precede relay_unknown_release_locked().
- LOW-3 fixed in 462fe5623: tests added; mutations M1, M2, M3, M4b all CAUGHT by negtest.
- LOW-5 note: bench zone configs still need a check for progress_window_s > wrong_dir_window_s (guard 1 latency); no code change.

## Findings (severity-ranked)

### MEDIUM-1: the F4 run-start clear drops an OFF retry that has not landed

Location: `firmware/KilnFW/App/drivers/control/profile_executor_run.c:721`, `s_exec.zone_off_pending_mask = 0;`.

A bit set in `zone_off_pending_mask` means a relay OFF write failed and the relay may still be closed. Clearing the mask at run start forgets that relay.

Before this commit, a stale bit cost one redundant OFF write. `zone_off_pending_retry()` (`profile_executor_relay_io.c:514`) already runs every non-RUNNING tick, including IDLE (`profile_executor.c:767`), and an OFF write to an open relay is harmless.

After this commit, if the relay is not in the new run's zone masks, nothing drives it OFF again:
- during the run, which re-requests K4 so heat can flow through a closed contact;
- after the run, either.

The sibling comment four lines below (`profile_executor_run.c:723`) keeps `aux_claim_mask` across run start for exactly this reason: "forgetting it would strand a closed relay". The F4 clear contradicts that rationale.

The owner check that F4 also added (`profile_executor_relay_io.c:523-530`) already stops the retry from writing to a relay another owner holds. That was the actual hazard, so the run-start clear is not needed to fix it.

Recommendation: revert the clear at line 721. Ownership is already handled at retry time.

Related, and pre-existing: pending bits are never retried while RUNNING, because the only call is in the non-RUNNING branch. A failed OFF on a relay outside the running zones therefore stays closed for the whole run, even without the clear.

Negative test: removing the clear is MISSED (M2). No test pins this line in either direction.

### LOW-1: MED-A does not cover an enable send already in flight

Location: `firmware/KilnFW/App/drivers/control/heat_enable.c:308` (`send_enable` sets `granted = true` on success) and `heat_enable.c:722-729` (the fatal branch of `note_pico_boot`).

The fatal branch sets `reboot_hold`, sets `release_pending` only when `granted || pending`, and then clears both flags. A REQUEST_ENABLE(true) issued by another task before the hold can complete afterwards. `send_enable` does not check `reboot_hold`, so `granted` becomes true under a hold that has no release queued.

The mitigation is partial:
- The `pico_fatal_reboot` pause releases the claim, but only if the executor is RUNNING and the bounded lock is obtained.
- An autotune run holding the claim is not paused by this path. That is a known gap.

Recommendation: re-check `reboot_hold` in `send_enable` after a successful send. If the hold is set, queue `release_pending` instead of setting `granted`.

### LOW-2: relay_unknown_release_locked() runs before the guard 9 merge and assert

Location: `firmware/KilnFW/App/drivers/control/profile_executor.c:2342` (release), `:2344` (merge) and `:2358` (assert). This is pre-existing, but F1 makes it more relevant.

On the loop after a guard 9 lock timeout, `guard9_prelock_check()` has asserted APP on the link. `relay_unknown_release_locked()` then recomputes the link fault source from `global_fault_source`, which does not yet contain APP, so APP can briefly drop. `guard9_assert_stale_tick_fault()` re-asserts it two statements later in the same locked section.

The relays stay cut throughout, so no heat path opens. The window is a few microseconds, but the link frame could be sent in that window.

Recommendation: move the merge and assert ahead of `relay_unknown_release_locked()`, or OR APP into `global_fault_source` in the prelock path.

### LOW-3: the F1 loop wiring, the status gate and the bounded lock wait are untested

Negative tests M1, M3 and M4b were MISSED:
- **M1:** the `guard9_merge_pending` call line in the watchdog loop (`profile_executor.c:2344`). The F1 test calls the helper directly; nothing exercises the loop.
- **M3:** the failed-status-read gate (`profile_executor.c:2494`, `if (safety_status_ok)`).
- **M4b:** the 1000 ms bound inside `profile_executor_pause_with_reason_bounded` (`profile_executor_status.c:347`). Replacing it with `portMAX_DELAY` is not caught.

M4a, which swaps the call site to the unbounded `profile_executor_pause_with_reason`, is CAUGHT, but only by a source-text scan (`test_heat_enable.c:1160`).

Recommendation: add source-pin or behavioral tests for these three lines.

### LOW-4 (informational): MED-B bounds only the lock wait

Location: `firmware/KilnFW/App/drivers/control/profile_executor_status.c:336` and `:347`.

Inside `pause_with_reason_impl`, the watchdog task still makes two calls after it gets the lock:
- `kiln_io_owner_command_set_relay_mask_authorized`, through `force_all_relays_off`. This is bounded and returns `ESP_ERR_TIMEOUT`.
- `run_state_note`, a flash write done outside the lock.

The worst case per watchdog loop is roughly 1 s for the guard 9 lock, 1 s for the pause lock and about 5.2 s for `heat_enable_reconcile`. A guard 9 timeout `continue` skips the pause on that loop, which is safe: the relays are already cut.

No action is needed. The cost is noted for the guard 9 period budget.

### LOW-5 (informational): F3 can loosen guard 1 latency for existing configs

Location: `firmware/KilnFW/App/drivers/control/thermal_guard.c:327-331`.

On the climbing branch, an explicit `progress_window_s` now takes precedence over `wrong_dir_window_s`, up to `ZONE_GUARD_TIME_S_MAX` (7200 s). For any zone where `progress_window_s > wrong_dir_window_s`, guard 1 now trips later than before.

Guard 1 detects a failure to heat, not an overheat, so this adds no heat-on risk. The change is intended. The bench zone configurations should be checked for that inequality.

### INFO-1: stack ceiling not re-measured

Location: `firmware/KilnFW/App/test/check_all_task_stack_budgets.py:1095`, where the ceiling goes from 2736 to 2752.

The task is created at 6144 B (`profile_executor_start.c:206`, registered at `:217`). This review could not re-measure the ceiling, because target builds are out of scope here. A bench high-water mark is still the honest confirmation.

## Verified correct

- **F2:** APP is asserted before `kiln_io_all_relays_off` in `guard9_prelock_check`, and the ordering is tested. The guard 9 pending flag is written and read only on the watchdog task, so there is no race.
- **F1:** `guard9_merge_pending` (`profile_executor.c:2163`) returns `tick_stale || pending`. M6 is CAUGHT.
- **MED-A:** the failed-status-read gate is logically correct, though untested (M3). `release_pending` is set on the fatal branch only when a claim is held (M7 CAUGHT), and reconcile skips while `reboot_hold` or `reboot_classify_pending` is set.
- **NaN/inf duty:** `heater_output.c:70` renders a non-finite duty as OFF. M8 is CAUGHT.
- **F4 fallback** (`profile_executor_relay_io.c:29-80`):
  - It excludes other zones' readable masks, `aux_claim_mask` (M10 CAUGHT) and active aux relays.
  - It refuses rather than writes when the fallback mask is empty.
  - The aux bit index matches the relay number (bit = relay - 1).
  - A mask is unreadable only when `zone_index >= thermo_count`. The system-mode gate blocks mid-run config changes, so this cannot happen during a run.
- **F4 retry owner check:** CAUGHT (M5).
- **F3 precedence:** CAUGHT (M9).

## Negative tests

Tool: `tools\negtest.ps1 -RequireAssertion -ExpectPattern 'RUN FAILURES \('`.

I did not use `-Preset kilnfw-host`. I passed `-Command` with `build_host_tests.ps1 -Only "profile_executor_prestart|heat_enable|thermal_guard|heater_output"` instead. These are the host-test executables that compile the mutated sources, so the result should match the full preset; the run is faster. The unmutated baseline passed. Base: `c2121889b`. The real tree was unchanged afterwards, and every temporary copy was removed.

| # | Mutation | Verdict |
|---|----------|---------|
| M1 | Watchdog-loop `guard9_merge_pending` call removed (`profile_executor.c:2344`) | MISSED |
| M2 | Run-start `zone_off_pending_mask = 0` removed (`profile_executor_run.c:721`) | MISSED |
| M3 | `if (safety_status_ok)` gate replaced by `if (true)` (`profile_executor.c:2494`) | MISSED |
| M4a | `pico_fatal_reboot` call site made unbounded (`profile_executor.c:2509`) | CAUGHT (source scan, `test_heat_enable.c:1160`) |
| M4b | Bounded implementation given `portMAX_DELAY` (`profile_executor_status.c:347`) | MISSED |
| M5 | Retry owner check disabled (`profile_executor_relay_io.c`) | CAUGHT (`test_profile_executor_prestart.c:13069`) |
| M6 | `guard9_merge_pending` drops the pending flag | CAUGHT |
| M7 | MED-A `release_pending = true` removed (`heat_enable.c`) | CAUGHT |
| M8 | `isfinite` removed from the duty clamp (`heater_output.c:70`) | CAUGHT |
| M9 | F3 old precedence restored (`thermal_guard.c`) | CAUGHT |
| M10 | Fallback `aux_claim_mask` exclusion removed (`profile_executor_relay_io.c:47`) | CAUGHT |

## Host-test count

Run: `build_host_tests.ps1`, full, on dev tip `c2121889b`.

Result: "Built: 86/86 executables, all 86 host test executables built and passed". `$totalExpected` is 86, and no executable is missing.

The author's 83/83 came from a run on a base taken before the rebase. Three executables landed concurrently from other work:

- `test_uart_bridge_core.c` (`bbb25f5de`)
- `test_ct_leak_alarm_service.c` (`ba267a692`)
- `test_dashboard_autotune_http_handlers.c` (`251f9b2e3`)

The script prints MISMATCH when the built count differs from `$totalExpected`, so a full run on the post-rebase tree would have flagged a shortfall.
