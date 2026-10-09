# cplval75 settled holds — what they say about the coupling matrix, and why `ff_hold_infeasible` went quiet

2026-09-10. Analysis only. No control code, config, firmware or board state was
changed; the bench board was idle throughout and only read-only queries
(`control_get_zones`, `wifi_get_status`, `GET /api/zones`, `coupled_ident_report`)
were issued.

Inputs: `docs/audits/cplval75_settled_holds_2026-09-10.md` and
`logs/coupling/cplval75_20260910_settled_hold_points.tsv` (commit `1efbdc0c`),
plus the local-only raw capture `logs/coupling/cplval75_20260910.jsonl` and the
pre-guard `logs/coupling/cplval75_20260909.jsonl`.

---

## Bottom line, in order of consequence

1. **Finding B is confirmed outright, and by two independent routes.** The
   coupled hold solve did not execute on a single tick of this run. The
   `ff_hold_infeasible` flag's absence carries **no information about
   feasibility**. The historical "infeasible above ~ambient+38 °C" finding is
   untouched and still stands for the coupled path.
2. **The premise numbers behind `3605f278` (`docs/audits/dc_gain_factor_of_ten_2026-09-09.md`)
   are computed against a diagonal the board has never carried.** The board's
   live `model_k_dc` is **39.2459 / 31.9669 / 31.6810** °C/duty. The audit — and
   `docs/COUPLING_JOINT_IDENTIFICATION_CAPTURE.md`, and three simulation
   harnesses — use **31.9609 / 23.4805 / 21.7422**, which is
   `tools/PcTools/config_presets/tuned_baseline_20260831.json`, a preset file,
   not the board. Its headline figures do not survive the correction (§4).
3. **Finding A is real but is not a zone-2 problem.** The matrix under-predicts
   the settled rise on **all three** zones, by 11 % / 18 % / 30 % along this
   run's duty direction. The inverse solve concentrates that shortfall onto z2
   because z2 is the only zone whose own duty carries most of its own rise. The
   error is a **gain deficit, not a missing loss term** — the sign is wrong for
   a loss — and it is essentially flat in temperature over 62–75 °C, so it is
   not a nonlinearity either.
4. **A and B are the same defect seen twice.** The live matrix predicts z2 hits
   `u = 1` at ΔT = 38.2 °C — i.e. ~62 °C at the earlier firings' ~24 °C ambient.
   That *is* the documented "infeasible above ~62 °C" boundary, reproduced to
   within a degree. cplval75 measures z2 actually needing 0.853 at ΔT = 46 °C.
   **The historical infeasibility was the matrix's z2-row gain deficit, not a
   plant power limit.**
5. **Verdict on the adopted matrix: refuted.** It fails its own pre-registered
   acceptance test (`COUPLING_JOINT_IDENTIFICATION_CAPTURE.md` §Acceptance, step 4)
   on 2 of 3 zones at every one of the three plateaus. It should not be adopted
   for the coupled path. The provenance guard is currently the only thing
   preventing it from being used, and it is doing the right thing.

---

## 1. Finding B — confirmed: the coupled solve never ran

**Route 1, the capture itself.** Every zone record in the run carries
`ff_hold_used_matrix`:

```
1811 samples per zone, 3 zones, 5433 total
ff_hold_used_matrix:  False  1811/1811 on every zone
ff_hold_infeasible:   False  1811/1811 on every zone
```

`zone_coupling_solve.c:452,458` sets `*out_infeasible = (reason == COUPLING_SOLVE_OK) && infeasible`.
With `used_matrix` false on every sample, `infeasible` is structurally
unreachable for the whole run. This is not "the problem went away"; it is the
reporting code not running.

**Route 2, the config that forces the refusal.** Live `GET /api/zones`:

```
zone 0  coupling_c1=27.32 c2=21.72   coupling_diag_k_dc = 0.0
zone 1  coupling_c0=14.30 c2=22.15   coupling_diag_k_dc = 0.0
zone 2  coupling_c0= 8.33 c1=12.42   coupling_diag_k_dc = 0.0
```

