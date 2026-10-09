# sim_credibility_gate: iterative coupling fit + h=1 re-examination, 2026-09-10

Follow-up to `docs/audits/sim_credibility_gate_2026-09-09.md`'s two open
items. No hardware was touched; a 3.3h bench heating capture was running
during this pass and was not interacted with. Simulation and host-only work.

> **CORRECTION, same day** (`docs/audits/cplval75_coupling_verdict_2026-09-10.md`
> §4, D2/D3): this file's central "structural infeasibility" finding below
> (item 2 — "Two of the six measured cross-gains equal or exceed the driving
> zone's own single-zone ceiling") was computed against
> `g_k_dc = {31.9609, 23.4805, 21.7422}`, a literal hand-copied from
> `tools/PcTools/config_presets/tuned_baseline_20260831.json` — a **preset
> file, not the live board**. The board's actual `model_k_dc` (`GET
> /api/zones`) is **39.2459 / 31.9669 / 31.6810**.
>
> Recomputed against the live `k_dc` (production values now unified into
> `firmware/KilnFW/App/test/sim_measured_zone_constants.h`, shared by
> `sim_credibility_gate.c`/`sim_iter_tune.c`/`sim_wide_temp_sweep.c` so this
> exact drift cannot recur silently across three separate literals):
>
> | pair (i,j) | `coupling_coeff[i][j]` | preset `model_k_dc[j]` | live `model_k_dc[j]` | feasible now? |
> |---|---|---|---|---|
> | (0,1) | 27.32 | 23.48 → infeasible | **31.97** | **yes** |
> | (0,2) | 21.72 | 21.74 → limit case | **31.68** | **yes** |
> | (1,2) | 22.15 | 21.74 → infeasible | **31.68** | **yes** |
>
> **All three flagged/limit-case pairs are feasible on the live diagonal —
> zero of six cross-gains are structurally unreachable.** Rebuilt
> `sim_credibility_gate.exe` against the corrected header and re-run against
> the same two captures confirms this: the printed feasibility table below
> now shows no `<-- UNREACHABLE` line for any of the six pairs (verified
> 2026-09-10, this pass).
>
> **What survives:** the gate still FAILS overall (ramp MAE and dwell offset
> both still miss their bars by a wide margin on both captures, all three
> zones) — that failure is real and not an artifact of `g_k_dc`, since the
> ramp/dwell scoring doesn't depend on the coupling-fit feasibility table at
> all. Item 1 (the `h=1` own-duty-ceiling argument) is also unaffected — it
> is a statement about `model_k_dc` being the correct steady-state gain by
> construction, independent of which value it happens to hold. **What does
> NOT survive:** "the gap is... two of six measured cross-gains exceeding
> what any coupling conductance could deliver" (§"Gate re-run", reason (b))
> is no longer a correct explanation for the gate's failure on the live
> constants — with zero infeasible pairs, the residual gap items 1+2 leave
> unexplained is now the ENTIRE gap, not a fraction of it structurally capped
> by an unreachable target. The gate's failure needs a different explanation
> than "2 of 6 cross-gains are physically unreachable" — that reason is gone.
>
> **Stated plainly, so this is not left implicit:** this file's headline
> "structural infeasibility" conclusion (item 2 below) is RETIRED. It does not
> survive in any weakened or partial form — zero of six cross-gains are
> unreachable on the live diagonal, not "fewer than two." The gate's continuing
> FAIL is real (see "What survives" above) but now has an **unexplained
> cause**: this document no longer supplies a reason for it, and neither does
> any other document as of this correction. Do not read the retired conclusion
> as still explaining the FAIL in a reduced way — it explains none of it.
> Finding a new explanation for the gate's failure is open work, not something
> this correction pass did.

## Original findings (as written 2026-09-10, before the correction above)

## Item 2 (coupling): iterative fit implemented and run

Added `sim_kiln_coupling_fit_iterative()` (`sim_plant.c`/`.h`) per plan sec
6.2: seeds from the existing algebraic first cut
(`sim_kiln_coupling_from_cross_gain()`), then coordinate-descends each
off-diagonal `coupling_w_per_c[i][j]` by actually driving `sim_kiln` with
zone `j` alone at duty=1.0 to quasi-steady-state (settle time = slowest
zone's dead time + 8 time constants) and comparing zone `i`'s simulated
rise against the measured `coupling_coeff[i][j]`, damped-proportional
update, re-swept until every pair is within 10% or a 50-sweep cap.

**Anti-circularity**: the fit's only physical inputs are `g_k_dc`/
`g_tau_s`/`g_dead_time_s` and `g_coupling_coeff[][]` — the same checked-in
bench measurements `sim_iter_tune.c`/`sim_wide_temp_sweep.c` already use. It
drives `sim_kiln` with synthetic step duties and reads back `sim_kiln`'s own
element temperatures; it never opens either capture file. `sim_credibility_gate.c`
now calls this once (`ensure_coupling_fitted()`) before scoring either
capture, so calibration and hold-out are both scored against the same fit,
same posture as before.

**Convergence: partial, and for a structural reason, not a tuning
shortfall.** In this model, `coupling_w_per_c[i][j]` lives only in zone
`i`'s own row — it is *not* a symmetric two-way resistor, so driving zone
`j` alone bounds zone `i`'s reachable rise at zone `j`'s own (loaded)
steady-state temperature, which itself can never exceed `model_k_dc[j]`.
Two of the six measured cross-gains **equal or exceed the driving zone's
own single-zone ceiling**:

| pair (i,j) | coupling_coeff[i][j] | model_k_dc[j] | feasible? |
|---|---|---|---|
| (0,1) | 27.32 | 23.48 | **no** — exceeds ceiling |
| (0,2) | 21.72 | 21.74 | only in the g→∞ limit |
| (1,0) | 14.30 | 31.96 | yes in isolation, but see below |
| (1,2) | 22.15 | 21.74 | **no** — exceeds ceiling |
| (2,0) | 8.33 | 31.96 | yes |
| (2,1) | 12.42 | 23.48 | yes |

No finite, non-negative conductance can make a passive receiver's induced
temperature exceed its (lossy) source's own temperature — the two flagged
pairs are unreachable by this model class at *any* conductance, not a
fit-quality problem. `sim_credibility_gate.c` now prints this table (with
the `<-- UNREACHABLE` flag) every run.

(1,0), nominally feasible on its own, also failed to converge (pinned at a
200 W/C numerical safety ceiling — see below): fitting (0,1) to its
excessive target drove `coupling_w_per_c[0][1]` up to ~74 W/C, which sits in
row 0's *own* balance as an outgoing loss whenever zone 0 is driven (the row
is shared across every column, not per-pair), crushing zone 0's own
steady-state when *it* is the driven zone and starving (1,0)'s fit of a
usable source temperature. This is a real, honest coupling between the
per-pair fits in a row-asymmetric conductance model, not a bug: 50 sweeps
were run (cap), the fit did not converge, and forcing it further would only
mean growing `coupling_w_per_c[0][1]` (already infeasible) without limit.

**Negative-tested (found and fixed during this pass, in production
`sim_plant.c`, not a test-local copy):** the first version of the update
step used an unbounded 0.1x-5x per-sweep multiplier with no finiteness
check. Confirmed RED by running the actual fit: `sim_kiln_step()`'s explicit
Euler integration diverged to NaN by sweep 2-3 for the aggressive pairs, and
a second, separate bug let it slip past undetected — `if (rel_err >
max_rel_err) max_rel_err = rel_err;` never updates on a NaN `rel_err`
(NaN comparisons are always false), so the diverged entry was silently
excluded from the convergence check and the gate printed a false
`CONVERGED`. Fixed by tightening the per-sweep multiplier to 0.5x-2x, adding
an explicit `!isfinite(sim)` branch that forces another sweep and backs the
offending entry off by half, and capping `coupling_w_per_c[i][j]` at 200 W/C
(comfortably inside this zone set's Euler-stability bound of roughly
`2*tau_min/dt ≈ 230` at `dt=1s`, `tau_min=114.8s`) so a bad sweep can never
hand the next measurement an unstable conductance. Restored the buggy
version by hand afterward and reran to confirm the NaN/false-CONVERGED
symptom reproduced, then reapplied the fix — `git diff` on the intermediate
revert was empty before the fix was reapplied.

## Item 1 (h=1 mapping): re-examined, found NOT to be the bug

The 2026-09-09 audit's open item 2 asked whether `h = loss_coeff_w_per_c =
1.0` was "the right free-scale choice" or whether `model_k_dc` needed
reinterpreting. Checked against the algebra in `sim_plant_step()`/
`sim_kiln_step()`: for any `s > 0`, rescaling `h' = h*s`,
`heater_power_w' = heater_power_w*s`, `thermal_mass_j_per_c' =
thermal_mass_j_per_c*s` leaves the ODE `C dT/dt = P*duty - h*(T-T_amb)`
identical after dividing through by `s` — the trajectory is provably
independent of which `h` is picked, as long as power and mass are rescaled
with it. `sim_plant_from_zone_cfg()` already does exactly this (picks
`h=1`, sets `heater_power_w=model_k_dc`, `thermal_mass_j_per_c=model_tau_s`),
so **there is no alternative `h` that raises the ~50-60°C own-duty
ceiling** — it is fixed entirely by `model_k_dc` itself, which is already
the correct steady-state gain by construction. `sim_iter_tune.c`'s own
2026-09-09 comment already reached the same conclusion independently
("literal G1 mapping caps this model... — the profile below therefore stays
inside 20-36°C") and worked around it by narrowing scope rather than by
adjusting `h`. This diagnosis (implementing against it) was NOT pursued;
`sim_plant.c`'s mapping is unchanged. The real, and only, lever available to
close the gap between a ~50-60°C own-duty ceiling and a 60°C+ recorded
dwell is cross-zone coupling — which item 2 above shows is itself
structurally short by an amount the measured cross-gain matrix cannot
supply for at least 2 of 6 zone pairs.

## Gate re-run: before / after

Same two captures, same bars (plan sec 6.5, unchanged, not loosened):
`logs/coupling/noise_floor_p7_run1.jsonl` (calibration),
`logs/coupling/noise_floor_p7d_run1.jsonl` (hold-out).

| | ramp MAE (bar 3.0C) | dwell offset (bar +/-1.5C) |
|---|---|---|
| **before** (algebraic first cut, 2026-09-09) | 8.5-11.2C, all zones, both captures | -15 to -18C, all zones, both captures |
| **after** (iterative fit, this pass) | 9.4-10.9C, all zones, both captures | -17.0 to -17.9C, all zones, both captures |

**Result: GATE STILL FAILS, on both captures, all three zones, on both
metrics — essentially unchanged, marginally worse on dwell offset.** The
iterative fit did not close the gap because the gap is not primarily a
fit-quality problem: it is the combination of (a) the own-duty ceiling
(item 1, confirmed correct and unfixable via `h`) and (b) two of six
measured cross-gains exceeding what any coupling conductance could deliver
from their driving zone (item 2's new finding). Per plan sec 6.5 and this
gate's own standing instruction, this is reported as a failure, not
loosened or forced to pass. `sim_iter_tune.c` / `sim_wide_temp_sweep.c`
results remain not evidence about the real kiln.

## What would actually close this

Not another numerical fit against the current `coupling_coeff[][]`/`k_dc[]`
pair — two of those six measured numbers are inconsistent with each other
under this model class by construction (a cross-gain cannot exceed the
source's own gain in a passive network), regardless of algorithm. Closing
this needs either: a joint identification of `k_dc` and the coupling matrix
from data recorded simultaneously (so the numbers are mutually consistent
by measurement, not just by curve-fit) — `docs/COUPLING_JOINT_IDENTIFICATION_CAPTURE.md`
already describes this capture and is the standing recommendation — or a
model class that lets coupling supply more heat than a linear resistor
network to a single receiver (e.g., an explicit two-way conductance plus a
separate radiative/convective path), which is a modeling decision beyond
this gate's scope to make unilaterally.

## Reproduce

```
firmware\KilnFW\App\test\build\sim_credibility_gate.exe ^
  logs\coupling\noise_floor_p7_run1.jsonl ^
  logs\coupling\noise_floor_p7d_run1.jsonl ^
  tools\PcTools\config_presets\noise_floor.json
```
Build (PowerShell, not git-bash — see CLAUDE.md's idf.py note, same MSVC
response-file include set `build_host_tests.ps1` uses):
```
cl @App\test\build\host_tests_common_flags.rsp /std:c11 /D_CRT_SECURE_NO_WARNINGS ^
   /Fe:App\test\build\sim_credibility_gate.exe ^
   App\test\sim_credibility_gate.c App\test\sim_plant.c App\drivers\control\heater_output.c
```
