# Ramp-transient identification — design

Owner-approved fix for the mistuned-kiln case: a plant whose PID tune no
longer matches it (mass changed, elements aged, a different kiln entirely)
has no way to re-identify itself short of a full autotune, which needs a
rested baseline at ambient and therefore cannot run during or near a firing.
This document designs re-identification from the transient that occurs
naturally wherever a scheduled ramp's demand changes, without a dedicated
step test.

## Why the previous attempt (`ramp_ident.c`) doesn't count

`ramp_ident.c` (parked, see the banner at the top of that file) implements a
two-point (28.3%/63.2%) reaction-curve fit — the same method
`pid_autotune_fit_fopdt()` uses on a clean, HELD step — applied to a
detrended slice of a ramp segment. Reproduced exactly against the two real
segments it accepted, the fitted `tau` reduces analytically to
`0.524*K*delta_duty/ramp_rate`: a closed-form function of the model gain,
the apparent duty step, and the profile's own commanded ramp rate — values
the caller already knows, containing no term from the actual plant. It would
return the identical number on a plant with any tau whatsoever. Two joint
causes: the response window closed before the true 63.2% crossing could
ever be observed, and the pre-step "baseline" window sat inside the plant's
own dead time, so detrending removed nothing. `ramp_ident.h`'s own
"What would actually work" section names the fix: **a whole-segment
output-error estimator that simulates a candidate model driven by the
actual recorded duty trace and fits parameters to the residual, instead of
extracting two crossing timestamps and doing algebra on them.**

This document is that estimator.

## Design: whole-segment output-error (OE) fit

