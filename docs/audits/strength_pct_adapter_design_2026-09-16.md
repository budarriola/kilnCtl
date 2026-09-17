# `strength_pct` cross-firing adapter: design (WI-10)

Date: 2026-09-16. `docs/SCENARIO_SIMULATION_PLAN.md` sec 6.2/7 WI-10:
"Design (do not build) a `strength_pct` adapter, tested in simulation only."
This document is the design; `firmware/KilnFW/App/test/sim_strength_pct_adapt.c`
is the simulation arm that exercises it. **No firmware change. No hardware.**
`strength_pct` itself (`pid_fuzzy.h`/`zones_config_*`) is untouched -- this is
a design for a policy that would eventually pick its value across firings,
not a change to the knob.

## 1. Premise check, done before writing any of this

`docs/audits/scenario_simulation_implementation_2026-09-14.md` records WI-1
through WI-8 done, WI-9 dropped (its premise -- a fuzzy/Ki mutual-exclusion
guard -- was deleted outright by `88bb4333`), WI-10 not started. Re-checked
against this worktree's HEAD (`ce83c580`, `origin/main`) before writing a
line of this file:

- `strength_pct` is a live, shipped scalar (`firmware/KilnFW/App/drivers/control/pid_fuzzy.h`,
  `zones_config_accessors.c`'s `zones_config_get_fuzzy_strength_pct()`/
  `_set_fuzzy_strength_pct()`, `zones_http_pid.c`'s HTTP surface) with a
  bit-exact identity at 0 (re-verified live by `sim_scenarios.c`'s own
  `all_bitexact` check and restated in this file's own test below). The
  premise sec 6.2 states -- "a single scalar with a monotone, bounded effect
  and a bit-exact identity at 0" -- still holds.
- `firing_compare.c`/`.h` (Bar 1 / Bar 2 / the no-degradation veto) is live,
  unchanged in shape since sec 6.2 was written, and is exactly the mechanism
  sec 6.2 names as the only defensible score for this adapter.
- The refuted design this document must not repeat
  (`docs/audits/adaptive_tune_confidence_authority_design_2026-09-13.md`,
  reviewed in `1142c73b`) is about a *different* mechanism -- a per-run
  residual-based confidence estimator for `adaptive_tune`'s Ki path -- not
  about `strength_pct` at all. Its failure mode (scoring a "forward"
  residual partly against the run's own training data, because the
  observation ring is never cleared at a run boundary) has no analogue here:
  this design never reads any state internal to the firing it is scoring
  from a *previous* firing's fit. See sec 3 for why.

Premise holds. Proceeding to design + simulation arm, not a plan amendment.

## 2. What is being designed

A **cross-firing scalar hill-climb over `strength_pct`**, using
`firing_compare`'s existing Bar 1 / Bar 2 machinery to accept, reject, or
declare insufficient each proposed step, with a hard revert on any reject.
It is deliberately as small as `iter_tune.c`'s own accept/reject shape
(`ITER_TUNE_REDESIGN_PLAN.md`) already establishes for PID gains -- this
design reuses that same posture for one more scalar rather than inventing a
new mechanism.

### 2.1 State carried across firings

Per zone:

```
strength_pct_current   -- the value actually run last firing (the "baseline")
firing_score_baseline  -- that firing's own firing_score_set_t (Bar-1/2 input)
step_pct               -- current step size, signed (direction), magnitude only shrinks
firings_since_accept    -- for reporting/wander detection only, not a gate
```

### 2.2 One adaptation step (one firing boundary)

1. Propose `candidate = clamp(strength_pct_current + step_pct, 0, 100)`.
2. Run the **next** firing at `strength_pct = candidate` (a real, separate
   firing -- see sec 3 for why it must be a different firing and never the
   same one already scored).
3. Score that firing into its own `firing_score_set_t` ("trial").
4. Call `firing_compare(&firing_score_baseline, &trial, NULL, &result)`.
5. Act on `result.verdict`:
   - `FIRING_COMPARE_ACCEPT`: adopt the candidate --
     `strength_pct_current = candidate`, `firing_score_baseline = trial`.
     Step size may grow (bounded) on a second consecutive accept in the same
     direction, mirroring `iter_tune`'s own step-schedule idea, but growth is
     out of scope for WI-10's simulation arm (see sec 5 -- the arm uses a
     fixed step and reports whether even that converges before proposing a
     schedule).
   - `FIRING_COMPARE_REJECT_DEGRADED`: do NOT adopt. Flip `step_pct`'s sign
     and halve its magnitude (bounded below by a floor, e.g. 5 percentage
     points) -- the same bracketing idea a 1-D line search uses when it
     overshoots.
   - `FIRING_COMPARE_INSUFFICIENT` or `FIRING_COMPARE_NO_MATCHED_PAIRS`: do
     NOT adopt, do NOT change direction -- this pair simply had nothing to
     say (thin matched-class evidence, or none). Retry the SAME direction
     at the SAME magnitude next firing, up to a bounded retry count, after
     which the step is halved anyway to avoid stalling forever on a
     scenario whose segment classes rarely re-occur.
6. `strength_pct_current` for the NEXT proposed firing is always the last
   *adopted* value, never the last *tried* one -- a rejected candidate must
   not become next firing's baseline; that is the hard revert.

### 2.3 Termination / reporting, not a hidden pass criterion

The design does not claim a fixed number of firings suffices to "converge."
It reports, per (scenario, zone) chain:

- the full `strength_pct_current` trace across the chain,
- how many steps were ACCEPT / REJECT_DEGRADED / INSUFFICIENT /
  NO_MATCHED_PAIRS,
- whether the trace settled (last K steps within one step-floor of each
  other), oscillated (repeated direction flips without settling), or
  wandered (net drift with no settling and no consistent direction).//

**A trace that wanders or fails to converge is a complete, valuable result
per WI-10's own acceptance line -- record it and stop.** This design does
not treat non-convergence as a design defect to be patched until it
converges; forcing convergence by tuning the step schedule against the
scenarios in this suite would be the same overfitting-to-the-harness mistake
`docs/audits/fuzzy_must_not_ship_fixture_trained` (memory) already names for
the fuzzy layer itself.

## 3. Why this is not circular

Sec 6.2's rule: **"a fuzzy strength adapter must never be scored by a
quantity the fuzzy layer itself moved [within the same run]."** This design
satisfies it structurally, not by argument alone:

- The score that adjudicates step N (`firing_compare(baseline, trial, ...)`)
  is built from **two entirely separate, already-completed firings' own
  `firing_score_set_t`s** -- baseline is firing N-1's segment scores, trial
  is firing N's. Neither firing's `pid_fuzzy_adjust()` calls ever see the
  other firing's score, because a firing's score is only computed AFTER
  that firing's simulation loop finishes (same ordering `sim_scenarios.c`'s
  `firing_score_seg_finish()` already uses). There is no residual, no
  ring, no within-run inference anywhere in this design -- it is a
  same-shape A/B as `iter_tune.c`'s own gain acceptance, generalised to one
  more scalar.
