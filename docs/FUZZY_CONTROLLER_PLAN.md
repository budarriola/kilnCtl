# Fuzzy controller plan (2026-09-11)

Companion to `docs/audits/fuzzy_controller_improvement_scoping_2026-09-11.md`
(commits `7e669c18`, `90abaf5f`, `0551d6c7`). That document establishes what the
fuzzy layer *is* and what evidence exists; **this document does not restate it.**
It weighs architectural options the scoping pass did not, and **disagrees with its
recommendation** (§2, §4).

**Update, `docs/audits/adaptive_tune_vs_owner_requirements_2026-09-11.md`
(`655da406`), postdates this plan's authoring commit (`5a16e950`) by one commit
and independently confirms finding (B) below in more depth than this plan
originally had it: `adaptive_tune` satisfies owner requirements (a) and (b) **in
code**, has never actually run on this board (disarmed, zero observations), and
does **not** address requirement (c) — confidence-graduated authority — in any
form. See §0.2.**

**Owner decision 2026-10-07: fuzzy stays at strength 0, no removal; revisit after real firings. No software work is pending from this plan until then.**

No controller behaviour is changed by this document. No board was flashed and no
heating run was performed. Board facts are live `kiln_call` reads.

**Update, 2026-09-13: Stage 0 (the (v-a) offline rule-cell probe) has run,
and its downstream comparison was then independently reviewed and partly
overturned.** `ed854ac5` found the centre cell's exact effect — kp x0.75 /
ki x1.25 / kd x0.75 at strength 50 (x0.875/x1.125/x0.875 at 25), the only
cell ever observed on real hardware (100% of a 2178-sample mode-3 capture).
Its headline "6 of 9 rule cells unreachable" claim rests on a ~0.083 degC/s
"measured plant maximum" that a same-day adversarial review (`8a12521b`,
`docs/audits/review_sim_fuzzy_commits_2026-09-13.md`) traced to an arithmetic
conversion of a profile-rate setting (300 degC/hr / 3600), not a
measurement — the same module's own header separately records a real
measured peak of 0.110 degC/s, above the "maximum" the reachability count is
built on, and the reachability test itself is a knife-edge 0.4% margin.
**Do not cite a reachability count** (neither "6 of 9" nor any other split);
the multiplier assertions (the centre-cell gain triple above) are sound and
still citable. A follow-on comparison (`ba230bca`) then asked whether the
centre-cell behaviour is doing anything beyond being a better fixed PID
tuning, and originally reported yes (~7.8% IAE, 0.22 degC MAE) — **that
result has since been retracted**: the same review found `ba230bca`'s
fuzzy-ON arms were measured against a stale build compiled minutes earlier
while `ed854ac5`'s negative test had the centre cell's rule sign inverted;
the hand-restored source gave an empty `git diff`, but the poisoned binary
was never rebuilt before being measured. Rebuilding from tracked sources
reproduces `fbdc5bd0`'s original figures exactly (strength 50: IAE 12675.2 /
MAE 3.0469), and against those, fuzzy vs. an equivalent flat retune is a
**0.011 degC MAE** gap at strength 50 and reverses sign (flat retune
slightly better) at strength 25 — indistinguishable from a fixed multiplier
on this scenario, not a demonstrated inference benefit. See §2(iv) and §4.3
for what this changes; do not describe fuzzy as beating a flat retune
anywhere in this document.

**Update, 2026-09-14 (owner decision + second independent measurement):**
the owner has decided the (i)-derived band normalisation SHIPS — see §2(i)'s
update. Separately, `docs/audits/fuzzy_overshoot_measurement_2026-09-13.md`
plus its appended review (`1570a65a`) re-ran finding (A) directly against the
owner's four-part objective (not IAE/MAE) and reached the same conclusion by
an independent route: that document's own "equivalent fixed retune" arm
turned out to be fuzzy_50's centre-cell MAXIMUM (kp x0.75/ki x1.25/kd x0.75,
reachable only at error=0/rate=0), not its measured ramp-phase average (kp
x0.8684/ki x1.1618/kd x0.8382). A plain static gain rescale at that true
average reproduces fuzzy_50 on all four objectives inside materiality, and
the penalty tracks gain magnitude monotonically with no discontinuity at the
inference boundary. **The fuzzy layer remains indistinguishable from a gain
change of the same magnitude — do not cite that document's headline
(0.42 degC overshoot advantage, 40-80 s settle improvement) without also
citing the review that overturns both numbers.** See §2(iv)'s 2026-09-14
update, which withdraws this section's own 2026-09-13 note about a
weakened "for" case.

**Scope correction, same day.** State this finding as **"no demonstrated
benefit under matched conditions on this bench"**, not "no demonstrated
benefit," everywhere it is cited. Every measurement behind it — the
~4 W bench fixture, a well-tuned PID, and a plant model matched to the
plant it is fit against — is the single condition **least** likely to
reveal a gain-adaptation layer's value, since such a layer earns its keep
when the plant does NOT match what the PID was tuned for. This is weak
evidence against the layer, not evidence for removing it: "untested under
the conditions where it would matter" and "tested and found useless" are
different claims, and only the first is supported. Untested conditions
where a benefit would be expected: a changed thermal mass (load added or
removed since the PID was tuned), a PID tune that was never good for the
real plant, the high-temperature regime (`k`/`tau` roughly 20x lower than
bench scale), and a thermocouple positioned close to the elements rather
than at the load — which shortens apparent dead time and adds a fast mode
the FOPDT fit does not represent, a plant/model mismatch of exactly the
kind a gain-adaptation layer exists to absorb, and one the simulator
cannot currently express at all. `docs/SCENARIO_SIMULATION.md`
(authored separately, in progress as of this note) is scoping simulation
coverage of these conditions — cite it as in-progress only; no outcome
exists yet. Separately, the owner has decided fuzzy constants, like PID
gains, are to be **derived per kiln, not shipped** — bench values may be
anything convenient precisely because they never ship, so no bench
measurement in this document is evidence for or against a shipped
default.

---

## 0.0 The objective is four-part, stated by the owner (2026-09-13) — governs everything below

The controller objective is not one thing to optimise. It is four separate
objectives:

