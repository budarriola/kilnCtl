# Scenario factorial cell driver (2026-09-14)

## Scope

**This is the CELL DRIVER, not the effects analysis.** It drives the 263
cells `sim_factorial_design.c` generates (`8d685bfe`) through three arms and
emits one TSV row per (cell, arm) plus a per-cell fuzzy rule-cell occupancy
histogram. It reports **numbers, not verdicts** -- the P8/P11 materiality and
interaction inference the design doc's §6 predicts is a separate, later
dispatch and is not computed here.

New files, all under `firmware/KilnFW/App/test/` (this dispatch's owned
prefix, `sim_factorial*`):

- `sim_factorial_driver.c` -- the driver (main, per-cell plant construction,
  the per-tick run loop, TSV emission).
- `run_sim_factorial.ps1` -- private-OutDir build+run script, `--suite
  factorial`'s separate target (see "CI posture" below).

Nothing else was touched except a temporary, hand-restored negative-test
injection in `firmware/KilnFW/App/drivers/control/pid_autotune.c` (see
"Negative test" below; the file is back to its pre-existing content, `git
diff` confirms no residual change).

## What this reuses, unchanged

- **The design generator** (`8d685bfe`, `sim_factorial_design.c/.h`):
  `sim_factorial_generate()` is called once in `main()`, its count checked
  against `SIM_FACTORIAL_TOTAL_COUNT_EXPECTED` (263) before anything else
  runs.
- **The three-node decomposition helper** (`a4477f59`,
  `sim_plant_decompose_three_node()`): every cell's plant is built by calling
  this real function with the cell's own `(Bi, phi)` -- never a home-rolled
  reimplementation.
- **Real production control code**: `pid.c`, `pid_fuzzy.c`, `pid_autotune.c`,
  `firing_score.c` -- linked, not mirrored, same posture as `sim_scenarios.c`
  (`1773d5f9`, `ba76ee92`) documents for itself.

`sim_factorial_driver.c`'s per-tick loop (`run_cell_firing()`) is a
deliberate **near-duplicate** of `sim_scenarios.c`'s `run_firing()`, not a
shared call: `sim_scenarios.c` is WI-4/5/6/7's own file, has its own `main()`,
and is outside this dispatch's owned prefix, so this driver keeps its own
copy rather than reaching into it. The two loops were diffed by hand at
write time and are wiring-identical except for: (a) no S10/S11
`retune_per_segment` branch (no factorial cell uses it), (b) the rule-cell
histogram accumulation (new), and (c) the sat_frac-based infeasibility
refusal (new, factorial-specific -- see below). Any future fix to one loop's
wiring must be checked against the other by hand; there is no shared
runner today.

## Factor-to-plant mapping (what wasn't already pinned by the design doc)

The design doc (§3, §9) pins levels and the decomposition contract but
leaves the concrete factor-to-`sim_plant_cfg_t` wiring to the implementer.
Every value below is a **TEST FIXTURE**, documented here so a reader never
has to guess:

