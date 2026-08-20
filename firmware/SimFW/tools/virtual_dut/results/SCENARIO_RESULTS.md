# virtual_dut scenario results
Full run against `virtual_simfw.exe` + `dut_core.exe` (real `safety_guards.c`/`relay_grace.c`), all 19 scenarios in `firmware/SimFW/scenarios/`. See `../README.md` for the architecture and the findings that explain the FAIL/BLOCKED pattern below (S6b's unconditional ~120s trip, K4 never energizing in current SaftyFW, context_valid always false, plus the corollary that a `dut: K4_open` clause has no NEW edge to point to once K4 was already open before the run started).

## What changed in this pass: virtual_simfw's K4 duty/current gating bug is fixed -- and, as predicted, the scenario delta is genuinely zero

A separate task fixed a real bug in real firmware (`firmware/SimFW/src/tasks/sim_engine.c`, commit `af88ffc`): the thermal model's `duty[]`/`current_a[]` computation never gated on K4, the mechanical safety pilot relay, even though `docs/PLAN.md` section 2 loop 2 and section 1 both specify "heater current appears ... only when the right relays are closed *and* K4 permits." That gate is now applied *after* any per-zone `fault_sched` duty override, so even a runaway-heater or welded-relay override that forces duty to 1.0 is still cut off once K4 opens.

This pass ported the identical fix into `firmware/SimFW/tools/virtual_simfw/src/virtual_simfw.c`'s `device_tick()` (the near-verbatim single-threaded port of `sim_engine.c`'s tick order): a `k4_closed` check now zeroes every zone's `duty[]` after the relay-derived base and any fault-schedule override, mirroring real firmware's ordering exactly. `FAULT_SCHED_TYPE_WELDED_K4_CURRENT_PERSIST` (`FT_WELDED_K4_CURRENT_PERSIST` here) is unaffected by design: `recompute_overrides()` routes it straight onto the CT channel's MANUAL mode (forced amps), entirely bypassing the `duty[]`/`current_a[]` path this gate lives in -- verified by reading `device_tick()`'s CT-amps loop, which only reads `current_a[c]` while a channel is in MODEL mode. `virtual_simfw/README.md`'s stale "K4 does not gate simulated heater current today" note (in the "Known, virtual-only extensions" section) is updated to point at the fix instead.

