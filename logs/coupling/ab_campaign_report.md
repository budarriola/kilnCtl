# Coupling-matrix A/B campaign: verdict

Campaign: `logs/coupling/ab_campaign_state.json`, 6/6 firings completed
2026-08-31. Profile 7, three matched alternating pairs (old/new), coupling
presets `coupling_matrix_pre20260902` ("old") vs `coupling_matrix_20260831`
("new"). Compared with the existing, unmodified tooling:
`tools/PcTools/src/kilnctrl/pid_ab_compare.py compare <old> <new>` per pair,
against the checked-in `tools/PcTools/config_presets/noise_floor.json`
(schema-2, 48 entries, n=6). Raw tool output for each pair:
`logs/coupling/ab_compare_pair{1,2,3}.txt`.

## Verdict: INDISTINGUISHABLE / INCONCLUSIVE. Confidence: LOW.

The campaign did not deliver what it was designed to deliver. Of the three
intended replicate pairs, **two of three (pair 1 and pair 3) are REFUSED
outright** by the tool's own start-temperature confound gate — every zone in
both pairs exceeds the 1.0 C threshold (pair 1: 1.66/1.63/1.36 C; pair 3:
1.20/1.20/1.35 C). Only **pair 2** (0.47/0.64/0.70 C, all zones under
threshold) produced a usable comparison. That leaves **n=1** valid A/B
firing pair, not the n=3 the campaign was built to give the >=3-zone
consistency rule and the multiplicity accounting something to work with.

A single A/B pair cannot support a confirmed verdict under this project's
own rule (§ below) or under the noise-floor methodology (which was built
from six *repeat* firings under one configuration, not one A/B pair). The
one usable pair is suggestive — see below — but "suggestive from one
unreplicated comparison" is not evidence of a real effect; it is exactly
the kind of single data point this project's rules exist to keep from being
overclaimed. Reported as indistinguishable, not as a win for either preset.

## 1. Per-zone, per-metric numbers vs. the noise floor

Only pair 2 (old_2 vs new_2) is usable. Whole-run normalized IAE, °C
(A=old, B=new), against the measured floor:

| zone | A (old) | B (new) | raw delta | noise floor | verdict |
|---|---|---|---|---|---|
| z0 | 1.726 | 1.026 | −0.700 | 0.116 | DISTINGUISHABLE (B lower) — PROVISIONAL, n=1 |
| z1 | 1.218 | 0.583 | −0.635 | 0.077 | DISTINGUISHABLE (B lower) — PROVISIONAL, n=1 |
| z2 | 0.959 | 0.815 | −0.144 | 0.147 | INDISTINGUISHABLE (delta ≈ floor) |

z0 and z1's raw deltas are 4–8x the measured floor — real movement, not
noise, *within this one pair*. z2's delta (0.144 C) sits right at its floor
(0.147 C) and is called indistinguishable. Segment-level and dwell-metric
deltas for pair 2 are in `ab_compare_pair2.txt`; the tool's own
`consistent_patterns` block (same file) lists, at the (zone, metric,
segment) key level within this one pair:

- `iae_normalized_c` — B (new) better, 5 keys, zones [0,1,2]
- `dwell_steady_state_offset_c` — B better, 5 keys, zones [0,1,2]
- `dwell_entry_overshoot_peak_c` — B better, 4 keys, zones [0,1]
- `ramp_mean_error_c` — B better, 3 keys, zones [0,1]
- `ramp_worst_error_c` — B better, 3 keys, zones [0,1,2]
- `dwell_entry_time_to_peak_s` — **A (old) better**, 3 keys, zones [0,1]
- `settle_time_s` — B better, 3 keys, zones [0,1,2]

Pairs 1 and 3 produced no scored comparisons at all — every key is
`REFUSED` by the confound gate, so there are no floor-comparable numbers to
report for them (see `ab_compare_pair1.txt` / `ab_compare_pair3.txt`).

