# PROPOSAL — first validation firing above the ~60 °C tracking ceiling (2026-09-04)

**Status: PROPOSAL ONLY, awaiting owner go-ahead. Nothing in this document is
scheduled. No firing has been started, no config has been written, and the
board has not been touched from this pass.**

## 0. The gap this closes

`tracking_synthesis_20260904_report.md` (§2) established that **none of the
64 raw firing logs in `logs/coupling/` — 27 of them usable, all profile 7 —
ever command a `target_c` above 60.0 °C.** `PID_EXPANSION_PLAN.md` §3.2/§3.6c
carries a load-bearing claim that the coupled hold solve becomes infeasible
above ~62 °C and that the identified coupling matrix is only valid below
~60 °C (own-diagonal figures: 58 °C pre-adoption → 65 °C own-diagonal → 60 °C
hybrid-that-actually-runs, PID_EXPANSION_PLAN.md lines 157-168; simulator
cross-check lines 1178-1205 puts the boundary at ~59 °C old matrix → ~64 °C
new matrix, "matching the ~62 °C → ~65 °C hardware finding in direction and
rough magnitude"). **That "hardware finding" is itself never sourced to a
tracking capture in this dataset** — it is the feasibility-sweep math
(`coupled_hold_feasibility_sweep()`, `tools/PcTools/src/kilnctrl/coupled_ident.py`),
not a measured run. This document scopes the firing that would test it.

## 1. Feasibility and safety envelope — is 65–80 °C permitted today?

**Verified live, read-only, 2026-09-04** (`control_get_zones`, harmless GET
while campaign `fuzzy_ab_20260904d` arm B1 was firing — no write made):

```
Zone 0 (mode 3): range=0..80C
Zone 1 (mode 3): range=0..80C
Zone 2 (mode 3): range=0..80C
```

All three zones' `max_temp_c` ceiling is **80 °C** right now. This matches
`firmware/KilnFW/TODO.md` lines 127-131 (2026-08-28 note): "the test fixture
is capped at 80 C, enforced today by `max_temp_c = 80` on all three zones... a
fixture threshold, not a kiln limit."

**Enforcement path, verified in code:**
- `firmware/KilnFW/App/drivers/profile_executor_run.c` lines 252-271: at
  profile start, every `PROFILE_SEG_KIND_ZONE_RAMP` segment's `target_c` is
  re-checked against the zone's *current* `zones_config_get_temp_limits()`
  ceiling. `target > zone_max_c` is a hard **refusal**, "refused, not
  clamped" (line 264-266) — not a silent clamp. A zone with `max_temp_c==0`
  is skipped here (line 259) but caught by a separate refusal below.
- Lines 301-338: `profile_zones_have_ceiling()` refuses to start any profile
  touching a zone whose `max_temp_c==0` ("no absolute temperature ceiling
  configured... commission the zone... before firing it") — so an
  uncommissioned zone cannot fire at all, at any temperature.
- `firmware/KilnFW/App/drivers/zones_config_accessors.c` line 771:
  `zones_config_set_temp_limits()` input-validates `max_temp_c` against
  `ZONE_MAX_TEMP_C_MAX` (a 2500 °C sanity bound mirrored from
  `profiles_http.c`'s `PROFILE_TARGET_C_MAX`, not a safety ceiling — per
  `profile_executor_run.c`'s own comment at lines 312-315) — this bound is
  irrelevant to a 65-80 °C question, it only stops absurd inputs.

**Conclusion: on the current live board, a profile commanding 65-80 °C is
permitted by every KilnFW-side check that exists** — `max_temp_c=80` on all
three zones is *above* the proposed target band, so nothing in
`profile_executor_run.c`'s refusal chain would block it. This is a
consequence of the fixture ceiling never having been lowered
(`TODO.md`'s "this must come out before a real kiln" note describes the
opposite direction of change, raising it further, not this one).

**The safety processor is a separate story, and here the picture is
materially worse than "permitted."** Per `firmware/KilnFW/TODO.md` lines
132-136 (2026-08-28, not re-verified live in this pass — no MCP tool
surfaced a direct `abs_max_temp_c` readback and this task is read-only):
`abs_max_temp_c` on the SaftyFW side was `set: true, value: 0`, and 0 there
means **never trip** — "the only ceiling in force runs on the same processor
that commands the heat." `safety_get_status()` (checked live, 2026-09-04)
confirms the safety link is up, K4 energized, SaftyFW armed, safety
thermocouple valid at 38.12 °C — the processor is alive and monitoring, but
whether its independent overtemperature trip is armed was not re-verified
here (would require a safety-config readback call, which this task did not
make since it is adjacent to files this task must not touch:
`docs/SAFETY_CASE.md`, `SAFETY_MODEL.md`, `GUARD_TEST_MATRIX.md`).
**This is the item that most needs checking, and re-confirming, before any
higher-temperature firing — not assumed fixed by this document.** If
`abs_max_temp_c` is still 0, a 65-80 °C firing runs with the KilnFW-side
`max_temp_c=80` ceiling as the *only* enforced absolute limit, and that
ceiling lives on the same processor commanding the relays — exactly the
single-point-of-failure TODO.md flagged in 2026-08-28, not something this
proposal changes.

**Owner-only decision, flagged explicitly:** whether to run a hotter firing
before `abs_max_temp_c` is commissioned to a real, non-zero value on the
safety processor. This document takes no position beyond stating the fact —
raising `abs_max_temp_c` is exactly the kind of safety-processor
configuration change this task's guardrails forbid making unilaterally, and
it is the single highest-leverage risk-reduction step available before this
firing. **Recommended, not decided here: commission `abs_max_temp_c` to a
real value (e.g. 90-100 °C, comfortably above the proposed 65-80 °C band but
still tight enough to catch a runaway) before running this firing.**

## 2. What breaks, predictively — a falsifiable prediction

From the plan's own feasibility math (PID_EXPANSION_PLAN.md §3.2, lines
141-168, 1178-1205):

- The **hybrid matrix that actually runs today** (measured off-diagonals,
  `ff_k_dc` on the diagonal — confirmed this is what's live, since
  `coupling_diag_k_dc` reads 0.0/"never identified" for all three zones per
  the live `control_get_zones` read above, so
  `s_coupling_use_measured_diag_k_dc` cannot be doing anything but falling
  through to `ff_k_dc`) has a feasible range ceiling of **~60 °C** (line
  161).
- Simulator cross-check (line 1183) puts the boundary at **~64 °C** for the
  new matrix, "matching the ~62 °C → ~65 °C hardware finding in direction
  and rough magnitude" — but as established above, that "hardware finding"
  has no tracking capture behind it in this repo; it is presumably an
  operator's field observation, not logged data this analysis can point to.
- **Prediction: somewhere in the ~60-65 °C band, `zone_coupling_solve_hold()`
  starts returning `_SINGULAR`/`_NONFINITE` or the solve becomes
  infeasible**, and the zone(s) affected fall through to the **uncoupled 1x1
  fallback** — `diagonal_hold()`/`diagonal_climb()` in `zone_coupling_solve.c`
  (confirmed current as of the 2026-09-03 "CLOSED" pass, lines 338-391),
  which divides by `coupling_diagonal_k_dc(zi, z_ff_k_dc, use_measured)` —
  and since `coupling_diag_k_dc` is unmeasured on this board (0.0 for all
  three zones, confirmed live above), that resolves to plain `z_ff_k_dc`,
  i.e. the same single-zone feedforward gain used before any coupling work
  landed.
- **What this looks like in telemetry**: `bd_ff_hold`/`bd_ff_climb` stop
  reflecting the coupled solve's cross-zone correction and `ff_hold_infeasible`
  flips true (field confirmed live in `dashboard_json.c` lines 103-105,
  116); `bd_coupling_correction` should collapse toward the value the
  fallback alone produces, and `ff_hold_used_matrix` should flip false for
  the affected zone(s) at the same tick. Above full duty saturation
  (line 1201: "duty is fully saturated at every band from ~65 °C upward" in
  the simulator sweep, at this rig's measured `K_diag`≈35-38 °C rise per
  duty=1), the falsifiable prediction sharpens further: **the affected
  zone's commanded duty should pin at 1.0 and hold there through the dwell**,
  not settle — a directly observable, unambiguous telemetry signature
  distinct from ordinary dwell-entry overshoot.

**The falsifiable prediction, stated as one sentence:** *between 60 and 65
°C dwell, at least one zone's `ff_hold_infeasible` flips true, its
`ff_hold_used_matrix` flips false, and its commanded duty pins near 1.0 and
fails to settle for a materially longer stretch than the same zone shows at
its 55-60 °C profile-7 dwell baseline (mean ~0.6-0.8 °C offset, §3 of the
synthesis report) — and if none of that happens up to 80 °C, the
feasibility-sweep math is wrong about where the real board's boundary sits,
not just imprecise about it.*

## 3. Proposed run design

**Target(s):** two arms, run back to back on the same rested-cold-start
protocol every prior campcampaign in this dataset used (§ "Autotune needs a
rested baseline" — every zone at ambient before start, not mid-cool-down):

- **Arm LOW (control)**: profile 7's existing 60 °C dwell, unmodified — a
  same-day baseline run on *this* board's *current* configuration, since the
  27 usable captures span two matrix eras and this board's exact present
  state (unmeasured `coupling_diag_k_dc`, current hybrid) was never itself
  the subject of a same-day 60 °C capture in this dataset.
- **Arm HIGH**: profile 7's shape (ramp segments unchanged) but with the
  final dwell segment's target raised from 60 °C to **70 °C** — inside the
  80 °C fixture ceiling with 10 °C of margin, and 5-10 °C past every
  predicted infeasibility boundary above (60/62/64/65 °C), so the run is
  positioned to actually cross the boundary rather than sit just short of
  it. Ramp rate unchanged (profile 7's own rate; already validated feasible
  at ≤900 °C/hr ceiling from the live `control_get_zones` read, and profile
  7 has never exercised anywhere near that ceiling).
- **Dwell length**: long enough to see the fallback behaviour settle or fail
  to settle — recommend **45-60 min** at the 70 °C dwell (longer than
  profile 7's stock dwell, since the thing being measured is whether the
  duty ever comes off saturation, which the synthesis report's own segment-1
  window, "hundreds of samples," suggests needs real dwell time, not a token
  few minutes).
- **Total duration per arm**: profile 7's existing ramp time to 70 °C (a
  modest addition over its stock 60 °C ramp) + 45-60 min dwell + standard
  cooldown tail already captured by every other file in this directory
  (`.cooldown.jsonl` convention) — roughly **2-2.5 hours per arm**, in line
  with this dataset's existing captures.
- **Repeats**: the project's own `CONSISTENT_PATTERN_MIN_KEYS` rule (line
  762, "≥3-zone... bar") and its noise floors (z0 0.116, z1 0.077, z2
  0.147 °C whole-run normalized IAE, PID_EXPANSION_PLAN.md line 1550) are
  built for *comparing two configurations*, not for characterising a single
  new regime. This is not that kind of question yet — the first firing's job
  is to establish **whether the predicted mechanism (§2) fires at all**, a
  yes/no/where question, not a magnitude-vs-noise-floor question. **One
  exploratory HIGH arm (plus one LOW-arm same-day baseline) is enough to
  answer "does the fallback engage, and where."** It is emphatically **not**
  enough to characterise how bad the resulting tracking error is with the
  ≥3-zone same-direction confidence bar this project holds every A/B
  campaign to — that would need the same **3-repeat-per-arm structure**
  every other campaign in `logs/coupling/` used (`ab_old_{1,2,3}`,
  `ab_new_{1,2,3}`, `easeoff_ab_*_run{1,2,3}`), i.e. **3 HIGH + 3 LOW = 6
  firings, ~12-15 kiln-hours**, if the exploratory pair shows something
  worth quantifying. Be honest about the cost split: **1 exploratory pair
  (~4-5 hours) buys "does this happen"; the full 6-firing campaign
  (~12-15 hours) buys "how much, with what confidence."** Recommend running
  the exploratory pair first and deciding on the full campaign from its
  result, rather than committing 12-15 kiln-hours up front to a prediction
  that might turn out qualitatively wrong (e.g. if the boundary is actually
  at 75 °C, not 60-65 °C, the profile needs redesigning before a 6-firing
  campaign is worth running).

## 4. Instrumentation — exact fields, not "capture everything"

Per §3.6b/§3.6c's standing pre-flight discipline (a campaign was found
structurally inert today for want of exactly this check) and the fact that
the 12 `bd_*` breakdown fields per zone are now captured
(`a62e14e`), this run's capture must include, per zone, sampled at capture
rate through the dwell (not just spot-checked pre-flight, since the
mechanism under test is a transition *during* the dwell, not a static
config difference between two arms):

