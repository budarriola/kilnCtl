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
   taken from the segment's own first sample (this identifies the segment's
   own transient, not an absolute ambient).
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
3. **Cost-curvature (identifiability) check** — the discriminator this gate
   actually rests on. After finding the best-fit `tau*`, re-evaluate the
   objective at `tau* * (1±RTI_PERTURB_FRAC)` and require the SSE to rise by
   at least `RTI_MIN_CURVATURE_FRAC` relative to the minimum. A flat or
   near-flat cost surface in `tau` means the segment does not actually
   distinguish candidate values — exactly the failure mode a two-point
   crossing method cannot detect (it always returns *a* crossing time
   whether or not the data constrains it). This is the check that would
   have caught `ramp_ident.c`'s failure directly, had it been run against
   the closed-form artifact: that artifact's "response" is arithmetic on
   known quantities, not a simulated plant, so it has no cost surface to
   test in the first place — this whole gate only exists because the new
   method actually builds one.
4. **Improvement-over-null gate** — compare the best fit's SSE against the
   SSE of simulating with the CALLER-SUPPLIED current `(tau, L)` (the model
   already in use). Require a relative SSE reduction of at least
   `RTI_MIN_IMPROVEMENT_FRAC`. This is the direct answer to "does this
   segment carry anything the current model doesn't already know" — a
   segment where the current model already predicts the trace this well is
   refused regardless of what number the grid search nominally reports,
   which is the requested negative case: reject rather than return a
   confident wrong answer.

All four must pass, matching `ramp_ident.c`'s "all five must pass"
convention and its fixed-buffer refusal-reason reporting
(`rti_result_t out->refusal_reason`).

## Non-rested bias — quantified, not just flagged

Autotune's own bias (`project_autotune_needs_rested_baseline`) comes from
fitting an equilibrium-relative model against a plant carrying residual
heat: the fitted gain comes out low because part of the observed rise is
"free" (already-stored heat continuing to diffuse) rather than driven by
the commanded duty. This estimator inherits the identical failure mode
whenever `T_amb` (sample 0 of the segment) is not actually at rest: any
non-zero `dT/dt` already present at t=0 that this model doesn't attribute
to `duty[0]` gets folded into the fitted `tau` as spurious "response."

Correction implemented, revised after measurement (see below): fit a short
linear trend over the first `RTI_TREND_SAMPLES` samples and use its
INTERCEPT (a fixed offset, not extrapolated forward) as the free-response
baseline `T_free`, replacing a flat `T_amb`. An earlier version of this
design fed the fitted SLOPE into the dynamics as well
(`T_free(t) = intercept + slope*t`, extrapolated across the whole segment)
— this was implemented, and then measured to be a real bug, not just an
approximation: extrapolating a slope fit over only `RTI_TREND_SAMPLES`
points linearly across an entire multi-thousand-second segment amplifies
any small fitted slope into tens of degrees of spurious drift by the
segment's end (a measured 0.014 C/s slope, fit over 6 points near t=0,
reached +43 C of extrapolated "baseline drift" by t=3000s and corrupted the
whole fit — recovered tau came out at the search's upper bound, 1000-1200s,
regardless of the true value). Reverted to a fixed intercept; the slope is
still computed and still drives the rested-baseline GATE below, it just no
longer feeds the simulated dynamics.

**Gate, and its measured limitation.** `RTI_TREND_RESIDUAL_TOO_LARGE`
rejects a segment whose pre-segment window is not close to linear — the
sign of curvature a still-decaying (non-rested) start produces. This
converts a rested-baseline requirement into a "was the plant on a locally
LINEAR trend, not curving, when the segment starts" requirement. **Measured
limitation, not merely theorized:** this gate detects a FAST, high-curvature
non-rested start (a step within the trend window itself) but does **not**
detect a SLOW one. A segment built with `tau_true=280s` starting 30s after a
0.5-duty step (constant-duty prior actuation, decay far from complete) has
a fitted pre-segment-window residual of only ~0.04 C regardless of whether
the trend window is 4, 6, 10, or 16 samples wide — an exponential decay's
curvature over a window this short relative to `tau` is genuinely,
measurably almost-linear, so no window size threshold can catch it without
also rejecting ordinary linear ramps. That segment is NOT refused by this
gate, and its recovered tau comes out at 658s against the true 280s (+135%,
biased HIGH here — opposite direction from autotune's low-biased gain from
residual heat, because this bias enters through the dynamics' time constant
rather than through steady-state gain). **Practical implication:** the
trend gate is real protection against a ramp immediately following another
zone's step or this zone's own very recent duty change (the fast case), but
is not a complete defense against a slow multi-minute decay tail from an
earlier transient — a caller wiring this up for real use should additionally
require a minimum QUIET period (no significant duty change) immediately
before the segment starts, which this module does not have visibility into
on its own (it only sees the segment it's handed). This is flagged as
required future work for any real integration, not solved here.

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
3. **Negative / flat-cost case**: a segment with realistic sensor
   quantization (0.1 C) but a duty trace whose variation is dominated by
   the controller's own reference-following (near-zero net excitation once
   detrended) is rejected at the curvature gate — confirms the gate fires
   on cost-surface flatness, not merely on duty range.

All synthetic traces use 0.1 C quantization on `actual_c`, per this repo's
documented idealized-input bug class
(`project_idealized_test_input_bug_class`).
