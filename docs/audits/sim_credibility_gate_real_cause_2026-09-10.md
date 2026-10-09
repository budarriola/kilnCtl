# Why the iter_tune credibility gate really fails: the sim couples zones by EXCHANGE, the board couples them by SOURCE, 2026-09-10

`firmware/KilnFW/App/test/sim_credibility_gate.c` (added `225d4b91`) has failed
since it was written, and both previously-published explanations have been
retired:

- "the literal sec 6.2 `h=1` mapping caps each zone near 50-60 C" -- refuted;
  `h` is a provably free scale.
- "2 of 6 measured cross-gains are structurally unreachable" -- retired; that
  used the stale preset diagonal. It is re-confirmed retired below (sec 3):
  on the live diagonal, **zero of six** are unreachable.

This pass reproduces the failure, names the cause, and demonstrates it by a
single structural substitution that moves ramp MAE from 8.4-9.7 C to 1.3-3.4 C.

## 1. Reproduction

Built from `HEAD` with MSVC (`sim_credibility_gate.c` + `sim_plant.c` +
`heater_output.c`, host stubs on the include path exactly as
`firmware/KilnFW/App/test/build_host_tests.ps1` sets them up) and run against
the two named captures:

```
calibration: logs/coupling/noise_floor_p7_run1.jsonl  (366 ticks)
hold-out:    logs/coupling/noise_floor_p7d_run1.jsonl (366 ticks)
```

| | ramp MAE (bar 3.0) | dwell offset (bar +/-1.5) |
|---|---|---|
| CALIBRATION z0/z1/z2 | 9.696 / 9.041 / 8.662 | -13.829 / -13.692 / -13.501 |
| HOLD-OUT    z0/z1/z2 | 9.501 / 8.793 / 8.432 | -13.694 / -13.471 / -13.225 |

The coupling fit reports `50 sweep(s), DID NOT CONVERGE`, with
`coupling_w_per_c[0][1]` and `[1][0]` both pinned at the 200 W/C numerical
safety rail.

Two things in that table are diagnostic on their own:

- The dwell offset is **the same to within 0.6 C on all three zones**, in both
  captures, despite the three zones running at wildly different duties
  (0.166 / 0.361 / 0.615). The fitted conductances are 100-200x the per-zone
  loss coefficient (`h = 1`), so the sim's three zones are welded into one
  lumped body that settles at a duty-weighted average. The real kiln's zones
  are not welded: they sit 1.3 C apart at a 33 C rise.
- Every simulated dwell-entry overshoot peak is exactly `0.000 C` -- the sim
  never reaches target at all, so "overshoot" is not being measured.

## 2. The cause: two incompatible coupling model classes

**The firmware's model is additive source-gain.** `coupled_ident.py`'s
`_hybrid()` substitutes `FF_K_DC_DIAGONAL` onto the diagonal of the
off-diagonal-only coupling matrix, and
`zone_coupling_gauss_solve_partial_pivot_vec()`
(`firmware/KilnFW/App/drivers/control/zone_coupling_solve.c`) solves
`G . u = b` with

```
G = diag(model_k_dc) + coupling_coeff        (off-diagonals ADD)
```

i.e. zone i's steady rise is `sum_j G[i][j] * u_j`. Neighbour duty raises zone
i **regardless of the temperature difference between them**, because the
neighbouring element is dumping power into shared chamber air.

**The simulation's model is conservative exchange.** `sim_kiln_step()`
(`firmware/KilnFW/App/test/sim_plant.c`) instead computes

```c
power_couple_w += cfg->coupling_w_per_c[i][j] * (state->zone[j].element_c - t_i);
```

a temperature-**difference** term. That term is **identically zero when the
zones are uniform**, for every value of `coupling_w_per_c`.

The two forms agree only in the differential mode. They disagree completely in
the common mode -- and a three-zone dwell at one target is almost pure common
mode.

### 2.1 The exact structural bound

Let `M = argmax_i T_i` at steady state. For that zone every exchange term is
`<= 0`, so its balance `0 = u_M*k_M - (T_M - amb) + sum_j g[M][j]*(T_j - T_M)`
gives

```
max_i (T_i - ambient)  <=  max_i ( u_i * model_k_dc[i] )
```

for **any** non-negative coupling matrix, at any conductance, with any `h`.

At the recorded dwell operating point (`u = 0.1660 / 0.3609 / 0.6152`, live
diagonal `39.2459 / 31.9669 / 31.6810`, ambient 28.58 C):

```
u_i * k_dc[i]           =  6.51 / 11.54 / 19.49 C   -> bound 19.49 C
recorded rise           = 33.40 / 32.39 / 32.14 C
additive (diag(k)+C).u  = 29.74 / 27.54 / 25.36 C
```

