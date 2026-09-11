# Scoping: can the fuzzy controller be tuned the way iter_tune tunes PID? (2026-09-11)

> **PARTIALLY SUPERSEDED (2026-09-11, added later same day).** §8.2's
> "continual adaptation across heat cycles" mechanism was designed here in
> ignorance of `adaptive_tune` (`firmware/KilnFW/App/drivers/control/adaptive_tune*`),
> which already implements bootstrap-from-autotune plus per-cycle refinement
> in shipped, host-tested code. See the superseding note inline at §8.2
> below for the detail, and the two current documents:
> `docs/audits/adaptive_tune_vs_owner_requirements_2026-09-11.md` (the
> verifying audit) and `docs/FUZZY_CONTROLLER_PLAN.md` (corrected to match).
> Every other section of this document, including the load-bearing
> `ERROR_BAND_C_DEFAULT`/`RATE_BAND_C_PER_S_DEFAULT` finding just below and
> §8.1's normalization proposal, stands unaffected.

**Question posed by owner:** `sim_iter_tune.c`/iter_tune tunes PID gains. Can
the fuzzy logic controller (`pid_fuzzy.c`) be improved the same way?

**No controller behavior is changed by this document.** No board was
flashed and no heating run was performed for this task; all board facts
below are live reads via `kiln_call`.

**Load-bearing finding (justifies §8's whole restructure, stated here up
front):** `pid_fuzzy.c`'s two membership-band constants —
`ERROR_BAND_C_DEFAULT = 20.0f` and `RATE_BAND_C_PER_S_DEFAULT = 0.5f`
(`pid_fuzzy.c:90-91`) — are **absolute degrees-C / degrees-C-per-second
values that the code's own header comment admits were sized by desk
reasoning about "a mid-size kiln zone," never derived from any measured
plant** (`pid_fuzzy.h:1-33`). They were not fitted on this bench rig either
— no fit exists at all — but they are exactly as installation-specific a
guess as a bench-fitted constant would be, just an unmeasured one instead
of a measured-then-misapplied one. This is why §8 below treats "bootstrap
from autotune" as a structural fix to a pre-existing defect this scoping
task surfaced, not merely a response to the owner's bench-fixture concern.

## 0. Live board state (verified, not assumed)

`control_get_zones()`, read 2026-09-11:

```
Zone 0 (mode 3): Kp=0.0371 Ki=0.00015 Kd=0.7476
Zone 1 (mode 3): Kp=0.0639 Ki=0.00025 Kd=0.9989
Zone 2 (mode 3): Kp=0.0703 Ki=0.00028 Kd=0.9126
http-only fields:
  z0: fuzzy_strength_pct=0.0, ease_off_window_mult=2.0
  z1: fuzzy_strength_pct=0.0, ease_off_window_mult=2.0
  z2: fuzzy_strength_pct=0.0, ease_off_window_mult=2.0
```

`control_mode` is **3** on all zones — confirms `91c5d6d` landed and mode 2
(structurally inert) is not selected. **However, `fuzzy_strength_pct` is
0.0 on all three zones right now.** Per `pid_fuzzy_adjust()`'s own documented
safety contract (`pid_fuzzy.c:194-213`), `strength_pct == 0` reproduces the
base PID gains bit-for-bit with "no fuzzy math in the path at all." So while
the board is no longer running the mode-2 defect, **it is currently running
classic PID in practice** — mode 3 selects the fuzzy code path, but strength
0 makes that path a no-op. This is a live fact, not a historical one, and it
is a state nobody flagged in the memory summary handed into this task: the
board is neither "fuzzy defect" (mode 2) nor "fuzzy actively adapting"
(mode 3 + strength > 0) today, but a third state — fuzzy selected, disarmed.
Any future characterization run needs `fuzzy_strength_pct > 0` set first, or
it will silently re-collect PID-only data under a "mode 3" label, repeating
the exact `control_mode` confusion that produced the withdrawn 28/29-file
claim.

## 1. What the fuzzy layer is and where it meets PID

Files: `firmware/KilnFW/App/drivers/control/pid_fuzzy.c`,
`pid_fuzzy.h`, called from `profile_executor_pid_tick.c:311-374`
(`pid_fuzzy_prepare_gains()`).

- **Inputs (2, per zone, per tick):** `error_c` (setpoint − measurement) and
  `error_rate_c_per_s` (`pid.c`'s `d_filtered`, i.e. `-d(measurement)/dt`,
  frozen while `|error| > pid_range_c`). No neighbor-zone or coupling input
  by design (`pid_fuzzy.h:23-32`: a measured disturbance belongs on the
  feedforward path, not folded into feedback gains — deliberate exclusion,
  not an oversight).
- **Membership functions:** triangular, 3 buckets per axis (NEG/ZERO/POS,
  FALLING/STEADY/RISING), symmetric around 0, each pair of adjacent buckets
  linearly interpolating to 1.0 at the band edge (`pid_fuzzy.c:101-128`,
  `triangular_memberships()`). Band half-widths are now **per-zone runtime
  config** (`zones_config_get_error_band_c()`/`_rate_band_c_per_s()`,
  `zones_config_json.h` `ZONE_ERROR_BAND_C_{MIN,MAX,DEFAULT}` = 1/100/20 °C,
  `ZONE_RATE_BAND_C_PER_S_{MIN,MAX,DEFAULT}` = 0.01/5.0/0.5 °C/s), added
  `ZONES_CFG_VERSION` 18→19. A 0 value is the "use firmware default"
  sentinel, same convention as `control_mode`/`tc_type`.
- **Rule base:** fixed 3×3 = 9 cells (`RULE_TABLE`, `pid_fuzzy.c:142-155`),
  Mamdani product AND, weighted-average defuzzification
  (`pid_fuzzy.c:265-279`). Each cell outputs a `{kp_dir, ki_dir, kd_dir}` ∈
  {−1,0,+1} direction, not a value. **The rule table itself is a compile-time
  constant, deliberately not exposed as config** — `pid_fuzzy.h:17-21`
  states the rationale explicitly: "a per-installation editable rule table
  reopens the trial-and-error tuning problem this whole mode exists to
  avoid."
- **What the output actually is:** the fuzzy layer does **not** produce a
  duty and does **not** blend with a separate PID output. It produces a
  **per-tick rescaled copy of kp/ki/kd** that `pid_update()` then runs
  exactly as it would classic PID gains (`pid_fuzzy.c:281-285`:
  `out = base * (1 + strength/100 * 0.5 * dir)`). The one exposed tuning
  knob is `strength_pct` (0-100, bounds how far gains may drift from the
  Autotune-measured base, capped at ±25% per gain at `strength_pct=50`,
  ±50% at 100). This matters directly for the question asked: **"improving
  fuzzy" cannot mean "improve the output signal" the way PID tuning does**
  — there is no separate fuzzy output to improve. It can only mean:
  reshaping the membership bands, reshaping the rule table's direction
  values, changing `strength_pct`, or changing the bucket/cell count.

## 2. Baseline: is there enough mode-3 data to say what fuzzy is doing today?

**No — one arm, `n=1` run, and it never completed.** Verified directly:

