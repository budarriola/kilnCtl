# Kiln-scaled sensor dead time and tau in the scenario factorial (2026-09-14)

## Scope

This is the plan's §6 remainder (`docs/ADAPTIVE_FUZZY_EVALUATION.md`,
committed `5387ff52`). §6.1 (raise `SIM_PLANT_DELAY_MAX_STEPS` 64→128, add the
loud `delay_truncated` refusal) is already done and committed as `69118a66`
and is unchanged here. This dispatch wires the actual kiln-scaled values the
plan specifies and re-runs the 3-arm factorial. §3, §5, §7, §8, §9 are other
agents' scope and are not touched.

## The change

`firmware/KilnFW/App/test/sim_factorial_driver.c`'s `build_cell_plant()` held
`sensor_delay_s = BENCH_L0_S` (40.3 s) and `sensor_tau_s = FIXTURE_SENSOR_TAU_S`
(15 s) for every cell, including the 112 kiln-span (`A4 = 20.7`) cells — a
fixture-valued sensor time constant on a kiln-sized plant. Per plan §6, kiln-span
cells now use:

- `sensor_delay_s = KILN_L0_S = 76.9 s` (= 40.3 × 488/255.6)
- `sensor_tau_s = KILN_SENSOR_TAU_S = 28.6 s` (= 15 × 488/255.6)

Bench-span cells are untouched (`BENCH_L0_S = 40.3 s`, `FIXTURE_SENSOR_TAU_S =
15 s`). `c_s_j_per_c` is left at 500 everywhere per the plan (provably inert —
not touched). `model_dead_time_s` (fed to `pid_autotune_tune_from_fopdt()`) is
now derived from the cell's own `sensor_delay_s` (`sensor_delay_s * tm.m_l`)
rather than unconditionally from `BENCH_L0_S`, so the A6 tune-mismatch
multiplier is applied on top of the correct base dead time for that cell's
span.

## Headroom check (was the point of §6.1)

76.9 s at `dt = 1 s` is 77 ring steps, comfortably under the 128-step capacity
`69118a66` provided (63 was insufficient; 128 is). Confirmed empirically: grep
for `truncat` across the full corrected-run TSV returns zero matches — no
cell hit `delay_truncated` in either the `--of 1` or `--of 4` run.

## Mandatory regression proof: bench-span cells, bit-identical

Built from two clean, disjoint worktrees to avoid any of the three other
sessions' live uncommitted work in the shared tree (`kiln_package.c`/
`kiln_cfg_store.c`/etc., `profile_executor_pid_tick.c`,
`ramp_transient_ident.*` — none of those files are touched by this change, and
none were present in the worktrees used):

- `C:\wt\sensordt` at HEAD (`4b0c9674`), unmodified — the "before" state — and
  the same worktree with only `sim_factorial_driver.c` overwritten with this
  change, both built from short paths (`C:\wt\sensordt_build`,
  `C:\wt\sensordt_build_after`) to stay under the MSVC command-line limit.
- Full rebuild each time via `run_sim_factorial.ps1` (private `-OutDir`, no
  reuse of any stale `.obj`/`.exe`).