1. reaches temperature **at the correct rate** (ramp-rate tracking / lag),
2. **settles quickly**,
3. **settles accurately** (steady-state error at dwell),
4. minimal **over/undershoot**.

The owner identified reducing over/undershoot (objective 4) as **the fuzzy
layer's specific purpose** — PID alone is expected to handle settling to the
correct temperature (objectives 2/3), and per §0.1 below, ramp tracking
(objective 1) is the feedforward climb term's job, not fuzzy's.

**Methodological consequence, retroactive:** IAE and MAE, used throughout §2
and §4 of this document to score the centre-cell-vs-flat-retune comparison,
are **aggregate metrics that collapse all four objectives into one number in
which they trade invisibly.** A change that cuts overshoot by slowing the
approach looks neutral-to-good on an aggregate error metric while regressing
objectives 1 and 2, and the reverse is equally possible. Every IAE/MAE figure
already in this document (the `fbdc5bd0`/`ba230bca` comparisons in the header
and in §2(iv)/§4.3) was scored this way — **that evidence needs re-scoring
against the four objectives separately, not silent re-ranking of the options
it was used to compare.** This document does not redo that scoring; it flags
where it is owed, per-option, in §2 and §4.3 below.

The repo already has the right instrument for the re-score: `firing_score.c`'s
per-objective subscores — `FIRING_SUBSCORE_LAG_S` for rate/lag (objective 1),
a dwell-entry-peak subscore for overshoot (objective 4;
`FIRING_SUBSCORE_ENTRY_PEAK_C`), and others for settle time/accuracy
(objectives 2/3). Future controller work in this space should report all
four separately and call out any trade explicitly, rather than reducing to
one IAE/MAE number. The existing 0.5 degC materiality rule
(`feedback_ignore_sub_half_degree_effects`) applies **per objective, to the
quantity that objective actually measures** — a 0.5 degC change in dwell
accuracy and a 0.5 degC change in overshoot are two separate findings, not
one.

### 0.0.1 Ramp tracking (objective 1) is closed against the fuzzy layer — do not modify the rule table for it

`c002ceaf` (`docs/research/fuzzy_ramp_tracking_2026-09-13.md`, read and
verified against source in this pass) answers the owner's question "can
fuzzy also help ramp tracking?" **No, structurally, with its current
inputs:**