- **`ff_hold_infeasible`** — the direct flag; this is the mechanism's
  headline signature. Expect `false` throughout the LOW arm's 60 °C dwell,
  and watch for a transition to `true` on the HIGH arm somewhere in the
  60-70 °C climb/dwell.
- **`ff_hold_used_matrix`** — should flip `false` on whichever zone(s) fall
  into the fallback, at the same tick `ff_hold_infeasible` flips true (they
  are read from the same struct write, per `dashboard_json.c` line 3's own
  comment grouping them).
- **`ff_membership_change_count`** — coupling membership churn is already
  documented as routine ("every commissioned kiln sees a few of these across
  a run," profile_executor_feedforward.c per PID_EXPANSION_PLAN.md line
  212); a spike correlated with the infeasibility transition would suggest
  membership churn (not just gain saturation) is a contributing trigger,
  worth distinguishing from a clean gain-saturation transition.
- **`bd_ff_hold`, `bd_ff_climb`, `bd_coupling_correction`** — the three
  breakdown terms the coupled solve outputs; expect `bd_coupling_correction`
  to collapse toward zero/noise the instant the fallback engages (the
  fallback is single-zone, it has no cross-zone term to report).
- **Commanded duty** (already in every capture's standard fields) — the
  sharper, model-independent tell per §2: full saturation (duty pinned near
  1.0, not settling) is the simulator's own predicted symptom at ≥65 °C, and
  needs no `bd_*` interpretation to read.
- **`heat_blocked`, `heat_blocked_sources`** — rule out an interlock trip
  masquerading as an infeasibility transition (both are read from the same
  struct dump, `dashboard_json.c` lines 103-104).

Recommend running `bd_reachability_check`-style diffing (not the literal
tool, which compares two arms' *reachability* of a config field — here the
comparison is LOW-arm-dwell vs HIGH-arm-dwell values of the same fields,
a different question) or simply eyeballing the four flags above across the
HIGH arm's climb-through-dwell window first, before any statistical
analysis — the prediction in §2 is binary enough to read by eye.

## 5. Risk review — go/no-go list

This would be the hottest firing run on this rig in this dataset (max seen:
60 °C profile-7 dwell; fixture ceiling 80 °C). Items to check before
lighting it, in order of how much they change relative to every prior
firing in this repo:

1. **Safety processor `abs_max_temp_c` commissioning — OWNER DECISION,
   FLAGGED.** Per §1: documented as `0` ("never trip") as of 2026-08-28, not
   re-verified live in this pass. If still 0, the *only* ceiling protecting
   a 70 °C firing is the KilnFW-side `max_temp_c=80` — on the same processor
   commanding the relays. Recommend commissioning a real non-zero value
   before this firing, but that recommendation is not authorization to make
   the change unilaterally — it touches SaftyFW-adjacent config this task
   was told not to touch, and the actual commissioning is a `safety_cfg_http`
   POST, a write, explicitly out of scope for this pass.
2. **Guard headroom.** `profile_executor_run.c`'s guard-5 refusal (lines
   301-338) already requires every participating zone to have a non-zero
   `max_temp_c` — satisfied (80 °C on all three, confirmed live). No new
   guard becomes newly *reachable* by raising the dwell target from 60 to 70
   within an already-commissioned 80 °C ceiling; what changes is how close
   the run sits to that ceiling (10 °C margin at 70 °C dwell target, vs the
   much larger margin every prior 60 °C capture ran with — 20 °C).
   Recommend NOT proposing an 80 °C target for this first run precisely to
   preserve that 10 °C margin against the hard ceiling, in case of any ramp
   overshoot on top of the dwell-entry overshoot already measured at ~2 °C
   for z0 (§3 of the synthesis report).
