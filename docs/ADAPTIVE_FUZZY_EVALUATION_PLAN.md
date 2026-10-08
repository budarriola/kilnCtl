# Adaptive fuzzy: the evaluation that would decide it (2026-09-14)

**Status: BUILT AND RUN (2026-09-16); outcome recorded in the audits, this plan
is now history.** Built: the `sim_plant.c` delay-ring truncation flag
(`delay_truncated`), `pid_fuzzy_confidence.{c,h}` (`ee55af58`, sec 3's
confidence gate) and the adaptive arms plus sec 7 gates in
`sim_factorial_driver.c`. Outcome, read in order:
`docs/audits/adaptive_fuzzy_section8_campaign_2026-09-16.md` (gate 1 FAIL,
run inert), `adaptive_fuzzy_section8_dwell_fix_2026-09-16.md` (gate 1 still
FAIL, second cause), `adaptive_fuzzy_section8_cell_mix_rebuild_2026-09-16.md`
(**gate 1 PASS, gate 2 PASS, gate 3 FAIL**; integrity PASS). **Open, owner
only:** (a) the keep/remove decision on fuzzy (no verdict has been rendered);
(b) whether to amend gate 3, which as written fires on any limit-cycle
crossing and cannot separate a fuzzy-induced cycle from ordinary adaptive-PID
dwell settling (3 of 7 pinned cells now differ from the control arm, 4 of 7
are identical). No board was flashed and no heating run was performed.

## 0. Why this document exists

`docs/audits/scenario_factorial_results_2026-09-14.md` (+ the opus review,
`e2245b8d`) established that **fixed-gain** fuzzy at strength 50 is net
harmful: 175 degraded cells vs 128 improved at the 0.5 °C materiality floor
across 263 cells; 91 degraded-only vs 9 improved-only in the 151 bench-span
cells; `ENTRY_PEAK_C` — the feature's own stated purpose — degrades 108 and
improves 53; seven cells show a converged, bounded limit cycle. Four artifact
hypotheses were tested and rejected, and correcting the bench-valued dead time
makes the harm *worse*.

That document says so itself: the suite has **no adaptive arm**, so
`D_adapt`/`D_combo` are not computable and the verdict does not reach the
learning variant the owner has repeatedly said is the intended one. This plan
specifies the run that would.

**The short answer to "is this even distinguishable in simulation?": yes, and
cheaply.** Measured on this machine, the existing 789-firing factorial driver
runs in **2.3 s** (~2.9 ms per simulated firing). The adaptive arms add
263 cells × 2 arms × 9 firings = 4,734 firings ≈ **14 s**. The multi-firing
sequencing is not the constraint, and **no cell needs to be dropped** (§5).

---

## 1. What "adaptive fuzzy" concretely is, in this codebase

Stated as three disjoint lists so the implementer never has to guess which is
which.

### 1.1 Already built and shipped

| Piece | Where | What it does |
|---|---|---|
| Band derivation from the identified plant | `pid_fuzzy_derive_bands()`, `pid_fuzzy.c` (`2c49465a`) | `error_band_c = K_dc * 0.5`, `rate_band_c_per_s = K_dc / tau_s` |
| No model ⇒ no fuzzy | `pid_fuzzy_prepare_gains()`, `profile_executor_pid_tick.c` (`fuzzy_no_model_no_fuzzy_2026-09-14`) | a never-autotuned zone forces `strength_pct = 0`, i.e. bit-for-bit plain PID |
| `K_dc` refinement across firings | `adaptive_tune.c` / `adaptive_tune_model.c` | moves `K_dc`, so the bands move with it |
| Harvest freeze (the "Option B" precondition) | `harvest_freeze` in `profile_executor.c:988` → `strength_pct = 0` during dwell harvest | fuzzy cannot shape the trace the identification fit is taken from |
| A 9-firing adaptive chain harness | `sim_scenarios_adaptive.c` (`SIM_ARM_PID_AT`, `SIM_ARM_FUZZY_AT`, `N_FIRINGS 9`) | real `adaptive_tune.c` against a persistent per-chain zones-config fake, over the 11-scenario table |

