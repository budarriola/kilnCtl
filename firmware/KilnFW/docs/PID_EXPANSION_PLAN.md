# PID Expansion Plan — control-algorithm work for KilnFW

Status doc for the per-zone control algorithms on the ESP32-S3 side. Rewritten
2026-09-01 after the bulk of it shipped; the previous 1364-line version, with
the full ten-paper literature review and the phase-by-phase checklists of
completed work, is in git history immediately before this commit if the
reasoning behind a landed decision is ever needed.

Conventions this doc follows: **the code is truth, not the checkboxes** — the
old version was stale in both directions, marking built work as pending and
pending work as built. Anything claimed done below names the commit. Anything
measured names the run.

---

## 1. Where this stands

Per-zone control mode (off / PID / PID+feedforward / fuzzy-PID), step and relay
autotuning, SIMC / ZN / Tyreus-Luyben / Cohen-Coon rules with machine-readable
refusals, coupled cross-zone feedforward, per-zone quality statistics, adaptive
tuning from ordinary firings, and the supporting HTTP/UI/telemetry surfaces are
all in the tree and tested.

What follows is only what is **not** done, plus the results and dead ends that
constrain it.

---

## 2. Measured results

Profile 7, three zones, ramp to 45 °C, dwell, ramp to 60 °C, dwell. Normalized
IAE (time-weighted mean absolute error, °C), whole run:

| | z0 | z1 | z2 |
|---|---|---|---|
| baseline, 2026-09-01 | 1.630 | 1.291 | 1.425 |
| after the day's work | **0.878** | **0.540** | **0.695** |
| RMS | 2.17 → 1.06 | 1.71 → 0.69 | 1.78 → 0.84 |
| max overshoot, °C | 5.24 → 1.69 | 4.48 → 1.50 | 4.67 → 1.84 |
| mean error, °C | +1.10 → −0.30 | +0.70 → +0.02 | +1.00 → +0.40 |

Roughly half the tracking error and a third of the overshoot, with the mean
centred on all three zones. The consistent hot bias that prompted this is gone. The original
complaint — a 70 °C target reaching 80.1 °C — traced to five defects stacked on
each other, each hidden by the one above it: a settle detector firing
mid-transient, a fit reading its last sample as the asymptote, guard 1 blocking
honest step tests, PWM chopping disarming four guards at partial duty, and an
uncoupled climb term over-driving zone 0 roughly tenfold.

Identified plant (bench rig, 0–80 °C): z0 K=39.2459 τ=263.8 L=52.8; z1
K=31.9669 τ=269.8 L=43.5; z2 K=31.6810 τ=270.9 L=33.9 — read back off the
board 2026-09-02, `model_k_dc`/`model_tau_s`/`model_dead_time_s`.

