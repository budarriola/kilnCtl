# Audit: KilnFW control and profile executor (2026-10-14)

Read-only device audit of the KilnFW control path at `origin/dev` `153c12cf5`.
No code was changed and the board was not touched. All paths are relative to
`firmware/KilnFW/App/drivers/` unless noted.

## Scope and method

- `control/profile_executor*.c`: segments, hold and dwell, the ramp lock band,
  pause and resume, live-edit pickup (`persist/live_profile.c`), the guard-9
  watchdog task and Pico link loss.
- PID, feedforward and fuzzy PID, `control/heater_output.c`,
  `control/thermal_guard.c`, ramp assist.
- `control/autotune_engine*.c`.
- ON_OFF zones, monitor-only (aux-converted) zones, aux outputs and profile aux
  rules, `zone_aux_convert_*`.

Method: read the code against the CLAUDE.md checklist (locks held across
producer calls, reset-one-side pairs, integer and float edges, NaN
thermocouple readings). Items already reported and fixed in
`docs/audits/FIRING_PATH_AUDIT_2026-10-10.md` and
`docs/audits/FIRING_PATH_REVIEW_2026-10-09.md` are not repeated. Three areas
(autotune; PID/feedforward/guards/heater output; aux and on/off) were reviewed
in parallel by opus agents and every finding they reported was checked against
the cited lines before it was kept here.

## Summary

HIGH 0, MED 4, LOW 3, INFO 5.

| ID | Area | One line |
|----|------|----------|
| MED-1 | executor / thermal_guard | Guard 1/2 window state survives pause/resume; a cooled kiln false-trips HEATING_FAILED/WRONG_DIRECTION right after resume and the firing aborts |
| MED-2 | executor / relay IO | A pause permanently cancels a running non-blocking relay/IO segment; it is never restarted on resume |
| MED-3 | PID / feedforward | A negative feedforward hold becomes a positive integral floor, so the zone is driven above setpoint |
| MED-4 | autotune | Accept unconditionally writes IDLE after its slow setters, orphaning a run started meanwhile on another zone |
| LOW-1 | on/off zones | Pause/resume does not reset the on/off actuation state, so a relay can re-energise against the decision for up to min_on_s |
| LOW-2 | autotune | A relay-authority block during STEPPING/CYCLING silently flattens the trace; the run continues and can be accepted |
| LOW-3 | PID | Derivative kick on the first valid sample after a sensor outage |

## Findings

### MED-1: thermal-guard window state survives pause/resume and can false-trip right after a resume

- **Where:** `control/profile_executor_status.c:353-444`
  (`profile_executor_resume()`) resets `prev_control_tick` and calls
  `seed_bumpless_with_ff()` for each zone. It never touches `z->guard_state`.
  `thermal_guard_reset()` is called only at run start
  (`control/profile_executor_run.c:1129`), and `thermal_guard_clear()` only on
  fault clear (`control/profile_executor_relay_io.c:1701`). While PAUSED,
  `thermal_guard_tick()` does not run (`control/profile_executor.c:760`). The
  guard 1/2 window fields therefore keep their pre-pause values:
  `progress_window_active`, `progress_window_start_c` and
  `progress_window_elapsed_s` (`control/thermal_guard.c:229-243`).
- **Scenario:**
  1. A zone ramps at duty >= `progress_duty_min` (0.5), with its window 250 s
     into 300 s.
  2. The firing pauses. The operator can pause it, or the watchdog
     auto-pauses on `pico_fatal_reboot` or `heat_grant_unconfirmed`
     (`control/profile_executor.c:2577-2584`).
  3. The kiln cools 30 C over a 20 min pause.
  4. On resume the zone is far below setpoint, so PID and feedforward
     command high duty. The window is still active, and `elapsed_s` counts
     only running time, so it reaches `window_s` within seconds.
  5. The window check sees `delta = now - start_c = -30 C`:
     - Climbing branch: guard 1 trips `HEATING_FAILED`
       (`control/thermal_guard.c:379-392`).
     - Arrival band or dwell: guard 2 computes `-delta / elapsed_min` from
       running time only, gets many C/min, and trips `WRONG_DIRECTION`
       (`control/thermal_guard.c:400-405`).
- **Consequence:** `escalate_guard_trip()`
  (`control/profile_executor_relay_io.c:1580-1615`) latches the zone. With the
  default `continue_on_zone_trip=false` it faults the whole firing. A healthy
  firing is lost after a long pause, and the trip text blames a dead element
  or miswiring. This also happens after the executor's own automatic pause on
  a benign Pico reboot, which owner decision 2026-10-10 says should resume
  heat. It fails safe, since heat goes off.
