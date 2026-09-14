# A1 false-accept root cause, 2026-09-14

## Task

Root-cause why `iter_tune`'s A1 bar (a NULL experiment: identical gains on
both sides of `firing_compare()`, only `noise_seed` and `start_offset_c`
differ per `sim_iter_tune.c`'s Part 2) accepts 24/660 comparisons (3.64%)
instead of the design target `A1_DESIGN_TARGET_PCT` (2.0%, `sim_iter_tune.c:622`),
and decide whether that gap is a harness defect or an honest property of the
current plant model.

## Re-confirmed baseline

Clean rebuild (`build_sim_iter_tune_bars_obj` deleted, `check_sim_iter_tune_bars.ps1`
re-run from scratch, `-ExecutionPolicy Bypass`):

```
660 null comparisons: ACCEPT 24 (3.64%)  REJECT 21  INSUFFICIENT 615  NO_PAIRS 0
```

Identical to the 2026-09-10/11 measurements this pin is built on
(`sim_iter_tune.c:750-751`, `A1_PINNED_MAX_ACCEPTS=24`, `A1_PINNED_TOTAL=660`).
No drift from stale objects, no drift from other in-flight work.

## Mechanism, named precisely

`sim_kiln_step()` (`firmware/KilnFW/App/test/sim_plant.c:211-242`) computes each
zone's coupling contribution as an **additive source-gain term driven by the
neighbour's instantaneous commanded duty**, `power_couple_w += coupling_w_per_c[i][j] * u_j`.
This replaced (`d63a5591`) an earlier temperature-difference exchange term that
is identically zero whenever two zones sit at a common temperature -- exactly
the operating point a dwell scores at. The new term is correct (it matches
firmware's real `zone_coupling_solve.c` model class) but it also means zone
i's instantaneous heat input now depends on zone j's **raw relay state**, not
a smoothed duty: `sim_iter_tune.c`'s own harness feeds `sim_kiln_step()` the
binary PWM-chopped relay output (`sim_duty[i] = relay_actual[i] ? 1.0f : 0.0f`,
the real ~60 s window, gap G2), so `power_couple_w` for a given zone steps
between two discrete levels every time a neighbour's relay edges, with no
filtering between that edge and the element temperature derivative.

`FIRING_SUBSCORE_ENTRY_PEAK_C` (`firing_score.c:206-215`) is a **raw,
unsmoothed maximum error sample** taken over the dwell-entry window -- by
design (`2edbb6eb` confirmed this "raw-peak definition sound" for
anti-gaming reasons: a smoothed/EMA'd peak was tried and reverted, see
`docs/audits/firing_score_entry_ema_review_2026-09-10.md`, because smoothing
blinded `firing_compare.c`'s one-sided degradation veto). A raw max is by
construction the statistic most sensitive to exactly one lucky/unlucky
sample.

Put together: in the A1 null experiment, side A and side B run the *same*
gains but with independently-drawn `noise_seed` and `start_offset_c`
(`sim_iter_tune.c:592-597`, deliberately -- these are A7's start-temperature
randomisation and the sensor-noise seed, not a bug). A different start
temperature shifts each zone's relay ON/OFF transition times relative to
that zone's own dwell-entry window by a few seconds to tens of seconds.
Because the coupling term now depends on the neighbour's *raw* relay edge,
this timing shift changes whether a neighbour's ON-to-OFF (or OFF-to-ON)
transition happens to land inside vs. outside a given zone's entry window --
and since `ENTRY_PEAK_C` records the single worst sample in that window,
landing or not-landing a neighbour's relay edge inside it changes the
recorded peak by an amount that has nothing to do with either side's PID
tuning. Zone 0 has the largest inbound coupling gains of the three zones
(root-caused with instrumentation at `8b96b591`), so it produces the
largest such swings, and `n_pairs == 3` (few dwell segments scored per
firing) means the accept/reject decision on any one comparison rests on a
median of only 3 draws of this quantity -- amplifying variance rather than
averaging it out. This is exactly the cluster reported in the task's
"established facts" (`ENTRY_PEAK_C`, dwell, `n_pairs==3`, mostly zone 0),
and it is now traced to a specific, nameable path: **coupling-by-raw-duty
edge timing, sampled by an un-smoothed single-sample peak statistic,
excited by A1's own deliberate per-side randomisation of start temperature
and noise seed.**

## Verdict: honest plant/scoring property, not a harness defect

No defect was found in `firing_score.c`, `firing_compare.c`, or
`sim_iter_tune.c`'s comparison/statistics code itself -- `ENTRY_PEAK_C`'s raw
definition, the median-of-n-pairs accept rule, and the null experiment's use
of independent noise seed and start temperature are all deliberate, each
individually justified and cross-referenced above and in `sim_iter_tune.c`'s
own comments. The 3.64% rate is what those deliberate design choices produce
when combined with `sim_plant.c`'s additive coupling model (`d63a5591`),
which is itself the physically-correct model class match to firmware, not a
harness artifact.

Two independent attempts to reduce the sensitivity by changing the coupling
model itself made it *worse* and were reverted:
- Level-scheduled coupling gain (`8cbd9d67`, reverted by `9f054181`): A1 went
  24/660 -> 38/660. The schedule was calibrated on a "joint excess" range
  the real PWM-chopped duty this harness feeds never visits (measured
  distribution 90.3%/9.4%/0.3% over 2.67M steps, zero samples in the
  calibration range), and the schedule inverts sign under PWM averaging
  (Jensen) at its own calibration point -- a documented do-not-retry class,
  not merely an unlucky fit.
- A flat-scale sweep (`c9ce6b7c`) found accepts non-monotone in coupling
  scale (0.85->20, 1.0->24, 1.173->38, 1.35->18): {18, 20, 24} cluster
  together at sampling noise (sd ~4.8 at n=660), and only 1.173 is
  marginally elevated -- there is no simple scale-factor fix either.

Given both a model-class fix and a scale fix have been tried and shown to
make things worse or to not cleanly separate from noise, and given no
defect exists in the comparator code to fix, this task's own instruction to
"decide, do not assume the first" resolves to: **3.64% is an honest property
of the current (correct) coupling model class combined with a deliberately
raw, anti-gaming peak statistic and a deliberately randomized null
experiment** -- not a bug to patch. The 2.0% design target was set before
the additive coupling model was adopted and has not been re-derived against
this model's real noise floor; that re-derivation, not further code changes
here, is the correct next step, and it needs new hardware-validated
coupling data this session does not have.

## What was changed

Nothing in `sim_plant.c`, `firing_score.c`, `firing_compare.c`, or
`sim_iter_tune.c`. Per the ratchet guard (`sim_iter_tune.c:744-749`) the pin
only ever tightens, and per this task's own instruction 3
("never as making the number smaller") no cosmetic change is justified
without a genuine mechanism fix -- none was found. `A1_PINNED_MAX_ACCEPTS`
stays at 24/660.

## Before/after distribution

Before (task's stated baseline) and after (this session's clean rebuild) are
identical:

| | ACCEPT | REJECT | INSUFFICIENT | NO_PAIRS | Total |
|---|---|---|---|---|---|
| Before | 24 | 21 | 615 | 0 | 660 |
| After (this audit, clean rebuild) | 24 | 21 | 615 | 0 | 660 |

No change, as expected from a diagnosis-only pass with no code edits and a
harness confirmed deterministic (`sim_iter_tune.c:708-730`).

## Checks

`check_sim_iter_tune_bars.ps1`: PASS (A1 against its pinned 24/660 ceiling,
A2/A5/A6 clear) -- see run above. `tools/run_all_checks.ps1` was run
separately; any `safety_ceiling_sync.c`/`config_divergence.c` failures
belong to concurrent ceiling work in progress in this tree and are not
addressed here.