3. **Thermocouple / cold-junction range.** MAX31856 channels and the K/other
   TC type in use are rated far beyond 70-80 °C (kiln-grade thermocouples
   are typically rated to hundreds or low thousands of °C) — not a concern
   at this band. Not independently re-verified against the specific
   commissioned `tc_type` in this pass (read-only board access did not
   surface it); flagging as a cheap pre-flight check, not a blocker.
4. **Relay duty / contactor wear.** §2's predicted failure mode is duty
   pinned near 1.0 for an extended dwell — more continuous relay-on time
   than any 60 °C dwell in this dataset (which settle to the ~0.4-0.9 °C
   offset numbers in §3 of the synthesis report, implying duty modulation,
   not saturation). If the prediction is right, this run will exercise the
   relays harder (fewer but longer on-periods, less cycling) than anything
   run before. Not a stop-ship risk for a 45-60 min dwell on hardware rated
   for continuous kiln duty, but worth noting as a real, if modest, change
   in wear pattern versus every prior capture.
5. **Newly reachable guard.** Nothing identified in KilnFW that a 65-80 °C
   target uniquely reaches beyond what 60 °C already exercises, given the
   80 °C ceiling is already commissioned and unchanged. The one guard this
   band could plausibly newly reach is the safety processor's own
   overtemperature trip *if and only if* `abs_max_temp_c` is raised to a
   value inside this band as recommended in item 1 — in which case that
   guard firing during the run would be the safety system doing exactly its
   job, not a defect; the abort criteria below should treat a real S-side
   trip as an expected, acceptable, non-alarming outcome, not a failure of
   the experiment.
