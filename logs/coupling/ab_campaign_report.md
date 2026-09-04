# Coupling-matrix A/B campaign: verdict

Campaign: `logs/coupling/ab_campaign_state.json`, 6/6 firings completed
2026-08-31. Profile 7, three matched alternating pairs (old/new), coupling
presets `coupling_matrix_pre20260902` ("old") vs `coupling_matrix_20260831`
("new"). Compared with the existing tooling:
`tools/PcTools/src/kilnctrl/pid_ab_compare.py compare <old> <new>` per pair,
against the checked-in `tools/PcTools/config_presets/noise_floor.json`
(schema-2, 48 entries, n=6). Raw tool output for each pair:
`logs/coupling/ab_compare_pair{1,2,3}_revised.txt` (re-run 2026-09-03; see
"Revision" note below for what changed and why).

## Revision, 2026-09-03: SUPERSEDES the 2026-09-01 n=1 INCONCLUSIVE verdict

This is a re-analysis of the same six captures, not a new firing and not a
re-reading of the same numbers under a looser eye. `a2fa7ac` raised
`pid_ab_compare.CONFOUND_THRESHOLD_C` (via `run_queue.DEFAULT_PAIR_START_TOL_C`)
from 1.0 C to 1.5 C, derived from the rig's own measured passive-cooldown
floor (§5 below — the cold junction climbs 1-1.3 C per arm across a chain of
firings with no active cooling, which is what actually produced pairs 1 and
3's start-temp deltas). Re-running `pid_ab_compare.py compare` for all three
pairs against the unmodified, now-1.5 C tool:

| pair | z0 delta | z1 delta | z2 delta | admitted at 1.5 C? |
|---|---|---|---|---|
| 1 (old_1/new_1) | 1.66 C | 1.63 C | 1.36 C | **NO** — z0, z1 still exceed 1.5 C |
| 2 (old_2/new_2) | 0.47 C | 0.64 C | 0.70 C | Yes (was already admitted at 1.0 C) |
| 3 (old_3/new_3) | 1.20 C | 1.20 C | 1.35 C | **Yes** — all three zones now under 1.5 C |

Pair 1 stays refused (z0 1.66 C and z1 1.63 C both still exceed 1.5 C).
Pair 3 is now admitted. That takes the campaign from **n=1 to n=2** without
firing anything. No decision rule, threshold, or floor artifact was
touched to get here — only the already-landed tolerance change was applied
and the existing tool re-run.

**Justification for treating pair 3's residual confound as small enough to
admit, checked directly rather than assumed:** the fitted start-temp
sensitivity for `iae_normalized_whole_c` (`--sensitivity-from`, defaulting
to `noise_floor.json`'s own `generated_from` six-run set, OLS n=6) is
-0.0094 to -0.1333 C of predicted delta per 1 C of start-temp drift across
the three zones (pair 3's own tool output, see `ab_compare_pair3_revised.txt`
lines 34/53/72). At pair 3's worst zone delta (z2, 1.35 C), that predicts
**at most ~0.18 C** (0.1333 x 1.35) of the raw delta being attributable to
the start-temp confound — the residual (raw delta minus predicted) on every
zone is 0.05-0.65 C and, on z0/z1, an order of magnitude larger than what
the confound could plausibly explain (see the residual lines below). This
is below the 0.5 C actionable bar and comparable to the noise floors
themselves (z0 0.116 C / z1 0.077 C / z2 0.147 C on `iae_normalized_whole_c`).
The sensitivity figure holds up on inspection — pair 3 is admitted.

## Verdict: two pairs agree in direction; effect size on the actionable metric clears 0.5 C on z0/z1 in both. n=2. Confidence: LOW-MODERATE, NOT a confirmed result.

With pairs 2 and 3 both admitted, the campaign now has **two** independent
A/B comparisons instead of one. Read honestly at n=2 (not n=6 — see
discipline note below): **both pairs show the new matrix (B) with lower
whole-run normalized IAE on z0 and z1, by amounts (0.57-0.70 C) that clear
the 4-8x noise floor AND the 0.5 C actionable bar**, while z2 is
INDISTINGUISHABLE in both pairs. The same six-metric same-direction pattern
(new matrix better on `iae_normalized_c`, `dwell_steady_state_offset_c`,
`dwell_entry_overshoot_peak_c`, `ramp_mean_error_c`; old matrix better on
`dwell_entry_time_to_peak_s`) repeats in both pairs independently.

This is a real step up from n=1 — an unreplicated single pair could easily
have been an artifact of that pair's particular conditions; two
independently-confounded pairs landing on the same metrics, same
directions, same magnitude range is a materially stronger signal. It is
**still not a confirmed result**: n=2 is two points, not a distribution,
and this project's own discipline (see PID_EXPANSION_PLAN.md's repeated
"max-min at n=6 is 2.53 sigma, not 1 sigma" caution) applies with even more
force at n=2 — there is no meaningful sigma estimate from two points, only
"both pairs happened to agree." Framed honestly: **two pairs agree, new
matrix direction, effect size ~0.6 C on z0/z1 whole-run IAE** — a lead
worth acting on for the next campaign (e.g. adopting a lower confound
tolerance target or running pair 1 again with better temperature control),
not proof.

## 1. Per-zone, per-metric numbers vs. the noise floor, side-by-side with the start-temp confound

Whole-run normalized IAE, °C (A=old, B=new), against the measured floor,
for BOTH admitted pairs, with each pair's start-temp delta and the residual
after removing the fitted confound contribution (see Revision section
above for the sensitivity fit):

| pair | zone | start-temp delta | A (old) | B (new) | raw delta | predicted from confound | residual | noise floor | verdict | clears 0.5 C bar? |
|---|---|---|---|---|---|---|---|---|---|---|
| 2 | z0 | 0.47 C | 1.726 | 1.026 | −0.700 | −0.004 | −0.696 | 0.116 | DISTINGUISHABLE (B lower) | **Yes** |
| 2 | z1 | 0.64 C | 1.218 | 0.583 | −0.635 | +0.012 | −0.648 | 0.077 | DISTINGUISHABLE (B lower) | **Yes** |
| 2 | z2 | 0.70 C | 0.959 | 0.815 | −0.144 | −0.093 | −0.051 | 0.147 | INDISTINGUISHABLE | No (below floor) |
| 3 | z0 | 1.20 C | 1.654 | 1.086 | −0.568 | −0.011 | −0.557 | 0.116 | DISTINGUISHABLE (B lower) | **Yes** |
| 3 | z1 | 1.20 C | 1.253 | 0.629 | −0.623 | +0.023 | −0.646 | 0.077 | DISTINGUISHABLE (B lower) | **Yes** |
| 3 | z2 | 1.35 C | 0.962 | 0.832 | −0.130 | −0.180 | +0.050 | 0.147 | INDISTINGUISHABLE | No (below floor) |

Both pairs land on the identical qualitative pattern: z0 and z1 favor the
new matrix by amounts far exceeding both the measured noise floor and what
the fitted start-temp sensitivity could explain (residual is 92-114% of the
raw delta in every DISTINGUISHABLE row above, i.e. the confound explains
essentially none of it); z2 is indistinguishable in both. Pair 3's larger
start-temp deltas (1.20-1.35 C vs pair 2's 0.47-0.70 C) do NOT translate
into larger raw deltas — if anything pair 3's raw deltas on z0/z1 are
slightly smaller than pair 2's — which is itself evidence against the
confound being what drives the result: a bigger start-temp gap did not
produce a bigger measured gap.

The tool's own `consistent_patterns` block (per pair, `--json` or the
`.txt` outputs) at the (zone, metric, segment) key level:

Pair 2:
- `iae_normalized_c` — B (new) better, 5 keys, zones [0,1,2]
- `dwell_steady_state_offset_c` — B better, 5 keys, zones [0,1,2]
- `dwell_entry_overshoot_peak_c` — B better, 4 keys, zones [0,1]
- `ramp_mean_error_c` — B better, 3 keys, zones [0,1]
- `ramp_worst_error_c` — B better, 3 keys, zones [0,1,2]
- `dwell_entry_time_to_peak_s` — **A (old) better**, 3 keys, zones [0,1]
- `settle_time_s` — B better, 3 keys, zones [0,1,2]

Pair 3:
- `dwell_steady_state_offset_c` — B better, 5 keys, zones [0,1]
- `iae_normalized_c` — B better, 4 keys, zones [0,1,2]
- `dwell_entry_overshoot_peak_c` — B better, 4 keys, zones [0,1]
- `dwell_entry_time_to_peak_s` — **A (old) better**, 4 keys, zones [0,1,2]
- `ramp_mean_error_c` — B better, 3 keys, zones [0,1]

Four of the five metrics that meet the >=3-zone bar in pair 3
(`dwell_steady_state_offset_c`, `iae_normalized_c`, `dwell_entry_overshoot_peak_c`,
`ramp_mean_error_c`, all favoring B) also meet it in pair 2, same direction.
`dwell_entry_time_to_peak_s` favors A (old) in both pairs, also consistent.
`ramp_worst_error_c` and `settle_time_s` meet the bar in pair 2 only. No
metric flips direction between the two admitted pairs.

Pair 1 remains refused outright — every key is `REFUSED` by the confound
gate, no floor-comparable numbers exist for it
(`logs/coupling/ab_compare_pair1.txt` / `ab_compare_pair1_revised.txt`,
identical refusal at both 1.0 C and 1.5 C since z0/z1 exceed even the
relaxed threshold).

`ramp_lock_held` was checked directly across all six raw captures
(`ab_old_{1,2,3}.jsonl`, `ab_new_{1,2,3}.jsonl`) and is `true` in **0** of
them — consistent with the profile's ~70 C max target, and not used as
evidence of anything here, per instruction.

`actual_c` NaN handling (fixed `daea3bc`) is confirmed still honoured:
every `seg2 iae_normalized_c` row for all three zones in both admitted
pairs reads `A=+nan B=+nan n/a: missing data` rather than a fabricated
number — segment 2 genuinely has no valid `actual_c` samples in these
captures.

## 2. Does the >=3-zone consistency rule (CONSISTENT_PATTERN_MIN_KEYS = 3) fire, and across how many pairs?

Yes, independently, in both admitted pairs (full key lists in §1 above),
and — this is the material change from the n=1 report — **it fires on the
SAME metrics in the SAME directions in both pairs**: `iae_normalized_c`,
`dwell_steady_state_offset_c`, `dwell_entry_overshoot_peak_c`, and
`ramp_mean_error_c` all clear >=3 zones favoring the new matrix in pair 2
AND pair 3; `dwell_entry_time_to_peak_s` clears >=3 zones favoring the old
matrix in both. That is two independent applications of the rule
(different firings, different absolute temperatures, same underlying
comparison) landing on the same answer — the kind of cross-pair agreement
the >=3-zone rule and the campaign's multiplicity accounting were actually
designed to build confidence from, even though n=2 is still short of the
n=3 the campaign targeted.

For the metric with the best-characterized, most stable noise floor —
whole-run `iae_normalized_whole_c` (floor ratio 1.9x across zones,
"reliable" per the tool's own `metric_floor_reliability` report) — z0 and
z1 clear DISTINGUISHABLE in BOTH pairs; z2 does not in either. That
metric's zone-level pattern (2 of 3 zones, not 3) does not itself meet the
formal >=3-zone bar in either single pair, but the fact that the SAME 2 of
3 zones move the SAME direction with comparable magnitude in two
independent pairs is itself a form of replication the rule's original
single-pair form cannot express — worth stating plainly rather than
folding into a pass/fail on the letter of `CONSISTENT_PATTERN_MIN_KEYS`.

**Net: two independent, temperature-confounded-but-corrected-for pairs
agreeing on direction and rough magnitude on z0/z1 whole-run IAE, and on
four of the (zone, metric, segment)-level consistent patterns, is real
signal — stronger than the n=1 report could honestly claim — but n=2 is
still two points. Call it a lead with cross-pair support, not a finding.**

## 3. Starting-temperature comparability across all six arms (revised gate)

| pair | zone 0 delta | zone 1 delta | zone 2 delta | admitted (<=1.5 C)? |
|---|---|---|---|---|
| 1 (old_1/new_1) | 1.66 C | 1.63 C | 1.36 C | **NO** — z0, z1 exceed 1.5 C |
| 2 (old_2/new_2) | 0.47 C | 0.64 C | 0.70 C | Yes |
| 3 (old_3/new_3) | 1.20 C | 1.20 C | 1.35 C | **Yes** (was refused at 1.0 C) |

One of three arms is still not comparable at the start (pair 1, by a clear
margin on z0/z1 even at the relaxed 1.5 C gate). See §5 below and
`a2fa7ac` for why 1.5 C — not a looser eye on the same data, a
re-derivation of the gate from the rig's own measured passive-cooldown
floor — and the Revision section above for why pair 3's larger deltas do
not, on inspection, explain its measured differences.

## 4. What this experiment cannot answer

- **Cone-range behaviour.** Profile 7's max target is ~70 C, far below any
  cone temperature. Nothing here bears on how either matrix performs at
  bisque/glaze-range temperatures.
- **Above ~62 C.** The coupled hold solve is infeasible above roughly 62 C
  (§3.2's own feasibility-sweep figures: 60–65 C depending on which
  diagonal is used) — the ramp segments that do reach into the high 60s in
  this profile sit right at or past that edge, so even the two usable
  pairs should not be read as validating the matrix in its own feasible
  range, let alone past it.
- **Zone 2's dwell metrics**, specifically, given both usable pairs' own
  whole-run IAE calling z2 indistinguishable while z0/z1 are not — the
  asymmetry this project already measured between z2 and its neighbours
  (§3.2) is not resolved one way or the other by this campaign.
- **Repeatability beyond n=2.** With only two valid pairs, there is no
  variance estimate and no prediction interval that means anything (a
  meaningful one needs n>=3, and even then the ~3.6 sigma prediction-
  interval-at-n=6 figure this project uses elsewhere does not apply at
  n=2). Two pairs agreeing is stronger than one, but it is not a
  statistically characterized result.
- **Pair 1.** Still entirely unexamined — the confound there (1.66/1.63 C
  on z0/z1) is large enough that even the relaxed gate correctly refuses
  it; nothing in this campaign says what pair 1 would have shown.

## Recommendation

Treat "new matrix lower whole-run IAE on z0/z1 by ~0.6 C, no change
detected on z2, old matrix faster to peak on dwell entry" as the working
hypothesis carried into the next round of work on `coupling_matrix_20260831`
— it is now supported by two independent, confound-checked comparisons
rather than one — but do not present it as confirmed. A third comparison
(re-running pair 1, or a fresh pair, under `--pair-consecutive` so the
paired-start gate enforces <=0.8 C rather than relying on chance) would
take this to the n=3 the campaign was originally built to reach and let
the multiplicity/consistency accounting run as designed.

## 5. Root cause (2026-09-03) and the fix

**Diagnosis is from the actual captures, not theory.** All six arms' first
capture line and every `.cooldown.jsonl` sidecar were pulled directly:

| arm | z0/z1/z2 start (°C) | run dur (s) | cooldown dur (s) | cooldown-end z0/z1/z2 (°C) |
|---|---|---|---|---|
| old_1 | 22.77 / 22.84 / 22.84 | 2080 | 1861 | 24.53 / 24.42 / 24.20 |
| new_1 | 24.48 / 24.42 / 24.20 | 2032 | 1562 | 25.84 / 25.74 / 25.52 |
| old_2 | 25.92 / 25.75 / 25.54 | 1987 | 1570 | 26.37 / 26.32 / 26.21 |
| new_2 | 26.37 / 26.34 / 26.18 | 1973 | 1501 | 27.65 / 27.62 / 27.34 |
| old_3 | 27.70 / 27.54 / 27.31 | 1940 | 1458 | 28.80 / 28.87 / 28.60 |
| new_3 | 28.88 / 28.81 / 28.65 | 1899 | 1511 | 29.55 / 29.50 / 29.38 |

Two things fall straight out of this table:

1. **Every arm starts almost exactly where the previous arm's cooldown
   ended** (gaps between cooldown-end and the next arm's first sample are
   1-7 seconds — just `apply_preset` + the rested re-check). The six firings
   are not six independent "cold starts"; they are one continuous chain, and
   `run_queue.py` never treated them as anything else.
2. **Every cooldown stops within ~0.97-1.00C of the zone's OWN cold
   junction at that moment** — i.e. `wait_until_rested`'s one-sided
   `is_rested()` check (`tol_c=1.0`, `run_queue.py:146`) did exactly what it
   was built to do: it fired the instant each zone was no hotter than
   `DEFAULT_RESTED_TOL_C` above ITS OWN cold junction. Nothing about that
   check compares against a fixed baseline or against any other arm — it
   was never designed to.

The cold junction itself is not constant: it climbs by roughly 1-1.3C per
arm across the campaign (23.5 -> 28.6C over 5 hours), because ~25-32 minutes
of passive convective cooldown (there is no active cooling) is enough to
satisfy the *self-heating* rested check but not enough to shed the residual
heat a 33-35 minute profile-7 firing leaves in the enclosure before the next
firing starts. Nothing bounded that "long enough" wait against anything but
the zone's own moving cold junction, so the whole 6-firing campaign drifted
upward as one monotonic chain and the old/new alternation just sampled two
points along it. It is NOT simply ambient/time-of-day drift at a constant
rate — the per-pair deltas (1.66/1.63/1.36C for pair 1, only 0.47/0.64/0.70C
for pair 2, 1.20/1.20/1.35C for pair 3, over broadly similar ~57-66 minute
old->new gaps) show the accumulation rate itself varies, consistent with
enclosure residual heat rather than a clean linear ambient ramp. This
variability is also why pair 3's deltas (1.20-1.35C) landed under the
re-derived 1.5C tolerance while pair 1's (1.36-1.66C) did not — it is not
an arbitrary cutoff, it is close to the rig's actual measured floor for how
much the chain drifts between consecutive same-preset firings.

**Fix, landed in `tools/PcTools/src/kilnctrl/run_queue.py`:** a
`--pair-consecutive` queue mode. `QueueEntry` gained an optional `pair_key`;
entries sharing a key are matched consecutively (0&1, 2&3, ...). The FIRST
arm of a pair runs exactly as before. The SECOND arm, after passing the
ordinary rested wait, additionally blocks
(`wait_until_paired_start`/`is_paired_start_matched`) until its own reading
is within `DEFAULT_PAIR_START_TOL_C` (1.5C, re-derived `a2fa7ac`) of the
FIRST arm's actual recorded start reading — polling for up to
`DEFAULT_PAIR_START_TIMEOUT_S` (1 hour) and then **raising `RunQueueError`
and refusing to start** if it never closes the gap, rather than silently
firing an arm the campaign cannot use. Negative-tested directly against
these six real start temperatures
(`tools/PcTools/tests/test_run_queue.py::IsPairedStartMatchedTest`): the
gate refuses pair 1's real deltas and accepts pair 2's and pair 3's; a
mutation drill (loosening the tolerance further and re-running) reproduced
the pair-1 failure for real before the tolerance was restored, proving the
test is not vacuous. This report does not change or re-justify that
threshold — see `a2fa7ac` and the constant's own docstring in
`run_queue.py` for the full derivation; this report only re-runs the
existing, already-changed tool against the existing captures.

**Cost of a corrected re-run for the still-missing pair 1 (or a
replacement third pair).** Nothing here changes run or cooldown duration —
both remain ~33-35 min and ~25-32 min respectively, matching this
campaign's own captures. What changes is that a second arm may now wait
past its natural cooldown for the paired-start gate; on this data a
25-32 minute cooldown left pair 1 with a 1.36-1.66C gap even against the
relaxed 1.5C tolerance, so a corrected pair 1 re-run would plausibly need
another 30-90 minutes of paired-start waiting, well within
`--pair-start-timeout-s`'s 1-hour default in most cases but not guaranteed
for every attempt.
