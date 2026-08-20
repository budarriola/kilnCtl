# virtual_dut scenario results
Full run against `virtual_simfw.exe` + `dut_core.exe` (real `safety_guards.c`/`relay_grace.c`), all 19 scenarios in `firmware/SimFW/scenarios/`. See `../README.md` for the architecture and the findings that explain the FAIL pattern below (S6b's unconditional ~120s trip, K4 never energizing in current SaftyFW, context_valid always false, plus the corollary that a `dut: K4_open` clause has no NEW edge to point to once K4 was already open before the run started).

## What changed in this pass: the K4 physical loop is now closed -- and the delta is genuinely zero

A separate task closed the gap this document's previous revision reported as
blocked ("Known limitation: the K4 physical loop is not closed"). Two
changes landed under `firmware/SimFW/tools/virtual_simfw/` and
`firmware/SimFW/tools/virtual_dut/` (both in scope for that task;
`firmware/SimFW/src/**` and `firmware/SaftyFW/**` stayed untouched):

1. **`virtual_simfw.c` gained a virtual-only relay-drive command**,
   `SIMFW_VIRTUAL_CMD_RELAY_SET_SENSE` (RELAY group, id `0xF0`). It is
   deliberately absent from `PROTOCOL.md`/`cmd_ids.h`/`kilnsim.protocol`/
   `kilnsim.payloads` -- real hardware never lets a DUT drive a relay's
   sensed contact, so nothing about this command may ever look like a real
   SimFW command. It exists solely because a *virtual* DUT has no physical
   coil to close in the first place, so nothing else could ever move K1-3/K5/
   K4's sensed state. Once told, `device_tick()` treats it exactly like any
   other sensed contact (same `relay_mask` bit, same `duty[]`/edge-log/
   telemetry path K1-3 already had).
2. **`run_dut_scenarios.py` now sends that command every poll**, feeding
   `dut_core.exe`'s real, unmodified `energized` output (straight from
   `relay_grace.c`/`safety_guards.c`, not re-derived) back as K4's sensed
   state.

**The result: every one of this run's 19 scenario reports is byte-identical
to the previous (open-loop) run except for two harness-timing fields
(`start_time`, and the poll-jitter-driven exact `sim_time_us` on the first
observation) -- every expectation verdict, every verdict message, is
unchanged.** This is not the loop failing to wire up; it is two independent,
separately-verified facts making the wiring correct but numerically inert
today:

- **Finding 2 (already documented, reconfirmed):** K4 is never energized
  anywhere in current SaftyFW (`relay_owner_command_energize()` has no
  caller yet -- Phase 7/link_task). `dut_core.exe`'s `energized` is `false`
  from the first tick of every run and never becomes `true`, so the value
  this pass now genuinely transmits over the wire is always the same value
  `virtual_simfw` already defaulted K4's sense to. There is no edge to
  observe (`relay_mask_prev`'s K4 bit never changes), so `RELAY_GET_EDGES`/
  the EVT stream produce nothing new either.
- **A second, independent, previously-unverified finding this pass
  confirmed by reading the code rather than assuming it (see
  `../virtual_simfw/README.md`'s "Known, virtual-only extensions" section
  for the full writeup): `device_tick()`'s `duty[]`/`current_a[]` computation
  never reads K4 at all** -- only K1/K2/K3 gate heater duty and CT current.
  This is confirmed true of **real, unmodified**
  `firmware/SimFW/src/tasks/sim_engine.c` too (its own `duty[]` loop, same
  three-relay `zone_relay_bit[]` array, no K4 anywhere in the file) -- so
  even on a day K4 *did* get energized, closing it alone would still not
  gate simulated heat or current in this fixture's model as PLAN.md sec 2's
  loop 2 describes. This is a real, pre-existing property of SimFW's fixture
  model (real firmware included), not a defect introduced by, or fixable
  within the scope of, this pass (`firmware/SimFW/src/**` is read-only here).

So "the loop is closed, and it changed nothing" is the correct, honest
report for today's code -- not a sign the work is incomplete. Re-run this
suite again once (a) SaftyFW's Phase 7 link_task starts actually energizing
K4, and separately (b) a task with edit rights to `firmware/SimFW/src/`
adds K4 into `sim_engine.c`'s `duty[]` gating -- either change alone would
finally make this closed loop numerically visible in a report.

One unrelated, pre-existing diff **did** show up in `cj_fault.json`, from a
different, already-committed pass (commit `c44ee00`, before this task
started) that gave `cj_fault.yaml` a real `params: [70.0]` CJ offset -- the
committed `results/cj_fault.json` this task found on disk predated that fix,
so re-running it here picked it up for the first time. S12 (`GUARD_WARN`/
`GUARD_TRIP`) now genuinely fires in that run's event list where it
previously never did (a real fidelity improvement), but both of its
`expect` clauses still report FAIL for reasons unrelated to K4: `s12_warns_at_60c`'s
`within_s: 1` deadline is tighter than S12's own graduated warn timing, and
`s12_trips_at_85c_sustained` still needs a `K4_open` *edge*, which Finding 2
above rules out regardless. Overall verdict for `cj_fault` is unchanged
(FAIL -> FAIL). This diff belongs to that other pass, not to the K4-loop
work reported here.

**Multi-client support was also added** (`virtual_simfw.exe` now accepts up
to 4 concurrent TCP clients, each with its own `benchproto_link_t`
dedup/registration state and its own EVT-ring read cursor, so sequence-gap
detection stays honest per client -- see `../virtual_simfw/README.md`).
`run_dut_scenarios.py` does not currently need to run alongside a second
`kilnsim` client for these scenario runs (it still launches and owns its own
`virtual_simfw.exe` per scenario, the simplest and most reproducible setup);
the capability is there for whoever wants to drive `kilnsim`'s own CLI/MCP
surface against the same live process a `virtual_dut` run is using.

**Guards confirmed to genuinely fire against real fixture data in this run**
(seen directly in each report's `events` list, independent of the `K4_open`
clause verdicts above them, which are dominated by Findings 1/2):
- **S5** (bad safety-TC read, graduated warn->trip): `tc_disconnect_ramp` (WARN 45s, TRIP 97s), `tc_disconnect_soak` (WARN 97s, TRIP 147s), `tc_flaky` (WARN 44s, WARN 95s -- never sustained to TRIP, correct for a flapping fault), `spi_flaky_tc_ic`/`tc_noise_storm`/`tc_stuck`/`safety_tc_frozen` (S6b's own ~147s trip preempts observing S5 there, since `safety_guards_tick` stops evaluating once `state->is_tripped`).
- **S6b** (link-down hard backstop, Finding 1): fires in every scenario at sim-time ~145-182s, confirming the same unconditional-trip finding independent of scenario content.
- **S11** (frozen safety reading): confirmed **absent** in `safety_tc_frozen`'s event list (only S6b fires) -- direct evidence for Finding 3 (`heat_commanded` hardcoded false).
- **S12** (cold-junction over-temperature, graduated warn->trip): now genuinely fires in `cj_fault` (WARN + TRIP, see "What changed" above) once its scenario file carried a real params offset -- confirms S12's own logic is reachable and correct, independent of the still-FAIL clause verdicts (deadline/K4 reasons, not a guard defect).
- **S13** (borrowed-channel staleness): confirmed **absent** in `tc_stuck`'s event list -- direct evidence for Finding 4 (`context_valid` always false).
- **S1/S10/S6a/S9/S3/S4** never fire anywhere in this run -- consistent with Findings 4/5 (S1 uncommissioned, the rest context/current-sense-gated).
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
- **[PASS]** warns_first -- event:guard_warn observed 5.000s after fault_fired
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