- `logs/coupling/fuzzy_ab_20260904d_s50_run1.jsonl` is the **only** capture
  in `logs/coupling/` that ran `control_mode: 3` (confirmed in
  `fuzzy_behavior_20260904d_report.md` and independently in
  `fuzzy_bands_envelope_20260904e_report.md`'s correction). 726 usable
  rows / 2178 zone-samples, strength_pct=50, profile 7. The paired
  `strength_pct=0` arm (A1) **never ran** — the campaign died to a
  task-watchdog reset ~64 minutes into a planned ~104-minute run, so this is
  explicitly **not an A/B result**: no comparison of fuzzy-on vs fuzzy-off
  tracking exists anywhere in this archive.
- Within that one arm, **100% of 2178 zone-samples landed in the single
  center rule cell (ZERO error × STEADY rate)** — peak |error| 5.55 °C
  (27.8% of the 20 °C band), peak |rate| 0.110 °C/s (22% of the 0.5 °C/s
  band). The other 8 of 9 rule cells have **never been exercised on
  hardware**, ever, under any capture in this repo's history. This is
  confirmed, not merely repeated from memory: I re-read
  `fuzzy_bands_envelope_20260904e_report.md` directly, which independently
  widened the same conclusion across 29 archived captures (37,008
  zone-samples) and found **100.000% of those also land in ZERO/STEADY**
  — though 28 of those 29 files ran `control_mode: 2` and are explicitly
  WITHDRAWN as evidence about the fuzzy rule table (they only show this
  rig's PID tracking envelope). Only the same 2178-sample mode-3 arm counts.
- Given the corrected/withdrawn state of the evidence, and per this task's
  explicit instruction not to resurrect cell-crossing as a metric: the
  right metric is gain output, and on that metric the one real arm shows a
  **consistent, near-ceiling, one-directional rescale** — kp/kd down
  ~22-23% (mean), ki up ~20-23% (mean), essentially constant for the whole
  63.8-minute capture (`fuzzy_behavior_20260904d_report.md` §1 table). At
  today's shipped bands (20 °C / 0.5 °C/s) and this rig's demonstrated
  envelope (max 5.72 °C / 0.165 °C/s across all 29 captures, mode 2 and 3
  combined), the fuzzy layer is **not adaptive control** — it is a fixed
  gain rescale, indistinguishable in this rig's operating regime from
  hand-editing kp/ki/kd once and leaving them there.

**Finding, stated plainly per the task's instruction: there is not enough
mode-3 data to characterize the fuzzy layer, and this is itself the
important result.** The correct first step is collecting a real baseline —
not tuning — and that baseline collection must (a) set `fuzzy_strength_pct >
0` first, since it currently reads 0 on the live board (§0), and (b) verify
`control_mode` per file before trusting any filename, per the exact trap
`fuzzy_behavior_20260904d_report.md`'s correction names (`fuzzy_ab_20260904_s50_run1.jsonl`
vs `fuzzy_ab_20260904d_s50_run1.jsonl` — one character apart, different modes).

## 3. What is tunable, ranked by risk

| # | Parameter | Count | Persisted? | Change requires |
|---|---|---|---|---|
| 1 | `strength_pct` | 1 per zone (3 total) | Yes — `fuzzy_strength_pct` float, `zones_config_json.h`, HTTP-only field (not on the UART CONTROL wire per `control_get_zones` output) | Runtime config POST only, no firmware change |
| 2 | Membership-band breakpoints (`error_band_c`, `rate_band_c_per_s`) | 2 per zone (6 total) | Yes — `ZONES_CFG_VERSION` 18→19, per-zone, bounded [1,100]°C / [0.01,5.0]°C/s, 0-sentinel = firmware default | Runtime config POST only, no firmware change (already the case since `904db54`) |
| 3 | Rule-consequent directions (`RULE_TABLE[3][3]`, 3 outputs/cell) | 27 values (9 cells × {kp_dir,ki_dir,kd_dir}), currently ∈ {−1,0,+1} | **No** — compile-time constant, deliberately excluded from config by design (`pid_fuzzy.h:17-21`) | Firmware change; also a documented design decision to reopen, not merely a code change |
| 4 | Number of rules/cells (bucket count per axis, currently 3×3=9) | Structural — changes the array shape and `triangular_memberships()`'s signature | No | Firmware change, larger than #3 |
| 5 | `MAX_NUDGE_FRACTION` (global fuzzy-PID blend ceiling, currently 0.5) | 1, board-wide | No — `#define`, not config | Firmware change |

Note on the 15-character NVS key cap flagged in project memory
(`project_nvs_key_too_long_zone_normals.md`): `fuzzy_strength_pct`,
`error_band_c`, `rate_band_c_per_s` field names in `zones_config_json.h`
are struct fields serialized to the `cfg` LittleFS partition / JSON blob,
not raw NVS keys, so this cap does not directly constrain them the way it
broke the 16-char `zone_normals` key — but any *new* fuzzy-tuning parameter
that also needs a standalone NVS key (rather than living inside the
existing zone-config blob) would need its own key ≤15 characters, worth
flagging explicitly if #3/#4 above are ever pursued.

Risk ordering rationale: #1-2 are already runtime, already reversible with
one config POST, and the owner has already been offered exactly this
decision once (`fuzzy_bands_envelope_20260904e_report.md` §5.1/§6, still
open — see below). #3-4 reopen a deliberately-closed design question (the
rule table was made a compile-time constant specifically to avoid
per-installation trial-and-error) and require firmware changes to
production control code.

## 4. Can iter_tune's machinery carry fuzzy parameters?

Read `firmware/KilnFW/App/test/sim_iter_tune.c` directly (not from memory).

**No — it is shaped around exactly kp and ki, one at a time, and does not
generalize to fuzzy's parameter set without a rewrite, not a config change.**

- The proposal/accept loop varies **kp and ki only**, per zone, **one
  parameter at a time, never two together** — explicit in-code comment,
  `sim_iter_tune.c:405`: "One parameter, one zone, per firing — never two
  (plan sec 4)." `kd` is asserted unchanged between trial and initial
  (`sim_iter_tune.c:429`: `trial_gains[i].kd != rep->initial[i].kd` is a
  rejection condition). So even PID tuning here is really a **2-parameter
  search per zone** (kp, ki), not 3 — kd is fixed by design, not part of
  the search space at all.
- The accept/reject apparatus (`bd_reachability_check`, the cage bounds
  `ITER_TUNE_CAGE_LOW_FACTOR`/`HIGH_FACTOR` around each parameter's own
  current value, `sim_iter_tune.c:423-429`) is **not parameter-agnostic in
  its current form** — it is a 1-D line search per call (perturb one scalar,
  measure `mean_abs_err_c`, accept/reject), repeated across a small,
  named set of scalar knobs (`kp`, `ki`) that the surrounding code addresses
  by name (`trial_gains[i].kp`, `.ki`, `.kd`), not through any generic
  parameter-vector abstraction. Carrying a new parameter (e.g.
  `fuzzy_strength_pct`) through this exact loop would need new fields
  threaded through the same call sites, not a drop-in extension.
- Dimension count if fuzzy parameters were added to the *search*, not just
  the *config schema*: strength_pct (1) + error_band_c (1) + rate_band_c_per_s
  (1) = **3 additional continuous parameters per zone**, on top of the
  existing 2 (kp, ki) — a jump from a 2-parameter to a 5-parameter search
  per zone, 15 across 3 zones (vs. 6 for kp/ki alone). If the rule table's
  27 direction values were ever opened to search (§3 risk tier 3, not
  recommended), the per-zone dimension would be irrelevant since the rule
  table is board-wide, not per-zone — but 27 more parameters, likely
  categorical/discrete ({-1,0,+1}) rather than continuous, is a materially
  different search problem than the continuous 1-D line search this
  machinery does today.
- `mc_runs` default is 40 (`sim_iter_tune.c:514`, `argc>1 ? atoi(argv[1]) :
  40`); the cited production run used `mc_runs=220`
  (`sim_iter_tune.c:625` comment: "24 ACCEPT / 660 null comparisons at
  mc_runs=220"). **220 Monte-Carlo trials is already the working budget for
  a 2-parameter-per-zone, one-at-a-time line search.** A genuine 5-parameter
  joint search per zone (or worse, a 15-parameter joint search across all
  three zones, since strength/bands could plausibly interact across zones
  through the shared plant) is a different order of search-space volume —
  even a coarse grid at 5 levels per axis is 5^5 = 3,125 combinations per
  zone for a 5-D joint search, vs. the handful of scalar candidates a 1-D
  line search evaluates per accept/reject step today. **220 runs has no
  realistic chance of covering that volume**; the existing apparatus is
  sized for the problem it was built for (kp/ki line search) and would need
  a fundamentally different search strategy (e.g. coordinate descent one
  fuzzy parameter at a time, mirroring the existing "never two together"
  discipline, or a much larger Monte-Carlo budget with a lower-dimensional
  reduction) to touch fuzzy parameters credibly. This is not a case for
  "just raise mc_runs" — the dimensionality problem, not the sample count,
  is the blocker.

## 5. Is fuzzy tuning confounded by the coupling-model defect?

**Independent, not confounded — but only because of what the fuzzy layer
explicitly excludes, and this needs to be verified against practice, not
just design intent.**

`pid_fuzzy.h:23-32` is explicit that coupling is deliberately kept OUT of
the fuzzy inputs — the two inputs are `error_c` and `error_rate_c_per_s`,
both derived from **this zone's own thermocouple only**, with cross-zone
coupling routed to the feedforward path instead, specifically to avoid
double-counting a disturbance that "is what 'coupled' means." By that
design, tuning the fuzzy membership bands or rule table does not touch, and
is not touched by, the coupling matrix or its currently-refuted
superposition assumption (`docs/audits/joint_load_model_class_design_2026-09-11.md`,
`docs/audits/coupling_sign_reversal_artifact_test_2026-09-11.md`).

However, this independence is only as good as the assumption that a fuzzy
tuning campaign's *evaluation data* (real firings, all three zones running
together) does not itself carry the coupling defect's fingerprint into
`error_c`/`error_rate_c_per_s` — it does, because those two inputs are
literally "how far off and how fast is this zone moving," and a zone's
tracking error during a joint multi-zone firing is exactly where an
unmodeled ~33% coupling superposition failure (or a sign reversal at
62-75 °C) would show up. So: **the fuzzy layer's own math is independent of
the coupling model by design, but any *evaluation* of a fuzzy tuning change
using multi-zone joint firings is not** — the same error signal a coupling
defect corrupts is the signal the fuzzy rule table reacts to. Fitting bands
or a strength value against joint-firing data risks tuning fuzzy to
partially compensate for (or be biased by) the coupling defect's residual,
which would be attributed to "fuzzy behavior" and would silently break if
the coupling model is later fixed. A single-zone-at-a-time evaluation (no
joint dwell, avoiding exactly the regime `coupling_failure_is_joint_dwell_specific`
identifies as where superposition fails) sidesteps this and should be the
evaluation protocol for any fuzzy characterization or tuning work, at least
until the coupling model is resolved.