`coupling_matrix_provenance_ok()` (`zone_coupling_solve.c:259`) finds
`any_measured_off_diagonal = true` (six non-zero cells), then requires every
member's `coupling_diag_k_dc` to be finite and `> 0`. All three are 0.0 —
`control_get_zones` renders this as *"never identified on hardware"*. The matrix
is refused whole with `COUPLING_SOLVE_FALLBACK_MIXED_PROVENANCE`, and the caller
gets the untouched uncoupled per-zone feedforward. `s_coupling_use_measured_diag_k_dc = false`
(`profile_executor_feedforward.c`) is a second, independent refusal on its own.

The guard commit `587a34ae` is an ancestor of the flashed ESP image `0dddd435`
(verified with `git merge-base --is-ancestor`), so the guard was live.

**Route 3 — a direct A/B across the guard.** The *aborted* 2026-09-09 attempt
(`logs/coupling/cplval75_20260909.jsonl`), same board, same profile, pre-guard
firmware, records:

```
ff_hold_used_matrix: True  80/80 on every zone
```

Same profile, one firmware change, the flag flips from True to False on 100 % of
samples. There is no remaining ambiguity.

**This was already documented and was missed.** `docs/COUPLING_JOINT_IDENTIFICATION_CAPTURE.md`
§"Why this capture exists" states in bold: *"Today's board runs on that fallback
— it has measured off-diagonals and no persisted `coupling_diag_k_dc`, so
`ff_hold`/`ff_climb` never assemble a coupled `G` at all."* The cplval75 audit
reported the flag's absence as a discrepancy that "revises the standing
expectation" without connecting it to that sentence. **Recommendation:** amend
`docs/audits/cplval75_settled_holds_2026-09-10.md`'s `ff_hold_infeasible`
section to say the flag is inert on this firmware, and leave
`project_ff_hold_infeasible_above_62c` standing.

---

## 2. Finding A — a per-row gain deficit, not a z2 loss term

The matrix the board actually holds (off-diagonals as stored, diagonal
substituted from `model_k_dc`, exactly what `zone_coupling_solve_hold()` runs):

```
G = [[39.2459, 27.32,   21.72  ],
     [14.30,   31.9669, 22.15  ],
     [ 8.33,   12.42,   31.6810]]     cond(G) = 5.508
```

Ambient reference = the capture's first zone temperatures, 29.19 / 29.07 / 29.03
(the convention `coupled_ident.py::_run_ambient` uses).

### 2a. The forward residual is negative on every zone

`G·u − ΔT`, in °C, at each plateau:

| plateau | z0 | z1 | z2 |
|---|---|---|---|
| 62 °C | −3.36 | −5.61 | −8.37 |
| 70 °C | −4.28 | −6.22 | −9.51 |
| 75 °C | −5.58 | −7.10 | −10.72 |

Every entry is negative: the plant produced **more** rise than the matrix
predicts, on all three zones, at all three plateaus. This is the single most
important number in the pass, and it kills two of the four candidate
explanations immediately:

- **"A missing loss term specific to the bottom zone" is ruled out on sign.** A
  missing loss makes the observed rise *lower* than predicted. The observed rise
  is higher. Whatever is missing is a heat *source* term or an understated gain,
  not a loss. It is also not z2-specific — z0 and z1 show the same sign.
- **"Something structural the linear model cannot express" is not supported.**
  Expressed as a per-row multiplicative correction `ΔT_obs / ΔT_pred`:

  | plateau | z0 | z1 | z2 |
  |---|---|---|---|
  | 62 °C | 1.114 | 1.205 | 1.339 |
  | 70 °C | 1.117 | 1.179 | 1.302 |
  | 75 °C | 1.138 | 1.183 | 1.304 |

  A single per-row scalar of ≈ **1.12 / 1.19 / 1.31** reproduces all nine
  measurements to within ~2 %, across a 33 → 46 °C span of rise. The linear
  model is adequate here; its coefficients are simply too small.

### 2b. Why the inverse solve blames z2 alone

Under `G`, each zone's own duty supplies this fraction of its own predicted rise:

| plateau | z0 | z1 | z2 |
|---|---|---|---|
| 62 °C | 0.223 | 0.434 | 0.757 |
| 70 °C | 0.190 | 0.442 | 0.764 |
| 75 °C | 0.171 | 0.449 | 0.766 |

z0's rise is ~81–83 % neighbour heat; z2's is ~24 %. When `G⁻¹` is asked to
absorb a shortfall that is spread over all three rows, the only zone with enough
own-duty leverage to move the answer is z2, so the correction lands there. The
reported `mean_error` of +0.303 on z2 (and −0.047 on z0, +0.009 on z1) is that
redistribution, not a statement that z2's row is uniquely wrong.

