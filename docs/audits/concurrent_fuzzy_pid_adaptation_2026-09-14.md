# Can the fuzzy layer and the self-improving PID adapt at the same time?

Research and design pass, 2026-09-14. **Doc only -- no production code was
written, no board touched, no firing run.** Scope: single zone throughout
(the multi-zone coupling model is refuted -- see
`docs/audits/cplval75_coupling_verdict_2026-09-10.md` and
`docs/audits/coupling_deficit_sim_hardware_identity_2026-09-10.md`); every
quantity here is reasoned, not measured, because the bench cannot exceed
~40 C above ambient and nothing in this document is validatable on it.

The owner's question: the fuzzy layer is staying, and the COMBINATION of
fuzzy plus adaptive PID is to be tested and shipped -- but `e78fbc5b`
currently makes them mutually exclusive (Ki adaptation is withheld whenever
fuzzy is active). Is concurrent adaptation possible at all, and if so how?

**Short answer: yes, and most of it already works.** The mutual exclusivity
in `e78fbc5b` is not a property of "fuzzy plus adaptation" -- it is a
property of ONE of the two adaptation paths, the one that infers from
closed-loop trace shape. The other path, the `K_dc`/SIMC one, already
composes with fuzzy correctly and is the one that actually moves the gains.

---

## 1. The hypothesis, checked against source

The brief proposed: **plant identification composes safely with any
controller; closed-loop heuristic tuning does not.**

**Verdict: it holds, and the mechanism is sharper than "plant vs loop". It
is a fixed-point argument, and it has two real qualifications (section 1.3,
section 1.4) that the bare statement misses.**

### 1.1 Why the `K_dc` path is frame-independent -- the actual reason

`adaptive_tune_zone_tick()`
(`firmware/KilnFW/App/drivers/control/adaptive_tune.c:290-380`) harvests
**one observation per dwell, only once settled**, and only after three
gates: a temperature-slope floor
(`ADAPTIVE_TUNE_SETTLE_SLOPE_FLOOR_C_PER_S`), a *duty*-stability gate
(`ADAPTIVE_TUNE_DUTY_STABILITY_ABS`/`_FRAC`), and a minimum duty. What it
stores is the pair `(duty, rise_c = actual_c - ambient_c)`.
`adaptive_tune_fit_gain()` (`adaptive_tune.c:151-166`) then does a
through-origin least squares of `rise` on `duty`: `K = sum(u*y)/sum(u*u)`.

The load-bearing fact is not "duty and temperature are physical". It is
that **at a fixed point of a loop containing integral action, the
equilibrium pair is set by the plant and the setpoint, and is independent of
the gains.** Integral action drives error to zero whatever Kp/Ki/Kd are; the
duty that holds a given rise is then `u ~= rise / K_true` by the plant
alone. Multiplying Ki (or Kp, or Kd) by 1.25 changes the *trajectory* to
that fixed point and the *speed* of arrival; it cannot change where the
fixed point is. `pid_fuzzy_adjust()` (`pid_fuzzy.c:291-293`) does exactly
and only a per-tick multiplication of the three gains --
`*out_ki = clamp_gain(ki * (1.0f + scale * ki_dir))` -- so it is precisely
the class of change the fixed point is invariant to.

This same argument already appears, independently derived, in the review
appended to
`docs/audits/adaptive_tune_confidence_authority_design_2026-09-13.md`
(commit `1142c73b`): "integral action drives duty to whatever the *true*
plant requires to hold that rise". That review used it to argue a wrong
*model* cannot relocate the operating point; the identical argument covers a
wrong or rescaled *gain*. Confirmed here by reading, not taken on that
document's word.

Two further confirmations from source:

- The excitation the fit needs comes from **outside** both controllers:
  `ADAPTIVE_TUNE_MIN_DUTY_SPREAD` (checked at `adaptive_tune_model.c:53`)
  requires the ring's observations to span a duty range, and that spread
  comes from the profile's differing dwell setpoints -- an external
  reference signal, not from either adapter. This is the classical
  closed-loop identifiability condition (section 2).
- The safety envelope is anchored independently of both: the plausibility
  ratio test reads `autotune_baseline_k_dc`, which
  `adaptive_tune_refine_zone_locked()` never writes after bootstrap
  (`97288659`, re-armed by `36f88d62`). The lifetime-bound induction proof
  in that function's own comment depends only on the fixed anchor and on the
  blend alpha lying in (0,1) -- **not** on which observations arrived or on
  what the controller did to produce them. So nothing in this document can
  weaken that envelope, and no option below proposes touching it.

