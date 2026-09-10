# The credibility gate's residual and the hardware matrix deficit are the same defect — and it is not a pure per-row scale, 2026-09-10

Question put to this pass: is the simulation credibility gate's leftover dwell
offset (`docs/audits/sim_credibility_gate_real_cause_2026-09-10.md` sec 5, after
the coupling model-class fix in `d63a5591`) the *same* defect as the hardware
forward-gain deficit measured against the adopted coupling matrix
(`docs/audits/cplval75_coupling_verdict_2026-09-10.md`, commit `d2e570ad`) —
quantitatively, not by eye?

**Answer: yes, to within ~0.04 in per-row gain once `cplval75`'s own documented
enclosure-drift allowance is applied — against a deficit of 0.12–0.31, so the two
observations explain 70–90 % of each other. But the joint evidence also refutes
the "single per-row scalar" shape on zone 0**, and the credibility gate's own
captures independently reproduce the z0 residual **sign flip** that the new 45 °C
plateau (`7c18a11e`) found hours later, from different data.

Nothing in the simulation was changed by this pass. The scalars are **not**
committed anywhere. `git diff` on `sim_plant.c` and `sim_credibility_gate.c` is
empty from this pass; the only artifact is this document.

---

## 1. Method, and what is held out

Two independent estimators of a per-row correction were computed, and then
compared.

