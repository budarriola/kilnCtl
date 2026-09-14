# Operating-point gain scheduling: a concrete design (2026-09-13)

Follow-on to `docs/FUZZY_CONTROLLER_PLAN.md` (`5a16e950`), which weighed five
options for the fuzzy/gain-adaptation question, recommended its staged option
(v), and stated it expects to land on option (iii) — replace the fuzzy layer
with operating-point gain scheduling — once (v)'s cheap prerequisites are
done. This document takes option (iii) from "expected direction" to a
concrete, reviewable design, per that plan's own citation of
`docs/audits/high_temperature_transfer_analysis_2026-09-08.md:342-359`.

No controller behaviour is changed by this document. No board was flashed,
no heating run was performed, and no code in this repository was edited —
this is design and documentation only, per this task's scope.

---

## 1. What the `zone_model_at()` / `coupling_at()` seam actually gives you

Read directly from commit `5d3bc854` and current source
(`firmware/KilnFW/App/drivers/persist/zones_config_accessors.c`,
`zones_config_accessors.h:661-674`), not from the plan's summary.

**What exists today:**

- `zone_model_at(zone_index, T_c, out_k_dc, out_tau_s, out_dead_time_s)` and
  `coupling_at(zone_index, T_c, out_row)` — two functions that accept a
  temperature argument and **ignore it**. Each is a byte-for-byte passthrough
  to `zones_config_get_model()` / `zones_config_get_coupling()`
  (`zones_config_accessors.c:1494` region, the `(void)T_c;` casts are
  explicit in the diff). No interpolation, no lookup table, no schedule
  logic exists anywhere in this codebase today.
- Two live call sites were converted to go through the seam instead of the
  raw getters: `profile_executor_feedforward.c`'s `zone_load_model()` (model
  read) and `zone_feedforward()` (coupling read), and
  `profile_feasibility.c`'s `segment_verdict()` / `effective_k_dc()` (both
  reads). These are the only two files where a temperature-dependent
  schedule would need to be reached — everywhere else in the control path
  still calls the un-seamed getters directly and would need auditing before
  a schedule could be trusted to apply uniformly (see §5).
- The temperature value already flowing into the seam at each site is the
  right one for a schedule: `zone_load_model()` passes `z->actual_c`
  (current measured temperature), `zone_feedforward()` passes
  `s_exec.zones[zi].actual_c`, and `profile_feasibility.c` passes
  `start_c`, this segment's own starting temperature — none of them pass
  setpoint. That matches recommendation 3 of the prior design (schedule on
  measured temperature, not setpoint) without any further plumbing.
- A place to *record* the operating point a model was fitted at:
  `zone_cfg_t::model_fit_temp_c` / `model_fit_ambient_c`
  (`ZONES_CFG_VERSION` 23→24), populated by
  `zones_config_set_model_fit_context()`, called from
  `autotune_engine_guard.c` immediately after a successful
  `zones_config_set_model()`, using the measured `m.baseline_c` /
  `step_ambient_c` from the same step test — never the setpoint. Sentinel
  `ZONE_MODEL_FIT_TEMP_UNKNOWN = -273.15f` marks "no context recorded"
  (every pre-v24 fit backfills to it). Getter:
  `zones_config_get_model_fit_context()`.

**What would have to be added, and is not there today:**

1. A schedule function — `s(T)` or equivalent — with actual math inside
   `zone_model_at()` / `coupling_at()` (or a new function they call), keyed
   on `T_c` and on the recorded `model_fit_temp_c` as the reference point
   `T_ref`. Today `T_c` is dead weight in both signatures.
2. A place to store the schedule's one free parameter per zone (a new
   `zone_cfg_t` field, another `ZONES_CFG_VERSION` bump, following exactly
   the same pattern `5d3bc854` just used for `model_fit_temp_c`) or a single
   board-wide constant if per-zone variation is judged unnecessary.
3. A software implementation of `loss_conductance_scale()` — it exists only
   PC-side today, in `tools/PcTools/src/kilnctrl/plant_sim.py:268-290`, with
   nothing in `firmware/KilnFW` (confirmed: no `loss_conductance` symbol
   anywhere under `firmware/`). Porting it to C, at minimum re-deriving
   `s(T)` in a form embeddable inside `zone_model_at()`.
4. Every call site the schedule needs to reach that currently bypasses the
   seam. `grep -rn "zones_config_get_model\|zones_config_get_coupling"
   firmware/KilnFW/App` beyond the two files above would need to be re-run
   at implementation time — this task is documentation-only and does not
   claim to have enumerated them exhaustively.

Net: the seam is real, live, and already carries the right variable to the
right call sites — that part of the prior design's "cheap seam" claim holds.
Everything that would make it *do* anything is unwritten.

---

## 2. The schedule itself

**Proposed functional form** (from
`high_temperature_transfer_analysis_2026-09-08.md:342-359`, restated and
evaluated here rather than assumed):

```
ki(T) = ki_ref * s(T) / s(T_ref)
```

where `T_ref = model_fit_temp_c` (the zone's own recorded fit operating
point — now available per §1), `ki_ref` is the Ki that autotune/adaptive_tune
already computed at that point, and `s(T)` is a radiative-loss-conductance
scale factor, `loss_conductance_scale()`, whose one free parameter is a
radiative-loss fraction at the reference temperature (`f_rad`, called
`RAD_LOSS_FRACTION_AT_REF` in the PC-side implementation).