### 1.2 Why the Ki path is frame-dependent

`adaptive_tune_diagnose_ki()` (`adaptive_tune_ki.c`) reads
`z->trace_actual_c[]`/`z->trace_duty[]` and classifies **trace shape**:
oscillation amplitude against a noise floor, zero-crossing count,
zero-crossing gap regularity, duty variance and rail proximity, steady
offset. Every one of those is a property of the closed loop -- of the plant
*and* the gains actually applied -- and every one moves when fuzzy rescales
the gains. It then writes a correction relative to the **stored reference**
Ki, which is not the Ki that produced the trace. That is the ratchet
`e78fbc5b` stopped.

Worse for this path than the audit states: the correction magnitude carries
no information at all. All three triggering verdicts assign the same
constant `+/-(ADAPTIVE_TUNE_KI_MAX_FRACTIONAL_MOVE * 100)`, i.e. `+/-20%`
(confirmed here by reading; independently confirmed by execution in
`a3803057` R3). It is a **classifier**, not a measurement. The two
quantities in this function that *do* have physical content --
`ku_estimate` (a describing-function relay estimate,
`4*duty_amp / (pi*amplitude)`) and `tu_estimate_s` -- are computed,
reported, and then never used to size the correction.

### 1.3 Qualification 1: frame-independence is pointwise, not sample-wise

Fuzzy cannot move a harvested `(duty, rise)` point. It **can** change *which
dwells produce a harvested point at all*, because the settle gates (slope
floor, duty-stability, and the run-level
`ADAPTIVE_TUNE_MAX_EXCLUDED_FRACTION` drop in `adaptive_tune_run_end()`)
are themselves closed-loop-shape tests. A gain change that improves settling
admits more observations; one that induces hunting admits fewer, and from
different setpoints.

So the *relation* being fitted is invariant, but the *sample* is selected by
a fuzzy-dependent process. This is the same selection-bias leak `1142c73b`
identified for the confidence design, and it applies here too. **It can bias
`K_dc`; it cannot ratchet it**, because the plausibility anchor is fixed and
the envelope induction is sample-independent (section 1.1). That asymmetry
-- bias yes, unbounded walk no -- is the whole practical difference between
the two paths, and it is why one of them can ship alongside fuzzy and the
other cannot.

### 1.4 Qualification 2: an errors-in-variables bias that predates fuzzy

Worth recording because it sits in the same fit and would be easy to
mistake for a fuzzy interaction later. At a settled dwell with integral
action the *rise* is pinned near `setpoint - ambient` (nearly noise-free),
while the *duty* is the variable that absorbs plant and sensor noise. The
fit regresses `rise` on `duty`, i.e. it puts the noisy variable in the
denominator (`sum(u*u)`). Classical regression dilution then biases the
estimate **low**, by roughly `1 / (1 + var(noise_u)/var(u))`. This is
independent of fuzzy and of `e78fbc5b`; it is the ordinary direct-closed-loop
bias the identification literature warns about (section 2.1). Magnitude is
**not quantified here** -- no data was taken -- and this pass does not
propose fixing it. It is flagged because "frame-independent" means
"invariant to which controller is running", not "unbiased".

### 1.5 Where the hypothesis is only partly right

"Plant identification composes safely with *any* controller" is too strong
as a general principle, and should be stated with its conditions:

- It needs a **fixed point to observe**. A controller that never settles, or
  that holds a limit cycle through the dwell, yields no valid observation --
  which is what the settle gates enforce, and where section 1.3's selection
  effect comes from.
- It needs the plant to be **static-linear over the harvested duty range**,
  because the fit is `rise = K*duty` with no intercept. Under a nonlinear
  plant the mean duty of an oscillating loop is not the duty at the mean
  temperature (a Jensen-inequality effect), so an oscillating controller
  *does* move the apparent operating point. The duty-stability gate is what
  keeps this out, and it is doing more work than its comment claims.
- It needs the **excitation to come from outside the adapters**
  (section 2.1).

All three hold in this codebase today. None is guaranteed by the principle
itself.

---

## 2. What the literature says

