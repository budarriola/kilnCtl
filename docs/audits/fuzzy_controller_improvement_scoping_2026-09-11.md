# Scoping: can the fuzzy controller be tuned the way iter_tune tunes PID? (2026-09-11)

**Question posed by owner:** `sim_iter_tune.c`/iter_tune tunes PID gains. Can
the fuzzy logic controller (`pid_fuzzy.c`) be improved the same way?

**No controller behavior is changed by this document.** No board was
flashed and no heating run was performed for this task; all board facts
below are live reads via `kiln_call`.

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
