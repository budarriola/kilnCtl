# Element aging: schedule stretch, arrival-gated hold, adaptive tuning

Goal (owner 2026-11-19): firings still reach their target temperatures as the elements age. When the kiln cannot follow the schedule, stretch the heating time within a limit. Stretching is a setting and is on by default. PID values adjust after each firing. None of this may cause an unnecessary safety trip.

Simulation: `docs/sim/aging_sim.py` and `docs/sim/proposed.py`.
- The plant is lumped: 11 kW new, conduction plus radiation loss, 60 s dead time, 30 s sensor lag, 0.15 C thermocouple noise.
- The model mirrors guard 1, guard 4, the ramp lock and the ramp-assist stretch.
- Aging is simulated as an element power fraction from 1.0 down to 0.52 (resistance growth plus mains sag).
- The figures quoted below come from those runs.

## Findings (current firmware)

- **F1. Guard 4 and the ramp lock share the same 25 C band.**
  - Evidence: `EXEC_RAMP_LOCK_BAND_C` defaults to 25 C and `DRIFT_HYSTERESIS_C` is 25 C. The lock is evaluated before `target_c` advances (profile_executor.c:1024), and guard 4 runs after, against the advanced target (profile_executor.c:1830).
  - Effect: a zone held by the lock sits at a lag of 25 C plus one step of the advancing target, so guard 4 sees more than 25 C on every tick. Without sensor noise, DRIFT trips after 600 s. In the sim, the noiseless runs tripped DRIFT at 94 C (cone6_fast at power 0.72); with 0.15 C of noise the runs completed.
  - Config makes it worse: `ramp_lock_band_c` can be set larger than `guard_drift_hysteresis_c`, and then the trip is certain.
- **F2. Holds count while the zone is still up to 25 C low.** The dwell timer runs whenever `lock_ok` is true, so a hold can finish with the zone up to 25 C under target.
  - Sim: cone6_fast at power 0.65 peaked at 1207 C against a 1222 C target. cone04 at power 0.58 peaked at 1042 C against 1060 C. Both are about one cone low.
  - Dwell credit only shortens the hold. It never lengthens it.
- **F3. The ramp-assist stretch practically never engages.**
  - `ramp_assist_zone_lag_tick` resets `lag_held_s` on any tick where the zone is not lagging. The lock holds the zone right at the band edge, so noise drops it out of "lagging" well inside 30 s and `lag_sustained` never latches.
  - What extends a firing today is the lock alone, and the lock has no limit.
  - Ramp assist is also off by default.
- **F4. Guard 1's 0.5 C/min floor sets the aging limit.**
  - While the lock is held, `target_rate_c_per_s` is 0, so guard 1 requires 0.5 C/min (30 C/h) whenever duty is at least 0.5 and the zone is more than 3 C below setpoint.
  - An aged kiln approaching its maximum temperature drops below 30 C/h before it reaches target and trips HEATING_FAILED. Sim: cone10 at power 0.58 (asymptote 1286 C) tripped at 1205 C. cone6 at power 0.55 tripped at 1170 C.
  - The common failed-to-heat threshold on commercial controllers is about 12 F/h (6.7 C/h).
- **F5. Stretching with assist on weakens guard 1.** `profile_executor_guard_sanity_rate` narrows guard 1's requirement to the stretched rate. That rate is the lagging zone's own achieved rate, so a near-stalled zone makes guard 1 demand almost nothing. A rate of exactly 0 falls back to 0.5 C/min, but 0.01 C/min does not.
- **F6. Adaptive tuning is opt-in.** `adaptive_tune` already refines K_dc and the PID gains after each clean firing (blend 0.15, at most 20% change per run, lifetime envelope of baseline /5 to x5, one-click revert). `adaptive_tune_enabled` defaults to 0, so feedforward K_dc stays at the commissioning value as the elements age.

## Changes (amended after the Opus plan review, 2026-11-19)

Owner decisions on 2026-11-19: C7 yes, with a floor of 0.15 C/min. C6 on by default, existing boards included.

- **C1 (bug, independent of the stretch setting).**
  - Per tick and per zone, the executor sets guard 4's band to `max(drift_hysteresis_c, EXEC_RAMP_LOCK_BAND_C(zi) + 5 + off)` while the lock is held or the arrival gate waits, else `drift_hysteresis_c + off`, where `off = min(max(0, actual_c - raw_c), 30)`. The lock reads `actual_c` (calibrated and lead-compensated); guard 4 reads `raw_c`. The value goes in `guard_cfg_this_tick` only.
  - On config save, reject `ramp_lock_band_c > drift_hysteresis_c - 5` whenever both are set.
  - Host tests:
    - a lock held for 1200 s with lead greater than 0 does not trip;
    - a negative calibration offset does not trip;
    - band + 6 C above the extended band still trips (negtest).
- **C4.**
  - `lag_held_s` resets only after the zone has been continuously not lagging for 30 s (stretch on).
  - `profile_executor_guard_sanity_rate` never lowers guard 1's demand below the configured `sanity_rate` while the target rate comes from a stretch (fixes F5). C7 is the only path that lowers it.
- **C7.** Guard 1's rate becomes `min(sanity_rate, 0.15 C/min)` only when all of these hold:
  - stretch is on;
  - it is the climbing branch;
  - the zone is saturated (commanded duty at least 0.95);
  - the measurement is at least 900 C;
  - the lock is held or the stretch is active.
  Every other case keeps `sanity_rate`. A constant, with no UI for it.
