# Adopting the `cplval75` row scalars into the simulator: adjudicated, and REJECTED, 2026-09-14

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

`docs/audits/credibility_gate_dwell_offset_2026-09-14.md` (`97e3e720`) concluded
that `sim_credibility_gate`'s failing dwell-offset bar is a real forward-gain
deficit in the checked-in plant model, and named the obvious fix: adopt the
independently-measured `cplval75` per-row scalars **1.12 / 1.19 / 1.31**
(`docs/audits/coupling_deficit_sim_hardware_identity_2026-09-10.md`,
`d2e570ad`/`7c18a11e`) into production constants, which was already shown
non-destructively to close the bar.

The same body of work separately established that **z0's residual crosses zero**
across eight settled plateaus and that **no per-row scalar can produce a sign
change** — so the scalars "describe z2 well, z1 roughly, and z0 not at all" — and
that the additive-duty coupling **model class itself is refuted** on hardware.

This pass was asked to decide whether those two findings are genuinely in tension
and to act.

**Verdict: they are genuinely in tension, the tension is measurable inside the
gate's own two captures, and the scalars are NOT adopted — not for three rows,
not for two.** The gate stays failing; `iter_tune` steps 6-9 stay gated. No
production constant was changed by this pass. What was added is a permanent,
load-bearing warning in `sim_credibility_gate.c`'s own header so the next reader
who reaches for these three numbers finds the measurement that rules them out
before they edit anything.

---

## 1. Method: the gate's pooled dwell offset, broken out per segment

The gate reports **one** dwell offset per zone per capture. `score_replay()`
(`firmware/KilnFW/App/test/sim_credibility_gate.c`) accumulates `dwell_sum[z]` /
`dwell_n[z]` across **every** dwell segment of a capture and divides once:

```c
s->dwell_offset[z] = (dwell_n[z] > 0) ? (float)(dwell_sum[z] / dwell_n[z]) : NAN;
```

Each capture has **two** scored dwell segments. A pooled mean of two cells cannot
show a sign change between them, and that is exactly the quantity the ±1.5 °C bar
is applied to. So the first thing this pass did was break it out.

A scratchpad-only copy of `sim_credibility_gate.c` was built (private `-OutDir`,
against the **real production** `sim_plant.c` and `drivers/control/heater_output.c`,
compiled with the same `host_tests_common_flags.rsp` the checked-in gate uses). It
adds three things and changes nothing else: per-`(zone, segment)` breakout of the
same accumulator, an optional `KILN_ROW_SCALE="a,b,c"` per-row forward-gain scale
applied to `model_k_dc[i]` and row *i* of the coupling matrix together, and a
per-tick TSV dump.

**Fidelity check.** At `KILN_ROW_SCALE=1,1,1` the copy reproduces the checked-in
gate's pooled numbers exactly (−2.104 / −2.752 / −4.816 calibration,
−1.412 / −1.957 / −4.049 hold-out), and at `1.12,1.19,1.31` it reproduces
`coupling_deficit_sim_hardware_identity_2026-09-10.md` §3's published result for
those scalars exactly (+0.642 / +1.450 / +1.350 and +1.473 / +2.468 / +2.455). It
is a faithful superset of the gate, not a re-implementation.

No production file was modified to obtain any number in this document, so there is
no negative test to run against a broken constant here; the fidelity check above
is what binds the analysis to the real `sim_plant.c`. (§6 records the one
production file this pass *does* edit — a comment block — and why it carries no
executable behaviour to break.)

## 2. Where the gate's dwells actually sit: they STRADDLE z0's crossing

This was the question that decides whether the tension is real or illusory. The
answer is unambiguous.

| capture | seg | z0 ΔT | z1 ΔT | z2 ΔT |
|---|---|---|---|---|
| CALIBRATION | 0 | 16.83 | 17.44 | 17.30 |
| CALIBRATION | 1 | 33.16 | 32.29 | 32.11 |
| HOLD-OUT | 0 | 17.34 | 17.81 | 17.63 |
| HOLD-OUT | 1 | 33.56 | 32.68 | 32.43 |

