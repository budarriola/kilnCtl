# Credibility gate dwell-entry peak: three-node wiring experiment (2026-10-07)

Follow-up to `credibility_gate_dwell_offset_2026-09-14.md` (L175-200).
Host tests only; no target build.

## Hypothesis

The element/load/sensor split (commit 7729f3c8, `SIM_NODE_THREE`) exists in
`sim_plant_three_node_step` but was not used by the multi-zone coupled
`sim_kiln_step`. The gate runs `sim_kiln_step`, so it still sees the one-node
plant, whose sensor reads the element directly. Wiring the split into
`sim_kiln_step` should remove the ~2-3 C excess dwell-entry overshoot.

## What was wired

- `sim_plant.c`: factored the per-step three-node deltas into
  `three_node_deltas()`. `sim_plant_three_node_step` calls it with zero
  extra input (bit-identical to before). In `sim_kiln_step`, zones with
  `node_model == SIM_NODE_THREE` use it with the neighbour-coupling power
  (minus radiative loss) injected into the element; load and sensor nodes
  are integrated; the sensor pipeline reads `sensor_node_c`. Legacy zones
  keep the exact old path (sim_iter_tune pinned counts unchanged).
- `test_sim_plant_three_node.c`: `test_kiln_step_three_node_wiring()` checks
  (a) uncoupled three-node zone in `sim_kiln_step` equals
  `sim_plant_three_node_step` bit for bit, (b) reading comes from the sensor
  node, (c) neighbour coupling heats the element before the load, (d) a
  legacy zone beside a three-node zone follows the unchanged path.
- `sim_credibility_gate.c`: opt-in diagnostic args
  `--three-node <bi> <phi> <sensor_bias_p> [sensor_tau_s [delay_scale [dc_comp]]]`
  and env `KILN_GATE_DUMP`, `KILN_GATE_SENSOR_LAG_S`. Default behaviour is
  unchanged (diff of default output against baseline: 0 lines). These are
  diagnostics only; nothing is adopted as default.

## Before / after (pass/fail per bar; peak adds unevaluable)

| Setup | Ramp | Dwell offset | Peak | Noise |
|---|---|---|---|---|
| Baseline (legacy) | 3/3 | 2/4 | 10/2/6 | 2/4 |
| Three-node nominal (Bi 0.3, phi 0.5, p 0.8333) | 5/1 | 3/3 | 4/8/6 | 1/5 |
| Three-node Bi 1.5, p 0.8333 | 0/6 | 0/6 | 0/12 | 1/5 |
| Three-node Bi 0.05, p 0.8333 | 3/3 | 3/3 | 10/2/6 | 2/4 |
| Three-node p=0, Bi 0.3, delay 0 | 6/0 | 0/6 | 12/0/6 | 3/3 |
| Three-node p=0, Bi 1.5, delay 0 | 4/2 | 1/5 | 12/0/6 | 4/2 |
| Three-node p=0, Bi 1.5, delay 1 | 0/6 | 3/3 | 12/0/6 | 4/2 |
| Three-node p=0, Bi 0.3, delay 1 | 0/6 | 3/3 | 11/1/6 | 3/3 |
| Three-node sensor tau 20 s, Bi 0.3, p 0.8333 | 4/2 | 2/4 | 5/7 | 1/5 |
| Three-node sensor tau 40 s, Bi 0.3, p 0.8333 | 4/2 | 2/4 | 6/6 | 1/5 |
| Legacy + `sensor_lag_tau_s` 5 | 6/0 | 1/5 | 11/1 | 3/3 |
| Legacy + `sensor_lag_tau_s` 40 | 4/2 | 1/5 | 12/0 | 3/3 |

Compensating DC gain for the sensor weighting (`dc_comp=1`) improved nothing
in any variant.

## Root cause of the peak failure

Failing cells are zone 0 segment 0 on both captures: sim peak 3.4 and 4.1 C
against recorded 1.3 C. With the legacy plant the sensor reads the element
directly, so the sim reading is a sawtooth with about +-1.5 C of PWM ripple
(60 s window). The gate's peak statistic is a single-sample max, so ripple
alone creates the "excess overshoot". The real kiln's reading is smoother.

## Verdict: hypothesis not confirmed (honest negative)

- Wiring is correct and tested, but with the plan's nominal near-element
  sensor (p = 5/6) the peak bar gets worse (4 pass / 8 fail) because the
  sensor still sees most of the element ripple, and the heat redistribution
  shifts ramp and offset.
- The peak bar passes (12/0) only when the sensor effectively reads the
  smoothed load (p = 0) or a plain legacy sensor lag is added. Both are
  smoothing choices with free, unidentified parameters (Bi, p, sensor tau).
  Picking them to satisfy these captures is circular, and they trade off
  dwell offset and ramp bars (see table). Nothing is adopted as default.
- The real finding is that the peak excess is a sensor-smoothing / ripple
  artefact, not a missing thermal node. Closing it needs an identified
  sensor time constant from a dedicated capture, not a fit to the gate data.

## cplval75 scalars (1.12 / 1.19 / 1.31): not adopted

Not adopted (needs owner sign-off). Effect if adopted, per the gate header
and the 2026-09-14 audit: it closes the dwell-offset bar but not the peak
bar, and zone 0's residual changes sign within the gate's own data, so a
single scalar cannot fix it. The ripple root cause above is independent of
the scalars.

## Validation

- Fresh-OutDir standalone build of test_sim_plant_three_node.c (+sim_plant.c, heater_output.c): 63 checks, 0 failures. Negative test (sensor pipeline forced to read the element) fails 2 checks as intended; restored by hand, fresh rebuild in a new OutDir passes again.
- Full build_host_tests.ps1 could not run: the heavy build gate timed out after 3600 s (other sessions held both slots), so the full host-test suite was NOT run for this change.
- Default gate output vs baseline: 0 diff lines. check_sim_iter_tune_bars.ps1 exit 0.
- run_all_checks.ps1 -Fast was started detached (log C:\wt\simcred_logs\checks.log); result not available at commit time.