So a weak form of adaptive fuzzy **already runs today**. Be precise about how
weak, per `docs/audits/concurrent_fuzzy_pid_adaptation_2026-09-14.md` §5:
both bands scale in the **one** scalar `K_dc`, and their ratio is exactly
`tau_s`, which `adaptive_tune` never moves ("dwell data cannot inform
dynamics"). The *shape* of the fuzzy input plane is frozen at the original
autotune for the life of the zone. Nothing about the rule table,
`MAX_NUDGE_FRACTION` or `fuzzy_strength_pct` adapts at all.

### 1.2 Specified but unbuilt

- **Confidence-graduated authority.**
  `docs/audits/adaptive_tune_confidence_authority_design_2026-09-13.md`
  specifies it, but for `adaptive_tune`'s **own** guards (`BLEND_ALPHA`,
  `MIN_OBSERVATIONS`) — *not* for fuzzy's strength. Its `1142c73b` review
  refuted the non-circularity argument as written and listed required changes
  before it is buildable.
- **The adaptive factorial arms.** `docs/audits/scenario_factorial_design_2026-09-14.md`
  §5.1 already names `D_adapt` and `D_combo` and already costs a 6-arm /
  22-firing-per-cell structure. `sim_factorial_driver.c` implements only
  3 single-firing arms. This is specified-but-unbuilt, not new.

### 1.3 Newly specified here

**(N1) Confidence drives `fuzzy_strength_pct`, from 0 upward.**
`concurrent_fuzzy_pid_adaptation_2026-09-14.md` §5 says adapting
`fuzzy_strength_pct` "should NOT, without Option B". **Option B is now
shipped** (`harvest_freeze`, table above), so that precondition is met — flagged
here rather than silently assumed. The remaining objection in that section is
that scoring *firing outcomes* is closed-loop-heuristic adaptation. This plan
**avoids that objection entirely by not using outcome scores**: confidence is a
property of the **plant identification** (§3), never of how the loop looked.

**(N2) A dead-time authority cap, and (N3) an in-firing oscillation detector.**
These are new and they exist because of a specific finding below.

### 1.4 The finding that shapes this whole design — state it loudly

The seven-cell failure is a **gain-scheduling-induced limit cycle closed
through a large transport delay**, and the review measured it **monotone in
dead time** (0 s → 9.63; 10/20/30 s → 22.2/46.2/64.5; 40.3 s → 84.2; 64 s →
117.8). Every *fixed* gain vector tested was stable, including ones outside
everything fuzzy realises. Only the time-varying schedule oscillates.

**Therefore a confidence gate built on `K_dc` agreement would NOT have
prevented it.** In those cells the model is *correct* — `A6 = MATCHED` — so
`K_dc` confidence would be HIGH and would raise authority into exactly the
regime that limit-cycles. A confidence gate that does not demonstrably prevent
the observed failure mode is not a fix (the task says so, and it is right).
Hence (N2) and (N3), which are aimed at the real mechanism (`L`, and
oscillation itself) rather than at model quality.

---

## 2. Bootstrap from autotune

Owner requirement: fuzzy constants derive from the PID autotune on the
installed kiln, never from fixture constants.

| Fuzzy parameter | Source | Notes |
|---|---|---|
| `error_band_c` | `K_dc * ERROR_BAND_K_FRACTION` | **already shipped**, do not re-derive |
| `rate_band_c_per_s` | `K_dc / tau_s` | **already shipped** |
| `fuzzy_strength_pct` | **0** at bootstrap, rising only under §3 | new; today's default of 50 is what the factorial condemned |
| rule table, `MAX_NUDGE_FRACTION` | compile-time, unchanged | out of scope; `pid_fuzzy.h:17-21` closed per-install rule tables deliberately |

**Before any autotune has ever run:** nothing changes — the shipped no-model
path already forces `strength_pct = 0` and logs once per zone per boot. An
un-autotuned zone runs plain PID. That is correct and needs no new code.

**Consequence worth naming:** with `strength_pct` starting at 0, **firing 1 of
the adaptive arm is bit-for-bit `A_PID`**. The feature's floor is exactly the
control it is compared against, so it cannot be worse than PID before it has
earned anything. This is the same "the floor state IS today's shipped
behaviour" safety argument the confidence design makes in its §4, reused
deliberately.

---

## 3. The confidence gate

### 3.1 What confidence is measured from

