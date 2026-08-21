# virtual_dut scenario results

Full run against `virtual_simfw.exe` + `dut_core.exe` (real, unmodified
`safety_guards.c` / `relay_grace.c` / `snapshots.h`), all 22 scenarios in
`firmware/SimFW/scenarios/`. See `../README.md` for the architecture and the
findings behind the PASS/FAIL/BLOCKED pattern below.

---

## This revision: the sim clock is right, and the two welded scenarios are alive

Two fixture-side defects found while writing the K4 scenarios, both fixed
here, both re-measured across the whole suite.

### 1. `virtual_simfw` was advancing sim by timescale-squared per wall second

Its host main loop scaled the tick **accumulator** by `timescale` and then let
each 100 ms tick advance sim by `100 ms x timescale`, applying the factor
twice. Real `sim_engine_task()` applies it exactly once (a
`vTaskDelayUntil(SIMFW_PERIOD_SIM_ENGINE_MS)` loop -- a REAL-time cadence --
whose body advances sim by `period x timescale`), and `device_tick()` already
ported that body verbatim. `timescale` now means what
`kilnsim.runner.run_scenario()`'s docstring always claimed: **N sim-seconds
per wall second**. Guarded by
`tools/PcTools/tests/test_kilnsim_virtual_simfw.py::test_timescale_advances_sim_linearly_not_quadratically`,
which asserts the *shape* (5x between timescale 1 and 5, not 25x) so no single
slow-machine measurement can explain a reintroduction away.

Wall-clock run times are unchanged. What changed is that every scenario's
sim-time span shrank by its own `timescale` -- to what its `run_duration`
estimate always intended. A `timescale: 10` scenario that used to overshoot to
~2 450 sim-seconds now ends at ~245.

**Exactly two pre-existing verdicts moved, both from a vacuous PASS to an
honest non-PASS:**

| Scenario | Clause | Before | After | Why |
|---|---|---|---|---|
| `mainfault_tc_disconnect` | `no_early_trip` | PASS | BLOCKED | At timescale 10 the FIRST telemetry frame did not land until sim-time ~45 s -- *after* the fault -- so K4's from-boot "open" edge was recorded after this `forbid`'s boundary instead of before it. The frame now lands at ~1 s and the clause sees what was always true (README Finding 2's corollary). Annotated `blocked_on:`. |
| `safety_tc_frozen` | `no_early_trip` | PASS | BLOCKED | Identical cause, identical annotation. |

`tc_flaky` looked like it had moved and had not: `no_warn_storm` still FAILs,
with S5 warns at t~51 s / t~92 s instead of t~146 s / t~196 s. Its run no
longer overshoots (96 s, not 953) and its per-poll tick batch dropped from
~500 ticks to ~50, but 2 Hz telemetry at timescale 10 still cannot resolve its
900 ms bad / 900 ms good alternation, so the batch still manufactures streaks
out of one TC sample. **It was deliberately not re-tuned**, so a clock fix and
a scenario change could not be conflated -- see README Finding 9.

### 2. `welded_ssr_midfire` / `welded_contactor_s9` were dead tests

Both were blocked twice over: no `dut.operator_actions:` (so K4 never closed,
so `sim_engine.c` gated all duty to zero) *and* an `at_zone_temp: 400` trigger
the run could never reach. Fixed differently in each, per what each proves:

| Scenario | What it now does | Measured |
|---|---|---|
| `welded_ssr_midfire` | Keeps a temperature trigger, at a temperature the run reaches (150 C). Enable at t=5 -> K4 closes at t=56.2 -> K1 commanded on at t=66.0 -> weld at **t=126.4** (`at_zone_temp` fired for real) -> K1 commanded **off** at t=135.6. That last step is what makes this file distinct from `stuck_load_no_command_s3`: it exercises S3's 150 s `correlation_window_s` as a *timing* property. | **`GUARD_TRIP {S3}` at t=303.8 s** -- 168 s after the last K1 command (150 s window + ~18 of the 20 s `stuck_on_time_s`, the shortfall being Finding 8's batching bias). K4 opens at 304.4, current gone at 304.8. 414 of 661 polls carry real CT current. Four clauses PASS. |
| `welded_contactor_s9` | Converts to `at_sim_time: 70` and reuses the uncommanded-weld recipe. Nothing about S9 depends on kiln temperature, and S3 is only the *initiating* trip here. | **`GUARD_TRIP {S3}` 18.6 s after the weld**, K4 opens, and the `welded_contactor` fault genuinely fires on that K4-open edge. `initial_trip` and `uncommanded_current_appears_with_the_weld` both PASS (they were BLOCKED / absent). |

