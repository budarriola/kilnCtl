# adaptive_tune's dwell-harvest gate could not harvest from a rough dwell entry (2026-09-14)

## The claim under investigation

WI-8's chained-firing harness (`f842acdb`, `firmware/KilnFW/App/test/sim_scenarios_adaptive.c`)
found that only S0_NULL_SLOW and S6_MASS_LIGHT (the two gentlest ramps of the 11-scenario suite)
ever reach the minimum 4 dwell observations across 9 chained firings. Every mistuned scenario,
including the plan's named acceptance case S7_TUNE_HOT, harvested zero. Reported cause: the
duty-stability check in `adaptive_tune_zone_tick()` spans the whole dwell-entry transient and is
evaluated exactly once per dwell.

## Independent verification

Read `adaptive_tune_zone_tick()` in `firmware/KilnFW/App/drivers/control/adaptive_tune.c` (pre-fix
shape, now changed by this pass -- see below) directly, then re-derived the mechanism from the
struct fields in `adaptive_tune_internal.h` rather than trusting the report's prose:

- `settle_duty_min`/`settle_duty_max` were seeded once, at the first valid tick of a fresh dwell
  (`z->settle_start_c = actual_c; z->settle_duty_min = duty; z->settle_duty_max = duty;`), and from
  then on could only ever WIDEN for the rest of the dwell (`if (duty < min) min = duty; if (duty >
  max) max = duty;`, unconditional every tick, never reset until the next dwell). Any large duty
  swing during the ramp-to-dwell entry transient -- exactly what a mistuned zone produces fighting
  its way onto a new setpoint -- was therefore permanently baked into these two fields, regardless
  of how flat duty became later in the same dwell.
- The temperature settle gate (`ADAPTIVE_TUNE_SETTLE_MIN_S` = 180s, `ADAPTIVE_TUNE_SETTLE_SLOPE_
  FLOOR_C_PER_S` = 0.003) is an AVERAGE-since-entry slope (`fabsf(actual_c - settle_start_c) /
  settle_elapsed_s`), which does eventually pass once a dwell's temperature has genuinely leveled
  off, even after a rough entry -- the elapsed-time denominator only grows, so a bounded transient's
  contribution to the average shrinks over time.
- Once that slope gate passes, `z->recorded_this_dwell = true;` was set UNCONDITIONALLY, before the
  duty-stability check even ran ("One observation per dwell, regardless of outcome below"). This
  made the duty check a single coin flip decided almost entirely by how rough the dwell's entry
  was: the temperature gate could recover from a rough entry (given enough elapsed time), but the
  duty-range gate, once evaluated and failed, could never be evaluated again for the rest of that
  dwell -- even if duty became genuinely steady moments later.

Confirmed directly with the chained harness: reading `firmware/KilnFW/App/test/sim_scenarios_
adaptive.c`'s own `run_one_chained_firing()`/`run_chain()`, the file's own 2026-09-14 comment at the
`S7_DIRECTION_CHECK` call site independently names the same mechanism ("that window's min/max duty
still spans the ramp-to-dwell entry transient by the time the temperature slope first reads flat").
Instrumented nothing further -- the harness's existing per-firing `harvest_reason` output (peeked
from `adaptive_tune_zones[0].last_refusal_reason` before `adaptive_tune_run_end()` overwrites it)
already showed `"duty still oscillating..."` as the terminal refusal for every mistuned scenario's
every firing, one single evaluation each, matching the diagnosis exactly.

**Diagnosis confirmed, not refuted.** Both halves of the reported mechanism -- the whole-dwell,
monotonically-widening duty accumulator, and the one-shot `recorded_this_dwell` latch that treated
a duty-stability failure the same as a genuinely terminal verdict -- were verified by direct code
reading, independent of the report's own framing.

## The fix