- The two ramp-lag rule cells — (error=NEG, rate=STEADY) and (error=POS,
  rate=STEADY), `RULE_TABLE` rows 0/2, column 1 (`pid_fuzzy.c:149-162`) —
  are both `{kp +1, ki 0, kd 0}`. They push on `Kp`. But for a PI(D) loop
  against a plant with no free integrator (this project's FOPDT model),
  steady-state ramp-following error is set by the velocity constant
  `Kv = Ki * P(0)` — it depends on **Ki**, which those cells leave
  untouched at any `strength_pct`. Raising `Kp` changes the transient, not
  the asymptotic ramp lag. Wrong lever, not merely poorly tuned.
- The layer's rate input is `-d(measurement)/dt` and has **no access to
  `d(setpoint)/dt`** at all (`pid_fuzzy.c`'s own header). A well-tracked
  ramp and a laggingly-tracked ramp differ almost entirely in steady
  *error*, not its derivative, so the rate axis cannot discriminate them —
  only the error axis can, and that reduces to "the ordinary large-error
  cells fire," which is the overshoot machinery, not a ramp-tracking one.
- The mechanism that structurally targets ramp lag is the existing
  **feedforward climb term** (`pid.c`), and the surveyed literature
  (abstract-only citations, see the research doc's §5) consistently assigns
  feedforward to the ramp/trajectory role and fuzzy/PID adaptation to the
  step/steady-state role — matching this project's existing architecture
  rather than motivating a change to it.

**Closed: do not modify `RULE_TABLE` or `pid_fuzzy.c`'s inputs for
ramp-tracking purposes.** This does not touch options (i)-(v) below, all of
which are about objective 4 (overshoot) and the bootstrap/adaptation
requirements — it forecloses only a fuzzy-side ramp-tracking redesign that
was never actually proposed in §2, so no option's ranking changes. Whether a
variant of the feedforward climb term (e.g. the reverted dwell-entry decay)
is worth retrying is a separate, explicitly out-of-scope question under
active review elsewhere — no outcome to report.

### 0.0.2 New, untested candidate for objective 4 (overshoot): setpoint weight `b`

`pid.c`'s `p_term = kp * (b*setpoint - measurement)` has a setpoint-weight
factor `b`, hardcoded to `1.0` via `PID_SETPOINT_WEIGHT_B`
(`profile_executor_internal.h:251`) and never tuned. The literature
(Visioli 1999, cited from its abstract only in `c002ceaf` — full text was not
retrievable through IEEE Xplore, mark it as such) treats setpoint weighting
specifically as a step-response/overshoot-vs-rise-time tool, which matches
objective 4 directly and does not touch `Ki` (so, by the same `Kv` argument
as §0.0.1, it should not move ramp-tracking lag materially — that is a
prediction to verify, not an assumed fact).

**Candidate, NOT YET TESTED:** sweep `b` in simulation (e.g. 0.4/0.6/0.8/1.0)
on a single-zone ramp-into-dwell profile, scoring dwell-entry peak
(overshoot, objective 4) and `FIRING_SUBSCORE_LAG_S` (objective 1, to confirm
`b` does not regress ramp tracking) separately — never as a combined IAE/MAE
number, per §0.0. One parameter, no rule-table change, no change to
`pid_fuzzy.c` at all. This is additive to §2's option set, not a replacement
for any of them — it competes with, and is far cheaper than, the fuzzy
layer's own overshoot role, so any options weighing (ii)/(iv)/(v) should
note this candidate as an unexplored cheaper alternative for the same
objective.

**Now tested, 2026-09-22, FALSIFIED for the current gain set:**
`docs/audits/setpoint_weight_b_sim_2026-09-22.md` — with this baseline's
`kp=0.0318 ki=0.0001 kd=0.8401`, the two "approach from below" dwells (where
overshoot risk actually lives) show zero overshoot at every `b` including
1.0, so there is nothing for `b<1.0` to fix, while ramp-lag, settle time, and
steady-state RMS all regress sharply as `b` drops (two dwells never settle at
all at `b<=0.6`). Shipped `b=1.0` stays correct; not closed for a materially
larger `ki`, which this sweep does not cover.

---

## 0. Facts this plan rests on (each verified in this pass, not inherited)

| Fact | Source, verified |
|---|---|
| `fuzzy_strength_pct = 0.0` on all three zones; `control_mode = 3` | `control_get_zones()` live read, 2026-09-11 |
| `strength_pct == 0` reproduces base gains bit-for-bit, no fuzzy math in path | `pid_fuzzy.c:208-213` (read) |
| Inputs are this zone's own `error_c` and `d_filtered` only; no neighbour state | `profile_executor_pid_tick.c:325-326`; exclusion is deliberate, `pid_fuzzy.h:23-32` |
| Bands are absolute degC / degC-per-s: `ERROR_BAND_C_DEFAULT 20.0f`, `RATE_BAND_C_PER_S_DEFAULT 0.5f` | `pid_fuzzy.c:90-91`; header admits desk reasoning about "a mid-size kiln", `pid_fuzzy.h:8-15` |
| Output is a bounded rescale of kp/ki/kd, +-25% at strength 50, +-50% at 100 | `MAX_NUDGE_FRACTION 0.5f`, `pid_fuzzy.c:96,281-285` |
| Autotune produces `k_gain_c_per_duty`/`tau_s`/`dead_time_s` per zone; none reach the fuzzy layer | `pid_autotune.h:53-57`; no `model_` symbol in `pid_fuzzy.c` or `pid_fuzzy_prepare_gains()` |
| k and tau fall together, central estimate ~20x, bench to cone-10 | `docs/audits/high_temperature_transfer_analysis_2026-09-08.md:115-116` |

### 0.1 Four findings this pass adds, which change the analysis

**(A) The centre rule cell is not neutral — it is a constant, full-magnitude
detune of autotune's answer.** `RULE_TABLE[1][1]` (ZERO error x STEADY rate) is
`{kp -1, ki +1, kd -1}` (`pid_fuzzy.c:149`). At `error = 0, rate = 0` the
membership degrees are exactly `e_zero = r_zero = 1`, so that single cell fires
alone at weight 1 and the output is `kp*0.75, ki*1.25, kd*0.75` at strength 50.
**A gain schedule that does nothing at the nominal operating point is the
expected design; this one systematically de-tunes the gains precisely where
autotune's identification is most valid.**

This matters because it *fully explains* the single mode-3 hardware arm the
scoping doc rests on: kp/kd down ~22-23%, ki up ~20-23%, essentially constant
for 63.8 min (scoping §2, citing `fuzzy_behavior_20260904d_report.md`). The
scoping doc reads that as a symptom of bands sized 3-5x wider than this rig's
envelope, and §8.1 proposes fixing it by deriving the bands from autotune.
**That inference does not hold.** The observed rescale is what the centre cell
outputs *by construction*, at any band width, for any plant, whenever error and
rate are small. Narrowing the bands does not remove it — narrower bands make
the *outer* cells reachable, but the centre cell's non-zero direction is
untouched. Any band-derivation work that does not also address `RULE_TABLE[1][1]`
leaves the one effect actually measured on hardware in place.

**(B) A continual gain-adaptation mechanism already exists and already meets
owner requirements (a) and (b) — for the base gains.** `adaptive_tune.c/.h`
refines each zone's `K_dc` from the settled dwells every ordinary firing already
produces, and recomputes that zone's PID gains through **the same SIMC path
autotune's Accept uses** (`pid_autotune_tune_from_fopdt()`), per-zone opt-in
(`zone_cfg_t::adaptive_tune_enabled`, `zones_config_json.h:621`), writes only at
run-end after relays are off, with `adaptive_tune_revert()` as one-click undo
(`adaptive_tune.h:1-48`). **The scoping document does not mention it once**
(`grep -c adaptive_tune` on that file returns 0). This is load-bearing: the
owner's "bootstrap from autotune, keep adapting over heat cycles" requirement is
already satisfied by an existing, reviewed, plant-measurement-driven mechanism —
just not by the fuzzy layer.

**(C) The seam for operating-point gain scheduling already exists at HEAD, and a
specific one-parameter schedule is already designed.** `zone_model_at()` /
`coupling_at()` (`zones_config_accessors.h:661-674`, added `5d3bc854`) route every
control-path model read through one function that accepts a temperature and today
ignores it — a deliberate passthrough seam for exactly this.
`high_temperature_transfer_analysis_2026-09-08.md:342-359` then gives the design:
**do not schedule Kp; schedule Ki on measured temperature as
`ki(T) = ki_ref * s(T)/s(T_ref)` with one free parameter, defaulting inert; fix
the feedforward hold term first.** `s(T)` (`loss_conductance_scale()`) exists
today only PC-side in `tools/PcTools/src/kilnctrl/plant_sim.py:268-290`, with its
one constant `RAD_LOSS_FRACTION_AT_REF` explicitly marked ASSUMED — there is no
firmware implementation. So this is a designed proposal, not shipped code, and
its single parameter is currently a guess too; but it is **one** physically-derived
guess against fuzzy's 27 rule directions plus 6 band constants.

**(D) The fuzzy layer reads a different error than the control loop does.**
`pid_fuzzy_prepare_gains()` uses `s_exec.target_c`, deliberately *not*
`z->effective_target_c`, while `pid_update_terms()`/`zone_feedforward()` in the
same tick use the per-zone effective target
(`profile_executor_pid_tick.c:313-325`, which documents the choice). With
`approach_rate_cap_c_per_hr = 0.0` on all three zones (live read) the two are
identical today, so this is inert — but it is exactly the documented
*paired-input-left-shared* class (`project_paired_input_left_shared`), armed by
the first non-zero cap. It is a live latent defect independent of everything
else in this plan.

### 0.2 Audit update (`655da406`): what finding (B) means, precisely

The 2026-09-11 audit (verified independently against source and a live
`kiln_call(name="adaptive_tune_get_status")` read, not against this plan)
sharpens finding (B) into three separate claims that this plan previously
ran together:

1. **(a) and (b) are satisfied by `adaptive_tune` today, in code.** It
   refuses to refine without an existing autotuned model/PID/coupling row
   (bootstrap-only, never invents starting gains) and keeps refining every
   clean firing indefinitely, both host-tested (`test_adaptive_tune*.c`,
   6 files).
2. **This is a claim about the code, not about demonstrated hardware
   behaviour.** All three zones read `enabled=False`,
   `observations_lifetime=0` on the live board — `adaptive_tune` is
   reachable end-to-end but has never actually run. It has contributed
   nothing to this board's current gains.
3. **Requirement (c), authority growing with measured confidence, is not
   addressed by `adaptive_tune` in any form.** Every guard it has
   (`ADAPTIVE_TUNE_MIN_OBSERVATIONS`, the blend factor, the per-run move
   cap) is a fixed constant applied identically to the 1st and the 100th
   accepted refinement — there is no confidence/authority state at all.
   This is the one owner requirement nothing shipped addresses.

The same audit also documents a **ratchet defect** in `adaptive_tune`
(K_dc's plausibility/move bounds are anchored to the most recently
adapted value, not the original autotune result, with no absolute
ceiling) — a separate, concurrent pass is fixing this as of this writing;
**it is in progress, not done**, and this plan does not describe its
outcome.

**Consequence for this plan's recommendation:** finding (B) below already
concluded "extend `adaptive_tune`, do not build a second mechanism" for
(a)/(b), and nothing here changes that. What the audit adds is precision
about what is *left*: requirement (c) is wholly unaddressed by any shipped
code, adaptive-tune or fuzzy, and is therefore the actual open target for
"confidence-driven authority" work — not a new fuzzy-side bootstrap
mechanism (scoping doc §8.2, which this plan's finding (B) already argued
against building; see §4.2). Whatever shape (c) takes, criterion 5 (one
writer per piece of state) says it belongs as additional state and a
schedule layered onto `adaptive_tune`'s existing run-end call, not as a
second mechanism reading/writing the same gains — the audit's own §4 and
§7 reach the identical conclusion independently. The fuzzy-specific work
that remains genuinely fuzzy's own — finding (A)'s rule-table detune and
finding (D)'s shared-input defect — is unaffected by any of this and is
still handled by options (ii)/(v) below.

---

## 1. Decision criteria — stated before the verdict

An option is preferred to the extent that it:

1. **Adds free parameters only where evidence can constrain them.** Parameters
   this project cannot measure are liabilities; see
   `project_load_estimator_cannot_observe_load` and
   `project_zone2_dead_time_not_observable`.
2. **Addresses the effect actually dominating a real kiln** — k and tau falling
   ~20x with temperature — rather than an effect nobody has measured.
3. **Bootstraps from the installed kiln's own autotune with no bench-derived
   constant in the shipping path** (owner requirement a).
4. **Is validatable single-zone in simulation**, since single-column transport is
   measured linear while multi-zone superposition is refuted (owner requirement d;
   `coupling_sign_reversal_artifact_test_2026-09-11.md`).
5. **Has exactly one writer per piece of state.** Two mechanisms adapting the
   same gains is the `project_bypassed_owner_module_bug_class` shape.
6. **Costs kiln time proportional to what it can prove.** This bench is a ~4 W
   fixture capped ~40 degC over ambient; it cannot falsify a high-temperature claim.

**What would change the recommendation** is stated per-option in §2 and
summarised in §4.3.

---

## 2. Options

### (i) Scoping doc's recommendation — dimensionless bands from autotune, per-cell confidence driving `strength_pct`

Derive `error_band_c`/`rate_band_c_per_s` from the zone's own `k_dc`/`tau_s`
(scoping §8.1); add 9 per-cell confidence values per zone (27 board-wide) that
raise `strength_pct` from 0 only under measured evidence (scoping §8.7).

**For.** Directly answers requirements (a)-(d) as literally posed. The band
constants genuinely are an unmeasured absolute-units guess, and normalising them
is a real correctness improvement regardless of anything else. Confidence-gated
authority is the right *shape* for any online adaptation on a heater. Bands are
already per-zone runtime config, so bootstrap needs no new schema.

**Against.** It fixes the axis scaling but not finding (A) — the only fuzzy
behaviour ever measured on this hardware survives the fix untouched, so the
headline deliverable does not move the one number we have. It adds ~33 adapted
values per board (6 bands + 27 confidences) on a project that has repeatedly
failed to constrain far smaller parameter sets, and each confidence value needs
evidence gathered *in its own rule cell*, of which 8 of 9 have never fired once
on this hardware (scoping §2) and cannot be made to fire on a 40 degC-range
fixture. The dimensionless constants `N` and `M` in the derivation are themselves
new free parameters chosen by desk reasoning — the same defect one level up. It
is by a wide margin the most machinery of any option here.

**2026-09-14 update — OWNER DECISION: SHIP the band-derivation half of this
option.** `2c49465a` implements the `error_band_c`/`rate_band_c_per_s`
derivation from `model_k_dc`/`model_tau_s` described above, with the
previous absolute constants retained only as an explicitly-logged fallback
for a zone that has never been autotuned. The owner chose to keep this
because it satisfies the standing requirement that nothing ship guessed for,
or tuned to, a kiln other than the installed one, at no material cost — it
does not add the 27 per-cell confidence values also proposed under this
option, and does not change the "Against" paragraph's objection that finding
(A) survives the fix untouched (see the header update and §2(iv) below):
band-unit correctness and rule-table/strength-authority are separate axes.
Do not read this decision as adopting (i) in full, only its band half.

### (ii) Keep the Mamdani structure, fit the rule table instead of hand-authoring it

Retain 3x3 membership, replace `RULE_TABLE`'s 27 hand-written directions with
values fitted offline against a closed-loop sim, shipped as a compile-time
constant (or, with a schema bump, per-install).

**For.** Directly attacks finding (A), the one thing actually measured. Fitting
would almost certainly drive `RULE_TABLE[1][1]` toward `{0,0,0}`, recovering the
"neutral at nominal" property for free. Structurally cheap: the array shape, the
call site, the bounded-nudge contract and the existing drift harness
(`pid_fuzzy_drift_harness.c` + `fuzzy_gain_mirror_drift_check.py`) all survive.

**Against.** Fitting 27 direction values against `sim_plant.c` produces a rule
table fitted to *the simulator*, and per scoping §6.2 a sim win is screening, not
evidence of hardware improvement. Shipping it per-install reopens the design
decision `pid_fuzzy.h:17-21` closed deliberately ("a per-installation editable
rule table reopens the trial-and-error tuning problem this whole mode exists to
avoid"). Shipping it board-wide contradicts owner requirement (a) unless the fit
is done on a plant model the installed kiln produced. And a fitted table is
harder to reason about than a hand-written one when a firing goes wrong.

### (iii) Replace the fuzzy layer with operating-point gain scheduling

Implement the `high_temperature_transfer_analysis` design: schedule Ki (not Kp)
on measured zone temperature through the existing `zone_model_at()` seam, one
free parameter (`RAD_LOSS_FRACTION_AT_REF`-equivalent), defaulting inert so the
change is a no-op until switched on. Fix the feedforward hold term first, as that
analysis requires.

**For.** Scores best on criteria 1, 2 and 5. **One** free parameter against
fuzzy's 33. It is derived from physics (radiation fraction growing as T^4) rather
than from a rule of thumb, so it extrapolates from bench to kiln *by
construction* rather than needing to be re-measured there. The seam exists at
HEAD; the design is already written and reviewed; the schedule defaults inert,
which is the safest possible shipping posture. It targets the ~20x effect that
actually dominates a real kiln, which fuzzy's +-25%/+-50% nudge cannot reach at all.

**Against.** Its one parameter is currently ASSUMED, not measured, and **this
bench cannot measure it** — the effect is only observable above the fixture's
~40 degC ceiling, so it ships untested at the operating point where it matters
(criterion 6 cuts against every option equally here, but this one most visibly).
The analysis itself makes it conditional on fixing the feedforward hold term
first (`:356-359`: "fitting a schedule to an artefact"), which is a prerequisite
piece of work this plan does not own. It also does nothing for requirement (c) —
there is no natural "authority grows with confidence" story for a schedule that
is either on or off.

### (iv) Delete the fuzzy layer; invest the same effort in PID + feedforward + `adaptive_tune`

Remove `pid_fuzzy.c/.h`, `ZONE_CONTROL_MODE_FUZZY`, the band/strength config
fields and the drift-check machinery. Spend the effort on the feedforward hold
term, on `adaptive_tune`'s existing cycle-over-cycle refinement, and on (iii).

**For.** The layer has never demonstrably done anything on this board: the only
behaviour ever measured is a constant rescale that finding (A) shows is a
property of the rule table, not adaptation. It is disarmed today
(`strength_pct = 0`), so nothing in production depends on it. Finding (B) means
requirements (a) and (b) are **already met by `adaptive_tune`** through a
mechanism driven by an independent plant measurement rather than by a rule table —
and keeping both means two mechanisms writing the same gains (criterion 5).
Deleting removes finding (D)'s latent defect outright. Every hour spent on fuzzy
is an hour not spent on the feedforward term, which
`high_temperature_transfer_analysis:356-359` says is *currently wrong* and is
forcing the integrator to do its job.

**2026-09-13 update, this case stands as originally written.** The Stage 0
probe (`ed854ac5`) plus a follow-on comparison (`ba230bca`) originally
reported that switching the fuzzy layer in beats an equivalent always-on flat
multiplier by ~7.8% IAE — which would have weakened this "for" case. An
independent same-day review (`8a12521b`,
`docs/audits/review_sim_fuzzy_commits_2026-09-13.md`) found that result was
measured against a stale, sabotaged build and does not hold: rebuilt from
source, fuzzy vs. an equivalent flat retune is a 0.011 degC MAE gap at
strength 50 and reverses sign at strength 25 — indistinguishable from a fixed
multiplier on this scenario. **"The layer has never demonstrably done
anything on this board beyond a constant rescale" remains the accurate
statement.** See the plan header for the corrected figures. This does not
strengthen (iv) either — it only removes a claim that would have weakened
it — so treat the "for"/"against" case as unchanged from before 2026-09-13.

**2026-09-14 update — the "weakened" framing above is WITHDRAWN, but this
does NOT strengthen (iv) either.** A second, independent measurement against
the owner's actual four-part objective
(`docs/audits/fuzzy_overshoot_measurement_2026-09-13.md` + its `1570a65a`
review, summarised in the plan header) found the same root cause on a
different harness: the document's "equivalent fixed retune" comparison arm
was fuzzy_50's centre-cell maximum, not its ramp-phase average, and a
correctly-scaled flat rescale reproduces fuzzy_50 on all four objectives
inside materiality. There is now no standing claim, from either harness,
that fuzzy beats an equivalent flat retune — **but every measurement behind
that conclusion was taken under matched conditions on this bench (a
well-tuned PID against a plant model matched to the plant), the single
condition least likely to reveal a gain-adaptation layer's value.** The
correct reading is **"no demonstrated benefit under matched conditions,"
still untested where a benefit would plausibly show up** — changed thermal
mass, a poorly-tuned PID, high-temperature `k`/`tau` scaling, or a
thermocouple-placement plant/model mismatch (see the header's 2026-09-14
scope correction) — **not "tested and found useless."** (iv)'s "for" case
rests on the layer never having demonstrated value; that remains true, and
the case is neither strengthened (nothing new was measured that favours
deletion) nor weakened (the earlier IAE headline that would have counted
against deletion is retracted) by anything landed today. Read it as
originally written, with this scope qualification attached.

**Against.** Deletion is irreversible on a hypothesis, and the hypothesis rests
on `n=1` incomplete hardware arm in one of nine rule cells — we have not actually
shown fuzzy is useless, only that we have never observed it being useful.
The layer is genuinely well-engineered defensively (NaN handling, the bit-exact
strength-0 contract, `pid_rescale_integral_for_new_ki()`'s bump transfer) and
costs nothing at rest. `MAX_NUDGE_FRACTION`'s +-50% bound is a real, meaningful
safety property that a gain schedule does not have. Deleting a selectable mode
is a user-visible removal and an owner decision, not an engineering one.

### (v) Hybrid / staged — neutralise, deprioritise, do not delete

Three small, independently-valuable steps, then stop and re-decide:
**(v-a)** offline probe of all 9 rule cells via the existing stateless drift
harness — no plant, no board, settles (A) and finds any other cell whose
direction is indefensible; **(v-b)** fix finding (D) (align the fuzzy error axis
with `z->effective_target_c`, or document in code why it must differ) — a small,
self-contained correctness fix with no behavioural change while the cap is 0;
**(v-c)** leave `strength_pct` at 0, take fuzzy off the roadmap as the adaptation
vehicle, and route the owner's four requirements to (iii) + `adaptive_tune`.

**For.** Costs a few hours, no kiln time, no firmware behaviour change, no
deletion. Converts the central open question from an opinion into a measurement.
Preserves every option including (ii) and (iv).

**Against.** It is deliberately not a fuzzy-improvement programme; an owner who
wants the fuzzy layer to *do something* will read (v-c) as a refusal. It defers
rather than settles the delete/keep question.

---

## 3. Options rejected without a full case

- **Routing fuzzy parameters through `iter_tune`'s apparatus.** Scoping §4 is
  right and its reasoning was checked: a 2-parameter-per-zone one-at-a-time line
  search at `mc_runs=220` does not extend to a 5-parameter search. Agreed, no
  further analysis.
- **Changing the bucket/cell count (3x3 to 5x5).** Multiplies the parameter count
  the project already cannot constrain. Strictly dominated by (ii).
- **Adding a neighbour-zone input to fuzzy.** Excluded by design
  (`pid_fuzzy.h:23-32`) for a correct reason, and blocked anyway while multi-zone
  superposition is refuted.

---

## 4. Recommendation

### 4.1 Verdict

**SUPERSEDED, 2026-09-14 (roadmap truth-up): the owner decided fuzzy is KEPT,
not deleted — see the header's "2026-09-14 update — OWNER DECISION: SHIP the
band-derivation half of this plan" note (line 370) and `2c49465a`
(`rate_band_c_per_s`/`error_band_c` derived per-zone from the autotune model,
shipped). The paragraph below, concluding "(iv) as the honest disposition of
the fuzzy layer," is the plan's original conclusion and is now contradicted
by that decision. Left in place rather than rewritten, per this repo's
standing rule that a retraction must be visible, not silent
(`project_retraction_hid_the_stale_claim`) — read it as history, not current
guidance.**

**Adopt (v) now; on its results, expect to land on (iii) as the answer to the
owner's four requirements, with (iv) as the honest disposition of the fuzzy layer
some cycles later.** Do not adopt (i). Requirements (a) and (b) are already
answered — in code, not yet on hardware — by `adaptive_tune` (§0.2); the
outstanding work this plan is actually deciding among is requirement (c)
(confidence-graduated authority, unaddressed by any shipped mechanism) plus
whatever is genuinely fuzzy-specific (finding A's rule-table detune, finding
D's shared-input defect) — none of which is a reason to build a second
bootstrap-and-adapt mechanism.

Against the §1 criteria: (iii) wins 1, 2 and 5 outright; (i) wins only 3 and
loses 1 badly; (ii) is the best option *if* the fuzzy layer is kept, and (v-a)
is its cheapest possible prerequisite. (v) is recommended first because criterion
1 says do not build parameter machinery before the cheap measurement that might
make it unnecessary, and (v-a) costs hours.

### 4.2 Where this disagrees with the scoping document

1. **The constant rescale is the rule table, not the bands** (finding A). The
   scoping doc's §8.1 band derivation is a real improvement to a real defect, but
   it is not a fix for the only behaviour ever measured, and §2/§8.1 read as if
   it were.
2. **`adaptive_tune` already exists** (finding B) and already implements
   "bootstrap from autotune, keep adapting each cycle" for the base gains. The
   scoping doc designs a second such mechanism (its §8.2, "Continual
   adaptation across heat cycles") without reference to it. Any
   continual-adaptation work must extend `adaptive_tune`, not sit beside it.
   `655da406` (§0.2) confirms this in more depth and narrows what remains:
   (a)/(b) are met **in code** (never yet run on this board), and
   requirement (c), confidence-graduated authority, is met by **nothing**
   shipped — not `adaptive_tune`, not fuzzy. That is the actual open target,
   not a second bootstrap mechanism on the fuzzy side.
3. **Per-cell confidence (§8.7) is not buildable on the evidence available.**
   It requires per-cell evidence for 9 cells; 8 have never fired on this
   hardware and cannot be made to on a 40 degC fixture. The mechanism would spend
   its life at confidence 0 for 8 of 9 cells, which is the correct-but-useless
   outcome, or be fed synthetic evidence, which is
   `project_idealized_test_input_bug_class`.
4. **Gain scheduling was not weighed at all**, despite the seam existing at HEAD
   and a specific design already written (finding C).

I agree with the scoping doc on: the sim-first ordering, the blocking finding
that no closed-loop sim exercises fuzzy today, the single-zone-before-multi-zone
constraint, the rejection of `iter_tune` reuse, and that the band constants are
a pre-existing shipped guess.

### 4.3 What would change this recommendation

- **(v-a) shows the 9 cells are individually sensible and `RULE_TABLE[1][1]`'s
  detune is small or defensible** then finding (A) weakens, (i) and (ii) both become
  substantially more attractive, and (ii) becomes the lead option. **(v-a) has
  now run (`ed854ac5`, 2026-09-13)** and confirmed the centre cell's detune
  (kp x0.75/ki x1.25/kd x0.75 at strength 50), the only cell ever observed on
  real hardware. Its claim that the other 6 of 9 cells are unreachable does
  **not** hold — a same-day review (`8a12521b`) found the "measured plant
  maximum" it rests on is an unmeasured profile-rate conversion, contradicted
  by this plant's own real capture. This bullet stays open rather than
  resolved: neither "sensible for all 9" nor "unreachable for 6 of 9" is
  established by the evidence so far.
- **A follow-on retune-vs-fuzzy comparison (`ba230bca`, 2026-09-13) originally
  reported the centre cell's switching behaviour beats an equivalent flat,
  always-on retune by ~7.8% IAE** — this would have weakened (iv)'s "never
  shown to do anything" argument. That result has since been **retracted**
  (`8a12521b`): it was measured against a stale build compiled while the
  centre cell's rule was sabotaged for `ed854ac5`'s own negative test.
  Rebuilt from source, fuzzy vs. an equivalent flat retune is a 0.011 degC
  MAE gap at strength 50 and reverses sign at strength 25 —
  indistinguishable from a fixed multiplier on this scenario. **Net effect:
  no option in this section gains or loses support from either 2026-09-13
  result** — the honest position is unchanged from before that date: nothing
  measured on this bench clears the 0.5 degC materiality line, in either
  direction, on any option. The deciding evidence for this design space
  cannot come from this bench fixture: the plant is capped ~40 degC above
  ambient with a real measured peak ramp rate (0.110 degC/s) still well under
  the rate band the off-centre cells need, so no measurement taken here can
  rule a larger effect in or out on the eventual installed kiln. **Separately,
  per §0.0: both the retracted 7.8% IAE figure and the 0.011 degC MAE gap
  that replaced it are aggregate metrics over an objective (overshoot) the
  owner has since split into four.** Neither number says anything about
  which of the four objectives moved, or in which direction, or whether any
  trade occurred between them. This evidence needs re-scoring on
  `firing_score.c`'s per-objective subscores before it can support or
  weaken any option here — it is not simply superseded by the retraction,
  it was never measuring the right thing even before the retraction.
- **A completed single-zone hardware A/B at `strength_pct > 0` shows a tracking
  improvement beyond the 0.5 degC floor** (`feedback_ignore_sub_half_degree_effects`)
  then fuzzy has demonstrated value for the first time; reopen (i)/(ii) properly.
- **The feedforward hold term is fixed and a Ki schedule is then shown
  unnecessary** (`high_temperature_transfer_analysis:356`) then (iii)'s payoff drops
  and the ranking between (iii) and (iv) collapses toward (iv).
- **The owner decides the fuzzy layer must ship as a working feature** — this is
  a product decision that overrides the engineering ranking; the correct path is
  then (ii) + (v-a), not (i).
- **`adaptive_tune` is turned on and run on this board, and/or its ratchet
  defect fix (in progress, `655da406` §5) lands** — either changes finding (B)
  from "satisfied in code" to "satisfied and observed," which strengthens (iv)
  and weakens the case for spending fuzzy-side effort anywhere near
  bootstrap/continual-adaptation territory. Confirm the fix actually landed
  (do not assume from this plan) before citing it as done.

---

## 5. Sequencing and gates

Each stage names its entry condition and its **stop** condition. Nothing here
requires kiln time until stage 4, and stage 4 is not reached by this plan.

**Stage 0 — (v-a) offline rule-cell probe.** *Entry:* none. Feed synthetic
error/rate spanning all 9 cells through `pid_fuzzy_drift_harness.c`; record the
gain triple per cell at strength 50. This is pure math with no plant, so the
refuted coupling model and the bench's range are both irrelevant. *Deliverable:*
a 9-row table in an audit doc. *Stop:* none — this cannot fail, only inform.
**DONE, 2026-09-13 (`ed854ac5`)** — centre cell's exact multiplier recorded
(kp x0.75/ki x1.25/kd x0.75 at strength 50); its reachability-count claim was
found unsupported by a same-day review (`8a12521b`) and must not be cited —
see the plan header update and §4.3. A follow-on sim comparison beyond this
stage's original scope (`ba230bca`) also ran, was later retracted by the same
review, and is recorded in the same places.

**Stage 1 — (v-b) fix finding (D).** *Entry:* stage 0 filed. Align or explicitly
justify the fuzzy error axis vs `z->effective_target_c`. *Stop:* if aligning
changes behaviour for any zone with a non-zero `approach_rate_cap_c_per_hr`, stop
and escalate — that is a behaviour change, out of scope here.
**DONE, already landed 2026-09-11 (`7d76d8fc`), predating this plan's own §8
schedule** — verified 2026-09-22 against `origin/main`:
`pid_fuzzy_prepare_gains()` (`profile_executor_pid_tick.c`) computes
`error_c` from `zone_commanded_setpoint_c(z, zi)`, the same helper
`pid_family_zone_tick()`'s `pid_update_terms()`/`zone_feedforward()` calls use,
rather than the shared `s_exec.target_c` — no second call site left to drift.
All three live zones still read `approach_rate_cap_c_per_hr == 0.0`, so the
entry stop-condition (no live behaviour change at cap 0) was and remains
satisfied; the sibling `seed_bumpless_with_ff()` instance of the same bug was
fixed in the same commit. Regression coverage:
`test_profile_executor_prestart.c` calls the real `pid_fuzzy_prepare_gains()`
with a configured cap and a diverged `effective_target_c` and checks its
output against two direct `pid_fuzzy_adjust()` calls (correct-error vs.
old-wrong-error), proving both the cap-0 no-op and the nonzero-cap coupling;
negative-tested by hand at the time (revert, rebuild, 4 checks fail, restore,
forced rebuild, 4949/4949 passes). No further code or test change made by
this pass.

**Stage 2 — decision point.** *Entry:* stages 0-1 complete. Re-run §4.3's
criteria against stage 0's table and put the (iii)-vs-(ii)-vs-(iv) choice to the
owner. **Everything below is conditional on this gate.**

**Stage 3 — sim, single zone only.** *Entry:* stage 2 chose (ii) or (i).
Requires the concurrently-built single-zone closed-loop fuzzy harness. The
harness must **print and assert its own `control_mode` and `fuzzy_strength_pct`
per run** — scoping §6.1's trap, which has already produced one withdrawn claim.
*Stop:* if the harness cannot demonstrate a configuration where fuzzy and base
PID diverge measurably, there is nothing to tune; return to stage 2.

**Stage 4 — hardware, single zone, one arm at a time.** *Entry:* stage 3 produced
a screened candidate. Not authorised by this plan; needs its own owner sign-off.
*Blocked throughout:* any multi-zone or joint-dwell fuzzy work, while multi-zone
superposition is refuted — sim cannot screen it and hardware would fit fuzzy to
the coupling residual (scoping §5, §6.2).

**If (iii) is chosen at stage 2**, its sequencing is owned by
`high_temperature_transfer_analysis_2026-09-08.md` §4, not by this plan, and its
own first gate is the feedforward hold term.

---

## 6. Risk register

Drawn from this repo's documented bug classes; only those that apply *to this
work specifically* are listed.

| Risk | Why it applies here | Guard |
|---|---|---|
| **Consumer without producer** (`project_consumer_without_producer_class`) | Any confidence/adaptation field added to `zone_cfg_t` is a reader with no writer until the whole pipeline ships; host tests would supply it by hand and pass | Do not add a schema field until its writer lands in the same commit. Under (v) no field is added at all |
| **Reset one side of a pair** (`project_reset_one_side_bug_class`) | `z->fuzzy_prev_effective_ki` is reset on mode change/reseed alongside `pid_state.integral` — two pieces of state joined only by a comment (`profile_executor_pid_tick.c:366-374`). Adding adapted bands or per-cell confidence adds more such pairs | Any new adapted state must be reset by the same function that resets what it derives from. Review by hand — the class has no mechanical check |
| **Bound justified against a test constant** (`project_bound_relative_to_persisted_state`) | Scoping §8.3 correctly identifies this for band adaptation. It applies identically to (iii)'s schedule parameter | If anything adaptive ships, anchor bounds to the last **full autotune**, stored separately from the adapted value; never to the running value |
| **Persisted baseline that ratchets** (same memory entry, `project_self_referential_rested_check`) | Monotonically-rising confidence is this shape exactly | Under the recommendation, nothing persists and ratchets — a further argument for (v)/(iii) over (i) |
| **Negative test on a mirror is vacuous** (`project_negative_test_on_a_mirror_is_vacuous`) | `fuzzy_gain_mirror_drift_check.py` is a Python mirror of `pid_fuzzy.c`. Breaking the mirror to prove a check fires proves nothing | Negative-test by breaking `pid_fuzzy.c` itself, then **restore by hand** and prove with an empty `git diff` — never `git checkout --` (`feedback_negative_test_restore_by_hand`) |
| **Idealised test input hides branches** (`project_idealized_test_input_bug_class`) | Stage 0 feeds synthetic error/rate by design. Unquantized synthetic input will exercise cells the 5 s / quantized real sampling never reaches | Stage 0's table is a statement about the *math*, not about reachability. Label it so; do not let a cell "exercised in the probe" count as evidence for a confidence scheme |
| **Paired input left shared** (`project_paired_input_left_shared`) | Finding (D). **Fixed** `7d76d8fc` (2026-09-11, before Stage 1 was scheduled here) — `pid_fuzzy_prepare_gains()` now calls the same `zone_commanded_setpoint_c(z, zi)` helper `pid_family_zone_tick()` uses | Closed — see Stage 1 |
| **Bypassed owner module** (`project_bypassed_owner_module_bug_class`) | A second gain-adaptation mechanism beside `adaptive_tune` (finding B) is this class applied to gains | Single-writer rule, criterion 5. Extend `adaptive_tune`; do not parallel it |
| **A sim win read as a hardware result** (scoping §6.2) | The single-zone harness is trustworthy for *ranking*, not for absolute degC | Every sim claim must be written as "under `sim_plant.c`'s model", and multi-zone claims not made at all |
| **`abs_max_temp_c` on the Pico** (`feedback_abs_max_same_or_looser`) | Nothing in this plan touches safety-processor state | Stated for the record; no work item |

---

## 7. Out of scope

- **Multi-zone or joint-dwell fuzzy evaluation** — blocked while superposition
  is refuted (`coupling_sign_reversal_artifact_test_2026-09-11.md`). Not a
  scheduling choice; sim cannot honestly screen it.
- **Changing the rule table's shape (bucket/cell count).** §3.
- **Extending `iter_tune` to carry fuzzy parameters.** §3; scoping §4.
- **Fixing the feedforward hold term.** A prerequisite for (iii), owned by
  `high_temperature_transfer_analysis_2026-09-08.md`, not by this plan.
- **Building the single-zone closed-loop fuzzy harness.** Owned by a concurrent
  pass; this plan consumes it at stage 3.
- **Any change to `abs_max_temp_c` or safety-processor behaviour.**
- **Actually deleting the fuzzy layer.** Option (iv) is argued, not scheduled —
  removing a selectable control mode is an owner decision.

---

## 8. Effort and payoff, honestly

| | Effort | Payoff |
|---|---|---|
| (v) recommended | Hours. No kiln time, no schema change, no behaviour change | Settles the central question; fixes one latent defect |
| (iii) | Days, plus the feedforward prerequisite | Addresses the ~20x effect that dominates a real kiln. One parameter, defaults inert |
| (ii) | Days, plus the sim harness | Could fix finding (A) properly, but the fit is to the simulator |
| (i) as scoped | Weeks. ~33 adapted values, 2 schema bumps, a multi-firing sim harness, a confidence estimator, suppression and revert paths | Uncertain. Does not address finding (A); 8 of 9 confidence values cannot be evidenced on this fixture |
| (iv) | Days of deletion | Removes complexity; forecloses an untested option |
| `b` sweep (§0.0.2, new) | Hours, single-zone sim only | Cheapest candidate for objective 4 (overshoot); NOT YET TESTED; does not compete for effort with (i)-(v) since it touches neither `pid_fuzzy.c` nor the rule table |

**The honest conclusion on (i): the expected gain does not justify the
complexity.** It is the largest body of work on the table, its headline
deliverable does not move the only number ever measured (finding A), and its
second half (per-cell confidence) requires evidence this installation
structurally cannot produce. That is a judgement about *this* work against *this*
evidence, not a claim that adaptive fuzzy control is a bad idea in general — and
§4.3 says precisely what would overturn it.

The broader honest conclusion: **the fuzzy layer is not where this project's
remaining control performance is.** A +-25% nudge around autotune's gains cannot
reach a 20x change in plant gain, and the one mechanism in the tree that already
adapts gains from measured plant data (`adaptive_tune`) does not go through it.
The cheapest useful thing to do with the fuzzy layer is to measure it once
(stage 0), fix its one latent input defect (stage 1), and leave it disarmed.