- Contrast with the refuted design's actual defect: there, the SAME run's
  observation ring fed both the fit and the "forward" residual that scored
  it, because the ring was never cleared at the run boundary
  (`adaptive_tune.c:365-380` at the time of that review). This design has
  no ring, no fit-then-residual-on-the-same-data step at all -- the only
  thing carried across firings is the scalar knob's value and the
  *previous* firing's already-finalised score, which is exactly the
  "different firing" comparison sec 6.2 requires.
- `strength_pct=0`'s bit-exact identity means a REJECT's revert is exact,
  not approximate: reverting to `strength_pct_current` reproduces the
  previous firing's controller behaviour bit-for-bit if that value is 0,
  and reproduces it functionally (same formula, same inputs) at any other
  value, because nothing about `pid_fuzzy_adjust()` retains state between
  calls that this design does not already carry explicitly
  (`pid_fuzzy_derive_bands()` depends only on the zone's belief model, which
  this design does not touch).

### 3.1 Selection bias from excluded non-settling runs

Sec 6.2's rule also names selection bias as the real leak in the refuted
design. This design has the same structural exposure `iter_tune.c` already
has, named plainly rather than argued away:

- A firing that never harvests any matched segment classes (e.g. a scenario
  refuses before reaching a scoreable dwell) produces `NO_MATCHED_PAIRS` and
  is excluded from the accept/reject decision by construction (sec 2.2 step
  5) -- it neither helps nor hurts the trace, it is simply silent. If a
  particular DIRECTION of `strength_pct` change were correlated with
  producing unscoreable firings (e.g. a large positive step destabilises the
  loop enough to blow the plant bounds and refuse before any segment
  finishes), the chain would systematically retry in that direction without
  ever being told "no" by `firing_compare` -- an accept-permissive gap
  structurally similar to the one sec 6.2 cites, but via refusal rather than
  ring contamination.
