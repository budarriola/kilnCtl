# Does the sim plant need a second-order model? Tested and refuted in the tested form, 2026-09-10

Follow-up to `docs/audits/sim_credibility_gate_real_cause_2026-09-10.md` sec 6-8,
which left the dwell-entry overshoot PEAK bar (+/-2C) open and listed three
candidates. This pass tests them against real data rather than assuming the
listed leading candidate (second-order plant) is right.

## Method

Reimplemented `sim_kiln_step()`'s exact discrete-time update (additive
source-gain coupling, per-zone first-order element ODE, pure transport delay,
optional second real pole via the existing-but-unused `sensor_lag_tau_s`
field in `sim_plant_cfg_t`/`sim_plant.c`) in a standalone NumPy harness driven
by the **real recorded duty sequence** from the same two captures the C gate
uses:

- calibration: `logs/coupling/noise_floor_p7_run1.jsonl`
- hold-out: `logs/coupling/noise_floor_p7d_run1.jsonl`

using the live `g_k_dc`/`g_tau_s`/`g_dead_time_s`/`g_coupling_coeff` constants
from `firmware/KilnFW/App/test/sim_measured_zone_constants.h`. This is a
scratchpad reimplementation, not the linked production `.c` (numbers differ in
absolute value from the built `kilnctl_sim_credibility_gate.exe` because the
gate's dwell-segment bookkeeping is C-side and slightly different from this
script's), but it reproduces the qualitative baseline (z0 dominates the peak
failure; z1/z2 are close at baseline) and is adequate for a controlled,
apples-to-apples A/B on model structure, which is what this question needs.

No production or test file was modified. `sensor_lag_tau_s` already exists in
`sim_plant.h`/`sim_plant.c` and is deliberately zeroed by
`sim_plant_from_zone_cfg()` ("dead_time_s already lumps sensor lag") -- it is
the natural, already-wired second pole to test, not a new addition.

## Experiment 1: split the existing tau into two lags in series (candidate 1, naive form)

Element lag `tau1` + sensor lag `tau2`, holding `tau1+tau2 == model_tau_s`
(same total inertia as the current single-pole model), grid-searched over the
split fraction, calibration capture only:

| tau1 fraction of total | z0 seg0 peak (real 1.31C) | z0 seg1 peak (real 3.78C) | cal RMS |
|---|---|---|---|
| 1.00 (baseline, single pole) | 4.415 | 3.931 | 2.569 |
| 0.90 | 5.024 | 4.441 | 2.444 |
| 0.70 | 5.724 | 5.125 | 2.391 |
| 0.50 | 5.936 | 5.321 | 2.405 |
| 0.30 | 5.678 | 5.079 | 2.389 |

**Splitting the same total inertia into two poles makes the peak-overshoot
mismatch worse, not better, across the whole split range** -- the sim
overshoots MORE, moving further from the real 1.31-3.78C. Overall RMS error
across all three zones ticks down slightly (2.57 -> ~2.39) but that
improvement is not concentrated where the peak bar fails; it does not rescue
the peak metric at any split tested. This directly contradicts the naive
version of candidate 1.

## Experiment 2: is it a tau-magnitude problem instead of a model-order problem?

Independently scaling just `model_tau_s` (single pole, no second lag):

| tau scale | z0 seg0 peak | z0 seg1 peak | cal RMS |
|---|---|---|---|
| 1.0 (baseline) | 4.415 | 3.931 | 2.569 |
| 1.3 | 2.33 | 2.08 | 2.934 |
| 1.5 | **1.28** | 0.93 | 3.459 |
| 1.6 | 1.06 | 0.39 | 3.753 |

A **larger single-pole tau (no second mass) gets z0's peak almost exactly
right (1.28 vs real 1.31 at scale 1.5)** -- but at a real cost: overall RMS
degrades monotonically (2.57 -> 3.46 at the scale that fixes z0), and z1/z2's
peaks (which the baseline already gets roughly right, e.g. z1 seg0 sim 1.87
vs real 2.06) collapse to 0.0 -- the slower model now never reaches target at
all within the 10-minute window for those zones, trading one zone's peak
error for two others' peak error growing to their full real value (worst
case). Checked on hold-out: same direction and same trade (z0 hold seg0 goes
1.28 sim vs 1.28 real at scale 1.5, but z1/z2 hold peaks go to 0.0 vs real
2.05/1.84). **Net: not a win, just a different, differently-shaped failure.**

## Experiment 3: does adding a second pole help once the effective total tau is corrected?

