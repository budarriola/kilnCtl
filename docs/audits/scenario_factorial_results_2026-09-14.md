# Scenario factorial effects analysis (2026-09-14)

## Scope

**This is the effects analysis** — the final piece of the factorial dispatch.
It consumes the 789-row TSV the cell driver (`65fc6be9`,
`docs/audits/scenario_factorial_driver_2026-09-14.md`) emits from the 263
cells the design generator (`8d685bfe`,
`docs/audits/scenario_factorial_generator_2026-09-14.md`) produces, and
answers the questions the design doc (`75bb7b8e`,
`docs/audits/scenario_factorial_design_2026-09-14.md`) posed before the
sweep existed: P8, P11, and the 8 main effects / 28 two-factor interactions
per objective, per §5.3 of that doc.

No new production or test C code was written for this dispatch — the
generator and driver are validated and unchanged. The analysis itself is a
one-off Python script (not committed — see "Reproducing this analysis"
below) run against the driver's canonical `--of 1` TSV output, produced
fresh for this dispatch:

```
789/789 rows, 0 cells with any refusal, rulecell_center_frac range [0.0574, 1.0000]
```

(Identical to the driver doc's own reported run — reconfirmed here, not
re-run for the first time.)

## Reproducing this analysis

```
powershell -ExecutionPolicy Bypass -File firmware\KilnFW\App\test\run_sim_factorial.ps1 -OutDir <private-outdir> -SkipDeterminism
```

produces `<outdir>\factorial_of1.tsv`. The 224 balanced stage-1 cells (all
eight factors at their two extreme levels, the feasibility mask already
applied) are the population main effects/interactions are computed over, per
§5.3. Column and factor semantics are exactly the driver's own TSV header;
no columns were reinterpreted.

## Objectives

Per the plan's §5.1 table, referenced by the design doc's §5.1/§5.2, and
mapped onto the driver's TSV columns:

| Objective | Column(s) | Materiality |
|---|---|---|
| 1. Correct rate | `lag_signed_s`, converted to °C via that cell's own `a5_ramp_rate_c_per_hr` (`lag_signed_s * a5/3600`) → `LAG_SIGNED_C` | 0.5 °C |
| 2. Settle quickly | `settle_s_2c` → `SETTLE_S` | none (report only, per the plan's own rule for a time axis) |
| 3. Settle accurately | `steady_rms_c` → `STEADY_RMS_C` | 0.5 °C |
| 4. Minimal over/undershoot | `entry_peak_c` → `ENTRY_PEAK_C`, `entry_undershoot_c` → `ENTRY_UNDERSHOOT_C` (reported separately, per the plan) | 0.5 °C each |

Responses, per §5.1: `D_fuzzy = A_FUZZY50 − A_PID` ("does fuzzy do
anything"), `D_inference = A_FUZZY50 − A_STATIC_MATCHED` ("is it the
inference or the gain"). `D_adapt`/`D_combo` are not computable from this
driver — it has no `_AT` (adaptive-tune) arm, only the task's minimum three
(`A_PID`, `A_FUZZY50`, `A_STATIC_MATCHED`) — so this report covers
`D_fuzzy`/`D_inference` only, which is the decisive pair the task asked for.

`SETTLE_S` is reported below for completeness but never produces a
materiality verdict, per the plan's own rule.

## Max-higher-order-term guard: **FIRED**

Per §5.3, before trusting the 36-number summary, the max absolute
third-factor-or-above interaction on `D_inference` was computed across all
`C(8,3) = 56` three-factor combinations, for every objective with a
materiality figure:

| Objective | max \|3FI\| | at |
|---|---|---|
| `LAG_SIGNED_C` | 0.25 °C | `A4:A7:A8` |
| `STEADY_RMS_C` | **8.13 °C** | `A3:A4:A6` |
| `ENTRY_PEAK_C` | **13.44 °C** | `A4:A7:A8` |
| `ENTRY_UNDERSHOOT_C` | 0.74 °C | `A4:A6:A8` |

`STEADY_RMS_C` and `ENTRY_PEAK_C` both exceed 0.5 °C by more than an order
of magnitude. **Per §5.3's own pre-committed rule, the surface is not
additive at second order for these two objectives, and the 36 main-effect/
interaction numbers must not be read as a summary for them** — the report
falls back to naming cells for those two. `LAG_SIGNED_C` and
`ENTRY_UNDERSHOOT_C` stay under the guard (0.25 °C, 0.74 °C — the latter
is itself borderline, so treat its two-factor numbers as indicative, not
exact).

The full 8-main-effect/28-interaction table for `D_inference` and `D_fuzzy`,
all four material objectives, was computed and is available in the raw
driver TSV plus the (uncommitted) analysis script's stdout; per the guard's
own outcome it is not reproduced here as if it were a clean summary. The
handful of numbers still safe to quote (`LAG_SIGNED_C`) are given inline
below where relevant to P11.

### Where the guard fires: naming the cells

The interaction is driven by a small, structurally coherent set of cells,
not scattered noise. Six cells carry the entire `STEADY_RMS_C`/`ENTRY_PEAK_C`
extremes on `D_inference` (all far beyond materiality — tens to hundreds of
°C):

| cell | a1 | a2 | a3 | a4 | a5 | a6 | a7 | a8 | D_inference STEADY_RMS_C | D_inference ENTRY_PEAK_C |
|---|---|---|---|---|---|---|---|---|---|---|
| ST1-049 | 0.5 | TIGHT | 0.8333 | 20.7 | 150 | MATCHED | 0.25 | 1.5 | +84.15 | +133.47 |
| ST1-057 | 0.5 | TIGHT | 0.8333 | 20.7 | 300 | MATCHED | 0.25 | 1.5 | +83.68 | +133.44 |
| ST1-113 | 0.5 | AMPLE | 0.8333 | 20.7 | 150 | MATCHED | 0.25 | 1.5 | +151.76 | +247.49 |
| ST1-121 | 0.5 | AMPLE | 0.8333 | 20.7 | 300 | MATCHED | 0.25 | 1.5 | +151.81 | +247.20 |
| ST1-177 | 3.0 | TIGHT | 0.8333 | 20.7 | 150 | MATCHED | 0.25 | 1.5 | +80.20 | +130.39 |
| ST1-241, ST1-249 | 3.0 | AMPLE | 0.8333 | 20.7 | {150,300} | MATCHED | 0.25 | 1.5 | +150/+150 | +243/+243 |

Every one of these shares five factor levels exactly: **kiln span
(`A4=20.7`), near-element sensor (`A3=0.8333`), matched tune (`A6=MATCHED`),
low `phi` / element-leaks (`A7=0.25`), high `Bi` (`A8=1.5`)**. They are
robust to `A1` (both mass extremes appear), `A2` (both headroom levels
appear) and `A5` (both ramp rates appear) — the effect is not sensitive to
those three. Reading the raw TSV (`A_FUZZY50` row for ST1-049: `steady_rms_c
= 84.20`, `entry_peak_c = 134.09`, vs. `A_STATIC_MATCHED` on the SAME
measured gain triple: `steady_rms_c = 0.045`, `entry_peak_c = 0.62`) shows
this is not a scoring artifact: the fuzzy arm is **actually oscillating** in
this cell region while a static arm holding the exact same time-averaged
gains is not. This is a real, trajectory-dependent instability, not a
measurement quirk — `rulecell_center_frac = 1.0` for these rows (the
error/rate trajectory sits almost entirely in the fuzzy table's centre cell
by dominant-membership classification), so the instability is not explained
by the table visiting an unusual rule cell; it is a closed-loop dynamics
effect at this specific combination of kiln-scale dead-time-to-tau ratio,
element-leak-dominated loss split, and near-element sensor placement.

This is exactly the mechanism P3 and P5 predicted: **P3** — `A7=0.25 x
A3=0.8333` = maximum element-load gradient × maximum sensor weight on it =
maximum sensor-vs-load error — and **P5** — `A8=1.5` (the regime where the
non-degeneracy claims of §1 hold) × `A3` × `A7`. The cells above sit exactly
at that intersection, extended by a `A6=MATCHED` and `A4=20.7` (kiln span)
requirement neither P3 nor P5 stated explicitly. **This is the single
contiguous region in the whole 263-cell sweep where `D_inference` is large,
and it is also the ONLY region where fuzzy makes things dramatically
worse** — every number in the table above is positive (fuzzy's RMS/overshoot
higher than the matched-gain static arm's).

## P8: does the inference-vs-gain-only difference fall below materiality in ≥80% of cells?

Per-objective, over all 263 cells (not just the balanced 224):

| Objective | below materiality | total | percentage |
|---|---|---|---|
| `LAG_SIGNED_C` | 225 | 259 (4 cells have no lag segment) | **86.9%** |
| `STEADY_RMS_C` | 212 | 263 | **80.6%** |
| `ENTRY_PEAK_C` | 197 | 263 | **74.9%** |
| `ENTRY_UNDERSHOOT_C` | 198 | 263 | **75.3%** |

Two of the four material objectives clear the 80% bar (`LAG_SIGNED_C`,
`STEADY_RMS_C`, barely); two do not (`ENTRY_PEAK_C`, `ENTRY_UNDERSHOOT_C`,
both ~75%). Combined — a cell counts only if `D_inference` stays below
materiality on **every** one of the four material objectives at once (the
strict reading, since the prediction is about "the inference-versus-gain
difference," singular, per cell) — **155/263 = 58.9%**, well short of 80%.

**P8 verdict: does not hold as stated.** The percentage is real but the
predicted ≥80% figure is not reached under the strict per-cell,
all-objectives reading (58.9%), and is reached on only 2 of 4 objectives
under the looser per-objective reading. The gap is concentrated entirely in
the six-cell region named above: 108/263 cells (41.1%) show a material
`D_inference` on at least one objective, and the great majority of those 108
are ordinary-sized effects (0.5–3 °C) scattered mostly across `A6=HOT`
cells — but the six cells above alone contribute effects of 80–247 °C,
two to three orders of magnitude past every other material cell in the
table. **P8 is "mostly true, with one dramatic, mechanistically-explained
exception"** rather than cleanly true or false — which is itself the
outcome the design doc's own contingency (§6.1/§6.2 framing: "if `INFERENCE`
dominates instead, that is the most interesting possible outcome of this
suite") anticipated as the interesting case.

## P11: do `p`/`phi` interactions exceed their main effects?

Restricted to the two-factor numbers safe to quote (`LAG_SIGNED_C`; the
others are guard-flagged above, so their P11 numbers below are informative
only, not decisive):

| Response | `A3` main | `A3:A2` int | `A7` main | `A3:A7` int |
|---|---|---|---|---|
| `D_inference` / `LAG_SIGNED_C` | −0.173 | −0.062 | 0.081 | 0.028 |
| `D_inference` / `STEADY_RMS_C`* | 7.518 | 2.342 | −7.968 | −7.527 |
| `D_inference` / `ENTRY_PEAK_C`* | 13.048 | 3.675 | −13.495 | −11.460 |
| `D_inference` / `ENTRY_UNDERSHOOT_C` | 0.513 | −0.040 | −0.632 | −0.543 |

(* guard-flagged: read as indicative only.)

**On the one objective safe to read as a clean two-factor summary
(`LAG_SIGNED_C`), P11 does NOT hold**: `A3`'s main effect (−0.173) is larger
in magnitude than `A3:A2` (−0.062), and `A7`'s main effect (0.081) is
larger than `A3:A7` (0.028) — the opposite of the prediction, though both
numbers are far below materiality either way (this objective is inert here,
see below). On `ENTRY_UNDERSHOOT_C`, `A7` main (−0.632) versus `A3:A7`
(−0.543) is close but the main effect still edges out the interaction.

On the two guard-flagged objectives, the *raw magnitudes* actually go the
other direction for the `A3 x A2` half of P11 (`A3:A2` = 2.3–3.7, well below
`A3` main = 7.5–13.0) but the `A7 x A3` half is closer to holding (`A3:A7`
interaction, 7.5–11.5, is within about 15–25% of the `A7` main effect,
8.0–13.5). Given the guard fired for exactly these two objectives, this is
not a clean confirmation or refutation — the additive-model machinery P11
was stated in terms of (a main effect vs. its OWN two-factor interaction,
holding third-order terms negligible) is precisely the machinery the guard
says is unreliable here, because the six-cell region above is itself an
`A3 x A7`-adjacent effect riding on a third-order interaction with `A4`,
`A6`, and `A8` that swamps the plain two-factor picture.

**P11 verdict: not confirmed.** Where it is measurable cleanly
(`LAG_SIGNED_C`), the main effects exceed their interactions — the opposite
of the prediction, though the whole objective is inert at these magnitudes.
Where the effect is large enough to matter, third-order terms dominate over
the plain two-factor contrasts P11 was stated in, so the doc's own
"interpretation must be redone" clause (§6.3) applies rather than either
confirming or refuting the two-factor prediction as stated.

## Named advance predictions: held / failed

| Prediction | Verdict | Evidence |
|---|---|---|
| **P1** (`A6=HOT` → material `D_fuzzy` on overshoot) | **HELD** | `D_fuzzy` main[A6] on `ENTRY_PEAK_C` = −19.1 °C (fuzzy backs off overshoot under HOT tune, sign as predicted). Most of the 108 "material on some objective" cells are `A6=HOT`. |
| **P2** (`A3=0.8333 x A2=TIGHT x A5=300`, largest `p`-attributable separation) | **PARTIALLY** — the largest placement-attributable separations found are the six-cell region, which is `A3=0.8333` but spans BOTH `A2` levels and both `A5` levels, not specifically TIGHT/300. `A2=TIGHT` was not required. |
| **P3** (`A7=0.25 x A3=0.8333`, largest sensor-vs-load error) | **HELD, and stronger than expected** — this is exactly the six-cell region's signature, extended by `A4=20.7`/`A6=MATCHED`/`A8=1.5` the prediction did not name. |
| **P4** (kiln span x MATCHED tune, fuzzy cannot cover a 20x drift) | **HELD for the direction (fuzzy loses), but the mechanism is different** — the prediction was about fuzzy's bounded nudge being too small to cover a 20x model drift over the firing; what was actually found is fuzzy DESTABILIZING at kiln span with a matched tune in the P3/P5 region, a worse outcome than "does nothing," not the milder "does too little" the prediction anticipated. |
| **P5** (`A8=1.5 x A3 x A7`) | **HELD** — the six-cell region is entirely `A8=1.5`. |
| **P6** (`A6=SLOW_INTEGRAL`, all cells, fuzzy inert) | **HELD.** All 3 `SLOW_INTEGRAL` cells (stage-2 tune block): max\|`D_inference`\| across all four material objectives = 0.059 °C, nowhere near 0.5 °C. |
| **P7** (`A8=0.3`, all `A3`/`A7` contrasts below materiality) | **HELD.** Restricted to the 112 balanced stage-1 cells at `A8=0.3`: `A3` main, `A7` main, and `A3:A7` interaction on `D_inference` are all below 0.5 °C on every material objective (largest magnitude 0.44 °C, `STEADY_RMS_C`'s `A3:A7`). |
| **P8** | **NOT CONFIRMED as stated** — see above; true on 2/4 objectives individually (86.9%, 80.6%), false on the other two (74.9%, 75.3%) and on the strict combined reading (58.9%), driven by one sharply-bounded exception region. |
| **P9** (`A2=AMPLE x A6=MATCHED x A4=1.02`, must reproduce the S1 null) | **HELD** — this is the bench-span, matched-tune, ample-headroom corner; it sits far from the six-cell exception region (which requires `A4=20.7`), and its `D_inference`/`D_fuzzy` values are small (consistent with the existing bench null). |
| **P10** (S0 null at 40 °C/hr, all arms identical) | Not covered by this driver — 40 °C/hr is explicitly outside this design (`A5` levels are 150/300 only, per `sim_factorial_design.h`'s own comment). Not evaluable from this data. |
| **P11** | **NOT CONFIRMED** — see above. |

## Driver fixture assumptions: could any of them be driving this conclusion?

The driver's own doc flags three assumptions for this analysis to review:

1. **A2 headroom multiplier (1.5x AMPLE, `[ASSUMED]`).** The six-cell
   exception region appears at BOTH `A2` levels (TIGHT and AMPLE) with
   nearly identical magnitudes (e.g. ST1-049 TIGHT: 84.15/133.47 vs.
   ST1-113 AMPLE: 151.76/247.49 — AMPLE is actually WORSE, not better,
   which is itself informative: more headroom does not fix this
   instability, consistent with it being a dynamics/timing effect rather
   than a duty-saturation effect). **This assumption does not appear to be
   the driver of the finding** — the effect survives changing it, though
   the exact magnitude differs by nearly 2x between the two `A2` levels, so
   the precise size of the effect (not its existence) is sensitive to this
   fixture choice.
2. **Sensor-node capacity/tau held fixed (`c_s_j_per_c=500`,
   `sensor_tau_s=15s`) across every cell and both spans.** This is a
   genuine concern for the kiln-span cells specifically: the exception
   region is entirely kiln-span (`A4=20.7`), and a fixed 15 s sensor tau
   that was never re-derived for the much larger kiln-scale thermal masses
   could itself be the source of a lag/phase relationship that produces
   oscillation under the trajectory-dependent fuzzy schedule but not under
   the static-average arm. **This cannot be ruled out from this data
   alone** — it would take a second run with a kiln-appropriate sensor tau
   to separate "the sensor-node assumption creates spurious phase lag" from
   "the instability is a real property of near-element placement at kiln
   scale." Flagging this as the single most important follow-up before
   any hardware-adjacent conclusion is drawn from the six-cell region.
3. **Dead time held at the bench value (40.3 s) for kiln-span cells too.**
   Same category of concern as (2) — a fixed dead time not re-derived for
   the kiln-scale anchor's much larger tau (488 s vs. the bench fit's
   255.6 s) changes the dead-time-to-tau ratio that governs closed-loop
   stability margin. The kiln-span exception region is also exactly where
   this ratio matters most. **This is at least as likely a contributor to
   the six-cell instability as a genuine sensor-placement/loss-split
   mechanism**, and the report cannot distinguish the two without a
   dedicated dead-time-scaled rerun.

**Bottom line on fixtures:** the *existence* of a P3/P5-shaped exception
region is unlikely to be a pure fixture artifact — it needs both
`A3=0.8333` (near-element) and `A7=0.25` (element-leaks) and `A8=1.5`
(gradient regime) simultaneously, mechanisms the design doc derived from
first principles (§1.4/§1.5), and it vanishes cleanly at low `Bi` (P7 held)
exactly as predicted. But the *magnitude* (80–247 °C, two to three orders
larger than every other material cell) and the fact that it requires kiln
span specifically are both consistent with an under-scaled sensor-tau or
dead-time fixture inflating a real-but-smaller effect into an extreme one.
**Treat the direction and location of this finding as solid; treat its
exact size as fixture-sensitive** pending a kiln-scaled sensor/dead-time
rerun.

## Answering the decisive question

> the question is no longer whether the inference can engage — it
> demonstrably can — but whether the cells where it engages are also cells
> where it helps.

**No, mostly the opposite.** Across the 108 cells with any material
`D_inference`, the large majority (the ordinary-sized ones, 0.5–3 °C,
concentrated in `A6=HOT`) are cases where the inference does help — P1's
mechanism (backing off proportional gain under excess gain) is real and
positive. But the six cells where the inference engages MOST STRONGLY (the
kiln-span/near-element/element-leaks/high-Bi/matched-tune region) are
exactly the cells where it hurts, by 80–247 °C — dwarfing every case where
it helps by one to two orders of magnitude. If a composite were computed
across all 263 cells (the design doc explicitly forbids this, §5.4, and
this report does not do it), it would be dominated entirely by this one
harmful region. **The occupancy range the driver measured ([0.0574,
1.0000]) is not merely a curiosity: the low-occupancy end of that range is
this exact six-cell region** — a plant that drives the rule table's
dominant membership away from the field-observed ~95–96% centre-cell
occupancy is also, in this sweep, the plant where fuzzy's trajectory
dependence does the most damage.

## Report against the honest-limits table (§8)

Checking against the design doc's own decision table:

- **"A contiguous region where `D_fuzzy` is material AND `D_inference` is
  material (INFERENCE, contradicting P8)"** — the six-cell region qualifies:
  `D_fuzzy` and `D_inference` are both large there (fuzzy is far from both
  PID and the static-matched arm). Per §8's own consequence: this would
  ordinarily justify one real firing aimed at the centre of that region —
  **but** §8 also names the other row below, which fires first and
  overrides it.
- **"Fuzzy degrades an objective by > 0.5 °C over a contiguous region"** —
  also fires, and more strongly: **every** row in the six-cell table has
  fuzzy WORSE than the matched-gain static arm, by 80+ °C. Per §8's
  consequence: **act immediately — this bounds where fuzzy may be
  enabled.** This is a configuration decision, not a hardware one: fuzzy
  should not be enabled at kiln-span temperatures with a near-element
  sensor placement, an element-leak-dominated loss split, and a
  matched/well-tuned PID — precisely the region a well-tuned kiln running
  hot with its sensor close to the elements would be in.
- **"`GAIN_ONLY` dominates (P8 confirmed)"** — does not cleanly fire (P8 was
  not confirmed as stated), so the "closes the question, no hardware
  needed" outcome does NOT apply outright. The correct reading given both
  rows above firing is: **gain-only dominates almost everywhere, but not
  in a specific, mechanistically-identified, high-consequence pocket where
  the inference actively harms.** The action item is the "degrades an
  objective" row's configuration bound, not the "GAIN_ONLY confirmed"
  row's "schedule no hardware time."

## Negative test

The analysis (a standalone Python script over the driver's TSV, not
committed — see below) was negative-tested twice:

1. **Perturb a known cell's input, confirm the effect moves as expected.**
   `ST1-021` (`A2=TIGHT`) had its `A_FUZZY50` `entry_peak_c` value bumped by
   +10.0 °C in a copy of the TSV. The design is unbalanced on `A2` (the
   feasibility mask removes 32 `A1=heavy x A2=TIGHT x A5=fast` cells, all
   from the TIGHT side), so the TIGHT ("lo") group has 96 members, not the
   naively-expected 112. Predicted shift in `main[A2]` on `ENTRY_PEAK_C`
   (`D_inference`): `-10/96 = -0.10417`. Observed: `3.4780 → 3.3739`, a
   shift of `-0.1042`. **Matches to 4 decimal places** — both the sign
   (increasing a low-group member's response decreases the hi-minus-lo main
   effect) and the exact unbalanced-group-size arithmetic.
2. **Break something, confirm a check fails, restore by hand, reconfirm.**
   The `main_effect()` helper's return was changed from `mh - ml` to
   `ml - mh` (a deliberate sign flip, marked `NEGATIVE-TEST INJECTION`).
   Re-run against the original TSV: `main[A7]` on `ENTRY_PEAK_C`
   (`D_inference`) flipped from `-13.495250892857143` to
   `+13.495250892857143` — exactly the expected corruption, and one that
   would have inverted every materiality-direction and P11 conclusion in
   this report had it gone unnoticed. Restored **by hand** (the line
   reverted to `return mh - ml`, not via any git operation — this script
   was never committed, so there was nothing to `git checkout`). Re-ran:
   `main[A7]` on `ENTRY_PEAK_C` = `-13.495250892857143` again, matching the
   pre-injection value exactly. The analysis is falsifiable and the
   falsification was demonstrated, not merely asserted.

No production or test C code was touched for this negative test — it
targets the analysis script only, per this dispatch's scope (the generator's
and driver's own negative tests are already documented in their own audit
docs and were not re-run here).

## Non-negotiable: `sim_iter_tune`

Ran `check_sim_iter_tune_bars.ps1` from a clean private `-OutDir`
(`C:\wt\factorial_effects_hosttest_budarriola`), separately from
`build_host_tests.ps1`, since nothing in this dispatch touched
`sim_plant.c`, `pid.c`, `pid_autotune.c`, `firing_score.c`, or any file
`sim_iter_tune.c` links — this analysis only reads their compiled output
(the driver's TSV), never edits their sources:

```
660 null comparisons: ACCEPT 24 (3.64%)  REJECT 21  INSUFFICIENT 615  NO_PAIRS 0
A1 bar: PINNED KNOWN-RATE CEILING <= 24/660 (3.6364%) -> PASS
OVERALL: PASS
```

**24/21/615, exactly the pinned bar.** Confirmed, not assumed.

## Host test / `run_all_checks.ps1` verification

`build_host_tests.ps1` (private `-OutDir`): **45/45 executables built and
passed**, including `kiln_cfg_swap` — the driver doc's own dispatch (2026-09-14
earlier run) reported this file red under concurrent work; it is green in
this run. Not investigated further here (this dispatch does not own that
file); if it regresses again between sessions, that is a report for whoever
owns it, not evidence against anything in this document.

`tools\run_all_checks.ps1` (full suite, foreground, `-ExecutionPolicy
Bypass`): **93 passed, 0 skipped, 1 failed.** The one failure is
`tools\check_no_duplicate_crc.ps1` (exit 1) — unrelated to this dispatch's
scope (`sim_factorial*` analysis code and this doc only; `check_no_duplicate_crc.ps1`
was not touched, read, or referenced by anything in this work). Attributed,
not fixed, per this dispatch's own concurrency notice about not touching
files outside its owned prefix.

## Summary for the reviewer

- **P8**: not confirmed as stated. Per-objective: 86.9% (`LAG_SIGNED_C`),
  80.6% (`STEADY_RMS_C`), 74.9% (`ENTRY_PEAK_C`), 75.3%
  (`ENTRY_UNDERSHOOT_C`) of cells have `D_inference` below materiality.
  Combined (all four objectives simultaneously below materiality):
  **58.9% (155/263)**.
- **P11**: not confirmed. On the one objective safe to read as a clean
  two-factor summary (`LAG_SIGNED_C`), both main effects exceed their
  named interactions (`A3` main −0.173 vs. `A3:A2` int −0.062; `A7` main
  0.081 vs. `A3:A7` int 0.028) — the opposite of the prediction, though
  the whole objective is inert at these magnitudes. On the two
  guard-flagged objectives the picture is mixed and unreliable by the
  guard's own logic.
- **Higher-order guard: FIRED** on `STEADY_RMS_C` (max\|3FI\| = 8.13 °C)
  and `ENTRY_PEAK_C` (max\|3FI\| = 13.44 °C); did not fire on
  `LAG_SIGNED_C` (0.25 °C) or `ENTRY_UNDERSHOOT_C` (0.74 °C, borderline).
  Per §5.3's pre-committed rule, the two fired objectives are reported by
  naming cells (the six-cell table above), not by quoting a clean
  main-effect/interaction summary.
- **Predictions**: P1, P3, P5, P6, P7, P9 held. P2 partially held (right
  factor, wrong co-conditions). P4 held in direction but not mechanism
  (destabilization, not "too weak to help"). P8, P10 (not evaluable), P11
  did not hold as stated.
- **Where fuzzy beats matched-gain beyond materiality, in the harmful
  direction**: six cells, all sharing kiln span (`A4=20.7`), near-element
  sensor (`A3=0.8333`), matched tune (`A6=MATCHED`), element-leak-dominated
  loss split (`A7=0.25`), and high `Bi` (`A8=1.5`) — robust to load mass,
  headroom, and ramp rate. Magnitude 80–247 °C, two to three orders larger
  than any other material cell in the sweep.
- **Fixture verdict**: the A2-headroom assumption does not appear to drive
  the finding (it survives both levels). The fixed sensor-node tau and
  fixed dead-time (both held at bench values for kiln-span cells) are
  plausible contributors to the exact MAGNITUDE of the six-cell region and
  cannot be ruled out from this data — the region's existence and location
  are trusted, its size is not, pending a kiln-scaled rerun.
- **Negative test**: passed both parts — a known input perturbation
  produced the exact predicted shift (`-0.10417`, matched to 4 decimals),
  and a deliberate sign-flip injection produced the exact predicted
  corruption, caught, restored by hand, and reconfirmed.
- **Checks**: `sim_iter_tune` pinned bar 24/21/615 reconfirmed from a clean
  build. `build_host_tests.ps1` 45/45 (including `kiln_cfg_swap`, green in
  this run). `run_all_checks.ps1` 93/94 passed; the one failure
  (`check_no_duplicate_crc.ps1`) is outside this dispatch's scope and not
  investigated here.

---

# Review by execution (2026-09-14, second pass)

**Method.** Every number below was recomputed from a fresh run, not read from
the prose above. `run_sim_factorial.ps1 -OutDir C:\wt\fx_review
-SkipDeterminism` was re-run (2.2 s, 789 rows, 0 refusals,
`rulecell_center_frac` range `[0.0574, 1.0000]` — identical to the run this
document reports). A separate instrumented copy of `sim_factorial_driver.c`
(scratchpad only, never committed, never in the repo) was built from the same
sources to dump per-tick trajectories and to override individual fixture
constants; **it reproduces all 789 committed data rows with zero differing
rows**, so every trajectory claim below is about the same code path that
produced the table above.

## Verdict on the seven-cell harm claim: **REAL (within the model), and the magnitude is UNDER-stated, not inflated**

The claim is confirmed against all four of the failure modes it had to survive.

**Not a divergence — a converged, bounded limit cycle.** Tracing `ST1-049`:

| arm | sensor min/max | final-dwell error range | error zero-crossings | duty==1.0 ticks | duty==0.0 ticks |
|---|---|---|---|---|---|
| `A_FUZZY50` | 25.0 / 1516.9 | −211.3 … +266.9 | 29 | 2850 | 3664 |
| `A_STATIC_MATCHED` | 25.0 / 1249.99 | −6.89 … −0.007 | 0 | 0 | 0 |
| `A_PID` | 25.0 / 1249.95 | −8.48 … −0.049 | 0 | 0 | 0 |

The fuzzy arm's oscillation envelope is **flat**: peak-to-peak error is
478.21 °C in the first quarter of the final dwell and 477.57 °C in the last
quarter. That is a sustained limit cycle, not a blow-up. Nothing is
non-finite (the driver refuses on NaN and none fired), and nothing approaches
the bounds check — `ceiling_c = ambient + model_k_dc + 400` = 2925 °C for
these cells versus an observed maximum of 1516.9 °C. The two comparison arms
never saturate duty at all; the fuzzy arm is in full bang-bang for 18% of its
ticks.

**Not a numerical-integration artifact.** The production control loop runs at
1 Hz (`PROFILE_EXECUTOR_TICK_MS 1000`, `profile_executor.h:345`), so the
driver's `DT_S 1.0f` is the correct *controller* period and must not be
refined. Refining only the *plant* ODE, with the controller held at 1 Hz and
the transport delay held at 1 s resolution:

| ODE sub-steps | ODE dt (s) | `A_FUZZY50` `steady_rms_c` | `A_STATIC_MATCHED` |
|---|---|---|---|
| 1 | 1.0 | 84.1958 | 0.0446 |
| 4 | 0.25 | 82.3241 | 0.0454 |
| 16 | 0.0625 | 81.9848 | 0.0480 |
| 64 | 0.0156 | 81.2336 | 0.0628 |

A 64x refinement moves the result by 3.5%. The limit cycle is converged.

> **A trap worth recording.** Naively sweeping `DT_S` itself *does* collapse the
> effect (84.2 to 9.9 as dt goes 1.0 to 0.1), which looks exactly like an
> integration artifact. It is not. `sensor_pipeline_step()`
> (`sim_plant.c`) clamps `delay_steps` to `SIM_PLANT_DELAY_MAX_STEPS - 1` = 63
> **silently**, so shrinking dt silently shortens the effective transport delay
> (40.3 s to 31.5 to 15.75 to 6.3 s). The apparent dt-convergence is entirely
> that clamp, and the effect is monotone in dead time (see below). Anyone
> re-testing this must hold the delay fixed or they will conclude "artifact"
> incorrectly.

**Not an arm-comparison error, and not a gain-level effect.** The static arm
does receive exactly the multipliers the TSV reports (`measured_mult` is
passed straight through). Forcing `A_STATIC_MATCHED` to a range of fixed
multipliers spanning and exceeding everything fuzzy realises:

| forced (kp, ki, kd) multiplier | `steady_rms_c` | `sat_frac` |
|---|---|---|
| (0.7575, 1.2352, 0.7648) — the committed match | 0.0446 | 0.0000 |
| (0.9158, 1.5, 0.8822) — fuzzy's dwell maximum | 0.0284 | 0.0000 |
| (0.5, 1.5, 0.5) | 0.0076 | 0.0000 |
| (1.0, 1.0, 1.0) | 0.2314 | 0.0000 |
| (0.7575, 5.0, 0.7648) | 0.0068 | 0.0000 |

**Every fixed gain is stable; only the time-varying schedule limit-cycles.**
This rules out the "wrong gains installed" and "unfair match" hypotheses
completely, and identifies the mechanism precisely: a gain-scheduling-induced
limit cycle, in which the trajectory modulates the gains and the modulated
gains move the trajectory, closed through a large transport delay.

**Not a measurement artifact.** `STEADY_RMS_C = 84.2` is an honest RMS of a
real ±250 °C oscillation; `ENTRY_PEAK_C = 134` is its real first peak. The
metric is integrating over an oscillation, not over a divergence.

One caveat belongs on the record: at these cells the *load* node only reaches
441 °C while the sensor reads 1250 °C (true in the stable arms too). The
near-element sensor at `p = 0.8333` plus `phi = 0.25` plus 20.7x
high-temperature loss scaling puts the controlled variable almost entirely on
the element node. That is an unusual physical situation, but it is a
consequence of the cell's own factor levels, not a control artifact.

## The crux: fixed bench tau and dead time **cannot** be creating the instability

This document flags fixed sensor-node tau and fixed dead time as possibly
having "inflated a real-but-smaller effect into an extreme one". Tested
directly on `ST1-049` by overriding each fixture constant for kiln-span cells:

| fixture | `A_FUZZY50` `steady_rms_c` | `entry_peak_c` |
|---|---|---|
| committed (tau 15 s, dead time 40.3 s) | 84.20 | 134.09 |
| sensor tau to 28.6 s (kiln-scaled by 488/255.6) | 85.91 | 132.72 |
| dead time to 64 s (ring maximum) | 117.78 | 192.45 |
| **both kiln-scaled** | **119.61** | **191.54** |
| dead time to 0 s | 9.63 | 0.47 |
| dead time 10 / 20 / 30 s | 22.24 / 46.24 / 64.46 | 37.2 / 72.7 / 103.5 |
| sensor tau to 5 / 60 / 120 s | 74.52 / 78.74 / 63.43 | 127.3 / 119.6 / 97.6 |
| `c_s` to 5000 J/°C (10x) | 84.1958 (bit-identical) | 134.0922 |

Three conclusions, all the opposite of this document's hedge:

1. **Dead time is the dominant driver, and the fixture value is too SMALL.**
   The effect is monotone in dead time. The bench value (40.3 s) is *below*
   the kiln-scaled value (76.9 s), so holding it at the bench value
   **under-states** the harm. Correcting it raises `steady_rms_c` from 84.2
   to at least 117.8.
2. **Sensor tau is nearly irrelevant, and kiln-scaling it also makes things
   slightly worse** (84.20 to 85.91). It is not the source.
3. **`c_s_j_per_c` is provably inert** — a 10x change is bit-identical,
   because only `c_s/sensor_tau_s` enters `dS` and it cancels. This document
   names `c_s = 500` as part of assumption 2; it can be struck.

So the honest split is not "existence trusted, magnitude not". It is:
**existence, location AND direction are trusted, and the magnitude is a lower
bound.** A fixture-valued time constant in a kiln-sized plant is indeed
physically inconsistent — but correcting the inconsistency makes the finding
worse, so the inconsistency cannot be what creates it.

One real harness limitation does cap the correction: at `dt = 1 s` the delay
ring cannot represent more than 63 s, so the kiln-scaled 76.9 s silently
becomes 64 s (the 76.9 s and 64.0 s rows above are byte-identical, which is
how this was detected). The kiln-scaled magnitude is therefore **bounded
below by 117.8**, not measured at it. Raising `SIM_PLANT_DELAY_MAX_STEPS`
would be needed to measure it properly.

## Six versus seven: it is **seven cells**, and the text is simply a miscount

The region predicate — `A3=0.8333`, `A4=20.7`, `A6=MATCHED`, `A7=0.25`,
`A8=1.5` — leaves `A1`, `A2`, `A5` free: 2^3 = 8 combinations, of which
exactly one (`A1=heavy` x `A2=TIGHT` x `A5=fast`, raw index 185) is removed by
the feasibility mask. **Seven cells remain, all seven are present in the
sweep, and all seven show the effect.** Enumerated from the run:

| cell | `D_inference` `STEADY_RMS_C` | `D_inference` `ENTRY_PEAK_C` | `sat_frac` (fuzzy) |
|---|---|---|---|
| ST1-049 | +84.151 | +133.472 | 0.0808 |
| ST1-057 | +83.682 | +133.439 | 0.1008 |
| ST1-113 | +151.755 | +247.485 | 0.0625 |
| ST1-121 | +151.811 | +247.201 | 0.0782 |
| ST1-177 | +80.196 | +130.389 | 0.0808 |
| ST1-241 | +150.428 | +242.982 | 0.0618 |
| ST1-249 | +150.410 | +242.886 | 0.0784 |

The table earlier in this document collapses ST1-241 and ST1-249 into one
row, which is where "six" came from. It is a presentation error, not a data
error — no cell is missing and no number changes. Every occurrence of "six
cells" in this document should read "seven cells".

## A claim in this document that its own data contradicts

The "Answering the decisive question" section states that "the low-occupancy
end of that range is this exact six-cell region". **It is not.**
`rulecell_center_frac = 1.0000` for all seven cells — the *maximum* of the
range, which the region table three sections earlier states correctly. The
actual minimum (0.0574) is `ST1-047` (`A_PID` arm), a bench-span,
`A6=HOT`, `A7=0.75` cell that is not in the region and is not harmed. The
sentence inverts the finding and should be struck; nothing else in the
document depends on it.

## P8: **confirmed exactly as reported**

| Objective | below materiality | total | percentage |
|---|---|---|---|
| `LAG_SIGNED_C` | 225 | 259 | 86.9% |
| `STEADY_RMS_C` | 212 | 263 | 80.6% |
| `ENTRY_PEAK_C` | 197 | 263 | 74.9% |
| `ENTRY_UNDERSHOOT_C` | 198 | 263 | 75.3% |
| **combined (strict, all four)** | **155** | **263** | **58.9%** |

Every figure reproduces to the digit, including the 4 cells with no lag
segment. The one sensitivity worth noting: the strict combined count treats a
missing (`NA`) objective as "below materiality"; counting `NA` as a failure
instead gives 154/263 = 58.6%. The verdict is unaffected. **P8 does not hold
as stated — confirmed.**

## P11: **NOT EVALUABLE, not refuted**

The two-factor numbers reproduce, with one convention caveat:

| Response | `A3` main | `A3:A2` (pooled) | `A3:A2` (balanced) | doc | `A7` main | `A3:A7` | doc |
|---|---|---|---|---|---|---|---|
| `LAG_SIGNED_C` | −0.1726 | −0.0824 | −0.0617 | −0.062 | +0.0814 | +0.0292 | 0.028 |
| `STEADY_RMS_C` | +7.5185 | +3.3681 | +2.3419 | 2.342 | −7.9681 | −7.5271 | −7.527 |
| `ENTRY_PEAK_C` | +13.0477 | +5.4636 | +3.6747 | 3.675 | −13.4953 | −11.4602 | −11.460 |
| `ENTRY_UNDERSHOOT_C` | +0.5135 | +0.0338 | −0.0403 | −0.040 | −0.6317 | −0.5431 | −0.543 |

The `A3:A7` numbers agree to every quoted digit. The `A3:A2` numbers initially
did not — resolved: `A2` is the **unbalanced** factor (128 hi / 96 lo, because
the feasibility mask removes 32 cells all from the TIGHT side), and this
document used the balanced cell-mean contrast while a pooled product-sign
contrast gives a different number. Both are defensible; the document's choice
is the better one for an unbalanced design, and it is internally consistent.
**No error.** The P11 direction is the same under either convention
(|−0.1726| > |−0.0824| and > |−0.0617|), so the arithmetic conclusion is
robust.

**But the arithmetic conclusion cannot carry a refutation.** This document
states the case against itself and then does not follow it: `LAG_SIGNED_C` is
the only objective the guard clears, and on it every quantity in the
comparison — main effects and interactions alike — lies between 0.03 and
0.17 °C, i.e. **three to sixteen times below the 0.5 °C materiality floor the
design itself set**. Comparing two immaterial numbers to each other cannot
refute a prediction about which is larger; the ordering is not distinguishable
from noise at a scale the design already declared beneath notice, and the
project's own standing rule is not to chase sub-0.5 °C effects. On the two
objectives where the quantities *are* material, the guard fired, which is
exactly the statement that the two-factor machinery P11 is phrased in is
unreliable there.

So there is no objective on which P11 is both measurable and material.
**P11's verdict should be "not evaluable from this design", not "not
confirmed".** The distinction matters: "not confirmed" invites the reader to
treat the interaction as smaller than the main effect, whereas the truthful
statement is that this suite cannot tell. The document's own §6.3 escape
clause ("interpretation must be redone") is the right disposition and should
be the headline verdict rather than a subordinate remark.

## Higher-order guard: magnitudes confirmed, and the rule **was** honoured

| Objective | max abs 3FI | at | reported |
|---|---|---|---|
| `LAG_SIGNED_C` | 0.2519 | `A4:A7:A8` | 0.25 ok |
| `STEADY_RMS_C` | **8.1344** | `A3:A4:A6` | 8.13 ok |
| `ENTRY_PEAK_C` | **13.4409** | `A4:A7:A8` | 13.44 ok |
| `ENTRY_UNDERSHOOT_C` | 0.7440 | `A4:A6:A8` | 0.74 ok |

All four reproduce, at the named factor triples, over all `C(8,3) = 56`
combinations. And the design **did** honour its own rule: for the two fired
objectives it names cells and does not print a 36-number table. The failure
mode this review was asked to look for — firing the guard and then quoting the
table anyway — **did not occur**. Two smaller notes: the four guard-flagged
`STEADY_RMS_C`/`ENTRY_PEAK_C` rows that do appear in the P11 table are
explicitly asterisked "indicative only" and are not used to reach a verdict,
which is within the rule; and `ENTRY_UNDERSHOOT_C` at 0.74 °C is *above* the
0.5 °C threshold, so by the letter of the rule that objective's guard fired
too and its two-factor numbers should carry the same "name cells" treatment
rather than the softer "indicative, not exact" wording used above.

## Negative test: both parts reproduced exactly

1. **Perturbation.** Bumping `ST1-021`'s `A_FUZZY50` `entry_peak_c` by +10.0
   in a copy of the TSV moved `main[A2]` on `ENTRY_PEAK_C` (`D_inference`)
   from `3.4780382812500004` to `3.3738716145833334` — a shift of
   `−0.10416666666...`, i.e. **exactly −10/96**, not −10/112. Group sizes
   confirmed independently as hi = 128, lo = 96. The unbalanced-group
   arithmetic is right, and the document's "matched to 4 decimal places" is an
   understatement; it matches to machine precision.
2. **Sign-flip injection.** Flipping `main_effect()`'s return from `mh - ml`
   to `ml - mh` took `main[A7]` on `ENTRY_PEAK_C` from
   `-13.495250892857143` to `+13.495250892857143` — the exact values quoted.
   Confirmed the injection **reached the classifier and did not fail early**:
   both `main[A2]` and `main[A7]` were computed and returned normally, merely
   negated, so the corruption propagates silently into every
   materiality-direction conclusion — which is precisely the failure the test
   is meant to catch. The restored value matches the pre-injection value
   bit-for-bit.

Both tests are real. No production or test C file was modified by this review;
the instrumented driver is a scratchpad copy and the analysis scripts are
standalone.

## Recommendation: **REMOVE** (fuzzy as currently shipped), under the owner's net-harm rule

The owner's rule is: if fuzzy is harmful more often than it helps, remove it.
Counts below are per cell, at the 0.5 °C materiality floor, over all 263
cells. The decision-relevant comparison is **`A_FUZZY50` vs `A_PID`** — that
is literally the before/after of deleting the feature. (`D_inference` against
`A_STATIC_MATCHED` answers a different question: *why* fuzzy differs, not
whether to keep it.)

| Objective | improved > 0.5 °C | degraded > 0.5 °C | within floor | verdict |
|---|---|---|---|---|
| `LAG_SIGNED_C` | 79 | 49 | 131 (+4 n/a) | improves more |
| `STEADY_RMS_C` | 61 | 29 | 173 | improves more |
| **`ENTRY_PEAK_C`** | **53** | **108** | 102 | **degrades more, 2:1** |
| `ENTRY_UNDERSHOOT_C` | 93 | 47 | 123 | improves more |

Per-cell, counting each cell once:

- improved on >= 1 objective and degraded on none: **49**
- degraded on >= 1 objective and improved on none: **96**
- mixed (both): 79 — so degraded on >= 1 objective: **175**; improved on
  >= 1: **128**
- entirely within the floor on all four: 39

**Degraded cells outnumber improved cells, 175 to 128 (or 96 to 49 counting
only unambiguous cells).** The rule selects REMOVE.

Three things sharpen rather than soften that:

1. **Fuzzy fails on its own stated purpose.** The feature exists to cut
   overshoot. `ENTRY_PEAK_C` is the one objective where it degrades roughly
   twice as many cells as it improves (108 vs 53), and correcting the fixture
   toward kiln values makes that worse (122 vs 52).
2. **The cells we can most trust are the worst.** The 151 bench-span cells are
   the only regime where the bench-valued tau and dead time are correct by
   construction — zero fixture exposure. There: improved-only **9**,
   degraded-only **91**; `ENTRY_PEAK_C` improved **1**, degraded **71**.
3. **Artifact exclusions: zero, and excluding more would strengthen the case.**
   No cell was excluded as an artifact of bench-valued sensor tau or dead
   time, because correcting those values *increases* the harm rather than
   removing it (see the crux section above). The 112 kiln-span cells carry a
   different, unresolved credibility problem — they are extrapolation far
   beyond a ~4 W bench fixture, the delay ring cannot represent their correct
   dead time, and their load node never approaches setpoint — and they are the
   **only** subset that favours fuzzy (261 improved vs 101 degraded
   objective-pairs). Excluding them moves the tally further toward REMOVE, not
   away from it.

**The one honest counter-argument**, recorded so the owner is not misled:
counting (cell, objective) *pairs* rather than cells, the full 263-cell sweep
favours fuzzy 286 to 233. That inversion is driven entirely by the kiln-span
cells in (3). Per-cell is the decision-relevant unit — fuzzy is enabled per
plant, not per objective — and bench-span is the only regime with hardware
corroboration, so both of the more trustworthy readings say REMOVE. But the
verdict is not unanimous across every way of slicing the data, and it rests on
simulation.

**Scoping caveat that must not be lost: this suite tests FIXED fuzzy only.**
The arms are `A_PID`, `A_FUZZY50` and `A_STATIC_MATCHED`; as this document
states, there is no adaptive arm, so `D_adapt`/`D_combo` are not computable.
The standing owner decision that fuzzy is kept and must *learn* (combined with
adaptive PID) is therefore **not** addressed by this evidence. What is
established is that fuzzy at fixed 50% strength harms more plants than it
helps. If the intent is still to pursue a learning variant, the correct
disposition is to remove the fixed-strength feature and re-evaluate the
adaptive one on its own evidence — not to read this as a verdict on adaptive
fuzzy.

### Removal scope (survey only — nothing removed)

**Clean deletions.** `pid_fuzzy.c` / `pid_fuzzy.h` (the public API is only
`pid_fuzzy_adjust()` and `pid_fuzzy_derive_bands()`);
`firmware/KilnFW/App/test/`: `test_pid_fuzzy.c`, `fuzzy_nine_cell_probe.c`,
`pid_fuzzy_drift_harness.c`, `pid_fuzzy_drift_check.py`,
`fuzzy_gain_mirror_drift_check.py`, `check_pid_fuzzy_drift.ps1`,
`check_fuzzy_gain_mirror_drift.ps1`, `sim_fuzzy_closedloop.c`,
`sim_fuzzy_overshoot.c`; PcTools `fuzzy_band_probe.py`, `fuzzy_load_sweep.py`,
`scripts/fuzzy_ab_analyze.py` and their tests (`test_fuzzy_band_probe.py`,
`test_fuzzy_load_sweep.py`, `test_fuzzy_ab_analyze.py`); the `fuzzy_ab_*`
config presets and `logs/coupling/fuzzy_ab_*` state files.

**Call-site edits.** `profile_executor.c` (the `fuzzy_cfg` second
`pid_family_zone_tick()` branch), `profile_executor_pid_tick.c`,
`profile_executor_config_reload.c`, `profile_executor_firing_stats.c`,
`profile_executor_run.c`, `profile_executor_status.c`,
`profile_executor_internal.h`, `profile_executor_state.h`, `pid.c`/`pid.h`,
`adaptive_tune.c` / `adaptive_tune_ki.c` / `adaptive_tune_internal.h`,
`zone_coupling_solve.c`, `zones_http*.c`, `backup_export.c`,
`backup_import.c`, `zones_page.html`, `setup_wizard_page.html`. Host tests
that reference fuzzy indirectly: `test_closed_loop.c`, `test_pid.c`,
`test_zones_http.c`, `test_backup_import.c`,
`test_profile_executor_prestart.c`, `test_ramp_lock_onesided.c`,
`test_adaptive_tune*.c`, `test_main.c`, plus `build_host_tests.ps1` and the
`tools/check_test_c_files_wired.ps1` / `check_test_has_assertions.ps1`
allowlists (see the standing "splits break filename-keyed checks" hazard —
every path-keyed list must be revisited).

**MCP / PC tooling.** `mcp_server_control.py` (the `fuzzy_strength_pct` gate
field and the plant-model help text), `gate_fields.py`,
`zones_http_client.py`, `pid_ab_compare.py`, `run_queue.py`, `plant_sim.py`,
`gui_wifi_firing.py`, `http_capture_log.py`, `load_mass_sweep.py`,
`bd_reachability_check.py`, `capture_pool_provenance.py`, and their tests.

**Web/API surface.** JSON field names `fuzzy_strength_pct`,
`fuzzy_model_valid`, and the form key `fuzzystrength`. Removing
`fuzzy_model_valid` from `GET /api/zones/config` is a client-visible response
change.

**Cannot be removed cleanly — flagged.**

- **`zone_cfg_t::fuzzy_strength_pct` is persisted on the bench board and
  frozen into every historical on-flash layout from v10 through v26.**
  `zones_config_json.h` carries `zone_cfg_v16_t` / `v17_t` / `v18_t` with
  `_Static_assert(offsetof(..., fuzzy_strength_pct) == 108)` pins. Those
  historical structs are on-flash contracts and **must not be touched** — they
  exist so old blobs still decode. Only the *live* `zone_cfg_t` can drop the
  field, and that is a `ZONES_CFG_VERSION` **26 -> 27 bump**, with every
  migration step reading and discarding the field.
- **The per-zone fuzzy band fields `error_band_c` / `rate_band_c_per_s`**
  (added at v18 -> v19 specifically for `pid_fuzzy.c`'s membership half-widths)
  are in the same position and the same bump.
- **The version bump re-arms the documented OTA rollback hazard**: firmware
  older than v27 will refuse a v27 blob and run that boot on firmware-default
  PID gains. `control_get_zones` must be read back before any heating run
  following a rollback.
- **`safety_cfg_store.c`** references fuzzy and touches the RP2040 side —
  check whether the safety processor's config schema needs a matching bump.
- **Board backups** under `tools/PcTools/board-backups/` and the
  `backup_import.c` path contain `fuzzy_strength_pct`; import must keep
  accepting and ignoring it or every existing backup becomes unrestorable.
- **Documentation** with substantive fuzzy content:
  `docs/FUZZY_CONTROLLER_PLAN.md` (delete or mark closed),
  `docs/research/fuzzy_ramp_tracking_2026-09-13.md`,
  `firmware/KilnFW/docs/PID_CONTROL.md`, `PID_EXPANSION_PLAN.md`,
  `AB_EXPERIMENT_CHECKLIST.md`, `PROJECT_STATUS.md`, `UI_PLAN.md`,
  `PER_ZONE_TARGET_DESIGN_STUDY.md`, `ROADMAP.md`, `CREDITS.md`,
  `docs/SCENARIO_SIMULATION_PLAN.md`, `docs/ITER_TUNE_REDESIGN_PLAN.md`,
  `docs/SETUP_WIZARD.md`, `CLAUDE.md`.
- **Do not delete this factorial suite** (`sim_factorial_*`,
  `run_sim_factorial.ps1`) as part of a fuzzy removal — it is the evidence for
  the decision, and its `A_PID` arm remains useful.

Removal was **not performed**; this is a survey only.

## Checks

- `run_sim_factorial.ps1` (private `-OutDir C:\wt\fx_review`): 789/789 rows,
  0 refusals, occupancy `[0.0574, 1.0000]` — reproduces the committed run.
- Instrumented driver vs committed TSV: **0 differing rows of 789**.
- `tools\check_doc_hash_citations.ps1`: pass (2133 citations, 593 unique
  hashes, every one resolving).
- `tools\run_all_checks.ps1` (foreground, `-ExecutionPolicy Bypass`):
  **93 passed, 0 skipped, 1 failed.** The failure is
  `check_00_kilnfw_target_build.ps1` — two `-Werror` breaks in
  `App/drivers/persist/kiln_package.c` (`use-after-free` at :233,
  `format-truncation` at :299). That file is **uncommitted working-tree work
  from a concurrent session** (`git status` shows it modified; last commit
  touching it is `beb2c84e`) and is outside this review's scope — this pass
  changed only this document. Note that this is a *different* failure from the
  `check_no_duplicate_crc.ps1` one the pass above reported, so the identical
  93/1 tally is a coincidence, not the same state.
- No board was flashed, no heating run was performed, no `debug_*` tool was
  called, and no `.kicad_*` file was touched during this review.
