# Re-examination of reverted ramp/overshoot control decisions (2026-09-13)

Deep-analysis pass requested by the owner: several ramp- and overshoot-related
control ideas were tried and REVERTED. Were they thrown away for the RIGHT
reasons? This document re-derives what was actually changed and actually
measured for each, asks whether the metric used could even detect the effect
the change targeted, checks each measurement against the instrument defects
found on 2026-09-11..13, and issues a verdict.

**No controller behaviour was changed by this pass.** No code was edited, no
board was flashed, no firing was run. Every commit hash below was verified with
`git cat-file -t`.

## Why this is being re-opened now — the four instrument defects in play

1. **Stale-artifact poisoning.** `8a12521b` root-caused six wrong numbers in a
   committed verdict to a negative test (`ed854ac5`) that left a poisoned
   `sim_fuzzy_closedloop.exe` behind; the following commit measured against it
   and its verdict was refuted. Any sim result whose build provenance is not
   stated is now suspect.
2. **Sampling noise below claimed resolution.** `8a12521b` also showed
   `sim_iter_tune`'s A1 accept counts have sd ≈ 4.8 at 660 trials, so
   18/20/24 are one cluster. Verdicts turning on A1 count deltas under ~10 are
   not measurements.
3. **A unit conversion mistaken for a plant measurement.** `ed854ac5`'s
   0.083 °C/s was a 300 °C/hr profile-rate conversion, contradicted by the same
   module's own measured 0.110 °C/s peak.
4. **The coupling model is REFUTED.** `5844a3e8`/`947709a8`'s matched-ΔT0 test:
   the linear matrix over-predicts z0 by ~33% in a joint dwell (predicted
   17.6–17.8 °C vs actual 13.28 °C), with >90% of the gap attributable to a
   genuine joint-load effect after bounding coefficient noise and the ambient
   term. `b64fe09d` refuted the adopted matrix directly (Criterion A fails 11
   of 12). The residual sign REVERSES between regimes and no model explains it.

## The objective framing, and the test it implies

The owner's objective is four-part. The controller must:

1. reach temperature **at the correct rate**;
2. **settle quickly**;
3. **settle accurately**;
4. minimise **over/undershoot**.

IAE/MAE collapses all four into one number in which they trade **invisibly**. A
change that improved ramp-rate tracking while slightly worsening settled error
can show a flat or negative IAE and be recorded as a failure when it actually
served objective 1. So the test applied to each candidate below is:

- Which of the four objectives did the change actually **target**?
- Which did the measurement actually **score**?
- If those differ, the reject is suspect **on that ground alone**.
- Where a change traded one objective against another, was the trade
  **recognised at the time, or hidden inside a single number**?

The 0.5 °C materiality rule applies **per objective, to the quantity in
question** — not to an aggregate. (For objective 2 the quantity is a time, not a
temperature, and no materiality figure has been set for it; see §8.)

A whole-firing IAE integrates thousands of settled dwell ticks, so a 2 °C peak
lasting ~70 s is a rounding error inside it. **A reject decided on IAE alone,
against a change targeting a transient, is not a reject.**

---

## 1. Dwell-entry climb decay — SOUND REJECT

**What was changed.** `b8b192db` + `a77db88c` decayed the feedforward climb term
over each zone's identified dead time at the ramp→dwell boundary instead of
stepping it to zero. Reverted as `f9d84453`.

**What was measured.** Hardware, profile 7, three zones, two dwell entries per
zone — **dwell-entry overshoot in °C**, i.e. exactly the owner's metric, not IAE:

| °C | z0 | z1 | z2 |
|---|---|---|---|
| before, seg0 | 2.13 | 2.00 | 2.45 |
| with decay, seg0 | 2.31 | 2.57 | 3.48 |
| before, seg1 | 2.41 | 1.97 | 1.95 |
| with decay, seg1 | 2.84 | 2.51 | 2.67 |

Six of six comparisons worse. Effect sizes 0.18–1.03 °C; four of six exceed the
0.5 °C owner floor. n is small (one A/B pair per cell) but the **sign is
unanimous across 6 independent transitions** — under a sign test that alone is
p = 1/64 ≈ 0.016 against a null of no effect, before the magnitudes are counted.

Independently, the second dwell drove an oscillation that tripped thermal guard
2 (temperature falling 1.32 °C/min under heat command) and aborted the firing.
That is a second, categorical failure mode that does not depend on the
overshoot arithmetic at all.

**Could the metric detect the effect?** Yes — this is the one candidate scored
directly on dwell-entry peak.

