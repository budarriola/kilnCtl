# Review: F5 APP holder fix (75eee94b2) -- 2026-10-10

Scope: KilnFW commit `75eee94b2` on origin/dev (status docs `6f29e0d6e`, `aa2c51e31`).
It fixes REVIEW_DK7FIX_2026-10-10 LOW-2 (explicit external APP-holder registration,
level relay-unknown assert), REVIEW_EXECTEST LOW-1/INFO-1, and REVIEW_SAFTYFX7 I-5
(claim-less lost-trip log in `heat_enable.c`, merged with upstream `reboot_fatal_latched`).
Reviewed at `aa2c51e31`. No code changes in this review.

## Verdict

No HIGH or MED findings. The fix does what it says. While a registered boot latch holds APP,
no executor path drops it. The level re-assert is idempotent and sends no link traffic. The
`heat_enable.c` merge is correct. The RELAY_IO OFF-on-block gate is ordered correctly and
writes once. What is left: four LOW findings and three INFO.

## Findings

### LOW-1: no test covers the two boot registration sites (negtest M5/M6 MISSED)

`firmware/KilnFW/App/main.c:182` and `firmware/KilnFW/App/main_control_bringup.c:108` are the
only callers of `profile_executor_note_external_app_hold()`. Host tests do not compile
`main.c` or `main_control_bringup.c`. Deleting either registration line still passes all 98
host executables (negtest M5, M6 below). The prestart tests call the registration function
directly, so they prove the executor honours a registration. They do not prove the latch
sites make one.

Failure scenario: a later refactor of `main_kiln_enter_safe_state()`, or a new boot latch
(for example a third `main_*_bringup.c` failure path), asserts `SAFETY_FAULT_SRC_APP` without
registering. A halt (`clear_this_runs_faults()`) or a relay-unknown release
(`relay_unknown_release_locked()`) then drops the boot safe-state latch. Before this commit the
link-bit inference covered that case. Now nothing does, and nothing mechanical catches it.
A grep check fits here: every `safety_link_set_fault_source(` call outside
`drivers/control/` whose mask can include APP must be preceded by the registration call.

### LOW-2: registration and executor release are not atomic; a concurrent release can drop a fresh boot latch

`profile_executor.c:2211` (registration is a plain store), `profile_executor_relay_io.c:1531`,
and `profile_executor.c:2241` (each release reads `pe_app_owner_foreign`, then calls
`safety_link_set_fault_source(APP, false)`). No lock covers both sides. The executor and its
watchdog task are started in `main_control_bringup()` (`main_control_bringup.c:363`). The two
later latch sites, `main_network_http.c:968/998` (PC-link UART failure) and
`main_bridges_bringup.c:240` (link watchdog failed to start), therefore run while the watchdog
task is live.

Interleaving: the watchdog reads `foreign == false` in `relay_unknown_release_locked()` →
app_main calls note() and then asserts APP → the watchdog deasserts APP. If APP was newly
asserted, the deassert is parked behind `SAFETY_FAULT_MIN_HOLD_MS` (300 ms). The poll task then
completes it. The boot latch is one-shot and never re-asserted, so the isolated fault line stays
released for the rest of the boot.

Mitigations: the window is a few instructions wide. It also needs a relay-unknown clear or an
operator halt at exactly that moment during boot. Relays were already driven off by the safe-state
path. Severity is LOW. A fix is to take the link state lock across "check foreign + deassert",
or to have the boot latch re-assert after registering (a second idempotent assert closes the window).

### LOW-3: a halt can still drop APP for up to one watchdog period while relay state is unknown or guard 9 is pending

`profile_executor_relay_io.c:1531` releases `global_fault_source`'s APP bit and checks only
`pe_app_owner_relay_unknown()` and `pe_app_owner_foreign`. It does not check
`s_guard9_bookkeeping_pending`, which `relay_unknown_release_locked()` does check
(`profile_executor.c:2240`). The relay-unknown half is the race the commit accepts by design.
`relay_unknown_prelock_check()` runs without the lock (`profile_executor.c:2358`). A halt holding
`s_exec.lock` can read the flag as false, then the prelock sets it (`:2226`) and asserts, then the
halt deasserts. The guard-9 half: a run already FAULTED by guard 9 (APP merged into
`global_fault_source`) goes stale again. `guard9_prelock_check()` re-asserts and sets pending, but the
watchdog's bounded lock take times out because the halt holds the lock. The halt then releases APP
while the tick is stale.