| Factor | Mapping |
|---|---|
| A1 `load_mass_mult` | `sim_plant_cfg_t::load_mass_mult` directly (multiplies `c_l_j_per_c` at step time, per that field's own header contract -- applied AFTER decomposition, never folded into the decompose call). |
| A2 `headroom` | Multiplies the decomposition's `k` input: TIGHT=1.0x, AMPLE=1.5x `[ASSUMED]`, applied to whichever base K (bench or kiln, see A4) the cell uses. |
| A3 `sensor_bias_p` | `sim_plant_cfg_t::sensor_bias_p` directly. Sensor-node capacity/tau (`c_s_j_per_c`=500, `sensor_tau_s`=15s `[ASSUMED]`) held fixed across every cell -- out of the decomposition helper's scope per its own header, and not itself a design factor. |
| A4 `loss_scale_span_r_s` | Selects which ABSOLUTE temperature span the firing runs over, not a directly-set ratio: `1.02` -> bench span (ambient 24C, targets 40/60C, base K/tau = the bench z0 fit `k=42.731 tau=255.6`); `20.7` -> kiln span (ambient 25C, targets 200/1250C, base K/tau reused from `sim_high_temp_kiln_scale_cfg()`'s own anchor, `k=2500W tau=488s` at its 200C tune point) -- reusing that file's already-vetted physical anchor rather than inventing a second one, because the bench fit's full-duty asymptote (~68C) cannot reach kiln temperatures at any headroom multiplier. `sim_high_temp_scale_conductances()` is applied every tick against the LOAD node's own temperature for every cell (both spans), same mechanism S10/S11 use, so the realised `R_s` a cell actually sees emerges from the real formula rather than being set as an independent knob. |
| A5 `ramp_rate_c_per_hr` | Used directly as the commanded ramp rate. |
| A6 `tune` | Model-vs-plant mismatch multipliers `(m_k, m_tau, m_L)` on `(model_k_dc, model_tau_s, model_dead_time_s)` fed to `pid_autotune_tune_from_fopdt()`, from `SCENARIO_SIMULATION_PLAN.md` §2.4's table (linked via the real SIMC call, never reimplemented): MATCHED=(1,1,1), HOT=(0.5,2.0,0.5), COLD=(2.0,0.5,2.0), SLOW_INTEGRAL=(1.0,3.0,1.0). The TRUE plant is always `true_k`/`true_tau` (A2-headroom-scaled base, A1/A3/A4/A7/A8-shaped); only the model handed to the tuning rule carries the mismatch. |
| A7 `phi`, A8 `Bi` | Passed straight into `sim_plant_decompose_three_node()`'s `req.phi`/`req.bi`. |
| dead time (not a factor) | Held at the bench value (40.3s) across BOTH spans -- a documented simplification; the kiln-scale anchor does not separately specify one. |

## Arms

Three arms, per the task's minimum: `A_PID` (strength_pct=0), `A_FUZZY50`
(strength_pct=50), `A_STATIC_MATCHED` (fuzzy math bypassed, fixed
Kp/Ki/Kd multipliers). Same two-pass measurement `sim_scenarios.c` uses
(WI-5): `A_FUZZY50` runs first, its own ramp-phase mean applied/base gain
ratio is measured, then `A_STATIC_MATCHED` replays that cell's OWN measured
triple as a fixed override -- **never** the rule table's centre-cell maximum
(`x0.75/x1.25/x0.75`), which `1570a65a` established is fuzzy's value only at
error=0 AND rate=0 and overstated ramp-phase strength ~2x.

## Per-row objectives and occupancy

TSV columns (header row in the driver's own output): `cell_id, stage,
a1..a8, arm, lag_s, lag_signed_s, settle_s_2c, steady_rms_c, entry_peak_c,
entry_undershoot_c, sat_frac, applied_kp/ki/kd_mult_mean,
rulecell_center_frac, rulecell_hist9`. Objectives are reported **separately,
never aggregated** across cells or arms.

**Rule-cell occupancy** is a 3x3 dominant-membership histogram (NEG/ZERO/POS
error x FALLING/STEADY/RISING rate, same bucket order as `pid_fuzzy.c`'s own
`RULE_TABLE`), computed every control tick of every firing (all four
segments, every arm, not just `A_FUZZY50`) against the SAME
`pid_fuzzy_derive_bands()`-derived bands the real fuzzy call uses. It answers
"where does this cell's own (error, rate) trajectory sit relative to the
bands" -- an operating-point question, not "did fuzzy actually engage" (it is
computed for `A_PID` and `A_STATIC_MATCHED` too, as a reference). This does
**not** call into `pid_fuzzy.c`'s internals (its membership/rule logic is
`static`); it re-derives the dominant bucket from the same symmetric
triangular-membership shape that file's own header documents, purely for
histogram purposes -- it is never fed back into a gain.

## Infeasibility / refusal

Two distinct refusal paths, both **loud, no numeric row**:

1. **Build-time refusal**: `sim_plant_decompose_three_node()` itself refuses
   (its own documented degenerate cases) -- printed as `CELL_REFUSED <id>
   (all arms): <reason>`, the whole cell skipped.
2. **Runtime refusal**: `run_cell_firing()` returns false for NaN, an
   out-of-bounds sensor reading, or (factorial-specific, since the
   generator's one analytic mask only excludes the single derived A1=heavy /
   A2=TIGHT / A5=fast corner) **duty saturated (>=0.98) for >=97% of the
   whole firing** -- the runtime backstop for "u_req perpetually saturated,"
   printed as `CELL_REFUSED <id> <arm>: duty saturated ... infeasible cell`.
   A silently-clamped cell would otherwise look like a valid, if extreme,
   result; this refuses instead.

Both were exercised for real: an early build (bench K applied unscaled to
kiln-span cells, ambient 25C targeting 1250C against a ~68C full-duty
asymptote) produced exactly this refusal for every kiln-span cell whose
headroom/ramp combination pinned duty at ~97%+ (77 of 263 cells, 150 of 789
rows) -- the fix (reusing `sim_high_temp_kiln_scale_cfg()`'s own K/tau anchor
for kiln-span cells instead of the bench fit) is documented in the mapping
table above, not silently folded in.

## Results (current, `--of 1` canonical run)

```
789/789 rows emitted (263 cells x 3 arms), 0 cells with any refusal.
rulecell_center_frac range observed across all (cell, arm) rows: [0.0574, 1.0000]
```

The occupancy range is the headline number this driver exists to produce:
**not every factorial cell sits in the fuzzy table's centre cell** -- at
least one cell/arm combination spends as little as 5.7% of its ticks there,
in contrast to the field capture's ~95-96% (this file's sibling comments,
`pid_fuzzy.c`'s own header). Whether that is a `p`/`phi`/`Bi` interaction, a
tune-mismatch effect, or something else is exactly the effects analysis's
question -- not answered here.

## Determinism (`--of 1` vs `--of 4`)

`run_sim_factorial.ps1` builds once, runs `--shard 0 --of 1`, then runs
`--shard {0,1,2,3} --of 4` via `Start-Process` (process-level parallelism
only, same posture as `run_sim_scenarios.ps1`), sorts both result sets by
`(cell_id, arm)`, and diffs them line-for-line:

```
=== --of 1 canonical run ===
  --of 1: 789 data rows
=== --of 4 sharded run ===
  --of 4 (aggregated): 789 data rows
=== byte-identical comparison ===
PASS: --of 1 and --of 4 data rows are byte-identical (789 rows compared).
```

Actual diff, not assertion -- confirmed by running the script (transcript
above is the real tool output, not typed by hand).

## Runtime

`--of 1`, single process, 263 cells x 3 arms (789 firings, each up to ~70k
ticks for the longest kiln-span/SLOW_INTEGRAL cells): **2.8 s** wall clock on
this machine. Comfortably outside the ~60s CI budget's *reason for existing*
(this suite is not part of that budget at all -- see "CI posture" below) but
worth stating plainly since the task asked for it.

## CI posture: separate `--suite factorial` target, does not inflate CI

`sim_factorial_driver.c`/`run_sim_factorial.ps1` are **not** wired into
`build_host_tests.ps1` at all -- the existing 13-named-scenario suite
(`sim_scenarios.c`, run automatically at `--shard 0 --of 1`) stays exactly as
it is, at its existing ~60s CI budget. `run_sim_factorial.ps1` is a fully
standalone build+run script (its own `cl` invocation, its own `/I` list
mirroring `build_host_tests.ps1`'s common-flags rsp, its own private
`-OutDir` defaulting under `%TEMP%`) a developer runs by hand.

## Negative test

Broke `firmware/KilnFW/App/drivers/control/pid_autotune.c`'s
`pid_autotune_tune_from_fopdt()` to unconditionally return
`AUTOTUNE_REFUSAL_NONPOSITIVE_GAIN` (a short-circuit inserted at the top of
the function, clearly marked `NEGATIVE-TEST INJECTION`). Rebuilt (private
OutDir, full recompile -- `run_sim_factorial.ps1` always deletes its own
exe before rebuilding) and re-ran:

```
CELL_REFUSED ST2B-07 A_PID: pid_autotune_tune_from_fopdt() refused: NEGATIVE_TEST_INJECTED refusal
CELL_REFUSED ST2B-08 A_FUZZY50: pid_autotune_tune_from_fopdt() refused: NEGATIVE_TEST_INJECTED refusal
CELL_REFUSED ST2B-08 A_PID: pid_autotune_tune_from_fopdt() refused: NEGATIVE_TEST_INJECTED refusal
=== sim_factorial_driver: shard 0/1 done. rows_emitted=0 cells_with_a_refusal=263 rulecell_center_frac_range=[0.0000,0.0000] ===
```

Every cell refused, zero numeric rows emitted -- the refusal path is real,
not vacuous. Restored `pid_autotune.c` BY HAND (the injected block deleted,
not reverted via git); `git diff` against the pre-existing file shows no
residual change. Rebuilt from a fully clean private `-OutDir` (deleted and
recreated, not reused) and reconfirmed the original PASS: 789/789 rows,
0 refusals, `--of 1`/`--of 4` byte-identical (see "Results"/"Determinism"
above -- those transcripts are from this post-restore rebuild).

## `sim_iter_tune` non-negotiable

Nothing in this dispatch touches `sim_plant.h`, `sim_plant.c`,
`sim_scenario_table.*`, or any file `sim_iter_tune.c` links (it links
`sim_plant.c`, `pid.c`, `pid_autotune.c`, `firing_score.c` -- all read-only
from this dispatch's own driver, none edited). Ran
`check_sim_iter_tune_bars.ps1` from a clean build (the same
`build_host_tests.ps1` pass that verified the rest of the suite, below) to
confirm directly rather than assert by inspection:

```
660 null comparisons: ACCEPT 24 (3.64%)  REJECT 21  INSUFFICIENT 615  NO_PAIRS 0
A1 bar: PINNED KNOWN-RATE CEILING <= 24/660 (3.6364%) -> PASS
```

24/21/615, exactly the pinned bar. Confirmed, not assumed.

## Host test / `run_all_checks.ps1` verification

Ran `build_host_tests.ps1` (private `-OutDir`) end to end: **45/45
executables built.** One RUN FAILURE: `kiln_cfg_swap`
(`test_kiln_cfg_swap.c`, 6 assertion failures around swap-success reporting,
`out_diverged`, and pending-record finalization) -- this is
`kiln_cfg_swap.c`, explicitly one of the files another agent owns per this
dispatch's own concurrency notice ("other agents own `kiln_cfg_swap.{c,h}`/
`safety_cfg_http.c`/SaftyFW's config-install path"). Not investigated or
touched here; attributed to that concurrent work, not to anything in this
dispatch. Every other executable in the suite passed, including the
`safety_link` host build the task flagged as a live triage target -- it
built and its own host tests passed cleanly in this run (`262/262`,
`267/267`, `1661/1661` checks passed across the safety_link-adjacent test
files); if that build is red again by the time this lands, it moved between
sessions and this run's clean result should not be read as a claim that it
stayed fixed.

Full `tools/run_all_checks.ps1` was not additionally run: `build_host_tests.ps1`
already covers KilnFW's host-test surface end to end (including
`sim_iter_tune`'s pinned bars above), and the task's own instruction was to
attribute rather than fix the one known-red area, which this section does.

## Assumptions carried as TEST FIXTURES (flagged for the effects analysis)

- A2 headroom multiplier (1.5x AMPLE) -- a round, undocumented-by-source
  number, not derived from any measurement.
- Sensor-node capacity/tau (`c_s_j_per_c=500`, `sensor_tau_s=15s`) held fixed
  across every cell and both spans.
- Dead time held at the bench value (40.3s) for kiln-span cells too.
- The 97% sat_frac infeasibility threshold is a round number, not derived
  from the design doc's own `u_req > 0.98` mask formula (that formula
  operates on believed/required duty analytically; this driver's backstop
  operates on the REALISED simulated duty trace instead, which is why the
  threshold is not identical to the mask's 0.98 literal).

Every constant above (and every constant in `sim_factorial_driver.c` itself)
is a TEST FIXTURE: never shipped, never written into `zones_config`, a
preset, or a firmware default.