- **C3. Arrival-gated hold (stretch on).** On a heating segment (ramp or step whose target is above the prior target), the dwell timer counts only when every zone that `profile_executor_zone_drives_run` counts satisfies one of:
  - `actual_c >= target - progress_band_c` (one-sided; overshoot never blocks), or
  - it is stalled: no duty condition (a cone-10 kiln runs near 100% duty at the top even when healthy). The signed rise over the last 300 s, measured as a slope between the first and last 30 s averages of a ten-bucket ring, is under 0.15 C/min (the C7 constant). Falling counts as stalled; a full 300 s window of valid readings is needed, restarted at dwell entry, segment change and pause/resume. When the gate opens by stall (zone not within the band), `target_shortfall_c` is recorded (largest shortfall over driving zones), same as budget exhaustion. Finish-short latch (owner 2026-11-19): when the gate opens because a zone is stalled short of target (stretch on, not budget exhaustion), that zone's guard 1 demands no rise for the remaining planned time of the current dwell only (no extension of the hold). Guards 2, 8 (frozen), the Pico guards and all others are unchanged. The latch clears as soon as the reading falls more than 10 C below the reading at latch time, duty falls below 0.5, the reading is invalid, the segment changes, or the run stops/resumes. `target_shortfall_c` and the per-run status flag `finish_short` are recorded; such runs are excluded from C6 learning.
  Dwell credit is not spent while C3 is active. The stretch budget is spent while the gate waits; once it runs out, the gate opens (today's behaviour) and the run records `target_shortfall_c`.
  - Stateless per tick.
  - The budget consumed (`stretch_total_s`) must survive resume (run_state).
  - Cooling segments are unchanged.
- **C5. Dropped** (review: it bypasses guard 1, and a dislodged thermocouple would overfire until Pico abs_max). With C7, a kiln that cannot manage 9 C/h near the top is a genuine failed-to-heat.
- **C2. Stretch setting:** "Stretch heating to reach targets", the existing `ramp_assist` key, default ON. A one-shot migration switches existing boards to ON once; after that, the user's choice persists. Add `stretch_max_pct` (default 100, range 0-300) to the same config, with backup/restore, web toggle and field, and the LCD status text "Stretching: +Xm". Accessor: `uint16_t ramp_assist_cfg_stretch_max_pct(void)`. Stretch off means today's behaviour exactly (C1 still applies).
- **C6. Adaptive tune ON by default.** One-shot blob migration sets `adaptive_tune_enabled=1` for zones with an accepted STEP model, and logs it. Backup round-trip test.
  - Freeze learning when the run's `k_dc_ratio` (learned / baseline) is below 0.6.
  - Raise an alert (not a trip) below 0.7: "elements weak".
  - Exclude runs whose C3 wait exceeded 10% of planned time.
  - Expose `k_dc_ratio` per zone in `/api/status`.

## Sim summary (0.15 C noise)

| case | today | C1-C5 (old C5) | C1-C4 + C7 (adopted) |
|---|---|---|---|
| cone6_fast, power 0.65 | done, peak 1207 | done, 1228 | done, 1228 |
| cone04, power 0.58 | done, peak 1042 | done, 1064 | same |
| cone6_med, power 0.55 | G1 trip at 1176 | done, best effort 1146 | done, 1210 |
| cone10, power 0.58 | G1 trip at 1205 | done, best effort 1193 | done, 1239 |
| healthy kiln, every profile | done | see note | see note |

## Pending
- [x] Opus review of this plan
- [x] C1, C3, C4, C7 (executor), host tests and negtests; review findings fixed (EA-08..EA-10)
- [x] C2 config default, migration, backup, web label, lag banner (LCD text not added: not trivial)
- [x] C6 adaptive default, `k_dc_ratio` (host-tested; see KNOWN_ISSUES "ELEMENT_AGING C2/C6 limits")
- [ ] Bench verification (4 W fixture: only C1 and C3 timing): C3 arrival hold and shortfall note, C1 no DRIFT trip with a held lock, run-state v2 record on the board
- [x] Opus review fixes EA-12..EA-16 (guard 4 band limited to lock/arrival, backup import clamps the lock band to drift - 5 (old backups restore), G1 defer while the stall window fills, 60 s duty clear, finish-short cross-check, adaptive freeze on result)
- [ ] Opus re-review of the fix diff
- 300% budget (stretch + arrival wait up to 2x the budget) is accepted as is (EA-17).

Notes: with stretch on, heating dwells start when the zone is within 3 C of target (or stalled), not when the setpoint arrives, so healthy firings may add a few minutes per heating dwell and reach peak more accurately.

C7 residual risk: C7 stays active during the arrival wait (owner-approved 0.15 floor; the top dwell is where aged kilns need it). A dislodged thermocouple reading low near the top with duty >= 0.95 is caught by guard 1 at 0.15 C/min, or by the stall release within 300 s, plus the Pico guards.

Finish-short residual risk: a thermocouple that stays put but reads low can overfire for at most the remaining hold time, bounded by the Pico guards. The `finish_short` flag is status-only (not in the run_state record).

Rollback: an older build reads the 4-byte `ramp_assist.dat` as undecodable, runs stretch OFF and refuses saves until that boot. run_state budget persistence is a breadcrumb only; pause/resume keeps the RAM budget.

Step segments (ramp_c_per_hr <= 0) count 0 toward the planned duration used for the stretch budget, so the budget is conservative. The arrival hold has its own budget (`arrival_wait_s`); with ramp stretch the total is capped at 2x stretch_max_pct of planned time.