- **Fix:** in `profile_executor_resume()`, under the lock, restart the rolling
  windows of every active zone from the current measurement:
  - the guard 1/2 progress window;
  - the guard 3 off-window and runaway baselines;
  - the guard 4 at-setpoint and idle clocks;
  - the guard 7 frozen clock.

  Keep any latched trip (`is_tripped`/`reason`); `thermal_guard_reset()` must
  not clear one. A `thermal_guard_restart_windows()` helper next to
  `thermal_guard_reset()` keeps that rule in one place. Host test: pause
  mid-window, drop the measurement 30 C, resume at full duty, and expect no
  trip within one window.
- Found independently by two reviewers.

Fixed by ctlfx: `thermal_guard_restart_windows()` (latched trips kept) called from `profile_executor_resume()` for every active zone; tests `test_resume_restarts_thermal_guard_windows` and the thermal_guard restart variants.

### MED-2: a pause permanently cancels a running non-blocking relay/IO segment

- **Where:**
  - Every PAUSED tick calls `io_segs_force_all_off(false)`
    (`control/profile_executor.c:768`).
  - That calls `io_seg_finish()`, which writes OFF, releases the claim and
    sets `r->active=false` (`control/profile_executor_relay_io.c:1348-1399`).
  - On resume, only the current segment is restarted, through
    `io_seg_start_pico()` when `!io_segs[segment_index].active`
    (`control/profile_executor.c:1054-1055`).
  - A non-blocking segment has already moved `segment_index` past itself on
    the tick it started (`control/profile_executor.c:1073-1077`). Its
    `remaining_s` is discarded and nothing restarts it.
- **Scenario:** a profile starts a non-blocking 60 min vent fan or IO output at
  segment 2. The firing pauses ten minutes into it, either by the operator or
  by the watchdog's auto-pause, and is then resumed.
- **Consequence:** the output stays off for the rest of the firing, and no log
  line or status field says so. The ware loses the venting or IO action the
  profile asked for, and both GUIs show a normal firing. Two related paths
  behave differently:
  - A blocking segment is restarted with its elapsed time kept.
  - Aux outputs hold their last state across a pause
    (`docs/SPARE_RELAY_ONOFF_PLAN.md`, "PAUSE: hold last").
- **Fix:** keep the forced OFF on pause, which is the safe choice.
  - On pause, mark each still-active non-blocking segment as paused and keep
    its `remaining_s`.
  - On resume, re-run `io_seg_start_pico()` for those segments with the kept
    time.
  - FAULTED, HALT and DONE keep today's finish behaviour.
  - If this behaviour is intended instead, document it and surface it in
    status.
  - Host test: pause and resume during a non-blocking segment.

Fixed by ctlfx: PAUSED sweeps with `io_segs_pause_all_off()` (remembers remaining time), `io_segs_resume_pending()` restarts through the gated start path; every other stop still cancels; tests `test_io_seg_nonblocking_survives_pause_resume`, `test_paused_tick_uses_pause_aware_io_sweep`.

### MED-3: a negative feedforward hold becomes a positive integral floor

- **Where:**
  - `control/pid.c:169-171` (`pid_update_terms`) sets `i_floor = -ff_hold`.
    When `i_term < i_floor` it clamps `i_term` and `integral` to that floor.
  - `control/pid.c:44` (`pid_seed_bumpless`) uses the same floor:
    `integral_floor = -ff_hold / ki`.
  - `control/profile_executor_feedforward.c:557` passes `*out_hold =
    hold_total` deliberately unclamped. Only `u_ff` is clamped to [0,1], at
    :558-559.
  - `hold_total` already includes the cross-zone coupling correction, which
    is subtracted at :535.
- **Scenario:** `hold_total < 0` in any of these cases:
  - The coupling correction exceeds the zone's own hold. This happens when
    PID neighbours run hotter than this zone's setpoint, for example a zone
    held back by its approach-rate cap (`effective_target_c` trailing) while
    its neighbours already sit at the shared target.
  - The coupled solve returns a negative u for the zone
    (`control/zone_coupling_solve.c:466-469` leaves it unclamped).
  - The setpoint is below ambient.

  Then `i_floor = -ff_hold > 0`.
- **Consequence:**
  - The floor was meant to stop I from cancelling feedforward, but here it
    forces I to be at least |hold|. With `u_ff` clamped to 0, the output at
    zero error is I = |hold| > 0, so the zone is heated when the model says it
    needs no heat.
  - It settles where P cancels that, about |hold|/kp above setpoint. With
    hold -0.28 and kp 0.02 that is about 14 C, enough to overshoot a dwell
    and to break guard 4's drift band or a neighbour's coupling assumptions.
  - Resume and config reload seed from the same wrong floor.