(ΔT = mean recorded dwell temperature minus that capture's own ambient, over the
gate's own scored dwell window.)

`coupling_deficit_sim_hardware_identity_2026-09-10.md` §4 places z0's residual
sign crossing at **ΔT ≈ 23 °C**. The gate's two dwell segments sit at ΔT ≈ 17 and
ΔT ≈ 32.5 — **one on each side of it.** The gate does not live inside the regime
where a scalar is valid. It spans the regime where z0's error changes sign.

**The tension is real.**

## 3. The crossing is visible in the gate's own captures, not just imported

Reducing each of the gate's four dwell cells the way `cplval75` reduces a plateau
— mean duty `u` and mean temperature over the **final 40 %** of the scored dwell,
each capture's own ambient, `G = diag(model_k_dc) + coupling_coeff` from
`sim_measured_zone_constants.h` (`4d42bafe`'s live-board values) — gives the DC
residual `G·u − ΔT_observed` with the dwell-entry transient removed:

| row | CAL seg0 (ΔT 16.8) | CAL seg1 (ΔT 32.5) | HOLD seg0 (ΔT 17.2) | HOLD seg1 (ΔT 33.2) |
|---|---|---|---|---|
| **z0** | **+1.006** | **−1.749** | **+1.143** | **−2.615** |
| z1 | −2.350 | −3.481 | −1.730 | −3.635 |
| z2 | −3.920 | −5.749 | −3.330 | −5.528 |

**z0's DC residual changes sign between the gate's own two dwell segments, on both
captures independently.** z1 and z2 keep one sign and grow with the rise — the
signature of a scale error. z0 does not.

This reproduces the eight-plateau finding from data the gate itself replays, so
the two documents are not two readings of one dataset: the gate's captures
already carried the crossing.

Two sanity notes on the reduction. First, on z1/z2 the DC residual and the
gate's own replayed offset over the same final-40 % window agree closely (e.g.
CAL seg0 z1: −2.350 vs −2.530; HOLD seg1 z2: −5.528 vs −5.656), confirming the
replay does reach its DC asymptote and that the algebraic and dynamic estimators
are measuring the same thing. Second, the **pooled** gate number is not the
final-40 % number — pooling mixes both segments and includes dwell-entry
transient, which is precisely why the crossing is invisible in it.

## 4. What happens to z0 specifically — and it is worse than luck

z0's gate offsets (−2.104 and −1.412) are the *smallest* of the six, and the
hold-out cell already **passes** the ±1.5 °C bar at scale 1. So the question is
whether a scalar that "describes z0 not at all" nonetheless fixes z0's gate
offset, and whether that is luck.

**It is not luck, and it is not a fix. It is a cancellation between two
per-segment errors of opposite sign, and applying 1.12 makes z0's steady-state
accuracy substantially WORSE over half the gate's own range.**

Per-row scale implied by each cell independently (`ΔT_observed / G·u`, final 40 %):

| row | CAL seg0 | CAL seg1 | HOLD seg0 | HOLD seg1 | published scalar |
|---|---|---|---|---|---|
| **z0** | **0.944** | **1.057** | **0.938** | **1.086** | **1.12** |
| z1 | 1.160 | 1.123 | 1.111 | 1.127 | 1.19 |
| z2 | 1.301 | 1.221 | 1.239 | 1.208 | 1.31 |

Read three things off this table.

1. **z0's own requirement straddles 1.0** — it wants the model turned *down* at
   ΔT ≈ 17 and *up* at ΔT ≈ 32.5. A single scalar cannot do both, and **1.12 is
   outside both**. Applying it takes z0's seg0 DC residual from +1.006 to
   `1.12 × 17.812 − 16.806 = +3.14 °C` (CALIBRATION) and from +1.143 to
   `1.12 × 18.370 − 17.226 = +3.34 °C` (HOLD-OUT) — a **3.1× and 2.9×
   degradation** of z0's settled accuracy at the lower dwell, to buy an
   improvement at the upper one.

2. **The pooled bar hides that completely.** Run at `1.12,1.19,1.31`, z0's
   per-segment replayed offsets are +0.771 / +0.514 (CALIBRATION) and
   **+1.759** / +1.186 (HOLD-OUT). The hold-out seg0 cell goes from −0.287 to
   +1.759 — **6× worse** — while the pooled hold-out number moves from −1.412 to
   +1.473, i.e. it *already passed* and ends up marginally **further** from zero.
   z0 contributes essentially nothing to closing the gate. The gate closes
   because z1 and z2 close.

3. **The published scalars are above what the gate's own data asks for on every
   one of the twelve cells**, z1 and z2 included (z1 wants 1.11–1.16 and is given
   1.19; z2 wants 1.21–1.30 and is given 1.31). That is consistent with
   `coupling_deficit_sim_hardware_identity_2026-09-10.md` §3's own note that they
   were fitted at a hotter operating point and overshoot here — but it means
   adopting them is not even the best scale-class correction available for z1/z2;
   it is a systematically hot one.

So: the correction that closes the gate makes the simulator **less** accurate for
z0 across the lower half of the very range the gate measures, and hot for z1/z2.
That is the definition of tuning a gate to pass.

## 5. The alternatives, ranked

**(a) Adopt 1.12/1.19/1.31 for all three rows.** *Rejected.* §4.1 and §4.3: it
degrades z0's settled accuracy 3× at ΔT ≈ 17 and overshoots on every cell. It
also applies a *scale* to a row whose error demonstrably changes sign — a
correction of a shape the data has ruled out, on a model class already refuted on
hardware. It cannot be justified as making the simulator more accurate, which was
the standing condition for adopting anything. A documented validity range does not
rescue it: the gate's own captures are *inside* the range where it fails.

**(b) Adopt for z1/z2 only, treat z0 separately.** *Rejected, and it does not even
work.* Measured directly (`KILN_ROW_SCALE=1,1.19,1.31`): z1 and z2 move to the
same +1.450 / +1.350 and +2.468 / +2.455 as before, and z0 stays at **−2.104
(CALIBRATION, FAIL)** and −1.412 (hold-out, pass). **The gate still FAILS**, 1 of 6
dwell cells. So (b) pays the full cost of importing a hot, out-of-range fit for two
rows and buys nothing: `iter_tune` 6-9 stay gated either way. Its only merit —
being honest about z0 — is available for free by doing nothing.

**(c) Re-derive a correction from the gate's own two captures.** *Rejected on
principle.* The gate's entire value is that the model has seen neither capture's
`actual_c` before being scored against both (the file header calls this a stronger
posture than the plan's minimum ask). Fitting the model to the captures it is
scored on destroys that, and would convert a failing independent check into a
passing tautology. This is the same trap
`coupling_deficit_sim_hardware_identity_2026-09-10.md` §3.1 declined, for the same
reason.

**(d) Leave the gate failing and record precisely why.** **Adopted.** It costs
`iter_tune` steps 6-9, which are gated behind real kiln time regardless. The real
fix is already specified and scheduled — `docs/COUPLING_JOINT_IDENTIFICATION_CAPTURE.md`'s
column-by-column identification, which §6 of the 2026-09-10 audit argued for
precisely because z0's row is nearly unobservable in a *joint* hold (>80 % of z0's
rise is neighbour heat) and that is where the shape error hid. Nothing in this
pass changes that plan; this pass adds the reason the cheap shortcut must not be
taken while waiting for it.

## 6. Weighing what the gate is for

The gate exists to stop `sim_iter_tune.c` / `sim_wide_temp_sweep.c` results being
read as statements about the real kiln. A **passing** gate is what licenses
`iter_tune` steps 6-9 — an automated gain-search that proposes PID changes for a
real kiln with real elements, evaluated in simulation.

Ask what a two-of-three-zones-valid correction buys that consumer. `iter_tune`
moves one parameter per trial, per zone, and scores the result. A simulator whose
z0 forward gain is deliberately biased 3 °C hot at ΔT ≈ 17 and whose z1/z2 gains
are uniformly hot would systematically mis-score z0 trials in the low-temperature
region — and z0 is the top zone, the one this project has repeatedly found hardest
to model (`project_z0_coupling_is_shape_not_scale`,
`project_coupling_failure_is_joint_dwell_specific`). Two zones of three is not
good enough for a decision of that shape. Worse, a **passing** gate removes the
standing signal that the model is known-wrong, and this repo has twice been bitten
by an empirical fit later read as identified physics.

A failing gate that says exactly why is more useful to the next reader than a
passing gate bought with a correction its own data contradicts.

## 7. What was changed

**No production constant.** `sim_measured_zone_constants.h`, `sim_plant.c` and the
coupling matrix are untouched; `git diff` on all three is empty from this pass.

The one production edit is a comment block added to
`firmware/KilnFW/App/test/sim_credibility_gate.c`'s header — the file a future
reader would reach for first when trying to make this gate pass — stating that
the `cplval75` row scalars are a **dated empirical observation about the currently
adopted matrix, not identified physics**, that they are known not to describe z0,
that this gate's own dwells straddle z0's sign crossing, and that a z1/z2-only
adoption leaves the gate failing anyway. It carries no executable behaviour, so
there is nothing in it to negative-test; the analysis it points at is negative-
tested by §1's fidelity check (the scratchpad harness is bound to the real
`sim_plant.c`, and reproduces both the unscaled and scaled published numbers
exactly).

