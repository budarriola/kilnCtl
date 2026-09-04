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

**Solver switch itself LANDED 2026-09-02, still OFF by default.**
`zone_coupling_solve_hold()`/`_climb()` gained a `use_measured_diag_k_dc`
parameter: true tries `zones_config_get_coupling_diag_k_dc(member, &v)`
first for each system member's `G[row][row]` and uses it when the call
succeeds and `v` is finite and `> 0.0f`, falling back to `ff_k_dc` exactly
as before otherwise — the same guarded-fallback shape `diagonal_hold`/
`diagonal_climb`'s own `ff_k_dc` reads already use. The one caller
(`profile_executor_feedforward.c`'s `solve_hold_for_zone()`/
`solve_climb_for_zone()`) passes a single file-scope constant,
`s_coupling_use_measured_diag_k_dc = false` — the flag this section called
for, compiled in but off, so shipped behaviour is unchanged until someone
flips one constant after weighing the two items still open below. Pinned by
`test_zone_coupling_solve.c` (now 5 cases): the pre-existing 3 still pin the
flag-off/hybrid answer, plus two new cases proving the flag actually
switches sources (flag on + `coupling_diag_k_dc` populated reproduces the
own-diagonal hand-solve, 0.2632, not the hybrid's 0.1614) and that it
degrades safely when unmeasured (flag on, nothing populated, reproduces the
hybrid 0.1614 exactly). Mutation-proven: forcing the helper to ignore the
flag and always return the fallback reproduced
`got 0.1614, want 0.2632` on the flag-on test; reverted, suite green again
(18/18 in that executable, 20/20 executables overall). `build_kilnfw`
confirmed OK after the change (no target-build regression).

Still open, unchanged from before this pass: an on-board identification
pass to actually populate `coupling_diag_k_dc` (today it can only be set by
hand or by a PC-side preset built from `coupled_ident.py`'s offline
analysis, never by autotune). Flipping `s_coupling_use_measured_diag_k_dc` to
true today would only change behaviour on a board that already has
`coupling_diag_k_dc` populated by hand/preset for at least one zone; on
every other board it is a no-op by construction (the unmeasured-fallback
case above).

**ON-BOARD IDENTIFICATION PASS — LANDED 2026-09-03, mechanism only, no board
run yet.** No new identification path or state machine: `autotune_engine.c`
already fits the exact data needed. `autotune_coupling_matrix_t`'s own doc
comment has always said "i == j is the direct/diagonal gain, identical to
the model in `autotune_engine_status_t` for the run that produced it" --
`finalize_fit()` (TODO.md 6A.5(b) block, ~line 1346) already fills
`s_at.coupling.cell[zone_index][zone_index]` from the SAME direct FOPDT fit
(`s_at.model`) that `autotune_engine_accept()` writes to `model_k_dc` via
`zones_config_set_model()`. This is exactly the "same kind of data" this
field needs to mean the same thing as the matrix it plugs into: a rested
single-zone step excitation, held to settle, fit with
`pid_autotune_fit_fopdt()` -- the identical trace and duty step the
off-diagonal cross-gain cells right above it in `finalize_fit()` are
already fit from and already persist unconditionally (not gated by operator
Accept, since those are peer-zone cells with no per-cell accept UI).

The diagonal cell IS the zone's own model, so it goes with the zone's own
model, at the moment the operator accepts it -- `autotune_engine_accept()`
now calls `zones_config_set_coupling_diag_k_dc(zone, m.k_gain_c_per_duty)`
immediately after `zones_config_set_model()` succeeds, using the identical
`m.k_gain_c_per_duty` value, gated by the identical settled/
extrapolation_converged/tau_consistent_with_gain trustworthiness bar (or an
explicit `ack_unsettled` override) that already gates `model_k_dc` and the
tuning-quality record. No separate quality gate was invented. A persist
failure is logged, not propagated, matching `model_persisted`'s own
handling immediately above it.

Because this write happens to reuse `model_k_dc`'s own fitted gain rather
than build a second estimator, `coupling_diag_k_dc` and `model_k_dc` will
read identically for any zone re-tuned after this pass -- that is not a
bug, it is the point (both numbers describe the same measurement of the
same run). They can still diverge exactly as before: a hand-edited or
preset-loaded `coupling_diag_k_dc`, or a `model_k_dc` left over from an
older run this pass never re-touched, are both untouched by this change.

**Trigger, for an operator or an automated caller:** identical to
identifying `model_k_dc` today -- `autotune_engine_run()` or
`autotune_engine_run_to_target()` on the zone (from ambient, prestart-gated
exactly as every step test always has been), then
`autotune_engine_accept()` once the fit reaches DONE. No new HTTP endpoint,
UART command, or UI control was added; existing `/api/autotune` accept
already threads through to this.

Host-tested in `test_autotune_engine_prestart.c`: a successful STEP accept
writes `coupling_diag_k_dc` with the exact fitted gain for the zone under
test (not zone 0 -- a hardcoded-index regression would be caught), and two
negative proofs -- skipped when `zones_config_set_model()` itself fails,
and skipped on the RELAY path (which fits no FOPDT model at all).
Mutation-proven: temporarily short-circuiting the new persist call
reproduced real failures (`got 0.0000, want 27.3200 +/-0.0001` and the
call-count/zone-index checks), reverted, suite green again (21/21
executables, this executable's own count included). `build_kilnfw`
confirmed OK after the change.

**Still open:** the values themselves need a hardware run to populate --
blocked while the A/B hardware experiment is in progress. `coupling_diag_k_dc`
stays 0.0 ("not measured") on this board until a fresh accepted step test
runs per zone. `s_coupling_use_measured_diag_k_dc` stays compiled `false`
per this pass's own scope -- flipping it is still a separate, explicit
decision for later, after real numbers land and can be checked against the
matrix the way §3.2's earlier hand-solved seam-sizing was.

**UNCOUPLED 1x1 FALLBACK — CLOSED 2026-09-03. Verdict: switch it, and it has
been switched.** `diagonal_hold`/`diagonal_climb` (`zone_coupling_solve.c`,
~lines 229/394) now compute their diagonal via
`coupling_diagonal_k_dc(zi, z_ff_k_dc, use_measured_diag_k_dc)` — the exact
same guarded-fallback helper the n>1 matrix path already uses for
`G[row][row]` when `row == zi` — instead of dividing by `z_ff_k_dc` directly.

Reasoning, reusing the seam-sizing methodology from 2026-09-02e rather than
inventing a new one:

- **When is the fallback actually taken?** Every time `zi` has zero
  qualifying neighbours (`COUPLING_SOLVE_FALLBACK_NO_NEIGHBORS`), is itself
  unqualified, is out of range, or the matrix solve degrades
  (`_SINGULAR`/`_NONFINITE`). All of these are exactly the boundary cases
  that sit right next to the n>1 matrix path in time — a zone drops in and
  out of the fallback as neighbours qualify/unqualify at tick rate
  (`heat_blocked` churn, "already absorbed" above). It is not a rare,
  isolated code path; it is the OTHER side of the same seam 2026-09-02e
  measured.
- **Do the two constants mean the same thing?** Yes, when `coupling_diag_k_dc`
  is populated: it comes from the SAME rested single-zone excitation runs as
  the off-diagonals sitting beside it in the matrix (2026-09-02's dataset),
  the same argument already used above to prefer it for the matrix diagonal.
  There is no reason that argument stops applying the instant a zone's
  neighbour count drops to zero — `coupling_diag_k_dc` is a per-zone
  property, not a property of the coupled system.
- **Unset/defaulted case.** Handled explicitly, not silently: `zi`'s own
  `coupling_diagonal_k_dc()` call reuses the identical `isfinite() && > 0.0f`
  guard already shipped for the matrix path, backed by the identical
  "getter reports true with an unwritten 0.0f default" board reality
  documented above (2026-09-02f) — an unmeasured zone falls through to
  `z_ff_k_dc` exactly as it always has. No new zero/garbage-gain path was
  introduced; `test_zone_coupling_solve.c` pins this for the fallback
  specifically (tests 15–17: flag-on+measured, flag-on+unmeasured, and
  out-of-range `zi` with the flag on), mutation-proven the same way tests
  1–14 were (reverting the fallback's divisor to `z_ff_k_dc` alone
  reproduces `got 0.7644, want 0.7867` — RED; restoring the fix turns the
  suite green again, 45/45).
- **Does this change behaviour on any currently shipping config?** No.
  `s_coupling_use_measured_diag_k_dc` is still `false` at the one call site
  (`profile_executor_feedforward.c`) — this pass did not flip it — so
  `coupling_diagonal_k_dc()`'s first argument check (`!use_measured`) makes
  every fallback call degrade to `z_ff_k_dc` exactly as before, on every
  board shipping today. The change is a no-op until the SAME rollout gate
  already established for the matrix path is cleared (flag flipped true AND
  `coupling_diag_k_dc` populated for a given zone) — at which point it does
  what it was always supposed to: it CLOSES the seam 2026-09-02e sized,
  rather than leaving the fallback and the matrix path disagreeing about
  `zi`'s own gain at every membership edge. `build_kilnfw` confirmed OK
  after the change.

This directly resolves the "not a drop-in change" caveat above: the seam
that made switching only the coupled-solve diagonal risky is gone, because
both paths now source `zi`'s own diagonal from the identical function call.

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

**TRANSIENT-AWARE METRIC BUILT, 2026-09-03 — does NOT retrodict the
asymmetry.** `coupled_ident.py` gained `score_matrix_transient()` /
`score_matrix_transient_from_http_paths()`: the SAME `u_pred = A⁻¹·(T −
ambient)` algebra `score_matrix()` uses, sampled over
`transient_window_row_indices()` — every ramp window plus each dwell's
entry portion up to that zone's own post-transition overshoot peak,
identical windows to `pid_ab_compare`'s ramp/dwell-entry metrics
(`log_analysis.build_windows`/`ramp_to_dwell_transitions`, reused not
reimplemented, so the two modules cannot silently disagree about what "the
transient" means). Rationale: `score_matrix`'s settled-tail sampling can
never see a matrix over-crediting a neighbour's heat, because by the time a
dwell tail is sampled that heat has already arrived; sampling the ramp and
dwell-entry instead — while the credited heat is still 620–730 s / 135–158 s
out — is exactly where an over-generous off-diagonal should show up as a
duty shortfall. `coupling-report`'s new section 7 reports it beside, not
instead of, section 4's bias score, wired in via the checked-in matched
A/B pair (`p7_oldmatrix_runC.jsonl` run 0 / `p7_newmatrix2_http.jsonl` run
0, same pair as the hardware A/B above).

Scored on that pair, transient-window mean `u_pred − u_actual`:

| | z0 | z1 | z2 |
|---|---|---|---|
| pre-adoption hybrid | +0.051 | +0.034 | −0.041 |
| adopted own-diagonal | −0.104 | −0.149 | −0.112 |
| adopted hybrid (runs today) | −0.135 | −0.147 | −0.061 |

**It does not retrodict.** Hardware: z1 improved (IAE 1.028→0.608), z2 got
worse (0.820→0.899). This metric: bias magnitude grows for the adopted
hybrid on **all three** zones (0.051→0.135, 0.034→0.147, 0.041→0.061) —
correct direction only for z2, and it flags z1 as the *largest* of the
three regressions, the exact zone that measurably improved. Per the honesty
gate this section opened with: a metric that gets one of two zones
backwards, and picks the wrong zone as the bigger loser, is no better than
the settled-tail one for choosing between candidates — sampling the right
*windows* was necessary but not sufficient. The residual problem is that
`u_pred = A⁻¹·(T − ambient)` is still a **static** steady-state inversion;
off equilibrium its bias is dominated by how far the plant is from
steady state (thermal mass, ramp rate, where in the transient a poll
landed) at least as much as by which matrix is driving it, and that
confound doesn't average out just because the windows are now the right
ones. Reproducible: `python -m kilnctrl.coupled_ident coupling-report`,
section 7; `test_score_matrix_transient_ignores_post_peak_rows` and
neighbors in `test_coupled_ident.py` pin the windowing itself (proven able
to fail — see those tests' own docstrings for the captured red).

**What still would settle it:** the doc's own numeric solve two paragraphs
up (`zone_coupling_solve.c`'s actual `G·u=b`, hold-duty-offload
percentages and row-sum growth) remains the more reliable evidence for
*why* z2 got worse — it is dynamics-aware in the sense that it reasons
about which zone's row depends on which lagging off-diagonal, where this
metric is not. A metric that actually retrodicts would need to be dynamic
(propagate each matrix through the identified per-cell tau/dead-time
before comparing to observed duty), not a static inversion moved to a
different sample set — that is future work, not shipped here.

**Would the adopted matrix still have been chosen under this metric?** No
differently than under the old one, and for the same reason: this metric,
like the bias metric it supplements, says every zone got *worse* under
the adopted hybrid — it would not have surfaced z1's real, measured
improvement any more than the settled-tail metric surfaced z2's real
regression. Neither metric, alone, would have produced today's actual
(mixed, z1-up/z2-down) hardware outcome as a prediction. The honest
conclusion is that no offline metric built so far — settled-tail or
transient-window, both static inversions — is sufficient to gate this
class of matrix change; the adoption decision remains, as recorded above,
an owner call made with that caveat attached, not a metric-validated one.

**Re-identification is now one command, not a night of manual steps.**
`kilnctrl.coupling_workflow` (`python -m kilnctrl.coupling_workflow --host
<ip> --preset <name> --out-dir <dir>`) wires `run_queue.py` (the three
single-zone excitation firings, profiles #4/#5/#6, from rested),
`coupled_ident.py` (settle audit, matrix assembly, condition number,
plausibility, the bias metric, and `score_matrix_transient`) and
`config_presets.py` (the emitted preset's schema) into one pass: capture →
audit → **refuse if any driven zone's own dwell failed the tightened
settle criterion, naming which** → assemble both orientations → score →
write a ready-to-apply preset. It does not reimplement any of the three.
Resumable at capture granularity (a capture already ending in a terminal
`profile_exec` state is reused, not re-fired); `--dry-run` drives every
stage over already-captured data with no HTTP/hardware call at all, which
is how its own test suite (`test_coupling_workflow.py`) exercises it. See
`tools/PcTools/src/kilnctrl/coupling_workflow.py`'s module docstring for
the capture-format note (`run_queue.py` writes the `http_capture_log.py`
envelope, not the plain format `coupled_ident.py`'s `*_from_paths` helpers
read) and the multi-run-file handling.

**Dead time / tau re-identification tried 2026-09-03 — ruled out explicitly,
per §3.4's own ranking, without a usable z2 number.** §3.4's "ranked
improvements" list named "a proper per-zone re-identification using z2's
dwell-entry dynamics (dead time/tau), not steady-state gain" as its #1 item,
flagged in advance as "the one lever this pass found doesn't work is worth
ruling out explicitly." That identification is now built:
`coupled_ident.identify_zone_dead_time_tau()` (offline, additive;
`identify_zone_dead_time_tau_from_capture_path()` for a real capture file).

*Method, and why it does not repeat the shelved ramp-rate artifact.* The
plant obeys `dT/dt = (u_ss - T)/tau`, `u_ss = ambient + K[zone]·duty_delayed(L)`
(`load_estimator.py`'s own equation, reused unmodified). For a grid of
candidate dead times `L`, the through-origin OLS regression of the measured
per-sample `dT/dt` against the computed drive term `u_ss - T` gives `tau`
(`1/slope`) and an R² in closed form; the candidate `L` with the best R² is
kept. This is a regression over every sample in a window (hundreds, not
two), against the ODE's own residual driven by the ACTUALLY APPLIED duty —
unlike the closed-loop two-point FOPDT fit shelved elsewhere in this repo
(which fit an exponential rise SHAPE to a controller's ramp-tracking
trajectory, so the fitted "tau" partly measured the commanded ramp rate),
this method never fits a rise shape and does not care whether duty was
tracking a ramp, holding a dwell, or anything else — only what duty WAS.
Verified in simulation first (`test_coupled_ident.py`): given a known-truth
`FOPDTPlant` with `L=50s`/`tau=260s` distinct from every production
constant, the grid search recovers `L` within one 5 s grid step and `tau`
within 2%, R²>0.999 — the regression math is sound before it is ever
pointed at real data.

*Applied to every COMPLETED capture in `logs/coupling/`* (the six-firing A/B
campaign running during this analysis was excluded past its first two
completed entries — `ab_campaign_state.json` marks `old_2` onward
`in_progress`/`pending`, never read here): every profile-7-shaped capture
(`p7_*_http.jsonl`, `floor_run1`, `noise_floor_p7*`, `ab_old_1`, `ab_new_1`,
`coupid6_run1(_part2)`), one fit per zone per RAMP window (the well-
conditioned regime, same choice `load_estimator.py` already documented),
`min_drive_c=5.0`, candidate `L` 0–300 s in 5 s steps. 35 z0 windows, 35 z1,
34 z2 windows total across every capture. Restricting to windows clearing
R²>0.3 (the same bar §3.8 uses to call a fit "usable," not a threshold
invented for this result):

| | z0 | z1 | z2 |
|---|---|---|---|
| windows total | 35 | 35 | 34 |
| windows R²>0.3 | 16 | 18 | 6 |
| fitted L, median (s) | 70.0 | 122.5 | 92.5 |
| fitted L, range (s) | 40–295 | 10–245 | 50–160 |
| board diagonal L (s, §2) | 52.8 | 43.5 | 33.9 |

**Verdict: not usable, on all three zones, not just z2 — ruled out per §3.4's
own framing, and the ranking's #1 item can be crossed off without a positive
finding.** Fewer than half of z0/z1's ramp windows and only 6 of 34 (18%) of
z2's clear even the loose R²>0.3 bar, and even restricted to that subset the
fitted `L` spans a 5–7x range within a single zone (z2: 50–160 s on n=6) —
per this doc's own discipline, a spread that size on n=6 is dominated by
run-to-run noise, not a real per-window signal (the same "max-min at n=6 is
2.53σ" caution §3.8 and elsewhere in this doc already apply). The medians
themselves do not support "z2 is the outlier": z1's median (122.5 s, n=18)
sits further from its own board value (43.5 s) than z2's (92.5 s vs 33.9 s,
n=6) does, and z0's median (70.0 s) also exceeds its board value (52.8 s).
Repeating the fit against the three cleanest single-zone excitation captures
(`cpl_z{0,1,2}_{mcp,thermo}.jsonl`, one zone driven alone, no cross-zone
ambiguity) is, if anything, worse — the 20 s poll cadence over one ~700–800 s
ramp leaves only 3–18 samples surviving `min_drive_c`, and R² is negative on
two of the three zones there. **No firmware change made here, and no z2-
specific dead-time/tau number is reported as reliable** — the honest
conclusion is that this repo's existing captures do not carry enough
per-window signal to re-solve any zone's dynamics this way, z2 included; a
trustworthy answer would need either a purpose-built single-zone step
capture at faster polling, or many more independent rested single-zone
excitation repeats than the one-per-zone this repo has today.

**Six-firing A/B campaign (old matrix `coupling_matrix_pre20260902` vs new
matrix `coupling_matrix_20260831`) — COMPLETED 2026-08-31, 6/6
(`ab_campaign_state.json`). RE-ANALYSED 2026-09-03 under the revised 1.5 C
confound tolerance (`a2fa7ac`). Verdict: two of three pairs now admitted
(n=2, up from n=1); both agree in direction on z0/z1 whole-run IAE
(~0.6 C, new matrix lower, clears the 0.5 C actionable bar); z2
indistinguishable in both. Confidence LOW-MODERATE — real cross-pair
agreement, but still not a confirmed result at n=2.** Full analysis,
per-zone numbers, the noise-floor comparison, and the side-by-side
confound/result table: `logs/coupling/ab_campaign_report.md` (raw tool
output per pair in `logs/coupling/ab_compare_pair{1,2,3}_revised.txt`).
Compared with the existing, unmodified `pid_ab_compare.py compare`/
`noise_floor.json` machinery — no decision-rule or floor-artifact change
made for this re-analysis; only the already-landed `a2fa7ac` tolerance
change (1.0 C -> 1.5 C, derived from the rig's measured passive-cooldown
floor) was applied to the same six captures.

**Original 2026-08-31 finding, SUPERSEDED below, kept for history:** of the
three intended old/new pairs, two (pair 1 and pair 3) were REFUSED outright
by the tool's then-1.0 C start-temperature confound gate — every zone in
both pairs exceeded it (pair 1: 1.66/1.63/1.36 C; pair 3: 1.20/1.20/1.35 C).
Only pair 2 (0.47/0.64/0.70 C) was usable, leaving n=1.

**Under the revised 1.5 C gate, pair 3 (1.20/1.20/1.35 C, all now under
threshold) is admitted; pair 1 (1.66/1.63 C on z0/z1) is still refused.**
The admission was checked, not assumed: pair 3's fitted start-temp
sensitivity for `iae_normalized_whole_c` is -0.0094 to -0.1333 C of
predicted delta per 1 C of drift (OLS, n=6, the checked-in noise-floor
repeat set), which at pair 3's worst zone (z2, 1.35 C) predicts at most
~0.18 C of confound — below the 0.5 C actionable bar and comparable to the
measured noise floors (0.077-0.147 C on this metric). The residual (raw
delta minus predicted-from-confound) is 92-114% of the raw delta on every
DISTINGUISHABLE z0/z1 key in pair 3 — the confound explains essentially
none of the measured difference.

With both pairs admitted, **the same pattern replicates independently**:
new matrix (B) lower whole-run `iae_normalized_whole_c` on z0 and z1 by
0.57-0.70 C in both pairs (clearing both the 4-8x noise floor and the
0.5 C actionable bar), z2 indistinguishable in both. At the (zone, metric,
segment) level, `iae_normalized_c`, `dwell_steady_state_offset_c`,
`dwell_entry_overshoot_peak_c`, and `ramp_mean_error_c` all clear the
>=3-zone `CONSISTENT_PATTERN_MIN_KEYS` bar favoring the new matrix in BOTH
pairs; `dwell_entry_time_to_peak_s` clears it favoring the OLD matrix in
BOTH pairs. No metric flips direction between the two pairs. This is
meaningfully stronger than the original n=1 finding — two independent,
differently-confounded pairs landing on the same metrics and directions is
the kind of cross-pair agreement the >=3-zone rule was built to build
confidence from — but n=2 is still two points, not a distribution; this is
reported as a lead with cross-pair support, not a confirmed finding. Pair 1
remains unexamined; a corrected re-run of it (or a replacement pair) under
`run_queue.py`'s `--pair-consecutive` mode, which now enforces the same
1.5 C gate before a second arm is allowed to start, would take this to the
n=3 the campaign originally targeted. As always, this profile's ~70 C max
target keeps the result silent on cone-range behaviour and on either
matrix's behaviour past the coupled hold solve's ~62 C feasibility edge.

**LIVE-BOARD CHECK, 2026-09-03: the new matrix is what is actually running.**
`GET /api/zones` against the bench board (192.168.1.156, idle, all relays
off) reports per-zone `coupling_c0/c1/c2` — the persisted
`coupling_coeff[affected][stepped]` row, diagonal contractually 0 — of
`z0 [0, 27.32, 21.72]`, `z1 [14.30, 0, 22.15]`, `z2 [8.33, 12.42, 0]`,
matching `coupling_matrix_20260831` exactly on all nine cells and matching
`coupling_matrix_pre20260902` (`z0 [0, 12.06, 6.00]`, `z1 [5.77, 0, 6.77]`,
`z2 [2.41, 4.11, 0]`) on none. No preset write was made — the board already
carries the matrix the A/B above favors, so this check confirms an already-
correct decision rather than changing anything.

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

      **2026-09-02f — the start-temperature confound is real, measured, and
      now accounted for rather than removed.** The "rested" precondition is
      relative to the on-board cold junction, not ambient, so it certifies
      "cooled to wherever the board is now" — three captures claimed to be
      the same repeated configuration started at 27.60/28.64/28.78 °C (1.18 °C
      range against the 1.00 °C confound threshold this module already
      refuses on). The owner's call: keep the firing protocol unchanged and
      make the comparison machinery account for the confound instead.
      `pid_ab_compare.py` gained two additions, both deliberately
      conservative and both purely additive — neither can loosen a REFUSED
      or INDISTINGUISHABLE verdict, which are still computed from the raw
      delta and measured floor only, unchanged:
      - `fit_start_temp_sensitivity` — an OLS regression of
        `iae_normalized_whole_c` (the one metric with a comparatively tight
        floor, see below) on start temperature, fit only across a caller-
        asserted same-config repeat set (typically the noise-floor
        artifact's own `generated_from`), refusing below
        `MIN_N_FOR_SENSITIVITY=3` points. Reported alongside a whole-run IAE
        comparison as raw delta / predicted-from-start-temp / residual — the
        "regress the outcome and report the residual" option, letting a
        reader see how much of a raw delta the known start-temp difference
        already explains. Verified this cannot manufacture significance: run
        against the real three-repeat noise-floor set both with and without
        the fit attached, the verdicts are byte-identical either way
        (`test_sensitivity_does_not_manufacture_effect_between_same_config_runs`).
      - `summarize_metric_floor_reliability` — this rig's own measured floor
        is internally consistent for some metrics and not others. A genuine
        defect fixed along the way: real HTTP captures often have
        `actual_valid=false` on row 0 (poll landed before the first
        thermocouple read), which the firmware wire-format carries as a
        literal `0.0` placeholder — `compute_run_metrics` used to take
        `rows[0]` unconditionally, so an affected run's start temp silently
        read as `0.0 °C`, defeating the confound gate on exactly the real
        captures this section is about. Fixed to use the first genuinely
        valid row per zone. (See the 2026-09-03 note below for the current,
        6-run version of this metric's ratio table — the numbers originally
        written here were from the 3-run era and are superseded.)

      **2026-09-03 — adversarial statistical review, re-measured against the
      checked-in 6-run repeat campaign
      (`logs/coupling/noise_floor_p7*_run*.jsonl`).** Every number in this
      subsection above predates that campaign; below is what actually holds
      today, after a second, unrelated fix landed the same day (see next
      paragraph) also changed one of these numbers a second time.
      - **Start temperatures across the 6-run set: 27.60–28.88 °C (1.29 °C
        range)** — still over the 1.00 °C confound threshold, same
        conclusion as the 3-run figure above, just re-measured.
      - **Metric floor reliability ratios (max/min noise floor, across
        zones/segments, per metric), current artifact:**
        | metric | ratio | cluster |
        |---|---|---|
        | `iae_normalized_whole_c` | 1.91x | reliable |
        | `ramp_worst_error_c` | 2.57x | reliable |
        | `iae_normalized_c` | 4.30x | reliable |
        | `dwell_entry_time_to_peak_s` | 4.73x | reliable |
        | `ramp_mean_error_c` | 5.49x | reliable |
        | `dwell_entry_overshoot_peak_c` | 6.55x | UNSTABLE |
        | `dwell_steady_state_offset_c` | 13.98x | UNSTABLE |
        | `settle_time_s` | 15.27x | UNSTABLE |

        `ramp_worst_error_c` was previously reported (3-run era, then again
        on the first 6-run artifact) as ~118x / "cannot resolve anything
        useful" — that was itself an artifact of a since-fixed defect: the
        HTTP-capture parser in `log_analysis.py` read `actual_c` while
        ignoring `actual_valid`, so the firmware's literal `0.0` "no reading
        yet" placeholder leaked into segment-0 windows as a spurious ~27 °C
        outlier on 5 of the 6 captures (target was already near 28 °C).
        Fixed 2026-09-03; the artifact was regenerated from the same 6
        captures. `ramp_worst_error_c` is now one of the TIGHTEST metrics.
        `tools/PcTools/config_presets/tuning_recommendations.json` and the
        `zones_page.html` reliability panel were written from the pre-fix
        floors and still claim `ramp_worst_error_c` "cannot resolve
        anything useful" — that is now false and those need regenerating
        (tracked separately; not `pid_ab_compare.py`'s files).
        `FLOOR_RELIABILITY_RATIO` stays at 6.0 (was 5.0 in the 3-run era) —
        the gap between the reliable and unstable clusters above (5.49x to
        6.55x) is unchanged by the parser fix, since only segment-0 windows
        were affected.
      - **The 1.0 °C confound threshold's justification, replaced.** It used
        to be argued from a single 4.8 °C confounded pair, which bounds
        nothing. The real argument: `fit_start_temp_sensitivity` against the
        6-run set gives per-zone sensitivities of roughly 0.009–0.133 °C
        per °C of start-temp delta; at the 1.0 °C threshold that predicts a
        0.009–0.133 °C shift in `iae_normalized_whole_c`, against that same
        metric's measured whole-run floors of 0.077–0.147 °C — the top of
        the predicted range falls inside the measured floor range. 1.0 °C
        is roughly where the confound's own predicted contribution reaches
        the noise floor. The threshold value is UNCHANGED, only its
        justification.
      - **Multiplicity.** A `compare` evaluates 48 `(zone, metric, segment)`
        keys with no correction. The range-floor statistic's own per-key
        false-positive rate is not 5% — it is roughly 12% at n=6 (Monte
        Carlo; `pid_ab_compare.summarize_multiplicity`), so a single
        DISTINGUISHABLE key in a report this size is close to the expected
        outcome under pure chance, not a finding on its own. `compare`'s
        text/JSON output now reports the family size, the per-key rate, the
        probability at least one spurious DISTINGUISHABLE verdict appears
        (both assuming independence, which overstates the risk, and at the
        review's estimated effective family size of 10–15 correlated
        tests), and flags the one pattern treated as potentially
        actionable: the same metric DISTINGUISHABLE in the same direction
        across several zones/segments.
      - **The range floor is not scale-stable across n.** `E[range]/sigma`
        grows with n (1.69 at n=3, 2.53 at n=6, 3.07 at n=10) — and the
        checked-in artifact already mixes n (`z0:settle_time_s:1` has n=3,
        every other key has n=6). Every comparison now carries `floor_n`,
        and `prediction_interval_floor` computes the scale-stable
        alternative — `t(.975, n−1) * std_c * sqrt(2)`, the honest "could
        two new same-config runs differ by this much?" question — reported
        alongside the range floor as `pi_floor_c` / `pi_distinguishable`.
        This is wider than the range floor by construction, so switching to
        it as the verdict floor would lose some currently-DISTINGUISHABLE
        findings; that is the statistically correct direction but it stays
        the owner's call — `compare`'s actual verdict still uses the range
        floor, unchanged.
      - **Start-temperature unit mismatch.** `noise_floor.py`'s
        `extract_start_conditions` (used by its own like-for-like check,
        `LIKE_FOR_LIKE_THRESHOLD_C = 1.0`) computes the MEAN across ALL
        status channels; `pid_ab_compare.py`'s `_first_valid_start_temp`
        (used by `CONFOUND_THRESHOLD_C = 1.0`) computes the PER-ZONE
        first-valid `actual_c`. Both constants are 1.0 °C but gate two
        different quantities — `pid_ab_compare.py` now prints an explicit
        note (`START_TEMP_METRIC_NOTE`) on every `compare` so the two are
        not conflated; unifying the two modules' start-temperature
        extraction is out of scope for this pass.

      **2026-09 — the loop closed: `ITER_TUNE_MIN_RELATIVE_IMPROVEMENT`
      checked against the now-measured floor.** The blocking unknown named
      at the top of this bullet is gone — `noise_floor.json` (schema 2,
      6-run campaign, above) gives `iae_normalized_whole_c`'s per-zone
      floor: z0 mean 1.6043/std 0.0527, z1 mean 1.1904/std 0.0318, z2 mean
      0.8765/std 0.0541. `iter_tune.c` compares exactly one trial firing
      against exactly one baseline firing, so the right absolute figure is
      each zone's two-sample PREDICTION interval (`t(.975,5)*std*sqrt(2)`,
      not the n=6 range, which is 2.53σ, and not raw σ, which is a 1-sample
      figure) — z0 0.1915, z1 0.1156, z2 0.1966 °C. Mapping 20 % relative to
      absolute terms at this bench's own measured typical magnitudes: z0
      0.20×1.6043=0.3209 °C (1.68x the floor, safe), z1 0.20×1.1904=0.2381 °C
      (2.06x, safe), z2 0.20×0.8765=0.1753 °C (**0.89x — already below its
      own floor, today, at today's typical baseline**). A purely relative
      bound is the wrong shape here: its absolute requirement shrinks as
      the mechanism succeeds at lowering the score, while the floor
      (sensor/tick-jitter/ambient-drift noise) does not shrink with it — z2
      is not a distant edge case, it is the demonstrated case. Fix landed
      in `iter_tune.h`/`iter_tune.c`: kept `ITER_TUNE_MIN_RELATIVE_
      IMPROVEMENT` at 20 % (still correctly conservative for z0/z1) and
      added `ITER_TUNE_MIN_ABSOLUTE_IMPROVEMENT_C = 0.20` (the worst-case,
      i.e. largest, of the three zones' prediction intervals, z2's 0.1966,
      rounded up for headroom), applied as `required = max(relative_
      required, absolute_floor)`. A single global absolute constant was
      used rather than three per-zone ones — the three floors (0.12/0.19/
      0.20) are within 2x of each other and sizing to the worst zone only
      makes the other two somewhat more conservative than their individual
      floor strictly requires, which is the safe direction to err in; three
      near-identical per-zone constants were judged not worth the added
      state/config/test surface for a difference this small. Host tests
      (`test_iter_tune.c`) updated: the two existing relative-only tests
      rescaled to realistic (~1.0) baseline magnitudes so they still
      isolate relative behavior; two new tests pin the absolute-floor-
      governs case (a 30 % relative "improvement" at a low baseline still
      reverts) and the relative-still-governs case (absolute floor alone
      does not loosen a large-baseline requirement); the real-capture
      regression test's mirror-direction assertion (previously ACCEPTED at
      47.5 % relative on ~0.02-magnitude captures) now correctly REVERTS,
      since that capture's absolute swing (0.0076) is an order of magnitude
      below the measured floor. All four negative-test mutations confirmed
      by hand (drop the `max()` term back to relative-only) before landing.
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

**Per-path dead time, tried 2026-09-03 — makes held-out RMS WORSE, not
better.** Two findings the same night named the single per-zone dead time
above as the same missing piece (this section's own held-out figure and
§3.8's load-estimator negative). `plant_sim.FOPDTPlantPerPath` (new,
additive — `FOPDTPlant` is untouched) gives each coupling PATH its own dead
time/tau instead of one per receiving zone: diagonal MEASURED (`L`/`tau`
above, unchanged), off-diagonal ASSUMED at the range midpoint (146.5 s /
675.0 s — §2 gives only an aggregate 135–158 s / 620–730 s range, not a
per-pair breakdown; `plant_sim.L_PAIR`/`TAU_PAIR`). Re-run of the exact
pooled held-out comparison above, same two captures, same segments, only
`plant_regime` changed:

| | z0 | z1 | z2 |
|---|---|---|---|
| single delay per zone (current) | 1.05 °C | 0.61 °C | 0.67 °C |
| per-path (new) | **1.63 °C** | **1.09 °C** | **0.95 °C** |

**Every zone gets worse**, by more than the 2.1/1.2/1.3 °C discrimination
threshold's own margin — this is not noise. Reading of why: the controller's
feedforward still uses one fixed, non-delayed `K_inv`/`tau` (exactly like
real firmware — see `coupled_ff_hold_climb`'s own note), so slowing down the
PLANT's cross-zone arrival without also modeling the controller's blindness
to that lag widens the mismatch between what the controller assumes and
what the plant does, rather than narrowing it. The single-delay model's
better RMS looks like it was, in part, accidentally compensating for that
mismatch by averaging cross-zone arrival time toward the (shorter) diagonal
value. **No new discrimination threshold is reported — a model that fits
worse does not get a tighter one.** The mechanism (§2's measured lag is
real) is not in question; this specific way of injecting it into the sim
is a regression against the tracking-RMS metric, and is NOT adopted as the
default (`plant_regime='measured_per_path'` stays opt-in, `'measured'`
unchanged and still what every existing caller uses).

**Zone-2 A/B reproduction, checked with per-path delays in place: still
does not reproduce the hardware direction.** §3.2's 2026-09-03 hardware A/B
found z1 tracking improved and z2 worsened switching old→new matrix. Solved
`coupled_ff_hold_climb` with the controller believing the old vs. new
matrix (plant fixed to the real, new-matrix identification) over the
`p7_newmatrix_http.jsonl` segment shape, whole-run mean |error| vs.
commanded target:

| plant model | z0 | z1 | z2 |
|---|---|---|---|
| single delay (current) | worse | **better** | **better** |
| per-path (new) | worse | worse (flat) | **better** |

Neither model reproduces z2 getting *worse* — both predict it improves.
Per-path delays do not flip that sign; they instead turn z1's previously
correct-direction "better" into "worse (flat)", which is a step away from
the hardware result, not toward it. **This sim, single-delay or per-path,
still cannot reproduce the one real hardware A/B result it would need to
in order to be trusted for this kind of comparison** — unchanged verdict
from the rest of this section, now checked against the specific mechanism
this task named.

**Lag-compensated feedforward, tried 2026-09-03 — worse on every zone, and
still does not retrodict the hardware A/B.** The per-path plant finding
above named the mechanism but slowed only the PLANT; the CONTROLLER still
credits a neighbour's heat instantaneously either way. This candidate
(`plant_sim.LagCompensatedFF`, opt-in via `climb_mode='lag_compensated'`,
SIMULATION ONLY) instead changes the CONTROLLER: each zone solves its own
hold duty directly off its own diagonal gain, crediting a neighbour's
contribution from the DELAYED duty it actually commanded `L_PAIR[i,j]`
seconds ago (0 before any duty has ever been recorded) instead of its
current value. Climb stays diagonal-only, unchanged from
`uncoupled_ff_hold_climb`'s own formula — sec 2's lag evidence is a
settled-dwell/hold phenomenon, and extending the same treatment to climb
would be a second, unmeasured guess stacked on this one.

Pooled held-out RMS, same methodology and same two rested captures as the
1.05/0.61/0.67 °C `'coupled'` figure above (run with the plant AND the
controller's belief both set to the same matrix):

| | z0 | z1 | z2 |
|---|---|---|---|
| `'coupled'` (current), new matrix | 1.05 | 0.61 | 0.67 |
| `'coupled'` (current), old matrix | 1.06 | 0.62 | 0.63 |
| `'lag_compensated'`, new matrix | **1.96** | **1.81** | **2.25** |
| `'lag_compensated'`, old matrix | **2.05** | **1.90** | **2.23** |

**Every zone is worse by roughly 2×**, well past the sec 3.4 discrimination
thresholds (2.1/1.2/1.3 °C) on z1/z2 and close to it on z0 — not noise, and
**z2's penalty under the new matrix is not removed; it moves from a small
`'coupled'` disadvantage to the largest absolute RMS of the three zones
under this candidate.** Reading of why: crediting only PAST duty means a
neighbour contributes zero credit for the first `L_PAIR[i,j]` (~146.5 s
off-diagonal) of every ramp and every dwell-entry — exactly the transient
windows that dominate whole-run IAE/RMS — so this formulation is not just
late relative to `'coupled'`, it is *under*-crediting through most of the
transient in a way `'coupled'`'s instantaneous, over-generous credit never
is. The idea that the controller's instantaneous-credit assumption is
wrong is not in question; this specific way of fixing it trades one bias
for a larger one.

Re-ran the same old-vs-new-matrix-belief experiment sec 3.2/3.4 used above
(plant fixed to the real new-matrix identification, controller told to
believe old vs. new, scored as whole-run mean |error| vs. commanded target
over `p7_newmatrix_http.jsonl`):

| plant model | z0 | z1 | z2 |
|---|---|---|---|
| single delay (current, `'coupled'`) | worse | **better** | **better** |
| `'lag_compensated'` | better | **better** | **better** |

Hardware: z1 better, z2 **worse**. `'lag_compensated'` predicts all three
zones improve moving old→new — it does not flip z2's sign either, and it
now disagrees with `'coupled'` on z0's direction too, without adding a
zone where the sign is right. **Third honest negative on retrodicting this
specific A/B**: single-delay plant, per-path plant, and now lag-compensated
feedforward have each been tried and none reproduces the one asymmetric
result (z1 up, z2 down) that would validate any of them for this
comparison. Something structural — not simply "which one lags and by how
much" — is still missing; per the owner's own honesty gate, this is worth
stating plainly rather than trying a fourth variant on the same axis
without a new idea about what that missing piece is.

Mutation-proven (`tests/test_plant_sim.py`): `_delayed_duty` returning a
phantom credit before any history exists, cross-zone terms added to climb,
and the delay lookup reading the undelayed current tick instead of the
`L_pair`-indexed one, each independently reproduced a failing assertion;
all reverted. The pooled-RMS finding above is itself pinned by
`test_lag_compensated_held_out_rms_is_a_regression_not_an_improvement`,
proven able to fail by reverting the delay lookup to undelayed (collapses
z0 RMS from ~2.0 °C back to ~1.1 °C, caught by the bound).

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

**Per-zone PID gains, tried 2026-09-03 — ranked improvement #3, now
tested. Verdict: NEGATIVE.** Item (3) above ("per-zone rather than shared
PID gains, untested this pass") is now built and run. Every sweep in this
module before this point — the fuzzy sweep, the load sweep, the tuning
campaign — drove all three zones with the same `kp`/`ki`/`kd`. `run_profile`
gained per-zone support (`kp`/`ki`/`kd` each accept a scalar, unchanged
byte-for-byte behaviour, or a length-3 sequence — `_broadcast_zone_param`),
plus `per_zone_gain_grid_search()`/`per_zone_gain_holdout_report()`: for
each zone independently, grid-search `kp`/`ki` multipliers (0.25–4.0×
the shared baseline 0.06/0.0003, `kd` left at 0) that minimize that zone's
own whole-run `iae_normalized`-equivalent metric on one capture, holding
the other two zones at baseline (the same diagonal-only simplification
§3.3's real adaptive-tuning refinement already makes on hardware), then
SCORE both the shared baseline and the per-zone gains on a *different,
held-out* capture — scoring on the same capture the gains were fit from
would trivially favor more free parameters on the same data and prove
nothing about generalization.

*Simulator fidelity checked before trusting this result* (the specific
failure mode this doc's own history warns about — a hardcoded ramp rate
once inverted a conclusion here): `segs_from_capture` derives ramp rate
from each capture's own segment boundaries, not a constant — confirmed
0.0333 °C/s (`p7_oldmatrix_http.jsonl`) and 0.0324 °C/s
(`p7_newmatrix_http.jsonl`), both inside the documented 2–3.5 °C/min
(0.033–0.058 °C/s) real range, nowhere near the old 10 °C/min bug. `K_full`/
`tau`/`L` are unchanged module constants (§3.2's matrix, the cooldown
`tau`, the bench-rig `L` — same numbers this section's own held-out RMS
figure above was computed from). Both captures split via the same
`log_analysis.split_runs`/run-0 convention already used for the 1.05/0.61/
0.67 °C held-out figure and the §3.2 hardware A/B, and both are rested-start,
complete, single profile-7 firings (`p7_oldmatrix_http.jsonl` run 0,
`p7_newmatrix_http.jsonl` run 0) — the only two complete rested full firings
checked into `logs/coupling/` at the time of this pass (`p7_newmatrix2_http
.jsonl` is a third, still `in_progress`, excluded). Fit/test roles were run
BOTH ways (fit on old→test on new, and fit on new→test on old) specifically
so a one-direction result could not be mistaken for a finding — this is the
same discipline §3.2's own withdrawn coupling-matrix claims were burned by
skipping.

Fit-then-held-out result (grid 0.25×–4.0×, `climb_mode='coupled'`,
`integral_floor='ff_hold'`), whole-run normalized-IAE delta = per-zone minus
shared baseline on the HELD-OUT capture, negative = per-zone better:

| | z0 | z1 | z2 |
|---|---|---|---|
| fit OLD → test NEW | −0.160 | −0.069 | +0.019 |
| fit NEW → test OLD | −0.185 | −0.087 | +0.006 |
| noise floor (§3.8, whole-run `iae_normalized_c`) | 0.116 | 0.077 | 0.147 |

z0 and z2's deltas exceed their own noise floors in both directions
(consistent sign); z1's is close to its floor (−0.069/−0.087 vs 0.077) —
real but marginal. On its own this table reads like a 2-of-3 win, exactly
the pattern this task was warned to distrust (a prior 1-of-3 simulated
"win" here was later measured WORSE on every zone, on hardware, and tripped
a thermal guard). Checking further is what turns it from a false positive
into a correct negative:

**Both z0 and z1 independently converged on the SAME grid corner in BOTH
fit directions** (`kp_mult=2.00, ki_mult=0.25`, i.e. `kp=0.12, ki=0.000075`)
— not two different per-zone optima that happen to both beat baseline, but
one shared retune that helps both zones. Re-running that single shared
`(kp=0.12, ki=0.000075)` gain UNIFORMLY across all three zones (no
per-zone specialization at all) on both held-out captures:

| | z0 | z1 | z2 |
|---|---|---|---|
| fit OLD → test NEW | −0.150 | −0.067 | +0.010 |
| fit NEW → test OLD | −0.177 | −0.086 | −0.010 |

Indistinguishable from the "per-zone" table above, including z2, whose own
separately-searched optimum (`kp_mult=3.00`, a genuinely different corner
from z0/z1's) does *not* outperform simply reusing z0/z1's shared retune —
z2 gains nothing measurable over its own §3.8 noise floor (0.147 °C) under
either treatment, and its sign is not even consistent between the two fit
directions (+0.019/+0.006 "per-zone", +0.010/−0.010 "shared retune").

**Conclusion: the win here is a plain shared-gain retune, not a per-zone
effect.** Two of three zones (z0, z1) want the identical new gain in every
condition tested; the one zone that searched somewhere else (z2) gained
nothing distinguishable from noise for the trouble. Per-zone gains were
tested honestly — fit on one real capture, validated on a different one,
in both directions, checked against the measured noise floor — and they do
not beat a single retuned shared gain by any margin this simulator can
resolve. Per-zone gains would cost three times the tuning surface, three
times the commissioning burden on a real kiln, and three times the chance
of one zone shipping mistuned; this data does not justify that cost.
**Recommendation, not a change to land:** if the shared-gain retune itself
(`kp≈0.12, ki≈0.000075` against the current `0.06/0.0003`) is ever tried,
it should go through a real hardware A/B like §3.2's, not ship from this
simulator result alone — mirroring this section's own repeated finding
that this simulator's mechanism comparisons are sound but its magnitudes
are not calibration-grade, and per its own TRUST section, absolute values
below the ~1 °C bench-identification noise floor are not to be trusted.
No firmware change made here. Reproducible via
`plant_sim.per_zone_gain_holdout_report()`/
`per_zone_gain_grid_search()`, pinned by
`tests/test_plant_sim.py`'s per-zone-gain test group.

**2026-09-03 addendum: the section above's baseline was never what the
board runs — fidelity audit and corrected re-run.** `control_get_zones`
read against the live (idle, ambient) board on 2026-09-03 showed
`kp=0.0318/0.0485/0.0631`, `ki=0.00010/0.00020/0.00020`,
`kd=0.8401/1.0548/1.0690` per zone — while every gain default in
`plant_sim.py` (`run_profile`, `per_zone_gain_grid_search`,
`per_zone_gain_holdout_report`, `render_sim_report`,
`render_sim_vs_capture_report`) was a single shared scalar,
`kp=0.06, ki=0.0003, kd=0.0`, carried forward from before per-zone gains
existed (`_broadcast_zone_param`, this section, above). Two mismatches are
cosmetic (kp/ki each off by roughly 2–3×) and one is severe: **`kd=0.0`
means every retune candidate this section produced, including the "shared
retune" recommendation above, was searched from a controller running NO
derivative action at all**, against a board that runs `kd` an order of
magnitude larger than `kp`.

Full audit against `pid.c`/`profile_executor_run.c`, mechanism by
mechanism — everything below is confirmed MATCHING except the default gain
just described:

  - **Derivative form**: on measurement, not error (`PID.update`'s
    `raw_d = -(measurement - prev_measurement) / dt_s`) — matches
    `pid_update_terms()`'s comment and code exactly, including the reason
    (a profile's ramp steps the setpoint every tick; derivative-on-error
    would spike on every step).
  - **Derivative filter**: low-pass, `d_tau=30.0` s hardcoded in
    `run_profile`'s `PID(...)` construction — matches
    `PID_D_FILTER_TAU_S` (`profile_executor_internal.h`), also 30.0 s. Not
    a divergence; was already right.
  - **Setpoint weighting**: `b=1.0` hardcoded — matches
    `PID_SETPOINT_WEIGHT_B`, also 1.0.
  - **Integral windup**: conditional integration (freeze when the
    unclamped output is already saturated *and* integrating would push it
    further) plus a hard `ki*integral` clamp — `PID.update`'s
    `would_push_further`/floor-clamp block is a line-for-line match to
    `pid_update_terms()`'s `would_push_further_out` block.
  - **Integral floor**: floors at `-ff_hold` (the steady-state hold
    feedforward only, never the climb component or the whole
    feedforward) — `integral_floor='ff_hold'` is `run_profile`'s own
    default and matches the shipped firmware fix this section's earlier
    `ifix`/`holdfix_clean` captures already validate.
  - **PWM window**: NOT modeled, and this is a reasoned omission, not an
    oversight — the module docstring states it explicitly ("PWM window
    quantization (`heater_output.c`) — averages out under 10 s sampling
    and is not implicated by any of the five captures' error shape").
  - **Feedforward hold/climb**: `coupled_ff_hold_climb`'s joint
    `K_full^-1` solve matches "the real coupled Gaussian solve, same math
    the firmware ships" per this section's own earlier text; unaffected by
    this addendum.

So the mechanism was already correct — only the *default gain fed into
it* was stale, and it happened to zero out an entire term.

**Fix**: `plant_sim.py` gained `BOARD_ZONE_KP`/`BOARD_ZONE_KI`/
`BOARD_ZONE_KD` (the per-zone values above) and
`per_zone_gain_grid_search()`/`per_zone_gain_holdout_report()` now default
to them instead of the old scalar (`run_profile`'s own default is left at
the old scalar deliberately — it is what the `after`/`ifix`/
`holdfix_clean`/`final` fixture captures this section's regression tests
pin against actually ran at capture time; changing it would break fidelity
to *those* historical builds, not improve it). Negative-tested:
temporarily reverted the two functions' defaults back to the scalar,
which produced `assert (0.06, 0.06, 0.06) == approx((0.0318, 0.0485,
0.0631))` in the new pinning test
(`test_per_zone_gain_search_default_baseline_matches_live_board`);
reverted back, suite green (45 passed).

**Re-run gain search from the corrected baseline** (same method as above:
fit on `p7_oldmatrix_http.jsonl` run 0, score on `p7_newmatrix_http.jsonl`
run 0, both directions, `climb_mode='coupled'`, `integral_floor='ff_hold'`,
`PER_ZONE_GAIN_GRID_MULT` 0.5×–2.0×):

| | z0 | z1 | z2 |
|---|---|---|---|
| fit OLD → test NEW | −0.287 (mult 2.00×/0.50×) | −0.242 (mult 2.00×/2.00×) | −0.148 (mult 2.00×/2.00×) |
| fit NEW → test OLD | −0.302 (mult 2.00×/2.00×) | −0.269 (mult 2.00×/2.00×) | −0.156 (mult 2.00×/2.00×) |
| noise floor (§3.8) | 0.116 | 0.077 | 0.147 |

Unlike the pre-fix table above, **all three zones now exceed their own
noise floor in both directions with consistent sign** — including z2,
whose sign previously flipped between directions (+0.019/+0.006) and now
does not. This looks like real signal, but it does not survive the same
"check further" discipline this section applied to the earlier apparent
2-of-3 win, for a new reason: **every zone's `kp_mult` pins at the grid's
own edge (2.00×), not an interior optimum.** Re-running with a much wider
grid (0.25×–8.0×) to see where it settles:

| | z0 | z1 | z2 |
|---|---|---|---|
| fit OLD → test NEW | mult 6.00×/0.25×, Δ=−0.157 | mult 4.00×/4.00×, Δ=−0.130 | mult 3.00×/6.00×, Δ=**+0.058** (shared wins) |
| fit NEW → test OLD | mult 6.00×/0.25×, Δ=−0.167 | mult 4.00×/4.00×, Δ=−0.144 | mult 3.00×/6.00×, Δ=**+0.046** (shared wins) |

z0 and z1's chosen multiplier keeps climbing as the grid widens (2.00× →
6.00×/4.00×) instead of converging to an interior point, and z2's sign
flips from "per-zone better" at the narrow grid to "shared/baseline
better" at the wide one. An optimum that keeps moving to whatever the
grid's edge currently is is not a real optimum — it is the search finding
that this simulator, run with no measurement noise/quantization model
active (`measurement_noise_std_c=0.0`, `measurement_quantum_c=0.0`, both
default-off per `run_profile`'s own docstring) and now driven by a large
nonzero `kd`, never penalizes an ever-larger `kp`/`kd` combination with
anything that looks like noise-amplified ringing, because there is no
noise to amplify. This is the module's own documented TRUST-section limit
("not calibration-grade... below the noise floor... a single 0–80 °C
bench-rig dataset") showing up as a genuine failure mode once a nonzero
`kd` is in play, not a new defect in this pass's fix.

**Verdict: no retune candidate survives from the corrected baseline.**
The pre-fix section's "no per-zone win, shared retune only, needs a
hardware A/B" conclusion is superseded — it was reached from a controller
model that never ran with derivative action live, so it cannot be
transferred to the real board either. The corrected-baseline search does
not converge to a specific, trustworthy `(kp, ki)` recommendation at any
grid width tried: the answer keeps changing with the search range, which
is disqualifying by this section's own repeated standard, not
encouraging. **Do not run 6135ee1's `kp≈0.12, ki≈0.000075` hardware A/B
recommendation** — it was derived against a `kd=0.0` model that is not the
board.

**Given the severity of the `kd` gap, five other findings in this section
should be treated as UNCONFIRMED against the real board's control law**,
not wrong, but never checked against a model with derivative action live:
the dwell-entry overshoot reproduction (peaks 0.95–1.8 °C vs hardware's
~2 °C — a live `kd` term changes both the model's peak time and magnitude
directly), the shared-retune "2-of-3 win dissolves to a single shared
gain" finding above (fit with `kd=0.0` throughout), the fuzzy-gain sweep
(§3.9, whose `pid_fuzzy_adjust` reads `error_rate_c_per_s` from
`d_filtered`, which was always being fed by a real filter but scaled by a
`kd` term that was zero at every gain the fuzzy layer scaled), and the
lag-compensated feedforward candidate comparison (climb-side only,
independent of `kd`, but run through the same `kd=0.0` PID loop). None of
these are known to be wrong — but none were re-checked with this fix
before this addendum was written, and closing that gap is follow-up work,
not done here. Before trusting any of them for a hardware decision,
re-run with `kd=BOARD_ZONE_KD` and confirm the conclusion holds.

**2026-09-03e — the four flagged findings above, re-checked against
`kd=BOARD_ZONE_KD` (read-only re-run, `plant_sim.py`/`test_plant_sim.py`
untouched; no firmware change; no gain search run without the module's
still-missing noise/quantization model active by default).**

1. **Dwell-entry overshoot reproduction — CONFIRMED, gap if anything
   widened.** Fixed-gain structural comparison (no search over `kp`/`kd`),
   usable now. Re-ran `run_profile` over all five §3.4 fixture captures
   (`baseline`/`after`/`ifix`/`holdfix_clean`/`final.jsonl`) with per-zone
   `kp=BOARD_ZONE_KP, ki=BOARD_ZONE_KI, kd=BOARD_ZONE_KD` in place of the
   old `kp=0.06, ki=0.0003, kd=0.0` scalar, `climb_mode='coupled'`,
   `integral_floor='ff_hold'`, and measured the ramp→dwell transition's
   peak sim-vs-target error in each capture's dwell-entry window. Peaks
   move from 0.95–1.8 °C @ 42–70 s (old `kd=0.0` scalar) to **0.82–1.42 °C
   @ 40–85 s** (corrected per-zone gains) against hardware's documented
   ~2 °C @ 65–145 s — the sim now underestimates the overshoot's
   *magnitude* by more, not less, though its *timing* shifts later, closer
   to (still short of) hardware's window. The qualitative verdict — sim
   reproduces the right direction but undershoots magnitude and peaks too
   early — holds under the corrected model; it was not an artifact of the
   missing derivative term. **This does not touch the dwell-entry-overshoot
   FIX itself (`zone_taper_climb_rate`, `e373c39`), which was validated on
   hardware and stands regardless.** What it does affect is confidence in
   using this simulator to size `PROFILE_EXECUTOR_EASE_OFF_WINDOW_MULT`
   (currently 2.0×): the sim's persistent magnitude/timing gap against
   hardware means it should not be trusted to pick that multiplier's third
   decimal place — a **recommendation**, not a change to land, would be a
   hardware A/B of the window multiplier itself, the same discipline this
   section applies everywhere else.

2. **Shared-retune "2-of-3 win dissolves" finding — BLOCKED, needs the
   noise model.** This conclusion depends on a `kp`/`ki` grid SEARCH, the
   exact category this task's own known limitation applies to. It has
   already been re-run once against the corrected `BOARD_ZONE_KD` baseline,
   two sections above this one (2026-09-03d): with the original 0.5×–2.0×
   grid every zone now exceeds its own noise floor with consistent sign
   (looks like signal), but every zone's optimum pins at the grid's own
   edge, and widening the grid to 0.25×–8.0× makes z0/z1's chosen
   multiplier keep climbing (2.00× → 6.00×/4.00×) instead of converging,
   while z2's sign flips between "per-zone better" and "shared/baseline
   better." That is the documented signature of a search with nothing to
   penalize an ever-larger gain — i.e. exactly the missing
   measurement-noise/quantization model, not a re-discovery of the original
   6135ee1 mechanism. **No verdict on "does the win dissolve into a shared
   retune" can be produced right now**, for either the old or the new
   claim: the search itself does not converge to an answer under either
   gain baseline without noise/quantization active to stop it running to
   the grid's edge. Re-run once the noise model lands, with a fixed grid
   width decided in advance (per this section's own "no post-hoc grid
   picking" discipline) so this cannot be re-litigated by choosing a grid
   that happens to look interior.

3. **Fuzzy-gain sweep (§3.6) — CONFIRMED.** This is a fixed-gain sweep
   over `fuzzy_strength_pct` (0/25/50/75/100), not a search over `kp`/`kd`,
   so it is usable now. The original sweep used the old code's single
   scalar broadcast (`kp=0.0318, ki=0.0001, kd=0.8401` — zone 0's own board
   gains applied to all three zones, since per-zone `kp`/`ki`/`kd` support
   did not exist yet when this sweep was written) rather than each zone's
   real gains. Re-ran the identical sweep (`final.jsonl`'s real segment
   shape, `climb_mode='coupled'`, `integral_floor='ff_hold'`, rested 24 °C
   and warm 34 °C starts) with `kp=BOARD_ZONE_KP, ki=BOARD_ZONE_KI,
   kd=BOARD_ZONE_KD` per zone instead:

   | start | z | 0 | 25 | 50 | 75 | 100 |
   |---|---|---|---|---|---|---|
   | 24 C | z0 | 1.273 | 1.349 | 1.431 | 1.517 | 1.617 |
   | 24 C | z1 | 1.150 | 1.217 | 1.290 | 1.367 | 1.461 |
   | 24 C | z2 | 0.824 | 0.867 | 0.911 | 0.975 | 1.055 |
   | 34 C | z0 | 0.902 | 0.937 | 0.977 | 1.024 | 1.084 |
   | 34 C | z1 | 0.772 | 0.803 | 0.840 | 0.884 | 0.940 |
   | 34 C | z2 | 0.590 | 0.613 | 0.642 | 0.678 | 0.724 |

   Ranking is still monotonic — `strength=0` scores best at every zone,
   both starts, no crossover — matching the original sweep exactly in
   shape. Largest spread across all five strengths is 0.344 °C (z0,
   rested), still well under the 2.1/1.2/1.3 °C per-zone discrimination
   thresholds, so the "cannot call a winner" honesty-gate verdict also
   holds unchanged. The corrected per-zone gains (with real `kd`) do not
   change this conclusion in either direction or magnitude enough to
   matter.

4. **Lag-compensated feedforward candidate comparison — CONFIRMED,
   direction and rough scale, with one softened margin.** Structural
   comparison at fixed gains (climb-side mechanism only, independent of
   `kd`), usable now. Re-ran the pinned pooled held-out RMS comparison
   (`test_lag_compensated_held_out_rms_is_a_regression_not_an_improvement`'s
   own method: both rested full firings, `p7_oldmatrix_http.jsonl` +
   `p7_newmatrix_http.jsonl`) with per-zone `kp=BOARD_ZONE_KP,
   ki=BOARD_ZONE_KI, kd=BOARD_ZONE_KD` instead of the test's default
   `kp=0.06, ki=0.0003, kd=0.0` scalar:

   | | z0 | z1 | z2 |
   |---|---|---|---|
   | `'coupled'` (current), corrected gains | 0.958 | 0.578 | 0.680 |
   | `'lag_compensated'`, corrected gains | 1.639 | 1.222 | 1.463 |
   | ratio (lag / coupled) | 1.71× | 2.11× | 2.15× |

   Every zone is still worse under `'lag_compensated'`, by roughly the
   same ~2× the `kd=0.0` run found (1.96/1.05=1.87×, 1.81/0.61=2.97×,
   2.25/0.67=3.36× there) — the mechanism reading (crediting only past
   duty under-credits through the transient windows that dominate
   whole-run RMS) is unaffected by which gains drive the loop, as expected
   for a climb-side-only, `kd`-independent candidate. **One margin moved
   enough to flag**: reading the candidate's own RMS against the §3.4
   discrimination thresholds the same way the original `kd=0.0` text did
   (2.1/1.2/1.3 °C), z0's corrected RMS (1.639 °C) now sits clearly *under*
   its threshold, where the `kd=0.0` run's 1.96 °C read as merely "close
   to" 2.1 from the other side — z0 alone is weaker evidence than the
   original text implied. z1 (1.222 vs 1.2) still barely clears its
   threshold and z2 (1.463 vs 1.3) clears it more comfortably, so two of
   three zones still individually distinguish from noise on this
   candidate's own absolute error, and the ratio-based reading (~2× worse
   than `'coupled'`, essentially unchanged from the `kd=0.0` run) holds
   across all three zones regardless. Verdict unchanged: NOT adopted,
   `climb_mode='lag_compensated'` stays simulation-only/opt-in.

No firmware change made here (constraint of this pass). Reproducible via
`plant_sim.per_zone_gain_holdout_report(rows_fit, rows_test,
grid=<wider tuple to check convergence>)`; pinned by
`test_per_zone_gain_search_default_baseline_matches_live_board` in
`tests/test_plant_sim.py`.

**2026-09-03f — measurement-noise/quantization model added; gain search
re-run with it active; grid edge STILL not resolved.**

The `f89eb2a` fix above diagnosed the grid-pinning as the simulator having
"no measurement-noise/quantization model active" (`measurement_noise_std_c`
/`measurement_quantum_c` both default-off per `run_profile`'s own
docstring). That gap is now closed:

- **`MAX31856_QUANTUM_C = 0.0078125` C** — derived, not guessed, from
  `firmware/KilnFW/App/drivers/max31856_codec.h`'s
  `MAX31856_TC_TEMP_C_PER_LSB` (1/4096 C per raw 24-bit-word LSB; the low
  5 bits of that word are hardware-fixed 0, so the real step between
  representable temperatures is 32× that — 1/128 C — which the header
  states as an explicit equivalence). This supersedes the 0.1 C figure
  some of this module's own mechanism tests still use as a generic
  exercise value; that number was never read off the driver.
- **`MEASURED_THERMO_NOISE_STD_C = (0.0584, 0.0631, 0.0907)`** — measured
  directly off the same 2026-09-02 coupling excitation captures used to
  identify `K_full`/`tau`/`L`
  (`logs/coupling/cpl_z{0,1,2}_thermo.jsonl`): each zone's own-channel
  reading, linearly detrended over a rested/steady dwell plateau (z0
  `CH0` samples[260:300], slope −0.017 C/sample; z1 `CH1`
  samples[90:139]; z2 `CH2` samples[90:139]), residual std taken. Same
  order of magnitude as §3.8's independently-derived whole-run IAE noise
  floors (0.116/0.077/0.147 C, built by a different method — run-to-run
  range across six repeat captures, not per-tick std within one run — so
  expected to read somewhat higher, and does: ratios 1.99×/1.22×/1.62×).
  Injected at `run_profile`'s 1.0 s ticks even though the source captures
  poll at ~20 s: each MAX31856 conversion is an independent read with no
  on-board averaging, so per-sample noise magnitude does not shrink at a
  faster poll rate.
- Both default OFF in `run_profile` itself (0.0/0.0, unchanged — the
  `after`/`ifix`/`holdfix_clean`/`final` fixture captures stay
  byte-identical, confirmed by the existing
  `test_measurement_chain_defaults_off_reproduces_noise_free_result`) and
  default ON in `per_zone_gain_grid_search`/`per_zone_gain_holdout_report`
  (pinned by the new `test_gain_search_defaults_to_measured_noise_not_
  noise_free`). Noise is seeded from a **fixed, explicit tuple**
  (`GAIN_SEARCH_NOISE_SEEDS = (0,1,2,3,4)`), averaged over all five draws
  per candidate, with the SAME seed set reused for every candidate
  including the baseline (common random numbers → a paired comparison on
  matched noise, not two independently noisy samples) — deliberately not
  a single draw (noise alone could pick a "winner") and deliberately not
  a persisted/mutated generator crossing calls (the project's other,
  opposite seed bug — see `project_scenario_runner_state_bleed` — is a
  generator whose state leaks between runs; each `run_profile` call here
  builds its own fresh `np.random.default_rng(seed)` from an explicit
  seed argument, so nothing can carry over).

**Re-ran the same fit/test-swap comparison this section has used
throughout** (`p7_oldmatrix_http.jsonl` / `p7_newmatrix_http.jsonl`, both
directions, `climb_mode='coupled'`, `integral_floor='ff_hold'`,
`kd=BOARD_ZONE_KD`), now through `per_zone_gain_holdout_report`'s
noise-on-by-default gain search:

| grid | | z0 | z1 | z2 |
|---|---|---|---|---|
| 0.5×–2.0× (narrow, same as the pre-noise re-run) | fit OLD→test NEW | mult **2.00×**/0.50×, Δ=−0.286 | mult **2.00×**/2.00×, Δ=−0.241 | mult **2.00×**/2.00×, Δ=−0.147 |
| 0.5×–2.0× | fit NEW→test OLD | mult **2.00×**/2.00×, Δ=−0.300 | mult **2.00×**/2.00×, Δ=−0.268 | mult **2.00×**/2.00×, Δ=−0.155 |
| 0.25×–8.0× (wide) | fit OLD→test NEW | mult **4.00×**/4.00×, Δ=−0.325 | mult **4.00×**/4.00×, Δ=−0.258 | mult 2.00×/4.00×, Δ=−0.082 |
| 0.25×–8.0× | fit NEW→test OLD | mult **4.00×**/4.00×, Δ=−0.251 | mult **4.00×**/4.00×, Δ=−0.196 | mult 2.00×/8.00×, Δ=−0.033 |

**The grid edge is not resolved.** With realistic measurement noise and
the real MAX31856 quantization step active, `kp_mult` still pins at
whichever edge the grid offers (2.00× on the narrow grid, 4.00× on the
wide one) on z0 and z1 in every direction, and the deltas are numerically
close to the pre-noise re-run's own table (e.g. z0 fit-OLD→test-NEW:
−0.287 pre-noise vs −0.286 with noise) — noise of the measured, real
magnitude changed almost nothing. z2 is the partial exception: its
optimum moved off the immediate 2.00× edge on the wide grid (to 2.00×
still on kp but drifting on ki, Δ shrinking toward zero, 4.00×/8.00×
rather than climbing further), consistent with it already being the
weakest, most easily-erased signal in every earlier pass of this section.

**This is the disqualifying finding this addendum exists to report, not a
partial win.** Diagnosis: the injected noise (σ≈0.06–0.09 C, the real
sensor's own magnitude) is roughly an order of magnitude smaller than the
multi-degree ramp-tracking error the search is minimizing (whole-run
mean |error|, `sim_whole_run_iae_normalized` — the deltas above are
tenths of a degree on tracking error measured in whole degrees). A metric
that scores mean absolute tracking error simply cannot register
noise-amplified derivative ringing of that size; a larger `kp`/`kd`
keeps buying real tracking-error reduction (from the coupled ramp/dwell
dynamics this section has calibrated) far faster than it costs anything
this objective can see. Confirmed this is the metric, not a plumbing bug:
`test_gain_search_noise_actually_changes_the_ranking` proves the
noise/quantization chain is not inert (it does move the numbers, just not
by enough to flip which candidate wins), and it changed almost nothing
about which multiplier wins on any zone above.

**What would actually be needed to see the failure mode this section
originally expected** (not attempted here — out of this pass's scope,
`plant_sim.py`/tests/this doc section only): an objective that scores
something noise-amplified derivative action directly degrades and IAE
does not — commanded-duty variance/chatter, a rate limit or PWM-window
quantization on the actuator side (explicitly NOT modeled, see the module
docstring's "Not modeled" section), or an ITAE-style metric that would at
least weight the same tracking error differently. Absent one of those,
**this simulator's gain search cannot be trusted to reject an
ever-larger kp/kd on its own** — realistic sensor noise, injected at its
measured real-world magnitude, is not the missing ingredient the
2026-09-03e addendum expected it to be. The prior verdict stands and is
now on firmer ground: **no retune candidate survives from this
simulator**, and that conclusion is not an artifact of a missing noise
model — a correctly-derived one was tried and did not change it.

No firmware change made here. Reproducible via
`plant_sim.per_zone_gain_holdout_report(rows_fit, rows_test, grid=<...>)`
with no `measurement_noise_std_c`/`measurement_quantum_c` arguments (now
defaults to the measured chain); pinned by
`test_max31856_quantum_matches_driver_lsb`,
`test_measured_thermo_noise_std_is_same_order_as_iae_noise_floor`,
`test_gain_search_defaults_to_measured_noise_not_noise_free`,
`test_gain_search_noise_is_reproducible_across_calls` and
`test_gain_search_noise_actually_changes_the_ranking` in
`tests/test_plant_sim.py`.

**2026-09-03g — PWM window modeled, actuator-cost objective added; the
optimum still pins, just at a different edge, and it is a genuinely
disciplined fourth elimination, not a plumbing failure.**

The 2026-09-03f addendum above named the exact missing ingredient: "an
objective that scores something noise-amplified derivative action
directly degrades and IAE does not — commanded-duty variance/chatter, a
rate limit or PWM-window quantization on the actuator side (explicitly
NOT modeled...)". Both pieces of that were built this pass.

**The real PWM window** (`firmware/KilnFW/App/drivers/heater_output.c`'s
`heater_output_duty_ex`, the ordinary non-`force_new_window` path every
PID-driven zone calls): a fixed window (`HEATER_DEFAULT_WINDOW_MS` =
60000 ms); at each window boundary, that window's on-time is
`duty * window_ms`, quantized — below `max(min_on_ms,
HEATER_MIN_ON_MS_FLOOR)` (10000 ms) renders OFF for the whole window (not
rounded up), within `min_off_ms` (default 2000 ms) of the full window
renders ON for the whole window; and a RUNNING min-on hold independent of
the window boundary — once actually on, an off decision is deferred until
10 s of continuous on-time has accumulated, even across a window edge.
`plant_sim.py`'s `_pwm_render` is a line-for-line port of this (see its
own docstring), driven into `FOPDTPlant.step()` in place of the
continuous PID duty when `run_profile(..., pwm_window_ms>0)`. Default is
`0.0` (off, byte-identical to every prior caller — confirmed by
`test_run_profile_pwm_window_defaults_off_byte_identical`); the gain
search entry points default it ON at `HEATER_DEFAULT_WINDOW_MS`, same
convention as the noise defaults.

**Actuator cost, TWO signals, because they disagree in sign.**
`sim_relay_transitions_per_hour` (post-window relay transition count,
`heater_output_state_t.cycle_count`'s own accounting, mirrored) is the
obvious first candidate and IS physically grounded — but a sweep of
`kp_mult` 0.25×–8.0× on `p7_oldmatrix_http.jsonl` z0 found it **DECREASES**
as kp rises (98.5/hr at 0.25× down to 46.2/hr at 8.0×), the opposite of
"more gain chatters the relay more." This is not a bug: the window samples
duty ONCE per 60 s to decide that window's one on-time, so it is a hard
low-pass filter on relay-visible chatter by construction, and a
higher-gain loop that settles faster actually spends MORE time saturated
near duty 0 or 1 (fewer window-boundary crossings). `sim_duty_chatter_rate`
(mean `|Δduty|` per tick, measured on the continuous PID output BEFORE the
window quantizes it) was added as the second signal and rises
monotonically over the same sweep (~0.0027 to ~0.0170 duty/s, roughly
6×) — it is what a large kp/kd actually does to the control signal,
whether or not today's window happens to filter it into relay
transitions. Both are reported, never just one.

**Actuator-cost grounding, corrected mid-pass.** `RELAY_RATED_LIFE_CYCLES`
was initially set to 1e6 from a `heater_output.h` comment aside ("their
own loaded life is 1e6 operations"). Corrected to **1e5** after the
project's own `firmware/KilnFW/App/drivers/relay_cycles.h` was pointed
to — the module that actually performs persisted, per-relay lifetime
contact-cycle accounting, not a comment aside: *"the EE2-12NUH relays on
this board are electromechanical, with a contact life budget on the
order of 10^5 operations (docs/HARDWARE.md). A 60 s time-proportioning
window can spend that in a few hundred hours of firing... 'A kiln
controller that silently eats a relay's contact life is a controller
that fails mid-firing at cone temperature.'"* The arithmetic
cross-checks 1e5, not 1e6 (see `sim_relay_cycle_life_fraction_per_hour`'s
docstring): at a 10 s window, 1e5 / 360 cycles/hour = 278 hours — "a few
hundred hours," matching; 1e6 would give 2,778 hours, which would not.
The CONSEQUENCE this module states (a controller failing mid-firing at
cone temperature, ruining the load) is what makes the relay-life signal a
real cost, not merely a maintenance one — but note it is priced in
**degrees of tracking error traded per hour of life-fraction consumed**,
a value judgement this repo has no data to fix a single number for (see
below), not a probability of that specific failure.

**Composite objective, both weights swept, not invented.**
`per_zone_gain_grid_search` gained
`actuator_weight_c_per_life_fraction_per_hour` and
`actuator_weight_c_per_duty_chatter_rate` (both default `0.0` — identical
argmin to before these parameters existed unless a caller opts in) and
now reports, per zone, BOTH the tracking-error and BOTH actuator
components separately at baseline and at the chosen gains
(`fit_iae_*`, `fit_life_fraction_per_hour_*`, `fit_transitions_per_hour_*`,
`fit_duty_chatter_rate_*`) plus the composite actually used for argmin
(`fit_composite_*`) — never only the blended number.
`actuator_weight_sensitivity_sweep` runs the search across
`GAIN_SEARCH_ACTUATOR_WEIGHTS_C_PER_DUTY_CHATTER_RATE = (0, 1, 10, 50,
100, 300)` rather than committing to one invented C-per-duty-chatter
trade rate.

**Re-ran the gain search on `p7_oldmatrix_http.jsonl`, sweeping the
duty-chatter weight, on both the narrow (0.5×–2.0×) and wide
(0.25×–8.0×) grids:**

| grid | weight | z0 kp_mult | z1 kp_mult | z2 kp_mult |
|---|---|---|---|---|
| narrow | 0 – 10 | 2.00 (edge) | 2.00 (edge) | 2.00 (edge) |
| narrow | 50 | 1.50 (interior) | 0.50 (edge) | 0.50 (edge) |
| narrow | 100 – 300 | 0.50–1.25 | 0.50 (edge) | 0.50 (edge) |
| wide | 50 | 1.25 (interior) | 0.25 (NEW edge) | 0.25 (NEW edge) |
| wide | 100 | 0.25 (NEW edge) | 0.25 (NEW edge) | 0.25 (NEW edge) |

**The optimum still pins — the acceptance criterion (an interior optimum
that stays put when the grid widens) is not met.** At low weight it pins
at the same high-kp edge the 2026-09-03e/f addenda already found. At
moderate weight one zone (z0) briefly lands interior on the narrow grid,
but the SAME weight on the wide grid pins it at the new low-kp edge
instead — not a stable point, a moving one. At high weight every zone
pins at whichever edge has the lowest chatter. This is the expected shape
for the objective actually built: `sim_duty_chatter_rate` rises
monotonically in kp over the tested range with no floor or saturation of
its own, and IAE's own interior structure (it does dip and rise again
across 0.25×–8.0× — see the per-mult IAE column in the table this
addendum's underlying sweep produced) is not strong enough curvature to
pin a joint (kp, ki) composite against a linear, unsaturating actuator
penalty. A linear cost plus a weight is mathematically guaranteed to push
the argmin toward whichever grid edge the weight favors; it does not, by
itself, manufacture an interior minimum that survives widening the
search range.

**What is still missing, stated plainly per this task's own instruction:**
a genuinely curved (saturating, or floored) actuator-cost signal — one
that itself has an interior minimum in kp, or a threshold below which
chatter demonstrably does not matter — or independent evidence bounding
the correct weight so the search is not free to slide along whichever
edge an arbitrarily-swept constant favors. Absent either, **this
simulator's gain search still cannot reject an ever-larger kp/kd on its
own** — modeling the real PWM window and adding two real, differently-
grounded actuator signals changed WHICH edge the search pins at (and
proved the post-window relay-transition metric moves the wrong direction
entirely, a finding worth keeping on its own), but did not produce the
interior, grid-width-stable optimum this task set out to find. Four
honest eliminations in a row (wrong gains, no noise model, noise at
measured magnitude, and now actuator cost with a swept weight) is a
result, not a search failure: **no retune candidate survives from this
simulator**, and the missing piece is now specifically characterized
(actuator-cost curvature/saturation, or an independently-sourced weight)
rather than vaguely gestured at.

No firmware change made here (constraint of this pass). Reproducible via
`plant_sim.actuator_weight_sensitivity_sweep(rows_fit, weight_kind=
'duty_chatter', grid=<...>)`; pinned by
`test_pwm_render_matches_heater_output_c_constant_duty`,
`test_pwm_render_below_min_on_floor_renders_off_not_rounded_up`,
`test_pwm_render_near_full_duty_renders_full_window_on`,
`test_pwm_running_min_on_hold_survives_a_window_boundary`,
`test_run_profile_pwm_window_defaults_off_byte_identical`,
`test_run_profile_pwm_window_changes_the_trajectory_when_enabled`,
`test_sim_relay_transitions_and_life_fraction_consistent`,
`test_sim_duty_chatter_rate_rises_monotonically_with_kp_on_this_capture`,
`test_per_zone_gain_grid_search_reports_actuator_cost_components_separately`,
`test_per_zone_gain_grid_search_pwm_default_is_firmware_window`,
`test_actuator_weight_zero_with_pwm_on_matches_iae_only_argmin` and
`test_actuator_weight_sensitivity_sweep_shape` in `tests/test_plant_sim.py`.

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
      Every measurement in §2 is with it effectively off. A hardware run was
      being set up as of the original writing, blocked briefly by a
      `zones_http_client` field-mapping bug: firmware emits the JSON key
      `fuzzy_strength_pct` (`zones_http_handlers.c` ~line 308-321) but the
      POST form field is `z%u_fuzzy_strength`, no `_pct` suffix (~line 954),
      and the client's `_ZONE_FIELD_FORM_KEY` map did not yet know that.
      **That named blocker is resolved** — fixed in `b1ea749d`
      (2026-08-31), covered by
      `test_fuzzy_strength_override_matches_the_hardware_repro_command`
      (`tools/PcTools/tests/test_zones_http_client.py` ~line 870) — so the
      hardware run appears available again. Nobody has verified it was the
      *only* thing blocking that run, and this checklist item stays
      unchecked because the run itself has not happened. A profile-7
      baseline with `fuzzy=0` on the current build is running concurrently
      to have a same-build comparison point.

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

      **STOP — 2026-09-04, arm B1 of `fuzzy_ab_20260904b` is structurally
      inert, campaign should be halted.** Checked live from the running
      board mid-arm-B1 (`fuzzy_ab_strength50_20260903` applied,
      `fuzzy_strength_pct=50` confirmed via `GET /api/zones`), using
      exactly the `bd_kp_effective`/`bd_ki_effective`/`bd_kd_effective`
      instrumentation `zone_duty_breakdown_t` exists for
      (`profile_executor.h` ~line 445, published on `GET /api/control`,
      written at `profile_executor_pid_tick.c:90-92`). Six samples over
      ~2 minutes while `target_c` held near 40 °C and each zone's error
      swung from ~+6 °C (undershoot) through zero to ~-4.6 °C (overshoot)
      — exactly the error-varying stretch a fuzzy layer should react to —
      show `bd_kp_effective`/`bd_ki_effective`/`bd_kd_effective` bit-for-bit
      identical to the zones' configured base `pid_kp`/`pid_ki`/`pid_kd`
      (z0 0.0318/0.0001/0.8401, z1 0.0485/0.0002/1.0548, z2
      0.0631/0.0002/1.0690) on every sample, for every zone, through the
      error sign crossing. That is precisely `pid_fuzzy_adjust()`'s
      `strength_pct==0` short-circuit signature (`pid_fuzzy.c:161-166`) —
      not "a small nonzero nudge," an exact bit-for-bit match, which the
      fuzzy math cannot produce by chance once error crosses zero.

      Root cause found in `profile_executor.c`'s per-zone dispatch
      (~line 827-847): `pid_fuzzy_prepare_gains()` — the only call site of
      `pid_fuzzy_adjust()` — is reached exclusively from the
      `case ZONE_CONTROL_MODE_PID_FUZZY:` arm; `case ZONE_CONTROL_MODE_PID:`
      calls `pid_family_zone_tick()` with `&z->pid_cfg` unchanged and never
      looks at `fuzzy_strength_pct` at all. The live board's `GET
      /api/zones` reports `control_mode:2` (`ZONE_CONTROL_MODE_PID`) on
      all three zones, not `3` (`ZONE_CONTROL_MODE_PID_FUZZY`,
      `zones_http.h:849-852`) — and both campaign presets,
      `tools/PcTools/config_presets/fuzzy_ab_strength50_20260903.json` and
      `..._baseline_20260903.json`, set `"control_mode": 2` for every zone.
      The strength-50 preset's own description ("applying this preset
      changes `fuzzy_strength_pct` only") is the bug: `fuzzy_strength_pct`
      is a value with no reachable consumer, because the mode switch that
      gates the only code path that reads it was never part of either
      preset. This is the repo's "config value set, mode gate left off"
      inert-feature shape — the campaign's B arm and A arm run the
      byte-identical control path (plain `ZONE_CONTROL_MODE_PID`); the
      only thing that differs between them is a struct field nothing
      reads.

      **Recommendation: halt `fuzzy_ab_20260904b` now.** Ten hours across
      6 arms would produce a confident "no difference" verdict comparing
      plain PID against itself — arm B1's own duty/gain telemetry already
      proves this, no further sampling needed, well before hitting the
      documented per-zone noise floors (z0 0.116 °C, z1 0.077 °C, z2
      0.147 °C) or the ≥3-zone-agreement decision rule; a genuinely-off
      fuzzy layer was never going to clear either bar in the first place
      here, because it isn't engaged at all. To actually exercise the
      fuzzy layer on hardware, both presets need `"control_mode": 3` added
      alongside `fuzzy_strength_pct` (50 for B, 0 for A — note
      `strength_pct=0` in `PID_FUZZY` mode still exercises the mode-switch
      and bump-transfer machinery, just not the rule table, so it remains
      a fair baseline arm), and the presets/campaign runner should be
      re-verified live the same way before committing kiln time again.

      **FIXED 2026-09-04.** Both presets now set `"control_mode": 3`
      (`ZONE_CONTROL_MODE_PID_FUZZY`) on all three zones, `fuzzy_strength_pct`
      unchanged (50 for `fuzzy_ab_strength50_20260903`, 0 for
      `fuzzy_ab_baseline_20260903`), and each description corrected to say
      so instead of the "changes `fuzzy_strength_pct` only" claim that was
      the bug. Also checked whether the JSON fix alone was sufficient:
      `mcp_server_config_presets.py`'s `load_config_preset` only writes
      `control_mode` back to the board when called WITH `zones_host` (its
      docstring already says so — without it, `control_mode` is "reference/
      expected state only, NOT written"). `run_queue.py`'s own campaign
      path always supplies it: both `run_entry()` (line ~1095) and
      `_restore_baseline_preset()` (line ~1706) call
      `apply_preset_fn(control, preset, zones_host=cfg.host)`
      unconditionally, and with no `--serial-port` given (the normal case
      here, board reachable only over Wi-Fi) `apply_preset_fn` resolves to
      `_apply_preset_http_only`, which round-trips through
      `zones_http_client.apply_zone_preset` — i.e. the exact HTTP path that
      writes `control_mode`. So the campaign runner was never the problem;
      the JSON was the only thing that needed to change.

      **Live proof before restarting kiln time.** Applied
      `fuzzy_ab_strength50_20260903` via `run_queue._apply_preset_http_only`
      directly; `GET /api/zones` read back `control_mode:3,
      fuzzy_strength_pct:50.0` on all three zones (previously `2`). Started
      profile #7 (the campaign profile) to force PID ticks and sampled
      `GET /api/control` three times over ~15s:

      | zone | base kp | sampled bd_kp_effective | base kd | sampled bd_kd_effective |
      |---|---|---|---|---|
      | z0 | 0.0318 | 0.02518 → 0.02532 → 0.02542 | 0.8401 | 0.65181 → 0.6542 → 0.65585 |
      | z1 | 0.0485 | 0.03832 → 0.03864 → 0.03907 | 1.0548 | 0.81751 → 0.82185 → 0.82909 |
      | z2 | 0.0631 | 0.05079 → 0.0513 → 0.05174 | 1.0690 | 0.83632 → 0.84259 → 0.84779 |

      Every sample differs from the configured base gain (not bit-identical,
      unlike the mode-2 halt evidence) and drifts sample-to-sample as the
      bump-transfer/fuzzy state evolves — the signature the earlier
      diagnosis said was missing. Stopped the proof-firing
      (`profiles_stop`, `io_all_relays_off`, `io_read` confirmed R1-R4=0)
      before it ran long enough to matter thermally. Re-applied
      `fuzzy_ab_baseline_20260903` the same way and confirmed
      `control_mode:3, fuzzy_strength_pct:0.0` on all three zones. Campaign
      restarted as `fuzzy_ab_20260904c` (fresh prefix — `..._20260904_*` and
      `..._20260904b_*` are the earlier halted runs, left alone) with
      `--rested-timeout-s 14400` since the board was still ~38 °C from the
      B1 proof-firing when the queue was launched.

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

### 3.6b A/B reachability audit, prompted by the fuzzy-PID inert campaign (2026-09-04)

The fuzzy-PID finding above (§3.6, "control_mode:2" bug) forced the question:
**has the same flaw silently voided any other paired-run conclusion in this
project?** A campaign whose varied preset field never reaches the control
path produces a confident "no difference" that means nothing.

**`ease_off_window_mult` campaign (`logs/coupling/easeoff_ab_20260904_report.md`,
commit `5c1542e`) — audited, VALID, not inert.** Traced the consumer
(`zone_taper_climb_rate()` in `profile_executor_feedforward.c`, called from
`profile_executor_pid_tick.c` ~79-86): the gate is `!s_exec.dwelling &&
ff_rate != 0.0f` plus `z->ff_dead_time_s > 0.0f` inside the taper function —
**no `control_mode` gate**, unlike the fuzzy layer. Both `easeoff_ab_3p0` and
`easeoff_ab_2p0` presets carry real per-zone identified models (`ff_enabled`
true) and differ only in `ease_off_window_mult`; no second shared field gates
it out. The captured `.jsonl` logs show real ~150-175s ramp segments with
`dwelling==False` in every run of both arms, and `window_s = mult *
ff_dead_time_s` (105.6s at 2.0x vs 158.4s at 3.0x for z0) falls inside that
ramp length in both arms — the taper mechanically engages differently
between arms, over a meaningful fraction of every ramp. The campaign's
"indistinguishable" verdict reflects a genuinely small, noise-dominated
effect (the multiplier only shifts *when* within the ramp's final dead-time
multiples the taper starts, not whether one exists), not a wiring defect.
**No retraction — the "no board change" conclusion and the guidance not to
re-run this specific comparison without new information both stand.** One
gap found in the process, not the conclusion: the campaign's captures kept
only `exec`/`status` snapshots, not the finer-grained `bd_ff_rate_pretaper`/
`bd_ff_rate_posttaper` fields `dashboard_json.c` already exposes, so the
taper fraction itself could not be read back byte-for-byte from these logs —
addressed by the standing pre-flight check below, which asks for exactly
that field, live, before the fact rather than after.

**Other paired-run conclusions that changed a setting or closed an item**
(§3.2's coupling-matrix six-firing A/B, §3.9's tuning-method campaign) were
spot-checked for a `control_mode`-shaped gate on their own varied field
(`coupling_coeff` is read unconditionally by `zone_coupling_solve.c` once a
zone qualifies as a coupling neighbour, which is not itself
`control_mode`-gated beyond requiring a non-faulted, non-heat-blocked,
`ff_enabled` zone — satisfied by both arms) and found no equivalent bug; both
already carry their own extensive noise-floor/multiplicity discipline (§3.2,
§3.9) independent of this audit. Not re-verified line-by-line to the same
depth as the ease-off trace above — flagged here rather than re-litigated,
since neither conclusion moved.

**Standing pre-flight check — required before any future control-law A/B on
this board:** before committing kiln time to a new paired-run campaign,
prove from a **running** profile, live, that the varied preset field
actually reaches the control law and differs between arms in real telemetry
— not just that the preset JSON differs, and not just "verified by
readback" of the field itself (that is exactly what the fuzzy-PID bug's
original "verified" claim did: it read back `fuzzy_strength_pct`, the field
that was set, not `control_mode`, the field that gated it). Concretely:
apply arm A, start the profile, sample the relevant `bd_*` breakdown field
(`GET /api/control`'s `duty_breakdown`, e.g. `bd_kp_effective`/
`bd_ff_rate_posttaper`/`bd_coupling_correction` as applicable) a few times
live, apply arm B, sample again, and confirm the sampled value differs
between arms in a way consistent with the parameter under test — not merely
that the config readback shows the new value. Stop the proof-firing before
it runs long enough to matter thermally. This is the same discipline §3.6's
"Live proof before restarting kiln time" subsection already used to re-open
the fuzzy campaign; it is now required, not incidental, for every future A/B
in this section. A campaign whose own captures cannot show this after the
fact (as `ease_off_window_mult`'s could not, above) should capture the
relevant `bd_*` fields going forward so the proof survives in the log, not
only in a pre-flight check that happened once and was not recorded.

**Tooling that makes this checkable, not just describable (2026-09-04).**
`run_queue.py` now polls `GET /api/control` on every capture line and stores
its per-zone `bd_*` fields under a `"control"` key (`RunQueueConfig.
capture_control_bd`, CLI default **on** — pass `--no-capture-control-bd` to
opt out of a campaign that genuinely does not need it; costs ~371B/zone per
poll, so it is not unconditional). Once both arms of a campaign are captured
this way, run the after-the-fact form of this check directly against the
captures instead of re-deriving it from source each time:

    python -m kilnctrl.bd_reachability_check ARM_A.jsonl ARM_B.jsonl \
        --field bd_kp_effective --field bd_ki_effective --field bd_kd_effective

It reports, per zone/field, whether the two arms' sampled values are
EXACTLY bit-identical (the fuzzy-PID inert signature: exit 1, "INERT") or
genuinely differ (exit 0, "REACHABLE") — and refuses (exit 2) rather than
guessing when a capture predates `capture_control_bd` or was made with the
opt-out, exactly the gap `ease_off_window_mult`'s captures had. See
`tools/PcTools/src/kilnctrl/bd_reachability_check.py`'s module docstring for
the full mechanics and `tools/PcTools/tests/test_bd_reachability_check.py`
for worked examples of both verdicts.

**One command for the whole conclusion (2026-09-04).** Running
`bd_reachability_check` and `pid_ab_compare` by hand, in the right order,
with the right fields, is exactly the kind of improvisation that let two
fuzzy-PID campaigns ship inert without anyone noticing until a third,
unrelated audit caught it. `tools/PcTools/scripts/fuzzy_ab_analyze.py` is
the single entry point for a paired-run campaign's conclusion: it runs
`bd_reachability_check` FIRST per pair (gating -- an INERT pair voids that
pair's tracking-error numbers before they are even computed), then
`pid_ab_compare` per reachable pair (including its own start-temp confound
report), then applies the project's `>=3`-zone same-direction decision rule
across whatever complete pairs exist. For the live `fuzzy_ab_20260904d`
campaign:

    python tools/PcTools/scripts/fuzzy_ab_analyze.py \
        --log-dir logs/coupling --prefix fuzzy_ab_20260904d --pairs 3

It is safe to run against a campaign that has not finished: a pair with a
missing arm file is reported `not yet available` and skipped rather than
erroring, the overall verdict is `INCOMPLETE` (exit 3) until at least one
complete, reachable pair exists, and a pair whose fuzzy term turns out
bit-identical is reported `VOID` (exit 1) and excluded from the campaign
verdict rather than silently folded into it. See the script's own module
docstring for the exit-code contract and
`tools/PcTools/tests/test_fuzzy_ab_analyze.py` for the mandatory
negative test (two arms built from identical source data -- confirms the
tool reports VOID/INERT and refuses to proceed to a tracking-error
conclusion, rather than reporting a false REACHABLE).

### 3.7 Validation gap

Everything above is measured on a bench rig spanning 0–80 °C. Radiative transfer
goes as `T⁴`, so the plant at kiln temperatures is not the plant identified here.
Every result in §2 validates the **mechanism**, not the behaviour at firing
temperature. This stays open until a real firing.

**Unmeasured mains voltage is a second, standing confound.** Per
`firmware/SaftyFW/docs/CURRENT_SENSE.md` §"`mains_voltage_v` is a nominal":
"There is no voltage measurement anywhere in this design. `mains_voltage_v` is
a commissioning constant... Kiln elements are resistive, so power goes as
V²... a 5% supply sag... is a 10% error in the power figure." That error is
uninstrumented and uncompensated in every gain identification, every A/B
controller comparison, and the noise-floor campaign (§3.3, §3.7 above): some
fraction of any run-to-run scatter could be mains sag rather than the
controller, and this cannot currently be ruled in or out.

**PC-side tooling can grow a hard dependency on firmware that is not yet
flashed.** `ramp_assist_enabled` became a REQUIRED `config_presets.py` field
whose apply path calls `POST /api/ramp_assist`; the board was running
firmware predating that endpoint, so every preset apply failed with
`{"ok":false,"error":"no such endpoint"}` — a Python traceback mid-experiment
instead of a clear message, threatening an unattended campaign. Lesson: when
tooling gains a required field backed by a new endpoint, either the board
must be reflashed or the tooling must handle the endpoint's absence
deliberately (pinning a feature OFF is trivially satisfied when the firmware
lacks the feature; pinning it ON is a hard failure against old firmware).
Related: the bench board is currently running firmware older than the work
committed on 2026-09-02, so anything landed that day is unverified on
hardware until a reflash.

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

**Leading suspect tried 2026-09-03: per-path dead time — zone-dependent,
does NOT flip the verdict to positive.** `estimate_zone_mass_mult()`
delayed EVERY column of the drive term by the receiving zone's own (short,
34–53 s) dead time, including the neighbour columns whose real path delay
is 3–4x longer (§3.4's `plant_sim.L_PAIR`, 146.5 s ASSUMED off-diagonal
midpoint) — exactly this section's own leading suspect. Fixed, additively:
`estimate_zone_mass_mult()`/`estimate_all_zones()`/`estimate_from_capture_
path()` take an optional `L_pair` (default `None`, reproduces the original
single-delay reconstruction byte-for-byte — every existing caller,
including `load_mass_sweep.py`'s simulated-truth tests, is unaffected);
passing `plant_sim.L_PAIR` opts a caller into per-path delayed columns.

Re-ran the same 17-window real-capture sweep with `L_pair=plant_sim.L_PAIR`:

| | zone 0 | zone 1 | zone 2 |
|---|---|---|---|
| single delay (current), frac windows R²>0.3 | 0.24 (5/21) | 0.05 (1/21) | 0.05 (1/20) |
| per-path (new), frac windows R²>0.3 | 0.10 (2/20) | **0.41 (7/17)** | 0.00 (0/11) |
| single delay, max R² | 0.78 | 0.49 | 0.36 |
| per-path, max R² | 0.61 | **0.91** | −0.14 |

Zone 1 improves markedly (best R² 0.49→0.91, the fraction of windows
clearing 0.3 goes 1-in-21 to 7-in-17) — the clearest positive result in
either the sim-validation or real-capture columns of this whole section.
Zone 0 is roughly a wash (slightly fewer usable windows, comparable R²
range). **Zone 2 gets worse**, both in usable-window count (20→11, the
`min_drive_c`/warmup filters now exclude more of it since the warmup
exclusion is keyed to the longest per-row delay) and in fit quality (max R²
0.36→−0.14, i.e. it no longer fits better than a flat line at all).

**Verdict unchanged: still NOT actionably observable, and the fix is
zone-dependent rather than a uniform win — §3.8 stays a negative, not
flipped to positive.** Zone 1's improvement is real and worth keeping
(`L_pair` is now available for a future zone-1-only attempt), but a
per-zone estimator that works for one of three zones and gets measurably
worse on the zone with the largest known plant-identification error already
(§3.2's z2 cold-hold bias) does not clear the bar this section set for
"observable." Zone 2 remains the next suspect named by this pass rather
than closed by it: its warmup/dead-time filtering interacting badly with
the longer off-diagonal delay is a plausible mechanism, not yet checked.

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

**Per-zone-AND-per-source dead time, checked 2026-09-03 — the flat-146.5s
off-diagonal is not the limiting factor either; verdict unchanged.** The
"further step, not yet checked" this section named above (search each
neighbour COLUMN's own delay independently, rather than sharing one ASSUMED
midpoint across every off-diagonal cell) is now built:
`load_estimator.estimate_zone_mass_mult_per_source()` (additive; capture-path
convenience wrapper `estimate_per_source_from_capture_path()`). Grid-searches
each neighbour
column's delay over a 60–200 s range (brackets sec 2's 135–158 s measured
aggregate with margin) and keeps whichever combination maximizes THIS
zone's regression R², rather than assuming every column shares
`plant_sim.OFFDIAG_L_S`. Sim-validated first (`test_load_estimator.py`):
given a known-truth plant with two DELIBERATELY DIFFERENT neighbour delays
(120 s / 180 s, both distinct from the flat 146.5 s value), the search
recovers each within one 20 s grid step and clearly beats the flat baseline
on R².

*Real captures — same 17-window set §3.8 already scored (`p7_*_http.jsonl` +
the rested-start fixtures)* — the naive comparison looked like a large win
(z0 18/20 windows now clear R²>0.3 vs 2/20 flat; z2 5/11 vs 0/11), **but
this is a same-day sampling-size trap this doc has been burned by before,
not a real result**: the per-source search adds two more free parameters
(one delay per neighbour column) fitted against windows as small as 3
samples after the `min_drive_c`/warmup filters, and re-checking each
"win"'s own `n_samples_used` shows exactly that — most of the apparently
R²>0.9 z2 fits ran on `n=3`, the estimator's own minimum floor, where 2 free
parameters trivially explain 3 points regardless of whether the delays mean
anything (the same class of failure as this doc's own "n=3 correlation
collapsed to r=0.031 at n=6" note). Re-scored requiring `n_samples_used≥8`
(comfortably above the 2-parameter/3-point degenerate case, below is where
this contamination lives) on BOTH the flat and per-source fits:

| | z0 | z1 | z2 |
|---|---|---|---|
| windows total | 20 | 16 | 11 |
| flat: windows with n≥8 | 12 | 5 | 1 |
| flat: of those, R²>0.3 | 1/12 | 3/5 | 0/1 |
| per-source: windows with n≥8 | 7 | 2 | 4 |
| per-source: of those, R²>0.3 | 5/7 | 1/2 | **0/4** |

Per-source dead time also SHRINKS the number of adequately-sampled windows
(its larger searched delays exclude more warmup, same mechanism the flat
per-path fix already documented for z2) — z0 goes from 12 to 7 windows with
n≥8, z1 from 5 to 2. z0's adequately-sampled fits do look genuinely better
(5/7 vs 1/12) and are worth keeping as a lead for a future zone-0-specific
attempt; z2's, once the n=3 artifacts are excluded, is unchanged at zero
usable wins either way. **Verdict: this is the further step named above,
now checked, and it does not flip §3.8's z2 conclusion — z2's dead time is
still not observable from this repo's captures well enough to trust, by
this method or the flat one.** No firmware change made here.

**Both surviving leads (z0 per-source, z1 flat `L_pair`) checked 2026-09-03
— zone 1's R² gain is real but the recovered value is not physically
plausible as load; zone 0's lead does not survive a consistency check on
top of the R²/window-count gate. Neither is adopted; no firmware or
default-argument change made.** Both were re-run against the same
23-ramp-window real-capture set (`logs/coupling/p7_*_http.jsonl` +
`tests/fixtures/plant_sim/*.jsonl`, superset of the 17-21 windows the
passages above scored — the window construction here also drops dwell
segments via `plant_sim.segs_from_capture`, same convention) with a tighter
grid, `min_drive_c` sweeps, and, this time, a check the two passages above
did not run: whether the fitted `mass_mult` value is *consistent* across the
independently-fitted windows, not just whether each window's own R² clears
0.3. A firing's real load did not change between windows or between
captures, so a genuinely-observed mass multiplier should cluster; one that
scatters is noise that happens to fit a line, the same failure shape as the
z2 n=3 collapse, just showing up in the fitted value instead of the sample
count.

*Zone 0, per-source dead time (`estimate_zone_mass_mult_per_source`).* The
baseline grid (60–220 s, step 20, `min_drive_c=5.0`, matching the passage
above) scores 8 windows at `n_samples_used≥8` on this superset (doc's
n=7 was scored on the smaller 20-window set) — **6/8 clear R²>0.3**,
consistent with the "5/7" already recorded. A tighter grid (step 10) and a
narrow grid centered on sec 2's measured 135–158 s range (100–200, step 5)
were tried next: neither moves the win rate outside 5/7–7/10, i.e. finer
resolution does not buy a cleaner signal, which argues this is sampling
noise rather than a discretization artifact hiding a sharper true delay.
Widening `min_drive_c` (3.0→10.0) trades window count for a *worse* win
rate at every step except a single n=1 window (11→8→4→1 windows at
n≥8, win rate 6/11→6/8→2/4→1/1) — no setting recovers a consistent >80%
clearance the way a real, well-conditioned signal should. **The consistency
check:** the 6 windows that pass n≥8 and R²>0.3 on the baseline grid report
`mass_mult` = 0.159, 0.415, 0.436, 0.460, 0.492, 0.516 — no cluster, a
3.2x spread across windows drawn from the same (constant, unknown) real
load. That is not a converging physical estimate; it is six different
answers that each individually happen to fit their own window well. **n=6-8
is exactly the regime this doc has already burned itself on** (max-min
range at n=6 is 2.53σ, not 1σ; an n=3 r=0.97 here collapsed to r=0.031 at
n=6) — a spread this wide at this n is unsurprising under pure noise, not
evidence against it. **Verdict: the zone-0 lead does not hold up.** It
clears the same window-count/R² bar the previous pass recorded, but the
value it produces is not a number a controller could trust — closing this
as a second negative, not a promotion to actionable.

*Zone 1, flat per-path delay (`L_pair`).* Re-scoring at the doc's own
`offdiag=146.5s` (the adopted `plant_sim.L_PAIR` midpoint) with
`min_drive_c=3.0` on the same window set reproduces the recorded number
exactly: **17 windows at n≥8, 7/17 (41%) clear R²>0.3** — this pass changed
nothing about the method and got the same answer, a useful sanity check
that the earlier number was not itself a fluke of a different window set.
A grid search over the flat off-diagonal value from 80 s to 210 s (step 10,
same `min_drive_c=3.0`) shows the win rate is NOT flat across that range —
it rises through the search: 0/22-23 for 80–110 s, 0.14–0.26 for 120–140 s,
peaking at **0.56 (5/9) at 180 s** before falling again at 190 s+ as the
window count collapses (n≥8 drops to 5, then 3). 180 s sits above sec 2's
own measured 135–158 s aggregate range — the current `L_PAIR` (146.5 s) is
not the value this data best supports, though the higher win rate at 180 s
also costs windows (9 vs 17), so it is not simply a better setting, only a
different point on the same count-vs-quality tradeoff already documented
for the per-source search. **The consistency check, run at the doc's own
146.5 s/`min_drive_c=3.0` setting:** the 7 windows clearing R²>0.3 report
`mass_mult` = 0.215, 0.303, 0.334, 0.347, 0.348, 0.367, 0.367 — five of the
seven sit in a genuinely tight 0.303–0.367 band (spread 0.064), a real
cluster and the closest thing to a converging physical estimate anywhere in
this section, zone 0 included. **But the cluster sits at ≈0.33x, not near
1.0x.** No load was added or removed on the bench rig between the capture
that produced the ADOPTED `K`/`tau` identification and these p7 captures —
by the model's own assumption (mass scales `tau` only, §3.8 top), a stable
~0.33x reading would mean the rig itself changed mass by roughly 3x between
runs, which did not happen. The far more likely explanation is that this
fit is absorbing a **structural bias in the reference `K`/`tau`/`L` model
itself** (already known to carry z1 0.61 °C held-out RMS, §3.2, and an
own-zone `L=43.5s` that is itself a single fixed value, not searched here)
rather than measuring real thermal mass. A consistent-but-wrong number is
a different failure mode from zone 0's inconsistent one, but it is still a
failure mode: **this is not an observable load signal, it is a repeatable
model-mismatch artifact.**

**What either number would have been used for.** Per §7's ramp-assist
plan, a load estimate's only named consumer is an advisory "loaded kiln"
warning (banner/event-log/LCD) — never gain scheduling or feedforward,
which stay recommend-only per this doc's own rule (§5). Even the more
convincing zone-1 cluster is not fit for that: five values in a 0.303–0.367
band is tight *relative to zone 0*, but the underlying number is
physically wrong (§ above), so a warning built on it would fire on a
model-error signature at constant real load, not on an actual heavier
firing — worse than no warning, since it teaches the operator to distrust
a banner that is right by construction. Zone 0's 3.2x window-to-window
spread would swing the same binary warning on and off within a single
firing depending on which ramp segment happened to be in progress when it
was last recomputed. Neither result changes anything usable in the
controller or the UI; both close as documented negatives, matching this
section's standing bottom line that load is not observable from what this
repo's captures and model support today. No change to
`load_estimator.py`'s defaults, argument signatures, or `coupled_ident.py`
was made — both leads were checked, neither adopted.

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

---

## 6. Thermal-guard × identification-mode audit (2026-09-02)

Guard numbers match `thermal_guard.c`'s own section comments (1 heating-failed,
2 wrong-direction, 3 runaway/heat-off, 4 drift, 5 absolute limits, 6 sensor
validity, 7 frozen sensor, 8 cross-zone — the `thermal_guard_trip_t` enum in
the header is off-by-one from these for 5–9, a pre-existing doc inconsistency,
not fixed here).

| Guard | Step test | Relay autotune | Normal firing |
|---|---|---|---|
| 1/2 progress | armed, correct (`progress_duty_min` overridden to 0.01 so any commanded duty arms it; pre-PWM `want_duty` fed as `commanded_duty`) | **was armed-but-inert** — fixed this pass, see below | armed, correct (pre-PWM duty; guard 1's rate capped to the commanded ramp rate so a legitimate slow ramp can't self-trip) |
| 3 runaway (heat off) | inert (duty never 0 while stepping) — not this guard's case | deliberately inert by design (low branch is 0.15, not 0; guard 5/4 cover a welded contact instead) | armed, correct |
| 4 drift | armed but only meaningful with a real setpoint; step test pins `setpoint_c` to ceiling/raw+headroom | armed, correct (real oscillation setpoint) | armed, correct |
| 5 absolute limits | armed, correct | armed, correct (setpoint window pre-validated against it) | armed, correct |
| 6 sensor validity | armed, correct | armed, correct | armed, correct |
| 7 frozen sensor | armed, correct (continuous pre-PWM duty) | **was armed-but-inert**, same root cause as 1/2 — fixed this pass | armed, correct (fixed earlier pass, PWM-chop defect) |
| 8 cross-zone | deliberately disarmed — no peer data passed (`autotune_engine.c` never wires `peer_c`/`peer_ok`) | deliberately disarmed, same reason | armed but inert unless the zone has `cross_zone_max_delta_c` configured (0 by default — no measured cross-gain matrix exists) |

**Defect found and fixed this pass:** relay autotune fed `thermal_guard`'s
`commanded_duty` as the POST-PWM `want_relay_on ? want_duty : 0.0f`, the same
defect class already fixed for the step-test path and for `profile_executor.c`.
Every PWM off-pulse (~every `window_ms`, 60 s default) zeroed the reported
duty regardless of which bang-bang branch (0.15/0.85 at the default `d`) was
actually selected, so guards 1/2's 300 s progress window and guard 7's 600 s
frozen window could never accumulate enough contiguous time to complete — a
dead or flat-but-plausible element during a relay run went undetected for the
whole multi-hour budget. Armed in `guard_cfg`, inert against this method's
actual duty pattern — the dangerous case, indistinguishable from working
protection until it doesn't fire.

Fix (`autotune_engine.c`): feed both methods the pre-PWM `want_duty` (already
computed per-tick for both methods), and give `autotune_engine_run_relay()`
the same `progress_duty_min` override the step-test path already had (the
low branch, 0.15 by default, is still below `thermal_guard.c`'s stock 0.5).
Guard 3 is unaffected and stays correctly inert for a normal relay run: the
low branch is a genuine nonzero duty, not a chopped zero.

Pinned in `test_autotune_engine_prestart.c`: `test_relay_run_overrides_
progress_duty_min_same_as_step_test`, `test_relay_run_flat_dead_element_now_
trips_a_guard` (a dead element must still trip guard 1 — previously
impossible), `test_relay_run_healthy_rising_element_does_not_spuriously_trip`
(the newly-continuous window must not false-trip a healthy oscillation).
Both regression tests were confirmed red by mutation (reverting the
`commanded_duty` expression, and separately dropping the `progress_duty_min`
override) before the fix landed.

---

## 7. Ramp assist / schedule-stretching (owner decisions 2026-08-31, plan 2026-09-02)

Owner decisions:

- Max-ramp capability is extrapolated above the ~80 °C measured band (§3.4,
  §3.7); every reported point is labelled measured vs extrapolated. Owner's
  reasoning: even with a real kiln on hand, tuning will not happen at maximum
  temperature.
- Unachievable profiles auto-stretch so every target is still reached, EXCEPT
  a target above the kiln's permitted maximum, which is a hard refusal, never
  stretched.
- Dwell credit is weighted by heat work, accrues only while the kiln is NOT
  rising at the desired rate, and its band runs from the target down half a
  cone step (cone spacing is non-uniform).
- Cone table covers the full Orton range, cone 022 to 14.
- The feature ships behind a setting, default OFF, to be defaulted ON once
  validated on a real firing. Warnings surface three ways: a web banner, an
  event-log entry, and the LCD.
- Load/mass is not measurable — load estimation from available captures was
  investigated and found not observable (§3.8) — so the loaded-kiln warning
  is qualitative only and must not predict a magnitude.

Landing here rather than a new doc: this is control-algorithm work on the
same executor/feedforward machinery §3 already covers, not a new subsystem.

### 7.1 Lag detection — ALREADY BUILT, do not rebuild

`profile_executor.c`'s ramp-lock (~line 354-369) already detects "not
achieving the commanded ramp rate": a zone is lagging when
`|actual_c - target_c| > EXEC_RAMP_LOCK_BAND_C(zi)` (25 °C,
`PROFILE_EXECUTOR_RAMP_LOCK_BAND_C`, `profile_executor.h:638` — the value
`exec_threshold(zi, 3)` in `profile_executor_pid_tick.c` falls back to
whenever no per-zone override is configured, true for every shipped
config. CORRECTED 2026-09: this section previously said "3 °C" here, which
is actually `PROGRESS_BAND_C` — guard 1's arrival band, `thermal_guard.c`
— a different constant. That stale "3 °C" claim in this very paragraph is
the most likely source of the identical mistake that shipped in
`ramp_assist.py`'s own mirror of this constant; see §7.6's "mirror bug"
note), tracked per-tick into
`s_exec.ramp_lock_held` / `s_exec.ramp_lock_lagging_mask`. While held, the
shared `target_c` simply stops advancing (segment-stepping code just below).
This alone already guarantees every ramp endpoint is eventually reached,
including back-to-back ramps with no dwell between them — ramp assist adds
credit and warnings on top of this signal, it does not need a second lag
detector.

### 7.2 Auto-stretch — DONE (2026-09-03)

Implemented in `profile_executor_ramp_assist.c`'s `ramp_assist_stretch_rate_c_per_s()`
and wired into `profile_executor.c`'s segment-stepping code (the `else if
(lock_ok)` branch, now `else if (lock_ok || stretched_this_tick)`). Sec 7.1's
ramp-lock (already built) still unconditionally guarantees every ramp
endpoint is eventually reached, by freezing `target_c`/`segment_elapsed_s`
while a zone lags — that guarantee, and its unconditional behaviour with the
flag off, is untouched. What this adds, gated on `ramp_assist_cfg_enabled()`:
once a lagging zone's lag has been continuous for `EXEC_SUSTAINED_LAG_S`
(~30s, sec 7.1's existing `lag_sustained` field) — i.e. the lock would
otherwise hold indefinitely, not just a normal momentary PID-settling hold —
`target_c` stops sitting fully frozen and instead creeps forward at the
slowest sustained-lagging zone's own demonstrated achievable rate
((`actual_c - lag_start_actual_c) / lag_held_s`, clamped >= 0, minimum across
every active/non-faulted/lagging/sustained zone — same conservative
"never outrun the slowest one" direction sec 7.3's dwell-credit spend
already uses) instead of the commanded `seg->ramp_c_per_hr` it cannot keep
up with.

Hard refusal (a segment target above the zone's `max_temp_c` ceiling is
refused, never stretched): still enforced entirely by
`profile_executor_run.c`'s existing `max_temp_c` re-check at firing start
(~line 252-271) — unchanged by this pass. Auto-stretch's rate only ever
advances `target_c` TOWARD `seg->target_c`, using the exact same "reached"
clamp the pre-existing ramp path already used, so there is no code path
through the new stretch logic that could push `target_c` past a target that
check would have refused — the constraint holds by construction, as
`profile_executor_ramp_assist.c`'s own top-of-file comment already argued
for the (then instrumentation-only) sec 7.1/7.4 pieces.

Composition with the next segment (owner decision: stretching must not
corrupt the following segment's own target/shape): once a stretched ramp's
`target_c` reaches `seg->target_c` it hands off to dwelling through the
identical code path a normal (unstretched) ramp does — the next segment is
read fresh from `s_exec.profile.segments[]` when the schedule steps forward,
untouched by which rate (commanded or stretched) got the previous segment
there. Only the wall-clock time a segment takes changes.

Dwell-credit hookup (owner decision 3, "wire the stretch to it, don't
rebuild it"): no new wiring needed — sec 7.3's `ramp_assist_dwell_credit_tick()`
already runs every tick a ZONE_RAMP segment is active and not dwelling,
regardless of whether that tick's `target_c` advance came from the strict
hold, the commanded rate, or the new stretched rate; it watches `actual_c`
against the band, not which rate is driving `target_c`. Stretch and credit
compose automatically.

Ships behind the existing `ramp_assist_cfg_enabled()` flag/`/api/ramp_assist`
route (sec 7.5) — no second flag or route added, per owner decision 4.

Host tests: six new tests in `test_profile_executor_prestart.c`
(`test_stretch_rate_*`) pin `ramp_assist_stretch_rate_c_per_s()`'s gating
sentinel (assist off, no zone sustained), its exact achieved-rate arithmetic,
the >=0 clamp on a cooling zone, the minimum-across-zones rule, and that a
faulted/not-lagging zone is excluded. `profile_executor.c`'s own tick-loop
wiring is not directly host-testable (this test file's own header note: no
FreeRTOS task harness), same limitation sec 7.3.3 already documented for the
dwell-credit spend's caller-side wiring.

Mutation testing (this task's report has the exact commands/output): (1)
flipping the min-selection to max — caught by
`test_stretch_rate_uses_minimum_across_sustained_lagging_zones`; (2) deleting
the `>= 0` clamp — caught by
`test_stretch_rate_clamped_nonnegative_when_zone_cooled`; (3) deleting the
`assist_enabled` gate — caught by
`test_stretch_rate_returns_sentinel_when_assist_disabled`. All three applied,
built, run, real failure text captured, then reverted; reverted state
re-verified clean (`grep MUTATION` empty, all 21 host-test executables
green) before committing.

**Not done in this pass:** sec 7.4's warning surfaces are now built (see
§7.4 below, updated 2026-09-03) — the web banner and LCD notice do
distinguish strict hold from stretched, using the same
`ramp_lock_held`/`lag_sustained`/`stretch_by_segment_s` fields this
paragraph used to call unsurfaced. Sec 7.6's real-firing validation of this
specific control behaviour (as opposed to the credit) has not been run.

### 7.3 Dwell credit — FIRMWARE LANDED (2026-09-03, see §7.3.3), CREDIT GATE FIXED (2026-09-03, see §7.6), ACCRUAL EXTENDED PAST THE NOMINAL RAMP END (2026-09-03, see §7.6.1)

Heat-work-weighted accumulator, active only while the zone is BEHIND
SCHEDULE AT ALL (`actual_c` below the moving commanded setpoint — see the
gate-fix note in §7.6, and NOT the ramp-lock's 25 °C `lagging` signal,
which stays exactly as it was for the lock itself), band from the segment
target down half a cone step (uses the new `cone_table.c`/`.h`, Orton
022-14, already landed by another agent — half-step band and Arrhenius
heat-work weighting come from that module).

**Extended past the nominal ramp end (2026-09-03).** A SECOND gap, distinct
from §7.6's credit-gate fix: the ramp step formally ends (`s_exec.dwelling`
flips true) once a lagging zone is back within the WIDE 25 °C ramp-lock
band (`EXEC_RAMP_LOCK_BAND_C`), but the credit band above is the much
NARROWER half-cone-step band. Under heavy thermal mass a zone routinely
un-locks (ending the ramp) while still outside the credit band, and only
crosses into it after dwelling has already begun — past the old
`ramping_now = (seg_kind == ZONE_RAMP) && !dwelling` gate. Measured:
credit was exactly 0.0 s at 2x/4x mass for bisque and cone 6, and at 4x for
cone 10 (§7.6.1's table). **Owner decision:** keep accruing for as long as
the current segment is a ZONE_RAMP, dwelling or not, until `actual_c`
actually reaches the segment target — `ramp_assist_credit_should_accrue()`
(`profile_executor_internal.h`) now gates on `seg_kind` alone, with no
dwelling term at all (its signature does not accept one).

**THE HAZARD, and how it is made impossible, not just avoided:** credit
banked after a dwell's timer has already started must never be allowed to
shorten THAT SAME dwell — that would let time spent dwelling (while still
in-band) shrink the very timer measuring it, a circular, self-shortening
loop, and one that would be very hard to notice in a real firing (the
dwell just quietly ends early). This is prevented structurally, not by an
extra runtime check: `s_exec.dwell_credit_applied_s` is captured EXACTLY
ONCE, at the single tick a dwell is entered (`ramp_assist_dwell_credit_
spend()`'s return value, `profile_executor.c`'s dwelling-transition code),
and every later tick's dwell-end threshold (`dwell_min*60u -
dwell_credit_applied_s`) re-reads only that one frozen float — never a
zone's live `dwell_credit_s`. Because `ramp_assist_dwell_credit_spend()`
is called nowhere else for that dwell occurrence, credit accrued after
entry has no code path back into its own threshold: it is simply carried
forward in `zone_runtime_t.dwell_credit_s` and can only ever be spent at
the NEXT dwell entry — a segment boundary later in time, never the one it
was earned during. `profile_executor.c` now carries an explicit
CIRCULAR-DWELL HAZARD comment at both the freeze point (right after the
`ramp_assist_dwell_credit_spend()` call) and the threshold read, so a
future "simplification" to read live credit cannot land silently.
**Pinned by `test_dwell_credit_spend_snapshot_is_frozen_against_later_
accrual`** (`test_profile_executor_prestart.c`): banks credit before dwell
entry, spends it (capturing the frozen snapshot), then runs several more
ticks of in-dwell accrual and asserts the already-captured snapshot is
untouched, that a live-recomputed threshold would have visibly drifted
from it, and that the correct frozen threshold (560 s exactly, from a 40 s
spend against a 600 s nominal) stays exactly 560 s throughout. Negative-
tested: recomputing the test's own "frozen" threshold from the live
post-accrual credit (i.e. simulating the circular bug directly) produced
the real failure `the correct, frozen threshold must still be exactly
560s -- unmoved by every tick of in-dwell accrual this test ran` — quoted
verbatim, then reverted. A second negative test on the new gate function
itself (`test_ramp_assist_credit_should_accrue_ignores_dwelling`, mutated
to `return false` unconditionally) produced `a ZONE_RAMP segment must
accrue -- this is the only kind dwell credit ever applies to...` — also
quoted, then reverted. Simulator: `ramp_assist.py`'s `run_ramp_assist`
mirrors this exactly — the `DwellStep` branch now also accrues into
`z.credit_s` (same behind-schedule/in-band test, against the dwell's held
target), and `z.dwell_remaining_s` is still computed once, from a frozen
`spend`, and only ever decremented by `dt` afterward — never recomputed
from `z.credit_s`. See §7.6.1 for the re-run cone-scale figures (mostly
UNCHANGED, and why) and the audit-accumulator fix this extension required
in `dwell_credit_parity()`.
Blocker found in survey: `profile_executor.c` (~line 461-465) zeroes
`segment_elapsed_s` exactly at the ramp→dwell transition, discarding the
near-target history the credit needs to accrue from. New accumulator state is
required; it cannot be reconstructed from `segment_elapsed_s` after the fact.

**Simulator-side validation (2026-09-02)**, `tools/PcTools/src/kilnctrl/ramp_assist.py`:
before this lands in firmware, the algorithm was prototyped against the
calibrated simulator (`plant_sim.py`) to check the credit's arithmetic in
isolation. First pass over that harness's own `compare_heat_work` helper
(whole-run heat work, assisted vs. unassisted) reported what looked like a
systematic under-fire — roughly 0.3-0.7% on an easy single-ramp cone-scale
scenario, 4.9-14.4% on a fast ramp straight into a dwell, and 13-22% on a
back-to-back-ramps scenario — worse on more heavily lagging zones in every
case. The working theory going in was a **unit mismatch**: banking credit in
raw in-band seconds but spending it as if each second were worth a full
weight-second.

That theory does not hold for this code. `ramp_assist.py`'s accumulator
(~line 301) already banks `weight * dt` — heat-work seconds, not raw
seconds — and spends exactly that many seconds off the following dwell
(~line 313: `spend = z.credit_s`). There is no unit conversion missing
between the two. Instrumenting the harness to separate the dwell's own
heat work from the ramp's (new `dwell_heat_work_s` field, exposed through
the new `dwell_credit_parity()` function) shows why `compare_heat_work`
was misleading: it compares *whole-run* heat work, and the ramp phase is
bit-for-bit identical whether or not credit is applied (`apply_dwell_credit`
only changes the dwell's spent duration, nothing upstream). Shortening a
period spent near 1.0 weight by `credit_s` seconds necessarily removes
close to `credit_s` weight-seconds from the whole-run total, regardless of
what unit the credit was banked in — there is no "give-back" to measure at
that granularity, so the metric was reporting an unavoidable arithmetic
artifact, not a defect in the credit.

Measured against the *actual* question — does `credit_s + (dwell heat work
during the shortened dwell)` match what the dwell would have delivered on
its own, uncredited (`dwell_credit_parity()`'s `parity_vs_unassisted_pct`)
— the credit *appeared* correct to within simulator noise:

| scenario | zone | naive whole-run delta (misleading) | parity vs. achievable dwell baseline | parity vs. idealized nominal | catch-up deficit |
|---|---|---:|---:|---:|---:|
| physical_cone (700 C, 300 C/min, 20 min dwell) | 0/1/2 | −0.34% / −0.52% / −0.71% | **+0.073% / +0.028% / +0.075%** | −3.4% / −4.6% / −6.3% | 42.1 / 56.0 / 76.3 s |
| back-to-back (700→803.9 C, 10 min dwell) | 0/1/2 | −13.3% / −17.0% / −22.4% | **+0.075% / +0.088% / +0.149%** | −2.7% / −3.5% / −4.8% | 16.4 / 21.6 / 29.5 s |
| physical_hard (900 C, 500 C/min, 30 min dwell) | 0/1/2 | 0.00% (zero credit earned) | 0.000% | −4.9% / −6.6% / −14.4% | 88.7 / 117.9 / 260.0 s |

`parity_vs_unassisted_pct` was ≤0.15% in every scenario that earned any
credit at all, which read as effectively exact. **That reading was wrong —
`parity_vs_unassisted_pct` was retracted on 2026-09-02, see "DEFECT 1 —
retraction" below. It is a near-identity, not a correctness check, and
must not be read as evidence the credit's arithmetic is right.** The naive
whole-run comparison is kept (as `compare_heat_work`, now documented as
such) because it is still useful bookkeeping, but it must not be read as
an under/over-fire signal on its own either.

#### DEFECT 1 — retraction of the 0.15% parity claim (found 2026-09-02, adversarial review)

The `parity_vs_unassisted_pct` figures in the table above (≤0.15% in every
scenario that earned credit) are **retracted, not just superseded** —
kept in this record deliberately, as the trap the next person should not
fall back into. The metric was:

```
recipe_total_s = credit_s + dwell_heat_work_assisted_s
parity_vs_unassisted_pct = 100 * (recipe_total_s - dwell_heat_work_unassisted_s)
                                / dwell_heat_work_unassisted_s
```

During a dwell the zone sits at target, so its heat-work weight is ~1.0.
Shortening the dwell by `credit_s` seconds therefore removes ~`credit_s`
weight-seconds from `dwell_heat_work_assisted_s` relative to the
unassisted run, so algebraically
`credit_s + dwell_heat_work_assisted_s ≈ dwell_heat_work_unassisted_s`
**for any value of `credit_s`** — the metric compared credit against
itself and read ≈0 regardless of whether the credit banked was correct,
half, double, or triple what it should have been. Proven by scaling
*only* the accrual line `z.credit_s += w * dt` by a constant factor and
re-running the back-to-back scenario from the table above
(`tools/PcTools/tests/test_ramp_assist.py`'s
`ScaleSweepDiscriminatesCreditErrorsTests` is this sweep, executed as a
permanent regression test):

| SCALE | zone 0 | zone 1 | zone 2 |
|---|---:|---:|---:|
| 0.5x | 0.038% | 0.131% | 0.162% |
| 1.0x (real code) | 0.075% | 0.088% | 0.149% |
| 2.0x | 0.151% | 0.004% | 0.123% |
| 3.0x | 0.055% | 0.092% | 0.923% |

A credit half, double, or triple the correct value all passed the
`abs(...) < 1.0` threshold this module's tests used to pin the metric.
Worse, the documented negative test for this metric (mutating the accrual
from `w * dt` to bare `dt`, i.e. banking raw in-band-and-lagging seconds
instead of heat-work seconds) gave parity 0.000% / 0.000% / 4.380% —
**two of three zones, which were genuinely mis-banked by roughly
10-30x, read EXACTLY ZERO.** It passed only by the luck of the third zone
saturating its dwell.

**Fix — `credit_audit_pct`** (replacing `parity_vs_unassisted_pct`,
`ramp_assist.py`): compares `credit_s`, the credit actually spent, against
`credit_reference_heat_s` — an audit accumulator computed at the identical
accrual gate (same tick, same lagging/in-band condition) but on its own
separate `+=` statement, never derived from `credit_s`. In correct code
both statements compute the identical `weight * dt` and the two totals
are equal to float precision for any schedule; a regression confined to
the real accrual line leaves the independent line untouched, so the two
diverge by exactly the size of the regression — nothing on the other side
of the comparison can absorb it the way `dwell_heat_work_unassisted_s`
absorbed `credit_s` in the retracted metric. Re-running the same SCALE
sweep against `credit_audit_pct`:

| SCALE | zone 0 | zone 1 | zone 2 |
|---|---:|---:|---:|
| 0.5x | −50.000% | −50.000% | −50.000% |
| 1.0x (real code) | 0.000% | 0.000% | 0.000% |
| 2.0x | 100.000% | 100.000% | 100.000% |
| 3.0x | 200.000% | 200.000% | 200.000% |

Exactly `(SCALE − 1) × 100%` for every zone — clean, monotonic,
unambiguous discrimination between 0.5x/1.0x/2.0x/3.0x, which is what a
correctness metric for this accumulator actually needs to provide. The
raw-seconds regression (`w * dt` → `dt`) now reads ~3056%/3180%/3135%,
not a suspicious exact zero.

**Scope of what this proves — `credit_audit_pct` is a two-writer
consistency check, not a correctness proof for the model.** Both the real
accrual line and the audit line share the same weight function, the same
`cone_table.band_bottom_c` band, the same target, the same accrual gate,
and the same tick loop — so it detects only an ACCRUAL/SPEND
IMPLEMENTATION SLIP between those two statements. It is blind to any
error shared by both writers, because a shared error moves both sides of
the ratio together and cancels out. Confirmed by two mutations that left
it unmoved on this module's reference schedule: doubling the credit
band's half-width moved `credit_s` from 32.45 to 132.16 (+307%), and
raising Ea from 300 kJ/mol to 500 kJ/mol moved `credit_s` from 32.45 to
27.26 (−16%) — `credit_audit_pct` read exactly 0.0% on every zone in both
cases. The weight function, the Ea choice, and the band width itself are
**not** validated by this check, and are not validated by any metric in
this module — see `cone_table.h`'s Ea-sensitivity note (§7.3.1/7.3.2) for
the current honest error range on Ea.

A physically-grounded alternative was also tried and rejected before
`credit_audit_pct` was adopted: comparing real heat work integrated over
a FIXED-length window (the dwell's own nominal duration, starting at the
dwell's entry — long enough that an early-ended, over-credited dwell
would visibly run into whatever comes next) against what an
always-at-target zone would deliver over that same span
(`window_parity_pct`, still reported by `dwell_credit_parity()` as a
secondary diagnostic). The SAME SCALE-sweep discipline that caught the
original bug caught this one too, before it landed as the fix: on a
trailing dwell (the schedule's last step), the simulator correctly keeps
holding the zone at the same target after the schedule finishes, so
"the ticks after an early-ended dwell" and "the ticks of the dwell
itself" are physically indistinguishable — the window's measured heat
work came back bit-for-bit IDENTICAL across all four SCALE values. On a
short dwell fed by a comparatively large credit, `window_parity_pct` also
moved only a few points and non-monotonically, swamped by an unrelated,
scale-independent catch-up-lag term. Both are documented as known,
real blind spots of that signal (see `dwell_credit_parity()`'s
docstring) — genuine dead ends caught by testing before shipping, which
is why this module treats `credit_audit_pct` as authoritative and
`window_parity_pct` as supplementary only.

**Conclusion:** no unit-mismatch bug was found in the credit's bank/spend
arithmetic either before or after this retraction — that underlying
finding survives. What did not survive is the claim that
`parity_vs_unassisted_pct` was capable of showing it either way; it could
not, and the record above exists so nobody re-derives it as "the obvious
metric" a second time.

**Second-order effect (real, and separate from the credit):** even at ZERO
credit (`physical_hard` row above — the ramp is fast enough that it never
enters the in-band-and-lagging gate, so `credit_s == 0` for every zone),
`parity_vs_nominal_pct` is still −4.9% to −14.4%. This is the effect flagged
as a risk before this investigation started: the zone is often still
climbing toward target when the dwell timer starts, so the EARLY seconds of
*any* dwell — credited or not — are worth less than 1.0 weight too. The
`catchup_deficit_s` column quantifies it directly (`dwell_nominal_s -`
achieved heat work in a full, uncredited dwell): 16-30 s on the cone-scale
scenarios, 89-260 s (up to ~14% of a 30-minute dwell) on the fast-ramp one,
worse on more heavily lagging/coupled zones — the same "worse when lagging
more" pattern the original naive metric showed, but here it is a genuine
plant-dynamics effect, unrelated to whether ramp assist is enabled at all.
Because it is identical in the assisted and unassisted runs (both enter the
dwell in the same physical state), it cancels out of
`parity_vs_unassisted_pct` — the credit mechanism neither causes nor fixes
it. Whether it argues for the minimum-dwell floor the owner asked about is
a separate, pre-existing question about dwell timers in general (does a
kiln recipe's `dwell_min` promise "N minutes of timer" or "N minutes at
temperature"?) — it is not evidence that ramp assist's credit needs a floor,
since the credit's own parity is already exact. Recommend surfacing it
alongside auto-stretch's warnings (§7.4) if a real firing (§7.6) confirms
the same pattern, rather than baking a floor into the credit formula itself.

Coverage: `tools/PcTools/tests/test_ramp_assist.py`'s `DwellCreditParityTests`
pins both results — near-exact parity against the achievable baseline, and
a nonzero-but-explained nominal gap even at zero credit — and is
negative-tested against exactly the raw-seconds regression this
investigation ruled out (mutating the accrual from `weight * dt` to a bare
`dt` reintroduces a ~4.4% parity error, caught immediately).

(Note: the two sub-80 C `measured_*` scenarios in the harness fall below
the cone table's covered range — Orton 022 and up — so `heat_work_weight`
raises `ConeTableError` on every tick and both `heat_work_s` and
`dwell_heat_work_s` are exactly 0 there; `dwell_credit_parity` reports
`nan`/-100% for those, which is the cone table's known low-temperature
floor, not a new finding.) All three scenarios above 80 C rest on the
simulator's extrapolated high-temperature plant parameters (unmeasured,
assumed) — the percentages are indicative of the effect's existence and
rough scale, not authoritative numbers to design a fix against.

**Owner decision (2026-09-02): dwell timing stays as-is.** `dwell_min`
means N minutes of *timer*, counted from when the ramp's setpoint reaches
the segment target — not N minutes at temperature and not N minutes of
heat work. The catch-up deficit documented above (4.9-14.4% of nominal
heat work, 16-260 s, identical with or without ramp assist) is accepted
as a known, measured property of the executor's existing dwell semantics
and will be documented rather than corrected. Rationale: it matches
long-established behaviour in commercial kiln controllers, keeps a
profile's total schedule length predictable, and "fixing" it would
silently change the fired result of every profile already stored on a
board. User-facing note added at `firmware/KilnFW/docs/PROFILES.md`
("A dwell is a timer, not a soak").

### 7.3.1 Cone table data-entry error — FOUND AND FIXED (2026-09-02)

The `cone_table.c`/`cone_table.py` table this section's dwell-credit math
depends on shipped with **ten wrong temperatures** out of 36 entries, found
by an adversarial review that pulled the actual Orton Self-Supporting
108 F/hr chart and independently re-verified every entry against it
(`hotkilns.com/sites/default/files/pdf/cone-chart.pdf`). Cones 011-018 were
each off by 15-31 C (013/012 had been swapped in from the SS 27 F/hr column;
016 was even further off), and cones 13/14 had been sourced from the wrong
chart section entirely (13 from the Large-cone 270 F/hr column, 14 from the
asterisked "different composition" Large-cone row). A tell that would have
caught this without a source lookup: the wrong table's 011→010 gap was only
8.9 C against neighbouring gaps of 15-56 C — the corrected table's 011→010
gap is a normal 27.8 C, and a full adjacent-gap scan after the fix found no
further outliers (the two genuinely tight gaps in the table, 10→11 at ~9 C
and 1→2/7→8 at ~5-10 C, are real per the source chart, not errors). Both
files were corrected to match (byte-identical content, values only); the
two tests that pinned the wrong cone-14 value were updated, not loosened.
A cross-language pin test (`ConeTableCrossLanguagePinTest` in
`tools/PcTools/tests/test_cone_table.py`) was added that parses `s_cones[]`
directly out of `cone_table.c` and asserts it against Python's `CONE_TABLE`
— this is the enforcement the module docstrings on both sides had claimed
existed but did not; before this pass, correcting the C table alone failed
zero Python tests. Two further documentation-only contradictions between
`cone_table.h` and the actual `cone_table.c` implementation were found and
corrected in the same pass (not behavioural fixes — the header text was
wrong, not the code): the heat-work weight is a min-max rescale across the
band, not "normalised by dividing by the rate at the target" as the header
claimed (the min-max form is the conservative direction — it under-credits,
which lengthens dwell rather than risking over-fire, so the code was kept
and the header corrected); and the between-cones band-bottom case does not
linearly interpolate anything (the lower bracketing cone is used as-is) —
`cone_table.py`'s docstring already had this right. Ea = 300 kJ/mol is
documented as chosen to reproduce the table's own qualitative
per-cone-step rate change at this module's own min-max normalisation, NOT
as consistent with the table's rate columns — inverting those columns
gives apparent activation energies roughly 500-1000+ kJ/mol across the
working range, and the min-max normalisation is currently absorbing most
of that gap. (At the time of this pass the over-credit was estimated as a
single ≈7% figure at Ea=300 kJ/mol vs. a table-consistent Ea in the
~700 kJ/mol range; that single-figure framing was itself later found to be
wrong -- the error scales with band width, which varies enormously across
the table, so there is no one honest bound. The corrected range -- ~1%
to ~32% at 700 kJ/mol, table-wide mean ~6.8%, worse against 1000 kJ/mol --
is in `cone_table.h`'s top-of-file comment, current as of the half-cone
addition in this same pass.) Ea was deliberately left unchanged in this
pass; changing the normalisation to match the old (incorrect) header
wording without also revisiting Ea would roughly double every figure in
that range. See `cone_table.h`'s top-of-file comment for the full
corrected rationale.

### 7.3.2 Band-width duplicate removed (2026-09-02)

`ramp_assist.py` carried its own copy of the bracketing-pair band-half-width
formula (`_local_band_half_c`/`_band_bottom_c_fixed`), written as a
workaround while `cone_table.band_bottom_c` still had the collapse defect
fixed in §7.3.1. Verified numerically equivalent across the table (between
cones, on/above/below a cone, both range boundaries) and removed; the
simulator now calls `cone_table.band_bottom_c` directly, so the simulator
and the firmware share exactly one band-width formula.

### 7.3.3 Firmware port — DONE (2026-09-03)

Ported the algorithm validated in `ramp_assist.py` (§7.3 above) onto the
ESP32-S3 executor, in `profile_executor_ramp_assist.c`'s two new functions
(alongside the sec 7.1/7.2 lag/stretch functions already there):

- `ramp_assist_dwell_credit_tick(zone_runtime_t *z, bool ramping_now, bool
  lagging_now, float segment_target_c, float dt_s)` -- accrual, ported line
  for line from `ramp_assist.py`'s RampStep branch: `in_band = band_bottom
  <= actual_c < target_c`; `if lagging and in_band: credit += weight * dt`.
  `segment_target_c` is the profile segment's own final `target_c`, not the
  moving `s_exec.target_c` a ramp is still interpolating toward (mirrors
  `zone_active_target_c()`'s use of `step.target_c`). Calls `cone_table_
  band_bottom_c()`/`cone_table_heat_work_weight()` (both already firmware-
  side, §7.3.1/7.3.2); an out-of-range target (either function returning
  non-OK) banks nothing rather than crashing or guessing. **Always runs**,
  regardless of `ramp_assist_cfg_enabled()` -- same "accrue/report always"
  convention §7.1/7.4's sustained-lag fields already established, so an
  operator gets live visibility into what the feature would be doing with
  the flag off. Also fills an independent audit accumulator
  (`zone_runtime_t.dwell_credit_audit_s`) on its own separate `cone_table_
  heat_work_weight()` call, the firmware analogue of `ramp_assist.py`'s
  `credit_reference_heat_s` (the DEFECT 1 lesson above, ported so a future
  regression confined to the real accrual line is visible on the firmware
  side too, not just caught in the simulator). Same scope limit as the
  Python original, restated here because it is easy to oversell: both
  accumulators share the same weight function, band and gate, so this
  catches only an accrual/spend implementation slip between the two `+=`
  statements -- a wrong Ea, band width or weight function moves both
  together and this check stays silent. Neither is validated by it or by
  any other metric in either module.

- `ramp_assist_dwell_credit_spend(s_exec_state_t *ex, float nominal_dwell_s,
  bool assist_enabled)` -- spend, called once per dwell entry (both
  transition shapes: the normal ramp-reaches-target case and the
  `ramp_c_per_hr<=0` instant-jump case, `profile_executor.c`'s dwelling-
  transition code). Mirrors `ramp_assist.py`'s DwellStep branch:
  `spend = credit if apply_dwell_credit else 0.0`, clamped to
  `[0, nominal_dwell_s]`, and **every** active/non-faulted zone's
  `dwell_credit_s`/`dwell_credit_audit_s` is reset to 0 unconditionally
  (the reference implementation's own `z.credit_s = 0.0`, outside its
  `apply_dwell_credit` branch -- "spent once" whether or not the spend was
  actually applied). One firmware-specific adaptation, forced by an
  architectural difference from the simulator: this executor has exactly
  ONE shared `segment_elapsed_s`/`dwelling` pair across every active zone
  (`s_exec_state_t`), not `ramp_assist.py`'s independent per-zone dwell
  timers, so the single spend value applied to that shared timer is the
  **minimum** of every active, non-faulted zone's own banked credit --
  never any zone's alone. Conservative direction (matches `cone_table.h`'s
  own documented under-credit-is-safe stance): no zone is ever credited
  for heat work it did not itself accrue, at the cost of one heavily-
  lagging zone capping every other zone's payback for that dwell. Because
  this shared-timer/minimum-credit behaviour has no counterpart in
  `ramp_assist.py` (which models independent per-zone dwell timers and so
  never caps one zone's credit by another's), the simulator's validation
  of the accrual/spend arithmetic does NOT transfer exactly to this
  multi-zone capping behaviour -- it is only exercised by the firmware's
  own host tests (mutation 3 below), not by anything run in the
  simulator.

GATING, exactly as required: `ramp_assist_dwell_credit_tick()`'s accrual
(and its report-only fields) is unconditional; `ramp_assist_dwell_credit_
spend()`'s RETURN VALUE is the only gated piece -- `assist_enabled ==
false` always returns 0.0, so `profile_executor.c`'s dwelling-transition
code always records `s_exec.dwell_credit_applied_s = 0.0f` for that run,
and the dwell's own `ready_to_advance` check (`segment_elapsed_s >=
seg->dwell_min*60u - (uint32_t)dwell_credit_applied_s`) reduces to exactly
`segment_elapsed_s >= seg->dwell_min*60u` -- the pre-existing expression,
bit-identical, with the flag off. Proven by `ramp_assist_dwell_credit_
spend()`'s own gated-on-flag host test plus an adversarial mutation
(removing the `if (!assist_enabled) return 0.0f;` gate) that reliably
fails that same test -- see below; a full FreeRTOS-task-driven bit-
identical proof through the live tick loop was not attempted, because
`test_profile_executor_prestart.c`'s own header comment already documents
that this file cannot drive `profile_executor_run()`'s real control task
without a task harness it deliberately does not build (see that file's
"needs a whole task harness" note) -- so this proof is at the level of
the two functions that changed, not an end-to-end run.

Reporting (dashboard only, per the buffer budget below -- **not** added to
`GET /api/status`, which the plan flagged as having only ~129B headroom):
`profile_exec_zone_status_t.ramp_dwell_credit_s` (live per-zone banked
credit, always reported), wired into `GET /api/profile_exec`
(`dashboard_json.c`'s `append_zone_status_json()`, `control_fields==false`
shape only -- `/api/control` untouched). `dashboard_json.h`'s
`DASHBOARD_JSON_PROFILE_EXEC_BUF_SIZE` worst-case-per-zone figure was
472B (Phase 7a) -> 616B (§7.1/7.4's four lag fields) -> **648B** (one more
`"ramp_dwell_credit_s":-1234.56,` field, 32B: 8B value + 24B key/
punctuation), still against the 1024B/zone allowance -- >375B headroom
remains. `test_dashboard_json.c`'s `fill_worst_case_zone()`/worst-case
render test were extended with the new field so a future addition is
caught the same way §7.1/7.4's fields already are.

**Correction (2026-09-03, see §7.3.4): the paragraph above previously also
claimed `profile_exec_status_t.ramp_dwell_credit_applied_s` (the run-level
"last spend actually applied" figure) was wired into `GET /api/profile_exec`
here. It was not -- the field was assigned in
`profile_executor_status.c:257` and read into the handler's local `st`, but
`dashboard_http.c`'s `profile_exec_status_get_handler()` never actually
serialized it into the JSON. §7.3.4 below is the real fix and the real
buffer arithmetic for it.**

Host-side discrimination check (owner asked whether an equivalent to
`ramp_assist.py`'s `ScaleSweepDiscriminatesCreditErrorsTests` -- proving
its `credit_audit_pct` metric returns exactly `(SCALE-1)*100%` for a
0.5x/2x/3x-scaled credit -- is possible on the C side): concluded a direct
port of that SPECIFIC sweep does not fit cleanly, because the firmware's
`dwell_credit_audit_s` is not a percentage metric computed after the fact
against a whole simulated run -- it is a live, independent per-tick
accumulator compared directly to `dwell_credit_s` inside the SAME executor
instance, so there is no separate "scaled copy of the run" to sweep a
SCALE factor across without rewriting the accrual call site itself for
each sweep point. What IS built and landed instead, proven equally
discriminating by direct mutation (see below): the two accumulators are
asserted to match to float precision in the correct-code case
(`test_dwell_credit_tick_accrues_while_lagging_in_band`), and a mutation
confined to the real `dwell_credit_s` accrual line (weight dropped, raw
`dt_s` banked instead) reliably diverges the two -- i.e. this IS the
audit-catches-a-scaled-regression property, just checked directly rather
than via a synthesized percentage metric, since firmware has no
"percentage vs. a separately-run baseline" concept to compute one against.

MUTATION TESTING (per this task's own requirement -- every test proven to
actually fail before being trusted): four independent single-line
mutations were applied to `profile_executor_ramp_assist.c`, the affected
test(s) run, the real failure text captured, then reverted:

1. Accrual line `z->dwell_credit_s += w * dt_s;` -> `+= dt_s;` (raw
   seconds, unweighted -- the exact DEFECT 1 shape from `ramp_assist.py`'s
   own investigation above). Failed 4 checks, including the audit-vs-real
   divergence this independent accumulator exists to catch:
   `weight is in [0,1], so credit must be < raw dt_s (10.0)`,
   `the independent audit accumulator must match the real one exactly in
   healthy code`, `weight at the band bottom must be exactly 0.0`,
   `closer to target must earn MORE credit per tick than closer to the
   band bottom`.
2. `ramp_assist_dwell_credit_spend()`'s `if (!assist_enabled) return
   0.0f;` gate deleted. Failed exactly the gating test:
   `assist_enabled == false must apply ZERO seconds of credit to the
   dwell -- dwell timing must stay bit-identical with the flag off`.
3. The per-zone minimum selection (`z->dwell_credit_s < min_credit_s`)
   flipped to `>` (max instead of min). Failed:
   `must apply the SMALLER of the two active zones' credit, not the
   larger, and must ignore the inactive zone's smaller-still value`.
4. The `if (spend > nominal_dwell_s) spend = nominal_dwell_s;` clamp
   deleted. Failed:
   `spend must clamp to nominal_dwell_s, never exceed it (a negative
   resulting dwell_remaining_s would follow if it did)`.
5. (also run) The `!lagging_now` half of the accrual gate dropped. Failed
   the two zero-credit-while-not-lagging checks:
   `not lagging must bank nothing, regardless of position in band`,
   `audit accumulator must track the same zero`.

All five mutations were confirmed applied (`grep MUTATION`), built, and
run before being reverted; the reverted state was re-verified clean
(`grep MUTATION` empty, all 20 host-test executables green) before this
change was committed.

`build_kilnfw` (ESP32-S3 target, via the `kilnctrl` MCP server) succeeded:
"kilnfw-build: OK in 64.5s (219 log lines)", no new warnings or errors
attributable to this change.

**Not done in this pass:** §7.2 auto-stretch's own control behaviour
(instrumentation only per §7.2's own status), §7.4's warning surfaces
(banner/event-log/LCD) for the credit specifically, and §7.6's real-firing
validation. `dwell_credit_applied_s`/`ramp_dwell_credit_s` are new
plumbing those can build on, not a replacement for them.

### 7.3.4 Adversarial opus review of §7.3.3 — five defects fixed (2026-09-03)

An adversarial opus review of the §7.3.3 firmware port (commit `5312e14`)
found the arithmetic sound but flagged the tests as vacuous on the one
dimension that matters most (accrual magnitude) and one field as claimed-
but-not-actually-emitted. All five findings, and what was done about each:

**DEFECT 1 — all 11 tests passed even if the credit were 2x too large.**
The three tests that touched accrual magnitude
(`test_dwell_credit_tick_accrues_while_lagging_in_band`,
`test_dwell_credit_tick_zero_at_band_bottom_max_near_target`, and the old
`_carries_across_back_to_back_ramps`) only ever asserted loose `>`/`<`
relations, every one of which a uniformly 2x-scaled (or halved) weight
still satisfies. Fixed by pinning THREE independently hand-computed
weights (Arrhenius formula worked out by hand from `cone_table.c`'s
documented `rate(T) = exp(-Ea/(R*T))`, Ea=300000 J/mol, R=8.314 J/(mol*K),
T in Kelvin, min-max rescaled across the band) rather than by calling
`cone_table_heat_work_weight()` and recording what it returns: two
temperatures within the 613.05-626.1C band (target 626.1C) and one in a
DIFFERENT band (target 650.0C, band_bottom 624.15C) so a bug confined to
one cone-pair's band selection cannot hide behind only ever being tested
against one pair. See `test_profile_executor_prestart.c`'s dwell-credit
section header comment for the full derivation and
`test_dwell_credit_tick_accrues_while_lagging_in_band()`/
`test_dwell_credit_tick_second_temperature_pin_same_band()`/
`test_dwell_credit_tick_pin_in_a_different_band()` for the pins themselves.
Mutation evidence: doubling the accrual weight
(`z->dwell_credit_s += 2.0f * w * dt_s;`) failed all three pins (real
failure text captured in this task's report), and reverted clean.

**DEFECT 2 — the audit accumulator was a two-writer test of nothing.**
`zone_runtime_t.dwell_credit_audit_s` and its `w_audit` computation in
`ramp_assist_dwell_credit_tick()` called the identical
`cone_table_heat_work_weight()` with the identical arguments as the real
accrual line, so it could only ever catch a mutation confined to ONE of the
two duplicate `+=` statements — it was blind by construction to a wrong
Ea, a wrong band width, a wrong weight function, a wrong dt, or a wrong
in-band predicate, i.e. every shape DEFECT 1 could actually take. §7.3.3's
own writeup above (mutation #1) had described this as "the audit-catches-
a-scaled-regression property" — true of that one specific mutation
(unweighted `+= dt_s`, which happened to only touch the non-audit line),
not of the property in general, and this was exactly the kind of overclaim
the docs had already been corrected for once before. **Decision: deleted**
(`zone_runtime_t.dwell_credit_audit_s`, the `w_audit` computation, its
dashboard/API surface — there was none, it was never exposed — and its
reset in `ramp_assist_dwell_credit_spend()`), rather than kept and
re-documented. Rationale: its actual protective value (a copy-paste typo
between two duplicate lines calling the same function) is far smaller than
the false confidence its name implied, and real protection against DEFECT
1's actual failure shapes now comes from the independently hand-computed
pins above, which do not share any code path with the accrual line they
verify.

**DEFECT 3 — two untested gates, either one silently doubling or shifting
the credit.** (a) The `ramping_now` gate
(`profile_executor_ramp_assist.c`'s `if (!ramping_now || !lagging_now)
return;`) had no negative test — dropping the `!ramping_now ||` half would
let a zone bank credit DURING a dwell (spent at the NEXT dwell entry,
crediting one segment's dwell against a different segment's own lag), and
nothing in the suite would have gone red. (b) The in-band upper bound
(`z->actual_c < segment_target_c`) had no negative test — dropping it would
let an OVERSHOOTING zone bank at `cone_table_heat_work_weight()`'s
documented weight=1.0 clamp for `current_c >= target_c`, i.e. full-rate
credit for being too hot. Fixed: added
`test_dwell_credit_tick_not_ramping_earns_nothing()` and
`test_dwell_credit_tick_at_or_above_target_earns_nothing()`. Mutation
evidence: removing `!ramping_now ||` failed the first test (real failure
text captured, reverted); dropping the upper-bound clause from `in_band`
failed the second test's both assertions (real failure text captured,
reverted).

**DEFECT 4 — `ramp_dwell_credit_applied_s` was emitted nowhere.** Assigned
at `profile_executor_status.c:257` into `profile_exec_status_t.
ramp_dwell_credit_applied_s`, but read by nobody: `dashboard_json.c`,
`dashboard_http.c`'s `profile_exec_status_get_handler()`, `main_page.html`
and `telemetry_format.c` all left it out, despite this section (above)
claiming it was reported run-wide on `/api/profile_exec`. Fixed two ways:
(1) `dashboard_http.c`'s `profile_exec_status_get_handler()` now serializes
`"ramp_dwell_credit_applied_s":%.2f,` into the run-level (not per-zone)
part of the `/api/profile_exec` JSON, sourced from `st.
ramp_dwell_credit_applied_s`. Buffer arithmetic: the run-level fixed part's
prior documented worst case was 567B (see `dashboard_http.c`'s own sizing
comment above `profile_exec_status_get_handler()`); this field's
key+punctuation (`"ramp_dwell_credit_applied_s":` + trailing comma, 28-char
key + 2 quotes + colon + comma) is 32B, plus an 8B `"%.2f"` worst-case value
("-1234.56", same convention as every other float in that format string) =
40B more, bringing the fixed part to 567+40 = **607B**, still well under
the 960B fixed allowance (`DASHBOARD_JSON_PROFILE_EXEC_BUF_SIZE = 960 +
MAX31856_CHANNEL_COUNT * 1024`) that base covers — no `#define` change
needed. (2) `telemetry_log.c`'s `telemetry_log_task()` now attaches a
`"dwc=<N>s"` note (max 15 chars + NUL, fits `EVENT_LOG_NOTE_LEN`==16) to
the `EVENT_CODE_FIRING_DONE` flash event record when
`ramp_dwell_credit_applied_s > 0.0f`, so a firing whose dwell was
shortened leaves a permanent, post-hoc-readable artifact in the flash
event log (`log_http.c`'s `/api/logs/firing` route) — not just a live JSON field
that resets to 0.0 at the next run's start
(`profile_executor_run.c:355`). Not attached to `EVENT_CODE_FIRING_FAULTED`
(the fault_guard already carried in `arg` is the more useful number at
that specific transition; the credit figure stays visible live via
`/api/profile_exec` and the `last_run` breadcrumb until the next firing
starts). `GET /api/status` was deliberately NOT touched (per this section's
own earlier note, it had only ~129B headroom — adding to it risks the
truncate-into-500 failure mode `dashboard_json.h`'s buffer-sizing comments
already warn about).

**DEFECT 5 — a test for a state the executor cannot reach.** The old
`test_dwell_credit_tick_carries_across_back_to_back_ramps` called
`ramp_assist_dwell_credit_tick()` for two "ramp segments" back to back with
no `spend()` call in between and asserted credit carried forward. That
sequence is unreachable in the real executor: `profile_executor.c`'s
segment-stepping code calls `ramp_assist_dwell_credit_spend()` — which
unconditionally zeroes `dwell_credit_s` — every time a `ZONE_RAMP` segment
reaches its `target_c` and sets `dwelling=true`, **even when `dwell_min ==
0`** for that segment. So credit never actually carries between ramp
segments in firmware, contradicting `ramp_assist.py`'s simulator, whose
independent per-zone dwell timers have no such forced intermediate spend.
**Decision: kept the firmware's real behaviour (credit does NOT carry
across ramp segments) and deleted the test**, rather than reworking the
executor's segment-stepping to match the simulator. Reasoning: dwell
credit exists to pay back time on the segment whose OWN lag it was
measured against; carrying it into a later, unrelated segment's dwell
would be a materially different and unreviewed feature, not a bug fix.
This leaves the simulator and the firmware in disagreement on this one
point — noted here explicitly per this pass's instruction to decide
deliberately rather than silently, and left as a known, accepted gap
between the two rather than something either side needs to change.

**ALSO DOCUMENTED — the min-across-zones spend rule does nothing in the
realistic single-weak-zone case.** `ramp_assist_dwell_credit_spend()`
applies the MINIMUM of every active, non-faulted zone's own banked credit
(the conservative direction — see that function's own doc comment). In the
common real-world shape where exactly ONE zone lags and every other active
zone tracks its ramp on-rate, the lagging zone banks real credit (say
300s) while every healthy zone banks exactly 0 (never lagging, so
`ramp_assist_dwell_credit_tick()`'s gate never lets it accrue anything) —
the applied minimum is 0, and the feature shortens nothing. It only acts
when EVERY active zone lags at once. The pre-existing "uses minimum" test
only ever exercised 50 vs. 80 (both nonzero), which demonstrates "the
smaller wins" without showing how often that smaller value is exactly
zero in practice. `test_dwell_credit_spend_single_weak_zone_applies_nothing()`
now pins the realistic 0-vs-300 case explicitly. This is defensible as the
conservative choice (never credits a zone for heat work another zone
accrued but it did not) but must not be a silent surprise the first time
an operator notices a firing with one lagging zone got no dwell
shortening at all — hence this note.

Host tests: all 20 host-test executables built and passed after every fix
above (`build_host_tests.ps1`, run via bash per this repo's own "PowerShell
tool can silently no-op" caution), including
`test_profile_executor_prestart.c` at 740/740 checks (up from the
pre-review suite's count, six new tests added, one deleted). `build_kilnfw`
(ESP32-S3 target, via the `kilnctrl` MCP server) verified against these
changes too — see this task's own report for the exact build output.

### 7.4 Warning surfaces — DONE (verified 2026-09-03)

All three the owner asked for, all while ramp-lock is holding, are built and
committed:
- Web banner: `main_page.html`'s "Kiln is falling behind schedule" banner
  (commit `6c284c5`, wired to the richer per-zone fields in `1e03448`) reads
  `zones[].ramp_lag_sustained`/`ramp_lag_held_s`/`ramp_lag_commanded_rate_c_
  per_hr`/`ramp_lag_achieved_rate_c_per_hr` and, when `ramp_assist_enabled`,
  appends the run-level `ramp_stretch_segment_s`/`ramp_stretch_total_s`
  sentence ("This segment has been stretched Xm so far (Ym total this
  run)"). Debounced: the rich per-zone path shows the instant firmware's own
  30s-sustained gate (`EXEC_SUSTAINED_LAG_S`) flips true, undebounced again
  client-side; only the fallback path (an older board with no rich fields)
  keeps a 6s client debounce. `fetchProfileFeas()`'s fetch has a real
  `.catch()` that degrades to "no icon, no gate" rather than an eternal
  loading state.
- Event-log entry: `telemetry_log.c`'s per-zone edge tracking emits
  `EVENT_CODE_FIRING_RAMP_LAG_STARTED`/`_CLEARED` (event_log.h) with the
  actual commanded/achieved rate and held-duration numbers
  (`telemetry_format.c`'s `telemetry_ramp_lag_event_for_transition()`),
  independent of `ramp_assist_enabled`. `EVENT_CODE_FIRING_DONE` also
  carries the run's total dwell-credit-applied seconds in its note field
  when nonzero (sec 7.3's DEFECT 4 fix).
- LCD: `ui_page_home.c`'s `s_lag_notice` strip (commit `bd7e9c8`), worded as
  informational rather than alert-styled ("normal ramp-lock behaviour", not
  "SAFETY TRIP"), single sustained zone shows the same commanded/achieved/
  held numbers as the web banner, multiple zones falls back to a name list.
  Fits the 320x480 no-scroll constraint via `LV_LABEL_LONG_DOT` truncation
  rather than wrap/scroll — no separate page was needed since this is one
  short strip on the existing home page, not new content that pushed
  anything else off it.

Selection-time and start-time surfaces (owner: "a profile known to be
unachievable should also warn at SELECTION time... and again with a popup
when the firing is STARTED") are also built, reusing `profile_feasibility.c`'s
existing per-segment/per-zone verdict:
- Selection-time clickable icon: `main_page.html`'s `#profileFeasIcon` next
  to the profile picker (⚠ for a real too-fast/unreachable verdict, ⓘ for
  unassessed/untuned) opens `openFeasInfoPopup()` on click, and
  `profiles_page.html`'s catalogue cards carry the same `fz-too_fast`/
  `fz-unreachable` badge plus an expandable `<details>` caption
  (`FZ_WHY`) with the explanation text.
- Start-time popup: `proceedToStart()`'s call site is gated by
  `openFeasStartPopup()`, shown instead of the plain `confirm()` whenever
  the selected profile has a non-OK verdict — "Start ... anyway?" with the
  same explanation body, Cancel leaves the POST uncalled.

Verified this pass: all 21 KilnFW host-test executables build and pass
(`build_host_tests.ps1`, including `test_ui_page_home_graph.c`'s
`lag_notice_active`/debounce tests and `test_lag_banner.js`'s 19 web-banner
cases), `build_kilnfw` succeeds via the `kilnctrl` MCP server, and a live
mutation of `ui_page_home_graph.c`'s `ui_page_home_lag_notice_active()`
(forcing the rich-data branch to always return false) was caught immediately
by `test_ui_page_home_graph.c` ("FAIL ... rich data, zone sustained, tick 0:
shows immediately (no client debounce)"), then reverted.

### 7.5 `ramp_assist_enabled` setting — DONE (control surface only; §7.2/7.3 behaviour still not built)

`ramp_assist_cfg.c`/`.h` exist, kiln-wide (not per-zone), persisted (`kiln_nvs`
partition, `kiln_cfg` namespace, same pattern as `unit_pref.c`), default OFF.
Wired into `dashboard_http.c` (`GET /api/status`'s `ramp_assist_enabled`
field) and `diagnostics_http.c` (`GET`/`POST /api/ramp_assist`, alongside the
watchdog-panic toggle) — see `docs/WEB_UI.md`'s API reference for the wire
shape. `diagnostics_page.html` has the toggle, reading the board's actual
state and showing an explicit error (not a misleading "off") if the read
fails. Reachable from tooling as `ramp_assist_get_enabled`/
`ramp_assist_set_enabled` (`mcp_server_ramp_assist.py`, confirm-gated write),
and `config_presets.py`'s `ramp_assist_enabled` is now a REQUIRED top-level
preset field so every preset pins it explicitly — all four schema-valid
presets under `config_presets/` currently pin it OFF. Defaults to ON only
once validated on a real firing (§7.6) — that flip is a future change to this
module's default, not part of this pass. **Testing hazard:** if left enabled
during a tuning run or an A/B comparison, it silently changes ramps and
dwells mid-run and invalidates the measurement. Experiments must PIN the flag
explicitly rather than inherit whatever it defaults to — `run_queue.py`'s
preset-apply path now does this automatically.

**NOT part of this pass:** the flag has no consumer yet. §7.2's auto-stretch
and §7.3's dwell credit still need to be built and will read
`ramp_assist_cfg_enabled()` at decision time.

### 7.6 Validation before defaulting ON — cone-scale exercise (2026-09-03)

**See §7.6.1 for the credit-gate fix and the further 2026-09-03 accrual
extension (past the nominal ramp end) — both correct the table below, and
§7.6.1's own re-run table is the current, load-bearing set of figures.**

Simulator can check: auto-stretch never produces a target above `max_temp_c`
(hard-refusal path); dwell credit accrual/band arithmetic against known
cone-table inputs; ramp-lock interaction (assist must not fight the existing
lock). Simulator CANNOT check: whether the extrapolated max-ramp curve above
~80 °C (§3.7) matches a real kiln, or whether the credit's heat-work weight
tracks an actual ware load. A real firing is required before the default
flips to ON, specifically to observe those two.

**Why this pass exists:** every prior validation ran profile 7 (bisque-scale
schedules top out ~60 °C, below the lowest cone, 586 °C). The band widths,
the Arrhenius weight and the credit accrual had never actually run inside a
real cone band. This pass drives `ramp_assist.run_ramp_assist`/
`dwell_credit_parity` against `plant_sim.PhysicalKilnPlant` at bisque
(cone 04, 1062.8 °C), mid-fire glaze (cone 6, 1222.2 °C) and high fire
(cone 10, 1285.0 °C), including the half-cone band widths introduced by
05HALF/5HALF. **Every number below is simulator-only and model-dependent:**
`PhysicalKilnPlant`'s element wattage, wall insulation and thermal mass are
ASSUMED, not measured — no plant data exists above ~80 °C (§3.4/3.7). This is
evidence about the mechanism's *logic*, not about a real kiln's numbers.

**Schedules used** (single ramp + dwell, schedule replicated per zone as
everywhere else in this codebase): bisque 150 °C/hr to 1062.8 °C, 30 min
dwell; cone 6 150 °C/hr to 1222.2 °C, 15 min dwell; cone 10 100 °C/hr to
1285.0 °C, 15 min dwell.

**MIRROR-BUG CORRECTION (2026-09, supersedes the numbers first reported
here):** this section's first pass ran against `ramp_assist.DEFAULT_LAG_BAND_C`
while it was hard-coded to 3.0 °C with a comment claiming it mirrored
`EXEC_RAMP_LOCK_BAND_C` — actually `PROGRESS_BAND_C` (guard 1's arrival
band, `thermal_guard.c`), a different constant confused for it. The real
firmware value, `PROFILE_EXECUTOR_RAMP_LOCK_BAND_C`, is 25.0 °C — 8.3x
wider. §7.1 above carried the same "3 °C" mistake in its own prose, most
likely the actual source of the error. Separately, firmware commit 8f12449
("Fix ramp-lock hot-start stall: one-sided lock + guard 4 arming backstop")
changed the lock from a symmetric `abs(target_c - actual_c) > band` to a
ONE-SIDED `(target_c - actual_c) > band` — only a zone COLDER than
commanded locks; an overshoot no longer does. `ramp_assist.py` now mirrors
both corrections: `DEFAULT_LAG_BAND_C = 25.0`, pinned against
`profile_executor.h` by a new cross-language test
(`RampAssistLagBandCrossLanguageTest`, `test_ramp_assist.py`), and the
one-sided comparison. Every number below is the RE-RUN result at the
corrected band and lock direction; the original 3 °C/symmetric numbers are
struck through in spirit, not reproduced, to avoid anyone citing them
again.

**Item 1/2 — per-target, per-load credit/dwell-reduction, zone 0, at the
REAL shipped band (mass_mult 1x/2x/4x):**

| target | band width | credit_s (1x / 2x / 4x) |
|---|---|---|
| bisque (cone 04) | 15.85 °C | 0.0 s / 0.0 s / 0.0 s |
| cone 6 | 9.60 °C | 0.0 s / 0.0 s / — |
| cone 10 (100 °C/hr) | 12.50 °C | 0.0 s / — / — |

**Verdict: EXACTLY zero, not "small," at every load level measured — and
this is provable, not just observed.** The credit-accrual gate requires a
zone to be simultaneously (a) lagging by more than the 25 °C band and (b)
inside the credit band below the segment target. The credit band's width
is the local cone-table spacing halved; the WIDEST it is anywhere in the
whole Orton table (cone 022–14) is 25.85 °C, at cone 019 (677.8 °C, a
target none of these three scenarios use) — every other tabulated cone's
band is narrower than 25 °C outright. So "lagging (>25 °C off) AND inside
the credit band (8–18 °C wide at these three targets)" is a **structurally
near-empty set for realistic cone-scale targets**, independent of load,
rate, or the cross-zone stall discussed below — increasing mass_mult makes
a zone fall further behind, but by the time it is more than 25 °C behind
it has necessarily fallen well past its own 8–18 °C-wide credit band, not
into it. `ShippedDefaultBandNeverFiresTests`
(`test_ramp_assist_cone_scale.py`) pins this directly: bisque at 1x/2x/4x
mass and cone 6 at 1x/2x mass all earn exactly `0.0` credit_s, and cone 6's
time-in-band-while-lagging is exactly `0.0` s at 1x and 4x mass.

**Item 3 — mechanism-only re-run (proves the accrual/audit/load-scaling
logic is still correct when it IS given a band that lets it fire; NOT the
shipped default).** Because the real band leaves nothing to measure,
`ConeScaleMechanismRunsTests`/`LoadGrowsCreditTests`/`TimeInBandTests`/
`CreditAuditHoldsAtConeScaleTests` now pass an explicit, synthetic
`lag_band_c=3.0` override (documented at each call site, never the
default) to confirm the mechanism itself — accrual gate, `credit_audit_pct`
consistency, monotonic growth with load — is unaffected by the mirror fix:
bisque at a faster 600 °C/hr rate (150 °C/hr no longer produces any
lagging window worth measuring even at 3 °C, now that the lock is
one-sided — see below) earns 38.4 s of credit, 2.1% of its 1800 s nominal
dwell; `credit_audit_pct` reads 0.0000%; cone 6 credit_s grows from 1x to
2x mass; cone 6 time-in-band grows from 1x to 4x mass. **This is evidence
the underlying mechanism remains sound, not evidence it does anything at
the shipped default.**

**Finding: the cross-zone "stall" reported in the original pass was two
separate things, and one of them is now fixed at the source.** The
original pass found zones 1/2 pushed by cross-zone coupling ABOVE their
own frozen commanded target, read as "lagging" by the OLD symmetric lock
exactly like a genuine shortfall, freezing the commanded target and
computing a permanent zero hold duty. Firmware commit 8f12449 fixed this
directly: the lock is now one-sided, so an overshoot no longer locks a
zone at all. Re-run confirms this at cone scale — mutating `ramp_assist.py`
back to the old symmetric `abs(actual_c - z.commanded_c) > lag_band_c` at
`lag_band_c=3.0` (bisque, 150 °C/hr, 1x mass) reproduces the exact
original failure, `targets_reached == [True, False, False]`; the real
one-sided code gives `[True, True, True]` on the identical schedule. A
SEPARATE, genuine catch-up failure remains at extreme load — bisque zone 2
at 8x mass still does not reach target+dwell within a 150000 s cap under
EITHER lock form, confirming that stall is a real "too far behind to catch
up in the time simulated" condition, not an artifact of lock direction.

**Overall verdict, CHANGED from the original pass: at its real, correctly
mirrored default (25 °C band, one-sided lock), the dwell-credit mechanism
does essentially nothing at cone scale, at any load level tested (1x
through 4x mass) — not "small," exactly zero, and provably so from the
band-width arithmetic above, not merely an artifact of these three
schedules' rates. It is not harmful (still bounded, capped, never
negative) but it is also not "useful mainly under load" as the original
pass concluded — that conclusion was itself downstream of the 8.3x-too-
tight band. The mechanism-only re-run (Item 3) shows the underlying logic
is still correct when given a band that lets it fire, so this is a
magnitude/applicability finding, not a new implementation defect: as
shipped, dwell credit would need either a much wider band than
`PROFILE_EXECUTOR_RAMP_LOCK_BAND_C`'s 25 °C, or targets that happen to
land in cone table's rare >25 °C-band gaps (only cone 019, 677.8 °C, in the
entire Orton range), to ever engage at these three representative
targets.** This materially weakens the case for ever defaulting this
feature ON without either revisiting the band/gate design or confirming
on a real firing that ordinary cone-scale firings actually fall more than
25 °C behind their commanded ramp for extended stretches (which the
tracking data in Item 1 gives no reason to expect at 1x-4x simulated
load).

**What this pass cannot prove:** whether the credit's heat-work weight
tracks a real ware load (§7's own "load is not measurable" framing);
whether `PhysicalKilnPlant`'s assumed parameters resemble a real
cone-10-capable kiln at all — in particular, whether a real kiln under
real load ever falls 25 °C+ behind its commanded ramp the way this finding
would require for the feature to do anything; and, as always, anything
about a real firing. A real firing remains required before the default
flips to ON, and this pass's headline finding is itself a reason that
firing should specifically look for >25 °C tracking lag before the feature
is judged useful at all.

Reproducible tests (SUPERSEDED — see §7.6.1 below for the current module
contents): `tools/PcTools/tests/test_ramp_assist_cone_scale.py`
(new module, kept separate from `test_ramp_assist.py` per repo convention)
— `ShippedDefaultBandNeverFiresTests` pins the real-default zero-credit
finding; `ConeScaleMechanismRunsTests`/`LoadGrowsCreditTests`/
`TimeInBandTests`/`CreditAuditHoldsAtConeScaleTests` pin the mechanism
itself via an explicit synthetic band. Every test in this module and in
`test_ramp_assist.py` was negative-tested (mutate → observe the quoted
real failure → revert); see each test's docstring, and
`RampAssistLagBandCrossLanguageTest` for the new cross-language pin on the
band constant itself. Full suite re-run for this correction:
`tools/PcTools/.venv/Scripts/python.exe -m pytest tools/PcTools/tests` —
`test_ramp_assist.py` 21 passed, `test_ramp_assist_cone_scale.py` 9 passed.

### 7.6.1 The "EXACTLY zero" verdict above was the bug, not the finding — credit gate fixed (2026-09-03)

**This is a correction, not a footnote: §7.6's "structurally near-empty
set" verdict above was ITSELF the defect this section documents fixing —
it was accurately describing what the code did, but what the code did was
wrong.** The credit-accrual gate reused `lagging` (the ramp-lock's own 25 °C
`EXEC_RAMP_LOCK_BAND_C`/`lag_band_c` signal) as its "is the zone behind
schedule" condition. Because `in_band` requires being within half a cone
step of the segment target (8-18 °C at these three scenarios' targets, at
most 25.85 °C anywhere in the whole Orton table), and a ramp's commanded
setpoint never exceeds the segment target, a zone lagging by more than 25 °C
is *necessarily* already past its own credit band — the two conditions
could essentially never both hold. §7.6's arithmetic proving this was
correct; the owner's read of it was that the GATE, not the band width or
the mechanism, was the thing to fix.

**Owner decision (2026-09-03): credit accrues when a zone is BEHIND
SCHEDULE AT ALL** — `actual_c < the moving commanded setpoint`, no 25 °C
threshold — while still `in_band`. The ramp-lock's own 25 °C band is
UNCHANGED and keeps its existing meaning everywhere else (freezing the
schedule, auto-stretch's `lag_sustained` gate, the warning surfaces, the
event log) — only the credit accrual's own gate changed. Firmware:
`ramp_assist_dwell_credit_tick()`'s third parameter was renamed
`behind_schedule_now` (was `lagging_now`) specifically so the two meanings
cannot be silently reconflated again at a future call site;
`profile_executor.c` now computes it as `zone.actual_c < s_exec.target_c`
(the moving commanded target), not from the `ramp_lock_lagging_mask` bit.
Simulator: `ramp_assist.py`'s `run_ramp_assist` computes
`behind_schedule = actual_c < z.commanded_c` and gates credit on that
instead of `z.lagging`; `lag_band_c` still governs `z.lagging`/
`z.stretched_s` (the lock/stretch accounting) exactly as before, but has
no further bearing on credit.

**Re-run headline numbers, zone 0, REAL shipped code (no `lag_band_c`
override needed — it no longer affects credit at all):**

| target | mass_mult | credit_s | % of nominal dwell |
|---|---|---:|---:|
| bisque (1062.8 °C, 150 °C/hr, 30 min dwell) | 1x | 180.6 s | 10.0% |
| bisque | 2x | 0.0 s | 0.0% |
| bisque | 4x | 0.0 s | 0.0% |
| cone 6 (1222.2 °C, 150 °C/hr, 15 min dwell) | 1x | 112.8 s | 12.5% |
| cone 6 | 2x | 0.0 s | 0.0% |
| cone 6 | 4x | 0.0 s | 0.0% |
| cone 10 (1285.0 °C, 100 °C/hr, 15 min dwell) | 1x | 218.7 s | 24.3% |
| cone 10 | 2x | 218.5 s | 24.3% |
| cone 10 | 4x | 0.0 s | 0.0% |

At light load (1x for bisque/cone6, 1x-2x for cone10's slower rate), credit
is now real and material — 10-24% of the nominal dwell — a direct reversal
of §7.6's "exactly zero, structurally" verdict, which was an artifact of
the gate bug, not a property of the mechanism or the band widths. At
heavier load the figure collapses back to exactly zero, for a DIFFERENT and
still-genuine reason: heavier mass makes the zone fall further behind
during the ramp itself, so by the time the segment's commanded setpoint
reaches `step.target_c` (ending the ramp step and starting the dwell), the
zone's actual temperature often has not yet entered the half-cone-step
in-band window — it crosses into that window only after the dwell has
already begun, past `ramp_assist_dwell_credit_tick()`'s own `ramping_now`
gate (dwell-phase credit is a separate, not-yet-built accumulator; see this
section's own scope). This is a real model-dependent finding about the
ramp/dwell boundary, not a reappearance of the mutual-exclusion bug — the
gate itself no longer references the 25 °C band at all, and 1x load
directly proves the gate can and does fire.

**VOID, retracted by this correction:** any prior report anywhere in this
document or its commit history of "up to 30% of a dwell at 4x load" credit.
That figure traced back to a simulator-only defect (`ramp_assist.py`'s
in-band check computed against a hard-coded 3.0 °C band rather than
`cone_table.band_bottom_c`'s real bracketing-pair spacing — a stand-in for
the `DEFAULT_LAG_BAND_C` mirror bug described above, not the same defect,
but the same family: a synthetic override standing in for a real value
that was never re-checked against the shipped code path). It does not
describe any figure this document currently stands behind; the honest
replacement is the table above.

Coverage: `tools/PcTools/tests/test_ramp_assist_cone_scale.py` was rewritten
for this fix — `ShippedBandNowFiresAtLightLoadTests` pins the corrected,
now-nonzero 1x/2x-cone10 findings (negative-tested: reverting the gate to
`z.lagging and in_band` collapses credit back to exactly `0.0`, quoted in
the test's own docstring); `CreditCollapsesAtHeavierLoadTests` pins the
heavier-load zero findings; `CreditAuditHoldsAtConeScaleTests` re-confirms
the accrual/spend consistency check at cone scale under the real gate.
`test_ramp_assist.py`'s bench-scale `DwellCreditGateTests` was updated the
same way: `test_credit_is_zero_when_never_lagging` (which asserted a
well-tracked, never-locking ramp earns exactly zero credit) is now
INCORRECT under the fixed gate and was replaced with
`test_well_tracked_ramp_still_earns_credit_while_in_band`, which pins the
new, intended behaviour — an ordinary on-schedule ramp is still marginally
behind the moving setpoint on nearly every in-band tick, so it now earns
real credit, capped at the dwell's own nominal duration. Firmware:
`firmware/KilnFW/App/test/test_profile_executor_prestart.c`'s
`test_dwell_credit_tick_accrues_a_few_degrees_behind_not_25` is the
permanent regression pin (a zone 3.1 °C behind — nowhere near the 25 °C
lock band — must still bank credit; negative-tested by re-adding a 25 °C
threshold inside `ramp_assist_dwell_credit_tick()`, which produced the real
failure `credit gate has been re-tied to the 25C ramp-lock band`, quoted in
this task's own report). Full re-run:
`powershell.exe -ExecutionPolicy Bypass -File firmware/KilnFW/App/test/build_host_tests.ps1`
(21/21 host executables), `build_kilnfw` (OK), and
`tools/PcTools/.venv/Scripts/python.exe -m pytest tools/PcTools/tests`.

**Correction to the "heavier mass makes the zone fall further behind"
sentence above:** that phrasing described a SYMPTOM, not the actual gate.
The precise mechanism is two DIFFERENT thresholds, not one: the ramp step
formally ends (`s_exec.dwelling` flips true) once a lagging zone is back
within the WIDE 25 °C ramp-lock band, but credit's own `in_band` test
requires being within the much NARROWER half-cone-step band. At heavy
load the zone is still outside that narrower band at the exact tick the
wide band releases the ramp step, so it starts dwelling before ever
entering the credit band — two genuinely different thresholds, not one
gate blocking itself. See §7.3's new "Extended past the nominal ramp end"
paragraph, which fixes exactly this.

#### 7.6.1a Accrual extended past the nominal ramp end (2026-09-03) — re-run figures

Ran the full 9-scenario cone-scale scan (bisque/cone 6/cone 10 × 1x/2x/4x
mass) in the FOREGROUND, three scenarios per batch, against the extended
code (`ramp_assist_credit_should_accrue()` in firmware,
`ramp_assist.py`'s `DwellStep` branch in the simulator — see §7.3):

| target | mass_mult | credit_s (before §7.3 extension) | credit_s (after) | % of nominal dwell |
|---|---|---:|---:|---:|
| bisque (1062.8 °C, 150 °C/hr, 30 min dwell) | 1x | 180.6 s | 180.6 s | 10.0% |
| bisque | 2x | 0.0 s | **0.0 s** | 0.0% |
| bisque | 4x | 0.0 s | **0.0 s** | 0.0% |
| cone 6 (1222.2 °C, 150 °C/hr, 15 min dwell) | 1x | 112.8 s | 112.8 s | 12.5% |
| cone 6 | 2x | 0.0 s | **0.0 s** | 0.0% |
| cone 6 | 4x | 0.0 s | **0.0 s** | 0.0% |
| cone 10 (1285.0 °C, 100 °C/hr, 15 min dwell) | 1x | 218.7 s | 218.7 s | 24.3% |
| cone 10 | 2x | 218.5 s | 218.5 s | 24.3% |
| cone 10 | 4x | 0.0 s | **0.0 s** | 0.0% |

**Heavy-load credit is still exactly zero, plainly — the extension does
not change a single figure in this table, and that is the correct,
honest result for these specific schedules, not a failure of the fix.**
Every scenario above is exactly one ramp segment followed by exactly one
dwell segment (`RampStep` + `DwellStep`, matching the real 3-segment
firing shape this scan approximates). §7.3's extension lets credit keep
accruing once dwelling begins, but that credit is only ever SPENT at the
NEXT dwell entry (`ramp_assist_dwell_credit_spend()` is called exactly
once per dwell occurrence, at its own start) — by construction, the same
non-circularity guarantee §7.3 documents. When a dwell is the LAST segment
in the schedule, as every scenario here is, there is no next dwell
occurrence left for that in-dwell accrual to ever be spent against, so it
is carried in `dwell_credit_s`/`z.credit_s` until the run ends and then
simply discarded — the same safe "unspent credit is lost, never negative"
direction `ramp_assist_dwell_credit_spend()`'s own doc comment already
describes for ordinary end-of-run credit. **Confirmed the mechanism itself
does work when there IS a following dwell to spend against:** a synthetic
two-`DwellStep` schedule at cone 6/4x mass (`DwellStep(15.0),
DwellStep(15.0)` back to back at the same target) banks ~0.2 s of credit
during the first dwell occurrence and spends it against the second —
small, because this synthetic case starts the second dwell already close
to on-target, but nonzero, proving the extension is live and non-vacuous.
A real multi-segment profile (ramp→dwell→ramp→dwell, the shape §7's
worked examples actually use) is the case this extension is FOR — this
scan's single-ramp-into-terminal-dwell shape happens not to exercise it,
which is a property of the test schedule, not of the fix.

**A real bug this extension exposed and fixed in `dwell_credit_parity()`
(simulator only, no firmware equivalent — the firmware audit path does
not have this accumulator):** `credit_audit_pct`'s independent audit
accumulator (`z.credit_reference_heat_s`) used to accrue for the WHOLE
run, never reset, while `credit_applied_s` only ever reflects credit that
was actually spent. Before the extension the two were always equal for a
terminal single-dwell schedule (all accrual happened during the ramp, all
of it got spent at the one dwell). After the extension, `credit_reference_
heat_s` also grows during a terminal dwell's own in-dwell accrual — heat
work that (per the paragraph above) legitimately never gets spent — so
the two diverged for a reason that has nothing to do with an accrual-
formula defect, and `test_credit_audit_pct_is_zero_at_cone_scale` failed
with `75.4961743744046 not less than 0.5` at bisque 1x. Fixed by adding
`_ZoneState.credit_reference_applied_s`, reset in lockstep with
`credit_s` at every dwell entry and accumulated the same way
`credit_applied_s` is — `credit_audit_pct` now compares `credit_applied_s`
against this "applied" variant instead of the raw whole-run accumulator.
Re-run: all 7 `test_ramp_assist_cone_scale.py` tests pass, all 21
`test_ramp_assist.py` tests pass (one of which —
`test_credit_audit_pct_discriminates_scale_errors` — asserts an exact
source-text occurrence count of the real accrual line and needed updating
from 2 to 3, since the extension added a second, textually identical real
accrual statement in the `DwellStep` branch), all 17
`test_ramp_assist_http_client.py` tests pass.

Full re-run for this extension:
`powershell.exe -ExecutionPolicy Bypass -File firmware/KilnFW/App/test/build_host_tests.ps1`
(21/21 host executables, including the two new tests above), `build_kilnfw`
(OK, 26.5s), and `tools/PcTools/.venv/Scripts/python.exe -m pytest
tools/PcTools/tests/test_ramp_assist.py tools/PcTools/tests/test_ramp_assist_cone_scale.py tools/PcTools/tests/test_ramp_assist_http_client.py`
(45/45 passed).

#### 7.6.2 Re-run against REALISTIC MULTI-SEGMENT schedules (2026-09-03)

**Why this re-run exists:** every table above (§7.6, §7.6.1, §7.6.1a) drives
a single `RampStep` + a single TERMINAL `DwellStep` — a real firing is not
shaped like that. §7.6.1a's own "confirmed the mechanism itself does work"
paragraph used a SYNTHETIC two-`DwellStep` schedule to prove credit can
carry from one dwell to a later one; this section replaces that synthetic
proof with schedules built from this repo's own shipped profile catalogue
(`firmware/KilnFW/App/drivers/profiles_builtin_table.inc`, sourced from
digitalfire.com/schedule) — bisque from `BQ1000` ("Plainsman Electric
Bisque"), cone 6 from `C6DHSC` ("Plainsman Cone 6 Drop-and-hold, Slow
Cool", cool-down leg dropped — no forced-cooling model exists in this
simulator), cone 10 from `C10RPL` ("Plainsman Cone 10R Firing") with a
standard 150 °C/30 min candle added (no shipped cone-10 profile in this
catalogue pairs a candle with a single soak). Each schedule's FINAL target
is snapped to the existing 1062.8/1222.2/1285.0 °C figures so the two
tables stay directly comparable; every other segment is the shipped
profile's own value, unmodified. Full sourcing and code:
`tools/PcTools/tests/test_ramp_assist_cone_scale.py`'s "RE-RUN AGAINST
REALISTIC MULTI-SEGMENT SCHEDULES" docstring section.

**Headline numbers, zone 0, 1 run per cell, REAL shipped code:**

| schedule | mass_mult | total banked (spent + discarded) | spent (applied to the final dwell) | discarded (never spent) | spent as % of that dwell |
|---|---|---:|---:|---:|---:|
| bisque (candle 121 °C/60 min, 945 °C, final 1062.8 °C/30 min) | 1x | 1160.8 s | 619.4 s | 541.5 s | 34.4% |
| bisque | 2x | 153.3 s | 153.3 s | 0.0 s | 8.5% |
| bisque | 4x | 425.9 s | 1.6 s | 424.3 s | 0.1% |
| cone 6 (candle 121 °C/60 min, 1148 °C, final 1222.2 °C/15 min) | 1x | 248.2 s | 105.4 s | 142.8 s | 11.7% |
| cone 6 | 2x | 0.0 s | 0.0 s | 0.0 s | 0.0% |
| cone 6 | 4x | 53.1 s | 0.0 s | 53.1 s | 0.0% |
| cone 10 (candle 150 °C/30 min, 980 °C, final 1285.0 °C/15 min) | 1x | 406.5 s | 406.5 s | 0.0 s | 45.2% |
| cone 10 | 2x | 406.2 s | 406.2 s | 0.0 s | 45.1% |
| cone 10 | 4x | 19.6 s | 0.0 s | 19.6 s | 0.0% |

"Spent" and "discarded" are reported separately, deliberately not netted —
conflating banked-and-discarded heat-work with credit that actually
shortened a dwell would overstate the feature.

**What multi-segment structure changes, and what it does not.**
`cone_table.CONE_TABLE`'s floor is 586.1 °C (cone 022): every candle
segment in all three schedules above (121 °C, 150 °C) sits below it, so
the credit gate's `in_band` check raises `ConeTableError` there and no
credit accrues or is owed during the candle — the candle dwell itself is
always entered with zero banked credit and receives a zero spend. The
intermediate ramp segments (945/1148/980 °C) ARE inside the cone table's
range and DO contribute banked credit that carries forward into the final
ramp+dwell — that is the entire reason the multi-segment "spent" figures
differ from the single-segment table's — but there is still only ONE
dwell inside cone-table range in any of these three shipped-catalogue
schedules, so there is still only one place for credit to ever be spent.
This is an honest property of real bisque/cone/glaze recipes (they candle
cold, below the range the credit band's cone-table lookup can even
evaluate), not a limitation of the test schedules chosen.

**Heavy-load verdict — the owner's motivating case (a kiln tuned empty
then fired full): UNCHANGED from §7.6.1a's single-segment table.** At 4x
mass, spent credit is 1.6 s / 0.0 s / 0.0 s (bisque/cone 6/cone 10)
against 1800/900/900 s final dwells — 0.1%, 0.0%, 0.0% of the dwell it was
applied to. Real credit DOES accrue under heavy load (424.3 s at bisque
4x, 53.1 s at cone 6 4x, 19.6 s at cone 10 4x, the "discarded" column) —
it is not that nothing happens — but under heavy load that credit accrues
mostly during the terminal dwell itself (§7.3's extended-past-ramp-end
accrual), after the one dwell it could have shortened has already been
sized, so it is banked and then discarded when the schedule ends.
Realistic multi-segment structure does not give that credit anywhere else
to go, because only one dwell in each of these schedules sits inside the
cone table's covered range. **The dwell-credit feature does not
materially help a loaded kiln**, on either the single-segment or the
realistic multi-segment schedules measured — a clear negative, consistent
with §7.6.1a, not a magnitude quibble introduced by the simpler test
shape.

**Statistical discipline:** every cell above is exactly ONE simulator run
(`run_ramp_assist`/`PhysicalKilnPlant` are deterministic given `mass_mult`;
there is no stochastic element to average over — same discipline as every
other table in this section). None of these nine runs newly cross the
coupled hold/climb solve's known ~62 °C feasibility ceiling (§ referenced
throughout this document) in any way that differs from the single-segment
table already in §7.6.1a — that ceiling is a bench-rig constraint;
`PhysicalKilnPlant` operates in a separate, unmeasured-above-80 °C regime
for all of these runs, as every table in this section already flags.

**Recommendation, not a firmware change (out of scope for this pass):**
because real recipes candle well below the cone table's floor, giving
dwell credit a second real spend opportunity under load would require
either lowering `cone_table.CONE_TABLE`'s floor to cover candling
temperatures (Orton has no published cone below 022/586.1 °C, so this
would need a different weighting model entirely below that point) or
accepting that, for real recipes, this feature can only ever help the
single dwell nearest the final target. Reported for the owner's
consideration; no firmware file was touched to produce this section.

Coverage: `tools/PcTools/tests/test_ramp_assist_cone_scale.py`'s
`RealisticMultiSegmentLightLoadTests`/
`RealisticMultiSegmentHeavyLoadDoesNotHelpTests`/
`RealisticMultiSegmentCreditAuditHoldsTests` (5 new tests, all
negative-tested — see each class's own docstring) pin this section's
table; the single-segment classes (`ShippedBandNowFiresAtLightLoadTests`,
`CreditCollapsesAtHeavierLoadTests`, `CreditAuditHoldsAtConeScaleTests`)
are kept unchanged as the labelled single-segment comparison baseline, not
superseded. Full re-run: `tools/PcTools/.venv/Scripts/python.exe -m
pytest tools/PcTools/tests/test_ramp_assist_cone_scale.py` (14/14 passed,
~9.5 minutes wall clock — this module simulates real hours of firing per
scenario, see its own RUNTIME NOTE) and `tools/PcTools/tests/
test_ramp_assist.py` (21/21 passed, unaffected — `ramp_assist.py` itself
was not modified for this section).

#### 7.6.2a BOUNDED in-dwell dwell credit (owner decision, 2026-09-03)

**The owner's decision, following §7.6.2's "does not materially help" and
"recommendation" paragraphs: let credit shorten the dwell it was earned in,
but BOUNDED so it can never run away.** §7.6.2 measured that heavy-load
credit accrues mostly during the terminal dwell itself (§7.3's extended-
past-ramp-end accrual) and is then discarded, because commit `0402ecb`
deliberately froze the spend snapshot at dwell entry so a dwell could never
shorten itself. This section relaxes that freeze — the two bounds below
are what make that relaxation safe, and are the deliverable this section
documents, not the feature.

**Bound 1 — a fixed cap on the total reduction.** `EXEC_DWELL_CREDIT_MAX_
FRACTION` (firmware: `profile_executor_internal.h`; simulator: `ramp_assist.
py`'s `DWELL_CREDIT_MAX_FRACTION`) = **0.5**. No combination of the frozen
entry-snapshot spend and any amount of in-dwell top-up accrued afterward
may ever reduce a dwell's timer by more than half its nominal duration —
computed once, at dwell entry, as `nominal_dwell_s * 0.5`, and never
recomputed upward for the life of that dwell occurrence. **Why 0.5 is safe
and why it keeps "N minutes of timer, not a soak" true:** at least half of
every dwell's planned wall-clock length is always honored regardless of how
much heat-work credit is banked, however implausibly large — a dwell can
degrade toward "half as long as planned" in the worst case, never toward
"ends the instant the zone arrives." That is what distinguishes a
(bounded-)shortened TIMED HOLD from a bare "wait for temperature" soak: a
soak has no wall-clock floor at all, and this cap guarantees one always
exists. 0.5 was chosen, not derived, against the measured heavy-load
numbers below (bisque 1x is the one case that hits it, at exactly 900.0 s
of a 1800 s dwell) — generous enough to matter at the load levels this
section measures, small enough that the floor is still substantial.

**Bound 2 — never before the zone reaches target.** `ramp_assist_dwell_
target_reached()` (firmware, `profile_executor_ramp_assist.c`) /
the `target_reached` computation in `ramp_assist.py`'s `DwellStep` branch:
true only once every active, non-faulted zone's `actual_c` has reached the
dwell's own held target (an invalid reading counts as NOT reached — never a
free pass). A credited early exit is honored ONLY when this is true, in
addition to Bound 1's threshold having been reached; the pre-existing
unconditional "elapsed >= nominal_dwell_s" fallback is untouched, so a run
with no credit (or `ramp_assist_cfg_enabled() == false`) stays exactly as
it was before this section — credit only ever grants an EARLIER exit
option, never removes the guaranteed one, and a dwell that never reaches
target still ends at the full nominal duration rather than hanging.

**Together, these make the exact hazard `0402ecb`'s freeze existed to
prevent — unbounded self-shortening — impossible:** the earliest a credited
exit can occur is bounded below by `nominal_dwell_s * (1 - 0.5)`, and no
exit at all is possible before Bound 2 is satisfied, however much credit is
banked. Firmware: `test_dwell_credit_total_spend_binds_at_cap` (the cap
proof — negative-tested by disabling the clamp in `ramp_assist_dwell_
credit_total_spend_s()`, which produced the real failures `entry_applied_s
(250) + live top-up (500) is 750, far more than cap_s (300)` and `total_
spend_s must never exceed cap_s`), `test_dwell_target_reached_false_until_
every_active_zone_arrives` / `test_dwell_target_reached_invalid_reading_
counts_as_not_reached` (Bound 2 — negative-tested by disabling the gate in
`ramp_assist_dwell_target_reached()`, which produced `must be false --
zone 1 has not yet reached target_c` and `an invalid reading must never be
grounds to end a dwell early`), and `test_dwell_credit_runaway_self_
shortening_still_impossible` (both bounds together, across 1000 simulated
ticks of absurd runaway accrual — negative-tested the same way, producing
`even AT the collapsed floor, the dwell must not be allowed to end while
target_reached is false`) — all in
`firmware/KilnFW/App/test/test_profile_executor_prestart.c`. Simulator:
`CreditExceedsDwellTests`/`BisqueLightLoadCapBindsTests` in `tools/PcTools/
tests/test_ramp_assist.py` and `test_ramp_assist_cone_scale.py`
(negative-tested by disabling the same clamp in `ramp_assist.py`'s
`total_spend_s` line, producing `bisque 1x credit_s must be capped at
exactly ... (900.0 s), not the far larger 1227.5 s banked`).

**Re-run against the same realistic multi-segment schedules §7.6.2's table
used (bisque/cone 6/cone 10 × 1x/2x/4x mass), banked/SPENT/discarded and
spent as a percentage of the dwell it applied to, alongside §7.6.2's own
figures for direct comparison:**

| schedule | mass_mult | banked | SPENT (§7.6.2, frozen-only) | SPENT (this section, bounded in-dwell) | discarded | spent % of that dwell (this section) |
|---|---|---:|---:|---:|---:|---:|
| bisque | 1x | 1227.5 s | 619.4 s | **900.0 s (capped)** | 327.5 s | 50.0% |
| bisque | 2x | 153.3 s | 153.3 s | 153.3 s | 0.0 s | 8.5% |
| bisque | 4x | 425.9 s | 1.6 s | **425.9 s** | 0.0 s | 23.7% |
| cone 6 | 1x | 342.4 s | 105.4 s | 342.4 s | 0.0 s | 38.0% |
| cone 6 | 2x | 0.0 s | 0.0 s | 0.0 s | 0.0 s | 0.0% |
| cone 6 | 4x | 53.1 s | 0.0 s | **53.1 s** | 0.0 s | 5.9% |
| cone 10 | 1x | 406.5 s | 406.5 s | 406.5 s | 0.0 s | 45.2% |
| cone 10 | 2x | 406.2 s | 406.2 s | 406.2 s | 0.0 s | 45.1% |
| cone 10 | 4x | 19.6 s | 0.0 s | **19.6 s** | 0.0 s | 2.2% |

(Single-segment, non-multi-segment shipped-target scenarios show the same
pattern — see `BisqueLightLoadCapBindsTests`/`CreditCollapsesAtHeavierLoad
Tests` in `test_ramp_assist_cone_scale.py` for those figures; bisque single-
segment 1x also binds the cap at exactly 900.0 s of 1800 s.)

**Verdict on the owner's motivating question (a kiln tuned empty then fired
full): YES, this now materially helps, reversing §7.6.2's "does not
materially help" verdict for exactly the heavy-load cells that verdict was
about.** At 4x mass, spent credit went from 0.1%/0.0%/0.0% (bisque/cone
6/cone 10, §7.6.2's frozen-snapshot figures) to **23.7%/5.9%/2.2%** — the
in-dwell accrual that used to be banked and then discarded (the "discarded"
column above collapses to 0.0 s everywhere except 1x, where the CAP is now
what limits it, not the freeze) is now spent, bounded, against the same
dwell it was earned in. bisque is the standout case both light and heavy:
23.7% at 4x is a genuinely material reduction of a 30-minute terminal
dwell. cone 6 at 4x stays comparatively small (5.9%) because, per §7.6.2's
own "what multi-segment structure changes" paragraph, this specific
target/mass combination simply banks less real heat-work credit in the
first place — Bound 1's cap is not what limits it (53.1 s is nowhere near
the 450 s cap that dwell's 900 s nominal duration allows). The
recommendation in §7.6.2 (a second cone-table floor extension to cover
candling temperatures) remains unbuilt and is now lower-priority: the
terminal dwell — the one dwell every shipped recipe's candle-below-586.1°C
shape guarantees credit will accrue into — is the one this section's fix
makes spendable.

Full re-run:
`powershell.exe -ExecutionPolicy Bypass -File firmware/KilnFW/App/test/build_host_tests.ps1`
(21/21 host executables — `main`, an unrelated executable owned by a
concurrent session's in-flight edits to `uart_owner.h`/`uart_protocol.h`/
`gpio_probe.c`, intermittently failed to build during this pass; none of
those files were touched here and `test_profile_executor_prestart`'s own
4768/4768 checks passed every run), `build_kilnfw` (OK), and
`tools/PcTools/.venv/Scripts/python.exe -m pytest tools/PcTools/tests/
test_ramp_assist.py tools/PcTools/tests/test_ramp_assist_cone_scale.py`
(21/21 and 15/15 passed respectively, run in foreground batches small
enough to fit the tool's per-call timeout — the full multi-segment scan
takes roughly 9-13 minutes wall clock).

## 2026-09-03: ramp-lock hot-start stall (confirmed executor defect, fixed)

**The defect (owner-confirmed via adversarial code review).**
`profile_executor.c`'s ramp-lock decision (`~line 358`, inside
`executor_task_entry()`'s control tick) used
`fabsf(actual_c - target_c) > EXEC_RAMP_LOCK_BAND_C(zi)` to decide whether a
zone was "lagging" and should hold the shared ramp setpoint. `fabsf` makes a
zone N degrees too HOT bit-identical to one N degrees too COLD — both froze
`s_exec.target_c`/`s_exec.segment_elapsed_s` at the ramp-stepping gate
(`~line 439`, `} else if (lock_ok) { ... }`). The band is **25C**
(`EXEC_RAMP_LOCK_BAND_C` → `PROFILE_EXECUTOR_RAMP_LOCK_BAND_C`,
`profile_executor.h`) — not the 3C `PROGRESS_BAND_C` some docs and the
simulator's mirror have used; that constant is guard 1's separate arrival
band (`thermal_guard.c`).

**The reachable failure.** `baseline_target_c` (`profile_executor_run.c`)
seeds from the FIRST ACTIVE zone's reading on a cold start, or the COOLEST
active zone's on a warm start — never the hottest. Re-firing ~40 minutes
after a previous run, with one zone (say z0) back near ambient and another
(z2) still 50C+ above it: `baseline_target_c` comes from z0 (or the
coolest), so `target_c` starts low and z2 is immediately >25C hot. Every
other escape this executor has was absent for that direction: guards 1/2/7
all gate on `commanded_duty >= PROGRESS_DUTY_MIN` (0.5) or `> 0`, and a hot
zone commands none; guard 4 (drift) only armed after
`at_setpoint_window_active` had ever latched true, which a zone that starts
hot and only passively cools never does. Result: the whole firing's
schedule silently freezes — `state` stays `PROFILE_EXEC_RUNNING`, nothing
logs a fault — until the hot zone cools 25C on its own.

**Fix, both halves (owner's chosen approach — neither alone is sufficient):**

1. **One-sided lock** (`profile_executor.c`, the loop building
   `lock_ok`/`lagging`): changed to
   `(s_exec.target_c - s_exec.zones[zi].actual_c) > EXEC_RAMP_LOCK_BAND_C(zi)`.
   Only a zone COLDER than target by more than the band holds the lock now.
   `!sensor_ok[zi]` still holds it unconditionally, unchanged — an invalid
   reading says nothing about direction. Rationale: the lock exists to stop
   the setpoint outrunning a zone that CANNOT KEEP UP, which is inherently
   one-sided. Holding the setpoint back for a zone that is already too hot
   doesn't help it (it can only passively cool; a frozen setpoint changes
   nothing about that) and actively denies it the rising setpoint that
   would let it reconverge from above.

2. **Guard 4 arming fix** (`thermal_guard.c`/`.h`): a zone that never once
   settles within `DRIFT_HYSTERESIS_C` (25C) of setpoint used to leave
   `at_setpoint_window_active` permanently false, so the sustained-excursion
   clock could never start — guard 4 was structurally inert on exactly the
   hot-start case above. Added `idle_elapsed_s`, accumulated only while
   `commanded_duty < progress_duty_min` (the same threshold guard 1 uses,
   i.e. the zone is NOT actively trying to heat); the guard now also arms
   once that reaches `DRIFT_PERIOD_S` (600s), even without ever settling.
   **Why gated on duty, not plain wall-clock time since run start:** an
   earlier version armed on wall-clock time alone and false-tripped this
   repo's own `test_closed_loop.c` 4-hour closed-loop host test (a
   completely healthy, `duty≈1.0` the whole way, cold start against
   `sim_plant.c`'s default thermal mass) at ~1180s — a heavy/well-insulated
   kiln can legitimately take far longer than one `DRIFT_PERIOD_S` to first
   close a 25C gap while genuinely trying the entire time. Gating the clock
   on duty means it never accumulates for a zone that's actively climbing
   (guard 1's own, much shorter 300s no-progress window is what would catch
   a truly stuck-but-still-commanded zone), and only accumulates for a zone
   commanding little or no heat while outside the band — exactly the
   hot-start shape. Once armed, a zone back inside the band still resets
   the sustained-excursion clock as before; only a zone BOTH outside the
   band AND idle for two full `DRIFT_PERIOD_S` (one to arm, one sustained)
   trips.

**`baseline_target_c` seeding (item 4 — NOT changed this pass).** Considered
seeding from the HOTTEST active zone instead of first-active/coolest.
Recommendation: **do not**, without further work:
- The one-sided lock fix already neutralizes the defect that motivated the
  question — a hot zone no longer freezes the shared lock regardless of
  which zone `baseline_target_c` comes from, so the reachable failure above
  is closed by items 1+2 alone.
- Seeding from the hottest zone risks landing `baseline_target_c` ABOVE
  segment 0's own `target_c` whenever the profile's first segment target is
  below a currently-hot zone's reading (e.g. a modest opening dwell target
  while one zone is still warm from a previous firing). The ramp-stepping
  code picks `direction` from `seg->target_c >= s_exec.target_c` on tick 1
  — if the seeded baseline already exceeds the segment target, `direction`
  flips negative and `reached` can go true on the very first tick,
  collapsing an intended ramp-up into an instant jump for every other
  (legitimately cooler) zone. This is untested, unvalidated, and not
  something this pass's fix requires.
- It also breaks the symmetry warm-start's own COOLEST convention was
  chosen for (`profile_executor_run.c`'s Q4 comment: "preferring the
  coolest one means warm-start can only ever skip work every active zone
  agrees is already done"). Hottest is the opposite bias — it would seed a
  target every OTHER active zone might not agree is already reached.
- No test in this repo exercises this seeding choice's interaction with
  warm start, the max-temp feasibility refusal at run start, and the
  ramp-direction math all at once; changing it blind is out of scope for a
  task that asked for a narrow, verified fix.

**Validation status: UNVALIDATED ON HARDWARE.** `ramp_lock_held` has never
been observed true in any capture in this repo — every bench firing on
record tops out around 70C, well within a single zone's own settle time,
and multi-zone hot-start re-fires have not been run on hardware at all.
This fix (both halves) is proven only by host tests against a hand-written
mirror of the production control-tick logic (`profile_executor.c`'s
ramp-lock/segment-stepping code lives directly inside
`executor_task_entry()`'s `for (;;) { vTaskDelay(...); ... }` body — a real
FreeRTOS task loop with no seam to call one tick at a time from a host test
without restructuring the module, which this task was not asked to do) and,
for guard 4, real calls into `thermal_guard.c` (which IS a pure, directly
testable function). See `firmware/KilnFW/App/test/test_ramp_lock_onesided.c`
(new file, mirror + reproduction) and the guard-4 arming tests added to
`firmware/KilnFW/App/test/test_thermal_guard.c`. The hot-start regime this
fix targets remains unexercised on real hardware.

## 8. A/B campaign ambient-confound protocol (2026-09-03)

Owner's binding constraint: **no cooling fan** will be added, and experiments
must be designed so ambient temperature is not a large factor. Differences
below 0.5C are not actionable — no experiment should be designed whose whole
effect size is sub-0.5C.

**The problem.** Profile 7 starts from room/enclosure ambient and its first
ramp is scored from there, so the score is directly exposed to ambient
drift. Passive cooling asymptotes rather than returning to a fixed start (a
firing raises working ambient ~1.4-1.5C; measured 29.83/29.95/29.99C at
16:30 vs 29.88/29.96/30.04C at 16:58 — no progress in 28 minutes, cold
junctions rising). Consequence measured on this rig: a six-firing campaign
yielded n=1 usable pair, and a paired A/B stalled 75 minutes still unable to
match. `fit_start_temp_sensitivity` (§ above, `pid_ab_compare.py`) puts the
per-zone sensitivity of `iae_normalized_whole_c` to start temperature at
0.009-0.133C per 1C of start-temperature drift, fit against the checked-in
6-run repeat set.

**Recommended design: prepend a stabilisation hold, then score only after
it.** Ramp to a fixed setpoint (48C — comfortably above the ~28-31C ambient
this bench has shown, plus the ~1.4-1.5C a firing itself adds; well inside
the coupled hold solve's ~60C validity ceiling, infeasible above ~62C; far
below profile 7's ~70C top), hold until settled, and score only the
segments after that hold. Implemented as:

  * `tools/PcTools/src/kilnctrl/profile_stabilization.py` —
    `prepend_stabilization_hold(segments, ...)` inserts one ZONE_RAMP
    segment (target_c=48, ramp_c_per_hr=300, dwell_min=45 by default) ahead
    of an arm's existing segments, unmodified otherwise. **No firmware
    change of any kind is needed**: `profile_segment_t`
    (`profiles_http.h`) already expresses "ramp to a target, hold N
    minutes" — a stabilisation hold IS an ordinary ZONE_RAMP segment, so
    this is a profile-definition change, not new executor machinery. (The
    one thing the executor genuinely lacks is a *settle-to-tolerance* dwell
    — `dwell_min` is a fixed duration, not a criterion — so the 45-minute
    default is a deliberately generous upper bound, to be tightened once
    real stabilised captures exist; see that module's docstring.)
  * `tools/PcTools/src/kilnctrl/pid_ab_compare.py` — `min_segment_index`
    added to `compute_zone_metrics` / `compute_run_metrics` /
    `fit_start_temp_sensitivity` / `compare_runs` (default 0, existing
    behaviour unchanged). Passing `STABILIZATION_SEGMENT_INDEX` (1) excludes
    the prepended hold from every metric — the "whole" window, all
    per-segment dicts, ramp-to-dwell transitions — and reports
    `start_temp_c` as the temperature AT THE START OF THE SCORED WINDOW
    (the stabilised ~48C), not the room-ambient temperature the run
    physically began at.

**Residual-confound arithmetic (the 0.5C bar).** The stabilisation hold
converts each arm's scored start temperature from "whatever room ambient
happened to be" (measured spread up to ~3.9C across captures on hand, and
the rig cannot repeat a start within a session) to "whatever the PID
settled to at the end of a 45-minute hold at a fixed 48C setpoint" — a
quantity dominated by steady-state tracking precision, not ambient. This
rig's own dwell-window steady-state offsets (§3.1, closed) run a few tenths
of a degree; take a deliberately pessimistic residual spread of up to 0.5C
between two stabilised arms (i.e. assume the hold does *nothing* to tighten
things beyond ordinary dwell tracking). Applying the fitted sensitivity's
own top end (0.133C of `iae_normalized_whole_c` per 1C of start delta):

    0.133 C/C * 0.5 C residual spread = 0.0665 C predicted confound

well under the 0.5C actionability bar — over 7x margin even under the
pessimistic assumption. Under a more realistic residual (a few hundredths
to a tenth of a degree, typical of a 45-minute PID-held dwell), the
predicted confound is in the 0.001-0.013C range. Either way the design
clears the bar with room to spare.

**Cost per arm.** Ramp 20C -> 48C at 300C/hr ≈ 5.6 minutes, plus the 45
minute dwell ≈ **51 minutes added per arm**. This REPLACES, not adds to, the
hour-long paired-start wait: `run_queue.py`'s `--pair-consecutive` matching
(`DEFAULT_RESTED_TIMEOUT_S` = 3600s, and the 75-minute stall this task's
brief cites) exists only because two arms currently have to coincidentally
start at the same uncontrolled ambient — once a stabilisation hold gives
every arm the same *controlled* start regardless of when it fires, arms no
longer need to wait for each other, they can fire back-to-back. Net effect:
~51 minutes of hold time per arm, in exchange for removing an open-ended
(up to 75+ minute, sometimes never-succeeding) wait — a net throughput win,
not just a wash.

**Alternatives considered:**

  * *Scored-window restriction alone, no profile change.* Cheaper (a metric
    change only) and IS shipped here as `min_segment_index` — but on its
    own, without a prepended hold, there is no later segment boundary in
    profile 7 to redirect scoring to that is actually decoupled from the
    ambient-exposed opening ramp: that ramp's own thermal transient (lag,
    integrator state) still conditions everything measured after it, and
    this rig has no capture of a run through a *later* segment to fit a
    reduced sensitivity against — extending the honesty requirement already
    enforced elsewhere in this module (never claim a number this rig hasn't
    measured). It is necessary infrastructure for the recommended design
    (the hold needs somewhere to redirect scoring to) but not, on its own,
    shown to clear the 0.5C bar. **Verdict: use it, but only in combination
    with the stabilisation hold, not as a standalone fix.**
  * *Randomising/blocking arm order.* Averages drift out over MANY pairs,
    not within one — doesn't help a campaign that (as measured) struggles
    to complete even one usable pair. Complementary to the recommended
    design for a multi-pair campaign, not a substitute for it.
  * *Start temperature as a regression covariate.* Already shipped
    (`fit_start_temp_sensitivity` / `_start_temp_adjustment`, 2026-09-02f)
    and deliberately never allowed to flip a verdict — it explains a delta
    after the fact, it does not prevent the delta from dominating the
    measurement in the first place. Keep it: report the covariate
    adjustment on stabilised-protocol runs too (using
    `min_segment_index=STABILIZATION_SEGMENT_INDEX` throughout, never mixed
    with `min_segment_index=0` data — see that parameter's docstring), as a
    second line of defense, not the primary fix.

**Protocol for future campaigns:**
  1. Build each arm's profile via `profile_stabilization.prepend_stabilization_hold`.
  2. Fire with `run_queue.py` as before — `--pair-consecutive` and the
     rested-tolerance gate remain useful (still refuse to start on a
     genuinely faulted zone) but no longer need to hold arms hostage to
     matching each other's ambient.
  3. Analyse every stabilised-protocol capture with
     `pid_ab_compare.compare_runs(..., min_segment_index=ab.STABILIZATION_SEGMENT_INDEX)`,
     and fit any sensitivity set used alongside it with the same
     `min_segment_index`.
  4. Do not mix stabilised-protocol runs (segment 0 = hold) with legacy
     runs (segment 0 = the real opening ramp) in one comparison or one
     sensitivity fit — the segment-index convention differs and neither
     `compare_runs` nor `fit_start_temp_sensitivity` can detect the
     mismatch for you.
