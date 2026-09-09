# `iter_tune` redesign — tracking-quality-driven iterative tuning

> **Status update, 2026-09-09:** steps 1, 2 and 5 are IMPLEMENTED and
> validated in simulation — `control/firing_score.c`, `control/firing_compare.c`
> and a rewritten `control/iter_tune.c` (the old whole-firing IAE path is
> deleted, not left dual). Numbers, and two design defects the simulation
> found in this document's own sec 4 step schedule, are in
> `docs/audits/iter_tune_redesign_sim_2026-09-09.md`. Steps 3-4 and 6-9
> (the sec 6.5 credibility gate against a recorded firing, the noise-floor
> artifact, persistence/HTTP surface, the write-surface check, shadow mode
> and hardware trials) are NOT done; nothing is wired into
> `profile_executor.c` and the module proposes nothing on hardware.
>
> **Owner decision, 2026-09-08:**
> *keep `iter_tune`, but redesign it* — "design it better so it does not
> require the same starting point. what i care about is how well it tracks the
> target temperature. test it in simulation until you are confident we have a
> good algorithm." (The "design only, nothing implemented" line that used to
> stand here is stale as of 2026-09-09 — see the status update above.)
>
> This document **supersedes** `docs/audits/iter_tune_decision_2026-09-07.md`
> (`3bf773af`), whose recommendation was "wire the existing module as-is".
> The existing decision core (`firmware/KilnFW/App/drivers/control/iter_tune.c`,
> built in `fe14ddf`/`17f7ebd`) is **not** wired and must not be wired in its
> current form.

---

## 0. Why the existing design cannot simply be wired

Three defects, in the order they matter:

1. **The score is a whole-firing scalar.** `iae_normalized`
   (`profile_executor_firing_stats.c`, `firing_stats_snapshot()`) is
   `iae_raw_c_s / (duration_s * setpoint_span_c)`. Every tick of the run is
   folded into one number, including the ticks where the zone was still
   climbing to meet the profile from whatever temperature the kiln happened to
   start at. Two firings that differ only in start temperature therefore score
   differently for a reason that has nothing to do with the gains.
2. **The comparability rule papers over that with a window, not a fix.**
   `ITER_TUNE_START_TEMP_TOLERANCE_C = 2.0` refuses to compare firings more
   than 2 °C apart. That makes the mechanism *correct* and *nearly always
   idle*: the only two full captures this repo has of the same profile and
   build differ by 4.8 °C at the first sample. The owner's requirement is
   precisely that this constraint go away.
3. **The accept threshold rests on an unknown floor.** `iter_tune.h`'s own
   analysis is honest and worth keeping: the six-repeat `noise_floor.json`
   campaign gives per-zone two-sample prediction intervals of 0.19/0.12/0.20 °C
   on `iae_normalized_whole_c`, the 20 % relative bar is **already below**
   zone 2's interval at today's baseline magnitude, and it gets *worse* as
   tuning succeeds. The absolute floor added on top (0.20 °C) is a patch on a
   metric that should not have been a whole-firing scalar in the first place.

What is worth keeping, unchanged, from the old module:

- The **exact-revert posture**: gains are never recomputed on revert;
  `iter_tune_active_gains()` returns the same float bits that were accepted.
  Keep this verbatim.
- **Pure decision logic, no ESP-IDF / NVS / lock / FreeRTOS**, with the caller
  owning persistence and the one run-boundary call site. Keep.
- **Opt-in per zone, default OFF.** Keep.
- The refusal to touch `kd`. Keep.

## 1. Closed avenues this design must not re-open

Recorded here so a later reader does not redesign back into them:

- **Closed-loop two-point FOPDT ramp fitting** returns an artifact that is a
  function of the commanded rate, with no plant content. This plan does no
  identification of any kind.
- **Dwell-entry climb decay** worsened overshoot on all three zones and tripped
  guard 2; reverted. Not revisited.
- **Coupling lead compensation** reduces at t=0 to the already-reverted
  uncoupled formula. Not revisited.
- **The load estimator cannot observe load** (all three zones closed negative).
  This plan therefore treats load/ware mass as an *unobserved nuisance
  variable* to be rejected by design, never as something to estimate.
- **"Dwell credit" is structurally unreachable** (the 25 °C lag gate and the
  half-cone-step band are mutually exclusive). Not used.
- **Differences below 0.5 °C are not worth chasing** (owner). This is promoted
  below into a hard accept-rule term, not just advice.

---

## 2. The tracking metric

### 2.1 The unit of comparison is a *matched segment*, not a firing

This is the central change. A firing is not scored as a whole; each
**segment** of the profile (a ramp at a commanded rate, or a dwell at a
commanded temperature) is scored independently, per zone. Two *different*
firings — different profiles, different lengths, different start temperatures
— are compared by matching their segments into pairs of the same **segment
class** and comparing pairwise.

