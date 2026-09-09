# The "factor of ten in identified DC gain" — resolved, and it is not an identification error

2026-09-09. Investigation only; no control code, config or firmware was changed
and nothing was flashed.

## Bottom line

**There is no factor-of-ten error in the identified plant gain.** The 330 °C/duty
"effective secant gain" cited in
`docs/audits/high_temperature_transfer_analysis_2026-09-08.md` §3 is a
**single-zone attribution of a three-zone hold**: it divides zone 0's entire rise
above ambient by zone 0's own duty, at an operating point where zones 1 and 2 were
simultaneously running at 0.53 and 0.90 duty and supplying ~86 % of zone 0's heat.

Accounting, against the board's own steady-state model `G·u = ΔT`:

| step | factor | cumulative |
|---|---|---|
| naive claim `48 / 0.145 / 31.96` | — | **10.3×** |
| cross-zone heat: only 12–14 % of z0's rise comes from `u0` | 7.2–8.4× | 8.4× |
| residual model shortfall (predicted ΔT 38.7–39.6 vs observed ~45 °C) | 1.13–1.15× | 9.6× |
| ambient drift + instantaneous (unaveraged) duty samples | ~1.07× | ~10.3× |

Cross-zone coupling alone accounts for **8.4× of the 10.3×**. Nothing is left over
for an asymptote-extrapolation failure.

## Verification of each claimed premise

- `firmware/KilnFW/App/drivers/control/pid_autotune.c:382` — **confirmed**:
  `m.k_gain_c_per_duty = rise_inf / duty_step`.
- `k_dc ≈ 32 °C/duty` — **confirmed**, `tools/PcTools/config_presets/tuned_baseline_20260831.json`:
  `model_k_dc` = 31.9609 / 23.4805 / 21.7422, `model_tau_s` = 166.9 / 129.1 / 114.8 s.
- `K_diag ≈ 38` — **confirmed** as the *coupling matrix's own* diagonal
  (38.13 / 35.90 / 35.32), a different quantity from `model_k_dc`. See §4.
- The bench observation — **confirmed and located**:
  `logs/coupling/coupid6_dwell_observations.tsv`, profile `coupid6` run 1,
  2026-09-02, three zones, 70 °C dwell:

  ```
  70  139s  72.2 / 72.1 / 70.3 C   duty 0.09 / 0.43 / 0.81
  70  163s  71.6 / 71.7 / 70.2 C   duty 0.11 / 0.46 / 0.83
  70  405s  70.8 / 70.4 / 69.6 C   duty 0.15 / 0.53 / 0.92
  ```

  Ambient at run start 25.66 / 25.54 / 25.34 °C. **All three zones hot.** The file's
  own header warns these are instantaneous UART samples, not settled-window
  averages, and that the 600 s dwells are only ~2.3 τ.

## 1. Candidate 2 (single-zone vs three-zone) — this is the cause, ~8.4×

The firmware's hold model (`zone_coupling_solve.c:229–305`) is exactly
`G·u = T_sp − T_amb`, with `G[i][i]` a per-zone DC gain and `G[i][j]` the
`coupling_coeff` row — same units, °C per unit duty of the *stepped* zone.
Evaluating that model forward at the observed duties `u = [0.145, 0.53, 0.90]`:

with the shipped diagonal (`model_k_dc`):
```
ΔT_pred = [38.66, 34.45, 27.36]      z0's own-duty share: 4.63 of 38.66 = 12.0 %
```
with the coupling matrix's measured diagonal:
```
ΔT_pred = [39.56, 41.04, 39.58]      z0's own-duty share: 5.53 of 39.56 = 14.0 %
```

So 86–88 % of zone 0's rise at that operating point was neighbour heat. The
identified diagonal is being asked to explain a rise it never produced.

## 2. Candidate 1 (asymptote extrapolation) — real but small, ≤1.8×, and structurally capped at 2×

Three **genuinely settled single-zone dwells** exist and directly measure the
secant gain — this is the measurement the task asked for, and it already exists.
`logs/coupling/cpl_z{0,1,2}_mcp.jsonl`, profiles #4/#5/#6, 55 °C, ~35 min dwells
(~8–20 τ), one zone in PID and the others off. Duty and temperature are flat over
the last ~1000 s of each (the trailing `duty=0.000 / T=55.00` rows are the profile
having ended — stale values, excluded):

| zone | settled T | settled u | secant (amb 25.5 / 27.5) | ÷ `model_k_dc` | ÷ matrix diagonal |
|---|---|---|---|---|---|
| 0 | 55.0 °C | 0.60 | 49.2 / 45.8 | 1.54 / 1.43 | 1.29 / 1.20 |
| 1 | 55.1 °C | 0.755 | 39.2 / 36.6 | 1.67 / 1.56 | 1.09 / 1.02 |
| 2 | 55.05 °C | 0.755 | 39.1 / 36.5 | 1.80 / 1.68 | 1.11 / 1.03 |

**A settled single-zone secant gain exceeds the identified `model_k_dc` by only
1.4–1.8×, and matches the coupling matrix's own diagonal to within 1.0–1.3×.**
That is the whole of the extrapolation-plus-concavity error, and it is nowhere
near 10×.

