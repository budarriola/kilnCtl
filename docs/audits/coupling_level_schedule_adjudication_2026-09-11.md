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

## 9. Second attempt (2026-09-11, same day) — all four retry conditions met, still worse

A second attempt at the level-scheduled coupling term in `sim_plant.c` was made
the same day, satisfying **all four** of section 8's conditions before being
measured. **This was a correct implementation of the retry plan — it still
failed.** That is the point of recording it: this is not a botched attempt to
be redone more carefully, it is evidence about the model class.

What was done, condition by condition:

1. **Window-averaged duty.** A per-zone rolling duty-history ring (capacity
   64) was added to `sim_kiln_state_t`, zeroed by the existing whole-struct
   `memset` in `sim_kiln_reset()` (the reset-one-side-of-a-pair class this
   repo tracks was explicitly checked and is not present here — one `memset`
   covers both the new field and everything else). Verified working: driving
   a binary PWM pattern whose ON-fraction matched the low-joint calibration
   point (u = [0.156, 0.219, 0.25], 32-tick period dividing evenly into the
   64-tick window) produced a window-averaged joint-excess of **~0.375** —
   inside the calibrated `[0.373, 0.639]` domain, rather than collapsing to
   the `{0, 1, 2}` values section 2 above showed the binary-fed schedule was
   actually evaluated at.
2. **Clamped.** Segment B was flattened at the plateau mean (scale 1.173) as
   a hard ceiling — no extrapolation past it. (The first attempt reached
   scale 4.14, a 4.6x energy-conservation violation per section 5 above.)
3. **No unsupported slope.** No segment-B slope was fitted at all; it was
   implemented flat-and-clamped above a short interpolation bridge, per
   section 8 point 3's "step-with-transition" recommendation. Section 4's
   finding — the three `cplval75` points' in-regime slope (0.144/unit) is
   statistically indistinguishable from flat (2.46% spread inside 2.9%
   coefficient repeatability) — was taken as the reason, not merely followed.
4. **Re-measured A1 at `mc_runs=220`, full breakdown, before proposing any
   pin change.**

Tests added: single-column invariance (all zones off; one zone at duty 1.0
with others at 0; a zone at exactly 0 beside a driven one — all must give
excess 0, matching section 6's proof), and a window-averaging regression
assertion that the level lands near 0.373 rather than snapping to an integer.
A negative test (reintroducing an unbounded slope) was confirmed RED, then
reversed by hand with an empty `git diff` against the intended state —
proving the test actually exercises the clamp rather than passing vacuously.

**Result: A1 went to 43/660 (6.52%).** Worse than the pinned ceiling (24/660,
3.64%) **and** worse than the first, already-rejected attempt (38/660,
5.76%). The pin was correctly left untouched.

Everything was reverted by hand and confirmed byte-identical to pre-attempt
`HEAD` (`git diff` empty against the touched files); the host suite returned
to 36/36 and `check_sim_iter_tune_bars.ps1` back to the passing 24/660.
**Nothing from this attempt was committed** — the code no longer exists
anywhere except as reconstructed in this section, which is why this section
exists.

### Conclusion

Evaluating the correction at the right quantity — window-averaged duty,
matching the calibration exactly, per section 2's own diagnosis — **did not
remove the ripple-amplification effect that sank the first attempt; it only
moved where it lands.** Both attempts made A1 worse, not better, despite
addressing that attempt's previously-identified defect. This points at the
**model class** (a level-scheduled, joint-excess-keyed coupling gain) being
wrong for this harness's false-accept sensitivity — not at either attempt's
calibration, clamping, or evaluation domain, all of which were correct the
second time.

### Precondition for a third attempt

**Do not retry a level-scheduled coupling term without first explaining why
43/660 resulted despite all four of section 8's conditions being satisfied.**
A third attempt that only re-checks calibration, clamping, slope support, or
evaluation domain will be repeating the second attempt's already-satisfied
conditions, not addressing the open question. The open question is why a
correctly-scheduled, correctly-clamped, correctly-evaluated nonlinear
correction still degrades A1 in both directions it has been tried.

### A hypothesis, tested and PARTIALLY SUPPORTED (see section 10)

One candidate explanation, offered explicitly as a hypothesis to be tested,
not as a conclusion: **both attempts raised the effective coupling scale
whenever two or three zones are on simultaneously** (scale > 1 in that
regime — 1.96/4.14 in the first attempt, 1.173 clamped in the second), and
under the real 60 s PWM window used by `sim_iter_tune.c`, multi-relay
overlap is common during joint operation. If A1's false-accept sensitivity
tracks coupling **magnitude** during multi-relay intervals rather than the
schedule's particular shape, then *any* schedule of this form that sits
above 1.0 at high joint level will worsen A1 regardless of how faithfully it
is calibrated to the plateau data — because the plateau data (steady-state,
time-averaged) is exactly what's insensitive to the PWM-ripple mechanism A1
is measuring.

The discriminating experiment, **not run here**: replace the schedule with
the original constant matrix scaled by a single flat factor equal to the
second attempt's clamped high-joint scale (1.173), with no joint-excess
dependence at all, and re-measure A1. If that also degrades A1 by a
comparable amount, the hypothesis is supported — the defect is in raising
coupling magnitude during multi-relay intervals per se, independent of
scheduling — and any future retry needs to address *that*, not the
schedule's shape. If flat-scaling does *not* degrade A1 comparably, the
hypothesis is wrong and the schedule's specific shape (its transition,
non-monotonicity, or interaction with the window-averaging itself) is
implicated instead, which would point back at the join-region dynamics
rather than the plateau magnitude.