Claims below marked *(search-summary)* come from search-result summaries or
abstracts only and were not read in full text; claims marked *(full text)*
were read.

### 2.1 Identifiability under closed-loop adaptation -- the crux

The governing result is the classical one: under output feedback **without
sufficient external excitation the plant estimate is biased toward the
negative inverse of the controller**, and at steady state with no added
excitation identifiability can be lost outright *(search-summary)*. That is
exactly the failure the Ki path exhibits in a different costume -- an
estimate that ends up describing the controller rather than the plant.

The standard survey is P.M.J. Van den Hof and R.J.P. Schrama,
"Identification and Control -- Closed-loop Issues", *Automatica* 31(12),
1995 --
https://people.ee.ethz.ch/~rsmith/idfiles/VandenHof_clpid_Auto_95.pdf
*(full text fetched, but the scanned PDF's OCR is poor; only short fragments
were legible)*. Two legible fragments bear directly on this pass: in closed
loop the noise can "be highly correlated with ('deliberately playing
against') the input signal" (p.5) -- the direct-method bias of section 1.4 --
and for the indirect family the sensitivity "determines the relation between
the external signal r and the input signal u" (p.9), i.e. the external
reference is what carries the information.

The consistency condition for the indirect family is stated in the
literature as requiring the **controller be linear and exactly known**
*(search-summary)*. That condition is why Option D below fails: `pid_fuzzy.c`
is nonlinear (product-inference Mamdani over triangular memberships with
weighted-average defuzzification) and time-varying, so "divide the
controller back out" is not available in its textbook form.

An inversion worth stating because it cuts against intuition: where
persistent excitation is lacking, **a sufficiently high-order or time-varying
controller can itself supply the identifiability condition**
*(search-summary)*. By that argument a fuzzy layer -- precisely a
time-varying gain -- is a mild *help* to identifiability of the underlying
plant, not a hindrance, provided the estimator observes a
controller-invariant quantity. Our settled-dwell fit does. Flagged as
literature-level, not verified for this estimator.

- https://www.researchgate.net/publication/224272585_The_closed-loop_identification_problem_in_indirect_adaptive_control
- https://www.researchgate.net/publication/228976560_Bias_issues_in_closed_loop_identification_with_application_to_adaptive_control
- https://www.sciencedirect.com/science/article/abs/pii/000510989500006I (bias-correction method for indirect identification of closed-loop systems)
- https://perso.uclouvain.be/michel.gevers/PublisMig/ECC07_BGM_final.pdf (Gevers et al., closed-loop identification of MIMO systems: identifiability and experiment design)
- https://onlinelibrary.wiley.com/doi/10.1002/acs.2929 (identification-based PID tuning without external excitation -- shifts the PID parameters themselves to create excitation)

### 2.2 Concurrent / nested adaptation

The literature's framing of two adaptive mechanisms on one loop is
overwhelmingly **hierarchical and timescale-separated**, not
concurrent-and-equal: a fast inner regulator and a slow outer supervisor.
The two-level fuzzy-PID work makes the division explicit -- "the low-level
tuning is dedicated to devise linear gain parameters in the FPID system
whereas the high-level tuning is dedicated to adjust the fuzzy rule base
parameters" *(search-summary)*.

- https://link.springer.com/chapter/10.1007/978-1-4020-6668-9_9 (Two-Level Tuning of Fuzzy PID Controllers for Multivariable Process Systems)
- https://link.springer.com/chapter/10.1007/978-981-15-5546-6_53 (Fuzzy Supervisory Expert Tuner for PID Controller)
- https://www.intechopen.com/chapters/39444 (hybrid fuzzy + fuzzy self-tuning PID for a servo electro-hydraulic system)

Note what the last one does: it **sequences by operating region** -- pure
fuzzy far from setpoint, self-tuning PID near it. That is Option B's shape,
arrived at independently in a different domain.

**The brief's suspicion about the timescale argument is correct.** Timescale
separation is the standard remedy and ours is enormous (per control tick vs
per firing, ~10^4), yet the failure happened anyway. The literature explains
why that is not a contradiction: separation guarantees the slow loop sees a
*quasi-static* fast loop -- it makes the fast loop's action look like a
constant. It does nothing about a slow loop that **misattributes that
constant to the plant**, and a constant bias is exactly what a bang-bang
classifier compounds. The timescale literature is about mixing rates
(https://www.researchgate.net/publication/221531272_Timescale_and_Stability_in_Adaptive_Behaviour,
*search-summary*); our problem is about *observables*, and is untouched by
separation.

