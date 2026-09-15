# Fuzzy scenario evidence, consolidated — 2026-09-14

Answers one question: **did the fuzzy layer show promise in simulation of the
cases we can't bench test?** Consolidates `docs/SCENARIO_SIMULATION_PLAN.md`
(`32b10a34`), `docs/audits/scenario_factorial_design_2026-09-14.md`
(`75bb7b8e`), `docs/audits/scenario_simulation_implementation_2026-09-14.md`,
`docs/audits/fuzzy_first_hardware_run_2026-09-14.md`, and a fresh run of
`kilnctl_sim_scenarios.exe` taken tonight against the current tree (this
session changed no production code — see "What was run" below). Superseded
figures are named and dated; only tonight's run is treated as current.

## What was run, and why the numbers here are new

`sim_scenarios.c`/`sim_scenario_table.c` (WI-1 through WI-8, per the
implementation audit) were already committed. This session built them fresh
into a private out-dir (`build_host_tests.ps1 -OutDir C:/wt/hostbuild_fuzzy_eval`,
44 executables built) and ran `kilnctl_sim_scenarios.exe --shard 0 --of 1`
directly to capture every column, including the `CLASSIFICATION`/`SEP_CHECK`
lines the `run_sim_scenarios.ps1` determinism wrapper strips. No prior audit
quotes this exact run's numbers — the implementation audit stopped after
WI-1 pending a since-resolved `firing_score.c`/`firing_compare.c` blocker
(`560cffe0`, `d41da85f`), so this is the first place S1–S12's six-arm,
four-objective table is reported in full with the `A_STATIC_MATCHED` vs
`A_FUZZY50` numbers spelled out rather than left inside the binary
classification.

**On `lag_ticks_dropped_frac`:** no field by that name exists anywhere in
`firmware/KilnFW/App` (checked by grep). The nearest instrumented quantity is
`sat_frac` — the whole-firing fraction of ticks at duty saturation, printed
by `sim_scenarios.exe` as a separate `SATFRAC` line per (scenario, arm) so it
does not disturb the TSV data-row regex. `FIRING_SUBSCORE_LAG_S` is
documented (hardware audit, quoted below) as "unsigned, drops
saturated-and-short ticks" — `sat_frac` is the available proxy for how much
of a scenario's ramp ran through that drop condition, not a literal count of
dropped ticks. Reported as such below; treat this as an acknowledged gap in
the current instrumentation, not a filled-in number.

## The two comparisons, defined

- **Fuzzy vs plain PID**: does `A_FUZZY50` separate from `A_PID` on any
  objective by more than 0.5 °C (or 0.5 °C-equivalent for `LAG_S`, converted
  at the scenario's own ramp rate)?
- **Fuzzy vs matched-effective-gain static** (`A_STATIC_MATCHED`): its
  Kp/Ki/Kd multipliers are `A_FUZZY50`'s own **ramp-phase mean** applied
  multipliers in that scenario, re-measured per scenario, inference off
  (`1570a65a`'s fix — never the rule table's centre-cell max ×0.75/×1.25/
  ×0.75). If `A_STATIC_MATCHED` reproduces `A_FUZZY50` inside materiality,
  the label is `GAIN_ONLY`; if it does not, `INFERENCE`. This is the decisive
  test and it is what the harness itself computes and prints per scenario.

All four objectives are reported separately below and never aggregated, per
the plan's own rule (§5.1).

## Per-scenario table (this session's run, `--of 1`, byte-identical to `--of 4`)

Materiality = 0.5 °C (or °C-equivalent), applied per objective. `Δ(F,S)` =
`A_FUZZY50` minus `A_STATIC_MATCHED`; sign shown so direction is legible.
`applied mult` = `A_FUZZY50`'s measured ramp-phase mean Kp/Ki/Kd multipliers.
`sat_frac` = `A_FUZZY50`'s whole-firing saturated-duty fraction (headroom
proxy, see above).