6. **Abort criteria, proposed:**
   - Any zone's thermocouple reports a fault (per `thermo_read_faults`)
     during ramp or dwell — stop immediately, this dataset's existing
     convention.
   - Commanded duty pins at 1.0 for more than [operator judgment — suggest
     10-15 min] past the predicted infeasibility point *without* the
     temperature converging toward the 70 °C target — this is the
     "coupled hold genuinely cannot hold this temperature" failure mode the
     experiment is designed to detect, and continuing past it teaches
     nothing further while extending relay-on time for no benefit.
   - Any zone approaches within 5 °C of the commissioned `max_temp_c=80`
     ceiling outside the intended dwell (i.e. an overshoot bigger than the
     ~2 °C z0 has ever shown) — stop and let it cool; this is the margin
     item 2 above exists to protect.
   - A safety-processor trip (if `abs_max_temp_c` has been commissioned) —
     let it trip, do not override, treat as a valid experimental data point
     about where that ceiling sits, not an incident.
   - Standard interlock/E-stop discipline unchanged from every prior firing
     in this repo.

**Summary go/no-go:** the KilnFW-side configuration permits this firing
today (§1, verified). The one item that should change before it runs is
owner-decided and firmware/safety-config-adjacent (`abs_max_temp_c`
commissioning) — not assumed done, not made here. Everything else in this
review is a "proceed with the listed margin and abort criteria," not a
blocker.