No mechanical `check_*` was added. A check that refused a particular numeric
constant would be pinned to three numbers rather than to the reasoning, would go
stale the moment the matrix is re-identified (which is the *intended* outcome),
and could not be honestly negative-tested against a future correct value. The
standing practice this repo already uses for that shape — a warning at the point
of temptation plus a dated audit — is the right instrument here.

## 8. Verification and check tally

- **`tools/run_all_checks.ps1` (`-ExecutionPolicy Bypass`, foreground): 94 passed,
  0 skipped, 0 failed.** No cross-agent failures observed on this run; the
  intermittent `adaptive_tune_internal.h` / `kiln_cfg_store.c` failures noted
  recently in `docs/audits/check_suite_cross_agent_failures_2026-09-14.md` did not
  reproduce here.
- **`firmware/KilnFW/App/test/check_sim_iter_tune_bars.ps1`: PASS.** A1 null
  experiment reports **ACCEPT 24, REJECT 21, INSUFFICIENT 615** — exactly the
  pinned `24/21/615` (`e78fbc5b`), unchanged. A2 `better 7, unchanged 653,
  WORSE 0`; A5 and A6 clear.
- **KilnFW host tests** (`build_host_tests.ps1`, private `-OutDir`), run twice —
  once at HEAD before this pass's comment edit, once as a clean rebuild into a
  fresh private `-OutDir` after it. `sim_credibility_gate` FAILs informationally
  with identical numbers both times (ramp MAE 5 pass / 1 fail; dwell offset 1 pass
  / 5 fail; dwell-entry peak 10/2/6; noise-floor spread 2/4), as expected from a
  comment-only edit.