A Monte-Carlo over 200,000 random non-negative coupling matrices
(each entry log-uniform over 1e-3 .. 1e3 W/C) reaches a best-case maximum zone
rise of **19.4901 C** against the algebraic bound of **19.4902 C** -- the bound
is tight and saturated.

**The recorded dwell operating point lies outside the exchange model's
reachable set by a factor of 1.7.** No coupling fit -- one-shot algebraic,
iterative, or a perfect global optimum -- can close a gate scored at that
point. This is why "the fit quality is not the lever" was the right
observation and why the iterative fit moved the numbers by ~1 C.

## 3. Confirming the retraction of the "unreachable cross-gain" story

Under the same steady-state solve, maximising each cross-gain independently
over 400,000 random matrices (entries log-uniform 1e-3 .. 1e4 W/C):

| pair | measured | max achievable | reachable |
|---|---|---|---|
| c[0][1] | 27.32 | 31.964 | yes |
| c[0][2] | 21.72 | 31.678 | yes |
| c[1][0] | 14.30 | 39.242 | yes |
| c[1][2] | 22.15 | 31.678 | yes |
| c[2][0] |  8.33 | 39.242 | yes |
| c[2][1] | 12.42 | 31.964 | yes |

Zero of six unreachable, independently confirming the retraction. (The
achievable ceiling approaches `k_dc[j]` rather than `k_dc[j]/2` because the
model permits a one-way conductance -- large `g[i][j]` with small `g[j][i]` --
which drags the receiver up without loading the source. That is not physical,
but it is what this model class allows, and it is not what makes the gate
fail.)

The gate's own printed FEASIBILITY paragraph still asserts the retired
`coupling_coeff[i][j] >= model_k_dc[j]` test. It is now vacuous (it flags
nothing) but it is still wrong as stated and should be deleted or corrected.

## 4. The discriminating experiment

One change, in a scratchpad copy only, to `sim_kiln_step()`'s coupling term:

```c
/* was: coupling_w_per_c[i][j] * (T_j - T_i)   -- exchange   */
   power_couple_w += cfg->coupling_w_per_c[i][j] * u_j;   /* additive source */
```

with `coupling_w_per_c` read directly as the measured `coupling_coeff`
(C per unit duty of the driving zone; no fit needed -- in the additive form the
measured cross-gain *is* the coefficient). Everything else identical: same
captures, same `model_k_dc`/`tau_s`/`dead_time_s`, same `heater_output.c` PWM
window, same relay lag, same quantiser, same bars.

| | ramp MAE (bar 3.0) | dwell offset (bar +/-1.5) |
|---|---|---|
| CALIBRATION z0/z1/z2 | **1.481 / 1.710 / 3.367** | **-2.104 / -2.752 / -4.816** |
| HOLD-OUT    z0/z1/z2 | **1.489 / 1.335 / 2.780** | **-1.412 / -1.957 / -4.049** |

Ramp MAE drops by a factor of ~5 and 5 of 6 zone-runs now pass it. Dwell offset
drops from -13.5 C to -1.4..-4.8 C. Nothing was tuned; only the model class
changed.

## 5. The residual is the already-documented forward-gain deficit

