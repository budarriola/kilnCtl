# virtual_dut scenario results

Full run against `virtual_simfw.exe` + `dut_core.exe` (real, unmodified
`safety_guards.c` / `relay_grace.c` / `snapshots.h`), all 22 scenarios in
`firmware/SimFW/scenarios/`. See `../README.md` for the architecture and the
findings behind the PASS/FAIL/BLOCKED pattern below.

---

## This revision: the harness stopped inventing reads, and S9 fired for real

Two fixture defects, both closed here, both regression-covered by
`../test_virtual_dut_harness.py` (8 checks, no pytest required).

### 1. A tick batch was manufacturing consecutive-read streaks

`run_dut_scenarios.py` used to turn each poll's elapsed sim time into
`max(1, delta // 100ms)` guard ticks and replay the SAME telemetry-derived TC
sample across all of them. `safety_guards.c` counts *reads* as well as seconds
(S5: 10 consecutive bad reads **and** 5 s; S1: 3 consecutive readings), so at
`timescale: 10` — one telemetry frame per 5 sim-seconds — one bad sample became
~50 consecutive bad reads and cleared a bar nothing had actually cleared.
`tc_flaky`'s `no_warn_storm` had been FAILing on precisely that.

Fixed by the rule rather than by the scenario: **one guard tick per observed
sample, `dt_s` = the sim time that actually elapsed since the previous sample,
and no tick at all when the fixture published nothing new**
(`plan_tick_dt_ms()`; `dut_core.exe`'s `TICK` line gained an optional trailing
`<dt_ms>`). There is no honest filler for the other N−1 ticks — repeating the
sample invents reads, dropping `tc_valid` invents *bad* reads — so the batch
must simply not tick more than once per sample. Time-based bars are unaffected
to the microsecond (`safety_guards.c` only ever does `+= in->dt_s`), and the
`max(1, ...)` floor's opposite-sign bias (README Finding 8: guard clock runs
fast, every timer fires early) went with it — `relay_grace`'s startup timer now
accumulates `dt_ms` instead of counting calls, so K4 arms at a true **60.8 s**
instead of 58.6 s, and S3 trips **19.2 s** after its weld instead of 18.2 s.

### 2. S9's persisted current had no observable

`WELDED_K4_CURRENT_PERSIST` forces the CT channel to MANUAL amps and bypasses
`duty[]`/`current_a[]` (real `sim_engine.c` does the same, by design), while
this harness read per-zone `i_amps` — the MODEL current, which K4 gates to
zero. Current is now read through the fixture's own **real** `CT_GET_STATE`
command (`read_ct_state()`), and used wherever the model path cannot carry what
the channel is synthesizing. MODEL-mode channels keep the telemetry frame's own
number on purpose: a live readback stamped into an older frame drags edges
earlier than the EVT stream that caused them (measured: it broke two
`enabled_firing_healthy` clauses that had been correctly passing).

### Verdicts moved: 5 PASS / 10 BLOCKED / 7 FAIL → 5 PASS / 11 BLOCKED / 6 FAIL

Four expectation verdicts, all upward, none downward:

| Scenario | Clause | Before | After | Why |
|---|---|---|---|---|
| `tc_flaky` | `no_warn_storm` | **FAIL** | **PASS** | No S5 warn is produced at all once the streak cannot be fabricated. Scenario verdict FAIL → BLOCKED (its other clause is the pre-existing K4-never-closes BLOCKED). **Read the caveat below.** |
| `welded_contactor_s9` | `s9_escalates` | BLOCKED | **PASS** | **S9 exercised end to end for the first time.** S3 trips at t=90.2 s → K4 opens → the welded contactor holds 20 A on CT0 → `TRIP_INEFFECTIVE_LATCHED` **9.2 s after K4 opened** (9.8 s on the re-run whose report is committed here), against `trip_verify_s` = 10 s plus one sim-second of telemetry quantization. `blocked_on:` removed from the scenario. |
| `welded_contactor_s9` | `escalation_latched_at_end` | BLOCKED | **PASS** | Follows the clause above; `blocked_on:` removed. |
| `enabled_firing_healthy` | — | PASS | PASS | Held, but only after the MANUAL-only rule above; an unconditional CT substitution had broken it. Listed so the trap is on the record. |