`ramp_lock_held` was checked directly across all six raw captures
(`ab_old_{1,2,3}.jsonl`, `ab_new_{1,2,3}.jsonl`) and is `true` in **0** of
them — consistent with the profile's ~70 C max target, and not used as
evidence of anything here, per instruction.

## 2. Does the >=3-zone consistency rule (CONSISTENT_PATTERN_MIN_KEYS = 3) fire?

Yes, mechanically, for six of the eight tracked metrics **within pair 2
alone** (`iae_normalized_c`, `dwell_steady_state_offset_c`,
`dwell_entry_overshoot_peak_c` at 2 zones only so it does NOT clear 3,
`ramp_mean_error_c` at 2 zones so it does NOT clear 3 either,
`ramp_worst_error_c`, `dwell_entry_time_to_peak_s`, `settle_time_s`) — see
the exact zone lists above; the ones spanning all three zones are
`iae_normalized_c`, `dwell_steady_state_offset_c`, `ramp_worst_error_c`, and
`settle_time_s`, all favoring the new matrix, plus `dwell_entry_time_to_peak_s`
favoring the OLD matrix at 2 zones (not 3).

But the rule was designed to guard against per-key noise **across
replicate firings of the same comparison**, using multiplicity accounting
built for a full n=3 (or more) pair set. Here it is firing on the
(zone × segment) breakdown of a **single** A/B pair, which is a different
and weaker thing: it shows the new matrix's advantage in pair 2 is
internally consistent across zones (not a fluke of one zone's segment), but
it says nothing about run-to-run repeatability, because there is no second
or third valid pair to repeat it against. The two pairs that would have
supplied that replication were both discarded by the confound gate before
they produced a single scored key.

For the metric with the best-characterized, most stable noise floor —
whole-run `iae_normalized_whole_c` (floor ratio 1.9x across zones, "reliable"
per the tool's own `metric_floor_reliability` report) — only 2 of 3 zones
(z0, z1) clear DISTINGUISHABLE in pair 2; z2 does not. That metric alone
does **not** meet the >=3-zone bar.

**Net: one internally-consistent but unreplicated pair is not what this
rule was built to certify. Treat the pattern as a lead for the next
campaign, not a finding.**

## 3. Starting-temperature comparability across all six arms

| pair | zone 0 delta | zone 1 delta | zone 2 delta | comparable (<=1.0 C)? |
|---|---|---|---|---|
| 1 (old_1/new_1) | 1.66 C | 1.63 C | 1.36 C | **NO** — all three zones exceed threshold |
| 2 (old_2/new_2) | 0.47 C | 0.64 C | 0.70 C | Yes — all three zones within threshold |
| 3 (old_3/new_3) | 1.20 C | 1.20 C | 1.35 C | **NO** — all three zones exceed threshold |

Two of three arms are not comparable at the start, by a clear margin (not a
borderline call — every zone in pairs 1 and 3 is 20–70% over the 1.0 C
gate). This is exactly the confound this project has hit before (the
4.8 C-delta fuzzy-layer pair, and the 27.60–28.88 C spread across the
noise-floor repeat set) and the reason `pid_ab_compare.py` refuses those
comparisons rather than reporting numbers that can't be attributed to the
matrix change. It is weighted into the verdict above as the primary reason
the campaign cannot support a confident call: the "six-firing" campaign
delivered, in practice, one usable A/B firing.

## 4. What this experiment cannot answer

- **Cone-range behaviour.** Profile 7's max target is ~70 C, far below any
  cone temperature. Nothing here bears on how either matrix performs at
  bisque/glaze-range temperatures.
- **Above ~62 C.** The coupled hold solve is infeasible above roughly 62 C
  (§3.2's own feasibility-sweep figures: 60–65 C depending on which
  diagonal is used) — the ramp segments that do reach into the high 60s in
  this profile sit right at or past that edge, so even the one usable pair
  should not be read as validating the matrix in its own feasible range,
  let alone past it.
- **Zone 2's dwell metrics**, specifically, given the single usable pair's
  own whole-run IAE calling z2 indistinguishable while z0/z1 are not — the
  asymmetry this project already measured between z2 and its neighbours
  (§3.2) is not resolved one way or the other by this campaign.
- **Repeatability of the pair-2 pattern.** With zero valid replicate pairs
  to compare it against, there is no way to know whether pair 2's
  consistent same-direction pattern would reproduce on a properly
  temperature-matched repeat, or whether it is itself an artifact of
  whatever pair 2's own particular conditions were.

## Recommendation

Re-run the two discarded pairs (or fresh replacements) with tighter start-
temperature control before drawing any conclusion about the new matrix.
Until then, the honest status is: no confirmed difference between
`coupling_matrix_pre20260902` and `coupling_matrix_20260831` has been
established by this campaign; one of three intended comparisons is usable,
and it leans toward the new matrix on ramp/dwell tracking metrics (not
`dwell_entry_time_to_peak_s`, where it leans the other way) but is
PROVISIONAL, single-instance, and not independently replicated.

## 5. Root cause (added 2026-09-03) and the fix

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
enclosure residual heat rather than a clean linear ambient ramp.

**Fix, landed in `tools/PcTools/src/kilnctrl/run_queue.py`:** a
`--pair-consecutive` queue mode. `QueueEntry` gained an optional `pair_key`;
entries sharing a key are matched consecutively (0&1, 2&3, ...). The FIRST
arm of a pair runs exactly as before. The SECOND arm, after passing the
ordinary rested wait, additionally blocks
(`wait_until_paired_start`/`is_paired_start_matched`) until its own reading
is within `DEFAULT_PAIR_START_TOL_C` (0.8C) of the FIRST arm's actual
recorded start reading — polling for up to `DEFAULT_PAIR_START_TIMEOUT_S`
(1 hour) and then **raising `RunQueueError` and refusing to start** if it
never closes the gap, rather than silently firing an arm the campaign
cannot use. 0.8C (not the tighter-looking 0.5C) is picked directly from
this campaign's own numbers: it clears pair 2's worst zone (0.70C) with
margin while still catching pairs 1 and 3 (minimum delta 1.20C) with
margin, and it leaves 0.2C of headroom under `pid_ab_compare.py`'s actual
1.0C confound gate — see the constant's docstring in `run_queue.py` for the
full arithmetic. Negative-tested directly against these six real start
temperatures (`tools/PcTools/tests/test_run_queue.py::IsPairedStartMatchedTest`):
the gate refuses pair 1 and pair 3's real deltas and accepts pair 2's; a
mutation drill (loosening the tolerance to 2.0C and re-running) reproduced
the pair-1/pair-3 failures for real before the tolerance was restored to
0.8C, proving the test is not vacuous.

**Cost of a corrected re-run.** Nothing here changes run or cooldown
duration — both remain ~33-35 min and ~25-32 min respectively, matching
this campaign's own captures. What changes is that a second arm may now
wait past its 25-32 minute natural cooldown for the paired-start gate: pair
2's own data shows that wait can be near-zero (its natural gap already met
tolerance), but pairs shaped like 1 or 3 would have needed materially
longer — on this data, a 25-32 minute cooldown left a 1.2-1.7C gap against
0.8C tolerance, so passive cooling would plausibly need a comparable
multiple of that gap closed, on the order of another 30-90 minutes per such
pair, with `--pair-start-timeout-s`'s 1-hour default able to absorb most of
that but not guaranteed to for every pair. Budgeting run+cooldown (~1
hour/arm x 6 = ~6 hours, matching the live campaign) plus up to ~1 extra
hour of paired-start waiting on each of the three second arms gives a
realistic **7-9 hour** corrected campaign, worse in wall time than the
original but producing 3 usable pairs instead of 1 — or, if a pair's second
arm cannot close the gap inside an hour, a loud, early refusal naming
exactly which pair failed, at the cost of that one pair's ~1 hour wait, not
another six hours of data that turns out unusable at analysis time.