**(A) Dynamic replay root-find (the thing that actually moves the bar).** A
*scratchpad-only* copy of `sim_credibility_gate.c` was built that reads a per-row
scale from the environment and applies it to row *i* of `G` — the diagonal
`model_k_dc[i]` (via `sim_plant_from_zone_cfg()`'s `heater_power_w`) and that
row's off-diagonals together. It links the **real production** `sim_plant.c` and
`drivers/control/heater_output.c`, exactly as
`firmware/KilnFW/App/test/build_host_tests.ps1` builds the checked-in gate, and
at scale `1,1,1` it reproduces the production gate's numbers to six decimal
places (see §2). Model constants come from `sim_measured_zone_constants.h`
(`4d42bafe`'s live-board values: `model_k_dc` 39.2459 / 31.9669 / 31.6810,
`model_tau_s` 263.8 / 269.8 / 270.9, `model_dead_time_s` 52.8 / 43.5 / 33.9);
no literal was reintroduced and no stale preset value was read.

The scalars were solved to zero the **calibration** capture's dwell offset only.
The hold-out capture was then scored with those calibration-derived scalars and
never used in the fit. The gate's existing posture — replay the recorded duty,
use recorded temperatures only as an answer key — is untouched.

**(B) Steady-state algebraic residual (dynamics-free).** Independently, each
settled dwell plateau in the gate's two captures was reduced the same way
`cplval75` reduces its plateaus: mean duty `u` and mean temperature over the
final 40 % of the dwell, ambient by each capture's own first-valid-reading
convention, then `G·u` versus the observed rise, with
`G = diag(model_k_dc) + coupling_coeff`. This involves no simulation at all — it
is the *same measurement* `cplval75` made, applied to different hardware captures.

Estimator (B) is the cleaner comparison (it is literally the hardware quantity);
estimator (A) is what the gate's pass bar responds to.

## 2. Reproduction and harness fidelity

Production gate at `HEAD` (`d63a5591`), the two named captures:

| | ramp MAE (bar 3.0) | dwell offset (bar ±1.5) |
|---|---|---|
| CALIBRATION z0/z1/z2 | 1.481 / 1.710 / **3.367** | **−2.104 / −2.752 / −4.816** |
| HOLD-OUT z0/z1/z2 | 1.489 / 1.335 / 2.780 | 1.412 (pass) / **−1.957 / −4.049** |

The scratchpad copy at scale `1,1,1` returns `cal_off −2.104223 / −2.752350 /
−4.816100`, `hold_off −1.412049 / −1.956725 / −4.049363` — identical. It is a
faithful superset of the checked-in gate, not a re-implementation.

**Negative test (production code broken, not a test-local mirror).** The coupling
term in production `firmware/KilnFW/App/test/sim_plant.c` was edited in place to
`power_couple_w += 0.0f * cfg->coupling_w_per_c[i][j] * u_j;`, the harness
rebuilt, and the dwell offsets moved from ≈0.000 / 0.000 / 0.000 (at the
calibration-derived scalars) to **−20.850 / −13.475 / −5.893**, ramp MAE 1.97/1.27/1.04
to 12.32 / 8.66 / 4.78 — RED, confirming the analysis is bound to the real
`sim_plant.c` and not to a copy. The edit was then reversed **textually by hand**
(no `git stash`, `git checkout --`, or `git restore`), `git diff
firmware/KilnFW/App/test/sim_plant.c` is empty, and the harness reproduces the
pre-break numbers exactly.

## 3. The two corrections, side by side

Per-row multiplicative correction `ΔT_observed / (G·u)`:

| source | operating point | z0 | z1 | z2 |
|---|---|---|---|---|
| **(B)** `noise_floor_p7_run1` seg1 (calibration) | ΔT ≈ 32.5 °C | 1.060 | 1.124 | 1.224 |
| **(B)** `noise_floor_p7d_run1` seg1 (hold-out) | ΔT ≈ 32.5 °C | 1.075 | 1.123 | 1.219 |
| `cplval75` 62 °C plateau, as published | ΔT ≈ 33.0 °C | 1.114 | 1.205 | 1.339 |
| `cplval75` 62 °C, with its own §2d drift allowance | ΔT ≈ 32.1 °C | 1.090 | 1.161 | 1.262 |
| published adopted scalars | 62–75 °C pooled | 1.12 | 1.19 | 1.31 |
| **(A)** replay root-find, calibration only | gate-weighted | 1.092 | 1.124 | 1.242 |

Read off the agreement:

- **Between the gate's two independent firings** (estimator B, same plateau):
  0.015 / 0.001 / 0.005. That is the reproducibility floor of this estimator.
- **Gate (B) vs `cplval75` as published**, at a matched ΔT: 0.04–0.12
  (z0 0.047, z1 0.081, z2 0.118). Uniformly the same sign — `cplval75`'s
  scalars are larger on every row.
- **Gate (B) vs `cplval75` drift-corrected**: 0.030 / 0.037 / 0.040 — a
  *row-independent* ≈0.035, which is the signature of a residual common
  ambient-reference error, exactly the one thing `cplval75` §2d left open. (The
  drift-corrected scalars are back-solved from that section's published
  no-drift and with-drift residuals and its scalar table; the arithmetic is
  stated here rather than hidden.)
- **Gate (A) vs the published scalars**: 0.028 / 0.066 / 0.068, and applying the
  published `1.12 / 1.19 / 1.31` verbatim lands the dwell offset at
  +0.642 / +1.450 / +1.350 (calibration) and +1.473 / +2.468 / +2.455
  (hold-out) — a slight *overshoot*, consistent with scalars fitted at a hotter
  operating point.

**Stated tolerance and verdict.** Two corrections derived from entirely separate
data by entirely separate methods agree to 0.030–0.040 in per-row gain
(2.8–3.3 %, ≈1.1 °C at a 32 °C rise) once the hardware side's own documented
drift allowance is applied, and to 0.12 at worst without it. The deficit they are
explaining is 0.12–0.31. They are the same defect.

What was done with the calibration-derived scalars (A), and the result:

| scalars 1.0919 / 1.1244 / 1.2421 (calibration-derived) | ramp MAE (bar 3.0) | dwell offset (bar ±1.5) |
|---|---|---|
| CALIBRATION z0/z1/z2 | 1.974 / 1.274 / 1.035 — all PASS | +0.000 / +0.000 / −0.000 — all PASS (fitted) |
| **HOLD-OUT** z0/z1/z2 | 2.297 / 1.625 / 1.228 — all PASS | **+0.798 / +0.941 / +1.031 — all PASS** |

The hold-out result is the load-bearing one: three scalars fitted on one firing
put a *different* firing's dwell offset inside ±1.5 °C on all three zones, with
ramp MAE also passing on all three, and it was never used in the fit.

### 3.1 What this establishes, and what it does not

**Establishes.** (i) The gate's residual and the hardware deficit are one
quantity, to the stated tolerance — two independent measurements of the same
missing gain. (ii) A three-parameter per-row correction, fitted on one firing,
generalises to another: the deficit is a property of the matrix, not of a
capture. (iii) The `ramp MAE ≤ 3 °C` and `dwell offset ≤ ±1.5 °C` bars are both
*reachable* by this model class.

**Does not establish.** The simulation is **not** validated by this. Applying a
hardware-derived correction to a sim and then reporting the sim passes is
circular on its own, and is not being claimed. The non-circular content here is
narrower and is the whole point: the *number* the sim needs and the *number* the
hardware measured match, having been obtained from disjoint data. The sim remains
ungated until the matrix itself is re-identified on hardware and the gate is
re-run against the corrected matrix with nothing applied on top.

**Also does not establish that the correction is a per-row scale.** See §4.

## 4. The shape is wrong on zone 0 — and the gate saw it first

Eight settled three-zone plateaus now exist across three campaigns. Reduced
identically (`G = diag(live model_k_dc) + on-board coupling_coeff`, each
capture's own ambient convention; `cplval75`'s three rows use its published
residuals to recover ΔT):

| plateau | pred = G·u (z0/z1/z2) | ΔT observed | residual pred−ΔT |
|---|---|---|---|
| `cplval45` 45 °C | 14.445 / 11.597 / 9.219 | 10.539 / 10.637 / 10.798 | **+3.906** / +0.960 / −1.579 |
| `noise_floor_p7` seg0 (45 °C) | 17.729 / 14.692 / 12.976 | 16.796 / 17.084 / 17.031 | **+0.933** / −2.392 / −4.055 |
| `noise_floor_p7d` seg0 (45 °C) | 18.363 / 15.615 / 13.883 | 16.997 / 17.322 / 17.574 | **+1.366** / −1.707 / −3.691 |
| `noise_floor_p7` seg1 (60 °C) | 30.714 / 28.340 / 26.028 | 32.544 / 31.864 / 31.867 | −1.830 / −3.524 / −5.839 |
| `noise_floor_p7d` seg1 (60 °C) | 30.632 / 28.685 / 26.599 | 32.937 / 32.226 / 32.413 | −2.305 / −3.541 / −5.814 |
| `cplval75` 62 °C | 29.566 / 27.354 / 24.720 | 32.926 / 32.964 / 33.090 | −3.360 / −5.610 / −8.370 |
| `cplval75` 70 °C | 36.498 / 34.651 / 31.464 | 40.778 / 40.871 / 40.974 | −4.280 / −6.220 / −9.510 |
| `cplval75` 75 °C | 40.323 / 38.843 / 35.269 | 45.903 / 45.943 / 45.989 | −5.580 / −7.100 / −10.720 |

**The credibility gate's own two captures already contained the z0 sign flip**,
independently of `cplval45` and measured on a different profile on a different
day: z0's residual is **positive** at both captures' 45 °C plateau (+0.93, +1.37)
and **negative** at both captures' 60 °C plateau (−1.83, −2.31). `cplval45` then
found +3.91 at ΔT ≈ 10.5 °C. Ordered by rise, z0's residual is monotone and
crosses zero once:

```
ΔT  10.5  16.8  17.0   32.5   32.5   32.9   40.8   45.9
z0  +3.91 +0.93 +1.37  −1.83  −2.31  −3.36  −4.28  −5.58
```

That is three campaigns agreeing on a single-crossing monotone trend. A pure
per-row scale cannot produce it: a scale error's residual is proportional to the
rise and never changes sign.

### 4.1 Per-row fit over all eight plateaus

`ΔT = s·(G·u) + c`, least squares per row:

| row | pure SCALE (c ≡ 0) | SCALE + OFFSET |
|---|---|---|
| z0 | s = 1.078, rms **2.448 °C**, max 5.025 °C | s = 1.331, c = **−7.65 °C**, rms **0.696 °C**, max 1.210 °C |
| z1 | s = 1.158, rms 1.295 °C, max 2.792 °C | s = 1.246, c = −2.50 °C, rms 0.960 °C, max 1.383 °C |
| z2 | s = 1.280, rms **1.078 °C**, max 1.634 °C | s = 1.311, c = −0.82 °C, rms **1.036 °C**, max 1.654 °C |

Leave-one-campaign-out, to check this is not one capture or one ambient
convention driving it:

| dropped | z0 pure / +offset | z1 pure / +offset | z2 pure / +offset |
|---|---|---|---|
| none (n=8) | 1.077 (2.45) / 1.331, −7.65 (0.70) | 1.158 (1.29) / 1.246, −2.50 (0.96) | 1.280 (1.08) / 1.311, −0.82 (1.04) |
| `cplval45` (n=7) | 1.089 (1.77) / 1.296, −6.46 (0.55) | 1.164 (0.88) / 1.204, −1.19 (0.81) | 1.282 (1.09) / 1.294, −0.33 (1.08) |
| `noise_floor` (n=4) | 1.105 (2.81) / 1.365, −8.71 (0.73) | 1.176 (1.56) / 1.298, −3.88 (0.79) | 1.307 (0.74) / 1.351, −1.30 (0.57) |
| `cplval75` (n=5) | 1.010 (2.30) / 1.315, −7.46 (0.65) | 1.114 (1.09) / 1.191, −1.74 (0.90) | 1.232 (0.60) / 1.205, +0.55 (0.56) |
*(rms °C in parentheses.)*

Three conclusions survive every subset:

1. **z2's deficit is a pure per-row scale.** The second parameter buys nothing
   (rms 1.08 → 1.04), its sign is not even stable across subsets
   (−1.30 … +0.55 °C), and a log-log fit returns an exponent of **1.040** —
   indistinguishable from 1. z2's scale is 1.23–1.31.
2. **z0's deficit is not a scale.** The offset cuts z0's rms by 2.5–3.9× in every
   subset, with a consistently large negative intercept (−6.5 … −8.7 °C), and the
   log-log exponent is **1.366**. Affine (rms 0.70) and power-law (rms 1.15) fits
   are not separable with eight points; either way the shape is superlinear in
   the rise, not proportional to it.
3. **z1 is intermediate** (exponent 1.136, offset −1.2 … −3.9 °C) and is the one
   row where the data does not choose.

### 4.2 One mechanism that would explain all three z0 observations

Three separate findings single out z0: the sign flip; the smallest row scalar
when fitted at high ΔT only; and (§5) the dwell-entry peak mismatch. A single
hypothesis covers them, and it follows from the physical arrangement —
z0 is the **TOP** zone, z2 the bottom.

**Buoyant transport from the lower zones into the top zone grows faster than
linearly in the rise.** The matrix credits z0 with a *linear* function of its
neighbours' duty, and z0's own duty supplies only 17–22 % of its rise
(`cplval75` §2b) — over 80 % is neighbour heat, so z0 is by far the most
exposed row to any nonlinearity in the cross terms. At small ΔT the convective
flow is weak and the linear model **over**-credits z0 (positive residual); by
ΔT ≈ 23 °C (§4.1's crossing) it under-credits it. z2, at the bottom, receives
almost none of this and reads as a clean gain error — which is what the data
says. A transport term with its own lag would also give z0 extra dynamic order
beyond the single FOPDT lag, which is where §5's overshoot mismatch concentrates.

This is a hypothesis with a named discriminator, not a finding: a per-row fit of
`ΔT = s·(G·u) + c` against a **superlinear cross-gain** model needs plateaus at
ΔT below ~10 °C and above ~46 °C to separate, and it predicts the z0 offset
shrinks when the lower zones are stepped *individually* rather than held jointly —
which the planned column-by-column identification measures directly.

## 5. The remaining bars, and whether they are the same question

**Dwell-entry peak (±2 °C) — anti-correlated with the row scalar, and scale-1's
passes are vacuous.** Raising the row gain makes this bar strictly worse:

| row scale | sim dwell-entry peaks, both captures | peak bar |
|---|---|---|
| 1.00 / 1.00 / 1.00 | 0.000 – 4.031 °C (many exactly 0.000) | 10 pass, 2 fail, 6 unevaluable |
| 1.0919 / 1.1244 / 1.2421 | 0.148 – 8.070 °C | 8 fail of 18 |
| 1.12 / 1.19 / 1.31 | 0.250 – 9.086 °C | 12 fail of 18 |

Recorded peaks are 0.28–3.78 °C throughout. The `0.000 °C` entries are the sim
never reaching target at all, so the bar is not being measured there — correcting
the gain is what makes it honestly measurable, and it then fails. So closing the
gain question does not close the peak question; it *exposes* it.

**Are they independent?** At the level of the model, yes, and this matters for
sequencing. Estimator (B) — the eight-plateau analysis in §4 — involves no
dynamics whatever: settled means against a DC matrix. A second-order (two-mass)
plant fitted to the same step data, constrained to the same DC gain, **cannot**
change any number in §4. What it would change is the overshoot, and — to a
smaller degree — the gate's replay-derived estimator (A), because the gate scores
dwell offset from 60 s into a ~8 min dwell and so picks up some transient. That
is visible already: estimator (A) scatters 0.033–0.049 between the two captures
where estimator (B) scatters 0.001–0.016. **So: re-identify the DC matrix first,
on steady-state plateaus, where the dynamics cannot contaminate the answer; fit
the plant order afterwards against the overshoot bar.** Doing it the other way
round risks absorbing a DC error into a dynamic parameter.

**Noise-floor spread bar.** Unaffected in kind by any of this — it fails at every
scaling tried (4 of 6 at scale 1, 5 of 6 at the calibration-derived scalars, 5 of
6 at the published ones). It is a variance question, not a gain question, and
nothing here speaks to it.

## 6. What this means for the joint re-identification capture

`docs/COUPLING_JOINT_IDENTIFICATION_CAPTURE.md` calls for three ~37-minute
column steps plus a 45 °C hold, roughly 6.5–8 h.

**The cheap shortcut is not available, and the planned procedure is the right
one — but its design should change in two specific ways.**

1. **A per-row rescale of the existing matrix would be cheap and is now ruled
   out as sufficient.** It is adequate for z2 (exponent 1.040, offset
   indistinguishable from zero across every subset) and arguably for z1, but on
   z0 it leaves 2.45 °C rms and a 5.0 °C worst case across the measured range —
   it would be a fudge that happens to pass a gate scored near one operating
   point and mispredicts elsewhere. Three settled plateaus could have produced
   the rescale; eight show why not to.
2. **The joint holds are the wrong experiment for z0 specifically, and that is
   exactly what the column steps fix.** z0's rise is >80 % neighbour heat, so in
   a *joint* hold z0's row is nearly unobservable in its own duty — which is why
   `cplval75`'s inverse solve blamed z2 alone, and why z0 is where the shape
   error hid. Stepping one column at a time is what gives each cross-gain
   independent leverage. **Keep the column-by-column procedure. Do not shorten
   it to more joint plateaus.**
3. **Add ΔT leverage at the bottom and top of the range, and measure ambient at
   the thermocouples.** The crossing is at ΔT ≈ 23 °C and the whole affine-vs-
   superlinear ambiguity lives below it. The existing 45 °C hold sits at
   ΔT ≈ 10.5 °C (because ambient had risen to 34.4 °C) which is usefully low —
   that was luck, not design. Pin it: the procedure should specify a *target ΔT*
   per plateau, not a target temperature, and record ambient at the
   thermocouples at the plateau. The row-independent ≈0.035 gap in §3 and
   `cplval75` §2d's unresolved drift are both the same missing measurement, and
   it costs nothing to take.
4. **The 8·τ dwell-length floor stands** on the live `model_tau_s` of
   263.8 / 269.8 / 270.9 s (≈36 min), so the ~37-minute column steps are
   correctly sized. No change there.

One genuinely cheap addition is worth it: **a second joint plateau at a high
ΔT in the same session as the column steps**, to pin each row's scale under the
same ambient as the columns. That is ~45 min on top of 6.5–8 h and removes the
cross-campaign ambient term that currently limits §3's agreement to 0.035.

## 7. Status of the gate

Unchanged and still failing, as it should. No scalar, correction, or fudge was
committed into `sim_plant.c`, `sim_credibility_gate.c`, or
`sim_measured_zone_constants.h`; the scaled harness exists only in a scratchpad.
The row scalars' standing is: **a dated observation about the currently adopted
matrix**, recorded here and in `d2e570ad`/`7c18a11e` — not a correction in force.
The gate closes when the matrix is re-identified and the gate passes with nothing
applied on top.

## 8. Verification

- `tools/run_all_checks.ps1 -ExecutionPolicy Bypass`: **89 passed, 0 skipped, 1
  failed** — `check_all_task_stack_budgets` (`autotune_engine` over budget),
  another session's in-flight work, unrelated to this pass.
- KilnFW host tests (`build_host_tests.ps1`): one build failure,
  `autotune_engine_prestart` (`test_autotune_engine_prestart.c`, modified by
  another session and unrelated). `sim_credibility_gate` reports FAIL
  informationally with the numbers in §2, as expected.
- This pass changed **no** production or test source. `git diff` on
  `firmware/KilnFW/App/test/sim_plant.c` is empty, including after the §2
  negative test was reversed by hand.
- No board was flashed, no firing started, no heat commanded, no board
  configuration written. The 45 °C plateau capture running on the bench was not
  disturbed.