### 2.3 Does anyone report our exact failure?

Searched specifically for an adaptation loop inferring from a signal another
controller has already shaped, and ratcheting. **No exact match found.** The
nearest named phenomena, which our failure resembles without being:

- **Parameter drift and bursting under lack of persistency of excitation** --
  B.D.O. Anderson, "Adaptive systems, lack of persistency of excitation and
  bursting phenomena", *Automatica* 21(3), 1985,
  https://www.sciencedirect.com/science/article/abs/pii/0005109885900585
  *(search-summary)*: adaptive parameters drift to large values and then
  produce a sudden error excursion. Ours drifts monotonically to a hard
  bound (5x in 9 runs) rather than bursting, because a cumulative clamp
  catches it first -- but the generating condition (an estimate fed evidence
  that does not identify the thing it updates) is the same family.
- **Estimator / covariance windup** under insufficient or non-uniform
  excitation *(search-summary)*, same source set.

So: our instance appears to be an unnamed special case of a known family,
not a novel phenomenon. Recording it that way is more honest than claiming
either novelty or a citation that does not exist.

### 2.4 The alternation precedent

Alternating identification and control design is a named, mature scheme --
the "windsurfer" approach: identify, redesign, *then* widen the closed-loop
bandwidth by a modest step, repeat, with validation as the stopping test
*(search-summary)*.

- https://link.springer.com/chapter/10.1007/978-1-4471-0205-2_7
- https://www.sciencedirect.com/science/article/abs/pii/000510989500092B
- https://onlinelibrary.wiley.com/doi/10.1002/acs.823

Its lesson for us is the **modest step**, not the alternation: each
iteration must not move the loop far enough to invalidate the model it was
identified against. `ADAPTIVE_TUNE_MAX_FRACTIONAL_MOVE` and the blend alpha
already implement that discipline for `K_dc`.

---

## 3. Criteria (stated before the verdict)

1. **Inference validity** -- does each mechanism observe a quantity whose
   meaning is invariant to the other's action? (Section 1's test.)
2. **Provable without the bench** -- the bench cannot exceed ~40 C above
   ambient and bench values never ship, so an option must be establishable
   by algebra plus host tests, never "we will see on hardware".
3. **Safety-envelope preservation** -- must not weaken the
   `autotune_baseline_k_dc` anchor (`97288659`, `36f88d62`); that anchor is
   deliberately tied to the original autotune and stays there.
4. **Cross-module coupling** -- how badly does it break if `pid_fuzzy.c`'s
   definition later changes? (This repo's named "reset one side of a pair"
   class: two pieces of state joined by a contract nothing expresses.)
5. **Convergence cost** -- firings are hours. An option that doubles
   firings-to-converge is expensive in a way an option that adds code is
   not.
6. **Persisted-state and surface cost** -- new NVS fields, new per-tick
   logging, new schema versions (and so new `ota_rollback_esp()` hazard
   surface, per CLAUDE.md).

---

## 4. Options

### Option A (brief's #2) -- identify the plant, not the loop. RANK 1, recommended

Make the `K_dc`/SIMC path the **sole writer of gains**, and demote
`adaptive_tune_ki`'s correction to a reported diagnostic that never writes
-- i.e. generalize `e78fbc5b`'s fuzzy-conditional withholding to
unconditional withholding, keeping the verdict, `ku_estimate`,
`tu_estimate_s` and the refusal string on `GET /api/adaptive_tune` exactly as
they are today.

Fuzzy and adaptation then run **fully concurrently, with no interlock, no
freeze window and no alternation**, because the only surviving writer
observes a quantity fuzzy provably cannot move (section 1.1).

- C1: best available. The one writer is frame-independent pointwise, with
  section 1.3's selection caveat bounded by the fixed anchor.
- C2: fully provable offline -- the fixed-point argument is algebra, and the
  existing host tests already exercise both paths.
- C3: untouched. This option removes a writer and adds none.
- C4: **eliminates** the coupling. No site needs to know what `pid_fuzzy.c`
  does any more, so a later change to fuzzy -- or a future temperature-keyed
  gain schedule, the case `e78fbc5b` explicitly says its guard does *not*
  generalize to -- needs no new guard.