## 6. Cost summary

- **Exploratory pair (recommended first step)**: 1 HIGH (70 °C) + 1 LOW
  (60 °C, same-day baseline) firing, ~4-5 kiln-hours total. Answers: does
  the predicted infeasibility/fallback mechanism engage, and roughly where.
- **Full confirmatory campaign (only if the exploratory pair shows
  something)**: 3 HIGH + 3 LOW firings matching this project's own
  ≥3-zone-same-direction confidence bar, ~12-15 kiln-hours total. Answers:
  how large is the effect, with the same statistical discipline every other
  campaign in `logs/coupling/` was held to.
- **Prerequisite, owner-decided, not costed in kiln-hours**: commissioning
  `abs_max_temp_c` on the safety processor to a real non-zero value.

---

*This document is a proposal prepared offline on 2026-09-04 while
`fuzzy_ab_20260904d` was live on the board. It commands nothing, changes no
configuration, and is not scheduled work — see PID_EXPANSION_PLAN.md's link
to this file for the owner go-ahead this needs before any of it runs.*

---

# Independent verification and corrections (2026-09-04, second pass)

The feasibility premise was recomputed directly from
`coupled_hold_feasibility_sweep()` on the hybrid matrix that actually runs,
and the predicted telemetry signatures were checked against the flags that
**are already in every existing capture**. **The proposal is worth its
~4-5 kiln-hours — but three of its statements are wrong and one of its two
predictions is likely to fail for a reason that would be misread as
"the math is wrong".**

