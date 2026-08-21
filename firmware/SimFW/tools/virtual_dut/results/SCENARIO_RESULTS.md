# virtual_dut scenario results

Full run against `virtual_simfw.exe` + `dut_core.exe` (real, unmodified
`safety_guards.c` / `relay_grace.c` / `snapshots.h`), all 19 scenarios in
`firmware/SimFW/scenarios/`. See `../README.md` for the architecture and the
findings behind the PASS/FAIL/BLOCKED pattern below.

---

## This revision: the fixture caught up with SaftyFW commit `f304392`

`f304392` wired `safety_core_build_input()` to real producers for
`context_valid`, `zone_count`/`max_zone_setpoint_c`/`nearest_zone_measured_c`,
`any_current_present`, `relay_commanded_recently`/`_continuously` and
`link_up`, and gave `relay_owner_command_energize()` its first caller.
**That commit's own re-run of this suite produced byte-identical verdicts and
was recorded here as "zero delta, by design" — which was true of the fixture
and false of the firmware.** `dut_core/main.c` is an independent, hand-written
stand-in for `safety_core_build_input()` (the real function is
FreeRTOS/pico-sdk shaped and cannot be host-compiled at all), and it was still
hardcoding the pre-`f304392` zero/false values. The fixture was mirroring code
that no longer existed.

This pass fixed that, and deliberately did it by **compiling the real helpers
rather than mirroring them again**: `dut_core/main.c` now `#include`s
`firmware/SaftyFW/src/snapshots.h` and calls the real `context_reduce_zones()`
and `current_any_present()`. Only the FreeRTOS-shaped glue (the staleness
test, the `correlation_window_s` comparison, the struct assembly) is
hand-written, and each piece is annotated with the `safety_core.c` line it
mirrors. `run_dut_scenarios.py` builds a `SAFETY_CMD_PUSH_CONTEXT`-equivalent
per poll from `virtual_simfw` telemetry — the same physical facts a real ESP
would report — rather than inventing any.

### Measured delta (same 19 scenarios, same seeds, before vs after)

| | Before | After |
|---|---|---|
| Scenarios where S6b's unconditional hard-backstop trip fired | **16 of 19** | **0 of 19** |
| Polls presenting `context_valid = true` | 0 | **1 229 of 1 229 (every poll of every scenario)** |
| Polls presenting `link_up = true` | 0 | **1 229 of 1 229** |
| Guards force-reset every tick by `!context_valid` (S2/S3/S4/S10/S13) | all 5 | **none** |
| Polls presenting `any_current_present = true` | 0 | **0** (see below — a fixture gap, not a firmware one) |
| Expectation verdicts changed | — | **none** |

**Guards that can now fire and could not before:** S2, S3, S4, S10 (all four
were force-reset every tick by `context_valid == false`). S6b changed
character rather than reachability: it went from *tripping unconditionally
~120 s into every run* to *not tripping at all on a healthy link*.

**Guards that are still dormant, and why each is a different kind of gap:**

| Guard | Still blocked by | Kind of gap |
|---|---|---|
| S1 | `abs_max_temp_c` uncommissioned | commissioning (Phase 9) |
| S6a | `main_fault_asserted` never populated | one-line wiring omission in `safety_core.c` |
| S9 | `relay_deenergized` never computed | wiring |
| S11 | `heat_commanded` hardcoded false | wiring (Phase 6) |
| S13 | `borrowed_zone_index` does not exist; `tc_source` defaults to `OWN_J7` | commissioning — deliberately left alone rather than guessing a zone index |

**Guards that are reachable but this fixture cannot provoke** — read a quiet
result for these as "no stimulus", never as evidence about the guard:

- **S2** — nothing in `kilnsim`/`virtual_simfw` carries a zone setpoint (a
  scenario's `dut: {profile: cone6_fast}` names a KilnFW profile nothing here
  executes). `setpoint_c` is sent as NaN — unknown, never a guessed ceiling —
  so `tc_c > max_setpoint + margin` is false rather than trippable against a
  fabricated number.