- **Fix:** floor at zero in both places, `-fmaxf(ff_hold, 0.0f)`. The
  alternative, reporting `fmaxf(hold_total, 0.0f)` as `out_hold`, would also
  change the duty breakdown, so floor at the use site. Host test: a negative
  hold with error 0 must let I reach 0 and output 0.

Fixed by ctlfx: `pid.c` clamps the feedforward hold to be non-negative in both the bumpless seed and the update floor; MED-3 block in `test_pid.c`.

### MED-4: autotune accept overwrites state with IDLE and can orphan a concurrent run on another zone

- **Where:**
  - `control/autotune_engine_guard.c:446-460`: accept reserves the accepted
    zone (`external_write_reserved`) and releases `s_at.lock`.
  - It then runs several flash-worker-backed setters (:511 onward): model and
    PID, fit context, baseline k_dc, max ramp, coupling diag, tuning quality.
  - `control/autotune_engine_guard.c:870-873` (step method) then takes the
    lock again and, without re-checking state, calls `clear_block_if_any()`
    and sets `s_at.state = AUTOTUNE_ENGINE_IDLE`. The relay method has the
    same block at `:577-580`.
  - Nothing stops a start in between. Start refuses only
    `state_is_running()`, which excludes DONE (`:33-37`), and a reservation
    on the same zone only (`control/autotune_engine.c:1211`).
- **Scenario:**
  1. Zone A is DONE. The operator accepts it over HTTP.
  2. While the setters run, a UART or LCD start for zone B passes the gates.
     It claims B's relay authority mask, zone claim and heat-zone claim, and
     enters SETTLING or RELAY_APPROACH.
  3. Accept(A) finishes and writes IDLE without `force_relays_off()`.
     `clear_block_if_any()` now acts on `s_at.zone_index == B`.
- **Consequence:**
  - The engine reports IDLE while zone B still holds RELAY_OWNER_AUTOTUNE,
    the zone claim and the heat-zone claim. None of these is released, so
    profile and autotune starts on B are refused until reboot.
  - B's relay may have been left commanded ON. `task_entry` only runs the K4
    release backstop when it sees not-running, so K4 most likely opens,
    which limits the heat hazard.
  - `sweep_unowned_relays()` does not cover autotune-owned relays
    (`control/profile_executor_relay_io.c:1799-1837`).
- **Fix:**
  - At the final lock, change state only when `s_at.state ==
    AUTOTUNE_ENGINE_DONE && s_at.zone_index == zone`.
  - Make `autotune_begin_run_locked` refuse while any `external_write_reserved`
    is set, whatever the zone. That closes the window entirely.
  - Host test: a start for another zone during accept is refused.

Fixed by ctlfx: accept writes IDLE only while it still owns DONE on its zone (both blocks) and a start is refused on any reservation; tests `test_reserve_blocks_a_start_on_another_zone`, `test_accept_does_not_idle_a_run_started_meanwhile`, `test_accept_step_method_does_not_idle_a_run_started_meanwhile`.

### LOW-1: ON_OFF actuation state is not reset by pause/resume, contrary to ON_OFF_ZONE.md sec 5

- **Where:**
  - `control/profile_executor_run.c:1130-1143` resets `on_off_trigger_state`,
    `on_off_actuated_on` and `on_off_actuated_held_s`. Its comment says this
    also covers "the warm-start/resume path".
  - `profile_executor_resume()` (`control/profile_executor_status.c:353`) does
    not call that path and resets none of these fields.
  - While PAUSED, `force_all_relays_off()` (`control/profile_executor.c:760`)
    opens the zone's relay, but `on_off_actuated_on` stays `true`.
- **Scenario:** an on/off zone turns ON, and the firing is paused less than
  `min_on_s` (default 30 s) later. After resume this tick's decision is OFF,
  but the actuation gate still thinks the relay has been held ON for less than
  `min_on_s`.
- **Consequence:**
  - `apply_relay(zi, true)` re-energises a relay that was open for the whole
    pause, against the current decision, for up to `min_on_s`.
  - Cycle counting (`control/profile_executor.c:1945-1948`) misses that pair
    of transitions.
  - This is bounded, and an on/off zone is not a heat source, but it breaks
    sec 5's rule: "Resume starts every on/off device in its fail-safe state".