- C5: no convergence cost for `K_dc`. The real cost is section 4.1's
  residual.
- C6: zero new persisted state.

**Failure modes.** (i) The SIMC path refuses on non-material moves
(`ADAPTIVE_TUNE_MIN_MATERIAL_MOVE_FRAC`), so on a well-modelled plant
*nothing* adapts -- correct if the model and SIMC's lambda are right for
this plant, a genuine regression if they are not (section 4.1). (ii) It
inherits section 1.4's low-side bias unchanged. (iii) It removes the only
mechanism that could respond to a persistent limit cycle whose cause is the
*tuning rule* rather than the *gain*.

### Option B (brief's #4) -- freeze fuzzy during the identification window. RANK 2, adopt alongside A

Hold `pid_fuzzy_adjust()` at its strength-0 identity contract while a zone
is inside a dwell whose trace/observation is being harvested, and leave it
fully active on ramps and approaches.

- C1: makes section 1.3's selection effect and section 1.5's Jensen caveat
  structurally impossible instead of merely unlikely, and is the
  **precondition** for any future shape-based inference ever being valid.
- C5: costs fuzzy essentially nothing where it matters. Fuzzy's value is in
  the transient; inside a settled dwell error and error-rate are both near
  zero, so the rule table is in its centre cell and the multiplier is near
  unity anyway (consistent with
  `docs/audits/fuzzy_nine_cell_offline_probe_2026-09-11.md`'s centre-cell
  measurements).
- C4: the one entry that *adds* coupling -- something outside `pid_fuzzy.c`
  must know when the harvest window is open. Mitigation: pass the freeze in
  as a caller argument at `profile_executor_pid_tick.c`'s
  `pid_fuzzy_prepare_gains()`, driven by the settle state `adaptive_tune.c`
  already computes, rather than having `pid_fuzzy.c` reach out for it.
- C6: no persisted state; one runtime flag.

**Failure modes.** A gain discontinuity at window entry/exit (mitigable:
enter only once settled, when the multiplier is already near 1). And a scope
trap: freezing fuzzy only at the single harvest tick does not make a
shape-based Ki corrector valid -- the trace `adaptive_tune_ki` reads spans
the whole dwell, so the freeze must cover the whole trace capture.

### Option C (brief's #3) -- alternate: adapt one per firing, freeze the other. RANK 3

Literature-endorsed (section 2.4) and unquestionably safe. Rejected as the
primary mechanism on C5 and on proportion:

- Convergence cost is a straight **doubling of firings**, and firings are
  hours each on real hardware and unavailable on this bench at all.
- The thing fuzzy has to learn across firings is currently **one scalar**
  (section 5), inherited from `K_dc`. Alternating a slow, expensive schedule
  to protect a single derived number is disproportionate.
- It does not fix anything Option A does not. On the runs where fuzzy is
  frozen the Ki heuristic's inference is valid; on the runs where it is not,
  we are back to withholding. It is `e78fbc5b`'s interlock with a calendar
  attached.

Keep it in reserve as the scheme to use *if* a genuinely
closed-loop-shape adapter is ever wanted with fuzzy live and Option B proves
insufficient.

### Option D (brief's #1) -- divide fuzzy's contribution back out. RANK 4, reject

The brief's premise is true: fuzzy is deterministic given error and
error-rate, so the applied multiplier is exactly knowable, and recording the
effective gains would make the adaptation's frame explicit. Reject anyway,
on three independent grounds:

1. **There is nothing to divide.** The correction being adjusted is a
   bang-bang `+/-20%` classifier output (section 1.2, independently
   confirmed by execution in `a3803057` R3). Dividing a classifier's label
   by a gain multiplier produces a number with no physical meaning. This is
   `a3803057` R1's finding and it is correct.
2. **The textbook condition fails.** The indirect closed-loop route needs
   the controller linear and exactly known (section 2.1); fuzzy is nonlinear
   and time-varying. One could still divide numerically, but the resulting
   estimate has no consistency guarantee to appeal to.