**New finding, from doing this:** `contactor_weld_engages_on_k4_open` still
cannot pass, and it is neither a scenario bug nor a guard gap.
`WELDED_K4_CURRENT_PERSIST` deliberately bypasses `duty[]`/`current_a[]` and
drives the CT channel's wave synth directly (real `sim_engine.c` says so at
that call site; `virtual_simfw.c` ports it verbatim), while telemetry's
per-zone `i_amps` is `snap.zones[].current_a` -- the MODEL value, which K4
gates to zero the instant it opens. Real hardware reads the persisted current
off the synthesized waveform through the DUT's own CT ADC; this fixture has no
waveform and no ADC, so **the persisted half of S9's signature has no
observable here at all.**

That is now S9's *only* blocker in this fixture: `relay_deenergized` is
genuinely produced today (`safety_core.c`'s
`.relay_deenergized = !relay_owner_is_energized()`, mirrored by
`dut_core/main.c`), so the "still dormant" table further down this file is
stale on S9's row.

### What did NOT change

Not one scenario-level verdict moved. The suite is 5 PASS, 10 BLOCKED, 7 FAIL,
same as before, and every FAIL is a pre-existing one this pass did not touch.

---

## Previous revision: S3 and S4 actually fire

The previous revision closed the context/current/link wiring and reported
S3/S4 as "reachable but not provokable": `any_current_present` was measured
true on **0 of 1 229 polls** across the whole suite, because
`sim_engine.c` gates heater duty and CT current on K4, and K4 never closed.

That was a chicken-and-egg, not a missing model. K4 closes only on an
operator `SAFETY_CMD_REQUEST_ENABLE` that survives the real 60 s
`SAFTYFW_STARTUP_GRACE_MS`, and KilnFW does not issue one automatically on
profile start, so no scenario ever had. This pass gave scenarios a way to
say so -- a `dut.operator_actions:` list, replayed against the sim clock by
`run_dut_scenarios.py`, that can request enable and command K1..K3 on/off
(see that file's module docstring for why each action models something a
real system does) -- and wrote the first three scenarios that use it.

### The three new scenarios