`firmware/KilnFW/App/drivers/control/adaptive_tune.c`'s `adaptive_tune_zone_tick()`, and new state
in `adaptive_tune_internal.h`'s `adaptive_tune_zone_t` (`settle_window_start_s`, `settle_window_
duty_min`, `settle_window_duty_max`):

1. **A trailing (tumbling) duty-stability window**, `ADAPTIVE_TUNE_DUTY_STABILITY_WINDOW_S` = 300s,
   replaces the whole-dwell accumulator for the STABILITY VERDICT ONLY. Every tick folds the current
   duty into the window's running min/max; only after that tick's verdict has been read from the
   window does the code check whether the window's age (`settle_elapsed_s - settle_window_start_s`)
   has reached 300s, and if so, seed a fresh window with the CURRENT duty for future ticks. This
   ordering matters: tumbling BEFORE reading the verdict would let a tick landing exactly on a
   300s boundary see a freshly-reset, single-sample window (range == 0) and pass trivially no
   matter how unstable duty genuinely is. Evaluating first, tumbling after, means every verdict is
   read from a window containing at least the fold that triggered the tumble decision, never a
   window with zero history. This is proven directly by `test_persistent_duty_oscillation_never_
   harvests()` (see Tests below) -- a persistent limit cycle that keeps oscillating every tick, for
   many multiples of the window, correctly never harvests despite the window tumbling repeatedly.
2. **`recorded_this_dwell` is no longer latched on a `duty_unstable` verdict.** It is now latched
   only on the genuinely terminal outcomes: too-low duty, an implausible (non-positive) rise, or an
   actual successful commit to the ring. A `duty_unstable` verdict now returns WITHOUT locking
   anything, exactly like the temperature slope's own "still drifting, keep waiting" retry a few
   lines above it in the same function -- the two gates now share the same retry posture for the
   same reason: neither an unsettled temperature nor an unsettled duty says anything permanent
   about a dwell that has not yet had the chance to actually settle.
3. The temperature settle gate itself (`ADAPTIVE_TUNE_SETTLE_MIN_S`/`_SLOPE_FLOOR_C_PER_S`) is
   **untouched** -- real-hardware constants, not touched by this pass, and CLAUDE.md's PSRAM/NVS
   and sim-credibility notes both caution against touching real-hardware-derived constants without
   new hardware evidence.

### Why 300s, not something shorter

`ADAPTIVE_TUNE_DUTY_STABILITY_WINDOW_S` is deliberately picked LONGER than `ADAPTIVE_TUNE_SETTLE_
MIN_S` (180s) -- a steady-state duty verdict should not be trusted from a slice shorter than the
temperature gate's own settle requirement -- and short enough that a genuinely long, mistuned dwell
(many multiples of its own tau; `sim_scenarios_adaptive.c`'s own dwell length is `16 * model_tau_s`,
~70 minutes at tau~264s) ages a stale entry-transient extreme out of the window well before the
dwell ends: 300s tumbles roughly 14 times over such a dwell. A shorter window (60s was tried first)
tumbles too fast relative to the existing unit-test fixtures' coarse 30s tick spacing in `test_
adaptive_tune_dwell.c` (`DT_S` = 30, chosen for that file's own test-speed reasons, unrelated to
production's real 1 Hz `PROFILE_EXECUTOR_TICK_MS`), producing a tumble every 1-2 ticks there and a
spurious pass from a coincidentally-narrow tail of an otherwise-still-oscillating sequence -- this
was caught by the pre-existing `test_oscillating_duty_flat_temperature_is_refused()` going red
during development of this fix, confirming the smaller window was wrong before it ever reached
production code. 300s does not disturb that test (its whole fixture is only 210s, so the window
never tumbles within it, reproducing the original whole-dwell-accumulator behavior for that short
fixture exactly) while still tumbling many times over a real multi-thousand-second dwell.

## Observation quality, and the 0.003*tau residual invariant (`cef1df2a`)

This fix does not touch the TEMPERATURE settle gate's own constants or comparison at all -- a rough
entry still has to fully decay out of the temperature slope's own elapsed-since-entry average
before that gate passes, exactly as before. `cef1df2a` established that the settle gates never
require `error == 0`: a still-converging approach passes with a residual around 0.003*tau (~0.8 C
at tau~264s), which is gain-dependent, and this pass's own change cannot make that residual larger,
because it never runs before the (unchanged) temperature gate has already passed.

What DOES change is the DUTY half of the steady-state definition, and if anything this is a
correctness fix rather than a loosening: "duty has been flat for the last `WINDOW_S` seconds" is a
more honest reading of "duty is presently steady" than "duty has never once moved since a dwell
that may have started 20+ minutes ago" -- the latter is not what steady state means, and it is
exactly why every mistuned scenario could never harvest at all (rejecting real steady-state dwells
forever is not evidence of a stricter quality bar; it is evidence the check could never fire once
poisoned). The trailing window still enforces the identical `ADAPTIVE_TUNE_DUTY_STABILITY_ABS`
(0.05) / `_FRAC` (25%) thresholds this file's own comment derives from the coupid6 hardware
capture -- unchanged by this pass -- just measured over a window that can actually reflect the
dwell's CURRENT behavior instead of one permanently anchored to its first few ticks.

## Per-scenario harvest counts, before and after (9 chained firings, `max_ring_count_reached`)

Produced by `firmware/KilnFW/App/test/build/kilnctl_sim_scenarios_adaptive.exe` via
`tools\...\build_host_tests.ps1`'s `sim_scenarios_adaptive` step (12 is the ring's capacity,
`ADAPTIVE_TUNE_RING_CAPACITY`, so a scenario reading 12 saturated the ring across the chain):

| Scenario | Arm | Before | After |
|---|---|---:|---:|
| S0_NULL_SLOW | PID_AT | 12 | 12 |
| S0_NULL_SLOW | FUZZY_AT | 9 | 12 |
| S1_BASELINE | PID_AT | 0 | 12 |
| S1_BASELINE | FUZZY_AT | 0 | 12 |
| S3_SENSOR_CENTRE | PID_AT | 0 | 12 |
| S3_SENSOR_CENTRE | FUZZY_AT | 0 | 12 |
| S2_SENSOR_NEAR_ELEMENT | PID_AT | 0 | 12 (refine applied) |
| S2_SENSOR_NEAR_ELEMENT | FUZZY_AT | 0 | 12 (refine applied) |
| S4_SENSOR_NEAR_FAST_RAMP | PID_AT | 0 | 11 (refine applied) |
| S4_SENSOR_NEAR_FAST_RAMP | FUZZY_AT | 0 | 11 (refine applied) |
| S5_MASS_HEAVY | PID_AT | 0 | 12 |
| S5_MASS_HEAVY | FUZZY_AT | 0 | 12 |
| S6_MASS_LIGHT | PID_AT | 12 | 12 |
| S6_MASS_LIGHT | FUZZY_AT | 0 | 12 |
| **S7_TUNE_HOT** | **PID_AT** | **0** | **12 (refine applied)** |
| **S7_TUNE_HOT** | **FUZZY_AT** | **0** | **12 (refine applied)** |
| S8_TUNE_COLD | PID_AT | 0 | 0 (see note) |
| S8_TUNE_COLD | FUZZY_AT | 0 | 0 (see note) |
| S9_TUNE_SLOW_INTEGRAL | PID_AT | 0 | 12 |
| S9_TUNE_SLOW_INTEGRAL | FUZZY_AT | 0 | 12 |
| S12_COMPOUND_WORST | PID_AT | 0 | 9 |
| S12_COMPOUND_WORST | FUZZY_AT | 0 | 9 |

S7_TUNE_HOT (the plan's own named acceptance case) went from 0/4 minimum observations
(`S7_DIRECTION_CHECK: INCONCLUSIVE`) to harvesting the ring's full 12-slot capacity and actually
applying a gain refinement, moving `belief_k_dc` toward the true plant gain on both arms:

- PID_AT: `before_gap=19.6229` `after_gap=16.7938` -- `S7_DIRECTION_CHECK: PASS (moved toward true plant)`
- FUZZY_AT: `before_gap=19.6229` `after_gap=16.8334` -- `S7_DIRECTION_CHECK: PASS (moved toward true plant)`

**S8_TUNE_COLD still harvests zero, and this is NOT this fix's defect.** Its per-firing
`harvest_reason` is blank (`"(none this firing)"`), not the "duty still oscillating" string this fix
addresses -- meaning the duty-stability gate never even got a chance to refuse it. S8's mistuned
belief (`belief_k_dc=78.49`, roughly double the true plant gain of ~39.25) drives the PID to command
a duty low enough to sit under `ADAPTIVE_TUNE_MIN_DUTY_FOR_OBSERVATION` (0.03) for the whole dwell
-- a separate, correctly-functioning guard (per CLAUDE.md's task constraints, not weakened by this
pass) doing its job: a steady-state duty too small to trust the duty/rise ratio at all. This is a
real, separate finding (an over-estimated gain belief can make a zone's own duty too small to ever
self-correct via this harvesting path) worth a follow-up, but it is outside the harvest-GATE defect
this pass was scoped to fix.

S12_COMPOUND_WORST now harvests 9/12 (above the 4-observation minimum) but does not apply a
refinement -- this is a separate downstream guard (the fit/jump-ratio/cumulative-bound family in
`adaptive_tune_refine_zone_locked()`, untouched by this pass) declining to apply, not a harvest
failure.

`any_chain_ever_harvested` and the suite-wide `INFO: at least one chain harvested...` gate were
already green before this pass (S0/S6 alone satisfied it) and remain green; the qualitative change
is that 8 of 11 scenarios now harvest at all, versus 2 of 11 before.

## Tests

Added to `firmware/KilnFW/App/test/test_adaptive_tune_dwell.c` (registered in `test_adaptive_tune.c`'s
`run_test_adaptive_tune()`), using the real production `adaptive_tune_zone_tick()` exactly like this
file's existing duty-stability tests:

- `test_overshooting_entry_dwell_still_harvests()`: a 20-tick (600s at this file's 30s test cadence)
  dwell whose first 4 ticks are a wide, fast duty swing (0.05/0.95/0.10/0.85 -- the ramp-to-dwell
  fight) followed by 16 ticks genuinely settled at ~0.50 +/- 0.01, flat temperature throughout.
  Asserts exactly one observation is recorded, AND (the quality check, not just a count) that the
  recorded `duty`/`rise_c` reflect the SETTLED value (duty within 0.05 of 0.50; rise_c exactly 78.0
  = 100 - 22), not a relic of the entry transient.
- `test_persistent_duty_oscillation_never_harvests()`: 40 ticks (1200s, several full window tumbles)
  of a genuine limit cycle alternating 0.20/0.80 every tick, for the WHOLE dwell. Asserts zero
  observations are ever recorded -- proves the trailing window did not turn the gate into something
  that eventually passes anything given enough ticks.
- The two pre-existing tests in the same file (`test_oscillating_duty_flat_temperature_is_refused()`,
  `test_genuinely_steady_duty_is_still_accepted()`) continue to pass unchanged and serve as the
  "does not reject everything" / "does not accept everything" pair for the SHORT-dwell case where
  the window never tumbles at all.

## Negative test

Broke production code by reading the WHOLE-DWELL accumulator (`z->settle_duty_max - z->settle_duty_
min`) instead of the new trailing window (`z->settle_window_duty_max - z->settle_window_duty_min`)
in the duty-stability check, rebuilt (`build_host_tests.ps1`), and confirmed RED:

```
FAIL C:\...\test_adaptive_tune_dwell.c:113: a dwell with a rough entry that genuinely settles for
its back half must still harvest one observation -- this is the WI-8/S7 defect this pass fixes
```

Restored the fix BY HAND (re-typed the correct field names, not `git checkout`/`restore`), deleted
`firmware/KilnFW/App/test/build/` entirely, and reran `build_host_tests.ps1` from a clean build:
**42/42 host test executables built and passed**, including both new tests.

## Constraints respected

- No `ZONES_CFG_VERSION` bump: still 26 (`firmware/KilnFW/App/drivers/persist/zones_config_json.h`).
- `autotune_baseline_k_dc` envelope untouched (this pass never touches `adaptive_tune_ki.c` or the
  cumulative-bound logic).
- `strength_pct == 0` bit-exact contract and no-model-runs-plain-PID: untouched (this pass's only
  change is inside `adaptive_tune_zone_tick()`'s harvesting gate, which runs regardless of fuzzy
  strength and does not feed back into the control loop itself).
- `sim_iter_tune.exe` (unrelated module, `firmware/KilnFW/App/drivers/control/iter_tune.c`, never
  touched by this pass) still reports `OVERALL: PASS` from a full rebuild -- Part 1 (24 zone-runs,
  0 regressed), A1 (PASS against its pinned ceiling), A2/A5/A6 (all PASS) -- consistent with a
  module this pass never modified.

## Check tally

`firmware/KilnFW/App/test/build_host_tests.ps1` (PowerShell, `-ExecutionPolicy Bypass`, full clean
rebuild after the negative-test restore): **Built: 42/42 executables, all 42 host test executables
built and passed.** The suite's own informational `sim_credibility_gate` (ramp MAE 5/1, dwell offset
1/5, dwell-entry peak 10/2/6, noise-floor spread 2/4) is unrelated to this pass (it gates
`sim_scenarios.c`'s plant-model credibility against captured firings, not `adaptive_tune`) and is
explicitly informational-only per its own printed line -- pre-existing and outside this pass's
scope (owned by `docs/audits/sim_credibility_gate_real_cause_2026-09-10.md`, not touched here).