**Vulnerable to today's defects?** No. Hardware, not sim: no stale binary, no
synthetic input. Profile 7's dwells are 45 °C and 60 °C, inside the coupling
matrix's ≤60 °C validity region, so the refuted matrix does not reach far enough
down to overturn this. The effect sizes are above the noise floor.

**Does the premise still hold?** Yes, and the mechanism was confirmed working —
ramp tracking was unaffected, as designed. What was refuted is the physical
premise: holding MORE climb duty into a dwell adds heat where the plant already
has surplus. Nothing since has disturbed that.

**Four-objective check — does the multi-objective view rescue it?** No; it
makes the reject *stronger*. The change targeted objective 4 and was scored on
objective 4. On objective 1 it was explicitly **neutral**: "ramp tracking was
unaffected, as designed" — the mechanism only acts after the ramp ends, so there
is no rate gain being traded away and hidden. On objective 3 nothing was
claimed either way. And on **objective 2 it clearly LOST**, which the original
write-up records without naming it as such: z2 entered its second dwell at 0.91
duty, was still above 0.5 duty **140 s later**, peaked 2.67 °C high, then
undershot far enough that the recovering duty ramp looked like a stuck heater.
That is a longer, oscillatory settle — objective 2 failing, and objective 4's
undershoot half failing with it.

**Was the guard-2 trip a symptom of the trade, or of the idea being wrong?** Of
the idea being wrong. Guard 2 fires on "heating commanded but temperature
falling"; it was reached via an *oscillation*, i.e. the zone overshooting and
then undershooting hard. An oscillation is not one objective being sacrificed
for another — it is objectives 2, 3 and 4 all degrading together, with objective
1 unchanged. There is no axis on which this change bought anything. The trade
was not hidden; there was no trade.

**Verdict: SOUND REJECT. Leave dead.** The opposite direction was tried and
shipped: `e373c395`'s `zone_taper_climb_rate()` eases the commanded rate off
*before* the boundary and cut dwell-entry overshoot on all six transitions
(e.g. z1 seg0 1.99→0.88, z2 seg0 2.67→1.80).

---

## 2. Coupling lead compensation — UNSOUND (worth re-opening, with a caveat)

**What was changed.** Nothing. This was rejected in design review (2026-09-01),
never implemented. Two reasons were given:

- (a) **The endpoint argument.** At t = 0 the proposed lag design outputs the
  diagonal-alone solve, which is bit-for-bit the uncoupled climb formula
  measured one day earlier as over-driving z0 ~10x (climb duty 0.187 vs the
  coupled 0.018 at 100 °C/hr; max overshoot 5.34 vs 2.16 °C), then blends toward
  the coupled answer over 620–730 s — most of a ramp segment.
- (b) **The stability argument.** The lagged sequential solve is Jacobi
  iteration on `A·u = b`; spectral radius of `−D⁻¹(A−D)` on the measured matrix
  is 0.984, so settling ≈ 11 hours and a 2% off-diagonal error pushes it past 1.

**Could the metric detect the effect?** Partly not. The measured A/B that
anchors (a) (`project_feedforward_climb_uncoupled`) reported **both** IAE and
max overshoot, and the overshoot numbers are large (5.34 → 2.16 °C) — so the
endpoint comparison is not an IAE-only artifact. That part is fine.

**Vulnerable to today's defects? YES — both reasons rest on the refuted
matrix.**

- The 0.984 spectral radius is a property of **that matrix**, the one
  `b64fe09d` and `5844a3e8` refuted. An argument of the form "a 2% coefficient
  error makes this diverge" is not a rejection of the design; it is a statement
  that **the matrix is too poorly known to iterate on** — which is now known to
  be true by a much wider margin (~33% joint-load model error, not 2%). The
  stability objection has, if anything, gotten *stronger*, but it has changed
  character: it no longer says "this design is unstable", it says "no
  matrix-iterating design can be commissioned until the matrix is fixed".
- The endpoint objection (a) is the weaker of the two and it **does not
  survive intact**. It assumes the coupled solve is the correct answer and the
  diagonal solve is the 10x-wrong one. On a matrix that over-predicts
  cross-zone contribution by ~33%, the coupled solve credits neighbour heat
  that does not arrive, i.e. it errs in the *under*-drive direction — the same
  direction as the residual ramp-onset lag this design existed to fix. The
  10x/2.16 °C figures are still a real measurement of the *old* uncoupled
  formula, but they no longer establish that "diagonal-alone at t=0" is a bad
  endpoint on today's model, because today's model's own endpoint is itself
  known-biased by an amount that has never been separated from that comparison.