| Scenario | Verdict | What it measured |
|---|---|---|
| `enabled_firing_healthy` | **PASS** | The S3/S4 anti-nuisance control run. K4 closes 52.2 s after K1 is commanded, current flows 1.4 s later, **125 of 564 polls carry real CT current**, K1 stays commanded for 195 sim-seconds (past S4's 150 s bar), and neither guard says anything. K4 still closed at end of run. |
| `stuck_load_no_command_s3` | **PASS** | **S3 trips for real.** Enable at t=5 s -> K4 energized at t=58.8 s (60 s grace) -> welded SSR at t=70.2 s -> 16 A on CT0 at t=70.8 s with `relay_recent_mask == 0` -> **`GUARD_TRIP {S3}` at t=89.0 s** -> K4 opens at 89.8 s -> current gone at 91.0 s. |
| `commanded_no_current_s4` | **PASS** | **S4 warns for real.** K1 commanded closed at t~6.8 s into a dead element, K4 energized at t=58.6 s and **closed for 323 of 373 polls**, zero current all run -> **`GUARD_WARN {S4}` at t=158.0 s**, 151.2 s after K1 closed (150 s `correlation_window_s`) -> and K4 is **still closed at end of run**, which is the WARN-only design property stated positively for the first time. |

### Were the thresholds actually attainable?

Deliberately, and checkably: **none of the three scenarios uses a
temperature trigger.** Every trigger is `at_sim_time`, every guard bar is a
time bar (60 s grace, 20 s `stuck_on_time_s`, 150 s `correlation_window_s`),
and the current magnitude is set by the preset's own physics --
`V_mains 240 / R_element 15 = 16.0 A` against `i_present_a`'s 2.0 A default,
an 8x margin, not a number tuned to just clear a bar. Both faults used
(`welded_ssr`, `broken_heater_coil`) take no `params[]` at all, so the
"fault with no `params:` silently injects magnitude 0" trap cannot apply.

The contrast is instructive (**superseded**: both files were fixed in the
revision above, and neither uses a 400 C trigger any more):
`welded_ssr_midfire`/`welded_contactor_s9`
triggered their weld at `at_zone_temp 400 C`, which the `fast_test` preset
**cannot reach** in those scenarios' time budget even with K4 closed --
it asymptotes at ~505 C with a ~200 s time constant, so 400 C needs ~304
sim-seconds of full duty against an estimated run duration of ~195 s. That
is now recorded in those files' own `blocked_on:` notes as a second,
independent blocker.

### Timescale: 2, not 10 -- and why a finer `--poll-interval` is NOT the fix

Two harness facts, both measured, both now in `../README.md` as Finding 8:

1. `virtual_simfw` advances its sim clock by **timescale squared** per wall
   second, so `timescale: 10` is 100x real time and its 2 Hz telemetry
   broadcast lands one frame every ~50 sim-seconds -- coarser than S3's own
   20 s window. The new scenarios use `timescale: 2` (~2 sim-seconds per
   frame). (**Superseded**: the squaring was a defect and is fixed in the
   revision above. `timescale: 2` is still the right choice for these files --
   one frame per sim-second -- but the number to reason from is now
   `0.5 / timescale` sim-seconds per frame, not `0.5 / timescale^2`.)
2. `run_dut_scenarios.py` converts elapsed sim time to 100 ms guard ticks
   with `max(1, delta // 100ms)`. Below 100 ms of sim per poll that floor
   **over**-ticks the DUT and every guard timer fires early. Measured on
   `stuck_load_no_command_s3`: at the scenario's own settings, K4 armed at
   58.6 s (true 60) and S3 tripped after 18.2 s of current (true 20); at
   `--timescale 1 --poll-interval 0.02`, K4 armed at 21.7 s and S3 "tripped"
   after 7.4 s. The fine-grained run is the distorted one.

`stuck_load_no_command_s3`'s `not_before_s` is therefore set at 15 s, one
tick-batch below the real 20 s bar, with that reasoning written into the
clause -- asserting `>= 20` would be asserting the fixture's sampling rather
than the guard's threshold.

### What did NOT change

Every other scenario's verdict is identical to the previous revision.
Four files' `blocked_on:` annotations were rewritten because their stated
cause had become false ("no scenario models the operator enable" /
"nothing in the fixture ever commands a zone relay on"): `baseline_firing`,
`broken_element`, `welded_ssr_midfire`, `welded_contactor_s9`. Their
verdicts are unchanged -- those clauses are still not passing, but the
reason is now local to each file (it schedules no operator actions) rather
than an upstream gap, and each annotation says so and names the new
scenario that does the thing. They are candidates for retirement, not for
another round of excuses.

---

## Previous revision: the fixture caught up with SaftyFW commit `f304392`

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
- **S3 / S4** (**superseded — see "This revision" above; both fire now**) —
  `sim_engine.c` gates heater duty and CT current on K4, and K4
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
"S3 was never evaluated". `K4 energized` is new this revision and is the
fastest way to tell a scenario that exercised the power path from one that
did not.

## baseline_firing -- overall: BLOCKED

_Guard inputs presented this run: 33 polls, context_valid 33, link_up 33, any_current_present 0, K4 energized 0, max eligible zones 3._

- **[PASS]** never_faults: dut:fault_line_asserted never observed before the boundary
- **[PASS]** never_estops: dut:estop_open never observed before the boundary
- **[BLOCKED]** never_trips: dut:K4_open observed before the boundary (first at seq 2000000000) _(see this scenario's own `blocked_on:` for the current reason)_
- **[SKIPPED]** heat_actually_cycles: triggering event (relay_edge {'relay': 'K1', 'edge': 'close'}) never occurred

## broken_element -- overall: BLOCKED

_Guard inputs presented this run: 79 polls, context_valid 79, link_up 79, any_current_present 0, K4 energized 0, max eligible zones 3._

- **[BLOCKED]** s4_warns: triggering event (relay_edge {'relay': 'K1', 'edge': 'close'}) never occurred _(see this scenario's own `blocked_on:` for the current reason)_
- **[BLOCKED]** s4_never_trips: dut:K4_open observed before the boundary (first at seq 2000000000) _(see this scenario's own `blocked_on:` for the current reason)_

## cj_fault -- overall: FAIL

_Guard inputs presented this run: 53 polls, context_valid 53, link_up 53, any_current_present 0, K4 energized 0, max eligible zones 3._

- **[PASS]** s5_never_trips_on_cj_alone: event:guard_trip never observed before the boundary
- **[FAIL]** s12_warns_at_60c: event:guard_warn not observed within 1s after fault_fired (seq 0)
- **[FAIL]** s12_trips_at_85c_sustained: dut:K4_open not observed within 65s after fault_fired (seq 0)

## commanded_no_current_s4 -- overall: PASS

_Guard inputs presented this run: 373 polls, context_valid 373, link_up 373, any_current_present 0, K4 energized 323, max eligible zones 3._

- **[PASS]** s4_warns_after_the_correlation_window: event:guard_warn observed 151.200s after relay_edge
- **[PASS]** no_current_ever_flows: dut:current_present never observed before the boundary
- **[PASS]** s4_never_opens_k4: dut:K4_closed held at end of run (last observed seq 2000000001)

## enabled_firing_healthy -- overall: PASS

_Guard inputs presented this run: 564 polls, context_valid 564, link_up 564, any_current_present 125, K4 energized 514, max eligible zones 3._

- **[PASS]** k4_closes_after_startup_grace: dut:K4_closed observed 52.200s after relay_edge
- **[PASS]** current_flows_once_k4_permits: dut:current_present observed 1.400s after relay_edge
- **[PASS]** s3_never_trips_during_a_commanded_firing: event:guard_trip never observed before the boundary
- **[PASS]** s4_never_warns_during_a_commanded_firing: event:guard_warn never observed before the boundary
- **[PASS]** current_stops_when_the_zone_is_commanded_off: dut:current_absent observed 1.200s after relay_edge
- **[PASS]** k4_still_closed_at_end: dut:K4_closed held at end of run (last observed seq 2000000001)

## estop_at_boot -- overall: PASS

_Guard inputs presented this run: 32 polls, context_valid 32, link_up 32, any_current_present 0, K4 energized 0, max eligible zones 3._

- **[PASS]** relay_never_energizes: dut:K4_closed never observed before the boundary
- **[PASS]** still_open_at_end: dut:K4_open held at end of run (last observed seq 2000000000)

## estop_midfire -- overall: PASS

_Guard inputs presented this run: 76 polls, context_valid 76, link_up 76, any_current_present 0, K4 energized 0, max eligible zones 3._

- **[SKIPPED]** fast_trip: triggering event (fault_fired {'slot': 'estop'}) never occurred
- **[SKIPPED]** stays_latched_after_physical_release: triggering event (fault_cleared {'slot': 'estop'}) never occurred
- **[PASS]** latched_at_end: dut:K4_open held at end of run (last observed seq 2000000000)

## main_safety_skew -- overall: BLOCKED

_Guard inputs presented this run: 37 polls, context_valid 37, link_up 37, any_current_present 0, K4 energized 0, max eligible zones 1._

- **[BLOCKED]** s1_trips_conservatively_early: event:guard_trip not observed within 120s after fault_fired (seq 0) _(see this scenario's own `blocked_on:` for the current reason)_
- **[PASS]** s10_stays_quiet: event:guard_warn never observed before the boundary

## mainfault_tc_disconnect -- overall: FAIL

_Guard inputs presented this run: 32 polls, context_valid 32, link_up 32, any_current_present 0, K4 energized 0, max eligible zones 3._

- **[PASS]** no_early_trip: dut:K4_open never observed before the boundary
- **[BLOCKED]** mainfault_trips_s6a: event:guard_trip not observed within 3s after fault_fired (seq 0) _(see this scenario's own `blocked_on:` for the current reason)_
- **[FAIL]** fault_line_asserted: dut:fault_line_asserted not observed within 3s after fault_fired (seq 0)
- **[PASS]** trip_latched: dut:K4_open held at end of run (last observed seq 2000000000)

## partial_element -- overall: FAIL

_Guard inputs presented this run: 20 polls, context_valid 20, link_up 20, any_current_present 0, K4 energized 0, max eligible zones 3._

- **[BLOCKED]** no_guard_trips: dut:K4_open observed before the boundary (first at seq 2000000000) _(see this scenario's own `blocked_on:` for the current reason)_
- **[FAIL]** current_still_present_just_reduced: dut:current_present not observed within 5s after fault_fired (seq 0)

## power_blip -- overall: BLOCKED

_Guard inputs presented this run: 84 polls, context_valid 84, link_up 84, any_current_present 0, K4 energized 0, max eligible zones 3._

- **[PASS]** mainfault_reads_healthy_during_blip: event:guard_trip never observed before the boundary
- **[SKIPPED]** relays_deenergize_no_current_during_blip: triggering event (dut_power {'state': False}) never occurred
- **[BLOCKED]** s6b_stays_at_warn_not_trip: dut:K4_open observed before the boundary (first at seq 2000000000) _(see this scenario's own `blocked_on:` for the current reason)_
- **[SKIPPED]** safe_resume: triggering event (dut_power {'state': True}) never occurred

## runaway_zone -- overall: BLOCKED

_Guard inputs presented this run: 82 polls, context_valid 82, link_up 82, any_current_present 0, K4 energized 0, max eligible zones 3._

- **[BLOCKED]** s3_catches_it_first: triggering event (fault_fired {'slot': 'runaway'}) never occurred _(see this scenario's own `blocked_on:` for the current reason)_
- **[PASS]** s8_produces_no_trip_of_its_own_today: event:guard_trip never observed before the boundary

## safety_tc_frozen -- overall: BLOCKED

_Guard inputs presented this run: 219 polls, context_valid 219, link_up 219, any_current_present 0, K4 energized 0, max eligible zones 3._

- **[PASS]** no_early_trip: dut:K4_open never observed before the boundary
- **[BLOCKED]** frozen_window_trips_s11: event:guard_trip not observed within 605s after fault_fired (seq 0) _(see this scenario's own `blocked_on:` for the current reason)_
- **[PASS]** trip_latched: dut:K4_open held at end of run (last observed seq 2000000000)

## spi_flaky_tc_ic -- overall: FAIL

_Guard inputs presented this run: 52 polls, context_valid 52, link_up 52, any_current_present 0, K4 energized 0, max eligible zones 3._

- **[PASS]** not_an_instant_trip: dut:K4_open never observed before the boundary
- **[FAIL]** warns_then_trips_like_disconnect: dut:K4_open not observed within 65s after fault_fired (seq 0)

## stuck_load_no_command_s3 -- overall: PASS

_Guard inputs presented this run: 245 polls, context_valid 245, link_up 245, any_current_present 18, K4 energized 26, max eligible zones 3._

- **[PASS]** uncommanded_current_appears_with_the_weld: dut:current_present observed 0.600s after fault_fired
- **[PASS]** s3_trips_on_uncommanded_load: event:guard_trip observed 18.800s after fault_fired
- **[PASS]** s3_does_not_trip_before_there_is_any_current: event:guard_trip never observed before the boundary
- **[PASS]** k4_opens_and_stays_open: dut:K4_open held at end of run (last observed seq 2000000003)
- **[PASS]** current_stops_when_k4_opens: dut:current_absent observed 2.000s after guard_trip

## tc_disconnect_ramp -- overall: FAIL

_Guard inputs presented this run: 55 polls, context_valid 55, link_up 55, any_current_present 0, K4 energized 0, max eligible zones 3._

- **[FAIL]** warns_first: event:guard_warn not observed within 6s after fault_fired (seq 0)
- **[FAIL]** trips_after_blind_grace: dut:K4_open not observed within 65s after fault_fired (seq 0)
- **[FAIL]** reports_nan_not_stale_value: safety_temp_valid state never observed; cannot be dut:safety_temp_valid at end

## tc_disconnect_soak -- overall: FAIL

_Guard inputs presented this run: 76 polls, context_valid 76, link_up 76, any_current_present 0, K4 energized 0, max eligible zones 3._

- **[FAIL]** warns_first: event:guard_warn not observed within 6s after fault_fired (seq 0)
- **[FAIL]** trips_after_blind_grace: dut:K4_open not observed within 65s after fault_fired (seq 0)
- **[FAIL]** reports_nan_not_stale_value: safety_temp_valid state never observed; cannot be dut:safety_temp_valid at end

## tc_flaky -- overall: FAIL

_Guard inputs presented this run: 36 polls, context_valid 36, link_up 36, any_current_present 0, K4 energized 0, max eligible zones 3._

- **[BLOCKED]** no_trip_from_flapping: dut:K4_open observed before the boundary (first at seq 2000000000) _(see this scenario's own `blocked_on:` for the current reason)_
- **[FAIL]** no_warn_storm: event:guard_warn observed before the boundary (first at seq 2000000001)

## tc_noise_storm -- overall: BLOCKED

_Guard inputs presented this run: 32 polls, context_valid 32, link_up 32, any_current_present 0, K4 energized 0, max eligible zones 3._

- **[BLOCKED]** no_trip_ever: dut:K4_open observed before the boundary (first at seq 2000000000) _(see this scenario's own `blocked_on:` for the current reason)_
- **[PASS]** no_fault_line_latch: dut:fault_line_asserted never observed before the boundary

## tc_stuck -- overall: BLOCKED

_Guard inputs presented this run: 52 polls, context_valid 52, link_up 52, any_current_present 0, K4 energized 0, max eligible zones 3._

- **[BLOCKED]** sample_counter_goes_stale: event:guard_warn not observed within 13s after fault_fired (seq 0) _(see this scenario's own `blocked_on:` for the current reason)_
- **[BLOCKED]** trips_after_stale_trip_deadline: dut:K4_open not observed within 65s after fault_fired (seq 0) _(see this scenario's own `blocked_on:` for the current reason)_

## welded_contactor_s9 -- overall: BLOCKED

_Guard inputs presented this run: 83 polls, context_valid 83, link_up 83, any_current_present 0, K4 energized 0, max eligible zones 3._

- **[BLOCKED]** initial_trip: triggering event (fault_fired {'slot': 'weld_ssr'}) never occurred _(see this scenario's own `blocked_on:` for the current reason)_
- **[BLOCKED]** contactor_weld_engages_on_k4_open: triggering event (relay_edge {'relay': 'K4', 'edge': 'open'}) never occurred _(see this scenario's own `blocked_on:` for the current reason)_
- **[BLOCKED]** s9_escalates: triggering event (relay_edge {'relay': 'K4', 'edge': 'open'}) never occurred _(see this scenario's own `blocked_on:` for the current reason)_
- **[PASS]** stays_open_and_latched: dut:K4_open held at end of run (last observed seq 2000000000)
- **[BLOCKED]** escalation_latched_at_end: event:trip_ineffective_latched never occurred _(see this scenario's own `blocked_on:` for the current reason)_

## welded_ssr_midfire -- overall: BLOCKED

_Guard inputs presented this run: 83 polls, context_valid 83, link_up 83, any_current_present 0, K4 energized 0, max eligible zones 3._

- **[BLOCKED]** safety_trips: triggering event (fault_fired {'slot': 'weld'}) never occurred _(see this scenario's own `blocked_on:` for the current reason)_
- **[SKIPPED]** no_early_trip: boundary fault 'weld' never fired
- **[PASS]** trip_latched: dut:K4_open held at end of run (last observed seq 2000000000)
- **[BLOCKED]** current_decays_after_k4_opens: triggering event (relay_edge {'relay': 'K4', 'edge': 'open'}) never occurred _(see this scenario's own `blocked_on:` for the current reason)_