This bench rig is also a ~4 W, 120 V fixture that cannot reach 40 °C above
ambient at full duty (per standing project memory), so any claim about
fuzzy behavior at real-kiln high-temperature / large-error regimes is
necessarily extrapolation from this rig's own 0-80 °C, small-error envelope
— not something this task can resolve, only flag.

## 6. Owner constraint added mid-task: sim-first

The owner added a standing constraint after the above sections were drafted:
**any fuzzy-controller improvement must be tested in simulation before any
hardware run.** This section addresses it directly; §7 below restates the
ranked recommendation under this constraint.

### 6.1 Blocking finding: neither closed-loop sim harness exercises the fuzzy path today

Checked directly, not assumed:

- **`sim_iter_tune.c`** (`firmware/KilnFW/App/test/sim_iter_tune.c`): grepped
  for `fuzzy` — **zero matches**. Its PID tick builds a `pid_cfg_t` from
  `gains[i].{kp,ki,kd}` and calls the plant/controller loop directly; there
  is no `pid_fuzzy_adjust()` call anywhere in this file.
- **`sim_credibility_gate_closedloop.c`** (same directory): also calls
  `pid_update_terms()` directly (line 519), and **explicitly, unconditionally
  forces classic PID** — `s_exec.zones[z].control_mode =
  ZONE_CONTROL_MODE_PID;` at line 466. This file's own comments (lines 47-55)
  independently confirm the same "sim ran PID, board ran fuzzy" mismatch
  this scoping task has been circling: "z1/z2 control_mode reads 0 (OFF)...
  LIVE (control_get_zones(), 2026-09-10 — different control_mode (fuzzy)
  than captures (plain PID))." That is a prior, independent sighting of the
  exact same gap.
- The **only** harness that calls `pid_fuzzy_adjust()` at all is
  `pid_fuzzy_drift_harness.c`, and it is not a closed-loop simulation: it
  reads 8-float test vectors from stdin, calls `pid_fuzzy_adjust()` once per
  line, and prints the output gains — a pure numerical-equivalence check
  against a Python port (`tools/PcTools/src/kilnctrl/fuzzy_band_probe.py`),
  wired to `check_pid_fuzzy_drift.ps1`/`fuzzy_gain_mirror_drift_check.py`.
  It proves the C and Python math agree; it says nothing about closed-loop
  behavior, because it never runs against `sim_plant.c` or any plant model
  at all — there is no temperature, no setpoint, no time axis, just one
  function call per line.

