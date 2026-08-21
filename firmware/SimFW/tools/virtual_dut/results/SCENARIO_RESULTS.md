# virtual_dut scenario results

Full run against `virtual_simfw.exe` + `dut_core.exe` (real, unmodified
`safety_guards.c` / `relay_grace.c` / `snapshots.h`), all 25 scenarios in
`firmware/SimFW/scenarios/`. See `../README.md` for the architecture and the
findings behind the PASS/FAIL/BLOCKED pattern below.

---

## This revision: three new scenarios (S6a audit, S11 trip+nuisance, S2, S10),
## a zone-setpoint capability, and two verdicts moved

Every guard input in `safety_core_build_input()` is now populated except two
deliberate commissioning gaps (S1's `abs_max_temp_c`, S13's
`borrowed_zone_index`). This pass re-audited which of the 22 existing
scenarios actually provoke their declared guards today, closed the gaps that
were closable through this harness, and documented precisely why one (S6a)
is not closable through this harness at all.

### New scenarios (25 files now, was 22)

- **`s2_setpoint_overshoot.yaml`** (S2). S2 was reachable in
  `safety_core_build_input()` since `f304392` but had never been provoked by
  this fixture: nothing in `kilnsim`/`virtual_simfw` carried a zone setpoint,
  so `max_zone_setpoint_c` was always NaN and S2's own
  `tc_c > max_zone_setpoint_c + margin` comparison was always false
  regardless of temperature. Plumbing a setpoint turned out to be a small,
  local change (not a fixture rewrite): `run_dut_scenarios.py` gained a
  `dut.zone_setpoints:` mapping (static, per-zone, held constant for the
  whole run -- there is no PID here to make it time-varying, and a constant
  declared value is the honest ceiling to compare against). With zone 0's
  setpoint declared at 150°C and K1 commanded on, `fast_test`'s own physics
  (steady-state ~505°C) overshoots the 75°C default margin and S2 trips for
  real: **`GUARD_TRIP {S2}` at t=267.0s after K4 closes**, comfortably inside
  a bracket (`within_s: 310`, `not_before_s: 220`) built from the
  crossing-time arithmetic in the scenario's own header comment, not guessed.
- **`safety_tc_frozen.yaml`** (S11, existing file, provocation fixed). S11's
  gate (`in->tc_valid && in->heat_commanded`) needs a closed K4 and real
  current, which nothing in this file provided before this pass -- it never
  scheduled `operator_actions:`. Added `request_enable` + `command_relay K1
  closed` (never commanded off), matching the pattern
  `stuck_load_no_command_s3.yaml`/`welded_ssr_midfire.yaml` already
  established for S3/S4. **`GUARD_TRIP {S11}` now fires for real, 664.0s
  after the freeze fault** -- inside the widened bracket
  (`within_s: 700`, `not_before_s: 630`) that accounts for the ~61s K4-close
  delay this file's own comment derives. `frozen_window_trips_s11`'s
  `blocked_on:` (heat_commanded hardcoded false) is removed -- it no longer
  applies. `no_early_trip` was rewritten from the structurally-broken
  `forbid: {dut: K4_open, before: {fault: freeze}}` form (K4 is open at
  sim-time ~1s regardless of any guard, the same K4-boot-open artifact
  documented elsewhere in this file) to `forbid: {event: {type: guard_trip,
  guard: S11}, before: {fault: freeze}}`, which is genuinely meaningful and
  PASSes.
- **`safety_healthy_reading_s11.yaml`** (S11, new file, the anti-nuisance
  half). Same operator actions as the fixed `safety_tc_frozen.yaml` --
  K4 closes, K1 stays commanded on -- but **no fault at all**, run for a
  comparable ~650 sim-second span (forced via a deliberate `within_s: 650`
  clause, not a guess). `virtual_simfw`'s real ADC noise/quantization means
  the safety TC reading is never bit-identical between ticks, so S11's
  window keeps resetting and **never accumulates anywhere near the 600s
  bar**: `s11_never_trips_on_a_live_reading` PASSes across the whole run.
  This scenario is also the "confirm it would FAIL with the fault removed"
  check for `safety_tc_frozen.yaml`'s new trip clause: same setup, minus the
  freeze, and S11 stays silent -- so the freeze fault, not the operator
  actions or the run length, is what makes the trip case trip.