- **Fix:** in `profile_executor_resume()`, apply the same three resets as run
  start: `on_off_trigger_state_reset`, `on_off_actuated_on=false` and
  `profile_executor_on_off_seed_hold`. Also correct the comment at
  `control/profile_executor_run.c:1131`.

### LOW-2: autotune measures through a relay-authority block without recording it

- **Where:** `control/autotune_engine_guard.c:109-116`. When
  `relay_authority_zone_blocked()` is true, the code logs, forces
  `want_on=false`, and carries on. Nothing records that the measurement was
  blocked. The guards still get the intended duty: the executor's "zero duty
  while blocked" fix was not ported here, as `control/autotune_engine.c:515-532`
  itself notes.
- **Scenario:** during STEPPING or CYCLING, a short safety-link or PC-link
  fault-source blip leaves an unheated gap in the trace.
- **Consequence:** the response may still pass `finalize_fit`'s range checks,
  so the run reaches DONE with gains that are in range but wrong (K low, tau
  or L skewed). The operator can accept them. A long block instead makes
  guards 1/2 trip with a false "heating failed", which fails safe.
- **Fix:**
  - Latch `blocked_during_measure` whenever the block fires in STEPPING,
    CYCLING or RELAY_APPROACH, and abort or refuse finalize when it is set.
  - Clear the latch in begin_run next to `per_zone_blocked`, so the pair
    resets together.
  - Feed the guards the real zero duty, as the executor does.

### LOW-3: derivative kick after a sensor outage

- **Where:** `control/profile_executor_pid_tick.c` (`pid_family_zone_tick`).
  While `!sensor_ok`, duty is forced to 0 and PID state is left untouched, so
  `prev_measurement` keeps its pre-outage value.
- **Scenario:** a thermocouple drops out for several ticks and then recovers.
- **Consequence:** on the first valid sample, D sees `(meas - prev) / dt`
  across the whole gap. The D filter softens this, but the output can still
  pin at 0 or 1 for a tick or two. Duty clamping and guard 6 bound it.
- **Fix:** on a `sensor_ok` false-to-true edge, set `prev_measurement` to the
  current measurement and reset the D filter, or bumpless-seed, before the
  update.

### INFO-1: aux temperature predicate has no NaN term

`control/profile_executor_relay_io.c:1018-1041` uses
`tz && tz->active && !tz->faulted && tz->actual_valid` without
`!isnan(tz->actual_c)`. The zone on/off path, about line 1003, has that term.
It is not reachable: `actual_c=NAN` is set only together with
`actual_valid=false` (`control/profile_executor.c:894-920`). Even if a NaN got
through, the comparisons are false and the path forces OFF. Add the term so
the aux and zone paths match.

### INFO-2: FIRING_PATH_AUDIT_2026-10-10 LOW-2 text is stale

`aux_apply_relay_pico()` (`control/profile_executor_relay_io.c:736-792`) ORs
the aux bit into `claimed_relay_mask` before writing. The unowned-relay sweep
(about line 1819) treats `aux_claim_mask & enabled_mask` as owned. The code is
correct; only that audit's text is out of date.

### INFO-3: autotune accept's follow-on setters are not atomic with the gains write

`control/autotune_engine_guard.c` after :511. Model and PID are written in one
atomic call, but the fit context, baseline k_dc, max ramp, coupling diag and
tuning quality are separate calls. Their failures (busy, mode gate) are only
logged, so a partial accept still reports OK. Every value is validated, so
this is not a safety issue. Report the partial success to the caller.

### INFO-4: autotune accept does not re-check the zone's type or masks

Between DONE and accept, the zone can be converted to ON_OFF or aux, or have
its relay_mask changed, and accept still writes gains. This is harmless,
since ON_OFF and monitor-only zones ignore PID. Refusing would be clearer.

### INFO-5: bang-bang `since_last_change_ms` is a uint32 accumulator

`control/heater_output.c`. It wraps only after 49.7 days without a relay
transition, and the worst case is one delayed transition. No action needed.

## Checked and found correct

- **Live-edit pickup** (`control/profile_executor.c:436-632`,
  `persist/live_profile.c:321-383`):
  - Pickup is generation-based. The candidate is read outside `s_exec.lock`;
    the window check and the swap run under the lock.
  - It is adopted only while RUNNING.
  - The window freezes `zone_mask`, earlier segments, and the running
    segment's kind and IO fields.
  - `remaining_s` is re-derived for non-blocking IO segments.