In both cases the next watchdog pass (`WATCHDOG_CHECK_PERIOD_MS` = 2000 ms) re-asserts, because
both prelock checks are now level-triggered. Relays were already cut. `kiln_io` independently
refuses any ON write while `relay_state_unknown` is set (`kiln_io.c:501-502`). So the exposure is
at most 2 s with the isolated line released and no coil able to close. LOW. The commit says this
window exists ("repaired by the next pass"), but the guard-9-pending half is not mentioned. Adding
`!s_guard9_bookkeeping_pending` to the `clear_this_runs_faults()` APP exclusion would make the two
release sites agree.

### LOW-4: a RELAY_IO ON dropped by the new gate is reported and honoured as ON

`profile_executor_relay_io.c:1181` sets only the local `drive_on`. `r->state_on` stays true.
Consequences:
- The `ESP_LOGI` right after (`:1203`) logs the segment as "ON" just below the
  "WANTS ON BUT IS BLOCKED ... commanded OFF" warning. The warm-start replay log
  (`profile_executor_run.c:1484`) does the same.
- `io_seg_finish()` computes `leave_on` from `r->state_on` (`:1232`). On a clean DONE with
  `leave_on_at_end`, the relay is handed back to NONE as an intentional leave-ON, but it is
  physically OFF. That is fail-safe (nothing is energised), but the end-of-run report and the
  operator's expectation disagree with the hardware.
- No status field shows the drop, and by design it is never retried. A safety-link or PC-link blip
  of any length at the instant a segment starts therefore leaves a vent fan or damper OFF for the
  whole segment. For a blocking segment, that can be most of the firing. The WARN log is the only
  trace.

None of this energises anything. LOW, for visibility and process correctness. Recording
`r->state_on = false` (or a separate `dropped` flag surfaced in status) would make log, finish
and status agree.

### INFO-1: level-triggered re-assert -- no spam, no link traffic, no starvation

`relay_unknown_prelock_check()` now calls `safety_link_set_fault_source(APP, true)` every 2 s
pass while unknown. That call is idempotent. It takes the link state lock briefly, records an edge
and logs only when the mask changes (`safety_link.c` `after != before`), and drives the opto line
through `safety_apply_fault_locked()`. It sends no UART frame. The re-assert also cancels any
pending deferred deassert, which is the repair the commit intends. The one per-pass log line,
"relay state unknown -- forcing relays off", was already level-triggered before this commit, so
the log rate is unchanged at one line per 2 s.

### INFO-2: heat_enable.c merge is correct

`heat_enable.c:783` computes `log_claimless_lost_trip` from `reboot_was_tripped` before line 784
ORs it into `reboot_fatal_latched` and line 786 clears it. It uses the same `fatal_u` mask, so the
log fires exactly when the latch is set *because of* a lost trip and not a fatal boot reason. The
log is emitted after `he_unlock()` (`:825`), as the module requires. It fires at most once per
resolved verdict. Upstream's "unconsumed fatal latch is not downgraded" semantics
(`|| s_he.reboot_fatal_latched`) are preserved. The log text "heat stays withheld until an
operator clears it" matches `heat_enable_acquire()`. The next acquire consumes the latch into
`reboot_hold`, and only an operator resume releases it. Pre-existing gap, not introduced here: a
claim-less reboot with a *fatal* `boot_reason` latches silently (no log). Only the lost-trip
variant logs.

### INFO-3: APP holder inventory (grep of every assert/deassert)