- **`main_safety_disagree_s10.yaml`** (S10). `main_safety_skew.yaml`'s own
  `s10_stays_quiet` is only ever an anti-nuisance case (its 80°C skew is
  deliberately below `tc_disagreement_c`'s 200°C default). This file reuses
  the same `main_safety_disagree` fault mechanism at +250°C, comfortably
  above the bar, no `operator_actions:` needed (S10's zone eligibility does
  not depend on K4/current). Measured **`GUARD_WARN {S10}` at 294.0s after
  the fault fires** -- within the bracket (`within_s: 330`,
  `not_before_s: 260`) built from that same measurement, not a guess (the
  first version of this file used a guessed 295/320 bracket derived from
  assuming the disagreement clock starts exactly at the wire-reported
  `fault_fired` event; it FAILed, because the guard's internal accumulator
  actually starts a few seconds earlier than the wire event is reported --
  see the scenario's own comment for the corrected reasoning). S10 is
  WARN-only (no `SAFETY_TRIP_*` case exists for it), so "genuine trip case"
  for this guard means a genuine WARN, which this is the first scenario to
  produce.

### S6a: audited, not newly provoked -- and precisely why

`mainfault_tc_disconnect.yaml`'s two S6a clauses (`mainfault_trips_s6a`,
`fault_line_asserted`) were re-diagnosed rather than fixed. SaftyFW's own
side is fully wired (`safety_core_build_input()` names
`.main_fault_asserted = discrete_task_main_fault()`), so the previous
`blocked_on:` reasons (pointing at missing SaftyFW wiring) are stale and
replaced. **The real blocker is two levels deep in the fixture, not the
firmware, and this pass confirmed both levels by reading the code rather
than asserting it from the scenario's own prior comment:**

1. `virtual_simfw.c`'s `fault_line_asserted` field (the thing this fixture
   *senses* on the Fault line) is written ONLY by real hardware's
   `i2c_owner.c`, reading a real I2C GPIO-expander pin wired to the ESP's
   GPIO6 -> U1 opto. Grepped this pass: `fault_line_asserted` is *read* in
   three places in `virtual_simfw.c`/`cmd_task.c`/`telemetry.c` and
   *written* in none of them -- it is zero-initialized and stays false for
   the life of the process, regardless of what fault is injected on `tc:0`.
   There is no KilnFW anywhere in `virtual_simfw`/`virtual_dut` to execute
   the "ESP decides to assert its own fault output" step this scenario's
   fault (`disconnected_tc` on `tc:0`) is meant to provoke indirectly.
2. Independently, `run_dut_scenarios.py`'s poll loop never reads
   `fault_line_asserted` from telemetry and forwards it as `dut_core.exe`'s
   optional TICK `<main_fault>` field -- it always calls `dut.tick(...)`
   with that parameter's default (`False`), even though `dut_core.exe`'s own
   protocol has supported the field since the S6a wiring pass.

