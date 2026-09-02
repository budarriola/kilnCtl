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

Identified plant (bench rig, 0–80 °C): z0 K=39.25 τ=263.8 L=52.8; z1 K=31.97
τ=269.8 L=43.5; z2 K=31.68 τ=270.9 L=33.9. Coupling matrix in wire form
`[stepped][affected]`: z0 39.25/15.78/9.70, z1 26.61/31.97/11.38, z2
20.73/21.09/31.68. RGA diagonal 1.53/1.69/1.34. Off-diagonal τ is 620–730 s
against 264 s on the diagonal, dead time 135–158 s against 34–53 s — cross-zone
heat arrives far later than a zone's own element, which is the root of the
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
firmware-facing adaptive-tune self-checks are calibrated against. The two stay
different until §3.2's matrix is adopted into firmware, an owner decision.

**Held-out validation** (`tests/fixtures/plant_sim/p7_fuzzy0_held_out.jsonl`,
a live profile-7 fuzzy=0 tracking run, deliberately excluded from the fit):
temperature RMS z0 3.51 °C, z1 3.15 °C, z2 3.56 °C — worse than the fitted
0.6–0.7 °C but the capture starts 342 s into the firing, so the sim's t=0
fresh-PID/plant state does not match hardware's already-settled state (a
cold-start artifact of this particular capture, not a re-identified plant
defect — the ramp segment right after the cold start carries nearly all the
error; the second ramp segment, once the sim has caught up, tracks to
0.4–1.6 °C). Test pins a 6.0 °C bound per zone; genuinely tighter validation
needs a held-out capture that starts from a rested zero, which does not exist
yet.

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

- [~] **Relay-feedback identification: first hardware completion, 2026-09-02
      — but the result cannot be adopted yet.**

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
      sim's own held-out validation RMS on zone 0 (3.51 C, §3.4) — **the sim
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
      (z0, rested). The sim's own held-out validation RMS is 3.51/3.15/
      3.56 C (§3.4) — **the spread is under 15% of that**, so per the
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

### 3.7 Validation gap

Everything above is measured on a bench rig spanning 0–80 °C. Radiative transfer
goes as `T⁴`, so the plant at kiln temperatures is not the plant identified here.
Every result in §2 validates the **mechanism**, not the behaviour at firing
temperature. This stays open until a real firing.

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
