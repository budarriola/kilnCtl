# Adjudication: level-scheduled coupling gain (8cbd9d67) vs. the A1 false-accept pin

**Date:** 2026-09-11
**Subject:** `firmware/KilnFW/App/test/sim_plant.c`'s level-scheduled coupling gain
(commit `8cbd9d67`), and the A1 false-accept regression it caused
(24/660 -> 38/660) in `firmware/KilnFW/App/test/sim_iter_tune.c`.
**Verdict:** the fit is wrong for its only consumer. **Reverted.**

## The question

`8cbd9d67` replaced the constant additive coupling matrix in `sim_plant.c` with a
two-segment schedule keyed on "joint excess" = total commanded duty minus the
largest single commanded duty, calibrated `(0, 1.0) -> (0.373, 0.593) ->
(0.639, 1.173)`. It measurably improved the joint-plateau RMS residual it was
built to fix, and measurably WORSENED the A1 false-accept bar in
`sim_iter_tune.c`, which the implementing agent correctly refused to re-pin.

Two readings were possible:

- **A — the model is better and the harness was being flattered.** A more
  faithful plant legitimately exposes false accepts an unrealistically benign
  plant was hiding (this repo's documented "idealised test input" class). The
  right action would be a visible, argued re-baseline of the pin.
- **B — the fit is wrong.** The extra false accepts are an artifact and the
  change should be reworked or reverted.

The evidence below settles it as **B**, on grounds stronger than "overfitted":
the schedule is *never evaluated inside its calibrated domain* by the only
consumer that drives it, and it *inverts the sign of its own correction* there.

## 1. Where the 14 new false accepts cluster

Both models were rebuilt with per-ACCEPT instrumentation (zone, matched
classes, and per-subscore `n` / `median_normalised` / `bar1_cleared` /
`degraded`) and run at the canonical `mc_runs=220` (660 null comparisons).

| | ACCEPT | REJECT | INSUFFICIENT |
|---|---|---|---|
| constant matrix (`8cbd9d67^`) | 24 | 21 | 615 |
| level schedule (`8cbd9d67`) | 38 | 46 | 576 |

Bar-1-clearing subscore of each false accept:

| | `LAG_S` (s0, ramp) | `ENTRY_PEAK_C` (s1, dwell) | zones |
|---|---|---|---|
| constant matrix | 2 | 22 | z0 20, z1 4 |
| level schedule | **16** | 22 | z0 18, z1 14, **z2 6** |

Every accept in both sets clears at `n == 3` and on exactly one subscore.

The original 24 are reproduced exactly as `8b96b591`'s root-cause pass
described them: `ENTRY_PEAK_C`, dwell, `n_pairs == 3`, mostly zone 0.

**The `ENTRY_PEAK_C` count is unchanged at 22.** The entire +14 is on
`LAG_S` — a *ramp-segment* subscore — which goes 2 -> 16 (8x) and spreads to
zone 2, which produced no false accepts at all under the constant matrix. Per
the discriminator this task was set with, the new failures cluster **somewhere
new**, which points at Reading B.

The `REJECT` count also more than doubled (21 -> 46). In a null experiment
(identical gains on both sides; only the noise seed and the ±15 C start offset
differ) the only correct verdict is `INSUFFICIENT`. Both error directions
rising together means the plant is producing larger spurious run-to-run
divergence, not that the comparator is being tested more honestly.

## 2. The decisive finding: the schedule is never evaluated in its calibrated range

`sim_iter_tune.c`'s `run_firing()` drives the plant through the real
`heater_output.c` 60 s PWM window and feeds `sim_kiln_step()` the **binary
relay state**:

```c
sim_duty[i] = relay_actual[i] ? 1.0f : 0.0f;   // sim_iter_tune.c
```

With binary per-zone duty, joint excess = (number of relays on) - 1, floored at
0 — so it can only ever take the values **0, 1 or 2**. A histogram of every
`sim_kiln_step()` call in the canonical run (2,669,994 steps) confirms it:

| joint-excess level | 0.0 | 1.0 | 2.0 | anything else |
|---|---|---|---|---|
| fraction of steps | 0.9028 | 0.0940 | 0.0032 | **0.0000** |

There is not one sample anywhere in `[0.373, 0.639]`, the range the two
calibration points define. Every evaluation of the schedule in this harness is
pure extrapolation past the high anchor, at

- `scale(1.0) = 1.960` whenever two relays are on, and
- `scale(2.0) = 4.141` whenever all three are.

versus the constant matrix's 1.0. That is a direct 2x-4x amplification of
exactly the neighbour-zone PWM ripple `d63a5591` identified as A1's root cause,
applied only during the (relatively rare) multi-relay ticks — i.e. an extra
burst of divergence injected into ramps, which is precisely where `LAG_S` is
scored and precisely where the 14 new accepts appeared.

## 3. Why this is a modelling error and not just an unlucky domain

The constant matrix is **linear** in duty, so it commutes with PWM averaging:
the time-average of `G·u_binary` equals `G·u_average`, and a matrix calibrated
from time-averaged plateau duties is consistent when fed instantaneous relay
states. A **scheduled** gain is nonlinear in duty and does not commute
(Jensen). Computing the expectation over independent PWM phasing at the
schedule's own calibration points:

| plateau | intended `scale(avg level)` | PWM-realised effective coupling scale |
|---|---|---|
| low joint (u = 0.163/0.210/0.247, level 0.373) | **0.593** | **1.421** |
| 70 C (level 0.656) | 1.209 | 1.873 |
| 75 C (level 0.721) | 1.352 | 1.965 |

At its own defining low-joint point the schedule is supposed to *reduce*
coupling by 41% and instead *increases* it by 42% — it inverts the sign of its
own correction in its only real consumer. The host tests in `test_sim_kiln.c`
do not see this because they feed **fractional** duties to `sim_kiln_step()`
directly, which nothing in the firmware-facing simulation path ever does. That
is this repo's "idealised test input" class again, pointing the opposite way
from Reading A: the *test* is the idealised input here, and the harness is the
honest consumer.

## 4. Independent check of the calibration arithmetic

Re-derived from the cited plateau data, not taken on trust. **The commit's
arithmetic is correct**; the problem is what was done with it.

- Low joint plateau, u = [0.163, 0.210, 0.247]: diagonal `42.731 x 0.163 =
  6.965 C`; nominal coupling `25.42 x 0.210 + 21.52 x 0.247 = 10.654 C`;
  unscheduled prediction `17.62 C` vs measured `13.28 C` (over-prediction
  32.7%, the quoted "~33%"); required scale `(13.28 - 6.965)/10.654 = 0.5928`.
  Matches the committed 0.593.
- `cplval75` plateaus, z0 row: required scales **1.1626 / 1.1658 / 1.1915** at
  levels **0.5394 / 0.6556 / 0.7209**. Mean level 0.6386, mean scale 1.1733.
  Matches the committed 0.639 / 1.173.

But the averaging *is* a defect, and in an unexpected direction. Those three
points are the only data that constrains the schedule's **slope** inside the
high-joint regime, and their least-squares slope is **0.144 per unit level** —
a spread of 2.46%, inside the 2.9% coefficient repeatability the commit itself
cites, i.e. indistinguishable from flat. The committed segment-B slope is
**2.180 per unit level, 15x steeper**, and it is an artifact of drawing a line
between two clusters measured in different regimes rather than anything
measured within either. Averaging the three plateaus discarded the one
measurement that contradicts the fitted slope.

## 5. Stressing the schedule

- **Monotonic?** No. It falls from 1.0 to 0.593 over `[0, 0.373]`, then rises
  without bound. That non-monotonicity is deliberate (it is the sign reversal
  the schedule exists to reproduce) and is not by itself a defect.
- **Derivative at the join:** discontinuous and sign-flipping, `-1.091` ->
  `+2.180`, a jump of 3.27. A trajectory whose level sweeps through 0.373 sees
  a corner. Moot in this harness (no sample ever lands there) but a real hazard
  for any consumer that feeds fractional duty.
- **Outside the fitted range:** unclamped linear extrapolation.
  `scale(1.0) = 1.96`, `scale(2.0) = 4.14` (2.0 is the physical maximum joint
  excess for three zones). At `scale = 4.14`, zone 1 exports
  `4.14 x (25.42 + 10.81) = 150 W` of coupling while its own heater delivers
  `32.4 W` — a 4.6x energy-conservation violation. There is no clamp anywhere.
- **Positive and bounded?** Positive yes (minimum 0.593 at the join). Bounded
  no.

## 6. The single-column invariance claim — verified, and sound

This is the one part of the design that survives. Joint excess is exactly 0
whenever at most one zone is driven, for every duty that zone takes:

- all zones at 0 -> `total = 0`, `max = 0`, excess 0;
- one zone at any duty including 1.0, others 0 -> `total = max`, excess 0;
- two zones where one is exactly 0 -> same, excess 0.

`sim_kiln_step()` computes the level from `u_eff[]`, the same clamped,
fault-adjusted duties the coupling term consumes, so a welded relay raises the
level exactly as a commanded duty would — deliberate and correct. The claim
holds for both fractional and binary duty. Keying a future schedule on joint
excess remains the right *variable choice*; the calibration and the domain are
what failed.

## 7. Action taken, and what the A1 bar now measures

**Reverted `8cbd9d67`** by hand (the three touched files restored to their
pre-commit content; no later commit had touched any of them, and none were
dirty). `check_sim_iter_tune_bars.ps1` is green against the 24/660 pin again,
and `main` is no longer red on an unexplained failure.

The pin is **not** re-baselined. Reading A would have required the new number
to be a more honest measurement of the same effect; instead the extra accepts
come from evaluating a nonlinear correction 2x-4x outside its calibrated
domain, on a variable whose realisation in this harness is a binary relay state
rather than the window-averaged duty the calibration was measured at.
Re-pinning to 38/660 on that basis would have been laundering an artifact.

**Is A1 still a useful gate?** Yes, but only as what it says it is: a
deterministic, exact-count regression tripwire on the *coupling model's
PWM-window ripple*, measured through the decision core. It is not a measure of
the decision core's quality — the 2.0% design target remains unmet and the
24/660 pin remains a known-failure ceiling, unchanged since 2026-09-10. Its
value is exactly that it caught this: a plant change that improved a
steady-state plateau residual while roughly doubling the harness's spurious
verdict rate in *both* directions. A bar that a plant-model change can move at
all is a bar worth keeping, provided nobody re-pins it to whatever it last
measured.

## 8. What would need to be true to try this again

The model class (level-scheduled coupling, keyed on joint excess) is not
refuted by any of the above. To retry:

1. **Drive the schedule from a window-averaged duty**, not the instantaneous
   relay state, so the nonlinearity is evaluated at the same quantity its
   plateau calibration was measured at. This means `sim_plant.c` needs duty
   history state — and that state must be reset in `sim_kiln_reset()`
   (the "reset one side of a pair" class this repo has four recorded instances
   of).
2. **Clamp the schedule to its calibrated range**, or extend it with a
   physically motivated asymptote. An unbounded extrapolation that lets a
   zone's coupling exceed its own heater power is not admissible.
3. **Do not fit a steep segment-B slope** the in-regime data contradicts 15x
   over. The three `cplval75` points support a near-constant high-regime scale
   (~1.17, flat to within repeatability), not a ramp. A step-with-transition is
   the shape the data actually shows.
4. **Re-measure A1 at `mc_runs=220` against the reworked model** before
   proposing any change to the pin, and report the accept/reject/insufficient
   breakdown, not just the accept count — the reject count moving is as
   informative as the accept count.