**Conclusion: S6a is not provokable through this harness at all, by what the
harness chooses to emulate -- not a wiring omission this pass, or any
scenario-writing pass, can close.** Closing gap 1 would require emulating
the I2C-expander/opto path in `virtual_simfw.c` (out of this scenario
library's ownership, `firmware/SimFW/tools/virtual_simfw/src/**`); closing
gap 2 alone would still leave `fault_line_asserted` permanently false. Both
`mainfault_tc_disconnect.yaml` clauses are now annotated `blocked_on:` with
this reasoning (the `fault_line_asserted` clause previously reported a bare
FAIL with no annotation -- an accurate outcome that did not carry the
reason).

### Zone-setpoint capability (`run_dut_scenarios.py`)

New: a scenario's `dut.zone_setpoints:` mapping (`{zone_index: setpoint_c}`)
is parsed once per run and fed into `_FixtureContext`, which now sends that
value instead of NaN for the matching zone's `setpoint_c`, unconditionally
for every zone-eligible tick. Zones with no entry keep `setpoint_c = NaN`
(unchanged default) -- `context_reduce_zones()`'s `z->setpoint_c >
max_setpoint` comparison is false for a NaN either side, so an unset zone
never wins the max. This is the same category of honesty as
`operator_actions:` -- a scenario-declared physical fact a real ESP would be
holding, not a guessed number engineered to make a guard fire.

Also: `_ACTION_TRAILING_MARGIN_S` raised 20s -> 30s and
`_ACTION_MAX_RUN_DURATION_S` raised 600s -> 900s (both local to this
orchestrator script, not `kilnsim.runner`'s own 600s ceiling, which is
unchanged and still applies to every scenario without
`operator_actions:`). S11's genuine trip needs ~660-700 sim-seconds
(startup grace + `frozen_window_s`), past the old local ceiling.

### Verdicts moved among the original 22 scenarios

Two moved, both upward, neither by loosening an expectation -- both closed
by fixing what the clause could actually observe:

| Scenario | Clause | Before | After | Why |
|---|---|---|---|---|
| `safety_tc_frozen` | `frozen_window_trips_s11` | BLOCKED | **PASS** | `heat_commanded` wiring landed since the last run of this file; adding `operator_actions:` let this scenario actually supply it. Trip measured at 664.0s after the freeze fault. |
| `safety_tc_frozen` | `no_early_trip` | BLOCKED | **PASS** | Clause form rewritten from the structurally-vacuous `dut: K4_open` boundary to a genuine `event: guard_trip` boundary (see above). |
| `mainfault_tc_disconnect` | `fault_line_asserted` | FAIL (unannotated) | **BLOCKED** | Re-diagnosed and annotated with the real, fixture-side reason (see S6a section above) rather than left as a bare, reason-less FAIL. |
| `mainfault_tc_disconnect` | `mainfault_trips_s6a` | BLOCKED (stale reason) | BLOCKED (current reason) | Verdict unchanged; the `blocked_on:` text was stale (pointed at SaftyFW wiring that is now done) and is corrected in place, superseded text kept for history. |

`welded_contactor_s9`'s `contactor_weld_engages_on_k4_open` also now reads
PASS in this run (previously recorded BLOCKED in this file) via the
`forbid: {dut: current_absent, after: {...}}` grammar form already present
in that scenario file -- that fix predates this pass (this session did not
author it) and is reported here only because it changes the scenario-level
overall verdict from BLOCKED to PASS in the suite-wide count below.

### Suite-wide count

**Before this pass (22 scenarios, as last recorded in this file): 5 PASS /
11 BLOCKED / 6 FAIL.**

**After this pass, same 22 scenarios: 7 PASS / 10 BLOCKED / 5 FAIL** (+2
PASS: `safety_tc_frozen`, `welded_contactor_s9`; +1 BLOCKED in from FAIL:
`mainfault_tc_disconnect`; -2 BLOCKED out to PASS).

**All 25 scenarios (22 + 3 new, all 3 new PASS): 10 PASS / 10 BLOCKED / 5
FAIL.**

No FAIL verdict moved in either direction -- `cj_fault`, `partial_element`,
`spi_flaky_tc_ic`, `tc_disconnect_ramp`, `tc_disconnect_soak` are unchanged,
pre-existing, and out of this pass's scope (S5/S12 fault-catalog/guard-timing
issues unrelated to the S2/S6a/S10/S11 audit this pass covers).

---

## Earlier revision: the harness stopped inventing reads, and S9 fired for real

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

- **[   PASS]** never_faults: dut:fault_line_asserted never observed inside the forbidden window
- **[   PASS]** never_estops: dut:estop_open never observed inside the forbidden window
- **[BLOCKED]** never_trips: dut:K4_open observed inside the forbidden window (first at seq 2000000000) [BLOCKED: This forbid clause is still satisfied for the wrong reason: K4 is open at sim-time 0 and never closes IN THIS FILE, so it is already true before a healthy no-fault run has had any chance to prove anything. NOTE (updated again, after the operator-actions pass): nothing upstream is missing any more. relay_owner_command_energize() has had a caller since f304392, and run_dut_scenarios.py now replays a scenario's own `dut.operator_actions:` list, so a scenario CAN issue SAFETY_CMD_REQUEST_ENABLE and CAN command a zone relay -- enabled_firing_healthy.yaml does exactly that and observes K4 close at sim-time ~58s and stay closed for a whole healthy firing. The only reason this clause is still vacuous is that THIS file does not schedule those actions.]
- **[SKIPPED]** heat_actually_cycles: triggering event (relay_edge {'relay': 'K1', 'edge': 'close'}) never occurred

## broken_element -- overall: BLOCKED

_Guard inputs presented this run: 46 ticks, context_valid 46, link_up 46, any_current_present 0, K4 energized 0, max eligible zones 3._

- **[BLOCKED]** s4_warns: triggering event (relay_edge {'relay': 'K1', 'edge': 'close'}) never occurred [BLOCKED: RESOLVED on both sides now -- what remains is local to THIS file. The original reason ('context_valid is never set true by safety_core_build_input()') was fixed by commit f304392. The follow-up reason ('nothing in the fixture ever commands a zone relay on') was fixed by the operator-actions pass: run_dut_scenarios.py replays a scenario's own `dut.operator_actions:` list, and commanded_no_current_s4.yaml -- the same broken-element stimulus, plus a commanded K1 and an operator enable -- drives S4 to a genuine WARN 152.8 sim-seconds after K1 closes. This scenario does not schedule those actions, so its own `relay_edge K1 close` trigger still never occurs and the clause still reports 'triggering event never occurred' rather than any verdict about S4.]
- **[BLOCKED]** s4_never_trips: dut:K4_open observed inside the forbidden window (first at seq 2000000000) [BLOCKED: This forbid clause is still satisfied for the wrong reason: K4 is open at sim-time 0 and never closes IN THIS FILE, so it is already true before anything under test has had a chance to say otherwise. NOTE (updated again, after the operator-actions pass): 'no scenario models the operator enable' is no longer true -- commanded_no_current_s4.yaml issues it, watches K4 close at ~58s, watches S4 warn, and then asserts the real design property this clause is reaching for (K4 STILL CLOSED at end of run, because S4 is WARN-only). That is the assertion this clause cannot make while K4 never closes.]

## cj_fault -- overall: FAIL

_Guard inputs presented this run: 30 ticks, context_valid 30, link_up 30, any_current_present 0, K4 energized 0, max eligible zones 3._

- **[   PASS]** s5_never_trips_on_cj_alone: event:guard_trip never observed inside the forbidden window
- **[   FAIL]** s12_warns_at_60c: event:guard_warn not observed within 1s after fault_fired (seq 0)
- **[   FAIL]** s12_trips_at_85c_sustained: dut:K4_open not observed within 65s after fault_fired (seq 0)

## commanded_no_current_s4 -- overall: PASS

_Guard inputs presented this run: 215 ticks, context_valid 215, link_up 215, any_current_present 0, K4 energized 157, max eligible zones 3._

- **[   PASS]** s4_warns_after_the_correlation_window: event:guard_warn observed 150.800s after relay_edge
- **[   PASS]** no_current_ever_flows: dut:current_present never observed inside the forbidden window
- **[   PASS]** s4_never_opens_k4: dut:K4_closed held at end of run (last observed seq 2000000001)

## enabled_firing_healthy -- overall: PASS

_Guard inputs presented this run: 323 ticks, context_valid 323, link_up 323, any_current_present 137, K4 energized 265, max eligible zones 3._

- **[   PASS]** k4_closes_after_startup_grace: dut:K4_closed observed 55.400s after relay_edge
- **[   PASS]** current_flows_once_k4_permits: dut:current_present observed 0.400s after relay_edge
- **[   PASS]** s3_never_trips_during_a_commanded_firing: event:guard_trip never observed inside the forbidden window
- **[   PASS]** s4_never_warns_during_a_commanded_firing: event:guard_warn never observed inside the forbidden window
- **[   PASS]** current_stops_when_the_zone_is_commanded_off: dut:current_absent observed 0.000s after relay_edge
- **[   PASS]** k4_still_closed_at_end: dut:K4_closed held at end of run (last observed seq 2000000001)

## estop_at_boot -- overall: PASS

_Guard inputs presented this run: 19 ticks, context_valid 19, link_up 19, any_current_present 0, K4 energized 0, max eligible zones 3._

- **[   PASS]** relay_never_energizes: dut:K4_closed never observed inside the forbidden window
- **[   PASS]** still_open_at_end: dut:K4_open held at end of run (last observed seq 2000000000)

## estop_midfire -- overall: PASS

_Guard inputs presented this run: 44 ticks, context_valid 44, link_up 44, any_current_present 0, K4 energized 0, max eligible zones 3._

- **[SKIPPED]** fast_trip: triggering event (fault_fired {'slot': 'estop'}) never occurred
- **[SKIPPED]** stays_latched_after_physical_release: triggering event (fault_cleared {'slot': 'estop'}) never occurred
- **[   PASS]** latched_at_end: dut:K4_open held at end of run (last observed seq 2000000000)

## main_safety_disagree_s10 -- overall: PASS

_Guard inputs presented this run: 46 ticks, context_valid 46, link_up 46, any_current_present 0, K4 energized 0, max eligible zones 1._

- **[   PASS]** s10_warns_after_sustained_disagreement: event:guard_warn observed 294.000s after fault_fired
- **[   PASS]** no_warn_before_the_skew_fires: event:guard_warn never observed inside the forbidden window

## main_safety_skew -- overall: BLOCKED

_Guard inputs presented this run: 26 ticks, context_valid 26, link_up 26, any_current_present 0, K4 energized 0, max eligible zones 1._

- **[BLOCKED]** s1_trips_conservatively_early: event:guard_trip not observed within 120s after fault_fired (seq 0) [BLOCKED: abs_max_temp_c defaults to 0.0 and safety_core.c never commissions it (its own comment: permanently the conservative 'nothing commissioned' state until config_store exists) -- safety_guards.c's S1 block only evaluates `if (cfg->abs_max_temp_c > 0.0f)`, so S1 cannot trip at all against present-day SaftyFW no matter what the fixture does, independent of whether this clause's own within_s:120 guess is even the right number.]
- **[   PASS]** s10_stays_quiet: event:guard_warn never observed inside the forbidden window

## mainfault_tc_disconnect -- overall: BLOCKED

_Guard inputs presented this run: 19 ticks, context_valid 19, link_up 19, any_current_present 0, K4 energized 0, max eligible zones 3._

- **[BLOCKED]** no_early_trip: dut:K4_open observed inside the forbidden window (first at seq 2000000000) [BLOCKED: K4 is reported open from this harness's very first observation, because nothing in current SaftyFW energizes it until an operator enable survives the 60 s startup grace, and this scenario schedules no `dut.operator_actions:` at all -- so a `forbid: {dut: K4_open}` clause always sees an 'open' edge at sim-time ~1 s, long before the oc_main fault fires, whatever any guard decides (virtual_dut/README.md Finding 2 and its corollary). This clause reported PASS until 2026-08-20 purely by accident of timing: virtual_simfw was advancing its sim clock by timescale**2 per wall second, so at timescale 10 the FIRST telemetry frame did not land until sim-time ~45 s -- after the fault -- and the K4-open edge was therefore recorded after the boundary rather than before it. Fixing the clock (one sim-second per wall second per unit of timescale) moved the first frame to ~1 s and exposed the clause's real state. Nothing about the DUT changed; a vacuous PASS became an honest non-PASS.]
- **[BLOCKED]** mainfault_trips_s6a: event:guard_trip not observed within 3s after fault_fired (seq 0) [BLOCKED: STALE REASON, SUPERSEDED 2026-08-20 -- kept below for history, real current reason follows. safety_core_build_input() now names `.main_fault_asserted = discrete_task_main_fault()` (immediately after `.estop_pressed`), so SaftyFW's own half of this signal is fully wired; that is no longer what blocks this clause. The real blocker is one level deeper, in the FIXTURE, not the firmware: `virtual_simfw.c`'s `fault_line_asserted` field (the Fault-opto sensing this harness's own header comment already says the fixture only 'senses', never drives) is set ONLY by real hardware's `i2c_owner.c`, reading a real I2C GPIO-expander pin (`EXP1_PIN_FAULT_LINE`) wired to the ESP's GPIO6 -> U1 opto. `virtual_simfw.c` has no I2C-expander emulation of that path at all (grepped this pass: `fault_line_asserted` is read in three places in virtual_simfw.c/cmd_task.c/telemetry.c and WRITTEN in none) -- it is zero-initialized and stays false for the entire life of the process, regardless of what TC fault is injected on tc:0. Disconnecting tc:0 has no code path to it at all: this fixture never executes KilnFW's own guard-6 logic (there is no KilnFW anywhere in virtual_simfw or virtual_dut, only the real SaftyFW guard code under test), so the 'ESP decides to assert its own fault output' step this scenario's header comment describes is not something any fault injection here can cause. Separately and independently, run_dut_scenarios.py's poll loop never reads `fault_line_asserted` from telemetry and passes it as dut_core.exe's optional TICK `<main_fault>` field at all (it always calls `dut.tick(...)` with the parameter's default, `main_fault=False`) -- so even a fixture that DID assert the Fault line would still need that wiring added before this scenario could see it. Both gaps are fixture-side; neither is a SaftyFW guard defect or a scenario-authoring gap. See GUARD_TEST_MATRIX.md §6 for the full three-way accounting (SaftyFW input: wired; fixture Fault-line emulation: absent; orchestrator TICK wiring: absent).]
- **[BLOCKED]** fault_line_asserted: dut:fault_line_asserted not observed within 3s after fault_fired (seq 0) [BLOCKED: Same fixture-side gap as `mainfault_trips_s6a` immediately above: `virtual_simfw.c` has no producer that ever sets `fault_line_asserted` true (only real hardware's `i2c_owner.c` does), so this clause cannot pass against this fixture regardless of what fault is injected or when. This clause previously reported as a bare FAIL (no blocked_on annotation) -- SCENARIO_RESULTS.md's own honesty rule (report.py's BLOCKED-vs-FAIL distinction) says a clause blocked on known, tracked fixture incompleteness should read as BLOCKED, not FAIL; a bare FAIL here was accurate about the outcome but did not carry the reason, and made the suite look more broken than it is.]
- **[   PASS]** trip_latched: dut:K4_open held at end of run (last observed seq 2000000000)

## partial_element -- overall: FAIL

_Guard inputs presented this run: 14 ticks, context_valid 14, link_up 14, any_current_present 0, K4 energized 0, max eligible zones 3._

- **[BLOCKED]** no_guard_trips: dut:K4_open observed inside the forbidden window (first at seq 2000000000) [BLOCKED: This forbid clause is still satisfied for the wrong reason: K4 is open at sim-time 0 and never closes, so it is already true before anything under test has had a chance to say otherwise. NOTE (updated after SaftyFW commit f304392): the previous wording here -- 'relay_owner_command_energize() is called by nothing in the current source tree' -- is no longer true. It now has a caller, safety_core_request_enable(), reached from link_task.c's SAFETY_CMD_REQUEST_ENABLE (0x02) decoder. That path is driven only by an explicit operator/PC command (KilnFW's uart_bridge.c -> safety_link_request_enable(), i.e. PcTools' safety_request_enable MCP tool); KilnFW does NOT request enable automatically when a profile runs, and no scenario in this library models that operator step.]
- **[   FAIL]** current_still_present_just_reduced: dut:current_present not observed within 5s after fault_fired (seq 0)

## power_blip -- overall: BLOCKED

_Guard inputs presented this run: 49 ticks, context_valid 49, link_up 49, any_current_present 0, K4 energized 0, max eligible zones 3._

- **[   PASS]** mainfault_reads_healthy_during_blip: event:guard_trip never observed inside the forbidden window
- **[SKIPPED]** relays_deenergize_no_current_during_blip: triggering event (dut_power {'state': False}) never occurred
- **[BLOCKED]** s6b_stays_at_warn_not_trip: dut:K4_open observed inside the forbidden window (first at seq 2000000000) [BLOCKED: One of the two original reasons here is now resolved, the other is not. RESOLVED: link_up is no longer permanently false -- commit f304392 wired it to link_task_link_up() (a CRC-valid frame within LINK_UP_RECENCY_MS), and S6b's unconditional ~120s hard-backstop trip, which previously fired in 16 of 19 virtual_dut scenarios, now fires in none of them. This clause's 'stays at warn, not trip' claim is finally testable in principle. NOT RESOLVED: K4 is still open from sim-time 0 (no scenario issues the operator SAFETY_CMD_REQUEST_ENABLE -- see the K4 blocked_on entries elsewhere in this library), so the forbid clause is still satisfied before the blip has had a chance to prove anything.]
- **[SKIPPED]** safe_resume: triggering event (dut_power {'state': True}) never occurred

## runaway_zone -- overall: BLOCKED

_Guard inputs presented this run: 48 ticks, context_valid 48, link_up 48, any_current_present 0, K4 energized 0, max eligible zones 3._

- **[BLOCKED]** s3_catches_it_first: triggering event (fault_fired {'slot': 'runaway'}) never occurred [BLOCKED: RESOLVED on the SaftyFW side, still blocked on the fixture. The previous reason here -- 'context_valid is never set true by safety_core_build_input()' -- was fixed by commit f304392: context_valid, any_current_present and relay_commanded_recently are all populated for real now (virtual_dut measures context_valid true on every poll of this scenario), so S3 is EVALUATED every tick instead of being force-reset. What it never sees is current: firmware/SimFW/src/tasks/sim_engine.c gates heater duty and CT current on K4, K4 never closes (no scenario issues the operator SAFETY_CMD_REQUEST_ENABLE -- see the K4 blocked_on entries elsewhere in this library), so any_current_present is false for the whole run (measured 0 of ~83 polls), and S3's `any_current_present && !relay_commanded_recently` condition can never be true.]
- **[   PASS]** s8_produces_no_trip_of_its_own_today: event:guard_trip never observed inside the forbidden window

## s2_setpoint_overshoot -- overall: PASS

_Guard inputs presented this run: 77 ticks, context_valid 77, link_up 77, any_current_present 53, K4 energized 53, max eligible zones 3._

- **[   PASS]** s2_trips_after_sustained_overshoot: event:guard_trip observed 267.000s after relay_edge
- **[   PASS]** no_trip_before_k4_closes: event:guard_trip never observed inside the forbidden window

## safety_healthy_reading_s11 -- overall: PASS

_Guard inputs presented this run: 144 ticks, context_valid 144, link_up 144, any_current_present 132, K4 energized 133, max eligible zones 3._

- **[   PASS]** current_flows_once_k4_permits: dut:current_present observed 2.000s after relay_edge
- **[   PASS]** s11_never_trips_on_a_live_reading: event:guard_trip never observed inside the forbidden window
- **[   PASS]** k4_still_closed_after_a_full_s11_window: dut:K4_closed observed 0.000s after relay_edge
- **[   PASS]** k4_still_closed_at_end: dut:K4_closed held at end of run (last observed seq 2000000001)

## safety_tc_frozen -- overall: PASS

_Guard inputs presented this run: 153 ticks, context_valid 153, link_up 153, any_current_present 119, K4 energized 119, max eligible zones 3._

- **[   PASS]** no_early_trip: event:guard_trip never observed inside the forbidden window
- **[   PASS]** frozen_window_trips_s11: event:guard_trip observed 664.000s after fault_fired
- **[   PASS]** trip_latched: dut:K4_open held at end of run (last observed seq 2000000003)

## spi_flaky_tc_ic -- overall: FAIL

_Guard inputs presented this run: 30 ticks, context_valid 30, link_up 30, any_current_present 0, K4 energized 0, max eligible zones 3._

- **[   PASS]** not_an_instant_trip: dut:K4_open never observed inside the forbidden window
- **[   FAIL]** warns_then_trips_like_disconnect: dut:K4_open not observed within 65s after fault_fired (seq 0)

## stuck_load_no_command_s3 -- overall: PASS

_Guard inputs presented this run: 137 ticks, context_valid 137, link_up 137, any_current_present 20, K4 energized 29, max eligible zones 3._

- **[   PASS]** uncommanded_current_appears_with_the_weld: dut:current_present observed 0.000s after fault_fired
- **[   PASS]** s3_trips_on_uncommanded_load: event:guard_trip observed 19.400s after fault_fired
- **[   PASS]** s3_does_not_trip_before_there_is_any_current: event:guard_trip never observed inside the forbidden window
- **[   PASS]** k4_opens_and_stays_open: dut:K4_open held at end of run (last observed seq 2000000003)
- **[   PASS]** current_stops_when_k4_opens: dut:current_absent observed 1.000s after guard_trip

## tc_disconnect_ramp -- overall: FAIL

_Guard inputs presented this run: 32 ticks, context_valid 32, link_up 32, any_current_present 0, K4 energized 0, max eligible zones 3._

- **[   FAIL]** warns_first: event:guard_warn not observed within 6s after fault_fired (seq 0)
- **[   FAIL]** trips_after_blind_grace: dut:K4_open not observed within 65s after fault_fired (seq 0)
- **[   FAIL]** reports_nan_not_stale_value: safety_temp_valid state never observed; cannot be dut:safety_temp_valid at end

## tc_disconnect_soak -- overall: FAIL

_Guard inputs presented this run: 44 ticks, context_valid 44, link_up 44, any_current_present 0, K4 energized 0, max eligible zones 3._

- **[   FAIL]** warns_first: event:guard_warn not observed within 6s after fault_fired (seq 0)
- **[   FAIL]** trips_after_blind_grace: dut:K4_open not observed within 65s after fault_fired (seq 0)
- **[   FAIL]** reports_nan_not_stale_value: safety_temp_valid state never observed; cannot be dut:safety_temp_valid at end

## tc_flaky -- overall: BLOCKED

_Guard inputs presented this run: 451 ticks, context_valid 451, link_up 451, any_current_present 0, K4 energized 0, max eligible zones 3._

- **[BLOCKED]** no_trip_from_flapping: dut:K4_open observed inside the forbidden window (first at seq 2000000000) [BLOCKED: This forbid clause is still satisfied for the wrong reason: K4 is open at sim-time 0 and never closes, so it is already true before anything under test has had a chance to say otherwise. NOTE (updated after SaftyFW commit f304392): the previous wording here -- 'relay_owner_command_energize() is called by nothing in the current source tree' -- is no longer true. It now has a caller, safety_core_request_enable(), reached from link_task.c's SAFETY_CMD_REQUEST_ENABLE (0x02) decoder. That path is driven only by an explicit operator/PC command (KilnFW's uart_bridge.c -> safety_link_request_enable(), i.e. PcTools' safety_request_enable MCP tool); KilnFW does NOT request enable automatically when a profile runs, and no scenario in this library models that operator step.]
- **[   PASS]** no_warn_storm: event:guard_warn never observed inside the forbidden window

## tc_noise_storm -- overall: BLOCKED

_Guard inputs presented this run: 19 ticks, context_valid 19, link_up 19, any_current_present 0, K4 energized 0, max eligible zones 3._

- **[BLOCKED]** no_trip_ever: dut:K4_open observed inside the forbidden window (first at seq 2000000000) [BLOCKED: Partially resolved. S10 is no longer blocked: commit f304392 populates context_valid/zone_count/nearest_zone_measured_c, and S10 is now genuinely evaluated against this scenario's noise (virtual_dut measures context_valid true on every poll). S1 remains blocked on uncommissioned abs_max_temp_c (Phase 9). The forbid clause itself is still satisfied for the wrong reason, though: K4 is open from sim-time 0 because no scenario issues the operator SAFETY_CMD_REQUEST_ENABLE, so 'no trip ever' still is not proving the anti-nuisance behavior it names.]
- **[   PASS]** no_fault_line_latch: dut:fault_line_asserted never observed inside the forbidden window

## tc_stuck -- overall: BLOCKED

_Guard inputs presented this run: 30 ticks, context_valid 30, link_up 30, any_current_present 0, K4 energized 0, max eligible zones 3._

- **[BLOCKED]** sample_counter_goes_stale: event:guard_warn not observed within 13s after fault_fired (seq 0) [BLOCKED: Re-diagnosed after SaftyFW commit f304392. The previous reason -- 'context_valid is never set true' -- no longer holds: context_valid is true on every poll of this scenario now. S13 is still dormant, for two reasons that are both COMMISSIONING gaps rather than wiring gaps, and both deliberate. (1) safety_core_build_input() leaves sample_counter_advancing false on purpose: deciding whether the borrowed channel advanced requires a commissioned borrowed_zone_index (0..2) naming which context zone is the borrowed one, and that field exists nowhere in the codebase (no config_store field, no safety_guard_cfg_t field) -- hardcoding zone 0 would invent a commissioning decision and be silently wrong on any kiln whose borrowed zone is not zone 0. (2) safety_guards.c gates the whole S13 block on cfg->tc_source being BORROWED_ZONE or BOTH, and it defaults to OWN_J7. Same category as S1's abs_max_temp_c.]
- **[BLOCKED]** trips_after_stale_trip_deadline: dut:K4_open not observed within 65s after fault_fired (seq 0) [BLOCKED: Same two commissioning gaps as `sample_counter_goes_stale` above (uncommissioned borrowed_zone_index, tc_source defaulting to OWN_J7) -- S13 cannot warn, so it certainly cannot escalate to a trip. Separately, and independently, K4 is open from sim- time 0 so there would be no K4_open edge for this clause to match even if S13 did trip.]

## welded_contactor_s9 -- overall: PASS

_Guard inputs presented this run: 216 ticks, context_valid 216, link_up 216, any_current_present 148, K4 energized 29, max eligible zones 3._

- **[   PASS]** uncommanded_current_appears_with_the_weld: dut:current_present observed 0.000s after fault_fired
- **[   PASS]** initial_trip: event:guard_trip observed 19.400s after fault_fired
- **[   PASS]** contactor_weld_engages_on_k4_open: dut:current_absent never observed inside the forbidden window
- **[   PASS]** s9_escalates: event:trip_ineffective_latched observed 9.600s after relay_edge
- **[   PASS]** stays_open_and_latched: dut:K4_open held at end of run (last observed seq 2000000003)
- **[   PASS]** escalation_latched_at_end: event:trip_ineffective_latched occurred (last at seq 2000000004)

## welded_ssr_midfire -- overall: BLOCKED

_Guard inputs presented this run: 373 ticks, context_valid 373, link_up 373, any_current_present 236, K4 energized 241, max eligible zones 3._

- **[   PASS]** current_flows_during_the_commanded_firing: dut:current_present observed 0.200s after relay_edge
- **[   PASS]** s4_never_warns_while_commanded_and_conducting: event:guard_warn never observed inside the forbidden window
- **[   PASS]** s3_waits_out_the_correlation_window: event:guard_trip never observed inside the forbidden window
- **[   PASS]** s3_trips_after_the_window_expires: event:guard_trip observed 179.000s after fault_fired
- **[BLOCKED]** no_early_trip: dut:K4_open observed inside the forbidden window (first at seq 2000000000) [BLOCKED: Not about S3 at all: K4 is reported open from this harness's very first observation, because nothing in current SaftyFW energizes it until the operator enable survives the 60 s startup grace -- and the first telemetry frame lands well before that. So a `forbid: {dut: K4_open}` clause sees an 'open' edge at sim-time ~1 s, long before the weld, no matter what any guard decides. See firmware/SimFW/tools/virtual_dut/README.md Finding 2 and its corollary. `s3_waits_out_the_correlation_window` above is the clause that genuinely tests early-trip behavior here, and it keys off the guard_trip event rather than the K4 level for exactly this reason.]
- **[   PASS]** trip_latched: dut:K4_open held at end of run (last observed seq 2000000003)
- **[   PASS]** current_decays_after_k4_opens: dut:current_absent observed 0.400s after relay_edge
