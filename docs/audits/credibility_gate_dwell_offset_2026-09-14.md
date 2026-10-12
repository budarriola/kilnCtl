# Credibility gate: the dwell-offset bar, re-investigated, 2026-09-14

**Addendum, 2026-09-21:** the per-zone tables below were computed before
commit `69118a66` ("sim_plant: fix DT_S trap"), which raised
`SIM_PLANT_DELAY_MAX_STEPS` from 64 to 128 and made truncation refuse loudly
instead of silently clamping. The gate's kiln-scaled dead time is ~76.9 s, so
the 09-14 numbers below used a silently-truncated 64 s delay. Current numbers
at `a8f1e524` (calibration `noise_floor_p7_run1`, hold-out
`noise_floor_p7d_run1`): ramp MAE 3 pass / 3 fail (was 5/1), dwell offset 2
pass / 4 fail (was 1/5), dwell-entry peak 10 pass / 2 fail / 6 unevaluable
(unchanged), noise-floor spread 2 pass / 4 fail (unchanged). Conclusion
unchanged: genuine forward-gain deficit, do not tune to pass, gate left
failing.

Roadmap survey flagged `sim_credibility_gate`'s current verdict — GATE FAILS,
ramp MAE 5/6, dwell offset 5/6 miss at −2.1 to −4.8 °C against ±1.5 °C — as
blocking `iter_tune` steps 6-9. This pass re-derives what the gate compares,
checks whether the sign is informative, re-runs it against the three-node
plant work landed earlier today, checks the obvious confounds, and reaches a
conclusion. No production file was changed by this pass; the gate was
rebuilt into a private `-OutDir` and run, nothing else.

## 1. What the gate actually compares, and where ±1.5 °C came from

`firmware/KilnFW/App/test/sim_credibility_gate.c` replays two real hardware
captures (`logs/coupling/noise_floor_p7_run1.jsonl` CALIBRATION,
`noise_floor_p7d_run1.jsonl` HOLD-OUT — two independent firings of the same
profile, ~3.6h apart, part of the six repeats behind `noise_floor.json`)
open-loop: it zero-order-holds the capture's *recorded PID duty command*
through the real `heater_output.c` PWM window, a relay-lag placeholder, then
`sim_kiln_step()` (the checked-in G1 plant + coupling constants, unified in
`sim_measured_zone_constants.h`), then the MAX31856 quantizer, and compares
the resulting per-zone temperature trace to the capture's own recorded
`actual_c` — never the other way around; the recorded temperature is used
only as the answer key. Dwell offset is the steady-state mean error over
each capture's dwell segments (`kind=2`, temp_bucket ∈ {1,2}).

The ±1.5 °C bar is **not a guess re-derived here** — it is quoted verbatim
from `ITER_TUNE_REDESIGN.md` sec 6.5 (`docs/audits/
sim_credibility_gate_2026-09-09.md`'s "Pass criterion" section, itself
citing the plan), alongside the standing owner rule that sub-0.5 °C
differences are not worth chasing (`feedback_ignore_sub_half_degree_effects.md`)
— 1.5 °C is 3x that floor, not an arbitrarily tight number. The bar's
provenance is a plan decision, not a fitted constant, so the "bound
justified against a test constant is justified against nothing" concern
does not apply directly to it — but see sec 5 below for where a *different*
value (the forward-gain scalar) was checked to make sure the same trap
wasn't happening one level down.

## 2. The sign is fully informative, and it is not new information

All five dwell-offset misses are **negative** (sim runs cooler than the
recorded dwell) and the magnitude order is **z0 < z1 < z2** on both
captures. This is not scattered noise; a one-sided, zone-ordered offset is
a bias in the model's forward gain, not a tolerance problem. Physically: a
negative sim-minus-recorded dwell offset means the simulator, driven with
the *exact recorded duty*, settles **below** where the real board actually
sat — the model does not turn engine input into enough steady-state
temperature rise, at the recorded operating point.

This sign and this exact zone ordering were already found and named
independently, twice, before this pass:

- `docs/audits/sim_credibility_gate_real_cause_2026-09-10.md` sec 5, which
  first isolated the residual after fixing the coupling model class (sec 3
  below) and matched it to —
- `docs/audits/coupling_deficit_sim_hardware_identity_2026-09-10.md`, which
  independently measured the same deficit as a **forward-gain** shortfall
  (row scalars ~1.09/1.12/1.24, matching the previously-published
  `cplval75` scalars 1.12/1.19/1.31 to within 0.03-0.12 °C-equivalent, all
  same sign) from a *different* measurement (plateau mean duty/ΔT fits, not
  this gate at all).