This hypothesis is recorded because it is falsifiable with one cheap run and
because it would materially change what a third attempt should even try;
it is not recorded as an explanation to be assumed true.

## 10. The discriminating experiment, run (2026-09-11, same day)

Section 9's flat-scale experiment was run. Method: `sim_plant.c`'s
`power_couple_w += cfg->coupling_w_per_c[i][j] * u_j;` line (the constant
matrix's only point of application) was temporarily multiplied by a bare
`float` factor — no schedule, no new state, no joint-excess dependence —
and `sim_iter_tune.c`'s Part-2 null-experiment loop was temporarily
instrumented to print, on every `ACCEPT`, which subscore cleared bar-1, its
`n`, and the zone (mirroring section 1's instrumentation). Both changes were
reverted by hand immediately after data collection; `git diff` on both
touched files (`firmware/KilnFW/App/test/sim_plant.c`,
`firmware/KilnFW/App/test/sim_iter_tune.c`) is empty, and a final re-run of
`check_sim_iter_tune_bars.ps1` after reverting reproduced 24/660 exactly,
confirming the revert is clean. `tools/run_all_checks.ps1` is green at
94/94. No code was committed for this section — only this write-up.

Control at factor **1.0** (must reproduce the pin exactly, checked first):
`24/660 (3.64%)` — `ENTRY_PEAK_C 22 / LAG_S 2`, zones `z0 20 / z1 4`, all at
`n == 3`. **Exact match to the pinned baseline**, so the instrumentation and
build are trusted for the rest of this section.

| factor | ACCEPT/660 | REJECT | INSUFFICIENT | `LAG_S` | `ENTRY_PEAK_C` | zones |
|---|---|---|---|---|---|---|
| 0.85 | 20 (3.03%) | 29 | 611 | 0 | 20 | z0 20 |
| **1.0 (control)** | 24 (3.64%) | 21 | 615 | 2 | 22 | z0 20, z1 4 |
| **1.173** | **38 (5.76%)** | 48 | 574 | 11 | 27 | z0 34, z1 4 |
| 1.35 | 18 (2.73%) | 54 | 588 | 3 | 15 | z0 15, z1 3 |

All accepts at every factor cleared at `n == 3` on exactly one subscore, same
as the pinned baseline and both schedule attempts.

### Reading the numbers

**Headline match, at the matched scale.** At factor 1.173 — the exact clamp
value the second schedule attempt settled on for high joint level — flat
scaling gives **38/660**, matching the *first* schedule attempt's 38/660
almost exactly (and landing between the two schedule attempts' 38 and 43).
A pure, uniform, always-applied magnitude increase of this size produces a
comparably-sized A1 regression to a level-scheduled gain that only reaches
that magnitude during multi-relay overlap. That is consistent with the
hypothesis: raising coupling magnitude during joint operation degrades A1
regardless of whether the increase is scheduled or constant.

**But the cluster shape does not match either schedule attempt.** Section 1's
table for the schedule showed `ENTRY_PEAK_C` **unchanged at exactly 22** (all
+14 new accepts landing on `LAG_S`, spreading into z2 for the first time: z0
18, z1 14, z2 6). The flat-scale run at the same headline size instead moves
**both** subscores (`ENTRY_PEAK_C` 22 -> 27, `LAG_S` 2 -> 11) and shows **no
z2 spread at all** (z0 34, z1 4 — the same two zones as the pinned baseline,
just more of them). If A1 sensitivity tracked coupling magnitude alone with
no dependence on the schedule's specific shape, a comparable-magnitude flat
change should have reproduced a comparable *cluster signature*, not just a
comparable *count*. It did not: the schedule's signature (`ENTRY_PEAK_C`
pinned, all growth on `LAG_S`, z2 newly implicated) looks like it carries
information about the schedule's transition/nonlinearity specifically, not
just about the peak magnitude it reaches.

**Reject count is monotone in `|factor - 1|`; accept count is not.**
`21 -> 29 -> 48 -> 54` as the factor moves `1.0 -> 0.85 -> 1.173 -> 1.35`
(sorted by distance from 1.0: 0, 0.15, 0.173, 0.35) is monotonically
increasing — more coupling perturbation in either direction produces more
divergence between the paired runs, exactly as expected for a magnitude
effect. But `ACCEPT` peaks near 1.173 and *drops* to 18 (below the pin) at
1.35, as some of the additional divergence crosses over from "clears bar-1
as an accept" to "trips the degradation veto as a reject" — a three-way
split moving through a threshold, not a magnitude-tracking count on its own.
This confirms the audit's earlier point (section 1: "the REJECT count
moving is as informative as the accept count") generalizes here too: ACCEPT
alone is not a monotone proxy for coupling-magnitude perturbation, but
INSUFFICIENT (monotonically falling: 615 -> 611 -> 574 -> 588 — actually
non-monotone here too, lowest at 1.173) and REJECT together are a more
reliable read of "how much divergence is this perturbation injecting."

### Verdict: PARTIAL — magnitude matters, but shape is not fungible with it

Neither pure reading from section 8/9 holds cleanly:

- **Not fully SUPPORTED**: if A1 tracked coupling magnitude alone independent
  of shape, the flat-scale run at 1.173 should have reproduced the schedule's
  cluster signature (pinned `ENTRY_PEAK_C`, all growth on `LAG_S`, z2 newly
  implicated) at a comparable count. It reproduced the *count* but not the
  *signature* — `ENTRY_PEAK_C` moved too, and z2 never appeared.
- **Not fully REFUTED**: a null, shape-free perturbation of the same
  magnitude the schedule reaches at high joint level degrades A1 by a
  comparable amount (38 vs 38, both well above the 24 pin) using no schedule
  and no window-averaging at all. The model class is not innocent merely
  because it isn't a flat scale — plain magnitude, applied uniformly across
  *all* joint operation (not just high-joint intervals), is already enough
  to fail this bar at the same scale value.

**Consequence for A1 as a gate on coupling-model changes:** A1 is not
purely a shape detector, and it is not purely a magnitude detector either —
it responds to both, and the two effects do not obviously add or factor
cleanly (the schedule's real-world exposure is a small high-joint-level
fraction of the trace — section 2 measured 9.4%/0.3% at level 1/2 — yet
produced a comparable regression to a factor applied unconditionally on
100% of joint operation). That asymmetry suggests the schedule's
*transition region* and its interaction with PWM windowing (Jensen,
section 2) carries disproportionate weight relative to its exposure
fraction — worth investigating directly in any future attempt, per section
9's "precondition for a third attempt," but not resolved by this
experiment. **A1 can still usefully gate coupling-model changes** (a model
that raises effective joint-operation coupling scale materially above 1.0,
schedule or not, should be expected to move this bar and must be
re-measured before being adopted) but it should not be read as isolating
*which* property of a candidate model (its magnitude vs. its shape) is
responsible for a given regression — this experiment shows both can produce
the same headline number via different cluster mechanisms, so the headline
alone under-determines the diagnosis and the per-subscore/per-zone cluster
breakdown (as run here and in section 1) is required every time, not
optional supporting detail.

The pin stays at 24/660. No model change is proposed by this section; it is
a measurement only.