It is also bounded by construction: `MAX_EXTRAPOLATION_RATIO = 2.0f`
(`pid_autotune.c:81`) refuses to let `rise_inf` exceed 2× the raw observed rise, so
even a maximally-truncated step cannot produce a 10× low `K`. The direction is
right (truncation biases `K` low; the file's own comment block tabulates −6 % to
−69 % on truncated traces) — the magnitude is not.

*Caveat:* the "single-zone" runs are not neighbour-free. During `cpl_z1`, zone 0
drifted to ~49.9 °C and zone 2 to ~36.8 °C. Those warm neighbours reduce z1's loss,
so the true cold-neighbour secant is somewhat larger than the table's — which makes
the residual gap smaller still, not larger.

## 3. Candidates 3 and 4 — ruled out and minor

- **Duty definition (3): ruled out.** Both numbers are the same field. The
  `coupid6` duties come from `/api/profile_exec`'s per-zone `duty=` (pre-PWM
  intended duty, 0–1); `duty_step` in `pid_autotune.c` is the same 0–1 quantity
  (`autotune_engine.c:614` logs "stepping duty to %.2f"). No percent/fraction
  factor, no chopped-vs-intended mismatch. There is no 10 anywhere in either path.
- **Ambient (4): ~1.05×.** Cold-junction readings drift 25.5 → 27.5 °C across the
  coupling runs; on a ~45 °C rise that is ~4 %. Real, but a rounding term here.

## 4. A genuine defect this surfaced: the solve's diagonal and off-diagonals come from different experiments

`profile_executor_feedforward.c:175` ships `s_coupling_use_measured_diag_k_dc =
false`, so the live solve builds `G` from the **FOPDT step** diagonal
(31.96 / 23.48 / 21.74) and the **coupling identification's** off-diagonals. Those
two halves were never jointly identified — the coupling run's own diagonal is
38.13 / 35.90 / 35.32, 17–63 % larger — and the settled dwells in §2 say the
coupling run's diagonal is the better of the two (1.0–1.3× vs 1.4–1.8×).

The consequence is measurable. Solving for the observed 70 °C hold
(`ΔT = 44.5 °C`, all zones):

| `G` | cond | solved `u` | observed `u` |
|---|---|---|---|
| shipped (`model_k_dc` diagonal) | **14.2** | `[0.076, −0.118, 2.085]` | `[0.145, 0.53, 0.90]` |
| matrix's own diagonal | **4.64** | `[0.207, 0.523, 1.027]` | `[0.145, 0.53, 0.90]` |

The shipped mixed matrix demands a **negative** duty from z1 and **2.09** from z2 —
grossly infeasible, and it triples the condition number. The self-consistent matrix
lands within 0.01 of z1's observed duty and 14 % of z2's, and is only marginally
infeasible at 70 °C. **This is a strong candidate explanation for the documented
"`ff_hold` infeasible 100 % of ticks above ~62 °C"**
(`project_ff_hold_infeasible_above_62c`), and it has the "reset one side of a pair"
shape named in `CLAUDE.md`: two quantities joined by the semantic contract "these
are cells of one matrix", each internally consistent, sourced from different
experiments, with nothing forcing them to agree.

*Not verified:* whether flipping that flag would actually improve tracking on
hardware. `coupling_diag_k_dc` has no writer on this board today
(`profile_executor_feedforward.c:168`), so the flag cannot simply be flipped —
`autotune_engine_guard.c:631` would have to populate it first, and it would then be
populated from the FOPDT fit, i.e. with the 32 number, not the 38 one. That is a
second half of the same mismatch.

## 5. What measurement would confirm the conclusion

Cheapest, and it settles the headline claim outright: **a settled three-zone hold**.
Hold all three zones at one setpoint in the 55–70 °C band for ≥ 8 τ (≥ 25 min, not
`coupid6`'s 600 s / 2.3 τ) and average duty over the last 10 minutes. Prediction, if
this report is right: at 70 °C the settled duties land near `[0.2, 0.5, 1.0]` and
*not* near `[0.145, ·, ·]` with z0 still creeping upward — `coupid6`'s z0 duty was
observed rising 0.09 → 0.11 → 0.15 through that dwell and had not converged.

Second, to close §4: re-identify the diagonal *in the same experiment as the
off-diagonals* (step each zone in turn from a common rested baseline and fit the
full row, diagonal included), rather than importing the diagonal from a separate
single-zone FOPDT run.

## Claims I could not verify

- The audit's `ΔT = 48 °C` figure. The capture gives z0 at 70.8–72.2 °C against a
  run-start ambient of 25.66 °C, i.e. **45.1–46.5 °C**. The 10.3× arithmetic above
  uses 44.5 °C at the settled-most sample; using 48 gives 11.1×. Immaterial to the
  conclusion.
- Ambient at the 70 °C dwell itself. Only the run-start value is recorded; the
  enclosure had been heating for hours, so true ambient was higher and every secant
  gain above is, if anything, overstated.
- Whether the coupling matrix's off-diagonals suffer the same truncation bias as the
  diagonal. They were fitted from the same `cpl_z*` runs, which *are* settled (§2),
  so probably not — but I did not refit them.