- **Ramp lock** (`control/profile_executor.c:944-983`):
  - It is one-sided: only a cold zone, or an invalid sensor, holds it.
  - ON_OFF and monitor-only zones are excluded through
    `profile_executor_zone_drives_run()`.
  - `actual_c` is NaN only when `sensor_ok` is false, and that is checked
    first.
  - The band comes from `exec_threshold(zi, 3)`.
- **Elapsed accounting:**
  - `exec_elapsed_accumulate` rejects NaN, negative and over-3600 s dt.
  - The dwell entry compares floats exactly, which is safe because both
    sides are set by assignment.
- **Pause and resume:**
  - Pause forces relays off, releases K4, hands relays to MANUAL, and writes
    the run-state note outside the lock.
  - Resume:
    - re-runs `relay_authority_start_blocked()`;
    - reclaims PROFILE;
    - resets `prev_control_tick` (no pause time in dt);
    - seeds bumpless with feedforward;
    - acquires heat via `heat_enable_acquire_since()` with an epoch guard,
      outside the lock.
- **Guard-9 watchdog and link loss** (`control/profile_executor.c:2285-2588`):
  - The stale-tick and relay-unknown checks run before the lock, and the lock
    wait is bounded.
  - A cached Pico TRIPPED report is acted on at any age.
  - 30 s of safety-link silence, or sustained PC-link loss, faults a RUNNING or
    PAUSED run.
  - The auto-pause uses the bounded pause and also aborts autotune.
  - `heat_enable_reconcile()` stays last in the loop.
- **PID, feedforward, heater_output:**
  - NaN and Inf are filtered at `thermo_channel_read`/`thermo_combine`. PID
    and feedforward run only when `sensor_ok`. heater_output maps NaN or
    duty <= 0 to 0, and a non-finite `u_ff` maps to 0.
  - dt comes from an unsigned tick subtraction and is never 0.
  - Anti-windup uses conditional integration plus I and raw-integral bounds.
    Ki changes rescale the integral, and D is on measurement.
  - Window quantization honours min-on and min-off.
  - `force_off` on pause bypasses the hold, as intended.
- **Autotune:**
  - The start refusals hold, including monitor-only and ON_OFF zones and a
    profile active on the zone.
  - The relay and heat-zone claims are taken together under `s_at.lock`.
  - Every abort and finalize exit calls `force_relays_off()`.
  - Lock order is `s_exec` before `s_at`.
  - Persisted gains go through the isfinite- and range-checked setters
    (`persist/zones_config_accessors.c:786-795`).
  - Every divide-by-K is guarded.
- **Aux and on/off:**
  - The not-RUNNING tick order is: aux OFF retry (skipped while PAUSED), then
    the K4 backstop, then `aux_fault_drop`.
  - The authority and stale gates refuse ON only, never OFF.
  - A failed OFF keeps the claim and sets pending.
  - Run-start refuses invalid aux rules and RELAY_IO segments aimed at an aux.
  - Target encoding `8+(relay-1)` is bounds-checked.
  - The zone-to-aux conversion's journal and rollback order is sound.
  - Monitor-only zones skip PID, ramp lock and heat guards, per owner decision
    2026-10-05.
  - Pico status and thermocouple reads stay outside `s_exec.lock`.

## Not covered in depth

- `pid_autotune.c` `tune_from_fopdt` with tau or L equal to 0.
- `adaptive_tune*.c`, `iter_tune.c`, `ramp_ident.c`,
  `ramp_transient_ident.c`, `autotune_engine_coupling.c`.
- `s8_rate_guard_estimate.c`, `on_off_trigger_decide.c`,
  `relay_off_tracker.c`.
- The aux rule checks in `profiles_validate.c`.

Their outputs reach persistence only through the validating setters.

### lowfx11 (2026-10-15)

- LOW-1 fixed: `profile_executor_resume()` resets the on/off trigger state, `on_off_actuated_on` and the actuation hold, as a fresh run does (aux outputs deliberately keep their state across a pause).
- LOW-2 fixed in the autotune engine: a blocked heat write while STEPPING/RELAY_* latches `blocked_during_measure`, and finalize then ABORTS ("heat was blocked during the measurement") instead of DONE. The thermal-guard zero-duty feed part of the finding is skipped (separate guard path, no safe host test).
- LOW-3 fixed: `pid_reseed_derivative` on the first valid sample after a sensor outage, so there is no derivative kick.
- INFO-1 (aux NaN temperature term): not done, optional.

- **LOW-2 zero-duty guard feed** (lowfin, 2026-10-15): left open deliberately. A blocked autotune measurement already ABORTS, and changing what `thermal_guard_tick()` is fed from autotune is a safety-guard input change with no safe host test; needs a deliberate review, not a LOW sweep.