Note also that the review's own recommended mitigation — a **bounded** blend
`α·u_diag + (1−α)·u_joint` with small α, rather than a wholesale solver swap —
was never tried. The review rejected the unbounded form and named the bounded
form as acceptable; the bounded form was then simply not built. That is a
design gap, not a reject.

**Verdict: UNSOUND as a *reject*, but NOT actionable yet.** The reasons given
were sound against the matrix as understood in September 1's state of
knowledge; they do not survive the matrix's refutation unchanged, and the
bounded variant the review itself endorsed was never evaluated. **However**, the
correct next step is emphatically *not* to build lead compensation now: every
version of it consumes off-diagonal τ/L and gain from a matrix that is ~33%
wrong in a joint dwell and infeasible above ~62 °C. Re-open this item only
*after* a joint-load-valid coupling model exists. Also still unaddressed and
still a real prerequisite: `autotune_engine.c` fits the full cross FOPDT
(K, τ, L) but `coupling_persist_job()` writes only the gain, so τ and L die with
the RAM matrix.

---

## 3. Uncoupled climb term / feedforward climb — NOT A REJECT; the LIVE code is the item at risk

**Established by reading the tree, not the summary.** As of HEAD today,
`profile_executor_feedforward.c` runs `solve_hold_for_zone()` **and**
`solve_climb_for_zone()` — both terms go through the coupling matrix. The
uncoupled per-zone climb formula `rate·τ/K_dc` is the **reverted** side; the
coupled climb is live. `zone_taper_climb_rate()` (the ease-off taper) is live.
`pid.c:170`'s `i_floor = -ff_hold` is live.

**What was measured** (2026-09-01, hardware, profile 7, three zones, one term
changed): normalised IAE 0.0737/0.0580/0.0644 → 0.0348/0.0227/0.0373, and — the
metric that matters — **max overshoot 5.34/4.51/4.79 → 2.16/2.13/2.96 °C**. The
overshoot improvement is 2.6–3.2 °C per zone, five to six times the 0.5 °C
floor. This was judged on the right metric as well as IAE.

**Vulnerable?** The *direction* of this result is not in doubt at any plausible
matrix error: a formula that over-drives z0 tenfold is wrong under any coupling
model, including no model at all. What today's findings do undermine is the
*magnitude* and the assumption that the coupled answer is right. Two live
qualifications:

- `project_ff_hold_infeasible_above_62c`: the coupled **hold** solve is
  infeasible on 100% of ticks above ~62 °C (0.0% ≤56 °C, 9.9% at 64 °C, 100% at
  72 °C, all three zones together), so feedforward contributes nothing valid
  there and the loop runs on P+I+D alone. Every measurement cited above came
  from profile 7 (45 °C and 60 °C dwells) — **entirely inside the valid region.**
- The matrix the climb term solves against over-predicts cross-zone heat by
  ~33% under joint load.

**Verdict: the reverted uncoupled formula is a SOUND REJECT — leave dead.** But
the live replacement is validated only below ~60 °C on a 4 W fixture and rests
on a refuted matrix. The honest statement is that **the ramp feedforward is
currently unvalidated above ~60 °C**, which is a larger open issue than any
item on this list; it is not a reverted-decision question and is tracked
elsewhere.

---

## 4. Integral floor hold-only (`−ff_hold` vs `−(hold+climb)`) — SOUND REJECT of the `−ff_u` floor

**What was changed.** `b7289db4` floored the PID integral at `−ff_u` (total
feedforward); `e690f8a6` changed it to `−ff_hold` (steady-state component only).
The mechanism is not statistical: when the floor binds, `i_term` exactly cancels
`ff_u`, so a ramp runs on P+D alone with feedforward entirely absent, and once
bound during a constant-rate ramp it does not release until the ramp ends.

**Could the metric detect the effect?** This one is scored on IAE — but here
IAE is *appropriate*, because the effect is a sustained whole-segment tracking
offset, not a transient peak. The supporting number is also not IAE-only:
segment-0 ramp on z2 went from −2.43 °C to +0.40 °C **mean error**, i.e. a
2.8 °C bias removed. That is a bias measurement, immune to the averaging
objection.

