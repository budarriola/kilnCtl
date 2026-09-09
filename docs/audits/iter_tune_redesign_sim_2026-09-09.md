# `iter_tune` redesign — simulation results, 2026-09-09

Evidence record for `docs/ITER_TUNE_REDESIGN_PLAN.md` steps 1, 2 and 5.
Harness: `firmware/KilnFW/App/test/sim_iter_tune.c`, linking the real
`pid.c`, `heater_output.c` (the 60 s PWM window, gap G2),
`zone_coupling_solve.c`, `sim_plant.c`, and the three modules under test
(`firing_score.c`, `firing_compare.c`, `iter_tune.c`). No hardware was
touched and nothing was flashed.

Plant: the measured G1 parameters (`tuned_baseline_20260831.json`,
`coupling_matrix_20260831.json`), relay actuation lag 0.5 s (G3, assumed —
still unmeasured on this bench), MAX31856 quantisation (G4). Profile: three
ramps (30/60/90 °C/hr) and three dwells (26/31/36 °C), giving six distinct
segment classes so the comparator sees `n = 3` per sub-score.

**Everything below is inside 20–36 °C.** The literal G1 mapping caps the
model at `ambient + model_k_dc` (~42–52 °C), so none of this is evidence
about behaviour at cone temperature.

## Oracle grid — how much is actually on the table

Mean |actual − target| over scored ticks, sweeping kp and ki multipliers on
the bench gains:

| kp× \ ki× | 0.25 | 0.5 | 1.0 | 2.0 | 4.0 |
|---|---|---|---|---|---|
| 0.25 | 1.97 / 1.99 / 0.83 | 1.29 / 1.29 / 0.84 | 0.98 / 1.01 / 0.88 | 0.97 / 1.04 / 0.96 | 2.46 / 2.31 / 2.30 |
| 0.5 | 1.93 / 1.96 / 0.81 | 1.24 / 1.25 / 0.80 | 0.93 / 0.96 / 0.81 | 0.91 / 0.99 / 0.93 | 1.69 / 1.65 / 1.67 |
| 1.0 | 1.84 / 1.91 / 0.79 | 1.19 / 1.19 / 0.77 | **0.92 / 0.90 / 0.76** | 0.83 / 0.86 / 0.81 | 0.89 / 0.96 / 1.02 |
| 2.0 | 1.71 / 1.79 / 0.79 | 1.15 / 1.19 / 0.77 | 0.90 / 0.88 / 0.74 | 0.78 / 0.83 / 0.78 | 1.22 / 1.33 / 1.29 |
| 4.0 | 1.51 / 1.64 / 1.11 | 1.27 / 1.36 / 1.07 | 1.23 / 1.31 / 1.19 | 1.45 / 1.47 / 1.63 | 1.47 / 1.48 / 1.43 |

(cells are z0 / z1 / z2, °C.) Reachable spread: **z0 1.68 °C, z1 1.48 °C,
z2 1.56 °C** — comfortably above the owner's 0.5 °C floor, and dominated by
`ki`, not `kp`. This grid is what made the two findings below falsifiable:
without it, "the mechanism never accepts" is indistinguishable from "there
was nothing to accept".

## Two design defects found, both in the plan's own §4 step schedule

1. **Halving the step after an unmeasurable trial is backwards.** The plan
   halves after any two rejects, but `INSUFFICIENT` (nothing cleared the
   0.5 °C floor) means the change was *too small to measure*. Halving makes
   the next signal smaller still. Measured: a 10 % step on kp or ki moves the
   sub-scores by 0.01–0.10 °C against a 0.5 °C floor, so under the literal
   schedule the mechanism refused **every** trial from **every** start set
   and walked itself down to the 3 % convergence threshold having learned
   nothing. Fixed: `INSUFFICIENT` now grows the step (doubling, capped at
   50 % of baseline and still hard-caged to [0.5×, 2×] of the anchor);
   `REJECT_DEGRADED` still halves it.
2. **A shared step plus a parameter that only advanced on failure spent the
   whole budget on `kp`.** The plan says coordinate descent cycles
   `kp → ki`; the first implementation only advanced on a reject, so with a
   six-trial budget `ki` — which the grid above shows carries nearly all of
   the available improvement — was never reached. Fixed: per-parameter step
   and direction state, parameter cycles after every scored trial.

## Results after both fixes

### Multi-start, nominal plant (24 zone-runs, 8 starting gain sets)

| start set | z0 | z1 | z2 |
|---|---|---|---|
| bench (as commissioned) | 0.919 → 0.919 | 0.904 → 0.904 | 0.756 → 0.756 |
| kp 0.6× (sluggish) | 0.916 → 0.916 | 0.946 → 0.946 | 0.795 → 0.795 |
| kp 1.8× (hot) | 0.899 → 0.899 | 0.883 → 0.883 | 0.746 → 0.746 |
| ki 0.4× (slow trim) | 1.355 → 1.355 | 1.367 → 1.367 | 0.777 → 0.777 |
| ki 2.5× (wind-up prone) | 0.885 → 0.885 | 0.952 → 0.952 | 0.925 → 0.925 |
| kp 0.7× ki 2.0× (mixed) | 0.863 → 0.869 | 0.916 → 0.900 | 0.862 → 0.811 |
| ki 0.15× (badly detuned) | 2.491 → 2.383 | **2.600 → 2.090** | 0.814 → 0.817 |
| kp 0.35× ki 0.3× (very detuned) | 1.718 → 1.718 | 1.733 → 1.733 | 0.820 → 0.820 |

mean |err| in °C, initial → final, evaluated on the *same* plant, seed and
start temperature so the difference is the gains and nothing else.

**24 zone-runs: 1 improved (−0.511 °C), 23 unchanged within the 0.5 °C
floor, 0 regressed, 24/24 converged** (no oscillation; every zone reached a
stopping rule). The largest adverse movement anywhere is +0.006 °C.

### A1 false accept (null experiment)

660 comparisons of two firings with **identical** gains, differing only in
noise seed and start temperature (±15 °C, i.e. A7 folded in):
**0 ACCEPT (0.00 %), 660 INSUFFICIENT.** Bar: ≤ 2 %. **PASS.**

### A2 never-worse, mismatched ensemble

220 plants with K ±30 %, τ ±40 %, L ±50 %, coupling ±50 %, start ±15 °C,
while the controller keeps the nominal measured parameters throughout;
660 zone-runs: **better 13, unchanged 647, worse 0 (0.00 %)**, mean cost
change **−0.031 °C**, terminated 660/660, **0 cage violations**.
A2 (≤ 1 % worse) **PASS**; A5 (≥ 95 % termination) **PASS**; A6 (0 cage
violations) **PASS**.

## What this does NOT establish

- **A3 and A4 are not met and were not claimed.** The mechanism improves
  tracking only from a badly detuned start, and only on one zone out of
  three there. On a well-commissioned zone it correctly does nothing: no
  single-parameter move inside the [0.5×, 2×] cage is worth 0.5 °C.
  Deployed, it should be expected to report "refused" almost always — which
  is what the plan itself predicts (§3.1, "refusing to act is a legitimate
  outcome and is the default one").
- The credibility gate of plan §6.5 (reproducing a recorded real firing from
  `logs/coupling/*.jsonl`) has **not** been run. Everything above is
  internal consistency of a model against itself.
- Bar 2 is unexercised: no noise-floor artifact exists, so every result here
  is Bar 1 plus the no-degradation veto.
- Persistence, the HTTP surface, the write-surface check and shadow mode
  (plan steps 7–9) are not implemented. Nothing is wired into
  `profile_executor.c`; the module still proposes nothing on hardware.
