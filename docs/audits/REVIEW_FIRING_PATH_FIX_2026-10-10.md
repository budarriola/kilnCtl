# Review: firing-path fix batch (FIRING_PATH_REVIEW_2026-10-09 items 1-7)

Date: 2026-10-10. Reviewer: Claude (opus), review only, nothing fixed here.
Reviewed at origin/dev `6201932f5`.

Commits reviewed: `6c7b9eddb`, `3f39e0dac` (item 1, guard 9), `b5502c191`
(item 2), `a56e51e0e` (item 3), `4106c8eab` (item 4), `fa1ebe421` (item 5),
`9929fbc0a` (item 6), `74b7bd9dd`, `19fa986a8` (source-scan tests),
`6a5a06c01` (item 7, `profile_exec_wdt` ceiling 2704 -> 2720).

## Summary

No path found where a relay stays ON after a trip or stall: every new path
cuts relays first and the APP fault source then blocks re-assertion through
`relay_authority`. No new lock-order hazard. One medium regression in item 1
(a stall that ends inside a narrow window leaves the run RUNNING with APP
latched, not FAULTED). Item 6 is only partly fixed. Three stack budgets fail at
dev tip, none caused by this batch.

## Findings

### F1 (MEDIUM, new regression, item 1): lock-timeout pass can lose the FAULTED transition

`profile_executor.c` watchdog loop, around lines 2253-2297.

On a pass where `guard9_prelock_check()` finds the tick stale, it cuts relays,
asserts `SAFETY_FAULT_SRC_APP` and sets `s_guard9_bookkeeping_pending = true`.
If `guard9_take_lock_bounded()` then times out, the loop does `continue`.
On the next pass (2 s later), if the control task has recovered meanwhile,
`tick_stale` is false. The locked block still runs (because of `pending`), so
relays are cut again and APP is re-asserted, and `pending` is cleared. But
`wd_in.tick_stale = tick_stale;` passes only the fresh value, so
`profile_executor_wd_decide()` never classifies the stall. The run is never
moved to FAULTED and gets no `fault_reason` or run-state note.

Result: a run that reads RUNNING, with APP latched in `global_fault_source`
(only cleared by `clear_this_runs_faults()` at halt), so heat is blocked. It
fails safe for heat, but the operator gets no fault, and the run keeps ticking
segments with relays held off until someone stops it. Before the fix, the same
stall produced FAULTED.

Window: the control task holds the lock for more than 1 s past the stale
threshold, then frees it and updates `last_tick_tick` before the next
watchdog pass.

Fix direction: latch the stale verdict. Feed `tick_stale || pending` (with
the age recorded when `pending` was set) into `wd_in` before clearing
`pending`. No test covers this path. A behavioral test should drive
`guard9_prelock_check()` stale, then lock timeout, then recovery, and assert
FAULTED.

### F2 (LOW, item 1): order inside `guard9_prelock_check()`

It calls `kiln_io_all_relays_off()` first and only then
`safety_link_set_fault_source(APP, true)`. A control tick that is about to
resume can re-assert a relay ON between the two calls, because
`relay_authority` is not blocked yet. The window is microseconds and the
next pass cuts again, but asserting APP first and cutting second closes it.

The commit text says relays are cut "through kiln_io's own owner-task
serialisation". That is not accurate: `kiln_io_all_relays_off()`
(`kiln_io.c:369`) writes the SX1509 directly from the watchdog task. The chip
access is locked inside `sx1509_write_port_locked`, so this is safe. The
`relay_shadow` read-modify race with the owner task is pre-existing.

### F3 (LOW/MEDIUM, item 6 incomplete): explicit `progress_window_s` still overridden

`thermal_guard.c:326`:
`window_s = effective_f(cfg->wrong_dir_window_s, climbing ? effective_f(cfg->progress_window_s, PROGRESS_WINDOW_S) : WRONG_DIR_WINDOW_S);`

When the guard is climbing, a configured `wrong_dir_window_s` still wins over
an explicit `progress_window_s`. The new 120 s floor (lines 361-364) is
skipped whenever `progress_window_s > 0`. With `wrong_dir_window_s = 60` and
`progress_window_s = 300`, the climbing window is still 60 s. The residual
false trip the item describes remains for that configuration, and the claim
"explicit progress_window_s honoured" does not hold.

The test meant to prove that case sets both values to 60. It cannot tell
which value was used, so it is vacuous. It should use unequal values.

Does the floor loosen an existing guard? Only guard 1's climbing-window
latency, by design. For example, a 60 s window becomes 120 s, which is still
under the 300 s default. The non-climbing (wrong-direction) branch, the other
guards and the Pico ceiling are unchanged. Accepted as an intended trade.

### F4 (LOW, item 4): unreadable-mask fallback scope