Before: 789/789 data rows, 0 refusals (matches the driver doc's own reported
run). After: 780/789 possible rows are present (see refusal finding below);
`--of 1` vs `--of 4` sharding is byte-identical (780/780 rows, confirmed by
`run_sim_factorial.ps1`'s own comparison, exit 0).

Row-for-row comparison, keyed on (`cell_id`, `arm`), restricted to the 151
bench-span cells (`a4_loss_scale_span_r_s = 1.0200`) × 3 arms = 453 rows:

```
bench diffs: 0
missing in after: 0
```

**All 453 bench-span rows are bit-identical before and after.** The kiln-scale
change touches only kiln-span cells, exactly as intended.

## New finding: 3 kiln-span cells now refuse (bounds check, not the delay ring)

Three cells that succeeded on all three arms under the old (physically
inconsistent) bench-valued sensor dead time now refuse on `A_PID` and
`A_FUZZY50` under the corrected kiln-scaled values:

| cell | a1 | a2 | a4 | a5 | a6 | refusal |
|---|---|---|---|---|---|---|
| ST1-053 | 0.5 | TIGHT | 20.7 | 150 | HOT | `sensor reading left [24.0, 1675.0]` |
| ST1-061 | 0.5 | TIGHT | 20.7 | 300 | HOT | `sensor reading left [24.0, 1675.0]` |
| ST1-181 | 3.0 | TIGHT | 20.7 | 150 | HOT | `sensor reading left [24.0, 1675.0]` |

This is **not** a delay-ring truncation (§6.1's mechanism) — `delay_truncated`
never fires anywhere in this run. It is the pre-existing `bounds_ok` check
(`ceiling_c = ambient_c + model_k_dc + 400`), which these three genuinely
exceed. All three share `A2 = TIGHT` (no actuator-authority headroom) and
`A6 = HOT` (the tune-mismatch triple `m_k=0.5, m_tau=2.0, m_l=0.5`: the model
believes the plant is half as responsive and twice as slow as it really is).
Combined with the now-correct, larger true dead time (76.9 s vs the old
40.3 s), a controller tuned against that badly-underestimated model overdrives
the real plant into a genuine, unbounded-looking divergence — this is a
physically real instability finding made visible only because the dead time
is now kiln-consistent, not an artifact of the harness change. Per the task's
instruction this is reported, not suppressed, and no bound was loosened to
make it disappear.

A related pre-existing harness behavior, unrelated to this change: when
`A_FUZZY50` refuses, `A_STATIC_MATCHED` is silently skipped with no
`CELL_REFUSED` line (`sim_factorial_driver.c:524`, `if (arm ==
CELL_ARM_STATIC_MATCHED && !have_fuzzy50) continue;`) because it needs
`A_FUZZY50`'s measured multipliers to run. That is why these 3 cells are
missing 9 rows total (3 arms × 3 cells) from only 6 printed `CELL_REFUSED`
lines. Not fixed here — it is a pre-existing driver quirk, not part of §6,
and touching the driver's refusal-printing logic risks nothing structural but
was left alone to keep this change minimal and reviewable.

Net effect: 260 of 263 cells produced usable rows post-change (109 of 112
kiln-span cells), vs 263/263 before.

## Tallies (registered rules): per-cell, 0.5 °C materiality floor, `A_FUZZY50` vs `A_PID`

Same comparison the prior review (`e2245b8d`) used as the decision-relevant
pair (`D_fuzzy = A_FUZZY50 − A_PID`, since that is literally the before/after
of removing the feature). Objectives: `LAG_SIGNED_C` (`lag_signed_s`, compared
by `|D|` since correct-rate tracking wants the *magnitude* of lag reduced,
signed or not), `STEADY_RMS_C`, `ENTRY_PEAK_C`, `ENTRY_UNDERSHOOT_C` (all
compared directly, lower magnitude = better). Threshold: `|D| >= 0.5 °C`.
This reproduces the prior review's own `ENTRY_PEAK_C` bench-span figure
exactly (1 improved / 71 degraded) as a methodology check; the full P8/P11
effects dispatch is a separate, later piece of work and is not reproduced
here — this is the tally table only, as the task scoped it.

**Bench-span (151 cells) — unaffected by this change, reported for reference:**

| Objective | improved | degraded | within floor |
|---|---|---|---|
| `LAG_SIGNED_C` | 19 | 126 | 2 (+4 n/a) |
| `STEADY_RMS_C` | 7 | 11 | 133 |
| `ENTRY_PEAK_C` | 1 | 71 | 79 |
| `ENTRY_UNDERSHOOT_C` | 10 | 26 | 115 |

Per-cell: improved-only 11, degraded-only 120, mixed 19, none 1. (Identical
before and after, as required by the bit-identical proof above.)

**Kiln-span — before (112 cells, bench-valued dead time, physically
inconsistent) vs after (109 cells, 3 refused, kiln-scaled dead time):**

| Objective | imp (before→after) | deg (before→after) | floor (before→after) |
|---|---|---|---|
| `LAG_SIGNED_C` | 79 → 69 | 32 → 34 | 1 → 6 |
| `STEADY_RMS_C` | 54 → 61 | 18 → 29 | 40 → 19 |
| `ENTRY_PEAK_C` | 52 → 43 | 37 → 46 | 23 → 20 |
| `ENTRY_UNDERSHOOT_C` | 83 → 45 | 21 → 15 | 8 → 49 |

Per-cell (kiln-span only): improved-only 36→29, degraded-only 1→5, mixed
75→72, none/floor 0→3.

**Combined, all cells (263 before, 260 after — 3 refused excluded):**

| Objective | imp (before→after) | deg (before→after) |
|---|---|---|
| `LAG_SIGNED_C` | 98 → 88 | 158 → 160 |
| `STEADY_RMS_C` | 61 → 68 | 29 → 40 |
| `ENTRY_PEAK_C` | 53 → 44 | 108 → 117 |
| `ENTRY_UNDERSHOOT_C` | 93 → 55 | 47 → 41 |

Per-cell (all): improved-only 47→40, degraded-only 121→125, mixed 94→91,
none/floor 1→4.

## How the kiln-span numbers moved

Correcting the sensor dead time/tau **did not flip the direction of the
9/14-owned decision** (`e2245b8d`'s REMOVE recommendation was already driven
primarily by the bench-span numbers, which are untouched here), but it moved
the kiln-span subset — the one regime that previously *favoured* fuzzy —
**toward parity, not away from it**:

- Kiln-span per-cell degraded-only count went **1 → 5** (5×) while
  improved-only fell **36 → 29**.
- `ENTRY_UNDERSHOOT_C` on kiln-span cells dropped from 83 improved / 21
  degraded to 45 improved / 15 degraded, with the missing mass moving almost
  entirely into "within floor" (8 → 49) — consistent with a larger sensor
  lag/dead-time smoothing out the entry transient enough that neither arm's
  undershoot clears materiality as often.
- Combined-all `degraded-only` rose 121 → 125 and `improved-only` fell
  47 → 40 — a small further shift toward REMOVE, in the predicted direction
  (larger, more realistic dead time increases fuzzy's measured harm, per the
  opus review's monotone-in-dead-time finding: 0 s → 9.63, 40.3 s → 84.2,
  64 s → 117.8 `steady_rms_c`).
- Three kiln-span cells (all `A2=TIGHT`, `A6=HOT`) now refuse outright on
  `A_PID`/`A_FUZZY50` rather than producing a comparable number at all — this
  removes them from every "favours fuzzy" or "favours PID" count entirely,
  which mechanically shrinks whichever side they used to fall on. All three
  had been counted on the "mixed" or favourable side of the old kiln-span
  tally, so their loss is part of why kiln-span's `improved-only` count fell.

**Bottom line:** correcting the physically inconsistent bench-valued sensor
dead time on kiln-span cells makes the measured picture slightly worse for
fuzzy overall, exactly as predicted, and it does not manufacture a new
favourable-to-fuzzy result anywhere. The bench-span numbers — the only regime
with zero fixture exposure and the ones the prior review already weighted as
"the cells we can most trust" — are provably unchanged.

## Extrapolation caveat

The kiln-span cells (200→1250 °C, `heater_power_w=2500W`) are pure
extrapolation from a ~4 W, 120 V bench fixture that cannot exceed roughly
40 °C above ambient. Every number in the kiln-span tables above is a
simulation-only projection, not a bench-verified result — labeled as such
per the task's constraint. No heating run, flash, or hardware call was made
for this dispatch.

## What was NOT done here (other agents' scope)

- No change to `profile_executor_pid_tick.c`, `ramp_transient_ident.*`,
  `kiln_package.c/.h`, `kiln_cfg_store.c/.h`, `kiln_cfg_http.c`,
  `settings_http.c`, `zones_config_store.c`, `ui_page_home_refresh.c`, or any
  of the three live test files under active edit by other sessions — none
  were touched, none needed to be.
- No P8/P11 main-effect/interaction re-derivation (§8/§9's scope) — the
  guard that fired for `STEADY_RMS_C`/`ENTRY_PEAK_C` in `e2245b8d` (non-additive
  at second order) would need to be re-checked against the corrected data by
  whoever owns that dispatch; this document does not attempt it.
- No firmware flash, no heating run, no `.kicad_*` file touched, no schedule
  table (`target_c`/`ramp_c_per_hr`/`dwell_min`/`segment_count`) touched.

## Checks

`tools/run_all_checks.ps1 -ExecutionPolicy Bypass` was run before and after
this change; see the commit for its final state (this factorial driver is
deliberately not wired into `build_host_tests.ps1` — `run_sim_factorial.ps1`'s
own header explains why — so it is not expected to move any check's outcome).