- Two cross-agent artifacts seen in those runs, neither attributable to this pass:
  - The first run reported one build failure, **`sim_fuzzy_overshoot`**, whose
    compile log shows `'vswhere.exe' is not recognized as an internal or external
    command` — the same benign toolchain-locator noise that appears on targets in
    the same run that built fine. **It built successfully on the clean rerun**,
    confirming an environment flake rather than a source break.
  - Both runs report an executable-count **MISMATCH** against
    `$totalExpected = 42` (43 built, then 44). `build_host_tests.ps1`'s expected
    count has not been bumped for test executables added by other in-flight
    sessions; `firmware/KilnFW/App/test/test_kiln_cfg_store.c` and
    `test_zones_http.c` are modified in the shared tree by other agents. This pass
    modified exactly one file under that directory — `sim_credibility_gate.c`,
    comment-only — which cannot change the executable count.
- No board was flashed, no firing started, no heat commanded, no board
  configuration written. Another agent's firing on the bench was not disturbed.

Commit hashes cited above, each verified present in this repository
(`git cat-file -t` → `commit`): `97e3e720`, `d2e570ad`, `7c18a11e`, `4d42bafe`,
`e78fbc5b`.

## 9. `iter_tune` steps 6-9

**Still gated.** `sim_credibility_gate` still FAILS (dwell offset 5 of 6, plus z2
CALIBRATION ramp MAE), by decision rather than by omission. Per
`ITER_TUNE_REDESIGN_PLAN.md` sec 6.5's own stated consequence, `sim_iter_tune.c`
and `sim_wide_temp_sweep.c` results remain internal-consistency checks only.

The gate closes when the coupling matrix is **re-identified on hardware** via the
column-by-column procedure in `docs/COUPLING_JOINT_IDENTIFICATION_CAPTURE.md` and
the gate passes with nothing applied on top — not when three imported scalars are
multiplied into it.