Two independent measurements (a replay gate and a plateau-fit) agreeing on
sign, rough magnitude, and zone ordering is exactly the corroboration a
single anecdote would lack — this is a real, external, previously-quantified
modelling gap, not an artifact of this gate's own scoring.

## 3. Re-run against tonight's three-node plant work

Checked whether `sim_credibility_gate.c` exercises the three-node model
landed today (`7729f3c8` opt-in element/load/sensor split,
`sim_plant_three_node_step()`; `a01a0f43` temperature-dependent `s(T)`;
`6f48c8d3` mistune-factor tuning path). It does not, and cannot yet:

- The gate's replay calls `sim_kiln_step()` exclusively (grep-confirmed —
  the only plant-stepping call in `sim_credibility_gate.c`), which is the
  **legacy single-node, multi-zone-coupled** function. `sim_node_model_t`
  defaults to `SIM_NODE_LEGACY` and the gate never sets it otherwise.
- `sim_plant_three_node_step()` operates on one zone's `sim_plant_state_t`
  in isolation (element/load/sensor for a *single* thermal mass) — there is
  no multi-zone, coupled three-node kiln step yet. `7729f3c8`'s own commit
  message says so explicitly: "Stopped after WI-1... WI-2 through WI-10 not
  started, pending that blocker" (a concurrent session's unrelated build
  break in `firing_score.c`/`firing_compare.c`, unrelated to plant model
  work). `a01a0f43`'s `s(T)` scale and `6f48c8d3`'s mistune path are both
  single-zone test fixtures (`sim_high_temp.c`, `sim_mistune.c`), exercised
  by their own dedicated test files, not by `sim_credibility_gate.c` or by
  any multi-zone coupled kiln step.
- Rebuilding and re-running `sim_credibility_gate.exe` from `HEAD` (private
  `-OutDir`, via `build_host_tests.ps1`) reproduces the legacy-path numbers
  exactly, confirming no three-node code is silently reached:

  | | ramp MAE (bar 3.0) | dwell offset (bar ±1.5) |
  |---|---|---|
  | CALIBRATION z0/z1/z2 | 1.481 / 1.710 / 3.367 | −2.104 / −2.752 / −4.816 |
  | HOLD-OUT z0/z1/z2 | 1.489 / 1.335 / 2.780 | −1.412 / −1.957 / −4.049 |

  (z2 ramp MAE 3.367 also fails its own 3.0 bar on CALIBRATION — the "5 of
  6" ramp-MAE tally the roadmap survey quotes is the z2/CALIBRATION cell
  failing, everything else passing.)

**Conclusion for this item: the legacy single-node path is what the gate
exercises, and that is the whole explanation for why tonight's plant work
does not move these numbers at all** — not because the three-node model
was tried and found not to help, but because it is not wired into the
multi-zone coupled path this gate calls, and per WI-1's own commit message
is not even attempted past WI-1 yet.

## 4. Confounds checked

- **Ambient reference**: `sim_credibility_gate.c`'s own header comment
  states `ambient_c` is derived per-capture from each capture's own
  first-valid `actual_c` (mean across zones), not a shared constant — the
  two captures differ in ambient by only ~0.26 °C, far too small to explain
  a 2-5 °C offset, and this is intentional, documented leakage (the file
  says so), not an oversight.
- **Dwell window alignment**: the offset is computed only over the
  harness's own classified dwell segments (`kind=2`, `in_band=1.000` for
  every scored class in this run's `firing_score_from_capture` output),
  not a hand-picked window.
- **Sensor vs. load node**: moot under the legacy single-node path — there
  is only one node, so no comparison could be reading the wrong one. This
  *is* relevant to the still-open dwell-entry **peak** bar (which needs a
  sensor/load split to explain lead/lag), not to the dwell **offset** bar,
  which is a steady-state gain question the legacy model already computes
  a definite (if too-small) answer for.
- **Coupling model class**: this was the confound that dominated the
  original 2026-09-09 run (−13 to −18 °C offsets) and was fixed in
  `d63a5591` (2026-09-10) — the exchange-vs-additive model-class mismatch
  documented in `sim_credibility_gate_real_cause_2026-09-10.md`. That fix
  is already in production `sim_plant.c` and is what today's −2 to −5 °C
  numbers reflect; it is not a live confound any more, just prior history
  worth naming since the roadmap survey's failing numbers are this
  post-fix state, not the original one.