- When the relay mask is unreadable, `apply_relay` writes OFF to all of
  `claimed_relay_mask`, including aux and the other zones' relays. That is
  safe, but while RUNNING it chatters every relay that should be ON. This is
  acceptable for a fault path. It is worth logging once per episode.
- When the fallback mask is 0, nothing is written and nothing is added to
  `zone_off_pending_mask`.
- `zone_off_pending_retry()` also runs in the not-RUNNING branch
  (`profile_executor.c:767`). After a run ends it can write OFF once to a
  relay that manual control or autotune has taken over since. This fails
  safe, but it is a one-shot surprise for that owner.
- `zone_off_pending_mask` is not cleared at run start. A stale bit from the
  previous run causes one extra OFF write.

### Items checked and found correct

- **Item 1, other aspects:**
  - Relays are cut before `s_exec.lock` is touched.
  - The lock take is bounded at 1000 ms.
  - `heat_enable_reconcile()` stays last. The lock-timeout `continue` skips
    `note_pico_state` and reconcile for that pass; both are retries, so this
    fails safe.
  - Lock nesting is reduced, and no new deadlock was found against the
    executor, save or claim locks.
  - No new false trips at startup, while paused or during autotune:
    `last_tick_tick` advances every tick in every state.
  - Pre-existing and unchanged: run-end persistence and the adaptive fit
    run outside the lock and could approach 10 s.
- **Race between the watchdog cut and the executor re-asserting ON:**
  - Covered by APP blocking `relay_authority_on_blocked` and
    `relay_authority_zone_blocked`, apart from the F2 window.
- **Item 2 (`b5502c191`):**
  - `thermal_guard_tick` now runs before `apply_relay`.
  - The authority is queried twice per tick. The two answers can differ by
    one tick, which is harmless because the second query blocks.
- **Item 3 (`a56e51e0e`):**
  - The ms carry is correct.
  - The 0 < dt <= 3600 s filter rejects NaN and negative values.
  - `dt` is computed from a wrap-safe tick difference.
  - The remainder resets at run start, at segment change and on resume, so
    no carry crosses a pause.
  - Cosmetic: the remainder-reset lines at 1138 and 1232 are mis-indented.
- **Item 5 (`fa1ebe421`):**
  - `if (!(duty > 0.0f)) duty = 0` renders NaN as OFF.
  - Positive infinity clamps to 1. That is ON at full duty, but it only
    happens if PID output reaches inf, and the existing guards still
    apply. Consider treating `!isfinite` as OFF.
- **Item 7 (`6a5a06c01`):** ceiling 2720 matches the measured value (below).

## Tests

Behavioral coverage:

- `guard9_prelock_check` stale/fresh (partial: F1's path is untested).
- Item 2 tick ordering.
- `exec_elapsed_accumulate`.
- `apply_relay` OFF-retry and unreadable-mask fallback.
- NaN and inf duty.

Source-scan tests (vacuity-prone):

- guard 9 call order.
- Acquire result not cast to `(void)`.
- `zone_off_pending_retry` call order.
- The clamp string.
- The item 6 60/60 case (F3).

These pass on any refactor that keeps the text and fail on any harmless
rewording. Prefer behavioral replacements.

Host tests at dev tip, using
`build_host_tests.ps1 -Only 'profile_executor_prestart|thermal_guard|heater_output|autotune_engine_prestart'`:
4/4 built and passed.

This needed local, uncommitted resolution of committed conflict markers
in `firmware/KilnFW/App/test/build_host_tests.ps1` around line 3300
(HEAD vs `3c9a3b1da`). **origin/dev carries unresolved conflict markers in
this file.** Without that resolution, `tools\negtest.ps1` cannot run: its
baseline fails with a PowerShell ParserError. The planned negtest
spot-check (NaN clamp `!(duty > 0.0f)` -> `duty < 0.0f`) was therefore not
run.

## Target build and stack budgets (dev tip `6201932f5`)

`check_00_kilnfw_target_build.ps1`: PASS.

| Task | Measured | Ceiling | Result |
|---|---|---|---|
| profile_exec_wdt | 2720 B (lower bound, INDETERMINATE) | 2720 | at ceiling |
| profile_executor (executor_task_entry) | 3408 B | 3408 | OK (LOW honest headroom, 1516 B) |
| kiln_io_owner | 2064 B | 1984 | FAIL |
| system_uart_bridge | 3152 B | 3136 | FAIL |
| http_async_job | 7712 B | 7632 | FAIL |
| uart_log_bridge | 2080 B | 2080 | OK |

None of the three failures is on a path this batch changed:

- **kiln_io_owner.** Deepest path: `owner_task` -> `kiln_io_all_relays_off`
  -> `kiln_io_reinit` -> `SX1509_write_port` -> I2C -> `esp_log`.
- **system_uart_bridge.** No change from this batch is on its path.
- **http_async_job.** The overage comes from the `backup_import_job`
  callback (5712 B).

`profile_exec_wdt` has zero margin. Any further growth in the watchdog
path fails the check.