**Vulnerable to today's defects?** No. The core of the argument is verified in
code rather than measured: `s_exec.target_rate_c_per_s` is zeroed every tick and
written only by the ramp branch, and `zone_coupling_solve_climb()` is
homogeneous in rate, so `ff_climb` is **exactly 0.0** during a dwell — meaning
the dwell floor is byte-identical between the two variants and the measured
dwell gain is preserved by construction. That is an algebraic identity, not a
measurement, so no instrument defect can touch it. The hardware A/B (best of
four builds on every zone) merely corroborates it. The sim in this case was
*pessimistic* (predicted 44–53% ramp recovery, observed essentially full), which
is the safe direction.

**Four-objective check — this is the one case where a trade was recognised and
then ELIMINATED rather than hidden.** `b7289db4` was a genuine objective-3
improvement (it fixed dwell offsets) bought at the cost of objective 1 (it
regressed every ramp) — exactly the trade the aggregate-metric warning is about,
and had it been scored on whole-firing IAE alone it might have read as roughly
neutral. It was not resolved by picking a winner; it was resolved structurally.
Because `ff_climb` is **exactly 0.0** during a dwell, `e690f8a6`'s floor is
byte-identical to `b7289db4`'s in every dwell and shallower only on ramps — so
the objective-3 gain is retained **in full, by construction**, while the
objective-1 regression is removed. That is the right shape for a multi-objective
fix and is worth holding up as the template.

**Verdict: SOUND REJECT of the `−ff_u` floor. Confirmed dead.** Do not
re-propose flooring at the total feedforward. Note the two documented traps
remain live invariants in the code: the Phase-3b disturbance correction must
fold into `hold_total` not `climb` (it does, `profile_executor_feedforward.c`
`hold_total -= term`), and `*out_hold` must be returned **unclamped** (it is —
only the sum is clamped).

---

## 5. Ramp-fit / two-point FOPDT closed-loop fit — SOUND REJECT (strongest on this list)

**What was changed.** `13dcf49f` added `ramp_ident.c/.h`; it was never wired
into a control path. `65f65252` corrected the record.

**Why it is dead is analytic, not empirical.** For a linear rise at rate R the
two-point crossings are `t28 = 0.283·K·Δd/R` and `t63 = 0.632·K·Δd/R`, so

    tau = 1.5·(t63 − t28) = 0.524 · K · Δduty / R

The fitted τ is a function of model gain, apparent duty step and the **commanded
profile ramp rate** only. **It comes out identical on a plant with any τ
whatsoever.** Predicted z1 43 s vs fitted 35.6 s; z2 22 s vs fitted 23.5 s. It
accepted 2 of 27 real ramp segments and fitted τ 22–37 s against a
bench-identified 264–271 s.

**Vulnerable?** No — and note this is the *same class* as today's defect (3):
a quantity that is really a profile-rate conversion being read as a plant
measurement. This item is a prior instance of that class, correctly caught.
Two joint causes were also identified concretely: a 15-sample response window
(153 s) against a true `L+τ` of 300–320 s, and a detrend baseline sitting inside
the plant's own dead time.

**Would a threshold fix it?** No. A two-point reaction-curve fit needs a held
step observed for ≥ 2·(L+τ) ≈ 600 s; this kiln never holds duty that long in
closed loop (the `STEP_NOT_HELD` gate reports exactly this 21 times) and duty
never saturates (0.04–0.45).

**Verdict: SOUND REJECT. Confirmed dead — and dangerous if revived.** Because
`u_ff` is linear in τ, integrating the shelved version would have cut climb
feedforward ~9x and shrunk the ease-off window ~5x. The named alternative — a
whole-segment output-error/ARX estimator fitting τ/L to a FOPDT driven by the
actual duty trace, no step and no hold required, using all 27 segments — is a
different estimator and remains a legitimate open idea. It should not be
confused with reviving this one.

---

## 6. firing_score dwell-entry EMA — SOUND REJECT, and the most important one to have got right

**What was changed.** `8b96b591` smoothed `firing_score.c`'s dwell-entry error
with a 60 s EMA before taking the max, to fix a `sim_iter_tune` A1 false-accept
regression (24/660 → 13/660). Reverted in `22cf674b`; `firing_score.c` confirmed
byte-identical to its pre-`8b96b591` content.

**This is the measurement instrument for the owner's own objective** —
`entry_peak_c` is the dwell-entry overshoot statistic. A bad call here would
distort every overshoot judgement since, so it gets the closest reading.

**What actually decided it — and it is not the A1 count.** Four independently
verified defects in the EMA itself:

1. `FIRING_SCORE_ENTRY_SMOOTH_TAU_S` (60 s) was a bare literal with no link to
   the per-zone, runtime-settable `heater_window_ms`; `firing_score.h`
   deliberately has no `heater_output.h` dependency, so a zone on a different
   PWM window silently decouples the filter from the ripple it exists to remove.
2. Signal attenuation, measured with a synthetic probe against the real
   `firing_score_seg_tick()`: a genuine 0.55 °C degradation measured 0.22 °C on
   an unfitted zone (the fallback `entry_window_s = 60.0f` is exactly one
   smoothing τ) and 0.47 °C on a fitted one.
3. Both of those sit **under** `FIRING_COMPARE_OWNER_FLOOR_C` = 0.5 °C, so a
   real 0.55 °C degradation would have failed to trip the one-sided degradation
   veto. The weakening was in the accept-permissive direction.
4. A one-tick bypass: the first entry-window tick seeded the EMA with, and could
   set `entry_peak_c` from, the raw unsmoothed sample — exactly the PWM-edge
   case the EMA targeted.

**Vulnerable to today's defects?** The A1 counts it was originally justified by
(24/660 vs 13/660) are now known to be noisy at sd ≈ 4.8, so an 11-count delta
is ~2.3 sd — real but far weaker evidence than it read as at the time. **This
cuts against `8b96b591`, not against the revert**: the justification for adding
the EMA has weakened, while the reasons for removing it (1–4 above) are
structural code facts and a synthetic-probe measurement, untouched by sampling
noise. Determinism of the harness itself was checked empirically (five
consecutive runs byte-identical, 24/21/615) and the seed call-order was audited,
so the stale-binary class was excluded here — and `8a12521b` independently
re-ran `check_sim_iter_tune_bars.ps1` and reproduced 24/21/615.

The only real-hardware measurement of this statistic (`49bb1123`, n = 6, median
delta −0.05 °C) sits ~10x inside the 0.5 °C floor and does not corroborate the
problem existing outside the simulator's coupling model — which is now known to
be refuted anyway.

**Verdict: SOUND REJECT. Confirmed dead.** Keeping `entry_peak_c` as a raw peak
is the right choice for an overshoot objective: a filter whose τ is comparable
to the window it filters cannot remove PWM ripple without removing the peak.
The honest 3.64% A1 failure is pinned as a known-failure ceiling rather than
smoothed away; the correct exit is a re-identified coupling matrix, not a filter
on the score. **Caveat to record:** the pin (24/660, exact count) now sits inside
one sd of the harness's own sampling spread as characterised by `8a12521b`. The
harness is deterministic at fixed `mc_runs=220`, so the pin will not flap — but
any *comparison* of A1 counts across model changes (as in candidate 7 below) is
not a measurement, and the pin must not be read as one.

---

## 7. Level-scheduled coupling gain (found by search; not on the owner's list) — UNDECIDABLE, and the adjudication needs a different instrument

**What was changed.** `8cbd9d67` replaced `sim_plant.c`'s constant additive
coupling matrix with a two-segment gain schedule keyed on "joint excess",
calibrated from the cplval75 plateaus. Reverted by `9f054181`. A follow-up flat
1.173x discriminator, `c9ce6b7c`, was recorded as PARTIAL.

**What it was judged on.** Primarily `sim_iter_tune`'s A1 false-accept count:
24/660 → 38/660. `8a12521b` has since shown A1 counts carry sd ≈ 4.8 at 660
trials, making a 14-count delta ~2.9 sd — suggestive, not decisive — and
explicitly called `c9ce6b7c`'s PARTIAL **overstated**, since 18/20/24 are one
cluster and 38 is ~1.8 sd out.

**But the revert does not actually rest on the A1 count**, and this is the part
that survives. `9f054181` established two things by direct measurement and
algebra:

- `sim_iter_tune` drives the real 60 s PWM window and feeds `sim_kiln_step()`
  the **binary** relay state, so "joint excess" can only be 0, 1 or 2. Measured
  over 2,669,994 steps: 90.3% / 9.4% / 0.3%, with **zero** samples anywhere in
  the 0.373–0.639 range the schedule was calibrated on. Every evaluation was
  extrapolation at scale 1.96 or 4.14.
- The schedule is nonlinear in duty and therefore does not commute with PWM
  averaging. At its own low-joint calibration point the intended scale is 0.593
  while the PWM-realised effective scale is 1.421 — **it inverts the sign of its
  own correction in its only consumer.** `test_sim_kiln.c` misses this because
  it feeds fractional duties directly: this repo's idealised-test-input class,
  pointing the opposite way.