The leftover -1.4..-4.8 C is still negative on every zone and ordered
z0 < z1 < z2 in magnitude -- the same sign and the same zone ordering as the
`cplval75` forward-residual finding (`G.u - dT` negative on all three zones,
reproduced by per-row scalars 1.12 / 1.19 / 1.31). Applying exactly those three
published scalars to row i of `G` (both the diagonal `model_k_dc[i]` and that
row's off-diagonals) in the additive variant:

| | ramp MAE (bar 3.0) | dwell offset (bar +/-1.5) |
|---|---|---|
| CALIBRATION z0/z1/z2 | 2.302 / 1.892 / 1.390 -- all PASS | +0.642 / +1.450 / +1.350 -- all PASS |
| HOLD-OUT    z0/z1/z2 | 2.690 / 2.462 / 1.907 -- all PASS | +1.473 / +2.468 / +2.455 -- 1 of 3 PASS |

The offsets cross zero and become slightly positive, i.e. those scalars are
marginally too large at this operating point -- expected, since they were fitted
at a different one. This is a *demonstration that the residual is that deficit*,
not a proposed correction; adopting fitted scalars into the gate would destroy
its non-circularity posture.

**So the gate's failure decomposes as: ~10 C of wrong-model-class (sec 2) on top
of ~2-5 C of the already-known forward-gain deficit (this section).**

## 6. Are the bars reachable by a model of this form?

- **ramp MAE <= 3 C**: yes, demonstrated (sec 4, five of six zone-runs, with no
  gain correction at all; six of six with it).
- **dwell offset +/- 1.5 C**: yes, demonstrated (sec 5), though only once the
  forward-gain deficit is accounted for.
- **dwell-entry peak +/- 2 C**: **not demonstrated, and this is the one bar
  still genuinely open.** In sec 5's run the sim overshoots 4.7-9.1 C where the
  real kiln overshoots 1.3-3.8 C. About 2 C of that is the positive gain bias
  the scalars introduce; the remaining 2-3 C is not explained here. Candidates,
  and the measurement that separates them:
  1. *Dynamics too underdamped* -- the plant is one first-order lag plus a pure
     transport delay, with no separate element/chamber mass. Discriminator: fit
     a second-order (two-mass) plant to the same single-zone step data that
     produced `model_k_dc`/`model_tau_s`/`model_dead_time_s` and compare its
     step response's overshoot under the same replay.
  2. *Open-loop replay* -- the recorded duty was generated in closed loop
     against the real temperature, so any residual model gain error accumulates
     into the sim's excursion instead of being corrected. Discriminator: re-run
     the replay in closed loop with the real `pid.c` linked, which the gate
     deliberately does not do today; a much smaller overshoot excess would
     implicate replay rather than the plant.
  3. *Relay lag* -- G3 is an unmeasured 0.5 s placeholder. Discriminator: bench
     measurement of the SSR/contactor actuation delay; a value large relative
     to the 60 s PWM window would matter, 0.5 s almost certainly does not.

  Candidate 1 is the most likely and (2) is the cheapest to test.
- **noise-floor spread <= 2x**: no evidence either way. In sec 1 it is vacuously
  OPTIMISTIC (all sim spreads are 0.000 because nothing ever overshoots); in
  sec 5 five of six exceed 2x, but on a run whose peaks are inflated. This bar
  cannot be judged until the peak bar is.

## 7. Candidates checked and ruled out

- **Stale constants.** `sim_credibility_gate.c` `#include`s
  `firmware/KilnFW/App/test/sim_measured_zone_constants.h` (`4d42bafe`), and the
  gate's own printout reports `model_k_dc` 39.25 / 31.97 / 31.68 -- byte-equal to
  `FF_K_DC_DIAGONAL` in `tools/PcTools/src/kilnctrl/coupled_ident.py`. Not stale.
  (Direct `GET /api/zones` confirmation was not obtainable this session: the
  board answered at neither `kiln.local` nor `192.168.4.1` while other agents
  were working on it. The two independent in-repo copies agreeing to six
  decimals is the evidence used instead.)
- **Capture provenance (coupled vs uncoupled feedforward).** Both captures ran
  `control_mode` 2 throughout, with `ff_hold_used_matrix` mixed true/false and
  `ff_hold_infeasible` false everywhere. This does not matter: the gate replays
  the **recorded duty sequence** through the plant and never recomputes a
  control decision, so the replay is agnostic to how that duty was produced. A
  controller change can alter which operating points get exercised, but it
  cannot produce a systematic plant-model offset. Ruled out as a cause; worth
  recording because it is a plausible-sounding story that this mechanism
  forecloses.
- **Coupling fit quality.** Bounded out by sec 2.1: the failing operating point
  is outside the model's reachable set for *every* matrix.

## 8. Recommendations

1. **Change `sim_kiln_step()`'s coupling term to the additive source-gain form**
   (sec 4), so the simulation and the firmware share one model class. This is
   the substantive fix and it is a simulation-harness change, but it also
   changes `test_sim_kiln.c`'s behaviour and the meaning of
   `sim_kiln_coupling_from_cross_gain()` / `sim_kiln_coupling_fit_iterative()`
   (both become unnecessary -- in the additive form the measured cross-gain is
   the coefficient). Deliberately **not** done in this pass: it deserves its own
   reviewed change with the host tests re-baselined, not a drive-by edit inside
   a diagnosis.
2. **Delete or correct the gate's printed FEASIBILITY paragraph** (sec 3). It
   states a retired and incorrect bound.
3. **Wire the gate into a check.** Today `sim_credibility_gate.c` is referenced
   by no build recipe and no `check_*.ps1`; only `sim_plant.c` is built by
   `build_host_tests.ps1` (via `test_sim_kiln.c`). It should be built and run,
   *but* its captures live under `logs/coupling/` and are gitignored, so on a
   fresh clone it will exit 3 (SKIP). That is the correct behaviour and the gate
   already implements it explicitly -- what must not happen is a wrapper that
   maps exit 3 onto a pass. Recommendation: add it to `build_host_tests.ps1`
   with exit 3 surfaced as a distinct SKIP line in the summary, never folded
   into the pass count.
4. **Do not loosen any bar.** Three of the four are demonstrably reachable once
   the model class is right; the fourth is an open dynamics question, not a bar
   problem.

## Method note

No `check_*` or assertion was added by this pass, so there is nothing here to
negative-test. Every number above is either read from an unmodified `HEAD`
build of the gate, or produced by a scratchpad-only variant; no file in the
repository was modified.