**`tc_flaky`'s PASS is under-resolved at its own `timescale: 10` — and was
checked at full resolution too.** The suite run took **18 samples** across ~96
sim-seconds, so the guards saw isolated single bad reads, never the 9-in-a-row
the scenario's own comment derives from a 900 ms bad phase at the MAX31856's
~100 ms cadence; that only proves a coarse sampler no longer fabricates a
streak. Re-running the file with `--timescale 0.2` (0.1 sim-seconds per frame,
the real conversion cadence) presents the actual stimulus: **453 guard ticks
over 45.2 sim-seconds, zero S5 warns, zero trips** — the anti-nuisance claim
the scenario exists to make, measured. The clause was **not re-tuned in either
direction**: the FAIL was removed by removing the fabrication. Worth
considering separately: lowering the scenario's own `timescale` so its default
run is the meaningful one.

**Still BLOCKED, for a corrected reason:**
`contactor_weld_engages_on_k4_open`. The persisted current is genuinely
observable now, so that annotation's old "no observable at all" is stale and
has been rewritten. `report.py` matches `then: {dut: current_present}` as an
**edge**, and a correctly persisting current produces none — it was already
present before K4 opened and never stops, and `virtual_simfw` fires the
`welded_contactor` fault in the same `device_tick()` that opens K4, so the CT
never dips. Expressing "current did not stop" wants a `forbid:` clause; that is
a scenario-grammar change, deliberately not made in the same pass.

---

## Previous revision: the sim clock is right, and the two welded scenarios are alive

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

`inputs:` lines report how many guard ticks presented each precondition — they
decide nothing, they just let "S3 stayed quiet" be distinguished from "S3 was
never evaluated". Since this revision there is exactly **one guard tick per
observed telemetry sample** (see the batching change above), so the tick count
is also the sample count — a small number next to a short guard window is the
signal that a quiet guard means "not resolved", not "evaluated and silent".
`K4 energized` is the fastest way to tell a scenario that exercised the power
path from one that did not.

## baseline_firing -- overall: BLOCKED

_Guard inputs presented this run: 19 ticks, context_valid 19, link_up 19, any_current_present 0, K4 energized 0, max eligible zones 3._