Also re-derived there: averaging the three cplval75 plateaus discarded the one
measurement constraining segment B's slope; the in-regime least-squares slope is
0.144 per unit level against the committed 2.180, 15x steeper.

**Verdict: the REVERT is correct on the extrapolation/Jensen grounds, which are
independent of A1 and untouched by the noise finding.** But the *adjudication
framing* — "worsened A1, therefore the fit is wrong for its consumer" — used an
instrument now known to be too coarse for the delta it cited, and `c9ce6b7c`'s
PARTIAL should be downgraded to **inconclusive**. The underlying physical
question (does the coupling gain rise with joint load?) is **UNDECIDABLE on this
evidence** and is exactly the question the refuted matrix leaves open. It should
be settled on hardware, not against A1 counts.

---

---

## 8. The scorecard itself: are `firing_score.c`'s subscores sound instruments for all four objectives? — **NO. Two objectives have no instrument at all.**

Every verdict the project reaches through `firing_score`/`firing_compare`
depends on these three subscores, so a defective or missing one distorts more
than any single reject. Read from `firing_score.c` at HEAD (205 lines, read in
full), not from its documentation.

There are exactly three subscores (`firing_score.h:103-106`):
`FIRING_SUBSCORE_LAG_S`, `FIRING_SUBSCORE_ENTRY_PEAK_C`,
`FIRING_SUBSCORE_STEADY_RMS_C`. Mapped onto the four objectives:

| Objective | Instrument | Status |
|---|---|---|
| 1. correct rate | `LAG_S` (ramp segments only) | Present, two blind spots |
| 2. settle quickly | **none** | **NO INSTRUMENT** |
| 3. settle accurately | `STEADY_RMS_C` | Sound |
| 4a. overshoot | `ENTRY_PEAK_C` | Sound (given the EMA revert) |
| 4b. **undershoot** | **none** | **NO INSTRUMENT** |

### Finding A — objective 2 (settle quickly) is not measured at all

A dwell segment's ticks are partitioned by one fixed boundary: ticks with
`elapsed_s <= entry_window_s` (= `dead_time + 2·tau`, or 60 s if the zone is
unfitted) feed **only** the entry peak; every later tick feeds **only** the
steady RMS. Nothing anywhere records *when* the zone came to rest.

Consequence, stated concretely: two trials that reach the same peak and the same
eventual steady RMS score **identically on all three subscores** even if one
settles in 60 s and the other rings for 400 s — provided the ringing decays
before the entry window ends, or is symmetric enough that its contribution to a
root-mean-square over a long dwell is small. Settling time is precisely the
quantity that distinguishes a well-damped loop from a marginally-stable one, and
the scorecard is blind to it.

This is not hypothetical. Candidate 1 above failed *primarily* on objective 2 —
z2 above 0.5 duty 140 s into a dwell, then an oscillation large enough to trip
guard 2 — and that failure was caught by a **thermal guard and a human reading a
trace**, not by any subscore. Had that change's peak happened to come out flat,
the scorecard would have accepted an oscillating controller.

**Recommended fix (design only, not implemented here):** a fourth subscore,
`FIRING_SUBSCORE_SETTLE_S` — dwell segments only, the elapsed time from segment
start to the last tick at which `|err| > settle_band_c`, with the band a
configured fraction of `cfg.band_c`. It is a streaming scalar (one float, one
comparison per tick), needs no history, and composes with the existing
`firing_score_set_add()` averaging unchanged. It must be `has[]`-gated like the
others, since a segment that never leaves the band has a settle time of 0 and
one that never enters it has none. **Note it cannot simply be bolted on**:
`firing_compare.c`'s Bar-1 floor is a temperature (`FIRING_COMPARE_OWNER_FLOOR_C`
= 0.5 °C) and a time subscore needs its own materiality figure, which the owner
has not set. That is the open question this finding hands back.

### Finding B — objective 4's undershoot half is deliberately discarded

`firing_score.c`'s `seg_finish()`:

```c
out->value[FIRING_SUBSCORE_ENTRY_PEAK_C] = (seg->entry_peak_c > 0.0f) ? seg->entry_peak_c : 0.0f;
```

with the comment "Overshoot only: a dwell entered from below never overshoots,
and reporting a negative 'overshoot' would let an undershooting trial score
better on the overshoot axis for the wrong reason." The tick handler likewise
keeps only the **maximum** `err` (signed, positive = above target) across the
entry window.