Given one segment's `(t, target_c, actual_c, duty)` trace (mirroring
`ramp_ident_sample_t`), and a fixed model gain `K` (same convention as
`ramp_ident.c` — this fits `tau`/dead-time only, never re-derives `K`,
matching PID_EXPANSION_PLAN.md 3.3's scope):

1. **Simulate**, don't extract crossings. For a candidate `(tau, L)`,
   integrate the same first-order-plus-dead-time model the rest of the
   codebase already assumes (`dT/dt = (K*duty_delayed(t-L) - (T-T_amb))/tau`,
   forward Euler at the trace's own sample spacing) over the ENTIRE segment,
   driven by the segment's own recorded duty at every sample — not a
   windowed step-and-response slice, not an assumed step shape. `T_amb` is
   the CALLER-SUPPLIED true ambient (`rti_fit()`'s `ambient_c` parameter),
   not derived from the segment at all — see "Ambient reference" below for
   why using the segment's own start temperature here was a real defect,
   found and fixed 2026-09-14.
2. **Fit** `(tau, L)` by least squares: grid search `tau` over a broad
   physically-plausible range with a golden-section refinement pass, `L`
   over a small integer-sample range (dead time this small relative to tau
   does not need continuous search), minimizing sum-of-squared residual
   between the simulated and actual trace.
3. This is immune to the previous artifact **by construction**: nothing in
   step (1) computes a closed-form crossing time from `K`, `delta_duty`, and
   a ramp rate. The residual is between a full numerically-integrated
   trajectory and the actual trace; the fitted `tau` cannot reduce to a
   ramp-rate expression because a ramp rate never appears in the model or
   the objective. This was verified empirically, not just argued: see
   "Simulation validation" below, including a closed-loop case with a
   *deliberately mistuned* controller model.

## Acceptance gate — rejecting a fit with no plant content

A whole-segment OE fit can still return a confident, meaningless number if
the segment does not actually distinguish candidate `tau`s (the standard
closed-loop identifiability problem: a feedback loop can hold `actual_c`
close to `target_c` regardless of the true plant, making many `tau` values
fit almost equally well). Four checks, all required:

1. **Duty excitation floor** — reuse `ramp_ident.c`'s
   `RAMP_IDENT_MIN_DUTY_SPAN`-style check: whole-segment duty range and
   standard deviation must clear a floor (`RTI_MIN_DUTY_STD`). A flat/held
   duty (a pure dwell) carries no information about `tau` at all — a
   constant input to a stable first-order system converges to the same
   steady state independent of `tau`, so the residual surface is flat.
   Rejected before any fit runs.
2. **Minimum duration** — segment must span enough time to plausibly
   contain a real transient (`RTI_MIN_DURATION_S`), same rationale as
   `ramp_ident.c`.
3. **Cost-curvature (identifiability) check** — after finding the best-fit
   `tau*`, re-evaluate the objective at `tau* * (1±RTI_PERTURB_FRAC)` and
   require the SSE to rise by at least `RTI_MIN_CURVATURE_FRAC` relative to
   the minimum. A flat or near-flat cost surface in `tau` means the segment
   does not actually distinguish candidate values. In practice, over a
   whole-segment simulation of thousands of samples this is hard to trip
   from segment *shape* alone — a 2026-09-14 review swept 1200 constructed
   no-information segments (tau, duration, duty amplitude, dead time) and
   found none flat enough. It IS reachable, and reliably, when the
   CALLER-SUPPLIED `k_gain_c_per_duty` is itself badly wrong (e.g.
   understated by 20x): every candidate tau then fits about equally badly,
   because the dominant residual term is the gain mismatch, not a tau
   mismatch, so perturbing tau barely moves the SSE (`test_ramp_transient_
   ident.c`'s "badly understated caller gain" case; curvature fraction
   ~0.0001 against the 0.05 floor). This is the gate's real, tested
   reachability story — treat "does this discriminate tau" and "is my
   assumed gain even approximately right" as the two things it actually
   catches, not solely the latter.
4. **Improvement-over-null gate** — compare the best fit's SSE against the
   SSE of simulating with the CALLER-SUPPLIED current `(tau, L)` (the model
   already in use). Require a relative SSE ratio of at most
   `RTI_MAX_SSE_RATIO_VS_NULL`. This is the direct answer to "does this
   segment carry anything the current model doesn't already know" — a
   segment where the current model already predicts the trace this well is
   refused regardless of what number the grid search nominally reports,
   which is the requested negative case: reject rather than return a
   confident wrong answer.

All four must pass, matching `ramp_ident.c`'s "all five must pass"
convention and its fixed-buffer refusal-reason reporting
(`rti_result_t out->refusal_reason`).

Reachability correction, 2026-09-15: gate 3's "understated by 20x" above is
the *tested* case, not the boundary. An independent sweep of the caller gain
against a true `K` of 200 found `RTI_FLAT_COST` firing continuously from
`k_gain` = 10 all the way up to 170, i.e. for any understatement of roughly
15% or more. The gate is genuinely non-vacuous and not a contrivance â€” but it
is strictly **one-sided**: every *overstated* gain passes all four gates and
returns a badly wrong tau (see "Sensitivity to the two caller-supplied
constants" below).

Two further gates were added 2026-09-15, outside the "four" above because
neither judges the segment â€” both judge the caller and the search:

5. **`RTI_INVALID_AMBIENT`** â€” `ambient_c` must be finite. A NaN or infinite
   ambient makes every `simulate_sse()` return NaN; every "is this candidate
   better" comparison against NaN is false, so the tau search never updates
   its running best and the module used to fall out of the bottom as
   `RTI_NO_IMPROVEMENT`, with a refusal reason blaming the SEGMENT for
   carrying no information when in fact the CALLER handed over a broken
   reference. Only finiteness is checkable here; `ambient_c`'s VALUE is a
   caller contract (next section).
6. **`RTI_TAU_AT_SEARCH_BOUND`** â€” see its own section below. Runs LAST, so
   the more specific diagnoses (notably `RTI_FLAT_COST`) still speak first.

## Ambient reference — the correctness-critical parameter

`rti_fit()` takes an explicit `ambient_c` parameter: the plant's TRUE
ambient, used as the fixed relaxation target `T_free` inside `simulate_sse`
(`dT/dt = (K*duty_delayed - (T - ambient_c))/tau`). **This is not
interchangeable with the segment's own starting temperature, and an earlier
version of this module (`83e04785`) got this wrong** — it relaxed toward
`trend_intercept` (the segment's own near-start temperature, from the
pre-segment trend fit below) instead of true ambient. A 2026-09-14 opus
review (`docs/audits/ramp_transient_ident_review_2026-09-14.md`) found and
root-caused this: writing `T = T_start + x`, a segment starting above true
ambient has true dynamics `dx/dt = (K*(u - u_hold) - x)/tau`, where
`u_hold = (T_start - ambient)/K` is the duty already needed just to HOLD
`T_start`. Relaxing toward `T_start` instead of ambient feeds the model the
FULL duty `u` rather than the excess `u - u_hold`, leaving a constant
phantom heating term of `K*u_hold/tau` that only `tau` can absorb. Measured
effect, real plant `tau_true=280s`, rested/zero-curvature segments so the
decay tail is not a confound:

| segment start above ambient | fitted tau (defect) | fitted tau (fixed) |
|---|---|---|
| 0 C | 278.1 | 279.8 |
| +2.5 C | 367.0 (+31%) | 279.8 |
| +5 C | 455.9 (+63%) | 279.8 |
| +10.2 C | 640.0 (+129%) | 279.8 |
| +20 C | 994.7 (+255%) | 279.8 |
| +35 C | 1200.0 (search ceiling, saturated) | 279.8 |
| +65 C | 1200.0 (saturated) | 280.0 |

*(Offsets corrected 2026-09-15. The version of this table committed with the
fix, and the fix's own commit message, shifted every row one step — reading
the source review's table of absolute segment-start temperatures as if they
were offsets above a 25 C ambient, so `+5 C` was credited with the `+2.5 C`
row's error. Re-measured directly: each offset costs roughly twice what the
shifted table claimed. Both the defect and the fixed columns above are from
a fresh run of the production function at each listed offset, sabotaged
copy vs. current source.)*

The "fixed" column was also extended past the review's +65 C: the estimate
stays at 279.8 s out to +100 C of start-above-ambient, and holds for
segments starting *below* ambient (−1, −2, −5, −10, −20 C: 279.8, 279.8,
276.8, 279.0, 279.9). Past about +125 C the synthetic plant's own duty
saturates at 1.0 and the segment is correctly refused
`RTI_INSUFFICIENT_DUTY_EXCITATION` — a property of a `K=200` plant asked to
hold more than `K` above ambient, not of the estimator.

All four acceptance gates returned `RTI_OK` with an empty refusal reason at
every row on the left — curvature and improvement-over-null both actively
endorsed the wrong answer, because the null model was evaluated against the
same broken baseline. Fixed by threading the caller's true ambient through
as an explicit parameter instead of deriving a reference from the segment;
see `test_ramp_transient_ident.c`'s "Ambient-reference regression" sweep,
which pins this table as a regression. **Practical reach:** only segment 1
of a firing starts at true ambient — every later segment starts hot, so
this was wrong on most real segments, not an edge case.

## Pre-segment trend — GATE input only, not a dynamics input

Independent of the ambient fix above: fit a short linear trend over the
first `RTI_TREND_SAMPLES` samples and use its max residual (how far the
window departs from a straight line) as the rested-baseline GATE's input
(`RTI_TREND_RESIDUAL_TOO_LARGE` below). The trend's INTERCEPT and SLOPE are
NOT fed into the simulated dynamics — an earlier version of this design fed
the intercept in as `T_free` (the ambient-reference defect above) and, even
earlier still, fed the fitted SLOPE in as well
(`T_free(t) = intercept + slope*t`, extrapolated across the whole segment).
That slope-extrapolation attempt was implemented and then measured to be a
real bug: extrapolating a slope fit over only `RTI_TREND_SAMPLES` points
linearly across an entire multi-thousand-second segment amplifies any small
fitted slope into tens of degrees of spurious drift by the segment's end (a
measured 0.014 C/s slope, fit over 6 points near t=0, reached +43 C of
extrapolated "baseline drift" by t=3000s and corrupted the whole fit).
Reverted; the trend's slope is not read anywhere today (`fit_trend()`
accepts a nullable `out_slope` for exactly this reason — `rti_fit()` passes
`NULL`), and the residual is the ONLY thing the trend fit contributes.

**Gate, and its measured limitation.** `RTI_TREND_RESIDUAL_TOO_LARGE`
rejects a segment whose pre-segment window is not close to linear — the
sign of curvature a still-decaying (non-rested) start produces. This
converts a rested-baseline requirement into a "was the plant on a locally
LINEAR trend, not curving, when the segment starts" requirement, and is
non-vacuous: `test_ramp_transient_ident.c` trips it directly with a step
inside the trend window (residual > the 0.6 C floor), reproducing the
2026-09-14 review's independent confirmation (2.971 C). **Measured
limitation, not merely theorized:** this gate detects a FAST, high-curvature
non-rested start (a step within the trend window itself) but does **not**
detect a SLOW one. A segment built with `tau_true=280s` starting 30s after a
0.5-duty step (constant-duty prior actuation, decay far from complete) has
a fitted pre-segment-window residual of only ~0.04 C regardless of whether
the trend window is 4, 6, 10, or 16 samples wide — an exponential decay's
curvature over a window this short relative to `tau` is genuinely,
measurably almost-linear, so no window size threshold can catch it without
also rejecting ordinary linear ramps. That segment is NOT refused by this
gate. **Previously misdiagnosed:** an earlier version of this doc reported
this exact segment's recovered tau as 658s against the true 280s and
attributed the +135% error to this slow-decay gap. The 2026-09-14 review
showed that attribution was wrong — removing the decay tail entirely and
starting a segment at the same ~35C-above-ambient offset reproduces ~640s
on its own, so the decay contributed essentially nothing; the ambient-
reference defect above was the actual cause. With that defect fixed, this
same segment now recovers tau within roughly 25% of true (see
`test_ramp_transient_ident.c`'s "mid-decay start — corrected regression").
**Practical implication, restated honestly:** the trend gate is real
protection against a ramp immediately following another zone's step or
this zone's own very recent duty change (the fast case), and — now that the
ambient reference is fixed — a slow decay tail it cannot see no longer
produces a gross tau error, only a modest one. It is still not a complete
substitute for a caller-side minimum QUIET period (no significant duty
change) immediately before the segment starts, which this module cannot
see on its own (it only sees the segment it's handed); that remains
recommended future work for any real integration, not solved here.

## Sensitivity to the two caller-supplied constants

Added 2026-09-15 after an adversarial re-review of the ambient fix. The fix
above is correct and complete for what it set out to do, but it moves the
failure from a *hardcoded* wrong reference to a *caller-supplied* one, and
that reference is now the single most accuracy-critical input in the module.
Both caller-supplied constants — `ambient_c` and `k_gain_c_per_duty` — enter
the model only through the standing heat balance `K*duty − (T − ambient)`,
so an error in either is absorbed almost entirely by `tau`.

Measured on the same closed-loop traces (true `tau` 280 s, `K` 200, true
ambient 25 C, 0.1 C quantized, 3000 s). Every row marked `RTI_OK` passed all
four gates with an empty refusal reason:

| `ambient_c` error | fitted tau | error | result | curvature (floor 0.05) |
|---|---|---|---|---|
| −5.0 C | — | — | `NO_IMPROVEMENT` | 20.7 |
| −2.5 C | 190.1 | −32% | **`RTI_OK`** | 489 |
| 0 | 279.8 | −0.1% | `RTI_OK` | 2385 |
| +2.5 C | 368.8 | **+32%** | **`RTI_OK`** | 2793 |
| +5.0 C | 456.6 | **+63%** | **`RTI_OK`** | 2730 |
| +10.0 C | 635.5 | **+127%** | **`RTI_OK`** | 2853 |
| +15.0 C | 811.4 | **+190%** | **`RTI_OK`** | 3126 |

| `k_gain_c_per_duty` error | fitted tau | error | result |
|---|---|---|---|
| −25% | — | — | `FLAT_COST` |
| −15% | — | — | `FLAT_COST` |
| −10% | — | — | `NO_IMPROVEMENT` |
| −5% | 186.9 | −33% | **`RTI_OK`** |
| 0 | 279.8 | −0.1% | `RTI_OK` |
| +5% | 365.7 | **+31%** | **`RTI_OK`** |
| +10% | 451.4 | **+61%** | **`RTI_OK`** |
| +20% | 621.2 | **+122%** | **`RTI_OK`** |
| +50% | 1110.6 | +297% | **`RTI_OK`** |
| +60% | 1200.0 | saturated | `TAU_AT_SEARCH_BOUND` (new gate) |

Two conclusions, which are *different* from each other:

**1. A wrong `ambient_c` is not detectable from inside this module, at all.**
`tau` and ambient are very nearly degenerate over a ramp segment: the model
still explains the data. Whole-segment RMS residual rises only 0.029 C →
0.060 C across the full 0 → +15 C ambient sweep, i.e. it stays within about
2x the 0.1 C telemetry quantization floor. No residual threshold, curvature
floor or null-comparison can separate "right ambient" from "+15 C ambient"
without also rejecting good fits — and note the curvature gate reads *higher*
on the wrong rows than on the right one, so it is not merely blind here, it
is anti-correlated. This is therefore a **caller contract**, documented on
`rti_fit()` in the header: a harvester must supply a genuinely measured
ambient (`profile_executor`'s `s_exec.ambient_c`) and must refuse to harvest
when `s_exec.ambient_from_cj` is false, because the `FALLBACK_AMBIENT_C`
substitution is exactly the several-degC error this table prices.

**2. A wrong `k_gain_c_per_duty` IS detectable, and is currently only half
gated.** `RTI_FLAT_COST` genuinely fires for gains understated by ~15% or
more (verified independently — the gate is not vacuous, and its reachability
is much wider than "understated by 20x"), but the entire *overstated* side
passes every gate. Unlike ambient, this one leaves a signal: the absolute
whole-segment RMS residual goes 0.029 C → 0.86 C at only +5% gain error and
1.55 C at +10%, a 30–50x separation from the quantization floor. **A proposed
absolute-residual gate** — refuse when `sqrt(best_sse/(n−1))` exceeds a few
multiples of the telemetry noise floor — would close this hole cleanly. It is
deliberately **not implemented here** because its threshold must be
calibrated against real telemetry noise on a real firing, and a constant
picked to make a synthetic test pass would be a guard justified against a
test constant (`project_bound_relative_to_persisted_state`). This is listed
as required work for anyone wiring the module up, alongside the ambient
contract above.

## Search-bound gate (`RTI_TAU_AT_SEARCH_BOUND`)

Added 2026-09-15. A fit that settles exactly on `RTI_TAU_MIN_S` or
`RTI_TAU_MAX_S` is not a measurement of `tau`; it is the bound, and it says
nothing about how far outside the searched interval the true optimum lies.
Before this gate, such a fit returned `RTI_OK`, `valid=true`,
`tau_s=1200.0` and an *empty* `refusal_reason`. The 2026-09-14 review
recommended this gate and then deliberately withheld it, on the grounds that
adding it while the ambient defect stood would silence that defect's loud
saturated rows while leaving its quiet 31–255%-wrong rows untouched — making
the module look safer without being safer. With the ambient defect fixed that
objection is void, so the gate is now in. It runs **last**, after curvature
and improvement-over-null, so the more specific diagnoses (notably
`RTI_FLAT_COST` for a badly understated gain, whose optimum also lands on a
bound) still speak first. It is exercised by a test driving a 3x-overstated
caller gain to the ceiling, and negative-tested by removing the gate.

## Guard 1 / profiles_stop interaction — deliberately avoided

This module is a **pure post-hoc analysis library**, exactly like
`ramp_ident.c` before it: it takes a finished sample array and returns a
result, with no FreeRTOS/ESP-IDF dependency and no call into
`adaptive_tune.c`, `profile_executor.c`, or any control path. It is not
wired to `adaptive_tune_run_end()` (the function under suspicion in the
open `profile_executor` panic at `profiles_stop()` —
`project_profile_executor_panic_at_stop`) and does not run during a firing
at all in this change. Consequently it cannot collide with Guard 1's stall
window (nothing here commands duty or observes guard state) and cannot
touch the panic-suspect path. Wiring a harvester that calls this at a safe
run boundary is future work, exactly as `ramp_ident.h`'s own "Integration
point" section scoped it — deliberately left undone here pending that
panic's resolution.

## Bench extrapolation warning

The bench fixture is a ~4 W, 120 V unit that cannot exceed roughly 40 C
above ambient (`project_bench_is_a_4w_test_fixture`). No hardware
validation of this estimator against bench captures is claimed or
attempted in this change — validation here is against a synthetic
simulated plant only, with parameters chosen near the bench-identified
values (`docs/audits` / `PID_EXPANSION_PLAN.md`) but this is calibration of
the SIMULATION, not a hardware claim. Any statement about full-size
kiln-temperature behaviour anywhere in this doc or the code comments is
explicitly extrapolation.

## Simulation validation (see `test_ramp_transient_ident.c`)

1. **Positive / mismatch case**: a simulated true plant (`tau_true=280s`,
   `K=200`) is driven by a closed-loop PI+feedforward controller whose
   internal model is DELIBERATELY WRONG (`tau_model=100s`) tracking a
   100 C/hr ramp. Despite feedback holding tracking error to ~0.08 C std
   (i.e. the loop visually "tracks perfectly"), the OE fit recovers
   `tau ≈ 278s` against the true 280s, while the null model (simulating with
   the wrong `tau_model=100`) has ~37,000x the SSE of the best fit — a
   massive, correctly-signed improvement-over-null margin. This is the
   direct rebuttal to the `ramp_ident.c` artifact: a real closed-loop ramp
   with a wrong internal model is exactly the case the old method could
   never distinguish from "any tau at all", and this method resolves it.
2. **Negative / no-information case**: a flat dwell segment (duty constant,
   plant already at steady state) is rejected at the duty-excitation gate
   (duty std/range is exactly 0) before any fit runs.
3. **Negative / current-model-replay case**: a segment simulated to be
   EXACTLY the current model's own prediction (same duty, same `(tau, L)`)
   is rejected via `RTI_NO_IMPROVEMENT`, not `RTI_FLAT_COST` — an earlier
   version of this doc mislabeled this as the flat-cost case; corrected
   2026-09-14 after a review found the claim false (`grep` over the test
   directory found zero references to `RTI_FLAT_COST` at the time).
4. **Negative / flat-cost case, now genuinely covered**: caller-supplied
   `k_gain_c_per_duty` badly understated relative to the true plant gain —
   see "Cost-curvature (identifiability) check" above. `RTI_FLAT_COST` was
   unreachable across a 1200-combination sweep of segment shape/duration/
   amplitude alone; a wrong caller gain reaches it directly.
5. **Positive / ambient-reference regression**: segment start temperature
   swept from true ambient to +65 C above it (ambient held fixed and
   supplied correctly) — fitted tau stays within ~20% of true at every
   offset. Pins the fix for the defect described in "Ambient reference"
   above.
6. **Negative / trend-residual case**: a fast step inside the pre-segment
   trend window trips `RTI_TREND_RESIDUAL_TOO_LARGE` — previously
   documented as reachable but untested; now has a real test.
7. **Regression / command- and plant-invariance**: E1 (true plant tau swept
   80/280/700s, command fixed) confirms the fit still tracks the real
   plant; E2 (true plant fixed, commanded ramp rate swept 40/100/220 C/hr)
   confirms the estimate stays flat rather than becoming a disguised
   function of the commanded ramp rate. Both properties were verified not
   to have regressed from the ambient-reference fix.

All synthetic traces use 0.1 C quantization on `actual_c`, per this repo's
documented idealized-input bug class
(`project_idealized_test_input_bug_class`).