## X1. The boundary is `ambient + 38.0 °C`, not "~62 °C"

Sweep re-run with the live diagonal (`model_k_dc` 39.246 / 31.967 / 31.681)
and the measured off-diagonals:

| ambient | first infeasible (current hybrid) | first infeasible (old matrix) |
|---|---|---|
| 20.0 °C | 58.5 °C | 56.5 °C |
| 24.5 °C | **63.0 °C** | 61.0 °C |
| 30.0 °C | 68.5 °C | 66.5 °C |

The offset is **exactly +38.0 °C above ambient** at every ambient (old
matrix: +36.0). The captures' own ambient runs **24.3–28.8 °C**, so for this
rig as operated the boundary sits at **62–67 °C, not a fixed 62 °C**.
§0/§2's absolute framing (inherited from PID_EXPANSION_PLAN §3.2) should be
restated as an ambient offset — it is the single most misleading number in
the chain.

**Does 70 °C cross it? Yes, comfortably** — it would take an ambient above
**32 °C** for a 70 °C dwell to remain feasible. The proposal's headline
choice of target is **correct as designed.** 65 °C would have been marginal
(needs ambient < 27 °C); 70 °C is the right call.

## X2. It is z2 that goes infeasible — name it

At the boundary the solved duty vector is **u = [0.103, 0.460, 1.008]**:
**z2 saturates first and alone; z0 sits at 0.10 and never saturates.** §2's
"at least one zone" and §4's "whichever zone(s)" should be sharpened to a
named, pre-registered prediction: **zone 2.** If z0 or z1 is the one that
flips, the matrix is wrong in a way the sweep does not anticipate — a much
more interesting result than a vague hit.

## X3. Existing captures already carry the flags — the negative half is proven

§0 says the >62 °C claim "has no tracking capture behind it". True for
tracking *error*, but **`ff_hold_infeasible` and `ff_hold_used_matrix` are
per-zone fields in all 30 parseable captures in this directory.**
`ff_hold_infeasible` is **false in every sample of every capture**;
`ff_hold_used_matrix` is **true throughout**. So the LOW arm's flag data is
already in hand, n=27, and consistent with a boundary above 60 °C. The LOW
arm still earns its place as a same-day tracking baseline — but it should be
costed as that, not as flag evidence.

Also: `bd_ff_hold`, `bd_ff_climb` and `bd_coupling_correction` are already
being captured (all twelve `bd_*` fields, 479/479 control rows in the live
`fuzzy_ab_20260904d_s50_run1.jsonl`), so §4's instrumentation list needs no
new work — it is the current CLI default.

## X4. The two predictions must be separated — the second is likely to fail

§2 bundles them into one sentence. They have very different value.

**(i) The flag prediction** — `ff_hold_infeasible` flips true,
`ff_hold_used_matrix` flips false, on z2, at ambient+38 °C. This is a pure
restatement of arithmetic the board performs on a matrix already in NVS.
**It is near-certain and computable offline; it tests that the firmware
implements the sweep, nothing about the plant.** Low information for
4-5 kiln-hours.