3. **C4 is the worst of any option.** It hard-wires `adaptive_tune_ki.c` to
   `pid_fuzzy.c`'s exact rule table, membership shape and
   `MAX_NUDGE_FRACTION`. Any later change to fuzzy silently invalidates the
   division with no failing test -- textbook membership in this repo's
   "reset one side of a pair" class. C6 also costs per-tick effective-gain
   logging into a trace ring already bounded by
   `ADAPTIVE_TUNE_KI_TRACE_CAPACITY`.

The one thing worth salvaging from Option D: recording the effective gains
actually run under is cheap and useful **as a diagnostic**, independent of
anyone dividing by them.

### Ranking summary

**A > B > C > D.** A and B are complementary and should ship together: A
removes the invalid inference, B removes the residual selection coupling and
buys the option of reinstating a shape-based adapter later on honest
footing.

### 4.1 Should the Ki heuristic simply be deleted?

**Its write authority: yes, delete it. The file: no, not yet.** Stated
plainly, as the brief asks.

The case for deletion is strong. `adaptive_tune_run_end()`
(`adaptive_tune.c`, the D5 block) already runs
`adaptive_tune_refine_ki_locked()` **only when the SIMC path did not apply**
-- the two were made mutually exclusive per-run long before `e78fbc5b`, for
the same reason in miniature ("the two layers do not compose in one run").
The SIMC path computes Kp/Ki/Kd *absolutely* from a fresh `K_dc` fit
(`adaptive_tune_model.c:204`, `pid_autotune_tune_from_fopdt()`), through the
same rule autotune's Accept path uses, and re-latches `ki_baseline` when it
does. So on every run where the model moved materially the Ki heuristic is
already dead weight, and on the runs where it does fire its output carries
no measurement content.

But it is **not literally redundant**, and claiming so would be wrong. The
SIMC path refuses on a non-material move, on an implausible fit, on too few
observations, on insufficient duty spread. On a well-modelled plant it
refuses most runs by design. What the Ki heuristic covers, in principle, is
a different error: not "`K_dc` is wrong" but "the FOPDT-plus-fixed-lambda
*design rule* is wrong for this plant". A persistent limit cycle at a
correctly identified gain is real evidence of that, and no amount of
refitting `K_dc` will see it. Deleting the file deletes that channel.

**Recommendation:** demote to diagnostic-only now (Option A). If a Ki
corrector is wanted back, rebuild it on the quantities this path already
computes and throws away -- `ku_estimate`/`tu_estimate_s`, a
describing-function estimate with actual physical content and a *continuous*
magnitude -- feed them through the same `pid_autotune_tune_from_fopdt()`
rule rather than nudging a stored gain, and gate it behind Option B's frozen
window so its evidence is controller-invariant. That is larger than this
pass, and it should not be started until someone has a real limit cycle to
point at. It would still be a *loop* observation, so Option B's freeze is
not optional for it, and `e78fbc5b`'s guard site remains the right home for
whatever gate it needs.

---

## 5. Can the fuzzy layer itself learn across firings?

Today it already does, in exactly one degree of freedom -- worth being
precise about rather than overselling.

`pid_fuzzy_derive_bands()` (`pid_fuzzy.c`, per `2c49465a`) computes:

- `error_band_c = model_k_dc * ERROR_BAND_K_FRACTION`
- `rate_band_c_per_s = model_k_dc / model_tau_s`