The reasoning is correct **for a single-axis score** — mixing signs on one axis
would indeed let undershoot buy credit. But the owner's objective 4 is
over/**under**shoot, and the consequence is that **a dwell entered 3 °C low
scores `entry_peak_c = 0.0`, identical to a perfect entry.** If the zone
recovers into band before `dead_time + 2·tau` elapses, that undershoot leaves no
trace in any subscore at all: it is excluded from `STEADY_RMS_C` by the entry
window, and clamped out of `ENTRY_PEAK_C` by the line above.

This matters directly to the candidates here. Candidate 3's residual is named as
"ramp-onset lag… under-drives by 1–2 °C early in a ramp", and candidate 2 exists
to fix that under-drive. Candidate 1's failure mode included an undershoot. **The
scorecard cannot see the error that two of the seven items on this list are
about.**

**Recommended fix:** a separate `FIRING_SUBSCORE_ENTRY_UNDERSHOOT_C` carrying
`max(0, −min_err)` over the same window — a new axis, not a sign change to the
existing one, which preserves the anti-gaming property the current comment
correctly protects.

### Finding C — `LAG_S` has two blind spots as an objective-1 instrument

Both are defensible design choices for *tuning comparison*, and both make it the
wrong instrument for the owner's objective-1 question as stated.

1. **It is unsigned.** `lag_s = fabsf(err) / rate_c_per_s`, so running 0.5 °C
   ahead of schedule and 0.5 °C behind produce the identical score. For
   "reaches temperature at the correct rate" that is arguably fine (either is an
   error); for diagnosing *why* — and specifically for detecting whether a
   feedforward change moved the loop from under-driving to over-driving — it is
   blind. A change that converted a 1 °C lag into a 1 °C lead would read as
   **no change at all.**
2. **It excludes exactly the worst ticks.** `if (saturated_high && err < 0.0f)
   return;` drops every tick where the zone is at full duty and still short of
   target — the ticks where objective 1 is failing hardest. The rationale ("the
   heater is the limit, not the gains") is right for comparing *gains*, but it
   means `LAG_S` answers "are the gains tracking the ramp?" and **not** "is the
   kiln reaching temperature at the correct rate?", which is the owner's
   question. On this 4 W fixture, which cannot exceed ~40 °C above ambient,
   saturation is not an exotic case.

Also note the firing-wide `zone_captured` latch: ticks before the zone first
comes within `band_c` of target are discarded entirely, so the initial approach
is never scored on any axis. That is correct (it removes start-temperature
dependence) but should be stated when quoting any of these numbers.

### Weight of this finding

**This is more valuable than any individual reject on the list.** Two of the
four stated objectives have no instrument, and both gaps are in the direction
that makes the scorecard *permissive*: an oscillating controller and an
undershooting controller can both score clean. Every `firing_compare` accept
since the scorecard was built has been an accept on objectives 1, 3 and 4a only.
Nothing in this document's candidate verdicts is overturned by it — candidates
1 and 3 were adjudicated on hardware traces rather than subscores, and 4, 5, 6
do not depend on subscores at all — but any **future** A/B judged by
`firing_score` inherits both gaps, including the experiment recommended below.

## Summary table

| # | Item | Objective targeted | Objective scored | Match? | Defect exposure | Verdict |
|---|---|---|---|---|---|---|
| 1 | Dwell-entry climb decay | 4 (overshoot) | 4, plus 2 observed informally | Yes; lost on 4 and 2, neutral on 1 — no hidden trade | None | **SOUND REJECT** |
| 2 | Coupling lead compensation | 1 (ramp-onset rate) | never measured | n/a | Both reasons rest on the refuted matrix | **UNSOUND reject — not actionable until the matrix is fixed** |
| 3 | Uncoupled climb formula | 1 | 1 + 4 (IAE **and** max overshoot), hardware | Yes | Live replacement rests on refuted matrix; unvalidated >60 °C | **SOUND REJECT of the old formula**; live code carries the open risk |
| 4 | Integral floor `−ff_u` | 3 (dwell accuracy) | 1 + 3 (IAE + mean-error bias); core is algebraic | Yes — and the 1-vs-3 trade was recognised and structurally eliminated | None | **SOUND REJECT** |
| 5 | Two-point FOPDT ramp fit | 1 (plant ID for rate) | analytic reduction | n/a | None — an instance of today's unit-conversion class, correctly caught | **SOUND REJECT** |
| 6 | firing_score entry EMA | the objective-4 **instrument** | code facts + synthetic probe (A1 secondary) | Yes | A1 justification weakened — cuts against the EMA | **SOUND REJECT** |
| 7 | Level-scheduled coupling gain | plant-model fidelity | A1 counts + PWM-range/Jensen analysis | No (A1 too coarse) | Sampling noise; idealised test input | Revert correct on other grounds; physics **UNDECIDABLE** |
| 8 | The scorecard itself | — | objectives 1, 3, 4a only | **No — 2 and 4b unmeasured** | Both gaps are accept-permissive | **DEFECTIVE INSTRUMENT** |

## The single most important finding

Not a reject at all: **§8 — two of the owner's four objectives have no
instrument.** Settling time (objective 2) is measured by nothing, and undershoot
(objective 4b) is explicitly clamped to zero. Both gaps are accept-permissive.
Fixing the scorecard is a precondition for trusting any future multi-objective
A/B, including the experiment recommended below.

## The reject most likely to be WRONG

**Candidate 2, coupling lead compensation** — but with a precise qualification.
Its endpoint objection (a) assumed the coupled solve is the correct answer; on a
matrix now known to over-predict cross-zone contribution ~33% in a joint dwell,
that assumption is no longer available. And the review's own endorsed
mitigation, a **bounded** small-α blend, was never built or measured. It is
nonetheless *not* the most valuable thing to retry today, because every variant
of it consumes off-diagonal parameters from the refuted matrix.

## Rejects confirmed dead — do not revisit

Candidates **1**, **4**, **5**, and **6**. Each was rejected on evidence that
either is algebraic (4, 5), is a structural code fact (6), or is a unanimous
hardware sign across six transitions plus an independent guard trip (1). None
depends on the coupling model, a sim binary, or an IAE average.

## The single cheapest experiment worth running

Not on any of the seven. The highest-value open overshoot question is the one
lever that **did** ship and was never validated on the owner's metric:

**Re-size the terminal ease-off window on hardware.** `e373c395`'s
`zone_taper_climb_rate()` tapers over `ease_off_window_mult × dead_time`. The
2.0x value was chosen against **a simulator later found to undershoot
dwell-entry overshoot** — `profile_executor_feedforward.c`'s own comment says
so — and `ease_off_window_mult` was deliberately made a runtime,
no-reflash-needed per-zone knob (`ZONES_CFG_VERSION` 15→16) precisely so it
could be A/B'd on real hardware. Grepping `docs/` finds it at 2.0 everywhere and
**no record of that A/B ever being run.**

- **Experiment:** one profile-7 firing (45 °C / 60 °C dwells — inside the
  coupling matrix's valid region and inside this 4 W fixture's ~40 °C-above-
  ambient ceiling), three zones set to *different* multipliers in the same run
  (e.g. z0 = 1.5, z1 = 2.0 control, z2 = 3.0). Six dwell-entry transitions, no
  reflash, no second firing, ~1 hour.
- **Metric:** dwell-entry peak (`FIRING_SUBSCORE_ENTRY_PEAK_C`, raw peak — which
  is exactly why candidate 6's revert matters). Secondary:
  `FIRING_SUBSCORE_LAG_S`, to confirm a longer taper does not buy overshoot with
  ramp lag.
- **The two objectives the subscores cannot score must be taken from the raw
  capture, by hand, in this run** (per §8): **settle time** (objective 2 — time
  from dwell start to the last tick outside a settle band) and **undershoot**
  (objective 4b — the most-negative error inside the entry window). A longer
  ease-off taper is precisely the change that would be *expected* to trade
  overshoot for undershoot and possibly for settle time, so running it on the
  existing three subscores alone would reproduce the exact blindness §8
  describes. This is also the cheapest way to sanity-check whether a
  `SETTLE_S`/`ENTRY_UNDERSHOOT_C` subscore is worth implementing: if the
  by-hand numbers move materially while the three shipped subscores do not,
  that settles it.
- **What changes the verdict:** a ≥0.5 °C peak reduction at a multiplier other
  than 2.0, with `LAG_S` not worse, says the shipped sizing is wrong and the
  sim's known undershoot bias steered it. All three within 0.5 °C says 2.0x is
  fine and the question closes.
- **Confounds to control:** per-zone comparison across zones is weak on this
  fixture (z2 is the bottom and behaves differently), so the run must be
  repeated with the assignment **rotated** if the first result is marginal — and
  each zone needs a rested baseline, since residual heat biases dwell entry.

The obvious alternative — re-identifying the coupling matrix under joint load —
is the higher-value item overall and is what unblocks candidates 2, 3 and 7, but
it is a multi-hour campaign and out of scope for a "cheapest experiment".