Reuse the confidence design's §1 signals verbatim — do not invent a second
definition:

1. **Forward-checked predictive residual.** At run N+1's run-end, before
   folding N+1's observations in, score how well run N's model predicted N+1's
   settled-dwell observations:
   `residual_frac = |K_dc_after_N * duty_obs_N+1 − rise_obs_N+1| / rise_obs_N+1`.
2. **Fit stability.** Coefficient of variation of the last K accepted raw
   `K_fit` values narrowing rather than wandering.
3. **Eligibility gate, not an input:** the existing
   `ADAPTIVE_TUNE_MIN_DUTY_SPREAD` must have been satisfied over the runs being
   scored.

Carry the `1142c73b` review's caveat forward rather than waving it off:
cross-run ring contamination makes the residual **partly in-sample**, and
selection bias is the real leak. That review's required changes are a
prerequisite for shipping this on hardware. **They are not a prerequisite for
the simulation run**, because in simulation the true plant is known and the
implementer must additionally log the *true* `K_dc` alongside the estimate, so
the residual's optimism is measurable rather than argued.

### 3.2 The authority schedule

```
c        = consecutive runs with both signals inside band, clamped to [0,4]
cap_L    = 1.0 if (L/tau) <= 0.10
           linear 1.0 -> 0.0 over 0.10 < (L/tau) <= 0.30
           0.0 if (L/tau) > 0.30                       <-- (N2)
strength = round( S_MAX * (c/4) * cap_L )              S_MAX = 50
```

- `c = 0 ⇒ strength 0 ⇒ plain PID.` **That is the answer to "what happens when
  confidence is LOW": the feature is off, bit-for-bit.** There is no
  low-confidence mode with reduced-but-nonzero authority to reason about.