APP asserts: `profile_executor.c:2148` (guard 9 prelock), `:2228` (relay-unknown),
`profile_executor_relay_io.c:1520` (guard 9 locked), `main.c:183` (registered),
`main_control_bringup.c:109` (registered; the bit comes from `main_boot_early.c:602`, expander
down). The main.c path covers the callers `main_bridges_bringup.c:240` and
`main_network_http.c:968/998`. APP deasserts: only `profile_executor_relay_io.c:1535` and
`profile_executor.c:2242`, and both honour `pe_app_owner_foreign`. `autotune_engine*.c` asserts
and clears only THERMO/THERMAL_SANITY and never APP. `uart_bridge.c` touches only PC_LINK.
`relay_authority.c:20` *reports* APP when there is no safety link, but holds nothing. No external
APP latch site is unregistered. Out of scope for F5, but noted: THERMO is likewise shared between
the boot latch (`main_boot_early.c:615/636`), the executor and autotune. A run's clear releases it
whoever else set it. In practice, a boot with no SPI/thermocouple cannot start a run, so this is
latent.

## Checked and correct

- No path releases APP while a *registered* boot latch holds: both release sites check
  `pe_app_owner_foreign`. The flag is sticky for the boot and never cleared outside tests.
  Negtests M2, M3 and M7 prove the tests see this.
- Removing the inference fixes both DK7FIX LOW-2 cases: (a) no false foreign holder after a racing
  halt, and (c) a latch registered while the executor already holds APP is kept.
- Relay OFF on blocked authority: the claim comes before the write (same discipline as
  `apply_relay()`). The block test uses the same `relay_authority_on_blocked()` as `apply_relay()`.
  There is one write and no retry, so no chatter. OFF is never gated. The write is under
  `s_exec.lock` like the other segment writes. The check-then-write TOCTOU against a source rising
  in between is the same as in `apply_relay()` (pre-existing class). Negtest M4 proves the new test
  sees the gate.
- Warm-start replay (`profile_executor_run.c:1473`) runs after `relay_authority_start_blocked()`.
  The new gate therefore only changes behaviour if a source rises between the two.
- In RECOVERY MODE the executor does not start, so registration only sets an unused global.
- Test hygiene (EXECTEST LOW-1): the `test_kiln_io_owner.c` and DONE-PSRAM test restorations look right.

## Tests run

- Full KilnFW host suite at `aa2c51e31` (`build_host_tests.ps1`, fresh OutDir): **98/98 executables
  built and passed**, exit 0.
- `tools/negtest.ps1 -Preset kilnfw-host -Parallel 3 -ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"`,
  base `aa2c51e31`, baseline PASS, real tree unchanged, copies removed:

| Mutation | Verdict |
|---|---|
| M1 relay-unknown assert back to edge-triggered | CAUGHT (prestart:2482, 2581) |
| M2 `clear_this_runs_faults()` ignores foreign | CAUGHT (prestart:2543, 2595) |
| M3 `relay_unknown_release_locked()` ignores foreign | CAUGHT (prestart:2540, 2543) |
| M4 RELAY_IO block not applied (`drive_on = false` removed) | CAUGHT (prestart:2803) |
| M5 `main.c` registration removed | **MISSED** (LOW-1) |
| M6 `main_control_bringup.c` registration removed | **MISSED** (LOW-1) |
| M7 registration function a no-op | CAUGHT (prestart:2536..2595) |

## Fix status

| Finding | Status | Fix |
|---|---|---|
| LOW-1 | fixed | `tools/check_app_hold_registration.ps1`: every APP-capable `safety_link_set_fault_source()` assert outside `drivers/control` and `test/` must be preceded by `profile_executor_note_external_app_hold()`. Negtest: deleting either registration line is CAUGHT. |
| LOW-2 | fixed | `pe_app_release_recheck()` runs right after each APP deassert (`relay_unknown_release_locked()`, `clear_this_runs_faults()`): if a boot latch registered in the window it re-asserts (idempotent, cancels the deferred deassert). `pe_app_owner_foreign` is now `volatile`. Host test `test_f5holder_low2_low3`. |
| LOW-3 | fixed | `clear_this_runs_faults()` also keeps APP while `pe_app_guard9_pending()`. Host test `test_f5holder_low2_low3`. |
| LOW-4 | fixed | `io_seg_runtime_t.state_on` is now the physical state; `wants_on` keeps the request. A gate-dropped ON logs OFF, is never handed over as leave-on, and `io_segs_tick()` retries it each tick through the same `relay_authority_on_blocked()` gate. Host test `test_io_seg_dropped_on_retried_through_gate`. |