**(ii) The consequence prediction** — z2's duty pins at 1.0 and fails to
settle. **This one is predicted here to FAIL, and for a reason that has
nothing to do with the sweep's arithmetic.** Comparing the matrix's
predicted steady-state duties against measured dwell duty at the 60 °C
dwell:

| capture | ambient | predicted u (z0/z1/z2) | measured dwell duty |
|---|---|---|---|
| `ab_new_1` | 24.3 °C | 0.096 / 0.431 / **0.945** | 0.161 / 0.415 / **0.728** |
| `ab_new_2` | 26.3 °C | 0.090 / 0.400 / **0.876** | 0.159 / 0.400 / **0.689** |
| `ab_new_3` | 28.7 °C | 0.083 / 0.371 / **0.812** | 0.161 / 0.383 / **0.643** |

z1 is predicted almost perfectly. **z2 — the zone that determines the
boundary — is over-predicted by 20–25% every time.** Extrapolating from the
measured z2 duty (0.728 at ΔT = 35.7 °C) puts the **physical** saturation
point near **ΔT ≈ 49 °C, i.e. ~73 °C at 24 °C ambient — roughly 10 °C above
where the solver declares infeasibility.** So the most likely outcome of the
HIGH arm is: **the flag flips at ~63 °C, the fallback engages, and the kiln
then tracks 70 °C perfectly well on ~0.9 duty.**

That is a genuinely useful result — it would say the solver's feasibility
gate is **conservative by ~10 °C** because the matrix over-states z2's own
gain requirement, and that the gate, not the plant, is what will limit
cone-range firing. But §2's closing sentence ("if none of that happens up to
80 °C, the feasibility-sweep math is wrong") would then be **misread**: the
sweep's arithmetic would be right and its *input matrix* wrong. **Restate
the falsification criteria as three separate outcomes:**

- flag flips near ambient+38 °C **and** tracking degrades → sweep validated
  end to end;
- flag flips near ambient+38 °C **and** tracking stays clean → **the
  expected outcome**; the gate is conservative, the z2 row of the matrix is
  over-estimated, and the actionable follow-up is re-identifying z2's
  diagonal (`coupling_diag_k_dc`, currently 0/never measured on all three
  zones) rather than anything about the profile;
- flag does not flip by 70 °C → the matrix or the diagonal fallback in NVS
  is not what this analysis believes is loaded; stop and re-read the config
  before drawing any plant conclusion.

## X5. The ≥3-zone rule reference in §3 is a misreading (same as §3.6d's)

`CONSISTENT_PATTERN_MIN_KEYS = 3` counts distinguishable **(zone, metric,
segment) keys**, not zones. §3's reasoning about the rule not fitting a
"single new regime" question reaches the right conclusion (an exploratory
pair is the right first step) by the wrong route — the rule would in fact be
satisfiable here; it is simply not the question the first firing asks.

## X6. Safety review — unchanged and correct

§1's code reading was spot-checked and stands: `max_temp_c = 80` on all
three zones, `profile_executor_run.c`'s per-segment ceiling check refuses
rather than clamps, and the `abs_max_temp_c = 0` flag on the safety
processor is the correct top item. **That recommendation is not weakened by
anything above** — if anything X4 strengthens it, since the expected outcome
involves z2 running at ~0.9 duty for a 45-60 min dwell.

## X7. Verdict

**RUN the exploratory pair, with the corrections above applied:** boundary
quoted as ambient+38 °C, z2 named as the predicted zone, the three-way
falsification criteria replacing the single bundled prediction, and the LOW
arm costed as a tracking baseline rather than flag evidence. **Do NOT
commit the 12-15 hour confirmatory campaign on a flag transition alone** —
per X4 the flag will almost certainly flip whether or not anything
physically interesting happens, so "the exploratory pair showed something"
must mean the *tracking* half moved, not the flag half.