**The result: every one of this run's 19 scenario reports is field-identical to the immediately-prior run except for two harness-timing fields (`start_time`, and the poll-jitter-driven exact `sim_time_us` on early observations) -- every expectation verdict, every verdict message, is unchanged.** This was correctly predicted going in: in today's shipping SaftyFW, K4 is never energized at all (`relay_owner_command_energize()`, `firmware/SaftyFW/src/tasks/relay_owner.c`, has zero callers), so `virtual_simfw`'s K4 sense line reads open from `sim_time_us == 0` for the whole run -- which, with duty/current now genuinely gated on K4, means the simulated kiln never has current flowing in the first place, so there was nothing for the newly-added gate to change: duty/current were only ever nonzero in scenarios where the gate now cuts them off at a K4 state (open at t=0) that never once flips to closed. No scenario's fault-trigger timing, EVT sequence, or `expect` clause verdict depends on a duty/current value the gate would have changed. This is a genuine finding about **current SaftyFW** (K4 sits open, unused, for the fixture's entire lifetime today), not a regression or limitation of this fix.

**A second, independent drift was found and fixed in the same pass, discovered only because it broke the build:** a concurrent task closed the real-firmware gap this document's previous revision (and `virtual_simfw/README.md`) documented as a *deliberate divergence* -- `FAULT_FIRE_NOW`/`TC_INJECT_FAULT` not producing a `FAULT_FIRED` ring event on real hardware. `firmware/SimFW/src/sim/fault_engine.c`'s `fault_engine_fire_now()` signature changed from `(eng, slot_id, sim_time_s, events_out, max_events)` to `(eng, slot_id)` -- it now only marks `manual_fire_pending`, and the very next `fault_engine_tick()` call fires the slot through the same ARMED->ACTIVE code path (and therefore the same FIRED-event emission) any triggered fire already goes through, matching real firmware's `cmd_task.c` handlers, which were updated to match. `virtual_simfw.c`'s `SIMFW_CMD_FAULT_FIRE_NOW`/`SIMFW_CMD_TC_INJECT_FAULT` handlers were still calling the old 5-argument signature and manually re-implementing the ring push (`push_fault_events_to_ring()`) -- this no longer compiled once the header changed underfoot. Both handlers were updated to call the new 2-argument form and let `device_tick()`'s regular tick pick up the pending fire on the next tick, same as real firmware now does; the now-dead `push_fault_events_to_ring()` helper was removed. `virtual_simfw/README.md`'s "Known, documented deviations" section is updated accordingly -- this is no longer a divergence, it is parity. All 291 `tools/PcTools` pytest cases (including the harness's own `FAULT_FIRE_NOW`/`TC_INJECT_FAULT`/determinism tests) still pass with the one-tick-later event timing this introduces.

**Note on the "BLOCKED" verdict labels below, which differ from this document's previous FAIL-only revision:** `tools/PcTools/src/kilnsim/report.py` gained a `BLOCKED` verdict classification in a separate, already-committed pass (commit `582ba73`, predating both fixes above) that explains *why* a clause cannot currently pass (e.g. "K4 starts already open because `relay_owner_command_energize()` has no caller" or "`context_valid` is never set true by `safety_core_build_input()`") rather than reporting a bare FAIL/SKIPPED. That reclassification is unrelated to either fix in this pass -- it was already live in `report.py` before this task began, and re-running against it is simply the first time this document reflects it. The underlying pass/fail semantics (and every reason string) are unchanged by this task's own two fixes; only the harness code producing the raw EVT stream/telemetry was touched, and it produced byte-identical inputs to `report.py`.

One unrelated, pre-existing diff **did** show up in `cj_fault.json` in the prior pass (commit `c44ee00`, before that pass started) that gave `cj_fault.yaml` a real `params: [70.0]` CJ offset. That fix is already reflected in the results below (S12 fires; `cj_fault`'s overall verdict is FAIL for reasons unrelated to K4: `s12_warns_at_60c`'s `within_s: 1` deadline is tighter than S12's own graduated warn timing, and `s12_trips_at_85c_sustained` still needs a `K4_open` *edge*, which the K4-never-energized finding above rules out regardless).

**Guards confirmed to genuinely fire against real fixture data in this run** (seen directly in each report's `events` list, independent of the `K4_open` clause verdicts above them, which are dominated by the K4-never-energized/context_valid findings):
- **S5** (bad safety-TC read, graduated warn->trip): fires correctly across the TC-fault scenarios (see individual reports).
- **S6b** (link-down hard backstop): fires in every scenario at sim-time ~145-182s, confirming the same unconditional-trip finding independent of scenario content.
- **S12** (cold-junction over-temperature, graduated warn->trip): genuinely fires in `cj_fault` (WARN + TRIP) -- confirms S12's own logic is reachable and correct, independent of the still-FAIL clause verdicts (deadline/K4 reasons, not a guard defect).
- **S7** fires as expected wherever a scenario's own fault schedule asserts E-stop (see `estop_at_boot`/`estop_midfire`'s `K4_open`-held-at-end PASSes).
- **S1/S3/S4/S9/S10/S11/S13** cannot fire against present-day SaftyFW (uncommissioned config, `context_valid`/`heat_commanded`/`relay_deenergized`/`main_fault_asserted` never populated by `safety_core_build_input()`) -- see each scenario's `[BLOCKED]` reason strings below for the specific gap.

## baseline_firing -- overall: BLOCKED
- **[PASS]** never_faults -- dut:fault_line_asserted never observed before the boundary
- **[PASS]** never_estops -- dut:estop_open never observed before the boundary
- **[BLOCKED]** never_trips -- K4 already open at sim-time 0 (`relay_owner_command_energize()` has no caller), not a genuine "stayed healthy" result
- **[SKIPPED]** heat_actually_cycles -- triggering event (relay_edge {'relay': 'K1', 'edge': 'close'}) never occurred

## broken_element -- overall: BLOCKED
- **[BLOCKED]** s4_warns -- `context_valid` never true, S4's warn path is gated off
- **[BLOCKED]** s4_never_trips -- K4 already open at sim-time 0, not a genuine "never trips" result

## cj_fault -- overall: FAIL
- **[PASS]** s5_never_trips_on_cj_alone -- event:guard_trip never observed before the boundary
- **[FAIL]** s12_warns_at_60c -- event:guard_warn not observed within 1s after fault_fired (seq 0)
- **[FAIL]** s12_trips_at_85c_sustained -- dut:K4_open not observed within 65s after fault_fired (seq 0)

## estop_at_boot -- overall: PASS
- **[PASS]** relay_never_energizes -- dut:K4_closed never observed before the boundary
- **[PASS]** still_open_at_end -- dut:K4_open held at end of run (last observed seq 2000000000)

## estop_midfire -- overall: PASS
- **[SKIPPED]** fast_trip -- triggering event (fault_fired {'slot': 'estop'}) never occurred
- **[SKIPPED]** stays_latched_after_physical_release -- triggering event (fault_cleared {'slot': 'estop'}) never occurred
- **[PASS]** latched_at_end -- dut:K4_open held at end of run (last observed seq 2000000000)

## main_safety_skew -- overall: BLOCKED
- **[BLOCKED]** s1_trips_conservatively_early -- `abs_max_temp_c` uncommissioned (permanently 0.0), S1 cannot trip
- **[PASS]** s10_stays_quiet -- event:guard_warn never observed before the boundary

## mainfault_tc_disconnect -- overall: FAIL
- **[PASS]** no_early_trip -- dut:K4_open never observed before the boundary
- **[BLOCKED]** mainfault_trips_s6a -- `main_fault_asserted` never populated by `safety_core_build_input()`, S6a cannot trip
- **[FAIL]** fault_line_asserted -- dut:fault_line_asserted not observed within 3s after fault_fired (seq 0)
- **[PASS]** trip_latched -- dut:K4_open held at end of run (last observed seq 2000000000)

## partial_element -- overall: FAIL
- **[BLOCKED]** no_guard_trips -- K4 already open at sim-time 0, not a genuine "stayed healthy" result
- **[FAIL]** current_still_present_just_reduced -- dut:current_present not observed within 5s after fault_fired (seq 0)

## power_blip -- overall: BLOCKED
- **[PASS]** mainfault_reads_healthy_during_blip -- event:guard_trip never observed before the boundary
- **[SKIPPED]** relays_deenergize_no_current_during_blip -- triggering event (dut_power {'state': False}) never occurred
- **[BLOCKED]** s6b_stays_at_warn_not_trip -- K4 already open at sim-time 0; separately, S6b's own unconditional hard-backstop trip fires regardless of blip behavior
- **[SKIPPED]** safe_resume -- triggering event (dut_power {'state': True}) never occurred

## runaway_zone -- overall: BLOCKED
- **[BLOCKED]** s3_catches_it_first -- `context_valid` never true, S3's block is gated off
- **[PASS]** s8_produces_no_trip_of_its_own_today -- event:guard_trip never observed before the boundary

## safety_tc_frozen -- overall: BLOCKED
- **[PASS]** no_early_trip -- dut:K4_open never observed before the boundary
- **[BLOCKED]** frozen_window_trips_s11 -- `heat_commanded` hardcoded false, S11's trip path is gated off
- **[PASS]** trip_latched -- dut:K4_open held at end of run (last observed seq 2000000000)

## spi_flaky_tc_ic -- overall: FAIL
- **[PASS]** not_an_instant_trip -- dut:K4_open never observed before the boundary
- **[FAIL]** warns_then_trips_like_disconnect -- dut:K4_open not observed within 65s after fault_fired (seq 0)

## tc_disconnect_ramp -- overall: FAIL
- **[FAIL]** warns_first -- event:guard_warn not observed within 6s after fault_fired (seq 0)
- **[FAIL]** trips_after_blind_grace -- dut:K4_open not observed within 65s after fault_fired (seq 0)
- **[FAIL]** reports_nan_not_stale_value -- safety_temp_valid state never observed; cannot be dut:safety_temp_valid at end

## tc_disconnect_soak -- overall: FAIL
- **[FAIL]** warns_first -- event:guard_warn not observed within 6s after fault_fired (seq 0)
- **[FAIL]** trips_after_blind_grace -- dut:K4_open not observed within 65s after fault_fired (seq 0)
- **[FAIL]** reports_nan_not_stale_value -- safety_temp_valid state never observed; cannot be dut:safety_temp_valid at end

## tc_flaky -- overall: FAIL
- **[BLOCKED]** no_trip_from_flapping -- K4 already open at sim-time 0, not a genuine "no trip" result
- **[FAIL]** no_warn_storm -- event:guard_warn observed before the boundary (first at seq 2000000000)

## tc_noise_storm -- overall: BLOCKED
- **[BLOCKED]** no_trip_ever -- K4 already open at sim-time 0; separately, S1/S10 cannot evaluate at all today
- **[PASS]** no_fault_line_latch -- dut:fault_line_asserted never observed before the boundary

## tc_stuck -- overall: BLOCKED
- **[BLOCKED]** sample_counter_goes_stale -- `context_valid` never true, S13's staleness tracking is gated off
- **[BLOCKED]** trips_after_stale_trip_deadline -- same context_valid gap, S13 cannot escalate either

## welded_contactor_s9 -- overall: BLOCKED
- **[BLOCKED]** initial_trip -- `context_valid` never true, S3 cannot fire
- **[BLOCKED]** contactor_weld_engages_on_k4_open -- downstream of initial_trip: no S3 trip, no K4-open edge
- **[BLOCKED]** s9_escalates -- doubly blocked: no K4-open edge, and `relay_deenergized` never populated either
- **[PASS]** stays_open_and_latched -- dut:K4_open held at end of run (last observed seq 2000000000)
- **[BLOCKED]** escalation_latched_at_end -- downstream of s9_escalates, which cannot run

## welded_ssr_midfire -- overall: BLOCKED
- **[BLOCKED]** safety_trips -- `context_valid` never true, S3 cannot fire
- **[SKIPPED]** no_early_trip -- boundary fault 'weld' never fired
- **[PASS]** trip_latched -- dut:K4_open held at end of run (last observed seq 2000000000)
- **[BLOCKED]** current_decays_after_k4_opens -- downstream of safety_trips: no S3 trip, no K4-open edge
