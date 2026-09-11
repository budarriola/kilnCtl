# Fuzzy controller plan (2026-09-11)

Companion to `docs/audits/fuzzy_controller_improvement_scoping_2026-09-11.md`
(commits `7e669c18`, `90abaf5f`, `0551d6c7`). That document establishes what the
fuzzy layer *is* and what evidence exists; **this document does not restate it.**
It weighs architectural options the scoping pass did not, and **disagrees with its
recommendation** (§2, §4).

No controller behaviour is changed by this document. No board was flashed and no
heating run was performed. Board facts are live `kiln_call` reads.

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

**Adopt (v) now; on its results, expect to land on (iii) as the answer to the
owner's four requirements, with (iv) as the honest disposition of the fuzzy layer
some cycles later.** Do not adopt (i).

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
   scoping doc designs a second such mechanism (§8.2) without reference to it.
   Any continual-adaptation work must extend `adaptive_tune`, not sit beside it.
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
  substantially more attractive, and (ii) becomes the lead option.
- **A completed single-zone hardware A/B at `strength_pct > 0` shows a tracking
  improvement beyond the 0.5 degC floor** (`feedback_ignore_sub_half_degree_effects`)
  then fuzzy has demonstrated value for the first time; reopen (i)/(ii) properly.
- **The feedforward hold term is fixed and a Ki schedule is then shown
  unnecessary** (`high_temperature_transfer_analysis:356`) then (iii)'s payoff drops
  and the ranking between (iii) and (iv) collapses toward (iv).
- **The owner decides the fuzzy layer must ship as a working feature** — this is
  a product decision that overrides the engineering ranking; the correct path is
  then (ii) + (v-a), not (i).

---

## 5. Sequencing and gates

Each stage names its entry condition and its **stop** condition. Nothing here
requires kiln time until stage 4, and stage 4 is not reached by this plan.

**Stage 0 — (v-a) offline rule-cell probe.** *Entry:* none. Feed synthetic
error/rate spanning all 9 cells through `pid_fuzzy_drift_harness.c`; record the
gain triple per cell at strength 50. This is pure math with no plant, so the
refuted coupling model and the bench's range are both irrelevant. *Deliverable:*
a 9-row table in an audit doc. *Stop:* none — this cannot fail, only inform.

**Stage 1 — (v-b) fix finding (D).** *Entry:* stage 0 filed. Align or explicitly
justify the fuzzy error axis vs `z->effective_target_c`. *Stop:* if aligning
changes behaviour for any zone with a non-zero `approach_rate_cap_c_per_hr`, stop
and escalate — that is a behaviour change, out of scope here.

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
| **Paired input left shared** (`project_paired_input_left_shared`) | Finding (D), live today and inert only because all three caps read 0.0 | Stage 1 |
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