| # | Scenario | vs PID | Δ lag (°C-eq) | Δ steady (°C) | Δ peak (°C) | Δ under (°C) | Class | applied mult (Kp/Ki/Kd) | sat_frac |
|---|---|---|---|---|---|---|---|---|---|
| S0 | NULL_SLOW (control) | no sep | 0.00 | 0.00 | 0.00 | 0.02 | — | 0.796/1.200/0.800 | 0.00 |
| S1 | BASELINE | separates | 0.00 | 0.01 | 0.06 | 0.10 | **GAIN_ONLY** | 0.869/1.157/0.843 | 0.00 |
| S3 | SENSOR_CENTRE (isolation check) | separates | 0.00 | 0.02 | 0.07 | 0.11 | **GAIN_ONLY** | 0.874/1.157/0.843 | 0.00 |
| **S2** | **SENSOR_NEAR_ELEMENT** | separates | 0.33 | **0.48** | **1.44** | **0.86** | **INFERENCE** | 0.960/0.986/1.015 | 0.169 |
| **S4** | **SENSOR_NEAR_FAST_RAMP** | separates | 0.08 | **0.46** | **1.45** | **0.54** | **INFERENCE** | 0.962/0.983/1.017 | 0.175 |
| **S5** | **MASS_HEAVY** | separates | 0.04 | 0.01 | 0.32 | 0.07 | **GAIN_ONLY** | 0.909/1.160/0.841 | 0.171 |
| **S6** | **MASS_LIGHT** | **no sep from PID at all** | — | — | — | — | (n/a) | 0.853/1.154/0.846 | 0.00 |
| **S7** | **TUNE_HOT** | separates | 0.21 | 0.07 | 0.04 | 0.03 | **GAIN_ONLY** | 0.801/0.947/1.053 | 0.280 |
| **S8** | **TUNE_COLD** | separates | 0.08 | 0.09 | 0.00 | 0.01 | **GAIN_ONLY** | 0.846/1.199/0.801 | 0.00 |
| **S9** | **TUNE_SLOW_INTEGRAL** | separates | 0.08 | 0.02 | 0.00 | 0.18 | **GAIN_ONLY** | 0.801/1.064/0.937 | 0.00 |
| **S10** | **KILN_HIGH_T** `[f_rad=0.05 ASSUMED]` | **no sep from PID at all** | — | — | — | — | (n/a) | 0.759/0.962/1.000 | 0.286 |
| **S11** | **KILN_HIGH_T_SCHEDULED** `[f_rad=0.05 ASSUMED]` | **no sep from PID at all** | — | — | — | — | (n/a) | 0.759/0.962/1.000 | 0.285 |
| S12 | COMPOUND_WORST (all three at once) | separates | **1.28** | 0.39 | 0.09 | 0.02 | **INFERENCE** | 1.096/0.861/1.139 | 0.332 |

Bold rows are the owner's four named un-benchable cases (S12 is the compound
of all three mismatch types plus a bad tune, reported separately below).
S0/S1/S3 are controls/anchors, not one of the four cases.

## Per un-benchable case

### 1. Sensor ~5× closer to the elements than to the load (S2, S4)

**Fuzzy separates from the matched-gain arm on two of four objectives, and
by more than the materiality bar in both scenarios.** `A_FUZZY50` overshoots
the dwell entry **1.4 °C more** than the gain-matched static arm would (S2:
+1.44 °C, S4: +1.45 °C) while undershooting **0.5–0.9 °C less** (S2: −0.86 °C,
S4: −0.54 °C) and reading a **~0.46–0.48 °C worse** steady-state RMS. This is
`INFERENCE`, not `GAIN_ONLY`: the static arm, built from the *same* measured
ramp-phase mean multipliers, does not reproduce it. Something about *when*
the multipliers change (not just their average value) matters here — the
inference is doing something the fixed gains cannot.

Direction matters: this is not an unambiguous win. Fuzzy trades a bigger peak
overshoot for a smaller entry undershoot relative to the static arm — a
real difference in *character*, not a strict improvement on every axis.
Ramp-rate tracking (`LAG_S`) barely moves (0.08–0.33 °C-equivalent, below or
at the edge of materiality) — the loop's raw lag is not where this case
shows up.