- Rise is rate-limited to one step per accepted run; fall is immediate to the
  floor on any disqualifier (confidence design §5's deliberate asymmetry).
- **`cap_L` (N2)** is the anti-limit-cycle term. `L` and `tau` are autotune
  outputs already on the board, so it is bootstrap-time computable and needs no
  new measurement. The 0.10/0.30 breakpoints are **chosen, not measured** —
  label them as such. The factorial run is precisely the instrument that tests
  whether they are placed correctly: the seven known cells sit at
  `L/tau = 40.3/488 = 0.083` at the bench value but at `76.9/488 = 0.158` once
  kiln-scaled (§6), i.e. **inside the taper**, which is the intended behaviour
  and is a falsifiable claim about the breakpoints.
- **Oscillation detector (N3).** Within a firing, count error zero-crossings
  over a rolling window during a dwell. The measured contrast is stark — 29
  crossings in the oscillating arm, **0** in both stable arms — so the
  threshold has enormous margin. On trip: `strength_pct → 0` for the remainder
  of the firing, and confidence → floor at run end. This is an outcome-derived
  signal, so per §1.3 it is deliberately allowed to **only ever reduce**
  authority, never raise it — that direction carries none of the
  closed-loop-heuristic hazard.

---

## 4. Known traps, and how this design avoids each

| Trap | Avoidance |
|---|---|
| **The `DT_S` trap.** Sweeping the controller timestep collapses the oscillation (84.2 → 9.9) and looks exactly like proof of a numerical artifact. It is not — it only shortens the effective dead time via the 63-step ring clamp. | **Do not sweep `DT_S`.** 1 Hz is the production tick (`PROFILE_EXECUTOR_TICK_MS 1000`) and is correct. Refine the *plant ODE* sub-steps instead if convergence must be re-checked; that was done and moves the result 3.5 % at 64×. And §6 makes the silent clamp loud, which removes the trap at its source. |
| Simulator feeds `sim_kiln_step()` the **binary** relay state, not fractional duty. | The factorial driver's single-zone path uses `sim_plant_three_node_step()` with continuous duty and is not affected. **If any multi-zone cell is ever added, window-average duty before feeding duty-nonlinear plant terms.** No multi-zone cell is in this design. |
| The firmware's coupling model class (`G*u = b`, additive duty-driven off-diagonals) is **refuted on hardware** — superposition fails by 33 %. | This design is **single-zone throughout** and makes no superposition claim. Sensitivity: the whole result is conditional on single-zone dynamics; on a real multi-zone kiln, cross-zone duty will perturb each zone's identification and therefore its confidence. **That is untested and must be stated in any conclusion drawn from this run.** |
| Fuzzy was previously A/B tested **inert** because the control mode was wrong (mode 2 made two whole campaigns meaningless). | §7's activity proof: instrumented per row, asserted mechanically, non-zero exit if inert. Never assumed. |
| A harness that prints a verdict and exits 0 (`sim_iter_tune`'s printf-only accept bars). | §7's failure signal: every gate returns a non-zero process exit. |
| The bench is a ~4 W / 120 V fixture that cannot exceed ~40 °C above ambient. | Every kiln-span number in this plan and in its output must be labelled **[EXTRAPOLATION]**. The 112 kiln-span cells are extrapolation beyond any hardware this project has; the 151 bench-span cells are the only regime with corroboration. Reported separately, always. |

---

## 5. How the factorial arm is added

### 5.1 Structure

- **Two new arms**, reusing the names that already exist in
  `sim_scenario_table.h`: `A_PID_AT` (adaptive PID, fuzzy off) and
  `A_FUZZY_AT` (adaptive PID + confidence-gated fuzzy).
- **Each cell runs a chain of N = 9 sequential firings**, adaptation state
  (model, gains, `autotune_baseline_k_dc`, confidence counter) carried across
  them. **N = 9 is not a new number** — it is `sim_scenarios_adaptive.c`'s
  existing `N_FIRINGS`, kept so the two harnesses stay comparable. The existing
  three arms stay single-firing.
- **Implementation route: extend `sim_factorial_driver.c`'s arm enum and lift
  `sim_scenarios_adaptive.c`'s chain mechanics** (its zones-config fake, its
  run-end wiring, its per-firing print) rather than writing a third copy of the
  tick loop. The driver's header already warns that it is a near-duplicate of
  `sim_scenarios.c`'s `run_firing()`; **do not create a fourth**. If the heavier
  link surface (`adaptive_tune.c` pulls `hal_kv`/`esp_log`/`flash_worker`/a
  FreeRTOS mutex shim) makes one executable unwieldy, build a **second**
  executable over the same cell list — that is exactly the reasoning
  `sim_scenarios_adaptive.c` already documents for its own split.

### 5.2 Cost, measured not guessed

| | firings | measured cost |
|---|---|---|
| existing 3 arms × 263 cells | 789 | **2.3 s** (measured, this machine, `--of 1`, `-SkipDeterminism`) |
| new 2 arms × 9 firings × 263 cells | 4,734 | ~14 s at the same 2.9 ms/firing |

Even allowing 3× for `adaptive_tune`'s per-run-end work and the kiln cells'
longer ramps, this is **under a minute single-process** and seconds sharded
four ways. **No factor is dropped and no cell is dropped.** The suite stays a
manually-invoked `--suite factorial` target; **do not raise
`check_sim_scenarios.ps1`'s 60 s CI budget** — the 13 named anchors stay there.

### 5.3 What does NOT need to change

The design generator (`sim_factorial_design.c`, 224/39/263 and the single
feasibility mask) is untouched. Sharding stays cell-index modulo; a chain is
entirely within one cell, so shard-independence survives and
`run_sim_factorial.ps1`'s `--of 1` vs `--of 4` byte-identity proof must still
pass (with `-SkipDeterminism` **not** set for the recorded run).

---

## 6. Kiln-scaled parameters

The fixed run held sensor-node tau and dead time at bench values even for
kiln-span cells. The review showed this **under-states** harm, so it was not
fatal there — but it is physically inconsistent and is corrected here.

| Fixture | Bench cells | Kiln-span cells (`A4 = 20.7`) |
|---|---|---|
| `sensor_delay_s` | 40.3 s (unchanged) | **76.9 s** (40.3 × 488/255.6) |
| `sensor_tau_s` | 15 s (unchanged) | **28.6 s** (15 × 488/255.6) |
| `c_s_j_per_c` | 500 | 500 — **provably inert**, a 10× change is bit-identical (only `c_s/sensor_tau_s` enters `dS`, and it cancels). Strike it from the assumptions list. |

### 6.1 The 63-step ring, and the required harness change

`sensor_pipeline_step()` (`sim_plant.c:51`) clamps `delay_steps` to
`SIM_PLANT_DELAY_MAX_STEPS − 1 = 63` **silently**. At `dt = 1` the kiln-scaled
76.9 s therefore becomes 64 s — detected originally because the 76.9 s and
64.0 s rows came out byte-identical.

Two changes, both in test-fixture code only:

1. **Raise `SIM_PLANT_DELAY_MAX_STEPS` from 64 to 128** (`sim_plant.h:24`). It
   is a `float` ring in a host-test struct; the cost is 256 bytes per zone.
2. **Make the clamp loud.** `sensor_pipeline_step()` must set a
   `delay_truncated` flag on the state when the requested delay exceeds
   capacity, and every driver must **refuse the cell** on that flag rather than
   silently proceed. This is the actual fix for the `DT_S` trap: after it,
   anyone shrinking `dt` gets a refusal instead of a quietly shortened dead
   time and a wrong "artifact" conclusion.

**Mandatory regression proof:** bench-span cells use 40.3 s < 63 steps, so they
must be **bit-identical** before and after. Re-run the existing 3-arm sweep and
diff the 151 bench-span cells' rows byte-for-byte. If they differ, the ring
change did something unintended — stop and investigate before proceeding.

---

## 7. Proving the adaptive arm is actually active

Registered before the run, because this project has already lost two whole
campaigns to an inert arm.

Per (cell, arm, firing) row, emit: `strength_pct_realised_mean`,
`strength_pct_realised_max`, `ticks_with_strength_gt_0`, `confidence_c`,
`cap_L`, `error_band_c`, `rate_band_c_per_s`, `model_k_dc`, `true_k_dc`,
`oscillation_tripped`.

Three mechanical gates, **each a non-zero process exit**, never a printed
verdict:

1. **Activity gate.** `A_FUZZY_AT` must reach `strength_pct > 0` for at least
   one tick in **≥ 30 %** of cells by firing 9, and `A_FUZZY_AT` firing 9 must
   differ from `A_PID_AT` firing 9 by more than the 0.5 °C floor on at least
   one objective in **≥ 10 %** of cells. Below either ⇒ FAIL: the arm is inert
   and the run is uninterpretable, exactly as in the mode-2 campaigns.
2. **Floor-identity gate.** `A_FUZZY_AT` firing 1 must be **bit-identical** to
   `A_PID_AT` firing 1 on every cell (§2's consequence). A difference means
   strength was non-zero before confidence was earned.
3. **Limit-cycle regression gate.** The seven known cells — `ST1-049`,
   `ST1-057`, `ST1-113`, `ST1-121`, `ST1-177`, `ST1-241`, `ST1-249` — are a
   pinned fixture. In `A_FUZZY_AT`, **zero** dwell error zero-crossings in any
   of the nine firings. Any crossing ⇒ FAIL.

Also required, and separately fatal: any cell refusal, any NaN, any delay-ring
truncation flag, and any `--of 1` vs `--of 4` mismatch.

---

## 8. The comparison that decides the feature — registered before any data exists

**Decision rule (the owner's, unchanged): count cells improved vs degraded
past a 0.5 °C materiality floor. Harmful more often than helpful ⇒ REMOVE.**

**Primary comparison, fixed now:**

```
D_adapt_combo = obj(A_FUZZY_AT, firing 9) - obj(A_PID_AT, firing 9)
```

i.e. **the adaptive-PID arm is the control**, not plain PID. That is literally
the before/after of deleting adaptive fuzzy from a board that will have
adaptive PID either way.

**Counting unit: PER CELL, not per (cell, objective) pair. Fixed now, before
any data exists.** Reason, stated in advance so it cannot be chosen after the
fact: **fuzzy is enabled per plant, not per objective** — a zone either runs it
or does not. A cell counts as improved if it improves on ≥ 1 of the four
objectives and degrades on none; degraded if it degrades on ≥ 1 and improves on
none; mixed cells count toward **both** improved and degraded totals, exactly as
the fixed run reported them (175 / 128).

The fixed run's per-pair count **inverted** the verdict (286 vs 233), driven
entirely by kiln-span cells. Per-pair will still be reported — as a **declared
secondary**, labelled as the inverting slice — so the reader sees the same
tension the fixed verdict disclosed. It does not decide anything.

**Pre-registered subgroup reporting** (all three, always, no post-hoc choosing):

- All 263 cells — the headline.
- **151 bench-span cells** — the only regime with hardware corroboration; this
  is the subgroup to believe if the two disagree.
- **112 kiln-span cells — [EXTRAPOLATION]**, far beyond a ~4 W fixture, with a
  load node that never approaches setpoint.

**Secondary comparisons, reported but not decisive:**

- `D_adapt = obj(A_PID_AT, firing 9) − obj(A_PID, firing 1)` — does adaptation
  alone help? Needed to tell "adaptive fuzzy helped" from "adaptation helped".
- `D_learn = obj(A_FUZZY_AT, firing 9) − obj(A_FUZZY_AT, firing 1)` — did it
  learn anything at all across the chain?
- `D_fixed_vs_adaptive = obj(A_FUZZY_AT, firing 9) − obj(A_FUZZY50)` — is the
  adaptive variant better than the condemned fixed one?

---

## 9. Falsifiable prediction, registered in advance

**Removal criteria — any one of these ⇒ REMOVE adaptive fuzzy:**

1. **Net harm.** In `D_adapt_combo`, per cell, over all 263 cells, degraded ≥
   improved. (The fixed arm scored 175/128 — a failure.)
2. **Bench-span harm.** In the 151 bench-span cells, degraded-only ≥
   improved-only. (Fixed scored 91/9.)
3. **Purpose failure.** On `ENTRY_PEAK_C` alone, degraded > improved. Fuzzy
   exists to cut overshoot; failing that is disqualifying regardless of the
   other three objectives. (Fixed scored 108/53.)
4. **Gate failure.** Any of the seven pinned cells still limit-cycles (§7 gate
   3). **This one is independent of every tally** — a confidence gate that does
   not prevent the observed failure mode is not a fix, so this removes the
   feature even if criteria 1–3 all pass.

**My prediction, written before the run.** The gate (§7 gate 3) holds — the
mechanism (`cap_L`, then the crossing detector as backstop) is aimed directly
at the measured driver, so I expect zero limit cycles. On the tallies I expect
**near-neutrality with a large within-floor mass**, specifically: within-floor
on all four objectives in **> 120** of 263 cells, improved > degraded but by a
margin **under 40 cells**, and `D_learn` material in **< 25 %** of cells.

That prediction is itself an argument against the feature and is stated as
such: if adaptive fuzzy survives its removal criteria only by being *inert
almost everywhere*, the honest reading is that it passes because it does
nothing, not because it works. **Register the follow-on rule now:** if
criteria 1–4 all pass AND the improved-minus-degraded margin is under 20 cells
AND within-floor exceeds 150, report the result as **"survives, but
indistinguishable from plain adaptive PID"** and put the keep/remove decision
back to the owner rather than claiming a win.

---

## 10. Honest limits

- Everything here is **simulation**. A sim win is screening, not evidence of
  hardware improvement — this project's standing position, and it is not
  relaxed by adding an arm.
- Single-zone only. The refuted coupling model means nothing here transfers to
  multi-zone behaviour (§4).
- The confidence residual is partly in-sample (`1142c73b`). The sim knows the
  true `K_dc`, so the optimism is *measurable* here; on hardware it is not, and
  that review's required changes remain a shipping prerequisite.
- `cap_L`'s breakpoints (0.10 / 0.30) are chosen by desk reasoning. The run
  tests them; it does not derive them.
- Every kiln-span number is **[EXTRAPOLATION]** from a ~4 W fixture.

## 11. Implementation order

1. §6.1 ring change + loud truncation, with the bench-span bit-identity proof.
2. §1.3's (N1)/(N2)/(N3) in `pid_fuzzy`/`profile_executor_pid_tick` +
   `adaptive_tune`, with a **negative test per new gate** — break the
   production function by hand, prove the gate fails, restore by hand, prove
   an empty `git diff`, then **force a full rebuild** before measuring anything
   (`ed854ac5`/`ba230bca`/`8a12521b`).
3. §5.1's two arms in the factorial driver.
4. §7's three gates, each proven to be able to fail.
5. Run; write the results audit against §8 and §9 **without editing either**.