**Coupling coefficients now on the board** (`GET /api/zones`, 2026-09-02;
§3.2's re-solved matrix, adopted): row = affected zone, column = stepped zone,
diagonal contractually 0.

```
z0: [ 0    , 27.32, 21.72]
z1: [14.30,  0    , 22.15]
z2: [ 8.33, 12.42,  0    ]
```

The solver substitutes `ff_k_dc` for the zero diagonal (§3.2's correction).
The old bench matrix (off-diagonals 26.61/20.73 · 15.78/21.09 · 9.70/11.38,
RGA diagonal 1.53/1.69/1.34) is superseded; that RGA has not been recomputed
for the adopted matrix, and `/api/autotune/matrix` reports no cells at all
(the identification matrix is RAM-only and this board has rebooted since).
Off-diagonal τ is 620–730 s against 264 s on the diagonal, dead time
135–158 s against 34–53 s — from offline analysis, not from the board, whose
`coupling_tau_c*`/`coupling_dead_time_c*` are all still 0. Cross-zone heat
arrives far later than a zone's own element, which is the root of the
remaining ramp-onset error.

**Orientation, which has been swapped by mistake more than once:** persistent
storage is `coupling_coeff[affected][stepped]`; `/api/autotune/matrix` reports
the transpose.

---

## 3. Remaining work

### 3.1 Dwell-entry overshoot — CLOSED 2026-09-01

Was the largest remaining error: 2.0–2.7 °C on entering every dwell, peaking
70–110 s after the ramp ended. Now 0.4–1.8 °C.

Diagnosis that led to the fix: duty does **not** crash to zero at the boundary
(the analyzer's duty-off-to-peak lag is `n/a` for 5 of 6 transitions), peak
timing tracks each zone's own dead time at 1.5–2×, and zone 2's second-dwell peak
lines up with the cross-zone dead time while its neighbours are still driving.
So the plant was arriving at setpoint with stored rate, not being over-driven
after arrival — which is why reducing rate before the boundary worked and
holding duty through it (§4) did not.

- [x] **Terminal ease-off** (`48940a5`) — taper the commanded rate as the target
      is approached so the plant arrives with less stored rate. Linear taper over
      a window of 2× each zone's own identified dead time, applied to the
      feedforward's rate input only, never to the setpoint schedule, so a stall
      is structurally impossible and the worst case is ordinary tracking lag.

      **Measured on hardware, improvement on every zone and every metric:**

      | | z0 | z1 | z2 |
      |---|---|---|---|
      | dwell-entry overshoot, seg0 | 0.59 → 0.41 | 1.99 → 0.88 | 2.67 → 1.80 |
      | dwell-entry overshoot, seg1 | 2.02 → 1.50 | 2.04 → 1.50 | 2.37 → 1.84 |
      | seg1 ramp RMS | 1.55 → 0.84 | 1.23 → 0.57 | 1.03 → 0.63 |
      | whole-run normalized IAE | 1.034 → 0.878 | 0.723 → 0.540 | 0.821 → 0.695 |

      The ramps improved as well as the dwells, which was not the goal: arriving
      with less stored rate leaves less to correct on both sides of the boundary.

      **This change was simulated, rejected, and reported as a dead end before
      the simulator's ramp-rate bug was found (§3.4).** It is the best single
      result of the session and it was nearly discarded on a bad model. When a
      simulation rejects a physically sound idea, check the simulation.

### 3.2 Zone 2's model over-predicts its hold duty — MEASURED 2026-09-02, ADOPTED 2026-09-02

Three single-zone excitation runs completed 2026-09-02, each from a rested
start (every zone at ambient) with a full settled 2100 s dwell: z0 0.60 duty
@55C (peers 35.9/31.6), z1 0.76 (peers z0 49.8, z2 36.9), z2 0.75 (peers
45.7/45.8). Coupling is strongly **asymmetric** — z1 raises z0 ~22C, but z0
raises z1 only ~9C.

New coupling matrix, `[affected][stepped]`:

```
[[38.13, 27.32, 21.72],
 [14.30, 35.90, 22.15],
 [ 8.33, 12.42, 35.32]]
```

Condition number 4.64 (vs 5.27 for the current matrix) — plausibility check
passes.

Revalidated against the historical dwell tails: z0 bias −0.081 → −0.047, z2
+0.113 → +0.032 (both improve), but **z1 −0.007 → −0.053 gets slightly
worse**. This is a trade, not a clean win, and should be recorded as such.
Feasible range extends 60C → 65C.

**CORRECTION, 2026-09-02d: the figures above describe a matrix that does not
run.** `zone_coupling_solve.c` (`zone_coupling_solve_hold()`/`_climb()`,
~lines 229 and 371) never reads the matrix's own diagonal cell —
`coupling_coeff[]`'s diagonal is contractually 0 (`zones_http.c`) — and
instead sets `G[row][row] = ff_k_dc`, the per-zone identified DC gain.
Every number above (cond 4.64, the −0.047/−0.053/+0.032 biases, 60C→65C) was
computed on the matrix's **own** diagonal (38.13/35.90/35.32), not on
`ff_k_dc` (39.2459/31.9669/31.6810 as last read off the board — within 1–3%
of the matrix diagonal on z1/z2 but ~2.9% off on z0). The solver actually
runs a **hybrid**: this matrix's measured off-diagonals with `ff_k_dc` on
the diagonal. Recomputed for that hybrid, off the same fixtures and the same
`coupled_hold_feasibility_sweep()`/`matrix_plausibility()` tooling
(`tools/PcTools/src/kilnctrl/coupled_ident.py`):

| | own-diagonal (doc's original figures, does not run) | hybrid (ff_k_dc diagonal, what actually runs) | pre-adoption hybrid (old off-diagonals, ff_k_dc diagonal — what ran before) |
|---|---|---|---|
| condition number | 4.64 | 5.51 | 1.92 |
| plausibility | pass | pass | pass |
| feasible range | 65C | 60C | 58C |
| bias z0 | −0.047 | −0.094 | +0.201 |
| bias z1 | −0.053 | −0.056 | +0.237 |
| bias z2 | +0.032 | +0.116 | +0.137 |

The hybrid that actually runs is worse than the own-diagonal figures on
every axis — roughly 2–3.6× the bias magnitude on z0/z2, a materially worse
condition number, and a feasibility boundary 5C short of the claimed one.
It is still an improvement over what ran before adoption (all three biases
shrink substantially, feasibility 58C→60C), so **adoption was still a net
win in what shipped**, just a smaller one than this section originally
claimed, and the win is despite the diagonal substitution, not because of
it.

This also answers a design question the substitution raises: should the
solver use the matrix's own diagonal instead of `ff_k_dc`? The own-diagonal
matrix wins on every metric above, and it is not a coincidence — its
diagonal comes from the *same* three rested single-zone excitation runs as
the off-diagonals it is paired with, while `ff_k_dc` is identified
separately (step-test autotune) and carries its own ~3% disagreement with
the matrix's own z0 reading. Using the matrix's own diagonal is better
supported by this data. It is not a drop-in change, though: `ff_k_dc` is
also the shared input to the uncoupled 1x1 fallback
(`diagonal_hold`/`diagonal_climb` in `zone_coupling_solve.c`) and to other
feedforward math outside coupling, so switching only the coupled-solve
diagonal would make the coupled and uncoupled paths disagree about a
zone's own gain at the seam where neighbours drop in/out of qualification —
that discontinuity would need to be sized before making the change, not
just the win above it. No firmware change made here.

**SEAM SIZED, 2026-09-02e.** Hand-solved both diagonal choices against the
adopted matrix and `ff_k_dc` at dT=30C (the bench dwell's rough operating
point), at the moment a neighbour joins (extra step introduced by switching
the diagonal ALONE, isolated from the much larger step the coupling terms
themselves already introduce at every membership edge):

| pair active | z0 step | z1 step | z2 step |
|---|---|---|---|
| z0+z1 | +0.102 | — | — |
| z0+z2 | +0.069 | — | −0.106 |
| z1+z2 | — | +0.010 | −0.086 |
| all three (z0+z1+z2) | +0.059 | −0.006 | −0.093 |

z0 and z2 clear the 0.05-duty "visible bump" bar in most configurations; z1
does not (its own-diagonal and `ff_k_dc` values are within ~12% of each
other and the off-diagonal terms further damp it). Reproducible via
`test_zone_coupling_solve.c`'s hand-solved constants.

**Reachable? Yes, but already absorbed.** `heat_blocked` is refreshed every
tick by `apply_relay()`, so a marginal interlock or OTA heat-block can flip
a neighbour's qualification at tick rate — membership churn is a normal,
observed occurrence ("every commissioned kiln sees a few of these across a
run," per `profile_executor_feedforward.c`'s own comment), not a rare edge
case. However, `pid_family_zone_tick()` already re-seeds the PID integral to
the CURRENTLY COMMANDED duty on every `ff_membership_changed` edge (Opus
review round 3, item 3, landed before this analysis) — specifically so a
feedforward discontinuity of any size at a membership edge never reaches the
heater as a commanded-duty step; it only reshapes how the integral converges
afterward. That mechanism does not care which candidate produced the
discontinuity, so switching the diagonal source does not introduce a new
*class* of risk here — the seam sizes above bound how much MORE work the
reseed is doing, not whether a bump escapes it.

**STORAGE LANDED, 2026-09-02f — the solver itself is still unchanged.**
The data-availability blocker described in the paragraph above is now
closed: `zone_cfg_t` carries a new `coupling_diag_k_dc` field (`zone_index`
of the matrix's own diagonal cell, ZONES_CFG_VERSION 14->15, migration
tested against a frozen v14 blob — `coupling_diag_k_dc` defaults to 0.0,
"not measured," identical to `coupling_coeff[]`'s own convention), with a
`zones_config_get/set_coupling_diag_k_dc()` pair mirroring
`zones_config_get/set_fuzzy_strength_pct()`'s discipline, a GET/POST
`/api/zones` field (`z%u_coupling_diag_k_dc`, omitted-preserves, same rule
`fuzzy_strength_pct`/`coupling_c%u` already use), a `backup_http.c`
export/import round trip (no `BACKUP_FORMAT_VERSION` bump — purely
additive, same as the 11->12 `coupling_tau_c%u`/`coupling_dead_time_c%u`
addition), and a `tools/PcTools/src/kilnctrl/zones_http_client.py` mapping
plus preset-override entry so a PC-side preset can carry it without the
client refusing the POST body as an unknown field. Deliberately named
`coupling_diag_k_dc`, not folded into `model_k_dc`/`ff_k_dc` — those are a
*different* identification (single-zone step/relay test) and can disagree
with this field on a real board (that disagreement is exactly what this
section measured above).

**Still not implemented: the solver switch itself.** `zone_coupling_solve.c`
is untouched by this pass — `G[row][row] = ff_k_dc` still runs, on purpose;
this pass only makes the alternative persistable. The seam sized above
(2026-09-02e) still applies unchanged. What is left, now that storage
exists, is a small, separately reviewed change: read `coupling_diag_k_dc`
where it is nonzero (falling back to `ff_k_dc` where it is still 0, i.e.
"not measured" — the same guarded-fallback shape `diagonal_hold`/
`diagonal_climb`'s own `ff_k_dc` reads already use) behind a flag, an
on-board identification pass to actually populate the field (today it can
only be set by hand or by a PC-side preset built from `coupled_ident.py`'s
offline analysis, never by autotune), and a decision on whether the
uncoupled 1x1 fallback switches too or keeps `ff_k_dc` deliberately (the
seam-sizing paragraph above is the reason that decision needs its own
pass, not a byproduct of this one).

Made explicit in code instead (`zone_coupling_solve.c`, the `G[row][row]`
assignment): a doc comment naming both candidates, why `ff_k_dc` is the one
used, and pointing at this section and `test_zone_coupling_solve.c` for the
seam numbers and the falsifiable pin on today's choice.

**ADOPTED 2026-09-02 (owner decision).** Every cell has exactly one
observation — no redundancy, no error bar — and that caveat travels with the
numbers, not just this paragraph. Reproducible from checked-in logs via
`coupling_pair_log.py` / `python -m kilnctrl.coupled_ident coupling-report`
(`e2c7f41`, `72508ea`).

Applied as **data, not a firmware default**: presets are deliberately never
compiled into firmware (`config_presets.py`'s own module docstring — a value
baked into firmware ships to every board including a real kiln, one read
from a PC-side JSON file cannot reach a board unless something chooses to
send it), and `coupling_coeff[]` itself has no compiled-in default anywhere
in this codebase today (every zone numeric field, PID gains and thermal
model included, is populated by autotune/config restore, never a compiled
constant) — so a new compile-time default for coupling specifically would
have been the odd one out.

**CORRECTION, 2026-09-02: the paragraph below (as originally written) was
wrong, and the matrix was NOT reapplicable through the path it described.**
`tools/PcTools/config_presets/tuned_baseline_20260831.json`, the file commit
`78f2134` actually edited to carry this matrix, is **not a preset** —
its top level is `{"kind":"kilnctl_backup","version":4,"profiles":[...]}`, a
full board *backup* (same shape as `pid_validation_backup.json` next to it)
that happens to live in the `config_presets/` directory. `list_config_presets`
rejects it (`config_presets.py`'s `_validate()`: `missing required field
'name'`), so `load_config_preset`/`factory_default_then_load_preset` could
never load it. Separately, `zones_http_client.py` (the client
`apply_preset(..., zones_host=...)` uses to reach `coupling_c%u`, since that
field has no UART setter) had no field mapping for a preset's
`coupling_coeff` at all — `build_post_body()` raised
`ZonesHttpUnknownFieldError` on it. Together, there was no sanctioned path
from this matrix to a live board.

**Fixed 2026-09-02**: `zones_http_client.py` now maps a preset zone's
`"coupling_coeff": [c0, c1, ..., c{N-1}]` (same `[affected][stepped]`
orientation as GET `/api/zones`' own `coupling_c%u` keys — entry `j` is this
zone's response to zone `j` being stepped) onto the per-cell
`z%u_coupling_c%u` POST fields, skipping the diagonal cell (forced to `0` by
the firmware, never overlaid even if a preset row carries something else
there). The matrix now lives in a real, schema-valid preset,
`tools/PcTools/config_presets/coupling_matrix_20260831.json`, alongside
(not replacing) the misfiled backup files above — those are left as-is,
being backups rather than presets is outside this correction's scope. **The
actual working path**: `config_presets.load_preset_data(
'coupling_matrix_20260831')`, then `apply_preset(control, preset,
zones_host=<esp-ip>)` — `zones_host` must be given, since `coupling_coeff`
has no UART setter and is otherwise reported in `not_written` and never
reaches the board. It does not survive a factory reset by itself —
reapplying the preset after a reset is the sanctioned recovery path for
every other tunable in that file too, not a gap specific to coupling.

**Prediction for the next run's analyst:** this matrix should shrink the
positive dwell idle offset the owner noticed (profile-7 baseline,
fuzzy=0, `logs/coupling/p7_fuzzy0_http.jsonl`, steady-state +0.18 to +0.92 C
on 5 of 6 dwells) — the old matrix over-predicted z2's hold duty, and the
integral floor at `-ff_hold` cannot correct downward past that. If a re-run
of that profile does NOT show the offset shrink, that is evidence against
this matrix, not just noise.

Parser gotcha caught while building that tool: the exec-status `elapsed=Ns`
field **resets to 0 at every ramp → dwell transition**; naive use makes each
phase boundary look like the start of a new run.

Cooldown tau fits from the same runs: z0 469 s, z1 455 s, z2 345 s (z2 loses
heat fastest, consistent with its lower DC gain). Only one clean decay
window per zone — rough, not calibration-grade.

Original diagnosis below, kept for the reasoning trail:

The coupled solve wants 0.861 duty where the kiln actually needs under 0.74.
Masked by integral action rather than corrected.

**Diagnosis.** Every settled dwell tail (last 150 s of each of 12 dwell
windows across 6 hardware captures — `baseline`/`after`/`ifix`/
`holdfix_clean`/`final`/`track3zone_easeoff`) gives an independent,
extrapolation-free reading of `u_pred = A⁻¹·(T − ambient)` against the
matching `u_actual`, using the §2 coupling matrix `A` in its
`[affected][stepped]` persistence orientation and each dwell's own
first-sample actual_c as the ambient reference (the CJ-based ambient the
firmware actually uses isn't in these poll captures).

Solved as a diagonal-only single-zone gain, all three zones disagree with
their identified `K` in the *same* direction (implied K 1.5–5× identified,
worst on z0) — which looked at first like ambient or fit-method error. But
solving the **full 3×3 system** instead (all three zones' duty and
temperature simultaneously) collapses z1 to near-zero mean error
(`u_pred − u_actual` = **−0.007**, 12 samples) while z0 and z2 stay biased
and diverge in **opposite directions**: z0 **−0.086** (model under-predicts
the duty z0 needs), z2 **+0.108** (model over-predicts — the ticketed
symptom), growing with dwell temperature on z2 (~0.12–0.15 at the 45 °C
dwell, ~0.13–0.21 at 60 °C, 5 of 6 non-baseline captures).

That rules out: the ambient reference (one shared scalar can't flip sign
between zones, and z1 fits `A` with the same scalar); zone 2 physical
degradation/duty-ceiling (z0's row is off by *more*, in the opposite
direction, on the same rig, same day, same matrix); and zone 2's own
diagonal `K` in isolation (the diagonal-only check that seemed to implicate
it is exactly the confound the coupled solve resolves — see
`kilnctrl-plant-sim`/`log_analysis.py` note below).

**Conclusion: it's the coupling matrix, specifically its z0 and z2
rows/off-diagonals, not zone 2's diagonal gain.** Consistent with the
matrix's own documented provenance (§4: off-diagonals are "two-point fits
on unexcited peer traces", condition number ~5.3) and with §3.3's
already-scoped, not-yet-built "full coupled identification from dwell
observations" item — which is the right fix (re-solve `A` from the dwell
data adaptive_tune is already harvesting, or a fresh MIMO bench
identification), not a hand-edit of these matrix entries from this one
offline pass, and not a code change to compensate for what is a
measurement problem. No firmware change made here.

**HARDWARE A/B, 2026-09-03: bias metric does not predict tracking; z1 and z2
both went the opposite way it predicted.** Matched rested-start pair
(`logs/coupling/p7_oldmatrix_runC.jsonl` run 0, 31.1 °C vs
`p7_newmatrix2_http.jsonl` run 0, 30.0 °C — both re-derived with
`pid_ab_compare compare --run-a 0 --run-b 0`, single matrix throughout, no
guard/fault events, no merged runs). z0 refused (1.02 °C start delta, over
threshold). z1: whole-run IAE 1.028→0.608, dwell overshoot 1.77/2.11→1.29/1.34,
steady offset 0.82/1.08/0.58→0.30/0.43/0.11 — clearly better. z2: IAE
0.820→0.899, overshoot 1.63/1.80→2.18/2.02, offset 0.90/0.82→1.02/0.92 —
slightly worse. The `u_pred−u_actual` bias metric predicted the reverse: z1
"slightly worse" (own-diagonal −0.007→−0.053; hybrid −0.056), z2 improving
(+0.113→+0.032 own-diagonal, +0.137→+0.116 hybrid).

Confounders checked and largely ruled out: start-temp deltas are 0.93 °C (z1)
/ 0.89 °C (z2), under the 1.0 °C gate; no fault/guard fired in either run;
`ff_hold_used_matrix` is true throughout both, `ff_hold_infeasible` never
sets. The only prior data point on start-temp sensitivity (`holdfix_clean` vs
`final`, 4.8 °C delta → 22–47 % IAE swings) is itself uncalibrated, but even
naive linear scaling puts a ~0.9 °C delta at single-digit-percent swings —
far short of z1's 41 % IAE drop, and the same order as z2's 10 % rise, so it
cannot be excluded as a partial contributor to z2 but cannot be the z1 story.

Mechanism, solved numerically from `zone_coupling_solve.c`'s actual G·u=b
system (`ff_k_dc` diagonal, both matrices' off-diagonals, at the real dwell
dT's — ratios are dT-independent since the system is linear): moving from the
old to the new matrix cuts each zone's own commanded hold duty by **z0 −81 %,
z1 −48 %, z2 −5 %** (condition number 1.92→5.51, matching the doc's own
table above). z0 and z1 get a large feedforward offload; z2's own duty is
almost untouched by the matrix change and instead becomes far more dependent
on crediting its neighbours' heat (z2's row sum grows 3.18×, the largest of
the three rows) — heat that, per §2, arrives through an off-diagonal path
with 620–730 s τ and 135–158 s dead time, against 264 s / 34–53 s on the
diagonal. The `u_pred−u_actual` bias metric is computed only from **settled
dwell tails** (`coupled_ident.py`'s `u_pred = A⁻¹·(T−ambient)` against 150 s
of already-arrived, steady-state duty) — it structurally cannot see this lag.
z1's win is a genuine steady-state correction (its old-matrix duty really was
too high, and the fix front-loads instantly). z2's loss looks like exactly
the kind of transient the bias metric is blind to: z2 now leans on a much
bigger, much later-arriving credit from z0/z1, so a dwell entry or short
segment sees z2 under-driven while that credit is still in flight, PID
integral winds up to cover the shortfall, and the delayed neighbour heat
lands on top of it — overshoot, not the undershoot a static crediting
argument alone would suggest.

**Verdict on the bias metric: it is not the right metric to gate this kind of
change on.** It validates the matrix's steady-state self-consistency, not
its effect on tracking through a transient — and transients (ramps, dwell
entries) are most of what whole-run IAE and overshoot are made of. Record
this before it gets used to justify the next matrix swap.

**What would settle it:** a same-day, same-matrix repeat pair (isolates
run-to-run noise from the matrix effect, which this single A/B cannot do),
and a bias metric computed over the SAME windows `pid_ab_compare` scores
(ramp + dwell-entry, not just settled tails) so it can be compared
apples-to-apples against what actually predicted the hardware outcome. The
noise-floor campaign being built separately would settle the "is the z1/z2
swing distinguishable from run-to-run noise at all" question, but not the
transient-vs-steady-state question above — that needs the metric itself
rebuilt over dynamic windows, not more repeats of the same static one.

### 3.3 Adaptive tuning — the layers not built

Shipped (`fcc1fc0`, `a772d78`): dwell harvesting, diagonal-only least-squares
gain refinement, bounded application at run end, per-zone opt-in default off,
HTTP endpoint and zones-page UI.

- [x] **Full coupled identification** from dwell observations — built
      (`6f6c8fd`), then hardened over four review rounds (`825bd82`, `e129f7c`,
      `12d709d`, `89ff20b`). Joint-observation ring committed once per dwell,
      normal-equations solve reusing the existing pivot floor as the
      conditioning refusal, off-diagonal only, plus a physical-plausibility gate
      (rejects negative coefficients and non-dominant diagonals — the condition
      number alone passed matrices with three negative entries).
      **Clearance WITHDRAWN 2026-09-02** — the solve is sound and
      mutation-proven, but what feeds it is not. Also still unvalidated against
      the real §3.2 observations: the test matrix is synthetic, so the solve is
      proven correct, never proven *better* than the matrix it would replace.

      **The harvest layer records non-steady duties as DC-gain observations.**
      Measured on the real `coupid6` capture (six 10-minute dwells, 30–70 °C,
      three zones): the firmware settle criterion — temperature slope
      ≤ 0.003 °C/s over ≥ 180 s — **fired on 12 of 12 joint observations**, and
      on several, duty was still ranging by up to **23 % of its own value**
      afterwards. Zone 0 inside one 46 °C dwell went 0.023 → 0.19 → 0.144: the
      plant is **under-damped** at these gains, so the temperature slope passes
      through zero at the top of an overshoot while duty is still swinging. A
      momentarily flat temperature is not steady state. Since a dwell IS the
      DC-gain measurement (`K = (T_dwell − ambient)/u_steady`), a non-steady
      `u_steady` biases K directly and everything fitted from it — including the
      diagonal refine, whose clearance is withdrawn on the same evidence.
      Found by `coupled_ident.py`'s `settle_criterion_audit()`, which mirrors the
      firmware criterion then checks whether duty kept moving after it fired.
      **This also means coupid6's 10-minute dwells cannot measure DC gain at
      all** (600 s is ~2.3 tau); the single-zone excitation profiles use 35-minute
      dwells and are the primary dataset.
- [x] **Integral diagnosis from dwells** — built and hardened alongside the
      above. Classifies steady offset, drift and limit cycle; a detected limit
      cycle yields Ku/Tu without a relay test. Per-run move capped at
      `ADAPTIVE_TUNE_KI_MAX_FRACTIONAL_MOVE` (20%), cumulative growth capped at
      5x a per-zone baseline that is persisted to NVS and re-latched whenever
      the model layer writes a fresh SIMC Ki, with a symmetric lower floor at
      baseline/5. **Code-level blockers all closed and mutation-proven** across
      six review rounds: the reboot ratchet (baseline in RAM while Ki persisted,
      measured reaching 850.6x after one power cycle), the dead remedy (nothing
      cleared the baseline, so the refusal's "re-autotune" advice was a no-op —
      now cleared by `autotune_engine_accept()`), a vacuous decreasing-direction
      test, and a real board-wide deadlock reached by Accept on the UART path.
      **Never yet run on hardware.** Enable one zone first, not three.
- [x] **Dynamics from ramps** — built (`13dcf49`) and **SHELVED** (`65f6525`).
      The two-point reaction-curve fit cannot work on closed-loop firing data:
      its output reduces analytically to `0.524·K·Δduty/ramp_rate`, a function
      of the commanded ramp rate and nothing else — identical on a plant with
      any τ whatsoever. Verified by porting the fit to Python and reproducing
      both accepted fixture fits (predicted 43 s / 22 s vs fitted 35.6 / 23.5).
      Two joint causes: the response window is capped at 15 samples (153 s at
      real telemetry rates) against a true `L+τ` of 300–320 s, so the response
      is never observed; and the detrend baseline sits inside the plant's own
      dead time, so it removes nothing and leaves the ordinary ramp climb *as*
      the response. Integrating it would have cut the climb feedforward ~9x and
      shrunk the terminal ease-off window ~5x.
      **No threshold change fixes this.** A two-point fit needs a held step
      observed for ≥ 2(L+τ) ≈ 600 s; this kiln never holds duty that long in
      closed loop (exactly what `STEP_NOT_HELD` reports on 21 of 27 segments),
      and duty never saturates (0.04–0.45), so "accept only saturated segments"
      has no data here. The route that would work is a **whole-segment
      output-error / ARX estimator**: simulate a FOPDT driven by the actual duty
      trace and least-squares fit τ/L to the residual. No step, no hold, uses
      all 27 segments, extends to the coupled case. The module stays in-tree,
      unwired, as a parked experiment with the artifact pinned by a test.
- [x] **Iterative tuning** — built as `iter_tune.c/.h` (`fe14ddf`, `17f7ebd`).
      Scores each firing by the `iae_normalized` the executor already records,
      perturbs gains, and keeps the change only if the next *comparable* firing
      beats the prior score. **The blocking unknown is the noise floor**: two
      firings with identical gains do not score identically, and this kiln's
      run-to-run spread has never been measured. The only candidate pair in the
      fixtures (`holdfix_clean` vs `final`, same climb mode and integral floor)
      differs by +22.5/+47.5/+28.6 % — but it is **confounded**: those runs start
      24.6 °C and 29.4 °C respectively, a 4.8 °C difference, which is the
      residual-heat trap this document already records. So that number is an
      overestimate of unknown size and must not be quoted as the floor. The
      accept threshold is a deliberately conservative 20 % pending real data, and
      the comparability window was tightened 5.0 → 2.0 °C because the old value
      would have accepted that very pair.
      **The experiment that would settle it:** N ≥ 5 firings of one profile,
      gains fixed, each from a genuinely rested start with every zone at ambient.

      **2026-09-02e — audit of every profile-7 capture on hand, and why none of
      them supply this.** All six `logs/coupling/p7_*_http.jsonl` files, run
      through `log_analysis.split_runs`/`describe_runs` (the same multi-run
      detector `pid_ab_compare.load_run` refuses on now):

      | file | matrix | run start temp (°C) | complete? | multi-run? |
      |---|---|---|---|---|
      | `p7_fuzzy0_http.jsonl` | (era before newmatrix) | capture starts 342 s into the firing, not from a rest | done | no |
      | `p7_oldmatrix_http.jsonl` run0 (= `p7_oldmatrix_runA.jsonl`) | oldmatrix | 28.66/28.77/28.83 | done | — |
      | `p7_oldmatrix_http.jsonl` run1 | oldmatrix | 30.10/30.15/30.25 | done | yes, 2 runs in this file |
      | `p7_oldmatrix_runC.jsonl` run0 | oldmatrix | 31.06/31.16/31.08 | done | yes, 1-row idle tail |
      | `p7_newmatrix_http.jsonl` | newmatrix | 25.24/25.32/25.46 | done | no |
      | `p7_newmatrix2_http.jsonl` run0 | newmatrix | 30.04/30.23/30.19 | done | yes, 2 runs in this file |
      | `p7_newmatrix2_http.jsonl` run1 | newmatrix | 31.08/31.17/31.16 | **running, not done** | — |

      Three files hold two runs each and would have handed
      `pid_ab_compare` a silent-second-run-vs-itself comparison before the
      multi-run refusal existed. `fuzzy0` is excluded outright (truncated
      start). `newmatrix2` run1 never finished. That leaves five genuinely
      complete, rested-from-`t≈0` candidates split across **two different
      coupling matrices** — oldmatrix (3 runs, 28.7→31.1 °C, ambient drifting
      2.4 °C over the session) and newmatrix (2 runs, 25.2 °C and 30.0 °C, a
      4.8 °C gap). A true repeat needs the same matrix AND a start temp within
      a tight tolerance of the other repeat(s): only `p7_oldmatrix_http.jsonl`
      run1 (30.10 °C) and `p7_oldmatrix_runC.jsonl` run0 (31.06 °C) — same
      matrix, 0.96 °C apart — clear that bar. **At most two true repeats exist
      today; N ≥ 5 has never been run.** (`p7_oldmatrix_http.jsonl` run0 is a
      third oldmatrix data point but 2.4 °C from the other two — plausible
      evidence for, not proof of, the same "ambient drifted 3 °C tonight"
      problem this section already names.)

      **The campaign now has a turn-key form**, so the gap above can be closed
      unattended: `run_queue.py --repeat N` (extended for this — see its own
      docstring) takes ONE `--run PRESET:PROFILE_ID:LOG_PATH[:LABEL]` entry and
      expands it into N `QueueEntry`s, same preset and profile, each with its
      own `_run1.jsonl`..`_runN.jsonl` log path so two repeats can never land in
      one file (the exact failure this table found three of, after the fact).
      `run_entry` already waits for every zone to be rested (within
      `--rested-tol-c`, default 1.0 °C of its own cold junction — not an
      absolute ambient number, since ambient itself drifts night to night)
      before every single entry, so repeating the entry N times gets "rest
      between every repeat" for free:

      ```
      python -m kilnctrl.run_queue --host <board-ip> \
          --run p7_floor:7:logs/coupling/noise_floor_p7.jsonl:noise_floor \
          --repeat 6 --rested-tol-c 1.0
      ```

      Then `python -m kilnctrl.noise_floor build logs/coupling/noise_floor_p7_run*.jsonl
      --out tools/PcTools/config_presets/noise_floor.json` turns the N captures
      into the checked-in floor artifact (currently a placeholder with
      `entries: {}` — this campaign has not been run for real yet). Per
      `(zone, metric, segment)` — the same keys `pid_ab_compare.py`'s own
      comparisons use — it reports `n`, `mean`, `std_c`, and `noise_floor_c`
      (the observed range, max−min, deliberately the wider of the two on N as
      small as an overnight campaign realistically produces). `pid_ab_compare`
      loads that artifact by default: a metric delta smaller than its measured
      floor is now reported `INDISTINGUISHABLE`, not `PROVISIONAL` — see its
      module docstring and `NOISE_FLOOR_KNOWN_NOTE`. A key the campaign never
      covered still falls back to the old "noise floor unknown, PROVISIONAL"
      behavior, unchanged. `--noise-floor none` on the `compare` CLI disables
      the lookup entirely, for a caller that wants the pre-floor behavior on
      purpose.
- [x] **One-click revert** to the last accepted gain set (`5b44403`). Snapshots
      Kp/Ki/Kd, K_dc/tau/dead_time and `ki_baseline` before each commit point and
      restores them exactly, `ki_baseline` included — restoring gains while
      leaving a baseline latched from the reverted-away value would recreate the
      stale-reference defect this layer already had. Refuses **board-wide** while
      any firing is running or paused, because coupled feedforward means
      reverting one zone changes another zone's commanded duty.
- [x] Consolidate the opt-in flag into the zone config blob (`5b44403`,
      ZONES_CFG_VERSION 13 → 14). A one-shot migration carries an upgrading
      board's old `adap_tune`/`en_mask` bits into the new home, marked by
      `en_migrated` so a later explicit opt-out is not clobbered by the stale
      byte; a silent reset to default-off would have quietly disabled a layer an
      operator had turned on.

### 3.4 The simulator — calibrated 2026-09-01, recalibrated + validated 2026-09-02

For most of the 09-01 session the simulator was wrong in both directions: it
predicted 44–53% ramp recovery where hardware delivered essentially full
recovery, and it favoured the climb-decay change hardware then measured as worse
on every zone. **The whole cause was one constant: it drove itself at a
hardcoded 10 °C/min while profile 7 ramps at 2–3.5 °C/min.** Coupled climb
feedforward is linear in commanded rate, so a 3–5× rate error produced the
entire phantom lag. Now lives as `tools/PcTools/src/kilnctrl/plant_sim.py`
(`7b56a8a`), CLI: `python -m kilnctrl.plant_sim compare <capture.jsonl>`.
`kilnsim` (the MCP server) was not connected when this pass ran — use the CLI
directly, or `tools/PcTools/scripts/mcp_servers.ps1 start` first.

**2026-09-02 recalibration.** Re-fit `K_full`/`tau` from data the 09-01 pass
never saw: three rested-start, fully-settled single-zone excitation runs
(`logs/coupling/cpl_z{0,1,2}_{mcp,thermo}.jsonl`) and three passive cooldowns
(`logs/coupling/cooldown_*.jsonl`). `K_full` is now §3.2's re-solved asymmetric
matrix directly (`[[38.13,27.32,21.72],[14.30,35.90,22.15],[8.33,12.42,35.32]]`,
cond 4.64); `tau` is `[469, 455, 345]` s from the cooldowns (materially longer
than the old bench-rig `[263.8, 269.8, 270.9]` — unresolved whether the bench
rig under-measured or heating/cooling `tau` genuinely differ). `L` (dead time)
is unchanged — no new dead-time data exists. Re-run against the original five
captures, aggregate residual **improved**: `after`/`final`/`holdfix_clean` all
now RMS 0.60–0.71 °C (was 1.45 °C aggregate); the previously-flagged zone-2
second-dwell cold bias (1.8–2.6 °C) is now nearly gone (0.15 °C).
`CURRENT_MATRIX` in `coupled_ident.py` — "what firmware ships" — was
deliberately decoupled from `plant_sim.K_full` — "the sim's best physical
estimate" — so this recalibration does not silently change what the
firmware-facing adaptive-tune self-checks are calibrated against. **Fixed
2026-09-02 (`5e96424`):** `CURRENT_MATRIX` was stale in two ways at once —
the board had moved off it, and it was never even the true pre-adoption
bench matrix (it held superseded sec-2-identification off-diagonals). Split
into three explicitly named constants: `ADOPTED_HYBRID_MATRIX` (adopted
off-diagonals + `ff_k_dc` diagonal — what the board runs today, used by
`render_report`'s "current on-board matrix" and `build_coupling_report`'s
delta-vs-current baseline) and `SEC2_IDENTIFICATION_HYBRID_MATRIX` (the old
`CURRENT_MATRIX` values, renamed but unchanged — still correct for
`self_check_against_known_figures`'s historical regression fixture, which
really was computed against those numbers). `build_coupling_report`'s "old"
(now current-board) scores moved from the sec-2 matrix's biases
(−0.081/−0.040/+0.113) to the actually-running hybrid's
(−0.094/−0.056/+0.116) — matching this section's "hybrid, what actually
runs" table above exactly.

**Held-out validation** (`tests/fixtures/plant_sim/p7_fuzzy0_held_out.jsonl`,
a live profile-7 fuzzy=0 tracking run, deliberately excluded from the fit):
temperature RMS z0 3.51 °C, z1 3.15 °C, z2 3.56 °C — worse than the fitted
0.6–0.7 °C but the capture starts 342 s into the firing, so the sim's t=0
fresh-PID/plant state does not match hardware's already-settled state (a
cold-start artifact of this particular capture, not a re-identified plant
defect — the ramp segment right after the cold start carries nearly all the
error; the second ramp segment, once the sim has caught up, tracks to
0.4–1.6 °C). Test pins a 6.0 °C bound per zone. **Superseded 2026-09-02d
below:** rested-start captures now exist and confirm this was a cold-start
capture artifact, not a plant defect — pooled held-out RMS from three t=0
rested full firings is z0 1.05 °C, z1 0.61 °C, z2 0.67 °C, an
order-of-magnitude tighter figure than the 3.51/3.15/3.56 °C above.

**Known behaviours, checked against the recalibrated sim:** dwell-entry
overshoot reproduces in the right direction and rough scale but undershoots
the measured numbers — sim peaks 0.95–1.8 °C at 42–70 s post-ramp vs hardware's
documented ~2 °C at 65–145 s (real overshoot is later and larger). The coupled
hold solve's infeasibility boundary shifts up with the new matrix as measured:
~59 °C (old matrix) → ~64 °C (new), matching the ~62 °C → ~65 °C hardware
finding in direction and rough magnitude.

**High-temperature extension (added 2026-09-02, simulator only — never run on
hardware).** Every parameter above is MEASURED across roughly 0–80 °C only. A
firing runs to bisque (~1000 °C), cone 6 (~1222 °C), cone 10 (~1285 °C) — 12–16×
past the fitted range, where radiative loss (∝T⁴) comes to dominate over the
conductive/convective loss the fit captures. `plant_sim.py` now scales total
thermal conductance (and therefore both `K` and `tau`, which share it) by a
Stefan-Boltzmann small-signal term above `T_REF_C=55` (MEASURED anchor); the
radiative split itself, `RAD_LOSS_FRACTION_AT_REF=0.05`, is **ASSUMED** — no
high-temperature measurement exists to fit it. `EXTRAPOLATION_BOUNDARY_C=80`
is the explicit confidence line: `is_extrapolation()`/`run_profile()`'s
`extrapolation` flag mark anything past it. The parallel sweep
(`python -m kilnctrl.plant_sim_sweep run`, one process per band×matrix via
`multiprocessing.Pool`, deterministic band-sorted output) finds the qualitative
finding that matters most here: at the current low-temperature-fit gain
(`K_diag` ≈ 35–38 °C rise at duty=1 relative to ambient), the coupled hold
solve is infeasible and duty is fully saturated at every band from ~65 °C
upward, bisque/cone6/cone10 included — i.e. the rig's own identification
(K_diag ≈ 35–38 °C rise at duty=1) belongs to a small test fixture whose
element cannot even reach 40 °C above ambient at full power against its own
losses; no loss model turns that into a plant reaching 1285 °C.

**Physical high-temperature model (added 2026-09-02b).** Rather than keep
extrapolating the rig's fitted gain, `plant_sim.py` now also models a
*different* physical object above `EXTRAPOLATION_BOUNDARY_C`:
`PhysicalKilnPlant`, an energy-balance simulation of a real cone-10-capable
kiln, parameterized by quantities a kiln owner can look up — element wattage
(`PHYS_P_MAX_W`, ASSUMED 2.5 kW/zone), chamber wall area/thickness/k-value
(ASSUMED IFB, `PHYS_WALL_*`), thermal mass (ASSUMED, from that same
geometry) and outer-shell convection+radiation (ASSUMED `PHYS_OUTER_*`).
Radiative loss (∝T⁴) is applied at the physically correct place — the outer
shell, solved quasi-statically each step (`solve_outer_wall_temp_c`) — not
on the chamber air directly, which would overstate loss by two-plus orders
of magnitude. Cross-zone coupling reuses the bench rig's identified K_full
ratios as a MEASURED dimensionless *shape* (`PHYS_COUPLING_FRAC`), damped by
an ASSUMED `PHYS_COUPLING_SEPARATION_DAMPING=0.25` (a real kiln has more
separation between zones than the compact rig) and capped by
`COUPLING_GROWTH_CAP=2.0` — both constants exist because the undamped/
uncapped version is a genuine positive-feedback runaway, caught while
building this section (two zones' duty pinned at 0 while temperature kept
climbing on coupled power alone) and now pinned by
`test_physical_kiln_plant_stays_bounded_at_cone10` and
`test_coupling_growth_is_capped`.

**Plausibility, with these ASSUMED numbers**: steady-state cone-10 hold
needs ≈0.30 duty of the 2.5 kW element (`test_physical_hold_duty_feasible_
at_cone10`), and the sweep's 3 °C/min commanded ramp reaches cone 10 in
≈7 hours — both in the range a real firing looks like. The full coupled run
(sweep's `plant_sim_sweep.py`, now routing to `PhysicalKilnPlant` whenever a
band's target exceeds `EXTRAPOLATION_BOUNDARY_C`, `'measured'` FOPDTPlant
otherwise — `plant_regime` on every result row says which) settles cone 10
within ~5 °C mean of target with only 6–55 % of dwell samples duty-saturated
increasing toward the top of the range, i.e. required duty rises with
temperature the way a real kiln's does. This is a genuinely different,
ASSUMED-parameter physical object from the bench rig — not a claim that a
3 kW bench element reaches cone 10, which it plausibly cannot (see above).
Treat cone-band sweep rows as "how a plausible full-size kiln, driven by
firmware's *fixed, low-temperature-only* feedforward matrix, behaves" — good
for mechanism-level control-strategy comparison at high temperature, not yet
for a specific gain's third decimal place, and not a substitute for a real
high-temperature identification pass if one ever becomes possible.

**Bench-rig anchor, and the correction this section makes (added 2026-09-02c).**
`PhysicalKilnPlant` above was previously described as reducing "to the measured
bench behaviour — trivial and exact" below `EXTRAPOLATION_BOUNDARY_C`. That was
not a validation and the wording was wrong: `PhysicalKilnPlant` is a
*different, larger* object (ASSUMED 2.5 kW/zone, cone-10-scale insulation) that
`run_profile`'s `plant_regime='physical'` path only ever instantiates *above*
the boundary — it is never run against the rig's own measurements at all, so
nothing about it had been checked against a single real reading.
`plant_sim.BenchKilnPlant` fixes that gap by anchoring the *same*
energy-balance structure (own-zone power in, conductive loss out, cross-zone
coupling as a fraction of the neighbor's own power) to the rig itself, using
only measured `K_full`/`tau` plus one explicit, unavoidable assumption
(`RIG_P_MAX`: equal element wattage per zone — the three zones share an
identical relay/heater circuit per `HARDWARE.md`, and no wattage figure exists
anywhere in this repo to derive a non-uniform split from instead; real Watts
are not recoverable at all from two equations — steady-state gain and cooldown
tau — in three unknowns per zone).

Checked with `python -m kilnctrl.plant_sim bench-validate` against the three
single-zone excitation runs (`logs/coupling/cpl_z{0,1,2}_{mcp,thermo}.jsonl`,
the same data `K_full` was fit from):

| | DC gain (diag) | cooldown τ | own-zone hold | cross-zone (peer) rise |
|---|---|---|---|---|
| z0 | 38.130 vs 38.130 (±0.00) | 468.5s vs 469.0s (±0.5s) | 24.40 vs 24.40°C (±0.00) | z1→z0 22.63 vs 21.31°C (+1.32); z2→z0 18.52 vs 17.16°C (+1.36) |
| z1 | 35.900 vs 35.900 (±0.00) | 454.5s vs 455.0s (±0.5s) | 28.00 vs 27.99°C (±0.01) | z0→z1 8.62 vs 9.15°C (−0.53); z2→z1 17.79 vs 17.50°C (+0.29) |
| z2 | 35.320 vs 35.320 (±0.00) | 344.5s vs 345.0s (±0.5s) | 27.90 vs 27.90°C (±0.00) | z0→z2 4.94 vs 5.33°C (−0.39); z1→z2 9.53 vs 9.69°C (−0.16) |

**The first three columns are not independent evidence** — `RIG_G_LOSS`/
`RIG_C_THERMAL` are algebraically solved from these exact `K_diag`/`tau`
numbers, so BenchKilnPlant's own-zone dynamics reduce to FOPDTPlant's own
equation and reproducing them is a sanity check on the derivation, not a
finding. **The fourth column is the genuine test**: nothing forces the
coupling-as-power-fraction mechanism to reproduce the measured *asymmetric*
peer rises (z1 raising z0 ~22 °C vs z0 raising z1 only ~9 °C) — it could have
come out symmetric, wrong-signed, or wrong-scale. It reproduces the direction
and rough scale correctly, RMS 0.83 °C / max 1.36 °C across the six
off-diagonal cells — comparable to this module's own ~1–2 °C documented
hardware noise floor.

**Verdict: BenchKilnPlant reproduces the rig within noise, on the one
comparison that actually tests it.** This says nothing new about
`PhysicalKilnPlant`'s cone-10 parameters (`PHYS_P_MAX_W`, `PHYS_WALL_*`,
`PHYS_OUTER_*` remain fully ASSUMED and untouched — `BenchKilnPlant` is a
separate class, not a refactor of `PhysicalKilnPlant`) — it validates the
*structural choice* (power-fraction coupling, single-thermal-mass-per-zone
energy balance) those high-temperature parameters are built on, at the one
scale where real data exists to check it. No parameter was fitted to make
this match: `RIG_G_LOSS`/`RIG_C_THERMAL` are fixed by the DC-gain/cooldown
equations before the cross-zone check ever runs, and `RIG_COUPLING_FRAC`
comes directly from `K_full` with no free scale or damping term. Pinned by
`test_bench_validation_report_cross_zone_is_close_to_measured` and
`test_bench_validation_report_reproduces_measured_asymmetry_direction` in
`tests/test_plant_sim.py`.

**What it can be trusted for**, per its own module docstring: ramp magnitude and
sign on coupled-feedforward builds within the fitted ~0–80 °C envelope; dwell
behaviour generally. **Not** the uncoupled baseline's exact saturation
dynamics, not anything above `EXTRAPOLATION_BOUNDARY_C` for magnitude (mechanism
only), and nothing below the ~1 °C noise floor.

The lesson worth keeping: every sim verdict in this chain was quoted with
confidence while resting on an unvalidated driving condition. A simulator is not
evidence until it reproduces a measurement someone actually took — and it stays
a lower bound on trust until a held-out run, not just a fitted one, checks it.

**2026-09-02d — honest held-out RMS from three rested-start full firings, and
a discrimination verdict.** The 3.51/3.15/3.56 °C figure above carried a
cold-start excuse (capture began 342 s into the firing). That excuse no
longer applies: `logs/coupling/p7_oldmatrix_http.jsonl` and
`p7_newmatrix_http.jsonl` are complete profile-7 firings captured from `t=0`
at a rested kiln (28.7 °C / 25.2 °C start); `p7_newmatrix2_http.jsonl` is a
third, in-progress at analysis time (used through the 522 s it had written).
Pooling the two complete runs (`n=868` per zone, sim driven off each
capture's own segment boundaries via `run_profile_from_capture`, coupled/
ff_hold, unchanged K_full/tau): **pointwise RMS z0 1.05 °C, z1 0.61 °C, z2
0.67 °C** — a real, order-of-magnitude improvement over the cold-start
number, confirming that excuse was correct, not a cover story.

Character of the error: z2 carries a small, tight, repeatable cold bias in
every dwell window across both matrices (mean −0.85 °C, spread only 0.09 °C
over 5 windows) — genuinely systematic. A grid search scaling `K_full[2][2]`
±10 % to close it made pooled z2 RMS *worse* at every setting away from 1.0
(0.67 °C at 1.00 → 0.99 °C at 0.95, 2.29 °C at 0.90), so the bias is not a
steady-state gain error correctable by nudging `K_diag` — it lives in the
dynamics (dead time / tau / feedforward structure), left open. z1 is the
best-behaved zone (0.61 °C, no established bias). z0 is the worst and,
critically, *not* a clean bias: per-window sim−hw dwell offsets swing
−1.77…+1.85 °C and average to ≈0 — noisy, not systematic, so no simple
correction applies there either.

**Discrimination threshold.** Treating each zone's pooled RMS as the sim's
own 1σ measurement error and requiring ≥2σ separation to call a winner
between two candidates: z0 needs a true difference ≳2.1 °C, z1 ≳1.2 °C, z2
≳1.3 °C before the sim can tell two candidates apart. The control changes
actually being compared (relay-gain, fuzzy-strength) move outcomes by
~0.4–0.5 °C — 2–5× below every zone's threshold. **Verdict: no — the
simulator cannot yet accelerate this work.** It is a large, confirmed
improvement over the cold-start number (which said the same thing more
loudly), and it settles that the earlier failure was a capture artifact, not
a plant defect, but the gap to a 0.4–0.5 °C effect size remains open on all
three zones.

Ranked improvements, by expected value per unit effort: (1) a proper
per-zone re-identification using z2's dwell-entry dynamics (dead time/tau),
not steady-state gain — the one lever this pass found *doesn't* work is
worth ruling out explicitly so the next pass doesn't repeat the grid search;
(2) more rested full-run captures to pool down z0's noise (its error isn't
biased, so averaging more independent runs is the correct lever, unlike z2);
(3) per-zone rather than shared PID gains, untested this pass; (4) modelling
the 60 s PWM window instead of continuous duty, untested this pass — lowest
priority since none of the three found error patterns (z2 bias, z0 noise)
look like a PWM-quantization signature.

**Implemented this pass: the measurement chain.** `run_profile()` gained
opt-in `measurement_quantum_c`/`measurement_noise_std_c`/`measurement_seed`
args (default `0.0`/off, so every existing caller is byte-identical —
pinned by `test_measurement_chain_defaults_off_reproduces_noise_free_result`;
`test_measurement_chain_noise_and_quantization_change_the_trajectory` pins
that enabling it is not silently inert). The PID previously read
`plant.temp[i]` — true state — directly; it now can be fed a quantized
(0.1 °C, the real MAX31856 LSB) plus noisy (0.05 °C σ) measurement instead,
closing a real gap since the fuzzy layer's whole design target is rejecting
noisy-derivative behaviour a deterministic measurement can never exercise.
Re-running the two complete rested captures with it enabled moved pooled RMS
by ≤0.003 °C per zone (noise this small is swamped by the ~0.6–1.1 °C error
already present) — this feature does not close today's discrimination gap,
but it was never expected to: its value is making a future fuzzy-strength
comparison exercise the layer it is meant to test at all, not shrinking
today's held-out number.

### 3.5 Documentation — CLOSED 2026-09-01 (`d382b06`)

`PID_CONTROL.md` now carries the strength-scaling formula for the fuzzy layer,
a terminal ease-off section with its measured before/after, and an honest
"what is actually measured, versus what is not" table.

That table's finding is the useful part: **only SIMC has bench data.**
Cohen-Coon, Ziegler-Nichols and Tyreus-Luyben have never been run on this rig,
and the last two cannot be until relay-feedback identification completes on
hardware, which it never has. The calibrated simulator was deliberately NOT used
to fill those cells — it simulates profile tracking under the coupled PID, not
an autotune identification cycle, so any number it produced would be exactly the
"plausible value nobody measured" the deferral existed to prevent.

Also fixed: the file's top-of-file hardware-status banner still claimed no
thermocouple daughterboard or relay expander had ever been attached, which the
same document's own 2026-08-29/30 bench sections contradict.

### 3.6 Untested control paths

- [x] **Relay-feedback identification — CLOSED 2026-09-02.** First hardware
      completion, both blockers found and fixed, Ku/Tu reconciled against the
      FOPDT model, and the resulting gains simulated against current
      production gains: no simulated advantage large enough over the sim's
      own noise floor to justify a hardware confirmation run. No gains
      applied to the board; current gains stand.

      **The confirming run happened.** Zone 0, setpoint 45C, Tyreus-Luyben:
      `relay_valid=true`, Ku=0.19540, Tu=334.3 s, amplitude 3.03C, 5 cycles
      seen / 3 used, 1676 s total, proposed kp=0.06106 ki=0.00008
      kd=3.24009. Confirms `c84abff`'s PWM-window diagnosis.

      **A second blocker was found and fixed on the way there (`17e67ee`).**
      `autotune_engine_run_relay()` validated the relay setpoint against both
      `max_temp_c − 50` and `min_temp_c + 50` independently. On this rig's
      80C/0C zone that demanded a [50, 30] window — empty — so every
      setpoint was refused, each with one of two contradictory messages.
      Fixed with span-proportional headroom (full 50C where the span allows
      it, else span × 0.25) and a single computed window with one refusal
      message naming the real range; the 80/0 zone now accepts [20, 60].
      This is exactly the "second contributor" this section already
      predicted before either fix.

      **Reconciliation CLOSED 2026-09-02 — the apparent Ku/Tu-vs-FOPDT
      conflict was an arithmetic error in the objection, not a defect.**
      The "FOPDT predicts Tu ≈ 4L ≈ 176 s, phase −126° not −180°" claim
      omitted the relay's own hysteresis phase lag, −arcsin(h/a). Solving
      the full describing-function phase+magnitude conditions against the
      identified plant (K 39.25, τ 263.8 s, L 52.8 s) predicts Tu 287.7 s /
      Ku 0.149 against 334.6 s / 0.195 measured. Verified independently of
      the firmware: re-detecting the cycles straight from
      `logs/coupling/relay_z0_thermo.jsonl` gives periods
      333.8/335.4/338.2/330.8 s (mean 334.6 s, 0.8% spread) and
      half-amplitude 3.04 C, reproducing the firmware's reported values to
      ~1%. `relay_amplitude_c` IS the half-amplitude, and
      `pid_autotune_fit_relay()` already uses the hysteresis-corrected
      Ku = 4d/(π·√(a²−h²)) — √(3.03²−2.00²) = 2.276 gives Ku 0.1957 against
      0.1954 reported. Coupling ruled out: the peer zones show no 334 s
      oscillation synced to zone 0's switching, and the 620–730 s cross-zone
      time constant cannot close a loop within one cycle. The residual ~14%
      (Tu) / ~24% (Ku) gap is consistent with §3.7's bench-vs-firing caveat
      on τ/L. Tooling: `python -m kilnctrl.relay_ku_tu_check check`.
      **The gains are arithmetically sound; they are simply not better than
      what is already on the board — see the simulation below.**

      **Simulated first, per owner decision (2026-09-02), zone 0 only.**
      `plant_sim.run_profile()` over profile 7's real two-segment shape
      (segments derived from `tests/fixtures/plant_sim/final.jsonl` via
      `segs_from_capture`), scored with `pid_ab_compare`'s metric set, at
      rested (24 C) and warm (34 C) starts, for current gains (kp 0.0318 ki
      0.0001 kd 0.8401), Tyreus-Luyben (kp 0.06106 ki 8.303e-5 kd 3.24009)
      and Ziegler-Nichols from the same Ku/Tu via firmware's own conversion
      (`pid_autotune.c`'s `kc=0.6·Ku, Ti=Tu/2, Td=Tu/8`: kp 0.11724 ki
      7.014e-4 kd 4.89917).

      | start | gains | whole-run normalized IAE (z0) |
      |---|---|---|
      | 24 C | current | 1.456 C |
      | 24 C | TL | 1.264 C |
      | 24 C | ZN | 0.940 C |
      | 34 C | current | 0.830 C |
      | 34 C | TL | 0.902 C |
      | 34 C | ZN | 0.828 C |

      Every |gain-set − current| gap (0.003–0.516 C) is well under the
      2026-09-02d discrimination threshold for zone 0 (2.1 C, §3.4, from the
      rested-start held-out RMS) — **the sim
      cannot call a winner here**, and TL's apparent edge at 24 C flips to a
      small loss at 34 C, which is itself smaller than the sim's noise
      floor. Verdict: neither TL nor ZN has a simulated advantage large
      enough to justify a hardware confirmation run yet, independent of the
      closed Ku/Tu reconciliation above, which turned out not to be the
      blocker. Simulation only — no gains were applied to the board.

      Original diagnosis below, kept for the reasoning trail:
      `relay_law_tick()` decides the bang-bang branch every tick (1 Hz), but
      actuation went through `heater_output_duty()`, the ordinary PID
      *time-proportioning PWM* renderer, which only re-evaluates at `window_ms`
      boundaries (60 s default). A branch flip landing mid-window did not reach
      the relay for up to 60 s, at random phase — **comparable to or larger than
      this plant's entire 34–53 s dead time**, and different every cycle.
      `pid_autotune_fit_relay()` rejects above 20 % period spread
      (`RELAY_PERIOD_SPREAD_MAX`) or 35 % amplitude spread, so the runs
      completed and the *fit* refused: "cycle periods inconsistent". No guard
      was ever involved — that hypothesis was ruled out from code, since guard
      1/2's progress window resets on every duty chop below `PROGRESS_DUTY_MIN`
      and the default relay drive (0.5 ± 0.35) chops constantly.
      Fixed by `heater_output_duty_relay_step()`, which ends and restarts the
      PWM window immediately on a branch flip; the ordinary duty path is
      untouched, so PID and the STEP method are unaffected. **No guard was
      weakened.**
      (The confirming run this paragraph called for has since happened —
      see the top of this item.)
- [ ] **The fuzzy layer has never run above `strength_pct = 0`** on hardware.
      Every measurement in §2 is with it effectively off. A hardware run is
      being set up as of this writing, blocked briefly by a
      `zones_http_client` field-mapping bug (being fixed separately). A
      profile-7 baseline with `fuzzy=0` on the current build is running
      concurrently to have a same-build comparison point.

      **Simulated first, per owner decision (2026-09-02), before any
      hardware run.** `plant_sim.py` gained a line-for-line Python mirror
      of `pid_fuzzy.c`'s `pid_fuzzy_adjust()` (rule table, triangular
      memberships, `MAX_NUDGE_FRACTION`, the `strength_pct==0` short-circuit
      that must reproduce base gains bit-for-bit), plus `pid.c`'s
      `pid_rescale_integral_for_new_ki()` bump-transfer, wired into `PID`
      exactly where `profile_executor_pid_tick.c`'s
      `pid_fuzzy_prepare_gains()` calls it (same one-tick lag on
      `error_rate_c_per_s`, same "before this tick's own P/I/D" ordering).
      The strength=0 invariant is pinned by
      `tests/test_plant_sim.py::test_fuzzy_strength_zero_matches_base_gains_bit_for_bit`,
      checked against an independent reference PID implementation that
      never calls `pid_fuzzy_adjust` at all (comparing two
      `fuzzy_strength_pct=0.0` calls to each other would be vacuous — both
      take the identical code path). Mutation-proven: perturbing the
      short-circuit by a factor of 1.0000001 produced 98.5% mismatched
      samples; reverted.

      Swept strength 0/25/50/75/100 over profile 7's real segment shape
      (`final.jsonl`, current board gains kp=0.0318 ki=0.0001 kd=0.8401,
      coupled ff, `ff_hold` floor), rested (24 C) and warm (34 C) starts,
      scored with `pid_ab_compare`'s whole-run normalized-IAE metric via a
      new sim-to-`PollRow` bridge:

      | start | z | strength=0 | 25 | 50 | 75 | 100 |
      |---|---|---|---|---|---|---|
      | 24 C | z0 | 1.453 | 1.541 | 1.645 | 1.767 | 1.910 |
      | 24 C | z1 | 1.282 | 1.345 | 1.420 | 1.510 | 1.617 |
      | 24 C | z2 | 1.015 | 1.070 | 1.135 | 1.209 | 1.293 |
      | 34 C | z0 | 0.923 | 0.948 | 0.974 | 1.004 | 1.035 |
      | 34 C | z1 | 0.783 | 0.805 | 0.830 | 0.858 | 0.886 |
      | 34 C | z2 | 0.669 | 0.679 | 0.692 | 0.707 | 0.725 |

      Largest spread across all five strengths, either start: 0.457 C
      (z0, rested). The 2026-09-02d discrimination threshold (§3.4, from
      the rested-start held-out RMS) is 2.1/1.2/1.3 C per zone — **the
      spread is well under that**, so per the
      honesty gate this sim cannot call a winner; every strength is
      indistinguishable from strength=0 given what this model can actually
      resolve. Worth recording anyway: the ranking is monotonic — every
      zone, both starts, strength=0 scored best and every increase in
      strength scored worse, no crossover. A monotonic-but-sub-noise-floor
      trend is not evidence the fuzzy layer hurts; it is exactly the
      "cannot call a winner" case the gate exists for, and does not by
      itself justify a hardware confirmation run.

      **Comparison tooling built and proven 2026-09-02**, ahead of the
      `fuzzy=50` run it will score: `tools/PcTools/src/kilnctrl/
      http_capture_log.py` parses the `{"t","exec","status"}` HTTP-capture
      shape (`logs/coupling/p7_fuzzy0_http.jsonl` /
      `p7_fuzzy50_http.jsonl`) into `log_analysis.PollRow`, and
      `pid_ab_compare.py` (`kilnctrl-pid-ab-compare compare`) reports, per
      zone: whole-run and per-segment normalized IAE, ramp mean/worst
      error, dwell-entry overshoot peak/time-to-peak, dwell steady-state
      offset, and settle time — the same figures §2's table already uses.
      It refuses to declare a winner for any zone whose start-temperature
      delta between the two runs exceeds 1.0 °C, and always prints "noise
      floor: UNKNOWN" — the run-to-run noise floor has never been measured
      on this rig (§3.3, §3.7), and the only data point bearing on it (a
      4.8 °C-confounded pair producing 22.5–47.5 % swings) is evidence a
      confound can dominate, not a calibrated floor. Interim numbers on the
      `fuzzy=0` baseline alone, run 1 segment 0 (ramp to 45 °C + dwell,
      still in progress): whole-run-so-far normalized IAE z0 0.62 °C / z1
      0.49 °C / z2 0.90 °C; dwell-entry overshoot z0 +0.41 °C@449s, z1
      +1.15 °C@90s, z2 +2.02 °C@90s. `fuzzy=50` has not run yet.

      **Audited 2026-09-02: correct-but-unhelpful, not defective.** The
      monotonic strength-vs-tracking result above raised an obvious
      suspicion — an inverted error sign or a transposed rule table would
      look exactly like this. Checked and ruled out: `pid_fuzzy.c`'s
      `RULE_TABLE` matches `pid_fuzzy.h`'s documented table cell-for-cell
      (verified all 9 cells by hand, not just the 4 "large error" corners
      the existing test suite covered); the error convention
      (`setpoint − measurement`, POS = too cold) and the rate convention
      (`d_filtered = −d(measurement)/dt`, so a climbing kiln reads NEGATIVE
      = FALLING = "error closing") are both self-consistent between
      `pid.c`, `pid_fuzzy.c`, and `profile_executor_pid_tick.c`'s wiring.
      The bump-transfer on a per-tick Ki move
      (`pid_rescale_integral_for_new_ki()`, hazard 3) is also sound: it
      preserves `ki·integral` across the gain change, and the `−ff_hold`
      floor (§4) recomputes `state->integral` from the floor directly
      whenever it binds, so a floored zone's `i_term` is exactly `−ff_hold`
      regardless of which Ki the fuzzy layer picked that tick — floored and
      unfloored ticks are both accounted for correctly.

      The actual explanation: the design intent, stated in `pid_fuzzy.h`'s
      own rationale, is "near setpoint favors more Ki and less Kp/Kd — fine
      settling, not chasing noise." `plant_sim.py` is deterministic and
      noise-free, so there is nothing for that trade to buy back — reducing
      Kp/Kd during the well-tracked stretches (the ZERO/STEADY cell, which
      is where a good tracker spends most of its time) only removes
      responsiveness with no compensating noise-rejection benefit in this
      benchmark. That is a real property of a noise-free sim, not a defect
      in the controller: on hardware, where the noise this trade is
      designed against actually exists, the same nudge could net positive.
      §3.7's validation gap already says results here don't transfer to
      firing temperature; this adds "and not to a noise-free bench either,
      for this specific layer."

      **Test gap closed.** The pre-existing `test_pid_fuzzy.c` asserted
      direction on all 4 "large error" corner cells (POS/NEG × RISING/
      FALLING) plus strength=0/NaN-input/monotonicity/clamping — real
      coverage, but silent on the ZERO row and the STEADY column, i.e.
      exactly the cells that fire during good tracking and that this
      audit's explanation turns on. Added 5 cases (ZERO/FALLING,
      ZERO/STEADY, ZERO/RISING, POS/STEADY, NEG/STEADY), each expected
      direction transcribed from `pid_fuzzy.h`'s documented table (not
      from the `.c` file, so an inversion has something independent to be
      caught against). Mutation-proven: inverting the ZERO/STEADY cell's
      direction reproduced 3 failing checks at exactly that test
      (`ZERO error, STEADY rate: kp/ki/kd nudged {down,up,down}`), all
      other 5961/5964 host-test checks unaffected; reverted. No production
      code changed — `RULE_TABLE` was correct as found.

      **Re-run WITH the measurement chain enabled, 2026-09-02.** The audit
      above pinned the noise-free sweep's flat/monotonic result on there
      being nothing in a deterministic sim for the fuzzy layer's
      near-setpoint Kp/Kd ease-off to buy back. `run_profile()`'s
      `measurement_quantum_c`/`measurement_noise_std_c`/`measurement_seed`
      (added in `5ee1990`, default off) close that gap: 0.1 °C MAX31856-LSB
      quantization plus 0.05 °C Gaussian noise, applied to the value each
      zone's PID reads, everything else (gains, ff, floor, profile shape)
      unchanged. Re-ran the identical strength 0/25/50/75/100 sweep over
      the same `final.jsonl`-derived segment shape, rested (24 °C) and warm
      (34 °C) starts, with noise on — 20 seeds per (start, strength) cell
      (`measurement_seed=0..19`; stopped at 20 because seed-to-seed std at
      every cell was ≤0.001 °C, two orders of magnitude below the smallest
      strength-to-strength gap, so more seeds could not have changed the
      read):

      | start | z | 0 | 25 | 50 | 75 | 100 | seed std (max over strengths) |
      |---|---|---|---|---|---|---|---|
      | 24 C | z0 | 1.453 | 1.541 | 1.645 | 1.767 | 1.910 | 0.001 |
      | 24 C | z1 | 1.282 | 1.344 | 1.420 | 1.510 | 1.617 | 0.001 |
      | 24 C | z2 | 1.015 | 1.070 | 1.135 | 1.209 | 1.293 | 0.001 |
      | 34 C | z0 | 0.923 | 0.948 | 0.973 | 1.004 | 1.034 | 0.001 |
      | 34 C | z1 | 0.783 | 0.805 | 0.830 | 0.858 | 0.886 | 0.001 |
      | 34 C | z2 | 0.669 | 0.679 | 0.692 | 0.707 | 0.725 | 0.001 |

      Noise model sanity-checked before trusting these numbers: a held-flat
      dwell's fed measurement lands exactly on the 0.1 °C grid every tick,
      the reconstructed pre-quantization noise std comes back 0.0499 °C
      against a 0.05 °C target, and the same `measurement_seed` reproduces
      byte-identical trajectories while a different seed diverges — pinned
      by `test_measurement_chain_quantization_and_noise_magnitude` in
      `tests/test_plant_sim.py` (mutation-proven: inflating the applied
      sigma 4× to 0.20 °C at the call site made the test's own settle guard
      fail red — `plant did not settle` at spread 0.084 vs the 0.05 bound —
      before the std check was even reached; reverted, suite green again).

      **Correction, opus review round four finding 2 (2026-09-02).** The
      test named above did not actually read the value `run_profile()` fed
      to the PID — it reconstructed noise+quantization locally from the
      TRUE temps in `result['temps']` and checked properties of its own
      reconstruction (a grid-rounding check that is tautological, and a
      sigma check against its own hardcoded 0.05). Proof it was vacuous:
      stubbing `run_profile()` to ignore
      `measurement_quantum_c`/`measurement_noise_std_c`/`measurement_seed`
      entirely still passed it. `run_profile()` now returns a `measured`
      array (the actual fed series) and the test reads that instead, plus
      checks same-seed reproducibility and different-seed divergence
      directly. Re-mutated (measurement args ignored at the call site) and
      confirmed this version goes red:
      `AssertionError: measured series does not land on the 0.1 °C
      quantization grid -- measurement chain is not being applied`;
      reverted, suite green again. **The chain itself was real, not
      inert** — the noise/quantization code path in `run_profile()` is a
      straight-line application to `meas_c` before the PID update, with no
      earlier bypass, so the ranking/conclusion above is unaffected. What
      changed is the strength of the proof, not the result.

      **Ranking unchanged, and the honesty gate still applies for the same
      reason.** Every noisy-sweep number is within ~0.001–0.005 °C of the
      corresponding noise-free number above (seed-averaging over 20 draws
      leaves essentially no residual noise contribution at this sigma) —
      strength=0 still scores best at every zone and both starts, still
      monotonically worse with increasing strength, no crossover. The
      2026-09-02d discrimination threshold (2.1/1.2/1.3 °C per zone) still
      dwarfs the largest strength-to-strength spread (0.457 °C, z0 rested),
      so **the sim still cannot call a winner** — this is not new evidence
      the layer helps, and now it is not because the sim was structurally
      blind to the trade either: the trade was given a real, verified
      noise/quantization signal to react to and made no measurable use of
      it at this sigma. Two readings are both consistent with the data:
      (a) the near-setpoint Kp/Kd ease-off's benefit, if any, needs noise
      larger than this rig's ~0.05 °C thermocouple floor to show up in a
      whole-run IAE metric, or (b) it does not net positive here at all.
      Distinguishing those needs a real firing, not a bigger sweep — a
      hardware fuzzy run is not disqualified by this result, but this
      result alone still does not justify one; the case for running it (or
      not) rests on §3.6's other open items, unchanged by this addition.

      **Crossed with load, 2026-09-02 — see §3.8's closing subsection.**
      This sweep and §3.8's load sweep each varied only one axis; crossing
      them (`fuzzy_load_sweep.py`) still finds strength=0 best almost
      everywhere across mass 1.0–4.0x, with one small, honesty-gate-failing
      exception at the heaviest/most-shelved corner (4.0x mass, 0.7x
      coupling) — not large enough to change this section's conclusion.

### 3.7 Validation gap

Everything above is measured on a bench rig spanning 0–80 °C. Radiative transfer
goes as `T⁴`, so the plant at kiln temperatures is not the plant identified here.
Every result in §2 validates the **mechanism**, not the behaviour at firing
temperature. This stays open until a real firing.

### 3.8 Load sensitivity — never tested on hardware, simulator-only 2026-09-02

The bench identification (K_full/tau §3.2) and the ADOPTED matrix were measured
at whatever load happened to be on the rig for the excitation/cooldown runs, and
the bench kiln has never since been fired at a different one. `load_mass_sweep.py`
sweeps two ASSUMED axes off that one anchor point (module docstring has the full
model): **mass_mult** scales `tau` only (more thermal mass ⇒ slower response, DC
gain unaffected — follows from `tau = C/G_loss`, `K = P_max/G_loss` sharing
`G_loss`, which IS measured); **coupling_mult** scales K_full's OFF-diagonal only
(shelves/ware blocking inter-zone view factor — the owner's own framing).
Controller feedforward always uses the fixed, unloaded matrix, exactly like the
firmware (no load sensor).

Profile 7 (measured/bench-rig plant, trustworthy — current production gains,
ADOPTED matrix), coupling held at 1.0 (pure mass effect): worst |mean ramp/dwell
error| across zones grows roughly linearly with mass — 0.65 °C at 1.0x (the
identified load) → 1.49 °C at 1.5x → 2.26 °C at 2.0x → 3.52 °C at 3.0x → 4.43 °C
at 4.0x. **z0's error crosses its 2.1 °C discrimination threshold between 1.5x
and 2.0x load**; z1 (1.2 °C threshold) and z2 (1.3 °C threshold) cross earlier,
around 1.0–1.5x, driven by z2's already-documented cold hold bias (§3.2)
compounding with mass. Below 1.5x nothing here clears the honesty gate. Duty
saturation on z2's second ramp is the earliest-moving metric, not tracking error:
it is already 41–78% at 1.0–2.0x load and pinned at 100% by 3.0x, before the
mean-error metrics clearly separate from noise — **saturation, not ramp/dwell
error, is the first symptom of an underloaded kiln getting heavier.** No
oscillation appeared at any swept mass/coupling combination — the feedforward
mismatch shows up as a growing steady bias and slower ramp tracking, not
instability. The coupled hold solve's feasibility is unaffected by load in this
model (the controller's K_inv never sees the load), so that failure mode stays
purely temperature-driven (§3.4), not mass-driven.

The cone-6 built-in schedule (C6DHSC, PhysicalKilnPlant/ASSUMED regime above
80 °C) is **not a usable load-sensitivity test as built**: even at the reference
1.0x load this schedule's own climb segments already duty-saturate (~98%) and
run 12–50 °C behind target from the ASSUMED element wattage/thermal-mass
constants alone (§3.4/3.7's open validation gap), before load is varied at all.
Comparing 1.0x against 4.0x on top of that pre-existing mismatch is not a clean
signal — it is reported here as a limitation, not a finding, so it doesn't get
mistaken for a real high-temperature load result.

Is load observable from data the firmware already has? Nothing here answers
that with hardware evidence — it would need an in-flight step-response fit
(commanded duty vs. temperature rate) compared against the identified τ, which
this repo has no capture of. Flagged as the natural next step, not measured.

**That step was taken 2026-09-02: `tools/PcTools/src/kilnctrl/load_estimator.py`
(new, additive; `tests/test_load_estimator.py`). Verdict: NOT actionably
observable from today's captures — a clean negative, closing this question.**

*The estimator.* The plant obeys `dT_i/dt = (u_ss_i - T_i)/tau_i`,
`u_ss_i = ambient + K_i·duty_delayed` (exactly `FOPDTPlant.step`'s own
update, `K`/`L` the ADOPTED matrix/dead time already on the board). That is
a one-parameter regression through the origin — `dT/dt` against the "drive"
term `u_ss - T` — whose slope is `1/tau`; `mass_mult = tau_est / tau_identified`.
Needs only what a `profile_exec` poll capture already has (commanded duty,
`actual_c`, per zone) — no new firmware logging. Two conditioning rules,
both found empirically and now load-bearing (mutation-tested):
drive-term samples below `min_drive_c` (near a dwell, where noise dominates
the near-zero denominator) are excluded, and so is the first `L[zone]`
seconds of any regression window — the delay reconstruction has no duty
history before the window starts, so it zero-order-holds the first sample
backward; skipping that exclusion alone turns a true 4.0x sim run into a
measured 4.53x, a >10% error the test suite now pins against regressing.

*Simulation validation (known truth, noise-free):* recovers the injected
`load_mass_sweep.py` mass multiplier to **<1% error at every tested load
(1.0x/1.5x/2.0x/3.0x/4.0x), R²>0.9**, provided the regression runs over one
contiguous ramp segment (pooling across a dwell gap breaks the delay
reconstruction's contiguity assumption — an early implementation mistake,
caught and fixed here, not a property of the method). Adding the honesty
gate's own measurement chain (0.1 °C quantization + 0.05 °C noise) barely
moves it (<0.05x error at 2.0x) — **sensor noise is not the limiting
factor.** Convergence: at 2.0x load, the estimate is within 2% by ~120–180 s
into a ramp and within 1% by ~180–240 s — call it "one dead time plus 2–4
minutes" as the data budget.

*Real captures* (`logs/coupling/p7_*_http.jsonl` + the rested-start
`tests/fixtures/plant_sim/*.jsonl` firings, all profile 7, same
unknown-but-constant load, 17 usable ramp windows total): estimates do
**not** cluster near a consistent multiplier and the fit itself is mostly
not distinguishable from noise. Zone 0 (best-behaved): range 0.16x–1.32x,
median 0.54x, R²>0.3 in only 4 of 17 windows. Zone 1: range 0.16x–1.11x,
median 0.57x, R² never exceeds 0.3 (max 0.05) — essentially no linear
relationship recovered. Zone 2: range 0.18x–1.8x, median 0.74x, R² as low as
−11.6. This is the inconsistency the task anticipated as itself the finding.

*Why the sim result doesn't transfer, per the honesty gate:* the estimator
is exact given the true K/tau/L; simulation validates only that the
regression math is sound, and inherits none of the real plant's model
error. On hardware, `u_ss` is computed from the ADOPTED `K`/`L` — which
the pooled held-out RMS already puts at z0 1.05 / z1 0.61 / z2 0.67 °C
(sec 3.2), and whose off-diagonal (cross-zone) dead time is separately
measured at 135–158 s against 34–53 s on the diagonal (sec 2) while this
estimator (like `load_mass_sweep.py`'s own FOPDT model) applies one
per-zone dead time uniformly to every column of `duty`, own-zone and
cross-zone alike. That structural mismatch, not sensor noise, is the
leading suspect for why real fits are frequently anti-correlated (negative
R²) rather than merely noisy — it would need per-zone AND per-source dead
times to test, which no identification run in this repo has produced.

**Bottom line: load is not observable in practice from the captures and
model this repo has today.** The estimator is provably correct in
simulation and provably unusable on the only hardware evidence available —
both facts recorded, closing this section's open question without
overclaiming. Nothing here should be used to gate-schedule or otherwise act
on a real firing's load; the natural next step, if this is revisited, is a
coupling-matrix re-identification that fits per-source dead time (matching
sec 2's own 3–4x diagonal/off-diagonal split) rather than the single-delay
FOPDT shortcut both this module and `load_mass_sweep.py` share.

**Gap closed 2026-09-02: fuzzy strength × load, never crossed before now.**
§3.6's fuzzy sweep and this section's load sweep each varied one axis while
holding the other at its single tested point (fuzzy at the identified/1.0x
load; load at fuzzy=0). Neither answers whether the fuzzy layer — whose
stated intent is robustness to a plant perturbation, just framed as
temperature rather than mass — helps precisely where §3.2/this section found
the fixed feedforward matrix degrading: z2's duty saturating first as load
rises, while the matrix has no load input at all.

`tools/PcTools/src/kilnctrl/fuzzy_load_sweep.py` (new, additive — does not
edit `plant_sim.py` or `load_mass_sweep.py`) crosses fuzzy strength
0/25/50/75/100 with mass 1.0/1.5/2.0/3.0/4.0x × coupling 1.0/0.7x, profile 7,
current production gains, ADOPTED matrix, measurement chain always on (0.1 °C
quantum + 0.05 °C noise, 5 seeds/cell — seed-to-seed IAE std ≤0.002 °C at
every cell, i.e. the seed spread never bound anything below). 750 runs, whole-
run normalized IAE per zone plus ramp/dwell/steady/settle/duty-saturation:

**Strength=0 still wins at every load and coupling tested except one
extreme corner: 4.0x mass with 0.7x coupling (heaviest mass, most
shelved/damped coupling), where z1 and z2's mean IAE decrease
*monotonically* as strength rises (z1: 3.418→3.340 °C strength 0→100; z2:
3.864→3.785 °C)** — the one cell where the ranking from §3.6 actually
reverses. **The reversal does not clear the honesty gate**: the margin
(0.078 °C z1, 0.079 °C z2) is two orders of magnitude below both zones'
discrimination thresholds (1.2/1.3 °C) and, separately, would need to beat
the ~0.001–0.002 °C seed std by more to be trusted at all — it clears easily
on seed spread alone but not on the threshold, so per the same honesty gate
§3.6 established, **no winner is called even here**. Every other
(mass, coupling) cell shows the flat/monotonic strength-0-best pattern §3.6
already found, with gaps growing roughly with load (e.g. z0 at 1.0x/1.0
coupling: 0.657→0.994 °C strength 0→100; at 3.0x/1.0: 2.595→3.134 °C) but
never approaching threshold either.

**Direct answer: no, no fuzzy strength beats strength 0 by a margin this sim
can stand behind, at any load or coupling tested, 1.0x through 4.0x mass —
a clean negative across the full load range, closing the gap the owner
flagged.** The one sign of a genuine crossover (4.0x mass, 0.7x coupling,
z1/z2, strength>0 wins) is worth recording as a hint of direction, not a
result: it is real (monotonic, not noise — seed std two orders of magnitude
below the margin) but far too small to matter, and it appears only at the
single most extreme corner of the grid, not as a trend building up to it
from 1.0x. Mutation-tested in `tests/test_fuzzy_load_sweep.py` (loop fidelity
pinned bit-for-bit against `load_mass_sweep` at strength=0/no-noise; honesty-
gate arithmetic checked against both bounds independently, mutating the
seed-spread check out of the gate reproduced a failure at exactly the
seed-spread test, reverted).

**Bottom line: only the profile-7 (bench-rig-fit) results clear the honesty
gate, and only at 1.5–2.0x load and up. The cone-schedule numbers are simulator
artifacts of an already-unvalidated high-temperature model, not a load result.
Gain scheduling by load is not justified by this alone — it would need a real
multi-load firing, which has never been run.** See
`tools/PcTools/src/kilnctrl/load_mass_sweep.py` (additive module, does not
touch `plant_sim.py`'s core) and `tools/PcTools/tests/test_load_mass_sweep.py`.

### 3.9 Tuning-method recommendation campaign — simulator-only 2026-09-02

Owner request: try the different autotune methods this firmware implements,
tuned on an EMPTY kiln (the only way `autotune_engine.c` ever runs one), then
test-fire to different peak temperatures at different loads and turn the
result into a recommendation the web GUI can show. The interesting question
isn't "which method tunes best at the condition it was tuned at" — it's
whether an empty-kiln gain set holds up once the kiln is loaded.

**Methods simulated, mirroring `pid_autotune.c` exactly (no invented
conversions):** SIMC and Cohen-Coon (FOPDT step-test path,
`pid_autotune_tune_from_fopdt`) and Ziegler-Nichols and Tyreus-Luyben
(relay-feedback path, `pid_autotune_tune_from_relay`). "step" itself is the
test the first two are fitted from, not a fifth rule. The FOPDT fit uses the
classic two-point (28.3%/63.2%) crossing method but not the firmware's
iterative end-of-trace asymptote correction — a documented simplification
(the campaign's step tests run to full settling, where that correction is a
near no-op by the firmware code's own comment); the tuning-rule arithmetic
consuming the fit is copied verbatim.

**Plant + load model:** tuning and evaluation both reuse existing objects
rather than inventing a new one. The one bench-scale peak (60 C, at or below
`EXTRAPOLATION_BOUNDARY_C`) tunes and evaluates against the real bench-rig
identification (`FOPDTPlant`, `K_full`/`tau`/`L`); the three firing peaks
(bisque 1000 C, cone 6 1222 C, cone 10 1285 C) tune and evaluate against the
ASSUMED full-kiln model (`PhysicalKilnPlant`). The load axis reuses
`load_mass_sweep.py` (landed before this campaign was written) unmodified:
`mass_mult` 1.0x/2.0x/4.0x, coupling fixed at its identified value. Evaluation
runs pure PID (no feedforward) so the comparison isolates each method's own
gains from the shared `ff_hold`/`ff_climb` formula. See
`tools/PcTools/src/kilnctrl/tuning_campaign.py`.

**Scoring, and a mistake caught before it shipped:** the first pass scored
each method by worst-case *dwell overshoot* across loads. That metric is
clamped at 0 on the undershoot side, so a method whose gains were too weak to
ever approach the setpoint (SIMC at firing temperatures — see below) reported
*zero* overshoot and ranked as "best," backwards from the truth. Fixed by
scoring worst-case **absolute steady-state offset** instead, and by adding an
explicit `unreachable` flag (>20 C chronic undershoot at dwell end) that
excludes a cell from the ranking rather than letting it participate as a
false zero.

**Results.** At the bench-scale peak (measured regime, all four methods
usable), Cohen-Coon and Ziegler-Nichols are within the sim's 2.1 C
discrimination threshold of each other — **indistinguishable**, pick either.
At all three firing peaks: SIMC's gains (tuned at the empty-kiln
identification's own long lambda) were too weak to track this campaign's ramp
rate even at the *identified* (1x) load — chronic hundreds-of-degrees
undershoot on every evaluated load, `unreachable` at all three and therefore
excluded from ranking entirely. Cohen-Coon overshoots badly at 1x/2x
(guard-trip-worthy, ~35–38 C peak) — usable but clearly worse than the other
two. Ziegler-Nichols and Tyreus-Luyben both track well at 1x/2x (steady
offset under 2 C, no guard trips, no oscillation) and are within the 2.1 C
discrimination threshold of *each other* — **indistinguishable**, pick
either — at all three firing peaks. All four methods fail the same way at
4x load (chronic undershoot, duty saturated): a shared ramp-rate-vs-thermal-
mass ceiling this campaign's ramp schedule hits regardless of tuning method
at the heaviest evaluated load, excluded from the ranking score (see the
scoring fix above) rather than left to swamp the comparison between methods
that both hit it equally. Read the full per-(method, peak, load) table by
re-running `python -m kilnctrl.tuning_campaign` before trusting any specific
number over what's in this paragraph.

**Honesty gate.** Every recommendation row carries `confidence`:
`measured` only below `EXTRAPOLATION_BOUNDARY_C` (80 C) — the sim's own
z0/z1/z2 held-out RMS is 1.05/0.61/0.67 C, discrimination thresholds
2.1/1.2/1.3 C — `extrapolated` above it (a mechanism comparison, not a
calibrated prediction), and `indistinguishable` whenever the margin between
the top two methods is under the threshold, which overrides both other
labels. **Every firing-temperature (>80 C) row in the current artifact is
`extrapolated`** — none is close enough to call `indistinguishable` — treat
the SIMC/Cohen-Coon/Tyreus-Luyben/Ziegler-Nichols ranking at bisque/cone6/
cone10 as which failure mode a method has, not a calibrated prediction of its
margin.

**Artifact:** `tools/PcTools/config_presets/tuning_recommendations.json`
(schema documented in the generator's docstring — flat, no nested objects in
any recommendation row, so it stays embeddable in firmware flash), validated
by `tools/PcTools/tests/test_tuning_recommendations.py`. That test's honesty
gate (`test_no_measured_claim_above_extrapolation_boundary`) was mutation
tested: forcing the cone-10 row to `confidence: "measured"` fails it with
`AssertionError: 'measured' == 'measured' : peak_temp_c_max=1285.0 exceeds
the 80.0 C extrapolation boundary but claims 'measured'`. Regenerate with
`python -m kilnctrl.tuning_campaign` from `tools/PcTools/src` on `PYTHONPATH`
(takes several minutes — the empty-kiln step/relay identification runs alone
simulate tens of thousands of seconds of plant time to reach genuine
steady state).

---

## 4. Rejected approaches — do not re-propose without new evidence

Three control-theory ideas were designed and rejected or reverted on 2026-09-01.
Each was plausible; recording why they failed is cheaper than rediscovering it.

**Coupling lead compensation (three design rounds, never implemented).** The
off-diagonal delay is real, but every bounded design either reduced at t=0 to the
diagonal-alone solve — bit-for-bit the uncoupled climb formula measured that same
day as a 10× over-drive on zone 0 and 5 °C overshoot — or, once the correction
was sized honestly against a 1.5 °C budget through the coupling matrix, was worth
under 1 °C-equivalent and inert whenever the integral sat on its floor.
The lagged sequential form is Jacobi iteration on `A·u = b` with spectral radius
**0.984** on this matrix: it converges in about 11 hours, and a 2% error in the
off-diagonals — which are two-point fits on unexcited peer traces — pushes it
past 1 into divergence.

**Integral floor at `−ff_u` (`b7289db`, superseded by `e690f8a`).** Flooring the
integral at the *total* feedforward means a bound integral exactly cancels the
whole feedforward, so commanded duty runs on P+D alone — and during a constant
ramp, once bound it does not release until the ramp ends. The floor must be
`−ff_hold`, the steady-state component only. Because the commanded rate is
exactly zero during a dwell, the climb term is exactly zero there and the dwell
behaviour is byte-identical to the old floor; only the ramp changes. Any future
change of this shape must check what the bound value is made of.

**Decaying the climb term into the dwell (`b8b192d`+`a77db88`, reverted
`f9d8445`).** Intended to cover the plant's dead time instead of stepping the
climb term to zero at the boundary. Measured worse on every zone in both dwells,
and on the second dwell drove an oscillation that tripped thermal guard 2
("heating commanded but temperature falling") and aborted the firing. Holding
*more* climb duty into a dwell adds heat exactly where the plant already has too
much. The guard trip was correct.

Two process notes from those three. A supporting simulation showing a gain on
one zone of three is a weak signal, not a green light. And the simulations in
this chain have been unreliable in **both** directions — one predicted 44–53%
ramp recovery where hardware delivered essentially full recovery; another
favoured a change that made hardware worse. Treat their mechanism comparisons as
informative and their magnitudes as not.

---

## 5. Rules that keep being relearned here

- **A dwell is a DC-gain measurement.** Steady duty against steady rise over
  ambient, with no asymptote to extrapolate and no settle detector to get wrong.
  Every bug in the overshoot chain existed because a step test must guess where
  the temperature would eventually land; a dwell is sitting at the answer.
  Different phases of a firing identify different parameters, and each must only
  be used for what it can support — fitting τ from a dwell is fitting noise.
- **Autotune needs a rested baseline.** Residual heat biases the fitted gain low;
  check every zone is at ambient, not just the one under test.
- **Never test a control change at rate = 0 or error = 0.** Neither can
  distinguish a climb-related change from a hold-related one. Two vacuous tests
  shipped in this area for exactly that reason.
- **Recommend rather than auto-apply, by default.** Silently rewriting the gains
  of a kiln that fires unattended, on evidence from a run nobody reviewed, is how
  a bad firing happens that nobody can explain afterwards. Auto-apply is opt-in,
  bounded per run, recorded, and revertible.
