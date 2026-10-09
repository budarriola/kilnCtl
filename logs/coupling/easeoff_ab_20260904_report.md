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
`firmware/KilnFW/App/drivers/persist/zones_config_accessors.c`) and stays there.

## Files

- Raw compare output: `logs/coupling/easeoff_ab_20260904_pair1.txt`,
  `logs/coupling/easeoff_ab_20260904_pair2.txt`,
  `logs/coupling/easeoff_ab_20260904_pair3.txt`
- Run queue log: `logs/coupling/easeoff_ab_20260904_run_queue.log`
- Raw captures (gitignored): `logs/coupling/easeoff_ab_20260904_{2p0,3p0}_run{1,2,3}.jsonl`

## Post-hoc reachability audit (2026-09-04, after the fuzzy-PID inert-campaign discovery, commit 8906686)

The fuzzy-PID campaign's presets both pinned `control_mode: 2`, so the fuzzy
layer (only reachable under `control_mode: 3`/`PID_FUZZY`,
`profile_executor.c` ~827-847) never ran, and two campaigns' worth of kiln
time compared plain PID against itself. That discovery required auditing
every other paired-run conclusion for the same class of bug: a varied preset
field that never reaches the code path it is supposed to affect. This
campaign was checked:

1. **Gate trace.** `ease_off_window_mult` is read in
   `firmware/KilnFW/App/drivers/control/profile_executor_feedforward.c`
   (`zone_taper_climb_rate()`, via `zones_config_get_ease_off_window_mult()`)
   and consumed only from `profile_executor_pid_tick.c` (~79-86). The call
   site's gate is `!s_exec.dwelling && ff_rate != 0.0f` (i.e. actively
   ramping, not dwelling and not stalled) plus, inside
   `zone_taper_climb_rate()` itself, `z->ff_dead_time_s > 0.0f` (the zone
   must have an identified plant model, i.e. `ff_enabled`). Unlike the fuzzy
   bug, **this path is not gated by `control_mode` at all** -- it runs
   identically for `control_mode: 2` (plain PID) and `3` (PID_FUZZY), so the
   fact both presets used plain PID does not disable it.
2. **Preset check.** Both `easeoff_ab_3p0_20260903` and
   `easeoff_ab_2p0_20260903` carry real, non-zero `model_k_dc`/`model_tau_s`/
   `model_dead_time_s` per zone (so `ff_enabled` is true for all three
   zones), and differ **only** in the top-level `ease_off_window_mult`
   (3.0 vs 2.0) -- no second field, matching the fuzzy bug's `control_mode`,
   is shared between the two arms in a way that would gate this one out.
3. **Capture check.** The captured `.jsonl` files carry `exec`/`status`
   snapshots, not the finer-grained `bd_ff_rate_pretaper`/
   `bd_ff_rate_posttaper` breakdown fields `dashboard_json.c` exposes --
   those were not selected for this capture, so the taper fraction itself
   cannot be read back byte-for-byte from these logs (a gap worth fixing,
   see the pre-flight check below). What the captures *do* show is that
   `s_exec.dwelling == False` with non-zero `target_c` movement for
   ~150-175s per ramp segment in every run of both arms (e.g. `2p0_run1`
   segment 0: 25.3C -> 40C over 172s; `3p0_run1` segment 0: 26.4C -> 40C
   over 156s) -- i.e. the gate conditions (`!dwelling`, `ff_rate != 0`) were
   genuinely satisfied on real hardware in both arms, not just in theory.
   With z0's identified dead time at 52.8s, `window_s = mult * dead_time_s`
   is 105.6s (2.0x) vs 158.4s (3.0x) -- both comfortably inside the ~150-175s
   ramp length, so the taper mechanically DOES engage differently between
   arms (a longer window starts tapering earlier and more aggressively) over
   a meaningful fraction of every ramp, not just at its very tail.
4. **Verdict: VALID, not inert.** `ease_off_window_mult` reached the control
   law in both arms of every one of the six firings. This is a different
   finding from the dwell-credit/PWM-chopping cases (features that are
   *structurally* unreachable under the conditions tested): here the term
   was live and differed by design; the campaign's "indistinguishable"
   result reflects a genuinely small and noise-dominated effect (the
   multiplier only changes *when within a ~2-3 dead-time window of segment
   end* the taper starts, not whether it exists), not a wiring defect. The
   original verdict, the "no board change" conclusion, and the "do not
   re-run without a reason" guidance all stand. No retraction.