`sat_frac` is 0.17–0.18 for both — non-trivial duty saturation during the
ramp, which is exactly the condition under which `LAG_S`'s
"drops saturated-and-short ticks" caveat applies, so the small `LAG_S`
separation should be read with that qualification, not as a clean number.

**This is the strongest and cleanest `INFERENCE` result of the whole table
for a mechanism-matched pair of scenarios** (S2 at 150 °C/hr, S4 at
300 °C/hr — the literature's own predicted direction, that overshoot from a
leading sensor should worsen at a faster ramp, is qualitatively visible: peak
delta and steady delta are essentially unchanged, but present at both
rates).

### 2. A bad starting tune (S7 hot, S8 cold, S9 slow-integral)

**No case here beats materiality against the matched-gain arm.** All three
separate from plain PID (as expected — a genuinely bad tune is a big enough
mismatch that even a bounded ±50 % nudge does something), but in every one
the static arm built from the same measured multipliers reproduces the
result inside 0.5 °C on all four objectives (largest residual: S7's 0.21 °C
lag-equivalent, S9's 0.18 °C undershoot). **`GAIN_ONLY` in all three.** The
benefit these scenarios show is the gain magnitude fuzzy happens to land on,
not anything about reading error/rate and choosing a response — a static
multiplier set to the same mean would have done the same job.

S7 (`TUNE_HOT`) was the plan's own named acceptance case for expecting a
benefit (`P1`, factorial doc §6.1: "fuzzy's rule table backs Kp off on large
sustained error, which is exactly the right lever against excess
proportional gain"). The mechanism is confirmed — Kp is nudged to ×0.80, the
strongest Kp cut in the table — but the *inference* claim is not: a static
×0.80 does the same thing.

### 3. High temperature (~1200 °C) (S10, S11)

**Clean negative: fuzzy does not separate from plain PID at all**, in either
the fixed-schedule kiln-scale scenario (S10) or the per-segment-retuned upper
bound (S11). Every objective's diff is below materiality (largest:
`d_lag_equiv_c = 0.08`). Multipliers do move (Kp to ×0.76, the largest Kp cut
in the whole table, larger than TUNE_HOT's), but they move the *same amount*
whether or not this makes any measurable difference — the kiln-scale plant's
duty stays saturated for ~28.5 % of the run (`sat_frac` 0.285–0.286, the
highest in the table alongside S12), and a controller pinned at duty limits
for over a quarter of the firing has little room for a gain nudge, of either
kind, to matter. This reproduces the plan's own advance prediction P4
("whether fuzzy's bounded ±50 % nudge can cover any of a 20× drift... my
expectation is it cannot").

**This entire result carries `[f_rad=0.05 ASSUMED]`** — the radiative-loss
split constant is unmeasured at any temperature in this dataset
(`SCENARIO_SIMULATION_PLAN.md` §2.3). A different assumed split changes the
plant's high-temperature character and could change this outcome; it is not
re-derivable from what exists today.

### 4. Changed thermal mass (S5 heavy, S6 light)

**Split result, and the light-mass half is a clean negative.** `MASS_HEAVY`
(load added after tuning, ×3.0 capacity) separates from plain PID but the
static arm reproduces it inside materiality on all four objectives (largest
residual 0.32 °C, entry peak) — `GAIN_ONLY`. `MASS_LIGHT` (kiln emptied after
tuning, ×0.5 capacity — the aggressive direction, where the plant is now
faster than the tune assumed) **does not separate from plain PID at all**;
its `sat_frac` is 0.00, so this is not a saturation-masking effect, the
mismatch genuinely does not move any of the four objectives past 0.5 °C
under this control mode at this magnitude.

Neither mass scenario shows `INFERENCE`.

## S12 COMPOUND_WORST — all three at once

Combining sensor placement, heavy mass, and a hot tune at once produces the
largest lag separation in the entire table (1.28 °C-equivalent, versus a
0.5 °C bar) and it is classified `INFERENCE` — the static arm at the same
mean multipliers does not reproduce it. This is consistent with, but not
proof of, an interaction effect between the three mismatch types (only one
compound point was run; see "what is not measured" below — a factorial is
required to actually attribute this to an interaction rather than to
whichever single factor dominates at this particular combination).

## Simulated centre-cell occupancy

`sim_scenarios.c` has no direct cell-occupancy instrumentation (same gap the
hardware audit already noted for the board itself — `pid_fuzzy.c` computes
membership internally and does not log it). The available proxy is the
ramp-phase mean applied multiplier: at `strength_pct = 50`, a multiplier of
exactly 1.0/1.0/1.0 corresponds to a tick spent entirely in the fuzzy table's
near-identity centre cell; deviation from 1.0 reflects time-weighted
membership in outer cells.

By that proxy, **every scenario in this suite drives the mean multiplier
measurably away from 1.0** — from a modest ×0.87/×1.16/×0.84 at the bench
baseline (S1/S3) to ×0.76/×0.96/×1.00 at kiln scale (S10/S11) and
×0.80/×0.95/×1.05 for a hot tune (S7). None of the 13 scenarios reproduces
anything close to the real board's measured ~95–96 % centre-cell occupancy
(`fuzzy_first_hardware_run_2026-09-14.md`) at the level of the *mean*
multiplier. This is not the same measurement (a simulated per-tick
cell-occupancy histogram was not built this session — see below), so the two
numbers are not directly comparable, but they point the same direction as
the qualitative finding: **every un-benchable case pushes the fuzzy layer
further from the centre cell than an ordinary bench firing does**, which is
the necessary (not sufficient) condition for the inference layer to have any
room to act.

**What this does NOT establish**: a true per-tick occupancy histogram for
each simulated scenario, comparable apples-to-apples with the board's
post-hoc reconstruction. Building that requires instrumenting
`pid_fuzzy_adjust()` (or a scenario-side shadow computation) to log
per-tick `(error_bucket, rate_bucket)`, which does not exist in
`sim_scenarios.c` today. Flagged as unmeasured, not approximated further.

## What is NOT yet measured

1. **The 263-cell factorial was never built.** `scenario_factorial_design_
   2026-09-14.md` designs a full 2^8 factorial (resolution VIII, no aliasing)
   over sensor placement × unevenness × mass × kiln span × ramp × tune ×
   duty-margin × Biot-like ratio, predicting (`P8`) that the inference-vs-
   gain-only difference falls below materiality in ≥80 % of cells. **This
   session did not build or run it** — it remains a designed-but-unexecuted
   plan. The 13 named scenarios above are single points in that 8-dimensional
   space, not a sample of it.
2. **What the 13 points can and cannot establish.** They give one value per
   factor combination actually run, each anchored to the plan's own SEP
   pin. Every un-benchable case is represented by only 1–3 points along its
   own axis (e.g., mass at ×0.5 and ×3.0 only, no intermediate; sensor bias
   at only 0 and 5/6). **They cannot establish an interaction** between two
   mismatch types except at the single compound point S12 already ran — the
   factorial exists specifically because `SCENARIO_SIMULATION_PLAN.md` §1
   and the factorial-design doc's P11 predict placement and unevenness act
   through *products*, not sums, and a one-factor-at-a-time sweep cannot see
   that. S2/S4/S5 in isolation vs. S12 combined is suggestive (S12's lag
   separation, 1.28 °C-eq, is far larger than any single-factor scenario's)
   but is one data point, not a measured interaction surface.
3. **`lag_ticks_dropped_frac` does not exist as instrumentation**; `sat_frac`
   is reported in its place, as stated above.
4. **The adaptive 9-firing convergence trajectories** (`A_PID_AT`/
   `A_FUZZY_AT`) are reported only as single-firing snapshots in this table
   (identical to `A_PID`/`A_FUZZY50` respectively — the notes column says so
   explicitly). The real 9-firing chained result lives in
   `sim_scenarios_adaptive.exe` (WI-8) and found something orthogonal to this
   report's question: across 11 of 13 scenarios, `adaptive_tune` never
   harvested even the minimum 4 dwell observations needed to attempt a
   refit, on either arm — the settle-slope/duty-stability gate is evaluated
   once per dwell with no retry and a normal ramp-into-dwell transient is
   wide enough to void it. This independently reproduces the real board's
   own `enabled=false, lifetime=0` finding. It says nothing about whether
   fuzzy helps; it says the self-improving layer this suite was also asked
   to test essentially never engages under these profiles, on hardware or in
   simulation.

## Reconciling with the hardware run

The one real firing with fuzzy engaged spent ~95–96 % of its runtime in the
centre rule cell across all three zones (`fuzzy_first_hardware_run_
2026-09-14.md`), making the layer effectively a fixed multiplier triple for
the overwhelming majority of that run. **None of this session's 13 scenarios
directly reproduces the hardware's occupancy measurement** (no per-tick
histogram was built here, see above), but every scenario's *mean* applied
multiplier sits further from 1.0 than the bench scenarios do, and the two
scenarios that show a genuine `INFERENCE` result (S2, S4 — sensor near the
elements) are exactly the ones with the largest measured duty saturation
outside the kiln-scale/compound scenarios (`sat_frac` 0.17–0.18). That is
consistent with — not proof of — the idea that a scenario has to push the
loop away from ordinary bench conditions before the inference layer has
anything to contribute. On the bench, at the conditions actually
demonstrated, the layer had almost no room to act; nothing in this suite
contradicts that, and the one case (S2/S4) where simulation shows real
separation is precisely a condition the bench cannot produce.

## Prior figures superseded by tonight's run

- Earlier `A_STATIC_MATCHED` measurements using the rule table's centre-cell
  maximum (×0.75/×1.25/×0.75) rather than the ramp-phase mean are wrong per
  `1570a65a` and are not used anywhere in this document; tonight's run uses
  only the corrected two-pass, re-measured-per-scenario construction (WI-5,
  verified present in `sim_scenarios.c`'s source read this session).
- The kiln-scale "saturation fell to ~0.29, fuzzy genuinely engaged (Kp
  ×0.76)" figure quoted in this task's brief matches tonight's S10/S11
  numbers exactly (`sat_frac` 0.285–0.286, Kp ×0.759) — reproduced, not
  superseded.
- No number from a pre-WI-5 run is cited anywhere above.

## Plain answer

**Fuzzy shows one genuine, inference-attributable benefit in simulation, and
it is exactly the case the owner most wanted to know about: a thermocouple
mounted close to the elements.** In both sensor-placement scenarios (150 and
300 °C/hr), a static gain set built from fuzzy's own measured average
multipliers does *not* reproduce fuzzy's behaviour — the inference is doing
something a fixed gain cannot, trading a larger dwell-entry overshoot
(~1.4 °C) for a smaller undershoot (~0.5–0.9 °C) and a slightly worse
steady-state RMS. That is a real, mechanism-traced, reproducible finding
from linked production code, not a citation. Every other un-benchable case
tested — bad starting tune (three variants), high temperature, and changed
thermal mass — either shows no separation from plain PID at all (mass-light,
both high-temperature scenarios) or shows separation that a fixed gain set
reproduces exactly as well (`GAIN_ONLY`, all three tune-mismatch scenarios
and the heavy-mass scenario). None of that supports scheduling hardware
time under the plan's own decision table, which requires inference-only
separation to justify it. Only the sensor-placement result does, and per the
plan's own §0.1 first row, that is the one outcome that would. The honest
overall picture is narrower than "fuzzy shows promise": one of four
un-benchable cases shows a real, inference-attributable effect; the
remaining three, tested at the single points this suite ran, do not, and the
263-cell factorial that would show whether that one effect holds across a
region (rather than one lucky point) or interacts with the others has not
been built. The owner's decision to keep the layer rests on that one
scenario's evidence plus the untested factorial — not on the three other
cases, which currently offer no simulated support.

## Checks

`tools/run_all_checks.ps1` result and `check_doc_hash_citations.ps1` status
recorded in the commit this document ships with.