**This is a blocking finding, exactly as the coordinator flagged as the
possible outcome: today, no simulation of any kind exercises the fuzzy
control path in closed loop.** The two existing sim harnesses both
hardcode classic PID; the one fuzzy-aware harness is a stateless unit-level
math check. The first task for a sim-first fuzzy campaign is **wiring
`pid_fuzzy_adjust()` into a closed-loop sim harness (a variant of
`sim_iter_tune.c`'s or `sim_credibility_gate_closedloop.c`'s per-tick PID
call, calling `pid_fuzzy_prepare_gains()`'s logic or `pid_fuzzy_adjust()`
directly ahead of `pid_update_terms()`, mirroring
`profile_executor_pid_tick.c:311-374`'s real call site), not tuning fuzzy
parameters** — there is nothing to tune against yet. This mirrors the exact
shape of the withdrawn 28/29-captures claim: absent an explicit check of
which control path actually ran, it would be easy to build a "fuzzy sim"
that silently runs PID throughout, the same mistake made twice already on
hardware (`control_mode 2`, then the filename trap between
`fuzzy_ab_20260904_s50_run1.jsonl` and `fuzzy_ab_20260904d_s50_run1.jsonl`).
Any new sim harness must print/assert its `control_mode` and
`fuzzy_strength_pct` per run, the same discipline the hardware captures
lacked until after the fact.

### 6.2 What simulation can and cannot honestly settle here

`sim_plant.c` (`firmware/KilnFW/App/test/sim_plant.c`) is the additive,
duty-driven multi-zone coupling model — the same model class
`docs/audits/joint_load_model_class_design_2026-09-11.md` designs against
and `docs/audits/coupling_sign_reversal_artifact_test_2026-09-11.md`
(`fd8d7b93`) confirms is **refuted on real hardware**: superposition across
zones fails ~33% at low level, and the residual sign reverses between
roughly 13 °C (model over-predicts) and 62-75 °C (model under-predicts) —
confirmed as a real effect, not a matrix artifact.

**Sim CAN honestly answer, once fuzzy is wired into a closed-loop harness:**
- Gross stability / non-instability of a candidate rule table or band
  setting (no runaway, no oscillation, no NaN/inf propagation) — a
  structural property of the fuzzy math and the control loop, not of the
  plant's absolute accuracy.
- Relative ranking between candidate fuzzy configurations (band A vs band
  B, rule table variant A vs B) under an identical, fixed plant model —
  since both candidates see the same (wrong) plant, a comparison between
  them is internally consistent even if the plant's absolute output is not
  trustworthy in absolute terms.
- Regression against existing accept bars (the same role
  `check_sim_iter_tune_bars.ps1` already plays for PID) — did a change make
  the simulated tracking error worse by the sim's own yardstick, holding
  everything else fixed.
- Exercising the 8 untested rule cells' *direction* of gain movement
  (§6.1's wiring task, then feeding large synthetic error/rate) — this is a
  property of `pid_fuzzy_adjust()`'s pure math, not the plant, so sim (or
  even the stateless drift harness) settles it regardless of plant
  accuracy.

**Sim CANNOT honestly answer, given the refuted coupling model:**
- Any absolute claim of tracking-error improvement in °C on real hardware —
  a sim result showing "fuzzy reduces mean|err| by X°C" is a statement
  about `sim_plant.c`'s additive model, not about the kiln, precisely
  because that model's own multi-zone interaction term is the refuted part.
- Anything that depends on multi-zone interaction at all — joint dwells,
  coupled ramps, any scenario where one zone's duty affects another's
  temperature through the modeled coupling matrix — since that is the exact
  mechanism shown to have a sign-reversing ~33% error.
- A verdict that a specific fuzzy tuning "works on this kiln" from sim
  alone. A sim win must be read as "consistent with sim's model of the
  kiln," not as evidence of a hardware improvement, and must not be
  reported to the owner or in any follow-up doc as if it were hardware
  confirmation. Per this task's own instruction: **do not recommend a sim
  result as evidence of a hardware improvement** — sim is a screening and
  regression tool here, not a validation tool, until a hardware A/B (with
  `fuzzy_strength_pct` actually nonzero and `control_mode` verified) exists.

### 6.3 Single-zone sim is on much firmer ground

Per project memory, single-column transport was measured to be linear —
`coupling_failure_is_joint_dwell_specific` and the two coupling audits above
place the refutation specifically at multi-zone superposition, not at a
single zone's own thermal response to its own duty. A **single-zone sim**
(one zone's own `k_dc`/`tau_s`/`dead_time_s` model, zero coupling terms
active, no other zone driven) sidesteps the exact mechanism that is
refuted, and is therefore a legitimate testbed for:

- Membership-band rescaling (§3 tier #1/#2): does a narrower `error_band_c`/
  `rate_band_c_per_s` cause the outer rule cells to engage sensibly during a
  single zone's own ramp/dwell, without inducing oscillation or excessive
  gain chatter — a question entirely about that zone's own error/rate
  trajectory against its own (measured-linear) plant.
- Confirming rule-table direction sanity (§6.1's wiring task) end-to-end
  through a real closed loop, not just the stateless math harness.
- `strength_pct` sensitivity — how much a given band/strength setting
  actually moves tracking error for one zone in isolation, which is exactly
  the "is this adaptive control or a near-constant rescale" question
  `fuzzy_bands_envelope_20260904e_report.md` raised and could not settle
  from hardware alone (n=1 incomplete arm).

This is likely the right first substantive step once §6.1's wiring exists:
it produces a directly usable, honestly-scoped result (single-zone fuzzy
behavior under a plant model that is not the refuted part) rather than one
that is confounded by §6.2's caveats from the start. It does **not** answer
whether fuzzy helps with cross-zone disturbances (that question is
structurally excluded from fuzzy's own inputs anyway — §5/§1 above — so a
single-zone answer is not actually a narrower answer to the question fuzzy
was built to address).

## 7. Ranked recommendation (revised under the sim-first constraint)

The owner's added constraint — simulate before any hardware run — reorders
this list relative to §2/§4's findings alone. Steps 1-2 below are now the
cheapest credible steps precisely because they are sim/host-only; the
hardware baseline that was "step 2" before the constraint moves to step 4,
after sim work has done what it honestly can.

1. **(Cheapest, do first, zero kiln time, zero firmware change, sim-only.)
   Directly exercise `pid_fuzzy_adjust()` with synthetic large-error/
   large-rate inputs against the stateless drift harness
   (`pid_fuzzy_drift_harness.c`) or a small standalone script — no plant,
   no board — to find out what the untested 8 of 9 rule cells actually do.**
   This is the same recommendation `fuzzy_bands_envelope_20260904e_report.md`
   §5/§6 already made and it is still open and unactioned. It settles a
   pure-math question (§6.2's "sim CAN answer" list) and is unaffected by
   the coupling-model defect since it involves no plant at all.
2. **Wire `pid_fuzzy_adjust()` into a closed-loop, single-zone sim harness**
   (§6.1's blocking finding: neither `sim_iter_tune.c` nor
   `sim_credibility_gate_closedloop.c` calls it today; the latter
   hardcodes `control_mode = ZONE_CONTROL_MODE_PID` at line 466). Scope this
   single-zone only, per §6.3 — one zone's own `k_dc`/`tau_s`/`dead_time_s`,
   no other zone driven, no coupling term active — since single-column
   transport is the part of `sim_plant.c` that is *not* refuted. Assert and
   print `control_mode`/`fuzzy_strength_pct` per run in the harness itself,
   the same discipline missing from the hardware captures that produced the
   withdrawn 28/29-file claim. This is host-only, no kiln time, and is the
   prerequisite for every subsequent step.
3. **Use the single-zone sim from step 2 to screen membership-band values
   and confirm rule-table sanity end-to-end** (risk tiers #1/#2 from §3):
   relative ranking between candidate `error_band_c`/`rate_band_c_per_s`
   settings, regression against a fixed baseline (mirroring
   `check_sim_iter_tune_bars.ps1`'s role for PID), and gross-stability
   screening of the previously-proposed rescale
   (`fuzzy_bands_envelope_20260904e_report.md`'s 6-8 °C / 0.20-0.25 °C/s
   candidate). Per §6.2, treat any sim win here as *screening*, not as
   evidence of a hardware improvement — it selects candidates worth taking
   to hardware, it does not validate them.
4. **Only after 1-3, and only as the next step (not a substitute for
   hardware validation): set `fuzzy_strength_pct > 0` on the board
   (currently 0 on all zones — §0) and collect one clean, single-zone
   mode-3 hardware capture** using whichever band/strength setting sim
   screening favored, verifying `control_mode` and `fuzzy_strength_pct`
   per file before trusting any run label (§2's exact trap). This is the
   first point at which a real tracking-error number can be trusted, and it
   remains a baseline/screening capture, not a full campaign, until it
   exists at all — today there is one incomplete arm and zero completed
   A/B comparisons in this repo's history.
5. **A multi-zone / joint-dwell hardware or sim campaign is not recommended
   until the coupling-model defect is resolved** (§5, §6.2) — sim cannot
   honestly screen it (the refuted interaction term is exactly what such a
   scenario would exercise), and a hardware campaign risks fitting fuzzy
   tuning to the coupling defect's residual rather than to real controller
   behavior.
6. **Do not attempt to route fuzzy parameters through the existing
   iter_tune apparatus as-is.** §4 shows it is a 2-parameter-per-zone,
   one-at-a-time line search at a Monte-Carlo budget (220 runs) sized for
   that problem; fuzzy tuning is a minimum 5-parameter-per-zone problem
   (or 27 more, board-wide, if the rule table is ever opened) and would
   need new search machinery, not a parameter added to the current loop.
   If fuzzy tuning is pursued at all, it should start as its own small,
   explicit coordinate-descent script (one fuzzy parameter at a time,
   matching the existing "never two together" discipline) built on the
   step-2 single-zone sim harness, rather than a generalization of
   `sim_iter_tune.c`.
7. **Rule-table or bucket-count changes (risk tiers #3-4 in §3) are not
   recommended at this time.** They reopen a design decision
   (`pid_fuzzy.h:17-21`) that was made deliberately to avoid
   per-installation trial-and-error, require a firmware change to
   production control code, and — per §5/§6.2 — cannot be credibly
   evaluated against joint-zone data until the coupling model defect is
   resolved.

## 8. Owner shipping constraint (added mid-task): no fixture-trained fuzzy parameters

**Requirement, as given:** the fuzzy controller must not ship with
parameters trained on this bench fixture. It must bootstrap from the PID
autotune result on whatever kiln it is installed on, and continue to adapt
over subsequent heat cycles on that kiln.

**Why this is correct, stated plainly:** this bench is a ~4 W, 120 V
fixture that cannot exceed roughly 40 °C above ambient. A real kiln runs to
~1200 °C, where radiation (∝T⁴) dominates over the bench's conduction/
convection-dominated regime, and both `k_gain_c_per_duty` and `tau_s` (the
same FOPDT quantities autotune identifies — see `pid_autotune.h:54-57`)
have been separately documented in this repo to fall by roughly 20x from
low-temperature to high-temperature operation
(`firmware/KilnFW/App/drivers/persist/zones_config_json.h:877-878`'s
`high_temperature_transfer_analysis_2026-09-08.md` reference: "every
model_k_dc/model_tau_s/model_dead_time_s fit ever taken was measured at
some real, finite temperature" — the file's own comment already flags this
as a known extrapolation hazard for the *plant model*, and the same
argument applies with equal force to any fuzzy constant fitted here). A
shipped constant tuned on this rig is not merely imprecise elsewhere — it
is fitted to a different physical regime, which is a correct and
independent reason to reject it regardless of the earlier findings in this
document.

### 8.1 Bootstrap: is the fuzzy layer already installation-independent?

**No — checked directly, and the band edges are absolute, fixture-scale
units, not derived from anything autotune measures.**

- `ERROR_BAND_C_DEFAULT 20.0f` (`pid_fuzzy.c:90`) is degrees C of
  **absolute setpoint error**. `RATE_BAND_C_PER_S_DEFAULT 0.5f`
  (`pid_fuzzy.c:91`) is degrees C **per second** of absolute measured rate.
  Both are compile-time constants (now per-zone-config overridable,
  §3 tier #1, but still entered and stored as raw degC / degC-per-s, not
  as a fraction of anything the plant model produces).
  `pid_fuzzy.h:1-33`'s own header comment confirms this was **desk
  reasoning about "a mid-size kiln zone"**, not a derivation from a
  measured plant — i.e. already an admitted case of guessing a constant
  for an installation that had not been measured, just not flagged as a
  shipping hazard until now.
- Compare against what autotune actually measures per zone
  (`pid_autotune.h:54-57`, `zone_cfg_t::model_k_dc`/`model_tau_s`/
  `model_dead_time_s`, `zones_config_json.h:386-388`): `k_gain_c_per_duty`
  (steady-state °C per unit duty), `tau_s` (FOPDT time constant, seconds),
  `dead_time_s` (transport delay, seconds), plus the resulting `kp`/`ki`/
  `kd` (`pid_autotune.h:187-190`) and, for relay-based identification,
  `ku` (ultimate gain, `pid_autotune.h:249`). **None of these are inputs to
  `pid_fuzzy_adjust()` today** — the fuzzy layer only ever sees `error_c`
  and `error_rate_c_per_s`, both raw degC/degC-per-s, compared against a
  raw-degC/raw-degC-per-s band. The gain-multiplication contract (§1) means
  the *output* (a rescale of kp/ki/kd) is already installation-independent
  by construction — a ±25% nudge on whatever kp/ki/kd autotune produced is
  proportionally correct regardless of the zone's absolute scale — but the
  *decision of which cell fires* is not: it is made by comparing an
  absolute-degree error against a fixed-degree band that has no relationship
  to this installation's own identified thermal response.
- **This is achievable to fix, and the fix is a real derivation, not a
  better guess.** A dimensionless membership axis can be built directly
  from what autotune already measures:
  - **Error axis:** normalize error by a band derived from the identified
    static gain and a characteristic duty step, e.g.
    `error_band_c ≈ N × k_gain_c_per_duty × duty_step_reference` for some
    fixed dimensionless `N` (a "how many duty-steps' worth of steady-state
    temperature swing counts as large" choice, which — unlike 20.0f today —
    is a dimensionless design choice, portable across installations by
    construction) or, more directly, some multiple of the FOPDT model's own
    characteristic scale (e.g. the overshoot a P-only step response of this
    plant would produce, which is already a function of `k_gain_c_per_duty`,
    `tau_s`, `dead_time_s`, and the identified `kp`). Either form makes the
    error axis scale with the zone's own identified static gain rather than
    with an assumed absolute degree count.
  - **Rate axis:** normalize by the identified time constant —
    `rate_band_c_per_s ≈ M × (typical error scale) / tau_s` for a
    dimensionless `M`. This directly encodes "how many degrees per time
    constant counts as a large rate," which is exactly the FOPDT-relative
    quantity a 20x-different `tau_s` on a real kiln needs; today's fixed
    0.5 °C/s has no `tau_s` term in it at all, so it cannot track a plant
    whose time constant differs from this bench's by an order of magnitude
    (this document's earlier finding that the bands sit 3-5x wider than
    this rig's own observed envelope, §2, is a direct symptom of exactly
    this — the band was sized for an assumed kiln, not derived from any
    measured one).
  - **Rule table:** the `RULE_TABLE` direction values (§3 tier #3) are
    already dimensionless (∈{-1,0,+1}, a control-law choice about which way
    to move a gain, not a physical quantity) and need no change under this
    requirement — the shipping hazard is specifically in the two band
    constants, not the rule directions.
  - This normalization is a genuinely stronger answer than "ship a default,
    then adapt away from it," because it removes the fixture-specific
    number from the bootstrap step entirely — firing #1 on a fresh install
    computes its own bands from its own just-completed autotune, with zero
    fixture-derived constant anywhere in the path. **Recommendation: this
    normalization should be designed and implemented before any continual
    adaptation scheme (§8.2) is built**, since it removes the single most
    obviously wrong shipped constant (an absolute band picked by desk
    reasoning about "a mid-size kiln") independently of whether continual
    adaptation is pursued at all, and continual adaptation without it would
    be adapting a scale-mismatched quantity from the start.

### 8.2 Continual adaptation across heat cycles

**Mechanism proposed (design only — not implemented in this task):**

- **What is updated:** the two membership-band values, `error_band_c`/
  `rate_band_c_per_s` (already per-zone config, §3 tier #1-2), re-derived
  per §8.1's normalization from whatever `model_k_dc`/`model_tau_s` the
  zone's most recent autotune (or continual re-identification) produced.
  `strength_pct` itself is a candidate second adaptation target (e.g.
  reduced automatically if a firing shows persistent oscillation — see
  §8.5) but should not be the first thing made adaptive, since it directly
  scales how far gains may move and is the parameter most directly tied to
  stability margin.
- **When:** **end of firing only, never per-tick or per-segment.** A
  kiln firing is exactly the kind of slow, high-dead-time process this
  project's own standing practice (`profile_executor` locking guidance,
  the reset-one-side bug class writeup) warns against touching mid-run:
  updating a band while a firing is in progress changes which rule cell an
  in-progress error/rate pair falls into, mid-firing, which is a behavior
  change during an active heat with no operator visibility into why. A
  clean re-identification (autotune re-run, or a lighter-weight passive
  re-estimate from the completed firing's own settled-hold data, if one
  exists) at the *end* of a firing, applied to the *next* firing, keeps the
  adaptation boundary aligned with the same boundary `zones_config_reload`
  and profile-executor state already use, and gives an operator a natural
  point (between firings) to inspect what changed.
- **From what error signal:** not the fuzzy layer's own tracking error —
  that would make the scheme adapt to how well IT is doing, which is
  circular (a band that made tracking look good by never leaving ZERO/
  STEADY, as today's bands already do per §2, would reinforce itself).
  Instead, adapt from the **same signal autotune already produces**: a
  fresh or updated `model_k_dc`/`model_tau_s` fit from the completed
  firing's own step response or settled-hold behavior. This ties adaptation
  to an independent measurement of the plant, not to the fuzzy layer
  grading its own homework.
- **Where persisted:** `zones_config_*` / the `cfg` LittleFS partition,
  the same store `error_band_c`/`rate_band_c_per_s` already live in
  (`ZONES_CFG_VERSION` 18→19, currently at 25 as of this pass). A
  version bump to carry an "adapted" pair of bands (or an adaptation
  history/counter, if bounded drift tracking needs one — see §8.3) needs,
  per this repo's established pattern (v24→25's `coil_power_w` addition,
  `zones_config_json.h:248,911-923`): a frozen `zone_cfg_v25_t` struct (already
  present), a new frozen struct for the next version, a converter function,
  and a CRC check on the migration path — no shortcuts, matching the
  discipline already documented for every prior bump in this file. Any
  *new* standalone NVS key (as opposed to a field inside the existing
  zone-config blob) would need to respect the 15-character NVS key cap
  that broke `zone_normals` (`project_nvs_key_too_long_zone_normals.md`) —
  a field added to the existing struct sidesteps this since it isn't a
  separate NVS key, but a design that gives adaptation its own store (e.g.
  a small history for drift detection) must check this explicitly, not
  assume it's fine because prior fields fit.

> **SUPERSEDED (2026-09-11).** This section designed a new bootstrap-and-
> adapt mechanism without checking whether one already existed. One does:
> `adaptive_tune` (`firmware/KilnFW/App/drivers/control/adaptive_tune.c` /
> `_model.c` / `_ki.c`) already implements bootstrap-from-autotune and
> per-cycle refinement, through the **same SIMC path**
> (`pid_autotune_tune_from_fopdt`) that autotune's own Accept path uses,
> writing the **same persisted zone-config gains** autotune writes, **opt-in
> per zone, default off, with a revert path**. Independently verified
> against source and the live board in
> `docs/audits/adaptive_tune_vs_owner_requirements_2026-09-11.md`.
>
> That audit found `adaptive_tune` satisfies owner requirements (a)
> (bootstrap strictly from an existing autotune result, never invents
> starting gains) and (b) (continues refining every clean firing
> indefinitely) **in code** — but it has **never run**: all three zones on
> this board read `enabled=False` with `observations_lifetime=0`,
> `revert_available=False`. This is off by design, not silently broken —
> it is correctly wired end to end (host-tested, reachable via
> `adaptive_tune_get_status`/`adaptive_tune_set_enabled`) — so this is
> **not** an instance of this repo's consumer-without-producer or
> inert-mode-flag defect classes; it is simply a feature nobody has opted
> a zone into yet.
>
> Requirement (c) — authority graduated by measured confidence — is
> satisfied by **nothing shipped**: every guard in `adaptive_tune`
> (`ADAPTIVE_TUNE_MIN_OBSERVATIONS`, `ADAPTIVE_TUNE_BLEND_ALPHA`,
> `ADAPTIVE_TUNE_MAX_FRACTIONAL_MOVE`) is a fixed constant applied
> identically to the 1st and the 100th accepted refinement — a real,
> open gap, but an additive one layered onto the existing mechanism, not
> a reason to build a second one.
>
> The same audit also found a **ratchet defect** in `adaptive_tune`: its
> K_dc plausibility/blend bounds are anchored to the most-recently-adapted
> value rather than the original autotune result (the reference point
> walks forward with every accepted refinement), and K_dc has no absolute
> ceiling analogous to `ZONE_COUPLING_COEFF_MAX`. A fix for this is
> **in progress by another agent** as of this note; this document does not
> describe its outcome.
>
> **Conclusion: the mechanism proposed below in §8.2 should not be built.
> The work belongs in `adaptive_tune`** — extending its fixed guards into a
> confidence-graduated schedule for requirement (c), and fixing the ratchet
> above — not in a second, competing bootstrap-and-adapt system. The
> original §8.2 text below is left unmodified for the record; do not treat
> it as a design to implement.

### 8.3 Bounded adaptation and safety

This is the section most likely to be gotten wrong, and this repo has
documented exactly how: **"a bound justified against a test constant is
justified against nothing"** (`project_bound_relative_to_persisted_state.md`)
and **"a RAM-latched baseline ratchets when the bounded quantity is
persisted"** (same memory entry). A continual scheme that re-derives its
own bands from its own most recent autotune, and persists that result, is
structurally exactly this hazard: if "the current adapted value" is itself
the reference point the next adaptation step is bounded against, there is
no independent floor/ceiling at all — the bound walks with the value.

Required to avoid this, concretely:

- **Every bound must be anchored to the bootstrap value (the autotune
  result at install/first-run), never to the most recently adapted value.**
  E.g. `error_band_c` may drift only within, say, [0.5x, 2x] of the value
  §8.1's derivation produced from the *most recent full autotune*, not
  [0.5x, 2x] of *last cycle's adapted band*. Storing the anchor separately
  (the bootstrap/last-autotune-derived band, immutable except by a fresh
  autotune) and the adapted value separately is the concrete fix for the
  ratchet hazard — same shape as this project's other per-zone model
  fields already separate "measured" from "in-use."
- **Suppression, not adaptation, during:** any active safety trip or fault
  (thermocouple invalid, guard tripped, E-stop), autotune itself (autotune
  is the source of truth being adapted around — it must not also be an
  adaptation input concurrently), and any aborted/stopped firing
  (`profiles_stop`, a firing that did not complete its planned segments) —
  an incomplete firing's tail data is not a trustworthy plant
  identification and must not update the persisted bands. This mirrors
  `project_autotune_needs_rested_baseline.md`'s finding that a biased
  starting condition corrupts a fit; an aborted-firing "fit" is the same
  class of bad input.
- **Operator revert:** a config action that restores `error_band_c`/
  `rate_band_c_per_s` (and any other adapted field) to the bootstrap/
  last-known-good autotune-derived value, discarding accumulated
  adaptation — the same shape as the existing 0-sentinel convention
  (`control_mode`/`tc_type`/etc.) that already means "use the firmware
  default," extended to mean "use the last-autotune-derived value" for
  these two fields specifically.
- **The Pico's `abs_max_temp_c` stays completely outside this scheme** —
  it is an independent, hard safety ceiling on the RP2040 safety
  processor, has no relationship to the ESP-side fuzzy adaptation, and
  per `feedback_abs_max_same_or_looser.md`'s standing rule must never be
  tightened *or* loosened by anything this scheme does. This adaptation
  proposal touches zero safety-processor state.

### 8.4 Cold start

Firing #1 on a fresh install runs classic, non-adaptive behavior derived
entirely from that install's own first autotune, with **no fixture-derived
number anywhere in the path**:

1. Operator runs PID autotune (existing, unmodified feature) → produces
   `model_k_dc`, `model_tau_s`, `model_dead_time_s`, `kp`/`ki`/`kd` for
   this zone, on this kiln.
2. §8.1's normalization derives `error_band_c`/`rate_band_c_per_s` from
   those measured values (not from `ERROR_BAND_C_DEFAULT`/
   `RATE_BAND_C_PER_S_DEFAULT`, which remain only as the fallback for a
   zone that has genuinely never been autotuned at all — the same role the
   0-sentinel already plays for other fields, and the only place this
   bench's numbers should ever appear: as a last-resort fallback for an
   un-autotuned zone, never as the shipped, intended value).
3. `fuzzy_strength_pct` starts at a conservative, non-zero shipped default
   (a genuine open design choice — this document does not set it, only
   notes it must not be 0 if the feature is meant to run, mirroring §0's
   live finding that 0 makes the whole layer inert) or, more
   conservatively, starts at 0 and is only raised once §8.1/§8.2's
   machinery is confirmed in place — an owner call.
4. Firing #1 runs with these bootstrap bands; no adaptation update happens
   until firing #1 completes (§8.2), and only if it completed normally
   (§8.3).

### 8.5 Convergence

**Cannot be shown to converge in general, and should not be presented as
if it can.** A scheme that re-fits `model_k_dc`/`model_tau_s` from each
firing's own data and re-derives bands from that fit is subject to the
same sources of noise and bias any repeated system identification is:
measurement noise (thermocouple quantization, this rig's own ~5 s sample
interval), a biased starting condition per `project_autotune_needs_rested_baseline.md`
(residual heat from the prior firing biasing a same-day re-fit), and
genuine physical drift (element aging, refractory condition) that a
converging scheme would need to track, not average away. Without an
explicit forgetting/averaging design, consecutive noisy fits can walk
`error_band_c`/`rate_band_c_per_s` back and forth (oscillation) or trend
in one direction indefinitely if a fit bias is systematic rather than
random (drift) — and per §8.3, drift is exactly what an anchor-to-bootstrap
bound is for: it does not make the underlying fit converge, it caps how
far a non-converging fit is allowed to carry the adapted value before the
next full autotune resets the anchor. Concrete guards, none of which prove
convergence but all of which bound the damage from its absence:

- Anchor bounds against the last full autotune (§8.3), not the running
  adapted value — caps drift regardless of whether the underlying fit
  sequence converges.
- A minimum number of qualifying (non-aborted, fault-free) firings before
  the first adaptation update is even applied, and/or an exponential
  moving average across firings rather than replacing the anchor with the
  single latest fit — reduces single-firing noise sensitivity, at the cost
  of slower tracking of genuine drift.
- A periodic full autotune re-run (operator-triggered or interval-based)
  as the actual convergence backstop — the adaptation scheme's job is to
  track a slowly drifting plant *between* autotunes, not to substitute for
  one indefinitely.
- This should be stated to the owner as an open engineering risk, not
  resolved by this scoping pass: a formal convergence proof would need a
  much more specific adaptation-law design (e.g. a bounded-gain recursive
  estimator with a proven contraction property) than "re-fit and average,"
  which is a reasonable first design but not one this document can certify
  converges.

### 8.6 Validation: sim harness for a continual scheme

Per the sim-first constraint (§6) and this document's own finding that
**no closed-loop sim exercises the fuzzy path at all today** (§6.1), a
continual-adaptation scheme needs a harness one level beyond §6's
single-firing single-zone sim:

- **Shape:** a loop of MANY simulated firings (not one), each running
  §6.1's wired-in single-zone closed loop to completion, then applying
  §8.2's end-of-firing adaptation step (re-derive bands from that firing's
  simulated data, apply §8.3's bounds) before the next simulated firing
  starts — i.e. the adaptation state must persist *across* simulated
  firings within one harness run, not reset each time, to actually
  exercise "continue to adapt over subsequent heat cycles."
- **Can `sim_iter_tune.c`'s Monte-Carlo apparatus be reused?** Partially,
  and only for a narrow piece. Its `mc_runs` loop (§4) is built to try many
  *independent* randomized starting conditions and score each once against
  a fixed target — useful for asking "does this adaptation law behave
  safely across a wide range of starting plant-model errors," reusing its
  existing plant-variant randomization (`plant_variant_t`,
  `sim_iter_tune.c:71-73`+). It is **not** built for the sequential,
  state-carrying-across-runs shape a continual scheme needs (each
  Monte-Carlo run in the existing tool is independent and disposable, not
  chained) — the multi-firing adaptation loop above is new machinery, using
  `sim_iter_tune.c`'s plant/scoring primitives as building blocks rather
  than its outer Monte-Carlo driver.
- **What this harness can and cannot validate**, restating §6.2 in this
  context: single-zone, many-cycle adaptation behavior (does the band
  settle, oscillate, or drift over N simulated firings; do the §8.3 bounds
  actually cap excursions; does a deliberately biased/aborted-firing input
  get correctly excluded) is **on firm ground** — single-column transport
  is measured linear, so `sim_plant.c`'s single-zone response is not the
  refuted part of the model. **Any claim about multi-zone joint adaptation,
  or about the scheme converging to a numerically better tracking result
  on a REAL kiln, is not** — the former hits the refuted coupling
  superposition directly, and the latter requires trusting `sim_plant.c`'s
  absolute accuracy, which §6.2 already rules out as a basis for hardware
  improvement claims. A many-firing single-zone sim run can honestly show
  "this adaptation law is stable and bounded across N simulated cycles
  under this plant model" — it cannot honestly show "this adaptation law
  will improve a real kiln's tracking," for the same reason §6.2 gives for
  the single-firing case.

### 8.7 Confidence-driven authority (`strength_pct` rising from 0)

**Owner requirement:** the fuzzy layer should play a stronger part as
confidence in it rises. **Agree with the framing: `strength_pct` is the
correct carrier, not a new mechanism.** It already IS "how much authority
fuzzy has over kp/ki/kd," bounded ±25%/±50% (`MAX_NUDGE_FRACTION=0.5f`,
`pid_fuzzy.c:96`), and it already reads 0.0 on all three live zones (§0) —
a confidence scheme's whole job is to be the thing that moves this one
number up from 0, under evidence, rather than an operator picking a number
by hand. No second knob is needed or proposed.

**1. What confidence must be measured from, and the circularity question
addressed directly.** Four candidate estimators, assessed:

- **Tracking-error improvement, fuzzy-adjusted gains vs. base PID gains,
  on comparable segments.** This is the most direct measure of "is fuzzy
  actually helping" but requires an A/B within or across firings (run a
  segment at the current `strength_pct`, a comparable one at 0, compare) —
  expensive in kiln time and awkward mid-firing profile-fidelity terms
  (§8.2 already rules out adapting mid-firing). Better suited as an
  end-of-firing or paired-firing measurement than a per-tick one.
- **Stability of the adapted parameters across cycles** (do §8.1's
  autotune-derived bands and the resulting rule-cell gain deltas stay
  consistent firing to firing, or do they swing). Cheap — reuses whatever
  autotune/re-identification data §8.2 already collects — and directly
  answers "is this a plant we understand well enough to trust a rescale
  on," independent of whether fuzzy has ever been switched on.
- **Rule-cell coverage** (§2/§8.7.2 below): has this specific cell been
  visited at all, and how many times.
- **Agreement between predicted and actual response** (does the FOPDT
  model's own prediction, using the currently-adapted `k_dc`/`tau_s`,
  track what the thermocouple actually reports during a firing) — this is
  effectively the same signal `zone_coupling_solve.c`'s residual checks and
  the coupling-audit work already compute for the plant model; reusing it
  here means confidence in fuzzy is really "confidence in the plant model
  fuzzy's bands were derived from," which is an honest and defensible
  target.

  **Recommended composite: confidence = f(rule-cell visit count, plant-fit
  stability across the last N firings, predicted-vs-actual agreement on
  the most recent firing).** Explicitly **excluding** the first candidate
  (fuzzy-on-vs-fuzzy-off tracking-error comparison) **as the primary
  driver** — see the circularity analysis below — while allowing it as a
  secondary, occasional check (an explicit periodic A/B, not a per-firing
  automatic signal).

  **The circularity question, addressed directly, since the owner flagged
  it as the subtlest point:** §8.2 already established that adaptation
  must not be driven by the fuzzy layer's own tracking error, because a
  layer that graded its own homework could reinforce a band that merely
  keeps tracking inside ZERO/STEADY (exactly what today's bands already do,
  per §2, with zero adaptive content). **The same argument applies to
  confidence, and for the same reason, whenever confidence is built
  directly from fuzzy-mode tracking error.** A rule that says "tracking
  looked good while fuzzy was on strength=50, so raise confidence" cannot
  distinguish "fuzzy helped" from "fuzzy did nothing because the error
  never left the centre cell" (§2's own finding) from "the base PID gains
  were already excellent and fuzzy's ±25% nudge was noise." **This is why
  the composite above is built from signals independent of fuzzy's own
  output**: plant-fit stability and rule-cell coverage say nothing about
  whether fuzzy helped, only about whether the *inputs* fuzzy's decision
  depends on (the plant identification, the visited cells) are
  trustworthy — which is the right thing for a bootstrap-from-autotune
  design to gate on. The predicted-vs-actual agreement candidate is
  slightly closer to fuzzy's own domain (it is validating the FOPDT model,
  which the bands are derived from, not fuzzy's rule table directly) but
  is still upstream of fuzzy's decision, not downstream of its output — it
  does not close the loop the way "did fuzzy's own tracking look good"
  would. **Position taken: confidence must be built from evidence about
  the plant and about coverage, never from fuzzy's own tracking-error
  outcome as the primary signal, for exactly the same reason adaptation
  itself must not be.** A periodic, explicit, deliberately-triggered A/B
  (not an automatic per-firing signal) is the one place a direct
  fuzzy-vs-PID comparison is legitimate, because it is designed and
  reviewed as an experiment, not silently folded into a rising number.

**2. Per-cell, not per-zone, not global.** The finding that 100% of 2178
mode-3 samples sat in exactly one of nine rule cells (§2) means a single
scalar confidence would let evidence gathered entirely in ZERO/STEADY
license full authority in the NEG/RISING or POS/FALLING cells that have
**never fired once on this hardware**. **Confidence must be per-rule-cell**
(9 values per zone, 27 board-wide across 3 zones) — each cell's confidence
rises only from evidence gathered while that specific cell was active, and
starts at (and reverts toward, see point 3) zero for a cell with no visits.
The applied `strength_pct` for a given tick should then be **the confidence
of whichever cell is currently firing** (or, since membership is
continuous/blended across up to 4 adjacent cells per the Mamdani weighting
in `pid_fuzzy.c:265-279`, a membership-weighted blend of the active cells'
confidences), not a single board-wide number. Cost: 9 floats/zone (27
total) instead of 1, well inside the existing per-zone config blob's
budget (`zones_config_json.h` already carries dozens of floats per zone);
persistence follows the same `zones_config_*`/`cfg`-partition/version-bump
discipline as §8.2's bands, as one more array field. This is a genuine
increase in adaptation-state complexity and should be treated as its own
schema addition, not folded silently into the same version bump as §8.1's
bands.

**3. Confidence must fall — concrete triggers, avoiding the same ratchet.**
Monotonically-rising confidence is exactly the bound-against-latest-value
ratchet already flagged in §8.3, applied to a different persisted
quantity. Evidence that should pull a cell's (or, for a plant-level event,
every cell's) confidence back down:

- **A firing that used this cell's current authority and tracked worse**
  than the plant-fit-stability baseline predicted — direct negative
  evidence against that cell's authority specifically.
- **Any guard trip or fault during a firing** — treat as inconclusive-to-
  negative for every cell active that firing (does not prove fuzzy caused
  it, but a tripped firing is not confirming evidence either, and per
  §8.3's suppression rule its data must not feed adaptation forward at
  all, confidence included).
- **A fresh full autotune that produces `model_k_dc`/`model_tau_s` outside
  a tolerance band of the previous fit** — this is the direct signal for
  "the kiln has physically changed" (element ageing, a rebuild, a
  different load, a new kiln entirely after a board swap). **This should
  reset ALL per-cell confidences for that zone to zero, not decay them
  gradually** — a materially different plant invalidates every rule cell's
  accumulated evidence at once, the same way it already invalidates §8.1's
  bootstrap bands and forces a fresh derivation. This is the concrete
  mechanism for "a kiln that has physically changed gets reset rather than
  coasting on history": the reset is triggered by the autotune delta,
  not by an operator having to remember to do anything.
- **Elapsed time or firing count alone must never raise confidence** — per
  the owner's explicit requirement — but a long *gap* since the last
  qualifying firing (kiln idle for months, moved, serviced) is reasonable
  grounds to require a fresh autotune before resuming adaptation at all,
  treated as a forced re-bootstrap rather than a confidence input.

**4. Interaction with bootstrap — confirmed, and the ramp cannot outpace
evidence by construction.** Confidence starts at zero for every cell on a
fresh install (no autotune history exists to build stability/coverage
evidence from), so `strength_pct` computed from it is 0 for firing #1 —
**this is exactly §8.4's cold start (plain PID on autotune-derived gains)
and is confirmed as the correct, safe result of this mechanism, not a
separate rule that needs to be added on top of it.** Because confidence
is built only from accumulated per-cell evidence (point 1/2), there is no
path for `strength_pct` to rise faster than that evidence exists — the
rate limit in point 5 below is a second, independent safeguard on top of
this, not the only thing preventing an outpaced ramp.

**5. Rate limiting and hysteresis — concrete numbers, justified.**
- **Per-cycle rise cap: no more than +10 percentage points of `strength_pct`
  per qualifying firing, per cell.** Justification: at `MAX_NUDGE_FRACTION
  = 0.5f`, a 10-point step changes the maximum possible single-cell gain
  nudge by exactly 5 percentage points of the base gain (10% of 50%) —
  small enough that one firing's worth of authority increase is very
  unlikely to itself be the difference between a stable and unstable
  firing, given the existing bound already limits the ceiling nudge to
  ±25% at strength 50 (§1) and this project's own measured fuzzy-vs-base
  gain deltas at strength 50 were 12-25% (§2's per-zone table) — i.e. a
  single 10-point step moves the achievable nudge by roughly the same
  order as the *smallest* deltas already observed on real hardware, not a
  step large enough to jump into an unexplored regime in one move.
- **Fall is not rate-limited the same way — a negative-evidence or
  autotune-delta trigger (point 3) should apply immediately, in full**,
  since the asymmetry (fast down, slow up) is the safe direction for a
  heating system: an authority increase should be earned slowly, a
  withdrawal of trust should not wait for a matching countdown.
- **Hysteresis: require 2 consecutive qualifying (non-aborted, fault-free)
  firings showing stable-or-improving evidence before any rise is banked**,
  so a single good firing right after a bad one does not bounce
  `strength_pct` back up immediately — this directly prevents oscillation
  between levels without needing a wider dead-band on the confidence
  estimate itself (which would just delay, not prevent, oscillation if the
  underlying evidence itself is noisy firing-to-firing).

**6. Safety envelope.**
- **Suppress the whole confidence-authority pipeline** (no rise, no fall
  from ordinary evidence — though the autotune-delta reset in point 3
  still applies, since a plant change is a fact regardless of what else is
  happening) **during any active fault, trip, autotune run, or recovery
  mode** (`boot_guard_is_recovery_mode()`, per CLAUDE.md's boot_guard
  section) — same suppression list as §8.3's adaptation suppression,
  applied here to the authority-raising mechanism specifically.
- **Automatic ceiling: `strength_pct` should never be allowed to reach 100
  without an explicit operator action.** A reasonable automatic ceiling is
  50 (the point at which `pid_fuzzy.c`'s comment already documents the
  concrete ±25% nudge this project has actual hardware data about, §2) —
  reaching the full ±50% nudge at strength 100 is a materially larger
  authority than anything ever measured on this hardware and should
  require a deliberate operator decision, not an automatic climb.
- **`abs_max_temp_c` on the Pico stays completely outside this mechanism**,
  as an independent hard ceiling never tightened or loosened by anything
  fuzzy's confidence does — same statement as §8.3, restated here because
  it applies with equal force to the authority-raising path specifically,
  not just to the band-adaptation path.

**7. Validation — blocked on the same missing harness, not a new one.**
This is inherently a multi-firing behavior (confidence accumulates evidence
across cycles), so it needs §8.6's many-simulated-firings harness with
state persisted across chained runs — **the same harness §8.2's continual
band-adaptation validation needs, not a separate one.** Since §6.1 already
establishes that no closed-loop sim exercises the fuzzy path at all today,
**confidence-driven authority cannot be tested in any form — not even a
single-firing sanity check — until that harness is wired.** This sits at
exactly the same point in the ordering as §8.2 in §9 below: after §6's
single-firing harness is built and after §8.1's dimensionless bands are
validated, since a confidence estimate over an unvalidated band derivation
would itself be confidence in the wrong quantity.

**8. Observability.** Per-zone `strength_pct` (already an existing HTTP-only
field, §0) plus, if per-cell confidence (point 2) ships, some rollup of it
(e.g. "cells with any evidence: 3/9" or the currently-active cell's own
confidence) must be visible to the operator. Constraints given: LCD pages
are 480×320 landscape, no scrolling, no new colors. A single numeric
`strength_pct` value (already just a 0-100 percentage) fits trivially
alongside existing per-zone numeric readouts on an existing zones/control
page without a new page or new color — reusing whatever numeric-field
style already renders `Kp`/`Ki`/`Kd` is the direct answer, not a new
widget. A full 9-cell-per-zone confidence breakdown does **not** fit
comfortably in that space without scrolling or a dedicated page, so for
the LCD specifically the recommendation is: show the single active-cell
confidence (or the applied `strength_pct` — the operator-relevant number is
"how much authority is fuzzy actually exercising right now," which is
exactly the blended value from point 2) as one more numeric field, and
reserve the full per-cell 3×3 breakdown for the web UI, which has no such
space constraint and can render it as a small 3×3 grid.

## 9. Recommendation ordering, revisited

**Agree with the coordinator's framing: `fuzzy_strength_pct = 0.0` on the
live board (§0) means there is no live adaptive behavior to preserve and
nothing currently in production to regress, which makes this a good moment
for a structural change rather than an incremental one.** Concretely, this
changes the ordering from §7 as follows:

1. **§8.1's normalization (dimensionless bands derived from autotune) moves
   ahead of the band-rescale recommendation in §7 step 3.** Rescaling
   `ERROR_BAND_C`/`RATE_BAND_C_PER_S` to this rig's own measured envelope
   (§7's prior step 3) would itself be fitting a constant to this bench
   fixture — precisely the shipping hazard §8 identifies. That rescale
   should not ship as a production default at all; it may still be useful
   as a **sanity check that the §8.1 derivation, when evaluated against
   this rig's own autotune numbers, lands in a similar range** (a
   consistency check on the normalization, not a competing shipped value).
2. **§7 steps 1-2 (exercise the untested rule cells; wire fuzzy into a
   single-zone closed-loop sim) are still correct and now do double duty**
   — the same sim harness is the prerequisite for both the original
   tuning-scoping question and §8.6's many-firing adaptation validation.
   Build it once, sized for reuse by §8.6 from the start (state that
   persists across chained firing runs) rather than building a
   single-firing harness first and retrofitting persistence later.
3. **§8.1's dimensionless-band derivation should be implemented and
   validated (single-zone, many simulated plant variants via
   `sim_iter_tune.c`'s existing `plant_variant_t` randomization, per §8.6)
   before any continual-adaptation mechanism (§8.2) is built.** Bootstrap
   correctness is a precondition for adaptation correctness — adapting a
   scale-mismatched quantity compounds the error `sim_iter_tune.c`'s
   dimensional analysis (§8.1) says is already there.
4. **§8.2's continual band adaptation and §8.7's confidence-driven
   `strength_pct` are the largest, riskiest pieces of this whole scoping
   question and should be attempted together, last, as one gated body of
   work — not §8.2 first and §8.7 added afterward.** They share the same
   validation harness (§8.6/§8.7 point 7), the same suppression list
   (faults/trips/autotune/recovery mode), and the same ratchet hazard
   (bound-against-latest for bands, monotonic-rise for confidence) — a
   single review pass covering both is more likely to catch an interaction
   between them (e.g. a band adaptation and a confidence rise landing in
   the same firing) than two separate passes. Gated on: §8.1 shipped and
   validated, §8.6's many-firing sim harness built and showing bounded
   (§8.3), non-diverging behavior across a wide range of simulated plant
   variants for BOTH the band values and the per-cell confidence/authority
   values, and only then a single-zone hardware trial with
   `fuzzy_strength_pct` starting at 0 and rising only under §8.7's measured,
   rate-limited, per-cell evidence — with every §8.3/§8.7 safety property
   (suppression during faults/trips/autotune, anchor-to-bootstrap bounds,
   autotune-delta confidence reset, operator revert, the automatic
   ceiling below 100) implemented and independently reviewed — this is
   online adaptation on a heating system's control gains, and per this
   project's own standing practice on production control-code changes, it
   is an owner sign-off decision, not something this scoping document
   authorizes.
5. **§7's original steps on iter_tune-apparatus reuse and rule-table/
   bucket-count changes (§7 steps 6-7) are unaffected by this section** —
   they remain not recommended for the reasons already given, independent
   of the shipping-constraint question.
6. **Multi-zone/joint-dwell work stays blocked on the coupling-model
   defect for both the tuning question (§7 step 5) and the adaptation
   question (§8.6)** — nothing in this section changes that.

## Answering the owner's question directly

The fuzzy layer does not produce an independent output to "improve" the way
PID's duty output can be improved — it rescales PID's own kp/ki/kd by a
bounded, direction-only rule table, so "improving fuzzy" means improving
the schedule that rescales PID, not a competing control law. Given (a) the
board is currently running with `fuzzy_strength_pct=0` — fuzzy selected but
disarmed, (b) the only real mode-3 hardware evidence is one incomplete
2178-sample arm sitting entirely in one of nine rule cells, and (c) under
the owner's sim-first constraint, **no existing simulation harness
exercises the fuzzy path at all today** — both `sim_iter_tune.c` and
`sim_credibility_gate_closedloop.c` hardcode classic PID — the honest
answer is:
**not yet, credibly** — the prerequisite is collecting a real baseline
(step 2 above) and verifying the untested rule cells offline (step 1
above), both cheap and zero-risk, before any tuning campaign, manual or
automated, can be evaluated against real data rather than against the same
single centre-cell regime already on record.

**Addendum — the shipping constraint (§8) changes what "improve" should
mean here.** The owner's requirement that fuzzy bootstrap from each
installation's own autotune and continue adapting, rather than ship a
fixture-fitted constant, is correct and independently motivated (§8: real
kilns run at a regime where `k`/`tau` differ from this bench by roughly
20x). That requirement also reveals that the two band constants
(`ERROR_BAND_C`/`RATE_BAND_C_PER_S`) are **already** a shipped,
fixture-flavored guess — desk reasoning about "a mid-size kiln," never
derived from any measured plant (§8.1) — so this is not a new defect
introduced by tuning, it is a pre-existing one this scoping task surfaced.
Given `fuzzy_strength_pct=0` live today (§0), there is no adaptive
behavior in production to protect, which makes now the right time for the
structural fix (§8.1's dimensionless, autotune-derived bands) rather than
an incremental rescale (§7's original step 3, which would itself have
shipped another fixture-fitted constant). **Recommended sequencing: ship
§8.1's normalization first (bootstrap-only, no continual adaptation, fully
sim-validatable single-zone per §8.6) as the near-term deliverable; treat
§8.2's continual-adaptation-across-cycles as a separate, larger, later
piece of work gated on §8.1 being validated and on the safety machinery in
§8.3 being implemented and reviewed — not something to bundle into the
same change.**