Confirming this: the same +0.30 miss appears at every plateau
(+0.278 / +0.316 / +0.352), rising steadily with the shortfall, and z0's error is
**negative** — a genuinely z2-local defect would not push z0 the other way.

### 2c. Diagonal versus off-diagonal — the data cannot tell you

Three plateaus of a *proportional* three-zone hold are nearly collinear in duty
space. Stacking the three duty vectors:

```
u(62) = [0.1680, 0.3714, 0.5905]
u(70) = [0.1764, 0.4792, 0.7589]      cond(U) = 730
u(75) = [0.1756, 0.5453, 0.8533]
```

`cond(U) = 730` on a 3×3 that is nominally exactly determined. Solving each row
exactly from the three plateaus gives nonsense — row 0 comes out
`[47.2, 173.0, −66.5]` — which is exactly the overfitting signature
`coupled_ident_report`'s own plausibility check flagged (negative entries, rows
not diagonal-dominant). **The re-fit is not bad because there are only 6
observations; it is bad because the 6 observations point in one direction.**

Holding the off-diagonals fixed and fitting only the diagonal by least squares:

| row | diagonal-only fit | vs. live `model_k_dc` | with a free offset |
|---|---|---|---|
| 0 | 64.75 | +65 % | 221.4, offset −27.2 °C (meaningless — z0's own-duty leverage is too small) |
| 1 | 45.40 | +42 % | 40.24, offset +2.46 °C |
| 2 | 44.58 | +41 % | 40.37, offset +3.16 °C |

Row 2's fit is the only one worth quoting, because row 2 is the only row whose
own duty dominates its own rise. It wants **44.6 °C/duty against the 31.68 the
board carries**, or 40.4 with a +3.2 °C offset. But an equally good fit is
obtained by scaling row 2's *off-diagonals* up instead. **This data identifies
the row's total gain along one duty direction and nothing finer.**

### 2d. Scale error or constant offset — genuinely open, and ambient matters

The run drifted +2.0 °C in enclosure temperature over 2h38m. Re-running §2a with
a linear ambient allowance (+0.7 / +1.2 / +1.9 °C at the three plateaus) flattens
the residuals considerably:

| | z0 | z1 | z2 |
|---|---|---|---|
| residual °C, 62 → 75 (no drift) | −3.36 → −5.58 | −5.61 → −7.10 | −8.37 → −10.72 |
| residual °C, 62 → 75 (with drift) | −2.66 → −3.68 | −4.91 → −5.20 | −7.67 → −8.82 |

A purely multiplicative deficit would grow by 1.40× across this span (the ratio
of the rises); a purely constant offset would grow by 1.00×. Observed: 1.28×
uncorrected, 1.15× drift-corrected. **The 62–75 °C span is too narrow to
separate the two hypotheses**, and the answer is sensitive to an ambient
correction the capture did not measure directly at the thermocouples. This is
the one thing left genuinely open by this data — see §6.

### 2e. Verdict on A

The 0.30 duty miss on z2 is a **~12 / 19 / 31 % per-row underestimate of the
adopted matrix's total gain**, temperature-independent to first order over
62–75 °C, projected onto the one zone the inverse can move. It is not a wrong
diagonal specifically, not a missing bottom-zone loss (ruled out on sign), and
not a failure of linearity. The physical story is consistent with the known
provenance problem: the off-diagonals come from a coupling run whose *own*
diagonal was 38.13 / 35.90 / 35.32, and the matrix on the board pairs them with
a smaller diagonal from a different experiment — a mixture that is too weak
overall in exactly the way measured here.

---

## 3. A and B are one defect

Solve the live `G` for the setpoint at which each zone's demanded duty reaches 1:

```
u per unit ΔT = [0.00268, 0.01195, 0.02618]
ΔT at u = 1   = [373.6,   83.7,    38.2   ] °C
```

z2 saturates at ΔT = 38.2 °C. At the earlier firings' ~24 °C ambient that is a
setpoint of **62.2 °C** — the documented boundary, reproduced from the live
matrix without fitting anything to it. At this run's ~29 °C ambient it would be
67.3 °C.

cplval75 measures what z2 actually needs: 0.591 at ΔT = 32.9, 0.759 at 40.8,
0.853 at 46.0. Linear extrapolation puts the real `u₂ = 1` near ΔT ≈ 51–53 °C,
i.e. a setpoint of ~80–82 °C — at or above the 80 °C ceiling this rig will never
be asked to exceed.

**So: `ff_hold_infeasible` above ~62 °C was the matrix's z2-row deficit
announcing itself, not the plant running out of power.** Two things follow.
First, the `COUPLING_JOINT_IDENTIFICATION_CAPTURE.md` §"Bench limits" claim that
the infeasibility is *"a direct, structural consequence of this gain, not a bug
to be engineered around"* is **wrong as stated** — it is a consequence of an
under-identified gain, and this run refutes the plant-limit reading. Second,
that document's acceptance-test step 6 ("no infeasible tick below ~62 °C, ...
consistent with 63.3 °C first-infeasible") is testing the wrong thing: a
correctly identified matrix should push the first-infeasible point out past the
80 °C ceiling entirely, not to 63.3 °C.

---

## 4. The stale-preset diagonal — the premise defect behind `3605f278`

The live board (`GET /api/zones`, read this pass):

```
zone 0  model_k_dc = 39.2459  model_tau_s = 263.8  model_dead_time_s = 52.8
zone 1  model_k_dc = 31.9669  model_tau_s = 269.8  model_dead_time_s = 43.5
zone 2  model_k_dc = 31.6810  model_tau_s = 270.9  model_dead_time_s = 33.9
```

`tools/PcTools/config_presets/tuned_baseline_20260831.json`:

```
zone 0  model_k_dc = 31.9609  model_tau_s = 166.9
zone 1  model_k_dc = 23.4805  model_tau_s = 129.1
zone 2  model_k_dc = 21.7422  model_tau_s = 114.8
```

These are different numbers. `tools/PcTools/src/kilnctrl/coupled_ident.py`'s
`FF_K_DC_DIAGONAL` carries the **live** values (39.2459 / 31.9669 / 31.6810,
commented "read back 2026-09-02") and matches the board exactly today; no
autotune has been accepted since (`tuning_valid=false`, `tuning_seq=0`,
`model_fit_temp_c=-273.15` on all three zones), so the board has carried these
values continuously across the period in question.

`3605f278` §"Verification of each claimed premise" marks `k_dc ≈ 32 °C/duty` as
**confirmed** and cites the preset file. That is a confirmation against a file,
not against the board. Consequences, recomputed:

| claim in `3605f278` / `COUPLING_JOINT_IDENTIFICATION_CAPTURE.md` | with preset diagonal | with live diagonal |
|---|---|---|
| cond(shipped mixed `G`) | **14.194** | **5.508** |
| solved `u` at ΔT = 44.5 °C | `[0.076, −0.118, 2.085]` | `[0.119, 0.532, 1.165]` |
| naive z0 secant ÷ `k_dc` (`48 / 0.145 / k`) | 10.3× | **8.43×** |

I reproduced 14.194 and `[0.076, −0.118, 2.085]` exactly, and only, with the
preset diagonal — so the provenance is not in doubt. On the matrix the board
actually ran there is **no negative duty and no 2.085**, and the condition number
is 5.51, not 14.2, against the "self-consistent" candidate's 4.641. The gap the
mixed matrix was condemned for is roughly a fifth the size it was reported as.

**This does not invalidate the provenance guard `587a34ae`.** Mixing two
experiments' halves is wrong on its own terms and the guard's logic is
independent of the magnitude. But the guard's justification comment
(`zone_coupling_solve.c:222–228`) quotes the 14.19 / negative-duty / 2.085
figures as the harm, and those figures are an artefact. That comment is
overstated and should be corrected — the *real* measured harm is §2's 12/19/31 %
row deficit, which is a better argument for the guard anyway because it is
measured on hardware rather than computed from a file.

**The same stale diagonal is baked into three simulation harnesses**, as
`static const float g_k_dc[NZ] = { 31.9609f, 23.4805f, 21.7422f }`:
`firmware/KilnFW/App/test/sim_credibility_gate.c:75`,
`sim_iter_tune.c:64`, `sim_wide_temp_sweep.c:41`. One consequence is immediate
and same-day: `docs/audits/sim_credibility_gate_coupling_fit_2026-09-10.md`'s
"structural infeasibility" table declares three cross-gains impossible because
they exceed the driving zone's own ceiling — `sim_credibility_gate.c:473`'s
`g_coupling_coeff[i][j] >= g_k_dc[j]`. Against the **live** `k_dc`:

| pair | `coupling_coeff[i][j]` | preset `k_dc[j]` | live `k_dc[j]` | verdict flips |
|---|---|---|---|---|
| (0,1) | 27.32 | 23.48 → infeasible | **31.97 → feasible** | yes |
| (0,2) | 21.72 | 21.74 → limit case | **31.68 → feasible** | yes |
| (1,2) | 22.15 | 21.74 → infeasible | **31.68 → feasible** | yes |

All three flagged infeasibilities disappear. That audit's central structural
conclusion rests on the stale constant. **Reported, not fixed** — it is another
session's live area and this pass changes no code.

Also affected, informationally: `firmware/KilnFW/App/drivers/control/s8_rate_guard_estimate.h:46,61,208`
reasons from "z0's own `k_dc` 31.96" against `27.32 + 21.72 = 49.04`. On the live
39.2459 the ratio is 1.25×, not 1.53×. The qualitative point (cross-gain sum
exceeds own gain) survives; the quoted margin does not.

---

## 5. The DC-gain audit's "~8.4× cross-zone" conclusion — it holds

`3605f278`'s structure was: naive single-zone attribution overstates the gain by
~10.3×, of which ~8.4× is cross-zone heat and ~1.13× a residual model shortfall.
Checked against three genuinely settled three-zone points rather than the
unsettled `coupid6` samples it used:

| plateau | naive z0 secant ΔT₀/u₀ | ÷ live `k_dc` | cross-zone share explains | residual model shortfall |
|---|---|---|---|---|
| 62 °C | 196.0 | 4.99× | 4.48× | 1.114× |
| 70 °C | 231.2 | 5.89× | 5.27× | 1.117× |
| 75 °C | 261.4 | 6.66× | 5.85× | 1.138× |

**The conclusion holds, and the decomposition is confirmed on settled data:
cross-zone heat is the overwhelming term and the residual model shortfall is
1.11–1.14×, matching the audit's own 1.13–1.15× estimate almost exactly.** What
does *not* survive is the specific multiplier: the naive ratio is
operating-point-dependent (4.99× → 6.66× across three plateaus of the same run,
because z0's duty barely moves while its rise does), and the audit's own 10.3×
becomes 8.43× on the live diagonal. **There is no "factor of ten" at any
operating point measured here**, and the residual left over for
asymptote-extrapolation error remains ~1.1×, as the audit said. Its central claim
— that the apparent gain error is a three-zone hold attributed to one zone — is
supported.

Note the shortfall column is §2's z0 row correction (1.114 / 1.117 / 1.138).
`3605f278` called this a "residual model shortfall" and moved on; §2 shows it is
the same deficit that produces the 0.30 z2 miss, so it is not residual noise —
it is the finding.

---

## 6. Verdict on the adopted matrix, and what the joint capture should now do

### Verdict: refuted, on its own pre-registered test

`COUPLING_JOINT_IDENTIFICATION_CAPTURE.md` §Acceptance step 4 fixed the criterion
before any data existed: every zone's settled duty within **0.05 absolute or 15 %
relative, whichever is looser**, of `u_pred = G⁻¹(T_sp − T_amb)`. Applied to the
adopted matrix at all three cplval75 plateaus:

| plateau | zone | `u_pred` | observed | abs err | rel err | |
|---|---|---|---|---|---|---|
| 62 °C | 0 | 0.086 | 0.168 | 0.082 | 48.6 % | **fail** |
| | 1 | 0.391 | 0.371 | 0.019 | 5.2 % | pass |
| | 2 | 0.869 | 0.591 | 0.278 | 47.1 % | **fail** |
| 70 °C | 0 | 0.106 | 0.176 | 0.071 | 40.1 % | **fail** |
| | 1 | 0.486 | 0.479 | 0.007 | 1.5 % | pass |
| | 2 | 1.075 | 0.759 | 0.316 | 41.6 % | **fail** |
| 75 °C | 0 | 0.121 | 0.176 | 0.054 | 31.0 % | **fail** |
| | 1 | 0.548 | 0.545 | 0.003 | 0.5 % | pass |
| | 2 | 1.205 | 0.853 | 0.352 | 41.2 % | **fail** |

Two of three zones fail at every plateau, by 6–9× the tolerance. This was the
coverage-of-1 prediction the matrix owed; it did not pay. **Do not adopt this
matrix for the coupled path. Leave the provenance guard in place** — it is
already refusing it for an unrelated but correct reason, and the refusal is
strictly better than the alternative, as the 70 °C row (`u₂ = 1.075`, saturated)
shows.

### What changes for the joint-identification capture

**Three things must change before it is run:**

1. **The duration floor is understated by ~60 %.** The doc sizes every step at
   `8 × 166.9 s ≈ 22.5 min → 25 min` from the stale preset `model_tau_s`. The
   board's live `model_tau_s` is **263.8 / 269.8 / 270.9 s** — the cplval75 audit
   independently used τ ≈ 265 s. The correct floor is `8 × 270.9 ≈ 2167 s ≈
   **36 minutes**`, per step, plus the 10-minute averaging window. At three
   columns this adds ~35 min of kiln time and is not optional: the whole point of
   the floor is that a truncated step biases `K` low, which is precisely the
   failure §2 is measuring.
2. **Acceptance step 6 must be rewritten or dropped.** §3 shows the 62 °C
   infeasibility boundary is a symptom of the defect being fixed, not a physical
   limit to be reproduced. A correctly identified matrix should push
   first-infeasible past the 80 °C ceiling. Requiring it to land at 63.3 °C would
   reject a correct result.
3. **Add one low-ΔT plateau.** See "the measurement that settles it" below.

**Two things are now already answered and need not be re-run:**

4. **The validation hold (steps 2–3) already exists.** cplval75's 70 °C plateau
   is a settled (6.8 τ, temp std ≤ 0.27 °C, duty flat) three-zone hold at exactly
   the setpoint the doc chose, and the 75 °C plateau is stronger still (10.2 τ,
   std ≤ 0.33 °C). Neither will be used to fit a column-step matrix, so both are
   genuinely held-out. **The pre-registered prediction can therefore be written
   down now, before the capture:** a jointly identified `G` must reproduce

   ```
   u(62 °C) = [0.168, 0.371, 0.591]      ambient 29.19 / 29.07 / 29.03
   u(70 °C) = [0.176, 0.479, 0.759]      (+~2 °C drift by the 75 °C plateau)
   u(75 °C) = [0.176, 0.545, 0.853]
   ```

   within 0.05 absolute / 15 % relative on every zone. That saves ~2 hours of
   kiln time and, more importantly, fixes the prediction against data that
   already exists and cannot be tuned to.
5. **The doc's core design — three single-zone column steps — is vindicated, and
   the reason is now measured.** §2c shows three *proportional* three-zone holds
   are collinear (`cond(U) = 730`) and cannot separate diagonal from
   off-diagonal no matter how many plateaus are added. Adding more three-zone
   holds to this dataset would not help. Single-zone excitation, one column at a
   time, is the only shape that breaks the collinearity. Do not be tempted to
   substitute more hold points for the column steps.

**One thing to note in the doc:** its §"Why this capture exists" motivation table
(cond 14.194, `u = [0.076, −0.118, 2.085]`) should be corrected per §4 to the
live board's 5.508 / `[0.119, 0.532, 1.165]`, so the capture is justified by the
measured 12/19/31 % row deficit rather than by an artefact.

---

## 7. Defects found (reported, not fixed — no code changed this pass)

| # | Where | What |
|---|---|---|
| D1 | `docs/audits/dc_gain_factor_of_ten_2026-09-09.md` §Verification, §4 | `model_k_dc` "confirmed" against a preset file, not the board. cond 14.194 and `[0.076, −0.118, 2.085]` are artefacts; live values are 5.508 and `[0.119, 0.532, 1.165]`. Headline 10.3× is 8.43×. |
| D2 | `firmware/KilnFW/App/test/sim_credibility_gate.c:75`, `sim_iter_tune.c:64`, `sim_wide_temp_sweep.c:41` | `g_k_dc` = 31.9609 / 23.4805 / 21.7422 does not match the board's 39.2459 / 31.9669 / 31.6810. Same for `g_tau_s` if it tracks the preset. |
| D3 | `docs/audits/sim_credibility_gate_coupling_fit_2026-09-10.md` | Its "structural infeasibility" conclusion (3 of 6 cross-gains impossible) is produced entirely by D2; all three flips to feasible on the live diagonal. Another session's live area. |
| D4 | `firmware/KilnFW/App/drivers/control/zone_coupling_solve.c:222–228` | The provenance guard's justification comment quotes D1's artefact figures as the measured harm. Guard logic unaffected; comment overstated. |
| D5 | `docs/COUPLING_JOINT_IDENTIFICATION_CAPTURE.md` §Step sizes | 25-minute step floor derived from stale `model_tau_s` = 166.9 s. Live τ ≈ 271 s → 36 minutes. A capture run to the current floor would reproduce the truncation bias it exists to avoid. |
| D6 | `docs/COUPLING_JOINT_IDENTIFICATION_CAPTURE.md` §Bench limits, §Acceptance step 6 | Treats the 62 °C infeasibility boundary as a physical plant limit. §3 refutes that; step 6 would reject a correct matrix. |
| D7 | `docs/audits/cplval75_settled_holds_2026-09-10.md` §`ff_hold_infeasible` | Reports the flag's absence as new information revising the standing expectation. The flag is inert on this firmware; the section should say so. |
| D8 | `firmware/KilnFW/App/drivers/control/s8_rate_guard_estimate.h:46,61,208` | Design reasoning quotes z0 `k_dc` = 31.96 (live 39.2459). Qualitative point survives, quoted margin does not. |

D1–D3 and D8 are one root cause: **`tuned_baseline_20260831.json` is not the
board's configuration and is being cited as if it were.** It also differs in
`control_mode` (2, the known-inert fuzzy mode) and in `pid_kd`. Nothing in the
repo checks a doc's or a harness's constants against the live board. A
`check_*`-style comparison is not obviously right here (the board legitimately
changes), but a one-line provenance note on that preset — "historical snapshot,
not the live board; read `GET /api/zones`" — would have prevented four of these.

---

## 8. Confirmed vs. inferred

**Confirmed by direct measurement or execution this pass:**
- 1811/1811 samples per zone with `ff_hold_used_matrix = false` (counted from the raw capture).
- 80/80 with `ff_hold_used_matrix = true` in the pre-guard 2026-09-09 capture.
- `coupling_diag_k_dc = 0.0` on all three zones and six non-zero off-diagonals (live `GET /api/zones`).
- `587a34ae` is an ancestor of the flashed image `0dddd435`.
- Live `model_k_dc` / `model_tau_s`, and their disagreement with the preset and with three sim harnesses.
- Every number in §2, §3, §5 and §6's tables (recomputed from the committed TSV and the live matrix).
- cond 14.194 and `[0.076, −0.118, 2.085]` reproduce *only* with the preset diagonal.

**Inferred, and the confidence:**
- That the board carried 39.2459/31.9669/31.6810 continuously through 2026-09-09.
  *High*: `coupled_ident.py`'s constant is dated 2026-09-02 and equals today's
  live value, and no autotune has been accepted (`tuning_valid=false`,
  `tuning_seq=0`). Not directly observable retroactively.
- That the deficit is "understated gain" rather than an unmodelled source.
  *High on sign, moderate on mechanism* — §2d cannot separate a multiplicative
  from an additive term over this span.
- That the real `u₂ = 1` point lies near ΔT ≈ 51–53 °C. *Moderate*: a three-point
  extrapolation past the measured range, mildly concave.

**The single measurement that would settle what is left open:** one more settled
three-zone plateau at a **much lower** setpoint — 45 °C, i.e. ΔT ≈ 16 °C, about
a third of the 75 °C plateau's rise — with the thermocouples' own ambient
recorded at the plateau rather than only at run start. Over 62–75 °C the rise
spans only 1.4×, which is why §2d cannot separate a per-row scale error from a
constant offset. A 45 °C point extends the span to ~2.9× and separates them
decisively: a scale error predicts a z2 residual near −2.9 °C there, a constant
offset predicts −8.4 °C. It is ~50 minutes of low-risk kiln time (ΔT ≈ 16 °C, no
zone above 0.35 duty on the current trend), well clear of every guard, and it
also gives the joint-identification capture a second held-out validation point
in a regime the current dataset does not cover at all.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