A segment class key is `(zone_index, kind, rate_bucket, temperature_bucket)`:

| Field | Definition |
|---|---|
| `kind` | `RAMP_UP`, `RAMP_DOWN`, `DWELL` |
| `rate_bucket` | commanded °C/hr, bucketed at 25 °C/hr granularity (`DWELL` → bucket 0) |
| `temperature_bucket` | the segment's mean target, bucketed at 25 °C |

Rationale for bucketing on temperature: plant gain is not constant with
temperature on this bench — the coupled `ff_hold` solve is already known to be
infeasible above ~62 °C, i.e. the identified model's validity is itself
temperature-bounded. A ramp at 100 °C and a ramp at 500 °C are not the same
experiment and must not be pooled.

**Start temperature stops mattering** because of two exclusions applied before
any segment is scored:

- **Capture-transient exclusion.** Every tick before the zone *first* comes
  within the profile-executor's own tracking band of the target is discarded,
  for the whole firing. That is exactly the work whose size depends on the
  start temperature. A firing started 5 °C warm and one started cold produce
  the same scored ticks; they just have different amounts of discarded prefix.
- **Infeasibility exclusion.** A tick is excluded when the zone commanded
  ≥ 98 % duty for the whole preceding PWM window and the error is still
  negative — the zone is saturated and the gains are not what is limiting it.
  Scoring saturated ticks measures the heater, not the controller.

A segment with fewer than `MIN_SCORED_TICKS` (60, i.e. one PWM window at the
1 Hz `PROFILE_EXECUTOR_TICK_MS` rate) surviving ticks is dropped entirely.

### 2.2 Three sub-scores per segment, no invented weights

Candidates weighed:

| Candidate | Verdict |
|---|---|
| Normalised IAE over the whole profile | **Rejected** — §0.1, the defect being fixed. |
| ITAE | **Rejected** — the time weight is arbitrary across a multi-segment profile and makes a long dwell dominate a short critical ramp for no physical reason. |
| Time-in-band against the guard's own band | **Rejected as the primary** — it saturates. Once a zone is always in band the metric is flat and the search has no gradient, which is exactly when we most want to know whether a change helped. Kept as a *reported* diagnostic and as a safety veto (see §3). |
| Worst-case ramp lag | **Kept, as one of three.** |
| Dwell-entry overshoot | **Kept, as one of three.** |
| Steady dwell error | **Kept, as one of three.** |
| Single weighted composite of the above | **Rejected as the accept rule** — the weights would be invented, and a composite silently permits trading overshoot away for lag. A composite *is* still computed and displayed for humans; it never decides anything. |

The three sub-scores, all per (segment, zone), all **lower is better**:

1. **`lag_s` — ramp tracking lag, in seconds.** For `RAMP_*` segments only:
   `median over scored ticks of ( |actual − target| / |commanded_rate_C_per_s| )`.
   Expressing lag in *seconds* rather than °C is what makes ramps at different
   commanded rates comparable: a 0.5 °C error on a 50 °C/hr ramp and a 5 °C
   error on a 500 °C/hr ramp are the same 36 s of lag and should score the
   same. Segments with `|rate| < 10 °C/hr` are classified `DWELL`, not ramp,
   so this never divides by a near-zero rate. Median, not mean, so one
   excluded-tick boundary artifact cannot move the score.
2. **`entry_peak_c` — dwell-entry overshoot, in °C.** For `DWELL` segments
   only: the maximum positive `actual − target` within the *entry window*, the
   first `L + 2τ` seconds of the dwell using this zone's own persisted
   `model_dead_time_s` and `model_tau_s`. Using the identified time constants
   makes the window physically sized per zone instead of a fixed constant.
3. **`steady_rms_c` — steady dwell error, in °C.** For `DWELL` segments only:
   RMS of `actual − target` over the dwell *after* the entry window. RMS, not
   mean-absolute, because a symmetric limit cycle should be penalised and a
   mean-absolute figure already exists in the firing stats.

A `RAMP` segment therefore yields one sub-score; a `DWELL` yields two. That
asymmetry is deliberate and harmless — comparison is per sub-score.

### 2.3 How two dissimilar firings become comparable

1. Score every segment of firing A and firing B into
   `(class_key) → {sub-score → value}`.
2. Intersect on `class_key`. Only classes present in both contribute.
3. For each sub-score, take the **paired differences** across the matched
   classes: `d_i = B_i − A_i` (negative = B better).
4. Aggregate by **median of the paired differences**, and record `n` = number
   of matched pairs. A single scalar per (zone, sub-score), with an explicit
   sample size attached.

Two firings of entirely different profiles that happen to share two ramp
classes yield `n = 2` for `lag_s` and `n = 0` for the dwell sub-scores — the
mechanism then simply has nothing to say about dwell behaviour from that pair,
and says so, rather than inventing a comparison. **`n = 0` is a first-class
outcome, not an error.**