**Is Ki the right gain to schedule?** The dimensional argument for scheduling
Ki rather than Kp is that the plant's steady-state gain `k` and time constant
`tau` move together (both fall ~20x from bench to ~1200 C,
`high_temperature_transfer_analysis_2026-09-08.md:115-116`), which is exactly
the pair that determines integral time in a SIMC-style tuning — the
proportional term's job (react to present error against present `k`) does
not need retuning by the same factor if `k` and `tau` co-vary as the analysis
claims, while the integrator's job (accumulate error against a dead time that
is shrinking 15x alongside a `k` that is growing 20x) is exactly where a fast
integrator tuned for a slow, low-gain bench plant becomes dangerously fast
for a high-gain, low-dead-time regime — the analysis's own words: "An
integral time of 11 s against a 167 s plant with L=41 s is roughly 4x shorter
than the dead time — that loop is not merely sloppy, it is a genuine
oscillation risk" (`high_temperature_transfer_analysis_2026-09-08.md`, the
section immediately preceding line 320). Not scheduling Kp is a claimed
simplification, not a proven one — this design inherits it from the prior
analysis without independent re-derivation, since re-deriving the SIMC
sensitivity of Kp to `k`/`tau` co-variation is outside what this bench, or
this task, can check.

**Is temperature the right scheduling variable?** The analysis considered and
rejected the alternatives, and this design accepts that reasoning:

- **Setpoint** — rejected because it injects a gain discontinuity at every
  profile segment boundary, exactly where the trajectory is already
  changing; a schedule keyed on setpoint would add a step change in loop
  dynamics on top of a step change in reference, compounding rather than
  isolating the two.
- **Estimated loss** — rejected because it is an unobserved derived
  quantity on this board. `project_load_estimator_cannot_observe_load`
  records all three zones closing negative on precisely this kind of
  attempt; there is no independent load/loss observer to feed a schedule
  from.
- **Measured temperature** — accepted: directly sensed, already filtered
  (it is the same `actual_c` value every other control-path consumer uses),
  and it is the physical driver of `s(T)` in the first place (radiative loss
  scales with T^4, sensed temperature is the input radiation depends on).

This design does not manufacture new confidence here — it is inheriting the
prior analysis's elimination of the two alternatives rather than
re-deriving it, because this bench cannot generate the high-temperature data
that would let it be checked independently (§3, §6).

**Dimensional justification.** `ki` has units of duty/(degC*s); `s(T)` is
dimensionless (a ratio of loss conductances, or equivalently a ratio of
plant gains at two operating points), so `ki(T) = ki_ref * s(T)/s(T_ref)`
preserves `ki`'s units for any value of the ratio — the schedule cannot
introduce a unit error regardless of `f_rad`'s value, which is a structural
safety property distinct from whether `f_rad` itself is correct (§3).

---

## 3. The parameter problem, stated honestly

**What value is assumed, and on what basis.** The PC-side reference
implementation's `RAD_LOSS_FRACTION_AT_REF` is explicitly marked ASSUMED
(`docs/FUZZY_CONTROLLER_PLAN.md`'s own citation, confirmed against
`tools/PcTools/src/kilnctrl/plant_sim.py:268-290`) — it is a physically
motivated guess (radiative loss growing as a fraction of total loss as T^4
overtakes convective/conductive loss with rising temperature) with no
measurement behind the specific numeric value on this or any kiln.

