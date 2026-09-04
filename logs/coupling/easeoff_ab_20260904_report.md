# ease_off_window_mult A/B campaign: verdict (2026-09-04)

Campaign: `logs/coupling/easeoff_ab_20260904_state.json`, 6/6 firings completed
2026-09-03/04. Profile 7 (with prepended 40C/45min stabilisation hold), three
matched alternating pairs, B=3.0 vs A=2.0 `ease_off_window_mult`. Presets
`easeoff_ab_3p0_20260903` / `easeoff_ab_2p0_20260903`. Campaign ended on the
baseline (A, 2.0) arm as required; board re-verified on 2.0 before this
analysis (`zones_http_client.get_zones()` on 192.168.1.156 reads
`ease_off_window_mult=2.0`; executor state=PROFILE_EXEC_DONE; all relays off).

Analysed with `tools/PcTools/src/kilnctrl/pid_ab_compare.py compare <A> <B>`
per pair (same invocation style as `logs/coupling/ab_campaign_report.md`),
against the checked-in `noise_floor.json`, scoring only segments >=
`STABILIZATION_SEGMENT_INDEX` (auto-detected from each capture's own meta
line -- the prepended stabilisation hold is segment 0 and is excluded from
scoring by construction). Raw tool output:
`logs/coupling/easeoff_ab_20260904_pair{1,2,3}.txt`.

## Start-temp confound

All three pairs paired-start well within tolerance (max delta 0.66C, versus
the 1.5C confound gate) -- no pair was refused, no confound correction
needed.

| pair | z0 delta | z1 delta | z2 delta |
|---|---|---|---|
| 1 | 0.25C | 0.66C | 0.05C |
| 2 | 0.63C | 0.39C | 0.06C |
| 3 | 0.07C | 0.22C | 0.08C |

## Whole-run iae_normalized_whole_c (A=2.0, B=3.0)

| pair | z0 A / B (delta) | z1 A / B (delta) | z2 A / B (delta) |
|---|---|---|---|
| 1 | 0.922 / 1.016 (0.094, **indistinguishable**, floor 0.116) | 0.807 / 0.820 (0.012, **indistinguishable**, floor 0.077) | 0.915 / 0.831 (0.084, **indistinguishable**, floor 0.147) |
| 2 | 1.121 / 0.808 (0.313, distinguishable, **B lower**) | 0.898 / 0.750 (0.148, distinguishable, **B lower**) | 0.844 / 0.798 (0.047, **indistinguishable**, floor 0.147) |
| 3 | 0.956 / 1.142 (0.186, distinguishable, **A lower**) | 0.793 / 0.874 (0.080, distinguishable, **A lower**) | 0.834 / 0.891 (0.057, **indistinguishable**, floor 0.147) |

z2 is indistinguishable in all three pairs. z0 and z1 are each
distinguishable in two of three pairs -- but the direction **flips between
pairs**: pair 2 says B (3.0) is better in z0/z1, pair 3 says A (2.0) is
better in the same two zones. That is the opposite of the same pair being
one-directional; a real effect of the window multiplier would push the same
way every time it's re-measured. This is exactly the pattern the noise-floor
review warned about: at n=3 the per-key false-positive rate is ~30.5%
(`MULTIPLICITY` block, all three pair files), so 1-2 "distinguishable" hits
per pair are close to the expected chance rate, not evidence.

Each pair's own `CONSISTENT PATTERN(S)` block does show one same-metric,
same-direction, 3-zone pattern *within that pair* (segment 1 only):
pair 1 -- `iae_normalized_c` A better (all 3 zones); pair 2 --
`iae_normalized_c` B better (all 3 zones); pair 3 -- `ramp_mean_error_c` A
better (all 3 zones). Applying the project decision rule across the
*campaign* (not one pair in isolation): these three "consistent" patterns
contradict each other pair-to-pair (A wins in pair 1 & 3, B wins in pair 2,
on different metrics each time), so there is no single same-metric,
same-direction result that holds across the repeats. That is not a real
effect by the stated rule -- it is what the ~30% per-key false-positive rate
at n=3 predicts by chance alone.

Even taking the single largest observed magnitude at face value (pair 2, z0
whole-run IAE, delta 0.313C), it is still below the owner's 0.5C actionable
bar and below the 3-zone-consistency requirement.

## Verdict

**INDISTINGUISHABLE.** `ease_off_window_mult=3.0` does not beat 2.0: no
metric shows a same-direction, >=3-zone-consistent difference across all
three repeat pairs, and the one candidate metric (`iae_normalized_whole_c`)
flips direction between pairs 2 and 3. All observed magnitudes are also
below the 0.5C actionable bar. Per project decision rule, this is reported,
not acted on.

**No board change and no default change.** The board is already on 2.0 (the
firmware default, `ZONE_EASE_OFF_WINDOW_MULT_DEFAULT` in
`firmware/KilnFW/App/drivers/zones_config_accessors.c`) and stays there.

## Files

- Raw compare output: `logs/coupling/easeoff_ab_20260904_pair1.txt`,
  `logs/coupling/easeoff_ab_20260904_pair2.txt`,
  `logs/coupling/easeoff_ab_20260904_pair3.txt`
- Run queue log: `logs/coupling/easeoff_ab_20260904_run_queue.log`
- Raw captures (gitignored): `logs/coupling/easeoff_ab_20260904_{2p0,3p0}_run{1,2,3}.jsonl`