---

## 3. The accept / reject rule

Two bars must **both** be cleared, per sub-score, for a trial to be accepted;
and a third condition must hold for it not to be rejected.

**Bar 1 — the owner's floor (known today, needs no experiment).**
The median paired improvement must be at least **0.5 °C**, or for `lag_s`, the
lag equivalent of 0.5 °C at that class's commanded rate
(`0.5 / rate_C_per_s` seconds). Differences smaller than this are explicitly
not worth kiln time, by owner instruction. This bar is available from day one
and is what the mechanism runs on before any noise floor exists.

**Bar 2 — the statistical floor (needs the floor, and is skipped until it
exists).** The median paired improvement must exceed
`floor[zone][class_kind][sub_score]`, a per-key two-sided prediction interval,
**and** the paired differences must be consistently signed: at least
`ceil(0.75 * n)` of the `n` paired differences must be improvements, with
`n >= 5`. A sign-consistency requirement across matched segments is far
stronger evidence than one scalar clearing one threshold, and it is available
*within a single firing pair* because a firing contains many segments — which
the old whole-firing design threw away.

**No-degradation veto.** The trial is **rejected** if *any* sub-score with
`n >= 3` degrades by more than its own Bar-1 floor, or if time-in-band (the
diagnostic of §2.2) falls at all outside noise. This makes the rule a
non-dominance test: a trial that buys 1 °C of lag by adding 1 °C of overshoot
is rejected, not silently traded.

### 3.1 Establishing the noise floor

- **In simulation (cheap):** the floor is *estimated* by the null experiment —
  run the same profile on the same plant with **identical gains**, N ≥ 200
  times, varying only the modelled stochastic inputs (§6.1), and take the
  97.5 % two-sided interval of the paired differences per key. This is the
  number Bar 2 uses.
- **On hardware (expensive):** the floor is *validated*, not established, by
  the shadow-mode phase (§8, step 8): ≥ 5 real firings with `iter_tune` scoring
  and proposing nothing. If the observed hardware spread exceeds the simulated
  floor by more than 2×, the simulated floor is discarded and Bar 2 is
  disabled until a real hardware floor campaign is run — the mechanism then
  runs on Bar 1 alone, which is the conservative direction.
- **Before any floor exists:** Bar 2 is skipped and Bar 1 plus the
  no-degradation veto are the whole rule. **Refusing to act is a legitimate
  outcome and is the default one.** The mechanism is designed to spend most of
  its life reporting `REFUSED_INSUFFICIENT_EVIDENCE`.

The existing `noise_floor.json` (six repeats, 2026-09) is **not** reusable as
Bar 2's floor: it is keyed on the *old* metric set, and its own
`start_conditions` block flags the six runs as not strictly like-for-like
(start temps span 1.29 °C). It is kept as a cross-check on the simulator's
realism (§6.3), not as a threshold source.

---

## 4. Perturbation strategy