**What a wrong value does.** This is the question that decides whether the
feature is shippable at all, and the schedule's own algebra answers it
favorably: `s(T)/s(T_ref)` is a dimensionless multiplier on `ki`, evaluated
between two points (`T_ref`, the recorded fit temperature, and `T`, the
current measured temperature) that at this fixture's operating range differ
by at most the bench's own ~40 degC ceiling
(`project_bench_is_a_4w_test_fixture.md`). Whatever functional form `s(T)`
takes, a wrong `f_rad` changes *how much* the ratio moves away from 1 as `T`
diverges from `T_ref`, not *whether* it is 1 at `T = T_ref`, and not the sign
of the direction it moves for physically-reasonable `f_rad` in `[0, 1]` (loss
increasingly dominated by radiation as temperature rises means `s(T)` is
monotonically increasing in `T` for any `f_rad > 0`, so `ki` only ever moves
downward as temperature rises above `T_ref` regardless of the specific
value chosen — a *smaller* integral gain at higher temperature, which is the
direction the stability analysis in §2 calls for, not the opposite). A wrong
`f_rad` therefore degrades the *size* of the correction, plausibly under- or
over-correcting Ki's reduction at high temperature, but does not by itself
flip the schedule's sign or destabilize a loop that was stable without it —
**provided** `T_ref` is correctly the zone's own fit temperature (already
guaranteed structurally by using `model_fit_temp_c`, not a guessed
constant) and provided the schedule is bounded (a clamp on `s(T)/s(T_ref)`,
analogous to fuzzy's own `MAX_NUDGE_FRACTION` bound, is not in the prior
design and should be added — see §7's proceed-conditions). Without a bound,
a sufficiently large `f_rad` evaluated at a sufficiently large `T` could
still drive `ki` toward zero or negative in principle; a floor/ceiling on
the ratio closes that off cheaply and is the one addition this design makes
beyond the prior analysis's text.

**Does a defensible, inert-by-default value exist?** Yes, and it is the
natural one: `f_rad = 0` (or equivalently, gate the whole schedule behind a
per-zone enable flag defaulting off, mirroring `adaptive_tune_enabled` and
`fuzzy_strength_pct`'s own precedent of shipping a mechanism disarmed). At
`f_rad = 0`, `s(T) ≡ 1` for all `T` (loss entirely non-radiative in the
model, the bench's own regime), so `s(T)/s(T_ref) = 1` identically and
`ki(T) = ki_ref` — **bit-identical to today's un-scheduled behaviour** (this
mirrors `zone_model_at()`/`coupling_at()`'s own current passthrough
contract: T accepted, ignored, output unchanged). This satisfies the owner's
standing requirement that nothing ship tuned to this fixture: the schedule
can land in firmware, be host-tested, and sit in the shipped build with zero
behavioural change on this or any bench until a real kiln's owner supplies a
measured or even estimated `f_rad` and switches the zone's flag on. This is
a stronger inert-by-default story than fuzzy's own `strength_pct = 0`
precedent, because fuzzy's rule table (`RULE_TABLE[1][1]`'s centre-cell
detune, `pid_fuzzy_nine_cell_offline_probe_2026-09-11.md`) is a **behaviour
that exists and is merely gated to 0% strength** — a config error that
un-gates it produces immediate, measured, non-zero rescaling. This
schedule's inert value is the mathematical identity of the schedule itself,
not a separately-maintained gate around a live rule table — there is no
analogous "un-gate and something non-trivial happens even at f_rad's default
value" failure mode, because the default value of the parameter itself,
not a separate switch, is what makes it inert.

**What measurement would identify the parameter.** A real kiln capable of
reaching the regime the schedule targets (order 800-1200 C, per
`high_temperature_transfer_analysis_2026-09-08.md`'s own recommendation to
identify "around 800-1000 C" rather than at the very top) run through the
step-identification protocol that analysis already specifies (§5 of that
document: step downward at high temperature, since available negative step
`u_hold` is large exactly where the positive one is headroom-starved; and
fit on the way up rather than at the top). Two or more `k_dc`/`tau_s` fits
recorded at different temperatures (both already carried through
`model_fit_temp_c` per §1) give two points on `s(T)`, from which `f_rad`
can be solved directly rather than assumed — this is the one piece of new
infrastructure `model_fit_temp_c` was added for, and it is not usable for
this purpose until a kiln reaching that range performs at least two fits at
different operating points.

---

## 4. Comparison against keeping fuzzy

The two mechanisms are being asked to do different jobs at different scales,
which is itself the first finding: fuzzy's own probe
(`docs/audits/fuzzy_nine_cell_offline_probe_2026-09-11.md`) measured a rescale
in the +-25/+-50% band around a fixed operating point; the schedule targets a
~20x change in plant gain/time-constant across the full firing range
(`high_temperature_transfer_analysis_2026-09-08.md:115-116`). They are not
substitutes solving the same problem at different fidelities — they are
aimed at effects of different magnitude, and only one of those magnitudes
(fuzzy's) is observable on this bench.

**Measured facts about fuzzy, restated for the comparison, not re-derived
here:**

- Fuzzy is strictly per-zone: its inputs are the zone's own `error_c` and
  `d_filtered` only, with no neighbour-zone state, by deliberate design
  (`pid_fuzzy.h:23-32`, cited in `docs/FUZZY_CONTROLLER_PLAN.md` §0).
- Of the 9 rule cells, 6 (every cell with a non-zero rate bucket) are
  reachable only under disturbance, not ordinary operation, on this plant:
  reaching a FALLING or RISING rate bucket needs `|rate| >= 0.5 degC/s`
  against a measured maximum real ramp rate of ~0.083 degC/s — roughly 6x
  too slow to leave the STEADY bucket
  (`docs/audits/fuzzy_nine_cell_offline_probe_2026-09-11.md`, "Reachability
  verdicts" section). The remaining 3 cells (both error extremes at STEADY
  rate, plus the centre cell) are reachable in ordinary operation, but only
  the centre cell has ever actually been observed firing: 100% of the one
  real mode-3 hardware capture's 2178 zone-samples landed there
  (same document, "Centre cell" section).
- At strength 50, fuzzy measurably beats an equivalent fixed-gain retune:
  IAE 11796.0 vs 12721.1, MAE 2.8356 vs 3.0579
  (`docs/audits/fuzzy_vs_fixed_retune_comparison_2026-09-11.md`, the "(b)
  vs (c) decisive comparison" — about 7.8% worse IAE and a +0.2223 degC MAE
  gap for the fixed-gain arm). This is a real, measured result: fuzzy's
  inference contributes something beyond a static rescale of the same
  magnitude.
- **Every MAE gap measured in that comparison sits at or below this
  project's 0.5 degC materiality line** (`feedback_ignore_sub_half_degree_effects`):
  0.4696 degC against the fully-fixed baseline, 0.2223 degC in the decisive
  (b)-vs-(c) comparison, and 0.08 degC in a third arm noted in the same
  document. The 7.8% *relative* IAE gap looks larger only because IAE
  integrates the same per-tick gap over ~2800+ ticks — the document itself
  flags this as a run-length artefact, not a magnitude one, and states MAE
  is the metric to judge by for exactly this reason.

**Judging fairly.** Gain scheduling is not "better" than fuzzy on any
evidence this bench can produce, because the two are not being evaluated on
the same axis: fuzzy's demonstrated effect is a small, real, sub-materiality
improvement measured *on this bench, at this bench's temperature range*, from
a mechanism (Mamdani inference over a hand-authored rule table) whose
worst-case behaviour (the +-50% `MAX_NUDGE_FRACTION` bound) is bounded and
board-tested. Gain scheduling's motivating effect — the 20x k/tau shift — is
*by construction* outside what this bench can produce or measure at all
(§3), so it has no comparable "measured on this bench" data point, favourable
or unfavourable, to weigh against fuzzy's. The honest comparison is: fuzzy
has weak, sub-materiality, bench-measured evidence of a small positive
effect; gain scheduling has a physically-motivated design with no bench
evidence either way, because the bench cannot reach the regime the design
targets. Recommending one over the other on "which works better" is not
answerable from anything measured to date — the two proposals answer
different questions (a small steady-state rescale near the current
operating point, versus a large excursion the operating point itself moves
through on a real kiln) and the data available characterizes only the first
question.

---

## 5. Interaction with `adaptive_tune` and fuzzy

Three mechanisms would then touch the same per-zone PID gains:

- **`adaptive_tune`**: writes persisted `zone_cfg_t` gains at run-end (after
  relays are off), refining `K_dc` from settled dwells via the same SIMC
  path autotune's Accept uses (`docs/FUZZY_CONTROLLER_PLAN.md` §0.1(B)).
  This changes the *stored* `ki_ref` a schedule would read.
- **Fuzzy**: rescales `kp`/`ki`/`kd` per control tick, multiplicatively,
  bounded by `MAX_NUDGE_FRACTION`. This happens after gains are read for the
  tick, ahead of `pid_update_terms()`.
- **The proposed schedule**: would rescale `ki` specifically, keyed on
  measured temperature, evaluated wherever `zone_model_at()` /
  `coupling_at()` are consulted for feedforward and feasibility math today
  — but the schedule as designed in §2 operates on the *model* Ki
  (`k_dc`/`tau_s`-derived), not directly inside the PID tick's own gain read
  path (`pid_update_terms()`), which is a different call site from the two
  the seam currently reaches (§1). Making the schedule affect the PID
  loop's actual `ki` (not just the feedforward/feasibility math the seam's
  two current callers use) would require a **third** call site conversion
  not covered by `5d3bc854` at all — the PID gain read in
  `profile_executor_pid_tick.c` reads `zone_cfg_t`'s stored `ki` directly,
  through neither `zone_model_at()` nor `coupling_at()`. This is a gap
  between "the seam this design would use" and "the value the schedule is
  actually meant to change" that the prior analysis's text does not
  resolve, and this document does not resolve it either — it is listed here
  as an open question for whoever implements this, not glossed over.

**Composition, assuming that gap is closed:** the three would compose as
`ki_effective = fuzzy_rescale(schedule(ki_ref, T), error_c, rate)` — schedule
first (a slow, temperature-keyed adjustment to the *reference* gain
`adaptive_tune` last wrote), fuzzy second (a fast, per-tick multiplicative
nudge around whatever the schedule currently outputs). This ordering is
consistent with `pid_rescale_integral_for_new_ki()`'s existing job of
transferring the integral term's accumulated bump when `ki` changes
underneath it — that mechanism already exists to handle `ki` changing
between ticks (today, from `adaptive_tune`'s run-end write); a
temperature-keyed schedule changing `ki` *within* a run at a similar or
higher frequency is a materially different load on that mechanism than the
once-per-run change it was built for, and would need re-validation for
tick-rate churn, not just run-boundary churn.

**Must any pair be mutually exclusive?** Not structurally — each has exactly
one writer if scoped as described (schedule modifies a transient "current"
`ki` derived from the stored reference; fuzzy modifies the tick's applied
gain; `adaptive_tune` modifies the stored reference at run boundaries only)
— but this satisfies criterion 5 of `docs/FUZZY_CONTROLLER_PLAN.md` §1
("exactly one writer per piece of state") only if the schedule is
implemented as a read-time transform and never itself persists a rescaled
`ki` back into `zone_cfg_t`. That constraint should be stated explicitly in
any implementation, not left implicit.

**Today's dormant state versus the designed state.** `adaptive_tune` is
disabled on all three zones (`observations_lifetime = 0`, live read cited in
`docs/FUZZY_CONTROLLER_PLAN.md` §0.2) and `fuzzy_strength_pct = 0.0` on all
three zones (`project_fuzzy_ab_inert_control_mode.md`, superseded live
2026-09-11 per that plan's §0). Nothing conflicts today because neither
existing mechanism is active — that is a fact about the current bench
configuration, not evidence the three would compose safely once all three
are live, which is the case this section is meant to address rather than
assume away.

---

## 6. Validation path

**Multi-zone simulation is not a valid venue.** The coupling model has been
refuted, and two separate replacement attempts (`9f054181`, and a second,
uncommitted attempt recorded in
`docs/audits/coupling_level_schedule_adjudication_2026-09-11.md` §9) both
failed — that document's §2 finding is that the schedule under test there
was "never evaluated in its calibrated range," a modelling failure mode a
gain-scheduling validation would risk repeating if run against the same
multi-zone sim. Any result produced there would carry the same credibility
discount `docs/FUZZY_CONTROLLER_PLAN.md` already applies to option (ii)'s
sim-fit proposal ("a sim win is screening, not evidence of hardware
improvement," §2(ii)).

**Single-zone simulation is trustworthy**, because single-column thermal
transport has been measured linear
(`project_coupling_failure_is_joint_dwell_specific.md` — buoyancy refuted on
hardware, single-column transport is linear) — this is the basis
`docs/FUZZY_CONTROLLER_PLAN.md` criterion 4 already uses to justify
single-zone-only validation for any of these options.

**What would actually be run, single-zone:**

1. Implement `s(T)` in `sim_plant.c` (the same host-linkable simulator
   `docs/FUZZY_CONTROLLER_PLAN.md` §2(ii) already treats as the trustworthy
   single-zone venue), parameterised by `f_rad`, and drive it through a
   `k`/`tau` schedule matching `high_temperature_transfer_analysis_2026-09-08.md`'s
   ~20x central estimate across a temperature sweep the sim can represent
   (the sim is not bounded by this bench's ~40 degC ceiling — it is a model,
   not a physical fixture).
2. Run the same single zone with the schedule off (`f_rad = 0`, today's
   behaviour) and on, at several assumed `f_rad` values spanning a
   physically plausible range (e.g. 0.02-0.20), against the same profile
   the high-temperature analysis's own step-identification recommendation
   targets (identification around 800-1000 C, not at the very top).
3. Compare closed-loop stability margin (not IAE/MAE — §4 already
   establishes MAE is the discriminating metric at bench scale, but at the
   scale this schedule targets the relevant question is whether the loop
   stays stable at all, not by how much it improves a settled trace) between
   scheduled and unscheduled `ki` at the high-temperature end of the sweep,
   where the un-scheduled integral time is closest to the dead time
   (`high_temperature_transfer_analysis_2026-09-08.md`'s own "roughly 4x
   shorter than the dead time" finding is the specific instability this
   schedule is meant to prevent).

**What result would justify proceeding to real-kiln implementation:** the
schedule needs to demonstrably restore an integral-time-to-dead-time margin
comparable to the bench's own margin, at the sim's high-temperature end,
across the plausible `f_rad` range in step 2 — i.e. the schedule should
still help (or at minimum, not destabilize) even when `f_rad` is wrong by a
factor the range in step 2 represents. If the sim shows the schedule only
helps for a narrow band of `f_rad` values and destabilizes the loop outside
it, that is grounds to defer (§7) rather than ship an inert-by-default
feature whose *non-inert* setting is fragile to the one unmeasured
parameter it depends on. This sim work does not require the feedforward
hold-term fix the prior analysis calls a prerequisite (§2's recommendation
4) to be evaluated in isolation — the sim step here answers "is a Ki
schedule structurally stabilizing at the target regime," which is
independent of the feedforward artefact question, though implementation
order should still respect the prior analysis's sequencing before this
touches a real board.

---

## 7. Verdict

**Criteria, stated before the verdict** (adapted from
`docs/FUZZY_CONTROLLER_PLAN.md` §1, applied specifically to shipping this
design):

- **Proceed** only if: (a) an inert-by-default form exists that is
  bit-identical to current behaviour when unparameterised — confirmed true
  in §3; (b) the schedule can be validated somewhere trustworthy before
  touching a real board — confirmed achievable single-zone, sim-only, in
  §6; (c) the schedule composes with `adaptive_tune` and fuzzy without a
  second writer on the same state — conditionally true in §5, contingent on
  an unresolved implementation gap (the schedule's current seam does not
  reach the PID tick's actual `ki` read); (d) a wrong parameter value fails
  toward *less* correction, not toward instability — confirmed structurally
  true in §3 for `f_rad` in the physically-reasonable range, given a bound
  is added.
- **Defer** if any of (a)-(d) is unresolved in a way that only a real kiln,
  not more design work, can close — which is the case here: the schedule's
  entire motivating effect is unmeasurable on this fixture (§3, §6), and the
  interaction gap in §5 (the seam not reaching the PID tick's live `ki`) is
  an implementation detail that can be closed on paper but not verified
  end-to-end without the same real-kiln access the parameter itself needs.
- **Drop** only if the motivating effect turns out not to exist, or an
  existing mechanism already covers it — neither is the case: the k/tau
  co-variation is independently documented
  (`high_temperature_transfer_analysis_2026-09-08.md:115-116`) and nothing
  else in this codebase addresses it (fuzzy operates at a ~25-50% scale on
  a bounded rule table, `adaptive_tune` refines `K_dc` but does not
  reschedule `Ki` against a moving operating point).

**Verdict: defer.** The design is sound as far as it can be checked from
this desk — inert-by-default is achievable and is a stronger safety story
than fuzzy's own gated-but-present rule table (§3); the schedule's sign is
structurally safe for any physically-reasonable parameter value (§3); the
comparison against fuzzy is not "gain scheduling wins," it is "the two
target different, non-comparable effects, one of which this bench can
measure and one of which it cannot" (§4). But it cannot be validated beyond
a sim exercise on this fixture, its motivating effect (the ~20x k/tau shift)
is entirely outside this bench's ~40 degC range, and closing the interaction
gap in §5 well enough to trust on a real board is itself something this
project has no venue to rehearse. This is the `docs/FUZZY_CONTROLLER_PLAN.md`
§1 criterion-6 case named explicitly in this task's brief: "defer — it
cannot be validated on this fixture and the effect it targets is outside
this bench's range" is the correct call here, not a failure to find
enthusiasm for the design.

**What would change this verdict:** access to a real kiln (or a
high-temperature test setup capable of exceeding several hundred degC) for
the single-zone sim-to-hardware step in §6, and/or completion of the
feedforward hold-term fix `high_temperature_transfer_analysis_2026-09-08.md`
already calls a prerequisite, which this document does not own and does not
claim is in progress or complete.

---

# Review, 2026-09-13 — adversarial; §3's "structurally guaranteed T_ref" and §3's justification for the bound are REFUTED, §2's "not independently re-derived" is unnecessary (the SIMC derivation closes exactly), and §5's composition argument misses a real persisted-Ki ratchet

Adversarial review of `e9036127` by a separate agent. Design review only: no
code was edited, no board flashed, no heating run performed. Everything below
labelled "traced" was read in source at the cited path; everything labelled
"taken on trust" was not independently re-derived. One live read of the bench
board was performed (read-only, `GET /api/zones` and the kilnctrl MCP's
`control_get_zones`).

## R1. The seam-to-`pid_tick` gap is REAL — confirmed in source, and it is worse than §5 states

Traced. `profile_executor_pid_tick.c`'s live Ki is `z->pid_cfg.ki`
(`pid_fuzzy_prepare_gains()` line ~410: `float adj_kp = z->pid_cfg.kp,
adj_ki = z->pid_cfg.ki, ...`). `z->pid_cfg.ki` is written in exactly one
place in the whole control tree —
`profile_executor_config_reload.c:96-104`, from
`zones_config_get_pid(zi, &kp, &ki, &kd)`. That is the raw persisted gain.
Neither `zone_model_at()` nor `coupling_at()` is anywhere on that path.
§5's statement of the gap is correct.

Two corrections to §1/§5's surrounding detail, both traced:

- **`zone_model_at()` is now called from `profile_executor_pid_tick.c`
  already** — line ~384, `(void)zone_model_at(zi, z->actual_c, &model_k_dc,
  &model_tau_s, &model_dead_time_s)`, feeding `pid_fuzzy_derive_bands()`.
  So the seam physically reaches the PID tick's function; what it does not
  reach is the *gain*. That is a sharper statement of the gap than "a third
  call site conversion" — the call site already exists, the data just is not
  wired to Ki. (This call is another session's uncommitted working tree at
  review time, per `docs/audits/fuzzy_dimensionless_bands_2026-09-13.md`.)
- **"the two live call sites" is understated.** `autotune_engine.c:81`
  also calls `zone_model_at(zone_index, actual_c, ...)`, and has since
  `bd77ffd1` (2026-09-10) — i.e. before this document was written. §1's own
  caveat that it "does not claim to have enumerated them exhaustively" covers
  this, but the count in the text is wrong, not merely incomplete.

**The MEASURED-not-setpoint claim SURVIVES, and extends.** Traced at all
four sites: `profile_executor_feedforward.c:92` passes `z->actual_c`;
`:488` passes `s_exec.zones[zi].actual_c`; `profile_feasibility.c:193/250`
pass `t_c`/`start_c` (segment start temperature, not setpoint);
`autotune_engine.c:81` and `profile_executor_pid_tick.c:384` both pass
`actual_c`. No call site passes a setpoint. Recommendation 3 of the prior
design is satisfied by construction at every existing site.

**Effort consequence.** §7's estimate treats the gap as "an implementation
detail that can be closed on paper." It is not a seam conversion: the
`pid_cfg` path is a *cached copy* refreshed by `reload_zone_config()` only
when the persisted gains change, with a deliberate `pid_seed_bumpless()`
handshake on every change (`profile_executor_config_reload.c:91-110`). A
temperature-keyed transform must be applied per tick *downstream* of that
cache — i.e. inside `pid_fuzzy_prepare_gains()` alongside fuzzy, not at the
config read — or the cache and the schedule become a two-copy pair of exactly
the `project_paired_input_left_shared` shape. That is a different and larger
change than converting a getter call, and the document should say so.

## R2. Inert-by-default SURVIVES as a genuine bit-identity — with two conditions the document does not state

Worked through against the actual reference implementation
(`tools/PcTools/src/kilnctrl/plant_sim.py:268-290`):

```
s(T) = (1 - f_rad) + f_rad * ((T+273.15)/328.15)^4
```

At `f_rad = 0`: `cond_ref = 1.0f - 0.0f == 1.0f` exactly; `rad_now =
0.0f * x` is exactly `+0.0f` for every finite `x`, and `x = powf(ratio, 4)`
cannot be infinite for any representable temperature (overflow would need
`ratio > 1.36e9`, i.e. `T` above 4.4e11 degC), so the `0 * inf = NaN` path is
unreachable. `s = 1.0f + 0.0f == 1.0f` exactly, `s(T)/s(T_ref) == 1.0f/1.0f
== 1.0f` exactly, and `ki * 1.0f == ki` bit-for-bit in IEEE-754 for every
value of `ki` including zero, subnormals and signed zero. **The identity is
real, not a value that happens to work.** §3's claim stands.

Two conditions §3 omits:

1. It is an identity *of the multiply form only*. Any implementation that
   computes `ki_new = ki_ref * s(T) / s(T_ref)` as two separate operations
   is still exact here (division by exactly 1.0f is exact), but an
   implementation that instead re-derives Ki from scheduled `k_dc`/`tau_s`
   through `pid_autotune_tune_from_fopdt()` would NOT be bit-identical — it
   would round-trip the gain through SIMC and land on a nearby, different
   float. Since §5 leaves open which of those two shapes the implementation
   takes, "bit-identical to today" is only established for one of them.
2. `s(T_ref)` is a divisor. It is zero only at `f_rad == 1.0` with
   `T_ref == -273.15` — which is not hypothetical, see R3.

## R3. `model_fit_temp_c` reads the UNKNOWN sentinel on ALL THREE zones of this bench — §3's "already guaranteed structurally" is REFUTED

Live read, 2026-09-13, `GET http://192.168.1.156/api/zones`:

| zone | model_k_dc | model_tau_s | model_dead_time_s | model_fit_temp_c | model_fit_ambient_c |
|---|---|---|---|---|---|
| 0 | 42.731 | 255.6 s | 40.3 s | **-273.15** | -273.15 |
| 1 | 32.3969 | 258.9 s | 31.3 s | **-273.15** | -273.15 |
| 2 | 33.8493 | 247.1 s | 26.0 s | **-273.15** | -273.15 |

Every zone reads `ZONE_MODEL_FIT_TEMP_UNKNOWN`. All three models predate
`5d3bc854` and were backfilled to the sentinel by
`zones_config_migrate.c:1006`; no zone has been re-autotuned since, so the
field that the whole schedule is normalised against is empty across the
entire fixture. §3 says using `model_fit_temp_c` rather than a guessed
constant makes correct `T_ref` "already guaranteed structurally." **It does
not.** The one value it is guaranteed to hold today is the sentinel.

What the sentinel actually does, worked through rather than assumed — and
the answer is neither of the two the brief offered:

- **Not a divide-by-zero** (except at `f_rad == 1`). `s(-273.15) =
  (1-f_rad) + f_rad*0 = 1-f_rad`, a finite, plausible-looking 0.95 at the
  repo default.
- **Not a silent identity either.** It is a silent *wrong* schedule: every
  ratio is inflated by `1/(1-f_rad)`, and — the part that matters — the
  schedule then applies a non-unity correction at `T = T_fit`, the one
  temperature at which it is supposed to apply none.
- **Magnitude, stated honestly:** at `f_rad = 0.05` the error is a uniform
  ~3.6-5% on Ki at every temperature. That is small — arguably below the
  threshold this project cares about. The defect is therefore one of
  *silence and principle*, not of present magnitude: a sentinel meaning "no
  context recorded" is being consumed as a temperature of absolute zero,
  which is the sentinel-as-value class this repo has hit repeatedly, and it
  becomes singular as `f_rad -> 1`.
- `zones_config_json.c:380-382` explicitly whitelists the sentinel past the
  `[-50, 1300]` range validator, so nothing downstream stops it.

**Required, not optional:** the schedule must test
`T_ref == ZONE_MODEL_FIT_TEMP_UNKNOWN` and fall back to the identity. The
design does not mention this. Secondary finding: because the field is unset
on every zone, the "two fits at different temperatures give you `f_rad`"
path in §3 is not merely waiting on a hot kiln — it is waiting on the first
autotune run of any kind since `5d3bc854`, which is a cheap, bench-doable
prerequisite the document does not identify.

## R4. Scheduling Ki alone under SIMC is CORRECT — and §2's disclaimer is unnecessary

§2 declines to re-derive the SIMC sensitivity, calling it "outside what this
bench, or this task, can check." It is checkable, it is three lines, and it
comes out in the design's favour. From `pid_autotune.c:493-510` (traced, the
live SIMC branch of `pid_autotune_tune_from_fopdt()`):

```
lambda = 3L        (default, lambda_s = 0)
Kc = tau / (K*(lambda+L)) = tau / (4*K*L)
Ti = min(tau, 4*(lambda+L)) = min(tau, 16L)
Td = L/2
Kp = Kc,  Ki = Kc/Ti,  Kd = Kc*Td
```

With the analysis's own scaling (`K ∝ 1/s`, `tau ∝ C(T)/s`, `L` constant):

- **Kp**: `Kc = tau/(4KL) ∝ (C/s)/(1/s) = C(T)`. The `1/s` cancels
  **exactly**. "Do not schedule Kp" is not a simplification — it is the
  exact SIMC answer, up to the specific-heat factor alone.
- **Kd**: `= Kc*L/2 ∝ C(T)`. Same.
- **Ki**: `= Kc/Ti`. While `tau < 16L`, `Ti = tau`, so
  `Ki ∝ C(T)/(C(T)/s) = s(T)`. **Exactly `ki(T) = ki_ref * s(T)/s(T_ref)`,
  with `C(T)` cancelling too.** The proposed functional form is not an
  approximation of the SIMC result; it *is* the SIMC result.

So the design is stronger than it claims on this axis. Two caveats it should
carry:

1. **Kp/Kd are not perfectly flat — they scale with `C(T)`.** The prior
   analysis's own `C x1.4` figure means required Kp at cone 10 is ~1.4x the
   bench value, i.e. an unscheduled Kp is ~30% too *low* at the top. That
   direction is benign (sluggish, not oscillatory) and sits inside fuzzy's
   own +-50% band, but "Kp needs no schedule" should be stated as "Kp is
   correct to within the specific-heat factor," not as exact.
2. **The `Ki ∝ s` identity depends on `Ti = tau`, i.e. on `tau < 16L`.**
   Verified live on this bench: `tau = 255.6 s`, `16L = 645 s` — the
   `min()` selects `tau`, and `tau` only falls with temperature, so the cap
   never binds going up. But for a kiln with short dead time (`L < tau/16`),
   `Ti` is pinned at `4(lambda+L)`, a constant, and then `Ki ∝ Kc ∝ C(T)` —
   **the schedule would be applying a ~20x correction to a gain that should
   barely move.** The schedule is only valid in the `tau < 4(lambda+L)`
   regime, and the implementation should assert it rather than assume it.

Sanity check that the above is the rule actually in force, not just the one
in the header: for zone 0, `Kc = 255.6/(42.731*4*40.3) = 0.0371` and
`Ki = 0.0371/255.6 = 0.000145`. The live board reports `Kp=0.0371`,
`Ki=0.00015`. The SIMC path is what produced these gains.

## R5. The `f_rad` bound as proposed is NOT justified — its stated hazard cannot occur, and the analogy it is drawn from would destroy the mechanism

§3 adds "a clamp on `s(T)/s(T_ref)`, analogous to fuzzy's own
`MAX_NUDGE_FRACTION`" and justifies it as: "a sufficiently large `f_rad`
evaluated at a sufficiently large `T` could still drive `ki` toward zero or
negative in principle."

**That hazard does not exist in the form given.** `s(T) = (1-f_rad) +
f_rad*x^4` with `x > 0` is bounded below by `1-f_rad > 0` for any
`f_rad < 1`, and the ratio of two positives is positive. `ki` can therefore
never go negative, and its *floor* is `ki_ref*(1-f_rad)/s(T_ref)`, strictly
positive. Rising `T` drives the ratio *up*, not toward zero. The justifying
sentence is refuted by the algebra the same section presents two paragraphs
earlier.

**Worse, the analogy is actively wrong for this mechanism.** Fuzzy's
`MAX_NUDGE_FRACTION` is +-50%. The schedule's entire purpose is a ~20x
(i.e. +2000%) ratio at cone 10. A bound "analogous to" fuzzy's would clamp
the correction at 1.5x and render the feature inert in exactly the regime it
was designed for — while looking like a safety improvement. This is the
`project_bound_relative_to_persisted_state` failure in its purest form: a
bound chosen by analogy to an unrelated constant rather than derived from
the quantity being bounded.

**Where "magnitude, not sign" actually stops being true**, since the
document asserts a region without naming its edge: the sign of the schedule
is safe for `f_rad` in `[0, 1)` and breaks at exactly `f_rad = 1`. At
`f_rad = 1`, `s(T_ref) = 0` when `T_ref` is the sentinel (R3) — the
divide-by-zero. For `f_rad > 1`, `cond_ref = 1-f_rad < 0` and `s(T)` is
negative for all `T` below the crossover, flipping `ki`'s sign. So:

> **The justified bound is `0 <= f_rad < 1` on the parameter, plus
> `T_ref != ZONE_MODEL_FIT_TEMP_UNKNOWN`. Not a clamp on the ratio.**

That bound is derived from the function's own algebra, applies to every
temperature, and — unlike a ratio clamp — does not neuter the feature.

## R6. The `adaptive_tune` interaction IS a ratchet — §5's "one writer each" is the wrong test

This is the finding that most changes the design. §5 asks "must any pair be
mutually exclusive?" and answers no, on the grounds that each mechanism owns
exactly one piece of state. **State ownership is not the hazard here; a
closed loop between two mechanisms is.** Traced:

`adaptive_tune` has *two* Ki writers, not one, and they have opposite shapes:

- `adaptive_tune_model.c:204` recomputes all three gains from a refitted
  `K_dc` through `pid_autotune_tune_from_fopdt()`. This is an **absolute**
  write: the new Ki is a function of identified plant parameters, not of the
  previous Ki. A schedule scaling the applied Ki cannot bias `K_dc` (a
  steady-state duty-vs-temperature identification), so **this path does not
  compound.** §5's composition argument is correct for this writer.
- `adaptive_tune_ki.c:245` is **relative and persisted**:
  `float new_ki = ki * (1.0f + capped_pct/100.0f)`, where `ki` is the
  *stored* gain and `capped_pct` is inferred from the **observed dwell
  trace** — i.e. from the behaviour of the *effective* Ki. It is written
  back with `zones_config_set_pid(zi, kp, new_ki, kd)` (line 316).

Put the schedule under that and the loop closes:

1. A run dwells at `T > T_ref`. The schedule divides the effective Ki by
   `r = s(T)/s(T_ref)`, deliberately.
2. `adaptive_tune_ki`'s diagnosis sees the resulting slow offset recovery,
   attributes it to Ki, and returns a positive `ki_correction_pct`.
3. It writes `ki_ref * (1+pct)` into the **unscheduled persisted
   reference**.
4. Next run at the same dwell, the schedule divides by `r` again. The
   symptom is unchanged. Step 2 repeats.

Ki ratchets upward run over run, bounded only by
`ADAPTIVE_TUNE_KI_CUMULATIVE_MAX_MULT` (5.0x,
`adaptive_tune_internal.h:266`) — and when that bound finally binds, its
refusal text says "re-autotune this zone," pointing the operator at the
wrong cause entirely. On the way down (a run dwelling *below* `T_ref`) the
same loop runs in reverse into the symmetric cumulative floor. This is
structurally the ratchet `97288659` and `36f88d62` just fixed for `K_dc`,
re-created for Ki by a different route.

The generating fault is the repo's own recurring shape: **a correction
inferred from a transformed observable, written back to the untransformed
reference.** `adaptive_tune_ki`'s input would be post-schedule; its output
is pre-schedule. Nothing in the code relates the two.

Fuzzy does not create this today because its nudge is bounded, symmetric
about the centre cell, and unpersisted — and because the centre cell is
where 100% of observed samples land. The schedule is different in exactly
the way that matters: at a given dwell temperature its factor is a
*constant bias*, not a symmetric nudge, so the diagnosis integrates it
instead of averaging it away.

**This must be resolved before the mechanism lands in any form, dormant or
not.** Two shapes work: (a) `adaptive_tune_ki` divides its correction by the
schedule factor evaluated at the dwell temperature, so it corrects the
reference rather than the effective value; or (b) the Ki-diagnosis layer is
hard-disabled whenever the schedule is non-inert. (a) is correct, (b) is
cheap and honest. Silence is neither. §5 should also record that the
`pid_rescale_integral_for_new_ki()` re-validation it calls for is the
*lesser* of the two interaction concerns.

## R7. Verdict: AGREE with defer — but for R1/R6's reasons, not §7's

The document's own reasoning for deferring ("the motivating effect is
outside this bench's range") is, on its face, an argument *for* landing the
mechanism dormant, not against it: an effect you cannot measure is precisely
one you ship inert and let a real kiln's owner parameterise. §7 does conflate
"cannot validate the parameter" with "cannot land the mechanism." If that
were the only consideration, land-dormant would be the better call.

It is not the only consideration, and defer is still right:

- **The mechanism does not exist to land.** Per R1, the design's Ki
  transform has no identified write path; what §2 specifies could be landed
  into `zone_model_at()` today, but that would schedule `k_dc`/`tau_s` for
  feedforward, feasibility and fuzzy band derivation — a *different* change
  from scheduling Ki, with its own unanalysed consequences. Landing "the
  mechanism" dormant would mean landing the wrong mechanism dormant.
- **Its non-inert path has a known, unresolved ratchet** (R6). Shipping
  inert code whose one non-inert setting silently ratchets a persisted
  safety-adjacent gain is how this repo produced
  `project_fuzzy_ab_inert_control_mode` and the "consumer without producer"
  class. A dormant footgun is still a footgun; the next session to flip the
  flag will not re-derive R6.
- **`T_ref` is empty on every zone** (R3), so even the dormant form would
  ship with its reference input unpopulated and its sentinel unhandled.

**What should land now, and is cheap:** (1) an autotune run on each zone, to
populate `model_fit_temp_c` at all — this is bench-doable today and is a
prerequisite for everything else, including a future `f_rad` fit; (2) the
`ZONE_MODEL_FIT_TEMP_UNKNOWN` guard, wherever the seam eventually grows math;
(3) the R6 decision recorded in `adaptive_tune`'s own documentation, since
the hazard exists the moment anyone scales Ki at read time — including the
fuzzy layer, if its bands ever become asymmetric.

**Net on the document.** §1 (seam is real, carries measured temperature),
§2's functional form, §3's inert-by-default identity, §4's refusal to
declare a winner and §6's venue analysis all survive review — §4 in
particular is unusually honest about a comparison that does not exist.
§2's Ki-vs-Kp argument is *stronger* than the document claims (R4). Three
things need correcting before implementation: §3's "structurally guaranteed"
`T_ref` (refuted, R3), §3's bound and the reasoning behind it (refuted, R5),
and §5's composition conclusion (incomplete in a way that matters, R6).
