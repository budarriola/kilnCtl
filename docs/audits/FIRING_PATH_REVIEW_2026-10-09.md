# KilnFW firing control path review, 2026-10-09

Scope: the profile_executor lifecycle (start, pause, resume, stop/halt, abort,
segment transitions, hold/dwell, the 25 C ramp lock), the PID/control tick and
heater output windowing, on/off zones and aux outputs, the system-mode gate, and
how the ESP-side thermal guards (S-guards, guard 8 cross-zone, guard 9
control-tick liveness) stop a firing. Reviewed on origin/dev `0dd056c6`. Findings
only; nothing here is fixed by this document.

Line numbers are against `0dd056c6`. Paths are relative to
`firmware/KilnFW/App/drivers/control/` unless shown otherwise.

## Summary

| # | Severity | Finding |
|---|----------|---------|
| 1 | Medium | Guard 9 (control-tick liveness) takes `s_exec.lock` with `portMAX_DELAY` before testing staleness, so a control task stalled while holding the lock also blocks guard 9 |
| 2 | Low | Within one tick a heater zone's relay is commanded before that zone's thermal guard runs, giving an ON-then-OFF pulse on the trip tick |
| 3 | Low | Run and segment clocks accumulate `(uint32_t)(dt_s + 0.5f)` per tick, so dwell and relay/IO segment durations drift from wall clock |
| 4 | Low | After `profile_executor_halt()` a failed relay OFF write is never retried, because the state is IDLE |
| 5 | Low | `heater_output_duty_ex()` does not reject a NaN duty; its clamp passes NaN through to a float-to-unsigned conversion |
| 6 | Low | The guard-1 climbing window still takes `wrong_dir_window_s` when no valid plant model exists (the 2026-09-10 audit's residual) |
| 7 | Info | Resume re-checks none of the start gates, but every mutating path it would need is already refused while PAUSED |

No path was found where heat stays on after a stop, fault or pause beyond the
retry gap in finding 4. On every one of those paths `heat_enable` (K4) is
released as well. No reset-one-side pair was found in the executor. No
integer or float overflow was found over a long firing.

## Findings

### 1. Medium: guard 9 sits behind the lock it is meant to police

**Status: fixed in a43f8b3c6 (+ 9cff09cdd reconcile fix).**

- `profile_executor.c:2176`: `watchdog_task_entry()` calls
  `xSemaphoreTake(s_exec.lock, portMAX_DELAY)`. Only after that does it compute
  `since_ms = now - s_exec.last_tick_tick` and test it against
  `WATCHDOG_TICK_DEAD_MS`. The relay force-off (`kiln_io_all_relays_off`,
  `io_segs_force_all_off`, `guard9_assert_stale_tick_fault`) sits inside that
  critical section.

**Failure scenario.** The control task can stop ticking while it holds
`s_exec.lock`. Causes include an infinite loop, a deadlock with another lock,
or a hang inside a call made under the lock (`kiln_io_owner` writes,
`zones_config_*` getters, `safety_link_set_fault_source`, `relay_cycles_add`).
In that case the watchdog task blocks on the same lock forever and guard 9
never fires. The earlier executor panics (heap walks looping inside a critical
section, stack corruption) are exactly this stall shape. Guard 9 only works
when the control task is starved or blocked *without* the lock, which is the
less likely way for it to die.

**Mitigation that exists today.** `safety_build_and_send_context()`
(`../safety/safety_link_frames.c:498`) also calls `profile_executor_get_status()`,
which takes the same lock with `portMAX_DELAY`. The safety poll task therefore
stalls too, the Pico stops seeing context and heartbeats, and S6b (link dead)
trips and cuts heat. The hardware backstop holds. The ESP-side guard is still
silently ineffective, and the operator-facing reason will say "link dead", not
"control tick stale".

**Suggested fix.** Make `last_tick_tick` readable without the lock: a
`volatile` 32-bit tick count written by the control task, or a
`portENTER_CRITICAL` spinlock pair. Test staleness first. On stale, force the
relays off through `kiln_io` (which has its own owner-task serialisation)
*before* trying to take `s_exec.lock`. Then take the lock with a bounded timeout
for the bookkeeping, and on timeout set `SAFETY_FAULT_SRC_APP` anyway.

### 2. Low: relay commanded before the same tick's guard

**Status: fixed in 46721679e.**

- `profile_executor.c:1568`: `apply_relay(zi, want_relay_on[zi])` is called for
  a heater zone.
- `profile_executor.c:1761`: that zone's `thermal_guard_tick()` runs afterwards
  in the same loop iteration. On a trip, `escalate_guard_trip()` then forces the
  relay off.

**Failure scenario.** This tick's reading is already at or above `max_temp_c`,
or shows runaway, while PID still wants heat. The relay gets an ON write
followed by an OFF write a few milliseconds later. The physical effect is
negligible because relay pull-in time is much longer. It does cost one
unnecessary contact cycle in `relay_cycles`, and it breaks the rule that heat is
never commanded on a reading the guard would reject.

**Suggested fix.** Evaluate `thermal_guard_tick()` (or at least its
reading-only checks: sensor invalid, max/min temperature) before
`apply_relay()`. Pass `want_on = false` when that zone tripped this tick.

### 3. Low: run and segment clocks round each tick to whole seconds

**Status: fixed in 52cb437f1.**

- `profile_executor.c:830`: `total_elapsed_s += (uint32_t)(dt_s + 0.5f)`.
- `profile_executor.c:1044` and `:1088`: `segment_elapsed_s` is accumulated the
  same way.
- `profile_executor.c:1045` (relay/IO segments) and the dwell-end check compare
  these counters against `dwell_min * 60u`.

**Failure scenario.** The loop is `vTaskDelay(1000 ms)` plus the tick's own work
(`profile_executor.c:692`), so `dt_ms` is about 1000 plus the work time. Any
work under 500 ms is discarded every tick, and the counters undercount by that
fraction. With 100 ms of work, a 10 h hold runs about 11 h. Work of 500 ms or
more on a tick double-counts it instead. Ramps are not affected, because the
target advances by `rate * dt_s` in float. Dwell, relay/IO segment duration,
`total_elapsed_s` and the remaining-time estimate are all affected. The error
is benign for safety (dwells run long), but it is a schedule-accuracy defect
that compounds over a long firing.

**Suggested fix.** Accumulate in milliseconds (`uint32_t` ms overflows only
after 49 days) and derive seconds from that. Alternatively, keep a
`float`/ms remainder carried between ticks.

### 4. Low: a failed OFF write at halt is never retried

**Status: fixed in 5b0e3bb56.**

- `profile_executor_status.c:47`: `profile_executor_halt()` calls
  `force_all_relays_off()`, and later `exec_enter_terminal_state(IDLE)`.
- `profile_executor_relay_io.c:96-104`: on a `kiln_io_owner` write failure,
  `apply_relay`/`force_zone_relay_off` only log. They still record
  `relay_commanded_on = want_on` (`:113`).
- `profile_executor_relay_io.c:460-463`: `force_all_relays_off()` returns
  immediately when the state is IDLE.
- `profile_executor_relay_io.c:1410`: `sweep_unowned_relays()` runs only in the
  RUNNING tick.

**Failure scenario.** An operator Stop coincides with a transient
`kiln_io_owner` failure (queue full, SX1509 I2C error). The zone relay stays
closed in hardware. PAUSED, FAULTED and DONE re-run `force_all_relays_off()`
every not-RUNNING tick and so retry. IDLE (after halt) does not, and nothing
else revisits the relay. Heat is still cut because `heat_enable_release()`
drops K4 (`profile_executor_status.c:80`). The contact stays closed, though, so
K4 is the only barrier left, and `/api/control` shows the relay as OFF.

A second, related gap: `apply_relay` returns early at
`profile_executor_relay_io.c:32` when `zones_config_get_relay_mask()` fails or
returns 0. That return skips the OFF write too. Zone writes are refused while
running, so this can only happen through a getter failure.

**Suggested fix.** Track write success per zone (only set
`relay_commanded_on` on success, or keep a `relay_off_pending` mask like the
aux path's `aux_off_pending`). Retry from the not-RUNNING branch regardless of
state, including IDLE, until confirmed.

### 5. Low: NaN duty is not rejected at the heater output

**Status: fixed in 88aa123ca.**

- `heater_output.c:66-69`: the clamp is `if (duty < 0) ... else if (duty > 1)`.
  A NaN passes through both tests.
- `heater_output.c:102`: `on_ms = (uint32_t)(duty * window_ms)`. Converting
  NaN to an unsigned integer is undefined behaviour in C.

**Failure scenario.** No live NaN source was found:
- The executor only calls PID with `sensor_ok`.
- `effective_target_c` is `isfinite`-checked (`profile_executor.c:1349`).
- `zone_feedforward` rejects a non-finite `u_ff`
  (`profile_executor_feedforward.c:549`).
- `pid_update_terms` has no other non-finite input.

This is defence in depth. A future change that lets a NaN through (for example
a zero `d_filter_tau_s + dt_s`, or a new feedforward term) would hand an
undefined on-time to the relay window. Depending on the compiler, that can come
out as a full-window ON.

**Suggested fix.** Use `if (!(duty > 0.0f)) duty = 0.0f; else if (duty > 1.0f) duty = 1.0f;`,
which maps NaN to 0. Add a host test that feeds NaN.

### 6. Low: climbing-window override when no plant model is valid

**Status: fixed in 699a65ec9.**

- `thermal_guard.c:327`: `window_s = effective_f(cfg->wrong_dir_window_s, climbing ? progress_window : WRONG_DIR_WINDOW_S)`.
  A configured `wrong_dir_window_s` therefore also sets guard 1's *climbing*
  window.
- The 2026-09-10 fix (`climb_window_floor_s`, `profile_executor.c:1720`) floors
  that window at `dead_time + tau`, but only when `z->ff_enabled` (a valid
  model).

**Failure scenario.** A zone has a short `wrong_dir_window_s` (sized for the
falling-while-heating case) and no valid feedforward model. It still gets
HEATING_FAILED false trips early in a firing. This is the residual of
`docs/audits/esp_panic_after_zone0_guard_trip_2026-09-10.md` lines 128-135.
It fails safe (a nuisance abort), so it is Low.

**Suggested fix.** Use `wrong_dir_window_s` only for the falling branch and
`progress_window_s` for the climbing branch, as that audit proposed.

### 7. Info: resume skips start gates, but they are covered

**Status: fixed in 032fa7d2b (acquire result logged).**

`profile_executor_resume()` (`profile_executor_status.c:324`) re-claims the
relays, seeds bumpless PID, sets RUNNING and acquires `heat_enable`. It
re-checks none of the start-time refusals: readiness, recovery/restore system
mode, OTA, current sweep, relay authority, zones generation. I checked whether
any of those can change while PAUSED:
- OTA: `../net/ota_interlock.c:60`
- current sweep: `zones_current_sweep_task.c`
- zone writes: `../safety/system_mode_gate.c`
- backup import: `../http/backup_import.c:4094`, `:4302`
- factory reset: `../http/factory_reset.c:474`, `:571`, `:615`

All of these treat PAUSED as running and refuse. Relay authority blocks are
evaluated again every tick in `apply_relay`. A Pico trip while PAUSED is caught
by the watchdog's FAULT action, which covers `state_running_or_paused`. No
exploitable gap remains.

The one soft spot: `heat_enable_acquire_since()`'s result is discarded at
`profile_executor_status.c:351`. A failed K4 acquire is handled by the
reconcile path, not by resume itself.

## Checked and judged correct

- NaN from a bad thermocouple read. `thermo_combine()` returns NaN with
  `valid = false`. `actual_c` is NaN only together with `actual_valid = false`.
  The PID tick runs only on `sensor_ok`, so with a bad sensor the duty is 0 and
  the integral is frozen. BANGBANG gives no heat on an invalid sensor. The ramp
  lock holds on `!sensor_ok`. Guard 6 debounces sensor invalid into a trip.
  Guard 8 skips peers with `!peer_ok`, and a NaN delta never wins the
  `delta > worst` comparison.
- Integral wind-up. `pid_update_terms()` uses conditional integration and
  floor/ceiling clamps, and `PID_INTEGRAL_RAW_ABS_BOUND` bounds the raw
  integral on every write site, including `pid_rescale_integral_for_new_ki()`.
  With `ki == 0` the raw integral is unbounded, but it is a float in the
  millions at worst over a day and is clamped as soon as `ki > 0`.
- The `cycle_count` and `cycles_reported` pair: both are zeroed together by the
  run-start `memset` (`profile_executor_run.c:746`).
- The producer-call rule. The thermo read, the Pico tripped read and the safety
  status read all happen before `s_exec.lock` in both the control task and the
  watchdog. Run-state and firing-stats persistence happen after the lock is
  released.
- Pause: zone relays are forced off, `heat_enable` is released, and claimed
  relays are handed to MANUAL (manual writes are refused by the mode gate while
  PAUSED). Aux outputs hold their last state by design
  (`docs/SPARE_RELAY_ONOFF_PLAN.md` "PAUSE: hold last").
- Reboot mid-firing. By design there is no auto-resume (`run_state.h` header).
  Relays come up OFF from `kiln_io_init()`, and `run_state` is a breadcrumb
  only.
- The dwell credit is capped at `EXEC_DWELL_CREDIT_MAX_FRACTION`, so
  `dwell_total_s` cannot underflow.

## Old control-loop bug cluster

- Fake setpoint to the guards from autotune: resolved (`1bfd5ee`,
  `no_setpoint`).
- Integral floor at `-ff_u` regressing ramps: replaced by the hold-only floor
  (`i_floor = -ff_hold`, `pid.c`).
- `adaptive_tune_ki_guard_timing_and_failopen_2026-09-14.md`: both defects are
  marked fixed in the doc.
- The guard-1 window on a slow zone: only partly fixed; see finding 6.