| Aspect | Decision |
|---|---|
| **What moves** | `kp` and `ki` only. `kd` is never touched (noise-sensitive, no model backing a blind nudge). No other field of any kind is ever written — see §5. |
| **How many at once** | **One parameter, one zone, per firing.** Never two. The measured coupling matrix is large and asymmetric (z0 rises 27.32 per z1 step, z1 only 14.30 per z0 step; adopted `78f2134`, series `813ad90`) — a simultaneous two-zone perturbation is unattributable by construction. Coordinate descent, cycling `kp` → `ki` → next zone. |
| **Step size** | Per-zone, adaptive: starts at **±10 %** of the current baseline value; **halves** after two consecutive rejects; **doubles**, capped at 20 %, after two consecutive accepts. Sign alternates when the previous trial in the same direction was rejected. |
| **The cage** | Every proposed gain is clamped to `[0.5×, 2.0×]` of the **commissioned** gains — the values written by autotune/hand-tuning at commissioning time, persisted once and never rewritten by this module — *and* to `[ITER_TUNE_GAIN_FLOOR_C, ITER_TUNE_GAIN_CEIL_C]`. Anchoring to a persisted commissioning value, not to the rolling baseline, is deliberate: a baseline-relative cage ratchets, which this repo has already been bitten by. **Settled (§9.1):** the anchor is captured automatically — the *first* time `iter_tune` is enabled for a zone, the gains active at that moment are snapshotted as "commissioned" and persisted; there is no separate manual capture step. The anchor is exposed as an explicit, operator-triggered "re-anchor" action (distinct from the existing "restore commissioned gains" revert of §4's Revert path) for the owner to deliberately move the cage centre later, e.g. after a hand-tuning pass. |
| **Stopping rule** | A zone stops (`CONVERGED`, tuning disabled, status sticky) on any of: step size below 3 %; three consecutive rejects at minimum step; **6** scored trials on that zone (§9.2 — reduced from an earlier 12); any gain hitting a cage edge twice. |
| **Not spending kiln time** | The module **never requests a firing.** It arms a trial only for a firing the operator was going to run anyway. If a firing yields `n = 0` matched pairs for every sub-score, the trial stays armed and unscored — but at most **3** such carries, after which it disarms and reverts, so a stale trial cannot ride indefinitely against a moving plant. |
| **Revert path** | Unchanged from the existing module, and this is the part worth keeping verbatim: trial gains live only in `pending_gains`; `active_gains()` returns `baseline.gains` the instant `has_pending` clears. Revert is a flag clear, never an arithmetic undo, so it is bit-exact. Baseline is persisted only on accept, so a power loss mid-trial reverts by itself. A single operator action ("restore commissioned gains") writes the persisted commissioning values back and disables the module for that zone. |

---

## 5. Safety and containment

Hard rules, each with the mechanism that enforces it:

1. **Never widens a limit.** The module's only write path into board state is
   `zones_config_set_pid(zone, kp, ki, kd)` with `kd` passed through
   unmodified. It has no reference to `max_temp_c`, `min_temp_c`,
   `max_ramp_c_per_hr`, `abs_max_temp_c`, any guard threshold, the coupling
   matrix, the model fields, or any safety-processor config.
2. **Enforced mechanically.** A new `check_iter_tune_write_surface.ps1` greps
   the module's translation unit for any setter other than the one allowed
   call and fails the build. Per this repo's standing rule, the check ships
   with a **negative test** proving it can fail — the negative test breaks the
   *production* function, and is **restored by hand**, never with
   `git checkout --`.
3. **Cannot propose an out-of-bounds gain.** The cage of §4 is applied inside
   `iter_tune_propose_perturbation()`, which is pure and host-tested at both
   edges; the caller additionally re-clamps before writing, so a bug in one
   layer is caught by the other.
4. **A bad proposal degrades tracking; it cannot defeat a guard.** The safety
   layers (SaftyFW's independent guards, the ESP's thermal guard, the
   profile-executor watchdog) are untouched and read the same inputs they do
   today. The worst outcome of a bad trial is a worse-tracking firing that the
   guards still bound.
5. **A fault ends it.** Any guard trip, `FAULTED` transition, or operator halt
   during a trial firing → the trial is discarded unscored, gains revert,
   `iter_tune` for that zone is **disabled** with a sticky operator-visible
   reason. It does not retry.
6. **Opt-in, per zone, default OFF**, with no path that auto-enables it —
   including the setup wizard.

---

## 6. The simulation harness

**A plant model already exists and this plan extends it — it does not build a
new one.** `firmware/KilnFW/App/test/sim_plant.c/.h` (added 2026-08-16, live,
maintained, already linked by `test_iter_tune.c` and `test_sim_kiln.c`) is the
asset. It is not what was deleted on 2026-08-28: `SimFW`/`kilnsim` were a
*bench-fixture* stack — a second RP2040 modelling TC/relay/CT electrical
interfaces over `benchproto` — not a thermal model. An earlier investigation
also compared `sim_plant.c` against the `thermal_model.c` that exists only in
git history and found `sim_plant.c` strictly more capable; that older file is
deliberately not resurrected.

What `sim_kiln` already gives us, for free:

- 3 coupled zones (`SIM_KILN_MAX_ZONES`), simultaneous-update coupling via a
  full `coupling_w_per_c[i][j]` conductance matrix, so coupling is not
  order-dependent;
- per-zone first-order lag plus transport delay, with a *separate*
  thermocouple lag (`sensor_lag_tau_s`) and delay (`sensor_delay_s`);
- a **radiative loss term** (`radiative_coeff_w_per_k4`, Stefan-Boltzmann in
  absolute temperature) — the one thing that makes plant gain fall with
  temperature, which a purely linear model can never show;
- per-zone fault injection (dead element, welded relay, TC detached / frozen /
  open) and a `sensor_map[]` for miswired connectors;
- deterministic, reproducible noise (one LCG advance per tick, so two reads in
  one tick agree).

### 6.1 The four gaps to close

Everything else in this section is scoped to exactly these. Nothing else about
`sim_plant.c` is to be rewritten.

| # | Gap | Why it matters here | Work |
|---|---|---|---|
| **G1** | **Real measured parameters are not loaded.** `sim_plant_cfg_t` is parameterised *physically* (`thermal_mass_j_per_c`, `heater_power_w`, `loss_coeff_w_per_c`) and `coupling_w_per_c` is a conductance; the board's measured data is *FOPDT* (`model_k_dc`, `model_tau_s`, `model_dead_time_s`) plus a fitted cross-gain matrix. Today's tests hand-pick constants. | **This is the most important one.** A model driven by measured plant data is evidence for the owner's "validated before it touches the kiln"; a model driven by invented constants is not. | A new `sim_plant_from_zone_cfg()` (see §6.2) plus a checked-in data file carrying the measured values. |
| **G2** | **No 60 s PWM window.** `sim_kiln_step()` takes a continuous `duty` in [0,1]. | The executor does not deliver continuous duty; `heater_output.c` time-proportions it over `HEATER_DEFAULT_WINDOW_MS` = 60 000. The dwell-entry overshoot and the `steady_rms_c` sub-score are *dominated* by window phase at a 1 Hz tick. Scoring against continuous duty measures a controller the board does not have. | Drive the sim from the **real `heater_output.c`**, linked into the harness — not a reimplementation. |
| **G3** | **No relay actuation lag.** The commanded relay state reaches the element instantly. | This is the exact mechanism that defeated relay-based autotune identification on this bench: 1 Hz switching actuated through a 60 s window, every cycle jittering past the fit tolerance. A model without it cannot reproduce a known real failure. | A fixed transport delay on the *relay state* (distinct from `sensor_delay_s`, which sits on the reading), configurable per zone, default from the bench-measured lag. |
| **G4** | **No MAX31856 quantisation.** `sim_kiln_reading_c()` returns a smooth float plus uniform noise. | Unquantised synthetic input is a bug class this project has already paid for — it hides whole branches while the suite reports green. Two of the three sub-scores (`entry_peak_c`, `steady_rms_c`) are sub-degree quantities where the LSB is not negligible. | Quantise at `MAX31856_TC_TEMP_C_PER_LSB` (`max31856_codec.h`) in the read path, after noise, before return. |

### 6.2 G1 in detail: mapping measured data onto `sim_plant_cfg_t`

The conversion is exact up to one free scale, and must be **written down and
then verified numerically**, never trusted algebraically.

For one zone, `sim_kiln_step()` integrates
`dT/dt = (P·u − h·(T − Ta)) / C`, so its steady-state gain is `P/h` and its
time constant is `C/h`. Given measured `K = model_k_dc` and `τ = model_tau_s`,
pick the free scale `h = loss_coeff_w_per_c = 1.0` and set:

- `heater_power_w   = K`
- `thermal_mass_j_per_c = τ`
- `sensor_delay_s   = model_dead_time_s`, `sensor_lag_tau_s = 0`
  (the identified `τ` already lumps the sensor's own lag; splitting it would
  double-count)

Units become nominal rather than physical — that is fine, because nothing in
the scoring cares about watts, only about the input/output dynamics, which are
then exact.

Coupling is harder and is the part that must be **measured inside the sim, not
derived**. The firmware's `coupling_coeff[i][j]` is a fitted steady-state
cross-gain (zone *i*'s rise per unit when zone *j* is stepped, with
`coupling_diag_k_dc` as that identification's own diagonal); the sim's
`coupling_w_per_c[i][j]` is a conductance on `(T_j − T_i)`. The algebraic
first cut is `g_ij ≈ h_i · coupling_coeff[i][j] / coupling_diag_k_dc[j]`, but
the sim's coupling also loads zone *j* (it is a conductance, energy flows both
ways), so this is only a starting point. **Procedure:** set the first cut,
then run a single-zone step test *in the simulator*, read off the resulting
cross-gains exactly the way the bench identification did, and iterate the
matrix until the simulated cross-gains match the measured ones
(`[0, 27.32, 21.72] / [14.30, 0, 22.15] / [8.33, 12.42, 0]`) to within 10 %.
That fitted matrix is checked in as data alongside the measured one, with the
residual recorded.

One honest consequence to state: the measured matrix is **asymmetric**, and a
conductance model of the form `g·(T_j − T_i)` is symmetric in form. `sim_kiln`
permits an asymmetric `g` (its own header says so), so the numbers can be
matched — but the resulting model is then not energy-conserving. That is
acceptable for scoring a controller and is **not** acceptable as a physical
claim about the kiln.

Measured values the mapping is driven from (`tools/PcTools/config_presets/`):

| Zone | `model_k_dc` | `model_tau_s` | `model_dead_time_s` |
|---|---|---|---|
| z0 | 31.96 | 166.9 | 41.1 |
| z1 | 23.48 | 129.1 | 38.1 |
| z2 | 21.74 | 114.8 | 37.2 |

(`tuned_baseline_20260831.json`; coupling matrix from
`coupling_matrix_20260831.json`, adopted `78f2134`, series `813ad90`.)
The loader reads these from a **checked-in JSON snapshot**, not from a
compiled constant table, so the day the board is re-identified the model is
updated by replacing data, and a diff shows what changed.

### 6.3 The harness

Following the pattern `heater_output_pwm_drift_harness.c` already establishes:
**link the real production `.c` files**, do not re-implement them.

Linked as-is: `pid.c`, `pid_fuzzy.c`, `heater_output.c` (this is G2),
`profile_executor_feedforward.c`, `zone_coupling_solve.c`, the new scoring and
comparison modules, and the new `iter_tune.c`. New code is limited to G1–G4
plus the driver loop and the Monte-Carlo runner.

**Deliberate model mismatch is mandatory.** The controller's feedforward
already *assumes* an FOPDT-plus-coupling plant. Scoring the algorithm against
the very model its controller assumes flatters it. Every acceptance run in §7
is therefore a Monte-Carlo over a **mismatched** ensemble: `K` ±30 %, `τ`
±40 %, `L` ±50 %, coupling ±50 %, `radiative_coeff_w_per_k4` swept from 0 to a
value that visibly bends the gain, plus randomised relay lag — while the
*controller* keeps the nominal measured parameters throughout.

### 6.4 What the model still cannot tell us, even with G1–G4 closed

Stated plainly, because a simulation that flatters the algorithm is worse than
none:

- **It cannot validate absolute gain values.** It validates the *decision
  algorithm's statistical behaviour* — false-accept rate, false-reject rate,
  never-worse, termination. Any gain it "finds" is a property of the model.
- **The measured parameters it is driven from were identified at low
  temperature.** The coupled `ff_hold` solve is already known to be infeasible
  above roughly 62 °C, i.e. the identification's own validity is
  temperature-bounded. G1 closes "invented constants"; it does **not** create
  evidence at cone temperature. The radiative term gives the model the right
  *shape* for falling gain with temperature, but its coefficient is not
  measured on this kiln — so high-temperature results are a sensitivity study,
  never a prediction.
- **Its noise floor remains a lower bound.** G4 adds sensor quantisation and
  the existing LCG adds sensor noise, but drafts, ware mass and placement,
  mains voltage variation, element ageing and thermocouple drift are absent
  and their magnitudes on this kiln are unknown. The hardware floor will be
  larger. This is exactly why §3.1 makes the hardware phase *validate* the
  floor and disable Bar 2 if the simulated floor proves optimistic by more
  than 2×.
- **The coupling model is fitted, not physical** (§6.2's asymmetry note), and
  a fitted matrix reproducing three measured cross-gains is not the same as a
  correct heat-transfer model — it will not extrapolate to a different profile
  shape with confidence.
- **It exercises no safety, persistence or UI path.** Guard trips, the safety
  link, NVS schema migration and the HTTP surface are outside it and need
  their own host tests and the bench. (`sim_kiln`'s fault injection can
  provoke guard *inputs*, but the guard wiring itself is not in this harness.)
- **It cannot settle anything below 0.5 °C.** Per the owner's rule, sub-0.5 °C
  effects are reported and not acted on, in the simulator as on the kiln.

### 6.5 The model's own credibility gate

Before any algorithm result is believed, the extended `sim_kiln` must
reproduce a **recorded real firing**. Drive it with the profile and gains from
one of the existing captures in `logs/coupling/*.jsonl` (64 files available)
and require, per zone, over the scored region:

- ramp-phase mean absolute error between simulated and recorded temperature
  ≤ **3 °C**;
- dwell steady-state offset within **1.5 °C** of the recorded one;
- dwell-entry peak within **2 °C** of the recorded one;
- the per-key spread produced by the null experiment to be within **2×** of
  the corresponding entry in the existing six-repeat `noise_floor.json`. The
  model coming out *optimistic* (smaller spread) is tolerated and handled by
  §3.1; coming out *pessimistic* is a failure to investigate, because it means
  the model has a noise source the plant does not.

Held out: fit the coupling matrix (§6.2) against one capture, gate against a
**different** one. If this gate fails, the plan stops here and reports — it
does not proceed to §7 with a model known not to reproduce the plant. Note
that G2 and G3 are load-bearing for this gate: without the PWM window and the
relay lag, the simulated dwell-entry peak has no mechanism to match a recorded
one.

## 7. Acceptance criteria — what "confident" means

All measured over the mismatched Monte-Carlo ensemble of §6.1, ≥ 200 plants,
after the §6.5 credibility gate has passed.

| # | Criterion | Bar |
|---|---|---|
| A1 | **False accept** — null experiment (trial gains *identical* to baseline, only noise differs): fraction of trials returning `ACCEPTED` | ≤ 2 % (hard fail above 5 %) |
| A2 | **Never worse** — final tracking cost vs. starting cost, per sub-score, per zone: fraction of runs ending worse by more than one Bar-1 floor | ≤ 1 % of runs. *This is the criterion that matters most*; a mechanism that cannot improve is acceptable, one that degrades is not. |
| A3 | **Can do something** — from a deliberately detuned start (gains at a cage edge), median improvement in `steady_rms_c` after ≤ 8 accepted trials | ≥ 0.5 °C |
| A4 | **True accept** — where an oracle grid search on the same plant proves a better gain exists inside the cage, fraction of runs that find an improvement within 8 trials | ≥ 60 % |
| A5 | **Termination** — fraction of runs reaching a stopping rule within 15 trials | ≥ 95 % |
| A6 | **Cage** — gains outside `[0.5×, 2×]` commissioned, or outside the absolute bounds, at any tick | 0 occurrences (assert, not a rate) |
| A7 | **Start-point independence** — A1 and A2 re-measured with start temperature randomised over ±15 °C | unchanged within their own bars |
| A8 | **Profile independence** — A1 and A2 re-measured with trial and baseline firings on *different* profiles sharing ≥ 5 segment classes | unchanged within their own bars |

A7 and A8 are the criteria that directly encode the owner's second
requirement; a design that passes A1–A6 but fails A7 or A8 has not been
redesigned, only re-tuned.

---

## 8. Ordered implementation steps

Riskiest last. No hardware exposure before step 8, no heat before step 9.

| # | Step | Risk | Gate to proceed |
|---|---|---|---|
| 0 | This document; supersede the 2026-09-07 brief; ROADMAP row. | none | — |
| 1 | `control/firing_score.c/.h` — pure per-segment scoring (§2.1–2.2): capture-transient and infeasibility exclusion, the three sub-scores, class keying. No wiring, no persistence. Host tests including quantised inputs. | low | host tests green |
| 2 | `control/firing_compare.c/.h` — matched-pair comparator and the accept rule of §3, pure, `n = 0` a first-class outcome. Host tests, including a negative test that breaks the *production* function (restored by hand). | low | host tests green |
| 3 | Extend the **existing** `test/sim_plant.c` with G2 (PWM window, via the real `heater_output.c`), G3 (relay actuation lag) and G4 (MAX31856 quantisation), then G1 — `sim_plant_from_zone_cfg()` loading the measured `k`/`τ`/`L` and the fitted coupling matrix from a checked-in snapshot (§6.1–6.3). Existing `test_sim_kiln.c`/`test_iter_tune.c` users must keep passing: the three new effects default OFF. | medium | **§6.5 credibility gate passes on a held-out capture** — if it fails, stop and report |
| 4 | Null-experiment noise-floor estimation in the simulator; floors emitted as a **data artifact**, not compiled constants. | low | floors within 2× of `noise_floor.json` where the keys correspond |
| 5 | Rewrite `control/iter_tune.c` decision core against the new comparator: cage anchored to persisted commissioned gains, adaptive step, stopping rule, carry limit. Keep the bit-exact revert posture verbatim. Replace `test_iter_tune.c`. | medium | host tests green; the old whole-firing path fully removed, not left dual |
| 6 | Monte-Carlo acceptance run → §7 A1–A8. | medium | **all eight criteria met.** Any miss ends the plan at this line with a report, not a workaround |
| 7 | Persistence + surface: new NVS namespace (never `adap_tune`'s), schema bump, per-zone opt-in, status + "restore commissioned gains" control, `check_iter_tune_write_surface.ps1` with its negative test. Still proposes nothing on hardware. | medium | full check suite green; schema migration tested both directions |
| 8 | **Shadow mode on hardware.** Scores every real firing, computes what it *would* have proposed, writes nothing. ≥ 5 firings. Compare observed spread to the simulated floor (§3.1). | medium | observed floor ≤ 2× simulated, else Bar 2 stays disabled and the mechanism runs on Bar 1 alone |
| 9 | **Enable trials on one zone, owner present, bench fixture kiln only** (§9.3), one parameter, cage active, with the operator able to stop and restore commissioned gains at any point. | highest | owner sign-off |

Steps 1–7 need no kiln time at all. Steps 8 and 9 are the only ones that do,
and step 8 spends none of its own — it rides firings the operator was running
anyway.

---

## 9. Owner decisions (settled 2026-09-08)

These three questions were posed as open in the prior revision of this
document. All three are now decided. Each subsection also states which other
part of this plan the decision constrains — §4's cage and stopping rule are
already updated to match; this section is the record of *why*.

### 9.1 Gain anchor — settled: automatic snapshot, re-settable

**Decision:** adopt the plan's own recommendation as written. The cage anchor
("commissioned gains") is captured automatically, with no separate manual
step: the *first* time `iter_tune` is enabled for a zone, the gains active at
that instant are snapshotted and persisted as that zone's commissioned
baseline. The owner's existing hand-tuned values therefore become the cage
centre with no extra action required at rollout. A distinct, explicit
operator action ("re-anchor") lets the owner deliberately move the cage
centre later — e.g. after a fresh hand-tuning pass makes the old anchor
stale — without that being confused with the existing "restore commissioned
gains" revert action (§4, Revert path), which moves gains, not the anchor.
Reflected in §4's "The cage" row.

### 9.2 Trial budget — settled: 6 scored trials per zone

**Decision:** neither the 12-trials-per-zone figure nor single-zone-only
tuning. The owner chose a smaller per-zone budget, covering all three zones
but stopping sooner on each and accepting partial improvement over
convergence. The chosen figure is **6 scored trials per zone** (half of the
originally drafted 12), reflected in §4's "Stopping rule" row in place of the
old "12 scored trials on that zone".

**Why 6, not some other number smaller than 12:** the plan's own statistical
bar (§3, Bar 2) requires `n >= 5` matched paired-difference samples, with the
sign-consistency check needing `ceil(0.75 * n)` of those `n` in agreement.
Six is the smallest round number strictly *above* that floor — it leaves one
trial of margin over the bar's own minimum, rather than landing exactly on
it with zero slack for a trial that scores fewer than the maximum possible
comparisons. Anything at or below 5 would make hitting Bar 2 at all a
knife-edge case dependent on every single trial contributing a countable
comparison; §4's own "not spending kiln time" rule already allows a trial to
go unscored (`n = 0`) and carry over up to 3 times, which by itself can
consume trials from the budget without ever producing a comparison. Note
that this per-zone trial budget is a distinct control from Bar 2's `n`: `n`
counts *matched segment-class pairs* found by comparing two firings, which
can be several even within one trial's before/after pair, while the 6-trial
budget bounds how many separate gain proposals a zone gets before the
stopping rule fires regardless of outcome. The two are related but not the
same count — the 6-trial cap does not by itself guarantee `n >= 5` on every
trial, it only keeps the exercise from stalling out if the owner's floor
(Bar 1) is what ends up doing the work, which is the expected common case
per §3.1 ("refusing to act is a legitimate outcome and is the default one").

**What this permits:** at one trial per firing, coordinate descent still gets
enough trials to run at least one full step-size adaptation (halve after two
rejects, double after two accepts) on both `kp` and `ki` if the schedule
favors one parameter early; it keeps Bar 2 reachable in the common case where
most trials produce a scorable comparison; and across three zones the total
operational cost is roughly **18 firings** (3 zones × 6 trials, one parameter
change per firing), versus roughly 36 under the original 12-trial figure —
consistent with "accepting partial improvement rather than convergence."

**What this forbids:** it does not give a zone enough trials to fully explore
both `kp` and `ki` through multiple step-size halvings each, so a zone is
expected to stop at `CONVERGED` (trial-budget exhausted) having tried only a
handful of proposals per parameter, not to reach the same degree of
refinement the 12-trial figure targeted. It does not change or relax Bar 2's
`n >= 5` requirement itself — a zone whose firings keep landing on `n = 0` or
`n < 5` matched pairs still cannot clear Bar 2 inside 6 trials any more than
it could inside 12, and falls back to Bar 1 (the 0.5 °C floor) plus the
no-degradation veto exactly as §3.1 describes. This budget is independent of
the simulation-only acceptance criteria A3/A4 (§7), which test the
*algorithm's* statistical capability at a fixed 8-trial figure in the
Monte-Carlo harness before anything is deployed; A3/A4 are unchanged by this
decision, since it governs deployed hardware behavior, not the simulator
acceptance bar.

### 9.3 Scope — settled: bench fixture only, revisitable

**Decision, in the owner's words:** *"Bench fixture only for now. Prefer the
situation until you see real success."* Step 9 (§8) is therefore limited to
the bench fixture kiln, and this is a **current boundary, not a permanent
one** — it is explicitly revisitable, not a closed avenue in the sense of
§1.

**What "real success" means, in terms of this plan's own criteria:** the
bench-fixture phase is judged successful once step 9 has run enough on the
bench fixture to show, on real hardware, the same shape of result the
simulator's acceptance criteria (§7) describe in simulation — concretely,
observed shadow-mode spread staying within the §3.1 bound (≤ 2× the
simulated floor, keeping Bar 2 usable rather than falling back to Bar 1
alone), and enabled trials on the bench fixture clearing Bar 1 or Bar 2 with
no instance of A2's "never worse" failure mode (a trial ending worse by more
than a Bar-1 floor) and no cage violation (A6). In short: the bench fixture
needs to demonstrate, on hardware, that the mechanism behaves the way §7
predicted it would — not merely that it runs without crashing.

**Widening beyond the bench fixture is not an incremental step.** Moving
step 9 to a production firing with ware in the kiln requires its own,
separate safety sign-off from the owner — it is not something this plan's
existing step 9 gate ("owner sign-off") already covers, and it is not
triggered automatically by "real success" on the bench fixture. This
document does not attempt to define that sign-off's criteria in advance;
that is deliberately left to be decided when it is proposed.