None of the four confounds explain the residual. It survives every check.

## 5. Conclusion: the model is wrong, and the missing physics is already named and quantified

This is **the model is wrong** — a real, quantified forward-gain deficit in
`model_k_dc`/the additive coupling matrix, independently corroborated by
`coupling_deficit_sim_hardware_identity_2026-09-10.md`'s plateau-fit
measurement and the previously-published `cplval75` scalars. It is not the
bar (±1.5 °C is a plan decision well above the owner's 0.5 °C
don't-chase floor, and the sign/magnitude match an independent measurement,
not an artifact of this harness), and it is not a comparison confound (sec
4). The three-node work landed today does not yet supply the fix — it is
not wired into the multi-zone coupled path this gate exercises, and its own
commit message says the remaining work items (WI-2 through WI-10) were not
attempted this session.

`sim_credibility_gate_real_cause_2026-09-10.md` sec 5 already demonstrated,
non-destructively (a scratchpad-only variant, not adopted here either),
that applying the independently-measured forward-gain scalars closes the
dwell-offset bar (offsets move to +0.6 to +2.5 °C, six of six pass on
CALIBRATION, crossing to slightly positive because those scalars were
fitted at a different operating point) while leaving ramp MAE passing. That
document explicitly declined to adopt the scalars into the gate itself,
because doing so from calibration-fitted numbers would compromise the
gate's non-circularity posture. This pass reaches the same position: the
fix (a corrected `model_k_dc`/coupling forward gain, derived from a
**held-out** measurement independent of the two gate captures — the
`cplval75` plateau data already is one) is known in outline and already
demonstrated to work, but adopting it into `sim_credibility_gate.c` or
`sim_measured_zone_constants.h` is a deliberate, reviewed change with its
own re-baseline, not something to fold into an investigation pass, and it
was explicitly out of scope here (`kiln_cfg_store.c`/`adaptive_tune*` and
the board are owned by other in-flight sessions tonight; the forward-gain
constants are adjacent but not in this pass's assigned files).

## What remains open

- **Dwell-entry peak bar**: still the one bar `sim_credibility_gate_real_cause_2026-09-10.md`
  left genuinely unresolved (2-3 °C of excess sim overshoot unexplained
  after accounting for the gain-bias contribution). The three-node
  element/load/sensor split landed today (`7729f3c8`) is a plausible
  candidate mechanism for exactly this bar (a sensor that settles
  differently from the load explains lead/lag on entry, not steady-state
  offset) — but it is not wired into a multi-zone coupled step yet, so this
  remains untested, not ruled out.
- **Noise-floor spread bar**: still not judged, same as
  `sim_credibility_gate_real_cause_2026-09-10.md` sec 6 — it cannot be
  judged independently of the peak bar.

## `iter_tune` steps 6-9: still gated

The dwell-offset bar's cause is understood, quantified, and independently
corroborated, but **not fixed in the checked-in model** — this pass changed
no constant. `sim_credibility_gate` still FAILS overall (dwell offset 5/6,
z2 ramp MAE 1/2), so per `ITER_TUNE_REDESIGN.md` sec 6.5's own stated
consequence, `sim_iter_tune.c`/`sim_wide_temp_sweep.c` results remain
internal-consistency checks only, and `iter_tune` steps 6-9 remain gated.
Adopting the forward-gain fix (owned by whichever session next touches
`model_k_dc`/coupling constants, informed by
`coupling_deficit_sim_hardware_identity_2026-09-10.md`'s held-out scalars)
is the identified path to closing it; the peak bar needs the multi-zone
three-node work (WI-2 onward) to even be tested.

## Verification

`sim_credibility_gate.exe` rebuilt from `HEAD` (commit at time of this run:
working tree HEAD, no production file modified by this pass) into a
private `-OutDir` via `build_host_tests.ps1`, run against the same two
named captures and `noise_floor.json`, reproducing the numbers in sec 3
above exactly. No check or assertion was added by this pass, so there is
nothing here to negative-test (same posture as
`sim_credibility_gate_real_cause_2026-09-10.md`'s own method note).
`sim_iter_tune.exe` was not re-run by this pass (no plant/model constant
was touched); the 24/21/615 pin is unaffected by an investigation-only
pass that changed no code.

Commit hashes cited above, verified present in this repository:
`d63a5591`, `7729f3c8`, `a01a0f43`, `6f48c8d3`, `d2e570ad` (all `git
cat-file -t` → `commit`).