- **Mitigation, stated as a requirement on any future implementation, not
  yet built in the simulation arm:** a firing that refuses (NaN, out of
  bounds, autotune refusal) must count as an implicit REJECT of whatever
  step produced it, not as a no-op retry -- otherwise the retry-same-
  direction rule in sec 2.2 step 5 would walk the knob toward instability
  one refusal at a time with no accept ever required. The simulation arm
  built for WI-10 (sec 5) implements this: a refused firing is scored as
  REJECT_DEGRADED for adaptation purposes and reported with an explicit
  `REFUSED_TREATED_AS_REJECT` tag, so the bias is visible in the trace
  rather than hidden by silence.
- This does not close the general case (a step that degrades tracking just
  short of a refusal threshold is still invisible to this particular
  mitigation), which is exactly the kind of residual gap sec 6.2's own
  ground rule expects to be named rather than declared solved. A firmware
  implementation would need a real accept-rate audit across many more
  firings than any bench campaign could run -- another reason this stays
  simulation-only per WI-10's own scope.

## 4. What this design deliberately does NOT do

- **No rule-consequent learning.** Sec 6.3's verdict (27 parameters,
  unreachable cells, offline-design-only) is unaffected; this adapter only
  ever moves the single `strength_pct` scalar.
- **No band adaptation coupling.** `pid_fuzzy_derive_bands()`'s own
  model-driven adaptation (sec 6.1, already shipped) runs independently;
  this design does not attempt to jointly optimise bands and strength, which
  would reopen exactly the "which lever moved the result" attribution
  problem sec 6.1 already flags for the bands alone.
- **No firmware/NVS/HTTP surface.** No accessor, no persisted adapter state,
  no board-facing knob. The simulation arm below is the only place this
  logic runs.
- **No claim about the real kiln.** Per sec 0.1's standing caveat, a
  convergent trace in this simulator says the adapter is well-posed *under
  this plant model*; it is not evidence the real strength_pct chosen this
  way would improve a real firing.

## 5. The simulation arm

`firmware/KilnFW/App/test/sim_strength_pct_adapt.c` (new file, own
executable `kilnctl_sim_strength_pct_adapt.exe`, wired into
`build_host_tests.ps1` as its own gating `Invoke-HostTestExe` call, same
posture as `sim_scenarios_adaptive.c`'s WI-8 harness):

- Runs the 9-firing chain of sec 2.2/2.3 above for **S2, S5, S7, S12**
  (the plan's own WI-10 line), single zone, plant and belief model FIXED
  for the whole chain (no `adaptive_tune` K_dc refit in this arm -- that is
  WI-8's own separate question; mixing the two would make it impossible to
  tell which mechanism moved a result).
- Each firing is scored with the SAME per-segment `firing_score_seg_begin/
  _tick/_finish` accumulation `sim_scenarios.c` already uses (real
  `firing_score.c`, not a mirror), folded into one `firing_score_set_t` via
  `firing_score_set_add()`.
- Adjudicates consecutive firings with the real, unmodified
  `firing_compare()` (real `firing_compare.c`, linked, not mirrored).
- Starting `strength_pct` is 50 (the same value `SIM_ARM_FUZZY50`/
  `SIM_ARM_FUZZY_AT` already use elsewhere in this suite, for
  comparability); initial step is 20 percentage points; floor is 5.
- A firing that refuses (NaN/out-of-bounds/autotune refusal) is treated as
  described in sec 3.1's mitigation: scored as REJECT_DEGRADED for the
  adapter's own step logic, reported with `REFUSED_TREATED_AS_REJECT`, and
  the chain continues from the last adopted value (it does not abort the
  whole chain the way `sim_scenarios_adaptive.c`'s hard-refusal posture
  does, because refusal is itself a signal this design intentionally
  incorporates rather than a harness failure).
- Prints one row per firing (`STRENGTH_ADAPT` tab-separated line: scenario
  id, firing index, proposed strength, verdict, adopted strength after this
  step, step_pct) and one summary row per scenario classifying the trace as
  SETTLED / OSCILLATED / WANDERED per sec 2.3's definitions.
- **Gates the build on internal self-consistency only**, never on a
  particular scenario converging: a chain that never accepts a single step,
  or one whose trace wanders, is a printed, non-fatal finding (exit 0 with
  the classification printed), exactly as WI-10's acceptance line requires
  ("If the simulation shows the adapter wandering or failing to converge,
  that is a complete and valuable result -- record it and stop"). The
  executable's exit code is non-zero only for a genuine harness defect
  (S1_BASELINE-style table lookup failure, a NaN this design's own mitigation
  does not explain, or `strength_pct=0`'s bit-exact contract breaking), never
  for "the adapter did not converge."