- **[PASS]** never_faults: dut:fault_line_asserted never observed before the boundary
- **[PASS]** never_estops: dut:estop_open never observed before the boundary
- **[BLOCKED]** never_trips: dut:K4_open observed before the boundary (first at seq 2000000000) _(see this scenario's own `blocked_on:` for the current reason)_
- **[SKIPPED]** heat_actually_cycles: triggering event (relay_edge {'relay': 'K1', 'edge': 'close'}) never occurred

## broken_element -- overall: BLOCKED

_Guard inputs presented this run: 46 ticks, context_valid 46, link_up 46, any_current_present 0, K4 energized 0, max eligible zones 3._

- **[BLOCKED]** s4_warns: triggering event (relay_edge {'relay': 'K1', 'edge': 'close'}) never occurred _(see this scenario's own `blocked_on:` for the current reason)_
- **[BLOCKED]** s4_never_trips: dut:K4_open observed before the boundary (first at seq 2000000000) _(see this scenario's own `blocked_on:` for the current reason)_

## cj_fault -- overall: FAIL

_Guard inputs presented this run: 30 ticks, context_valid 30, link_up 30, any_current_present 0, K4 energized 0, max eligible zones 3._

- **[PASS]** s5_never_trips_on_cj_alone: event:guard_trip never observed before the boundary
- **[FAIL]** s12_warns_at_60c: event:guard_warn not observed within 1s after fault_fired (seq 0)
- **[FAIL]** s12_trips_at_85c_sustained: dut:K4_open not observed within 65s after fault_fired (seq 0)

## commanded_no_current_s4 -- overall: PASS

_Guard inputs presented this run: 207 ticks, context_valid 207, link_up 207, any_current_present 0, K4 energized 148, max eligible zones 3._

- **[PASS]** s4_warns_after_the_correlation_window: event:guard_warn observed 150.600s after relay_edge
- **[PASS]** no_current_ever_flows: dut:current_present never observed before the boundary
- **[PASS]** s4_never_opens_k4: dut:K4_closed held at end of run (last observed seq 2000000001)

## enabled_firing_healthy -- overall: PASS

_Guard inputs presented this run: 315 ticks, context_valid 315, link_up 315, any_current_present 138, K4 energized 256, max eligible zones 3._

- **[PASS]** k4_closes_after_startup_grace: dut:K4_closed observed 56.000s after relay_edge
- **[PASS]** current_flows_once_k4_permits: dut:current_present observed 0.200s after relay_edge
- **[PASS]** s3_never_trips_during_a_commanded_firing: event:guard_trip never observed before the boundary
- **[PASS]** s4_never_warns_during_a_commanded_firing: event:guard_warn never observed before the boundary
- **[PASS]** current_stops_when_the_zone_is_commanded_off: dut:current_absent observed 0.400s after relay_edge
- **[PASS]** k4_still_closed_at_end: dut:K4_closed held at end of run (last observed seq 2000000001)

## estop_at_boot -- overall: PASS

_Guard inputs presented this run: 19 ticks, context_valid 19, link_up 19, any_current_present 0, K4 energized 0, max eligible zones 3._

- **[PASS]** relay_never_energizes: dut:K4_closed never observed before the boundary
- **[PASS]** still_open_at_end: dut:K4_open held at end of run (last observed seq 2000000000)

## estop_midfire -- overall: PASS

_Guard inputs presented this run: 45 ticks, context_valid 45, link_up 45, any_current_present 0, K4 energized 0, max eligible zones 3._

- **[SKIPPED]** fast_trip: triggering event (fault_fired {'slot': 'estop'}) never occurred
- **[SKIPPED]** stays_latched_after_physical_release: triggering event (fault_cleared {'slot': 'estop'}) never occurred
- **[PASS]** latched_at_end: dut:K4_open held at end of run (last observed seq 2000000000)

## main_safety_skew -- overall: BLOCKED

_Guard inputs presented this run: 25 ticks, context_valid 25, link_up 25, any_current_present 0, K4 energized 0, max eligible zones 1._

- **[BLOCKED]** s1_trips_conservatively_early: event:guard_trip not observed within 120s after fault_fired (seq 0) _(see this scenario's own `blocked_on:` for the current reason)_
- **[PASS]** s10_stays_quiet: event:guard_warn never observed before the boundary

## mainfault_tc_disconnect -- overall: FAIL

_Guard inputs presented this run: 19 ticks, context_valid 19, link_up 19, any_current_present 0, K4 energized 0, max eligible zones 3._

- **[BLOCKED]** no_early_trip: dut:K4_open observed before the boundary (first at seq 2000000000) _(see this scenario's own `blocked_on:` for the current reason)_
- **[BLOCKED]** mainfault_trips_s6a: event:guard_trip not observed within 3s after fault_fired (seq 0) _(see this scenario's own `blocked_on:` for the current reason)_
- **[FAIL]** fault_line_asserted: dut:fault_line_asserted not observed within 3s after fault_fired (seq 0)
- **[PASS]** trip_latched: dut:K4_open held at end of run (last observed seq 2000000000)

## partial_element -- overall: FAIL

_Guard inputs presented this run: 14 ticks, context_valid 14, link_up 14, any_current_present 0, K4 energized 0, max eligible zones 3._

- **[BLOCKED]** no_guard_trips: dut:K4_open observed before the boundary (first at seq 2000000000) _(see this scenario's own `blocked_on:` for the current reason)_
- **[FAIL]** current_still_present_just_reduced: dut:current_present not observed within 5s after fault_fired (seq 0)

## power_blip -- overall: BLOCKED

_Guard inputs presented this run: 49 ticks, context_valid 49, link_up 49, any_current_present 0, K4 energized 0, max eligible zones 3._

- **[PASS]** mainfault_reads_healthy_during_blip: event:guard_trip never observed before the boundary
- **[SKIPPED]** relays_deenergize_no_current_during_blip: triggering event (dut_power {'state': False}) never occurred
- **[BLOCKED]** s6b_stays_at_warn_not_trip: dut:K4_open observed before the boundary (first at seq 2000000000) _(see this scenario's own `blocked_on:` for the current reason)_
- **[SKIPPED]** safe_resume: triggering event (dut_power {'state': True}) never occurred

## runaway_zone -- overall: BLOCKED

_Guard inputs presented this run: 48 ticks, context_valid 48, link_up 48, any_current_present 0, K4 energized 0, max eligible zones 3._

- **[BLOCKED]** s3_catches_it_first: triggering event (fault_fired {'slot': 'runaway'}) never occurred _(see this scenario's own `blocked_on:` for the current reason)_
- **[PASS]** s8_produces_no_trip_of_its_own_today: event:guard_trip never observed before the boundary

## safety_tc_frozen -- overall: BLOCKED

_Guard inputs presented this run: 128 ticks, context_valid 128, link_up 128, any_current_present 0, K4 energized 0, max eligible zones 3._

- **[BLOCKED]** no_early_trip: dut:K4_open observed before the boundary (first at seq 2000000000) _(see this scenario's own `blocked_on:` for the current reason)_
- **[BLOCKED]** frozen_window_trips_s11: event:guard_trip not observed within 605s after fault_fired (seq 0) _(see this scenario's own `blocked_on:` for the current reason)_
- **[PASS]** trip_latched: dut:K4_open held at end of run (last observed seq 2000000000)

## spi_flaky_tc_ic -- overall: FAIL

_Guard inputs presented this run: 31 ticks, context_valid 31, link_up 31, any_current_present 0, K4 energized 0, max eligible zones 3._

- **[PASS]** not_an_instant_trip: dut:K4_open never observed before the boundary
- **[FAIL]** warns_then_trips_like_disconnect: dut:K4_open not observed within 65s after fault_fired (seq 0)

## stuck_load_no_command_s3 -- overall: PASS

_Guard inputs presented this run: 138 ticks, context_valid 138, link_up 138, any_current_present 20, K4 energized 29, max eligible zones 3._

- **[PASS]** uncommanded_current_appears_with_the_weld: dut:current_present observed 0.000s after fault_fired
- **[PASS]** s3_trips_on_uncommanded_load: event:guard_trip observed 19.200s after fault_fired
- **[PASS]** s3_does_not_trip_before_there_is_any_current: event:guard_trip never observed before the boundary
- **[PASS]** k4_opens_and_stays_open: dut:K4_open held at end of run (last observed seq 2000000003)
- **[PASS]** current_stops_when_k4_opens: dut:current_absent observed 1.000s after guard_trip

## tc_disconnect_ramp -- overall: FAIL

_Guard inputs presented this run: 33 ticks, context_valid 33, link_up 33, any_current_present 0, K4 energized 0, max eligible zones 3._

- **[FAIL]** warns_first: event:guard_warn not observed within 6s after fault_fired (seq 0)
- **[FAIL]** trips_after_blind_grace: dut:K4_open not observed within 65s after fault_fired (seq 0)
- **[FAIL]** reports_nan_not_stale_value: safety_temp_valid state never observed; cannot be dut:safety_temp_valid at end

## tc_disconnect_soak -- overall: FAIL

_Guard inputs presented this run: 44 ticks, context_valid 44, link_up 44, any_current_present 0, K4 energized 0, max eligible zones 3._

- **[FAIL]** warns_first: event:guard_warn not observed within 6s after fault_fired (seq 0)
- **[FAIL]** trips_after_blind_grace: dut:K4_open not observed within 65s after fault_fired (seq 0)
- **[FAIL]** reports_nan_not_stale_value: safety_temp_valid state never observed; cannot be dut:safety_temp_valid at end

## tc_flaky -- overall: BLOCKED

_Guard inputs presented this run: 18 ticks, context_valid 18, link_up 18, any_current_present 0, K4 energized 0, max eligible zones 3._

- **[BLOCKED]** no_trip_from_flapping: dut:K4_open observed before the boundary (first at seq 2000000000) _(see this scenario's own `blocked_on:` for the current reason)_
- **[PASS]** no_warn_storm: event:guard_warn never observed before the boundary

## tc_noise_storm -- overall: BLOCKED

_Guard inputs presented this run: 18 ticks, context_valid 18, link_up 18, any_current_present 0, K4 energized 0, max eligible zones 3._

- **[BLOCKED]** no_trip_ever: dut:K4_open observed before the boundary (first at seq 2000000000) _(see this scenario's own `blocked_on:` for the current reason)_
- **[PASS]** no_fault_line_latch: dut:fault_line_asserted never observed before the boundary

## tc_stuck -- overall: BLOCKED

_Guard inputs presented this run: 30 ticks, context_valid 30, link_up 30, any_current_present 0, K4 energized 0, max eligible zones 3._

- **[BLOCKED]** sample_counter_goes_stale: event:guard_warn not observed within 13s after fault_fired (seq 0) _(see this scenario's own `blocked_on:` for the current reason)_
- **[BLOCKED]** trips_after_stale_trip_deadline: dut:K4_open not observed within 65s after fault_fired (seq 0) _(see this scenario's own `blocked_on:` for the current reason)_

## welded_contactor_s9 -- overall: BLOCKED

_Guard inputs presented this run: 216 ticks, context_valid 216, link_up 216, any_current_present 147, K4 energized 29, max eligible zones 3._

- **[PASS]** uncommanded_current_appears_with_the_weld: dut:current_present observed 0.800s after fault_fired
- **[PASS]** initial_trip: event:guard_trip observed 20.000s after fault_fired
- **[BLOCKED]** contactor_weld_engages_on_k4_open: dut:current_present not observed within 5s after relay_edge (seq 2) _(see this scenario's own `blocked_on:` for the current reason)_
- **[PASS]** s9_escalates: event:trip_ineffective_latched observed 9.800s after relay_edge
- **[PASS]** stays_open_and_latched: dut:K4_open held at end of run (last observed seq 2000000003)
- **[PASS]** escalation_latched_at_end: event:trip_ineffective_latched occurred (last at seq 2000000004)

## welded_ssr_midfire -- overall: BLOCKED

_Guard inputs presented this run: 374 ticks, context_valid 374, link_up 374, any_current_present 237, K4 energized 241, max eligible zones 3._

- **[PASS]** current_flows_during_the_commanded_firing: dut:current_present observed 0.800s after relay_edge
- **[PASS]** s4_never_warns_while_commanded_and_conducting: event:guard_warn never observed before the boundary
- **[PASS]** s3_waits_out_the_correlation_window: event:guard_trip never observed before the boundary
- **[PASS]** s3_trips_after_the_window_expires: event:guard_trip observed 179.600s after fault_fired
- **[BLOCKED]** no_early_trip: dut:K4_open observed before the boundary (first at seq 2000000000) _(see this scenario's own `blocked_on:` for the current reason)_
- **[PASS]** trip_latched: dut:K4_open held at end of run (last observed seq 2000000003)
- **[PASS]** current_decays_after_k4_opens: dut:current_absent observed 0.000s after relay_edge