`adaptive_tune` refines `model_k_dc` and **explicitly carries `tau_s` and
`dead_time_s` through unchanged** ("dwell data cannot inform dynamics",
`adaptive_tune_model.c`'s own comment where the FOPDT model is assembled).
Therefore:

1. Both bands scale **linearly in the same single scalar**, `K_dc`. There is
   one adapted number, not two.
2. Their **ratio is exactly `model_tau_s`**, which `adaptive_tune` never
   moves. The shape of the fuzzy input plane -- how much error trades
   against how much error-rate -- is frozen at the original autotune for the
   life of the zone.

**Does that constitute "fuzzy improving with firings"?** Partly, and in a
weaker sense than the phrase suggests. It is genuinely automatic
model-tracking, and it **inherits `K_dc`'s frame-independence**, so it is
safe to run concurrently with everything above: fuzzy's own bands moving
cannot corrupt the fit that moves them, for the section 1.1 reason. But what
it delivers is "fuzzy stops being mis-scaled as the plant model improves",
not "fuzzy learns what works". Nothing about the rule table, the cell
asymmetry, `MAX_NUDGE_FRACTION` or `fuzzy_strength_pct` adapts at all.

**What should adapt beyond it, and what should not:**

- **Should (cheap, stays inside plant identification):** make the bands
  operating-point-dependent. `5d3bc854` has just added `zone_model_at()` and
  recorded each fit's operating point; bands derived at the dwell's actual
  temperature rather than from one global `K_dc` would be a real improvement
  with no new inference class. Recommended follow-up.
- **Should (bounded, still plant-grounded):** let `tau_s` be refit so the
  band *ratio* can move. That needs transient data, not dwell data, so it is
  an autotune-side change, not an `adaptive_tune` one. Out of scope here;
  recorded as the thing standing between fuzzy and a second degree of
  freedom.
- **Should NOT, without Option B:** adapting `fuzzy_strength_pct` or the
  rule table from observed firing outcomes. That is closed-loop-heuristic
  adaptation by definition -- it scores the loop's shape -- and it would sit
  in exactly the position `adaptive_tune_ki` sits in today, with the added
  hazard that its output feeds back into the very multiplier the trace was
  shaped by. If it is ever wanted it needs Option B's freeze, a continuous
  (not bang-bang) magnitude, and an anchor to a fixed baseline in the manner
  of `97288659`.

**Not repeated here:** the non-circularity argument as specified in the
confidence-authority design, which `1142c73b` refuted (cross-run ring
contamination making the residual partly in-sample; the duty-spread gate
being duty-independent for a linear predictor; selection bias as the real
leak). Nothing in section 5 relies on it. Where section 1.3 needed the same
idea it is used only in the direction `1142c73b` upheld -- the settled-dwell
fixed-point argument -- and that review's selection-bias objection is
carried forward as a live caveat, not waved off.

---

## 6. Constraints respected

- **Single zone only.** No claim here depends on the coupling matrix, which
  is refuted; `adaptive_tune_refine_coupled_locked()` was read but is out of
  scope.
- **Nothing is bench-validatable.** The bench cannot exceed ~40 C above
  ambient and is a ~4 W fixture; every argument above is algebraic or
  source-traced, and every option was ranked partly on being provable
  offline (criterion 2). No bench value appears in this document and none
  should ship.
- **The `autotune_baseline_k_dc` envelope is untouched.** No option
  proposes moving, re-anchoring or widening it; Option A removes a gain
  writer and adds none, and the envelope's induction proof is independent of
  everything discussed here (section 1.1).
- **No production code was written; no board was flashed; no firing was
  run.** Files owned by concurrent sessions (`pid_fuzzy.c`/`.h`,
  `profile_executor_pid_tick.c`, `sim_fuzzy_overshoot.c`, `ROADMAP.md`,
  `docs/FUZZY_CONTROLLER_PLAN.md`, `docs/SCENARIO_SIMULATION.md`) were
  read only.

## 7. Verification performed

- **By reading:** `adaptive_tune.c` (`adaptive_tune_fit_gain()`,
  `adaptive_tune_zone_tick()`'s settle/harvest chain,
  `adaptive_tune_run_end()`'s skip chain and the D5 ordering),
  `adaptive_tune_model.c` (`adaptive_tune_refine_zone_locked()` in full,
  including the baseline anchor and the envelope induction comment),
  `adaptive_tune_ki.c` (`adaptive_tune_diagnose_ki()`'s verdict branches),
  `pid_fuzzy.c` (`pid_fuzzy_adjust()`'s defuzzification and the three gain
  multiplies, `pid_fuzzy_derive_bands()`),
  `docs/audits/adaptive_tune_ki_effective_reference_loop_2026-09-13.md`
  including its `a3803057` review, and the `1142c73b` review appended to
  `docs/audits/adaptive_tune_confidence_authority_design_2026-09-13.md`.
- **By execution:** `git cat-file -t` on every hash cited here; web search
  plus one full-text PDF fetch (section 2.1);
  `tools/check_doc_hash_citations.ps1`.
- **Not re-verified, taken from the cited audits:** the 1.2x-per-run /
  9-runs arithmetic and the 4.2998 negative-test figure (`a3803057` R3 and
  R5 verified both by execution); the fuzzy centre-cell multipliers from
  `docs/audits/fuzzy_nine_cell_offline_probe_2026-09-11.md`.
- **Deliberately not run:** `tools/run_all_checks.ps1` -- concurrent
  sessions are running it.