- **S3 / S4** — `sim_engine.c` gates heater duty and CT current on K4, and K4
  never closes: `relay_owner_command_energize()` now has a caller, but it is
  reached only from an explicit operator/PC command
  (`SAFETY_CMD_REQUEST_ENABLE` → KilnFW's `uart_bridge.c` →
  PcTools' `safety_request_enable`), never automatically by a running profile,
  and no scenario models that step. `dut_core.exe` accepts an `ENABLE` command
  for whoever writes the first one that does.

### The one guard this pass turned from vacuous into real

`main_safety_skew`'s **`s10_stays_quiet`** — an +80 °C safety-TC skew must
stay under S10's `tc_disagreement_c` = 200 °C. It was a PASS before, but a
vacuous one: S10 was force-reset every tick. It is now a genuine
anti-nuisance PASS, evaluated against real fixture data at
`context_valid = true` on all 39 polls, with one eligible zone
(`small_kiln` is single-zone).

### The one event-stream change that needs reading carefully

`tc_flaky` previously showed a `GUARD_TRIP {S6b}` and now shows a
`GUARD_TRIP {S5}` at t≈247 s (plus one extra S5 warn). **This is not a newly
discovered S5 defect, and the scenario's expectation is not wrong.** It is
this harness's own documented batch-ticking approximation: at the default
`--poll-interval 0.25` and `timescale: 10`, one batch replays a single TC
sample across ~2.5 s of sim time, which cannot resolve `tc_flaky`'s 900 ms
bad / 900 ms good alternation at all — so a batch landing in a bad phase
synthesizes ~25 consecutive bad reads and clears S5's 10-read **and** 5 s
bars, which a real 100 ms sampler would never see. Verified directly:
re-running the same scenario at `--poll-interval 0.02` (0.2 s of sim per
batch) makes the trip disappear. S6b's own trip was simply latching first
before, masking it.

The `no_warn_storm` FAIL underneath it is **pre-existing and unchanged by
this pass**, and it survives the finer poll interval too (S5 warns still
appear at `--poll-interval 0.02`). That is a separate open question about
`tc_flaky`'s duty split versus S5's bars in this fixture, not something this
change caused or should paper over. **Do not lower `--poll-interval` only for
`tc_flaky` and call it green** — either fix the batching or record that this
scenario requires a finer poll to be meaningful.

---

## Per-scenario results

`inputs:` lines report how many polls presented each guard precondition —
they decide nothing, they just let "S3 stayed quiet" be distinguished from
"S3 was never evaluated".

## baseline_firing -- overall: BLOCKED

_Guard inputs presented this run: 32 polls, context_valid 32, link_up 32, any_current_present 0, max eligible zones 3._

- **[PASS]** never_faults: dut:fault_line_asserted never observed before the boundary
- **[PASS]** never_estops: dut:estop_open never observed before the boundary
- **[BLOCKED]** never_trips: dut:K4_open observed before the boundary (first at seq 2000000000) _(see this scenario's own `blocked_on:` for the current reason)_
- **[SKIPPED]** heat_actually_cycles: triggering event (relay_edge {'relay': 'K1', 'edge': 'close'}) never occurred

## broken_element -- overall: BLOCKED

_Guard inputs presented this run: 79 polls, context_valid 79, link_up 79, any_current_present 0, max eligible zones 3._

- **[BLOCKED]** s4_warns: triggering event (relay_edge {'relay': 'K1', 'edge': 'close'}) never occurred _(see this scenario's own `blocked_on:` for the current reason)_
- **[BLOCKED]** s4_never_trips: dut:K4_open observed before the boundary (first at seq 2000000000) _(see this scenario's own `blocked_on:` for the current reason)_

## cj_fault -- overall: FAIL

_Guard inputs presented this run: 53 polls, context_valid 53, link_up 53, any_current_present 0, max eligible zones 3._

- **[PASS]** s5_never_trips_on_cj_alone: event:guard_trip never observed before the boundary
- **[FAIL]** s12_warns_at_60c: event:guard_warn not observed within 1s after fault_fired (seq 0)
- **[FAIL]** s12_trips_at_85c_sustained: dut:K4_open not observed within 65s after fault_fired (seq 0)

## estop_at_boot -- overall: PASS

_Guard inputs presented this run: 32 polls, context_valid 32, link_up 32, any_current_present 0, max eligible zones 3._

- **[PASS]** relay_never_energizes: dut:K4_closed never observed before the boundary
- **[PASS]** still_open_at_end: dut:K4_open held at end of run (last observed seq 2000000000)

## estop_midfire -- overall: PASS

_Guard inputs presented this run: 77 polls, context_valid 77, link_up 77, any_current_present 0, max eligible zones 3._

- **[SKIPPED]** fast_trip: triggering event (fault_fired {'slot': 'estop'}) never occurred
- **[SKIPPED]** stays_latched_after_physical_release: triggering event (fault_cleared {'slot': 'estop'}) never occurred
- **[PASS]** latched_at_end: dut:K4_open held at end of run (last observed seq 2000000000)

## main_safety_skew -- overall: BLOCKED

_Guard inputs presented this run: 39 polls, context_valid 39, link_up 39, any_current_present 0, max eligible zones 1._

- **[BLOCKED]** s1_trips_conservatively_early: event:guard_trip not observed within 120s after fault_fired (seq 0) _(see this scenario's own `blocked_on:` for the current reason)_
- **[PASS]** s10_stays_quiet: event:guard_warn never observed before the boundary

## mainfault_tc_disconnect -- overall: FAIL

_Guard inputs presented this run: 32 polls, context_valid 32, link_up 32, any_current_present 0, max eligible zones 3._

- **[PASS]** no_early_trip: dut:K4_open never observed before the boundary
- **[BLOCKED]** mainfault_trips_s6a: event:guard_trip not observed within 3s after fault_fired (seq 0) _(see this scenario's own `blocked_on:` for the current reason)_
- **[FAIL]** fault_line_asserted: dut:fault_line_asserted not observed within 3s after fault_fired (seq 0)
- **[PASS]** trip_latched: dut:K4_open held at end of run (last observed seq 2000000000)

## partial_element -- overall: FAIL

_Guard inputs presented this run: 21 polls, context_valid 21, link_up 21, any_current_present 0, max eligible zones 3._

- **[BLOCKED]** no_guard_trips: dut:K4_open observed before the boundary (first at seq 2000000000) _(see this scenario's own `blocked_on:` for the current reason)_
- **[FAIL]** current_still_present_just_reduced: dut:current_present not observed within 5s after fault_fired (seq 0)

## power_blip -- overall: BLOCKED

_Guard inputs presented this run: 85 polls, context_valid 85, link_up 85, any_current_present 0, max eligible zones 3._

- **[PASS]** mainfault_reads_healthy_during_blip: event:guard_trip never observed before the boundary
- **[SKIPPED]** relays_deenergize_no_current_during_blip: triggering event (dut_power {'state': False}) never occurred
- **[BLOCKED]** s6b_stays_at_warn_not_trip: dut:K4_open observed before the boundary (first at seq 2000000000) _(see this scenario's own `blocked_on:` for the current reason)_
- **[SKIPPED]** safe_resume: triggering event (dut_power {'state': True}) never occurred

## runaway_zone -- overall: BLOCKED

_Guard inputs presented this run: 83 polls, context_valid 83, link_up 83, any_current_present 0, max eligible zones 3._

- **[BLOCKED]** s3_catches_it_first: triggering event (fault_fired {'slot': 'runaway'}) never occurred _(see this scenario's own `blocked_on:` for the current reason)_
- **[PASS]** s8_produces_no_trip_of_its_own_today: event:guard_trip never observed before the boundary

## safety_tc_frozen -- overall: BLOCKED

_Guard inputs presented this run: 221 polls, context_valid 221, link_up 221, any_current_present 0, max eligible zones 3._

- **[PASS]** no_early_trip: dut:K4_open never observed before the boundary
- **[BLOCKED]** frozen_window_trips_s11: event:guard_trip not observed within 605s after fault_fired (seq 0) _(see this scenario's own `blocked_on:` for the current reason)_
- **[PASS]** trip_latched: dut:K4_open held at end of run (last observed seq 2000000000)

## spi_flaky_tc_ic -- overall: FAIL

_Guard inputs presented this run: 52 polls, context_valid 52, link_up 52, any_current_present 0, max eligible zones 3._

- **[PASS]** not_an_instant_trip: dut:K4_open never observed before the boundary
- **[FAIL]** warns_then_trips_like_disconnect: dut:K4_open not observed within 65s after fault_fired (seq 0)

## tc_disconnect_ramp -- overall: FAIL

_Guard inputs presented this run: 56 polls, context_valid 56, link_up 56, any_current_present 0, max eligible zones 3._

- **[FAIL]** warns_first: event:guard_warn not observed within 6s after fault_fired (seq 0)
- **[FAIL]** trips_after_blind_grace: dut:K4_open not observed within 65s after fault_fired (seq 0)
- **[FAIL]** reports_nan_not_stale_value: safety_temp_valid state never observed; cannot be dut:safety_temp_valid at end

## tc_disconnect_soak -- overall: FAIL

_Guard inputs presented this run: 77 polls, context_valid 77, link_up 77, any_current_present 0, max eligible zones 3._

- **[FAIL]** warns_first: event:guard_warn not observed within 6s after fault_fired (seq 0)
- **[FAIL]** trips_after_blind_grace: dut:K4_open not observed within 65s after fault_fired (seq 0)
- **[FAIL]** reports_nan_not_stale_value: safety_temp_valid state never observed; cannot be dut:safety_temp_valid at end

## tc_flaky -- overall: FAIL

_Guard inputs presented this run: 36 polls, context_valid 36, link_up 36, any_current_present 0, max eligible zones 3._

- **[BLOCKED]** no_trip_from_flapping: dut:K4_open observed before the boundary (first at seq 2000000000) _(see this scenario's own `blocked_on:` for the current reason)_
- **[FAIL]** no_warn_storm: event:guard_warn observed before the boundary (first at seq 2000000001)

## tc_noise_storm -- overall: BLOCKED

_Guard inputs presented this run: 33 polls, context_valid 33, link_up 33, any_current_present 0, max eligible zones 3._

- **[BLOCKED]** no_trip_ever: dut:K4_open observed before the boundary (first at seq 2000000000) _(see this scenario's own `blocked_on:` for the current reason)_
- **[PASS]** no_fault_line_latch: dut:fault_line_asserted never observed before the boundary

## tc_stuck -- overall: BLOCKED

_Guard inputs presented this run: 53 polls, context_valid 53, link_up 53, any_current_present 0, max eligible zones 3._

- **[BLOCKED]** sample_counter_goes_stale: event:guard_warn not observed within 13s after fault_fired (seq 0) _(see this scenario's own `blocked_on:` for the current reason)_
- **[BLOCKED]** trips_after_stale_trip_deadline: dut:K4_open not observed within 65s after fault_fired (seq 0) _(see this scenario's own `blocked_on:` for the current reason)_

## welded_contactor_s9 -- overall: BLOCKED

_Guard inputs presented this run: 84 polls, context_valid 84, link_up 84, any_current_present 0, max eligible zones 3._

- **[BLOCKED]** initial_trip: triggering event (fault_fired {'slot': 'weld_ssr'}) never occurred _(see this scenario's own `blocked_on:` for the current reason)_
- **[BLOCKED]** contactor_weld_engages_on_k4_open: triggering event (relay_edge {'relay': 'K4', 'edge': 'open'}) never occurred _(see this scenario's own `blocked_on:` for the current reason)_
- **[BLOCKED]** s9_escalates: triggering event (relay_edge {'relay': 'K4', 'edge': 'open'}) never occurred _(see this scenario's own `blocked_on:` for the current reason)_
- **[PASS]** stays_open_and_latched: dut:K4_open held at end of run (last observed seq 2000000000)
- **[BLOCKED]** escalation_latched_at_end: event:trip_ineffective_latched never occurred _(see this scenario's own `blocked_on:` for the current reason)_

## welded_ssr_midfire -- overall: BLOCKED

_Guard inputs presented this run: 84 polls, context_valid 84, link_up 84, any_current_present 0, max eligible zones 3._

- **[BLOCKED]** safety_trips: triggering event (fault_fired {'slot': 'weld'}) never occurred _(see this scenario's own `blocked_on:` for the current reason)_
- **[SKIPPED]** no_early_trip: boundary fault 'weld' never fired
- **[PASS]** trip_latched: dut:K4_open held at end of run (last observed seq 2000000000)
- **[BLOCKED]** current_decays_after_k4_opens: triggering event (relay_edge {'relay': 'K4', 'edge': 'open'}) never occurred _(see this scenario's own `blocked_on:` for the current reason)_
