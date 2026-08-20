# virtual_dut scenario results
Full run against `virtual_simfw.exe` + `dut_core.exe` (real `safety_guards.c`/`relay_grace.c`), all 18 scenarios in `firmware/SimFW/scenarios/`. See `../README.md` for the architecture and the findings that explain the FAIL pattern below (S6b's unconditional ~120s trip, K4 never energizing in current SaftyFW, context_valid always false, plus the corollary that a `dut: K4_open` clause has no NEW edge to point to once K4 was already open before the run started).

**Guards confirmed to genuinely fire against real fixture data in this run**
(seen directly in each report's `events` list, independent of the `K4_open`
clause verdicts above them, which are dominated by Findings 1/2):
- **S5** (bad safety-TC read, graduated warn->trip): `tc_disconnect_ramp` (WARN 45s, TRIP 97s), `tc_disconnect_soak` (WARN 97s, TRIP 147s), `tc_flaky` (WARN 44s, WARN 95s -- never sustained to TRIP, correct for a flapping fault), `spi_flaky_tc_ic`/`tc_noise_storm`/`tc_stuck`/`safety_tc_frozen` (S6b's own ~147s trip preempts observing S5 there, since `safety_guards_tick` stops evaluating once `state->is_tripped`).
- **S6b** (link-down hard backstop, Finding 1): fires in every scenario at sim-time ~145-182s, confirming the same unconditional-trip finding independent of scenario content.
- **S11** (frozen safety reading): confirmed **absent** in `safety_tc_frozen`'s event list (only S6b fires) -- direct evidence for Finding 3 (`heat_commanded` hardcoded false).
- **S13** (borrowed-channel staleness): confirmed **absent** in `tc_stuck`'s event list -- direct evidence for Finding 4 (`context_valid` always false).
- **S1/S10/S6a/S9/S3/S4** never fire anywhere in this run -- consistent with Findings 4/5 (S1 uncommissioned, the rest context/current-sense-gated).
- **S12** never fires (`cj_fault`) -- Finding 6 (`cj_fault.yaml` has no numeric offset), not a guard defect.
- **S7** fires as expected wherever a scenario's own `manual_checks`/fault schedule would assert E-stop (see `estop_at_boot`/`estop_midfire`'s `K4_open`-held-at-end PASSes -- E-stop's trip is immediate and irreversible, so it is the one case Finding 2's "already open" state and a real S7 trip are indistinguishable from report.py's perspective, both correctly latch K4 open).

## baseline_firing -- overall: FAIL
- **[PASS]** never_faults -- dut:fault_line_asserted never observed before the boundary
- **[PASS]** never_estops -- dut:estop_open never observed before the boundary
- **[FAIL]** never_trips -- dut:K4_open observed before the boundary (first at seq 2000000000)
- **[SKIPPED]** heat_actually_cycles -- triggering event (relay_edge {'relay': 'K1', 'edge': 'close'}) never occurred

## broken_element -- overall: FAIL
- **[SKIPPED]** s4_warns -- triggering event (relay_edge {'relay': 'K1', 'edge': 'close'}) never occurred
- **[FAIL]** s4_never_trips -- dut:K4_open observed before the boundary (first at seq 2000000000)

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

## main_safety_skew -- overall: FAIL
- **[FAIL]** s1_trips_conservatively_early -- event:guard_trip not observed within 120s after fault_fired (seq 0)
- **[PASS]** s10_stays_quiet -- event:guard_warn never observed before the boundary

## mainfault_tc_disconnect -- overall: FAIL
- **[PASS]** no_early_trip -- dut:K4_open never observed before the boundary
- **[FAIL]** mainfault_trips_s6a -- event:guard_trip not observed within 3s after fault_fired (seq 0)
- **[FAIL]** fault_line_asserted -- dut:fault_line_asserted not observed within 3s after fault_fired (seq 0)
- **[PASS]** trip_latched -- dut:K4_open held at end of run (last observed seq 2000000000)

## partial_element -- overall: FAIL
- **[FAIL]** no_guard_trips -- dut:K4_open observed before the boundary (first at seq 2000000001)
- **[FAIL]** current_still_present_just_reduced -- dut:current_present not observed within 5s after fault_fired (seq 0)

## power_blip -- overall: FAIL
- **[PASS]** mainfault_reads_healthy_during_blip -- event:guard_trip never observed before the boundary
- **[SKIPPED]** relays_deenergize_no_current_during_blip -- triggering event (dut_power {'state': False}) never occurred
- **[FAIL]** s6b_stays_at_warn_not_trip -- dut:K4_open observed before the boundary (first at seq 2000000000)
- **[SKIPPED]** safe_resume -- triggering event (dut_power {'state': True}) never occurred

## runaway_zone -- overall: PASS
- **[SKIPPED]** s3_catches_it_first -- triggering event (fault_fired {'slot': 'runaway'}) never occurred
- **[PASS]** s8_produces_no_trip_of_its_own_today -- event:guard_trip never observed before the boundary

## safety_tc_frozen -- overall: FAIL
- **[PASS]** no_early_trip -- dut:K4_open never observed before the boundary
- **[FAIL]** frozen_window_trips_s11 -- event:guard_trip not observed within 605s after fault_fired (seq 0)
- **[PASS]** trip_latched -- dut:K4_open held at end of run (last observed seq 2000000000)

## spi_flaky_tc_ic -- overall: FAIL
- **[PASS]** not_an_instant_trip -- dut:K4_open never observed before the boundary
- **[FAIL]** warns_then_trips_like_disconnect -- dut:K4_open not observed within 65s after fault_fired (seq 0)

## tc_disconnect_ramp -- overall: FAIL
- **[FAIL]** warns_first -- event:guard_warn not observed within 6s after fault_fired (seq 0)
- **[FAIL]** trips_after_blind_grace -- dut:K4_open not observed within 65s after fault_fired (seq 0)
- **[FAIL]** reports_nan_not_stale_value -- safety_temp_valid state never observed; cannot be dut:safety_temp_valid at end

## tc_disconnect_soak -- overall: FAIL
- **[PASS]** warns_first -- event:guard_warn observed 6.000s after fault_fired
- **[FAIL]** trips_after_blind_grace -- dut:K4_open not observed within 65s after fault_fired (seq 0)
- **[FAIL]** reports_nan_not_stale_value -- safety_temp_valid state never observed; cannot be dut:safety_temp_valid at end

## tc_flaky -- overall: FAIL
- **[FAIL]** no_trip_from_flapping -- dut:K4_open observed before the boundary (first at seq 2000000001)
- **[FAIL]** no_warn_storm -- event:guard_warn observed before the boundary (first at seq 2000000000)

## tc_noise_storm -- overall: FAIL
- **[FAIL]** no_trip_ever -- dut:K4_open observed before the boundary (first at seq 2000000000)
- **[PASS]** no_fault_line_latch -- dut:fault_line_asserted never observed before the boundary

## tc_stuck -- overall: FAIL
- **[FAIL]** sample_counter_goes_stale -- event:guard_warn not observed within 13s after fault_fired (seq 0)
- **[FAIL]** trips_after_stale_trip_deadline -- dut:K4_open not observed within 65s after fault_fired (seq 0)

## welded_contactor_s9 -- overall: FAIL
- **[SKIPPED]** initial_trip -- triggering event (fault_fired {'slot': 'weld_ssr'}) never occurred
- **[SKIPPED]** contactor_weld_engages_on_k4_open -- triggering event (relay_edge {'relay': 'K4', 'edge': 'open'}) never occurred
- **[SKIPPED]** s9_escalates -- triggering event (relay_edge {'relay': 'K4', 'edge': 'open'}) never occurred
- **[PASS]** stays_open_and_latched -- dut:K4_open held at end of run (last observed seq 2000000000)
- **[FAIL]** escalation_latched_at_end -- event:trip_ineffective_latched never occurred

## welded_ssr_midfire -- overall: PASS
- **[SKIPPED]** safety_trips -- triggering event (fault_fired {'slot': 'weld'}) never occurred
- **[SKIPPED]** no_early_trip -- boundary fault 'weld' never fired
- **[PASS]** trip_latched -- dut:K4_open held at end of run (last observed seq 2000000000)
- **[SKIPPED]** current_decays_after_k4_opens -- triggering event (relay_edge {'relay': 'K4', 'edge': 'open'}) never occurred