Repeated experiment 1's split grid at `total = 1.5 * model_tau_s` (the value
that helped z0 in experiment 2), to check whether splitting that larger total
into two poles does *better* than the plain single-pole 1.5x scale (which
would be the actual evidence for "the plant is second order with a bigger
combined time constant than the current fit believes"):

| tau1 fraction of 1.5x total | z0 seg0 peak | z0 seg1 peak | cal RMS |
|---|---|---|---|
| 1.0 (= plain scale-1.5 single pole) | 1.28 | 0.93 | 3.459 |
| 0.5 | 2.77 | 2.30 | 3.681 |
| 0.1 | 1.77 | 1.50 | 3.407 |

No split at this total beats the plain single-pole case on both the peak
metric and RMS simultaneously; every split with `frac1 < 1.0` makes the peak
metric worse than the pure single-pole scale-up. **The two-pole-in-series
model does not outperform a corrected single pole anywhere in this grid.**

## Experiment 4: dead time

Scaling `model_dead_time_s` alone (0.1x to 2x, single pole unchanged) moved
the peak metric by less than 0.1C at every scale tested -- a pure transport
delay time-shifts the trajectory but barely changes its peak magnitude when
driven by the real (non-step) recorded duty. Ruled out as a lever for this
bar, independent of the relay-lag discussion below.

## Candidate 3 (relay lag) -- not simulated, ruled out analytically

`sim_relay_lag_step()`'s 0.5s placeholder is under 1% of the 60s PWM window
this plant is driven at. It cannot plausibly account for a multi-degree,
multi-hundred-second-scale overshoot mismatch; this matches the audit doc's
own assessment ("0.5s almost certainly does not [matter]"). Not tested
further -- there is no plausible mechanism by which it would, and the bench
measurement to pin down its true value is a hardware task orthogonal to this
question.

## Candidate 2 (open-loop replay vs closed-loop) -- not directly tested this pass

Building a closed-loop harness with the real `pid.c` linked (as the audit's
sec 6 discriminator calls for) was not done in this pass -- it is a
substantially larger harness change than the plant-order experiments above
and deserves its own reviewed pass. What the experiments above DO say about
it indirectly: the peak mismatch is concentrated on z0, the same zone with
the largest already-documented forward-gain deficit
(`docs/audits/sim_credibility_gate_real_cause_2026-09-10.md` sec 5, scalar
1.12 vs 1.19/1.31 for z1/z2 -- z0's scalar is smallest, but z0 is also the
zone with the two largest incoming coupling cross-gains, 27.32 and 21.72,
i.e. the zone most exposed to the coupling model's own residual). Nothing in
this data set separates "open-loop replay amplifies a small model error"
from "the model's gain is wrong at this operating point regardless of replay
mode" -- both would show up as a gain-shaped residual concentrated on the
worst-conditioned zone. This remains open and is the recommended next step
if the peak bar is still to be closed.

## Conclusion

**A second-order (two-real-pole) plant, in every form tested here (same
total tau, split to any fraction; and a larger total tau split any which
way), does not close the dwell-entry peak gap and in its most natural form
(same total inertia as today, just split into two masses) makes the
mismatch WORSE.** This refutes the audit doc's stated "most likely" ranking
for candidate 1 as tested. It was not possible to find a two-pole
configuration in this grid that simultaneously improves the peak bar and
does not degrade RMS fit or break z1/z2's peak match -- i.e., **no second-order
fit was adopted**, per the standing instruction not to fit to the gate: every
configuration that moves z0's peak toward real does so by degrading the
model everywhere else, which is evidence the mismatch is not a missing
thermal mass.

The one manipulation that helped z0's peak in isolation -- a larger single-pole
tau -- reproduces the exact model_tau_s-drives-everything failure mode this
whole gate exists to catch (TODO.md 6A.4 gain/dynamics-scheduling rationale):
tuning one number to fit one zone's one metric while breaking two other
zones. It is reported here as a diagnostic result, not adopted.

**Implication for the FOPDT-based controller, independent of the gate:**
since a second-order plant assumption does not even help the *simulation*
match reality here, there is no positive evidence in this investigation that
the real plant carries meaningfully separable second-order dynamics at this
operating point -- the earlier repo finding that dead time (34-53s) is large
relative to tau (~264-271s) remains true and remains a caution about a
first-order assumption in general, but this pass did not find a two-pole fit
that explains the kiln's actual near-target behavior better than the current
single pole. The `pid.c`/autotune FOPDT assumption is not shown to be wrong
by this data; the leading unresolved explanation for the peak bar is the
already-quantified coupling/forward-gain deficit (sec 5 of the prior audit),
not plant order. The closed-loop-replay discriminator (candidate 2) is the
recommended next step, not a second-order plant rebuild.

## Method note

No `check_*` or production assertion was added by this pass -- nothing here
to negative-test. All numbers above come from an unmodified read of
`firmware/KilnFW/App/test/sim_plant.c`/`sim_credibility_gate.c`/
`sim_measured_zone_constants.h` plus a scratchpad-only NumPy reimplementation;
`git status` on every file this doc discusses shows no working-tree change
from this pass.
