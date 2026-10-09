# Why S2, S4, S7, S12 still cannot harvest after the bucketed-window fix (2026-09-14)

## Question

After `8ac578a7` (bucketed sliding duty-stability window, replacing the
whole-dwell min/max that could never un-see an entry transient),
`adaptive_tune` harvests in 6 of 11 scenarios (S0, S1, S3, S5, S6, S9). S8's
zero is already explained and correct (`ADAPTIVE_TUNE_MIN_DUTY_FOR_
OBSERVATION` gate, 42 hits at `duty = 0.00000`, not touched here). This
leaves four unexplained: **S2, S4, S7, S12** — including S7 (`TUNE_HOT`),
the plan's own named acceptance case. This pass diagnoses, without changing
any gate, why each of the four refuses.

## Method

`adaptive_tune_zone_tick()` (`firmware/KilnFW/App/drivers/control/
adaptive_tune.c`) was instrumented with a temporary per-category counter
(`s_at_diag_hist[]`, values `AT_DIAG_SETTLE_WAIT` / `AT_DIAG_TEMP_DRIFT` /
`AT_DIAG_DUTY_WINDOW_WAIT` / `AT_DIAG_DUTY_UNSTABLE` / `AT_DIAG_LOW_DUTY` /
`AT_DIAG_BAD_RISE` / `AT_DIAG_RECORDED`), one increment at each of the
function's seven mutually-exclusive exit points, in the same order the
function evaluates them. `firmware/KilnFW/App/test/sim_scenarios_adaptive.c`
(WI-8's real-production-code 9-chained-firing harness, already linking
`adaptive_tune.c`/`adaptive_tune_model.c`/`adaptive_tune_ki.c` directly, not a
mirror) was given one added call, `adaptive_tune_diag_reset()` at the top of
`run_chain()` and a snapshot-and-print at the bottom, so each `(scenario,
arm)` chain's histogram covers exactly the 9 firings x 2 dwells that chain
ran, nothing carried over from a previous chain. Built with `build_host_
tests.ps1 -OutDir C:\wt\at_diag_build` (private out-dir per this repo's
concurrency convention) and run as part of the normal
`kilnctl_sim_scenarios_adaptive.exe` pass, which still reported **PASS**
(11 scenarios run, 2 skipped by design — S10/S11's dynamic-scale/retune
scope exclusion, unrelated to this instrumentation). Both instrumentation
sites (the .c and the test file) are marked `TEMP DIAGNOSTIC` /
`TEMP INSTRUMENTATION` inline and are reverted by hand after this document
is written — see "Revert" below.

## The refusal histogram, per scenario (both arms, summed over 9 firings x 2 dwells = 18 dwell-instances each)

| Scenario | Arm | settle_wait | temp_drift | duty_window_wait | duty_unstable | low_duty | bad_rise | recorded |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| S2_SENSOR_NEAR_ELEMENT | A_PID_AT | 3222 | 48501 | 18 | 24219 | 0 | 0 | **0** |
| S2_SENSOR_NEAR_ELEMENT | A_FUZZY_AT | 3222 | 47916 | 63 | 24759 | 0 | 0 | **0** |
| S4_SENSOR_NEAR_FAST_RAMP | A_PID_AT | 3222 | 55206 | 81 | 17451 | 0 | 0 | **0** |
| S4_SENSOR_NEAR_FAST_RAMP | A_FUZZY_AT | 3222 | 52344 | 81 | 20313 | 0 | 0 | **0** |
| S7_TUNE_HOT | A_PID_AT | 3222 | 10377 | 1035 | 137304 | 0 | 0 | **0** |
| S7_TUNE_HOT | A_FUZZY_AT | 3222 | 13527 | 270 | 134919 | 0 | 0 | **0** |
| S12_COMPOUND_WORST | A_PID_AT | 3222 | 85347 | 261 | 63108 | 0 | 0 | **0** |
| S12_COMPOUND_WORST | A_FUZZY_AT | 3222 | 85824 | 135 | 62757 | 0 | 0 | **0** |

`recorded=0` in every row: **none of the four scenarios ever harvests a
single dwell observation, in either arm, across all 9 chained firings.**
`low_duty` and `bad_rise` are 0 everywhere because the run never gets past
the duty-stability gate to reach them — those two gates are simply never
exercised for these scenarios.

`settle_wait` is identical (3222) across every row: this is `18 dwells x
~180 ticks` (the fixed `ADAPTIVE_TUNE_SETTLE_MIN_S` wait, at `dt_s=1`),
present in every scenario regardless of mismatch and therefore uninteresting
— it is not where these four differ from the six that harvest.

The real split is `temp_drift` vs `duty_unstable`, and both are large in
every row. Summing each row (`settle_wait + temp_drift + duty_window_wait +
duty_unstable`) accounts for essentially the entire dwell length in ticks
(confirmed against `dwell_ticks = 16 * model_tau_s` from `sim_scenarios_
adaptive.c:348`, `dt_s=1s` so ticks == seconds): e.g. S7's model_tau_s is
`BENCH_TAU_S * 2.0` (`SIM_MISTUNE_HOT.mismatch_tau`, `sim_mistune.h`), so
`dwell_ticks ≈ 16 * 528 ≈ 8448` per dwell x 18 dwells ≈ 152,064 ticks — S7's A_PID_AT row sums to `3222+10377+1035+137304 =
151,938`, matching within rounding of the `model_tau_s`/`BENCH_TAU_S`
estimate above (histogram only increments while `dwelling` is true, so
ramp-segment ticks are correctly excluded from both sides of this check).

## Dwell duration vs the 300 s duty-window requirement

The duty-stability window needs `ADAPTIVE_TUNE_DUTY_WINDOW_NUM_BUCKETS (10)
x ADAPTIVE_TUNE_DUTY_WINDOW_BUCKET_S (30) = 300 s` of closed-bucket history
before it renders any verdict — before that, every tick is `duty_window_
wait`. Each scenario's actual dwell length (`dwell_ticks = 16 * model_tau_s`
seconds, at `dt_s = 1 s`):

| Scenario | model_tau_s (tune's belief) | dwell length | vs 300 s |
|---|---:|---:|---|
| S2_SENSOR_NEAR_ELEMENT | `BENCH_TAU_S` (matched, ~264 s) | ~4224 s | 14x the requirement |
| S4_SENSOR_NEAR_FAST_RAMP | `BENCH_TAU_S` (matched, ~264 s) | ~4224 s | 14x the requirement |
| S7_TUNE_HOT | `BENCH_TAU_S * 2.0` (~528 s) | ~8448 s | 28x the requirement |
| S12_COMPOUND_WORST | `BENCH_TAU_S * 2.0` (~528 s) | ~8448 s | 28x the requirement |

Every one of these dwells is comfortably longer than 300 s — `duty_window_
wait` is the smallest nonzero category in every row (18 to 1035 ticks out of
tens of thousands), confirming the window matures early in the dwell and is
not the blocker. **The 300 s requirement is not a structural limitation for
any of these four scenarios** — this rules out finding #3 from the task
(a design-level "window too long for real firing profiles" problem) for
this set. It may still be a real concern for a genuinely short real-firing
dwell not represented in this suite, but that is a different question from
the one these four scenarios raise.

## Is the duty genuinely unsettled, or is this a gate-shape defect?

The per-firing `harvest_reason` string (peeked from `adaptive_tune_zones[0].
last_refusal_reason` immediately before `adaptive_tune_run_end()` overwrites
it, i.e. the LAST verdict computed in that firing's second dwell) makes the
magnitude of the "oscillation" directly readable, not just its presence:

```
S2_SENSOR_NEAR_ELEMENT A_PID_AT: duty still oscillating (range 1.000, 100% of 1.000)
S4_SENSOR_NEAR_FAST_RAMP A_PID_AT: duty still oscillating (range 1.000, 106% of 0.939)
S7_TUNE_HOT A_PID_AT: duty still oscillating (range 0.690, 103% of 0.671)
S12_COMPOUND_WORST A_PID_AT: duty still oscillating (range 1.000, 0% of 0.000)
```

`ADAPTIVE_TUNE_DUTY_STABILITY_ABS` is 0.05 (5 duty-percentage-points) and
`_FRAC` is 0.25 (25% of the current duty). Every one of these four scenarios
is failing that test by a wide margin, not marginally: S2 and S4 are
swinging duty across the FULL `[0,1]` range (relay fully on to fully off)
inside their trailing 300 s window, at the very end of an 8-firing-warmed,
4224-second dwell — a factor of 20x over the absolute floor. S7 and S12
are swinging ~0.69-1.0 of full range, similarly 14-20x the floor. This is
not a borderline case a tighter or looser threshold would flip — it is
sustained, full-amplitude relay chatter that persists through the ENTIRE
dwell (the `duty_unstable` tick count is comparable to or larger than the
whole post-settle dwell length in every row above, not a shrinking tail
that ages out of a 300 s window as time passes). If the window were
defective in the way the pre-`8ac578a7` whole-dwell min/max was — baking in
only the ENTRY transient — the oscillation category would shrink to a small
fraction of ticks late in a long dwell as the window slides past the entry.
It does not: `duty_unstable` fires at comparable magnitude from settle-onset
to dwell-end (confirmed by the unchanged `harvest_reason` string being
identical, digit-for-digit, across firings 7, 8, and 9 of the same chain for
S7/S4 above — the terminal-tick verdict is not improving run over run
either).

**Verdict for all four: CORRECT refusal, not a defect.** These four
scenarios' mismatches (sensor-placement bias for S2/S4, `TUNE_HOT`'s
over-aggressive SIMC gains for S7, all three compounded for S12) produce a
genuinely undamped or marginally-stable closed loop whose duty output never
settles for the dwell's whole duration. `duty_unstable` dominating (or
`temp_drift` dominating, in S12's and S2/S4's case — see below) over
`duty_window_wait` in every row is the signature of a real, sustained limit
cycle, not an artifact of window shape or timing. Harvesting from any of
these dwells and feeding the `(duty, rise)` pair to `adaptive_tune_refine_
zone_locked()`'s fit would be exactly the failure mode `8ac578a7` was fixed
to prevent: taking a duty sample from mid-oscillation and reporting it as a
steady-state gain point.

One secondary observation, reported but not investigated further (out of
this pass's "diagnose, do not fix" scope): `temp_drift` is also large in
every row (10,377 to 85,824 ticks) — the settled-temperature test is itself
frequently failing too, independent of duty. A zone whose duty never
stabilizes will generally also show measurable temperature drift (an
oscillating duty pumps a slowly wandering mean into the thermal mass), so
this is consistent with, not contradictory to, the duty-oscillation finding
above — both gates are seeing the same underlying instability from two
different signals.

## Consequence, stated plainly

`adaptive_tune`'s only path to a K_dc observation is a dwell whose duty has
genuinely settled. For every scenario in this suite where the *starting*
tune is bad enough to leave the loop persistently oscillating through an
entire dwell — S2, S4, S7, S12, and separately S8 by the already-explained
`MIN_DUTY_FOR_OBSERVATION` route — that observation never arrives, in any of
9 chained firings, under either the plain-PID or the fuzzy arm. **This is
not a bug to fix by loosening a gate: the four scenarios examined here show
real, full-amplitude, sustained relay oscillation, and harvesting through it
would re-introduce the exact defect `8ac578a7` closed.**

The honest structural conclusion: **`adaptive_tune` cannot rescue a kiln
whose starting tune is bad enough to prevent the dwell duty from settling at
all.** It can refine a tune that is already close enough to produce a
genuinely steady dwell (the six scenarios that do harvest — S0, S1, S3, S5,
S6, S9 — share that property; note S7 is precisely the plan's named "never
good" acceptance case, and it is exactly the one that cannot settle). The
owner's "improves with firings" requirement is therefore met only for kilns
already tuned well enough to settle a dwell — for the badly-mistuned case
where the requirement matters most, this mechanism structurally cannot act,
because its sole observable (a settled dwell's duty/rise pair) does not
exist in that regime. A fix, if one is wanted, is not a gate adjustment on
this path — it would need a different observable (e.g. identification from
the ramp segment's transient response, which does not require settling) or
an explicit fallback tuning strategy for the "duty never settles" case,
which is a design question for a future pass, not something this diagnosis
resolves.

## Revert

Both instrumentation sites are temporary and are reverted by hand
immediately after this document is committed:
- `firmware/KilnFW/App/drivers/control/adaptive_tune.c`: the `TEMP
  DIAGNOSTIC INSTRUMENTATION` enum/histogram/accessor block after
  `ADAPTIVE_TUNE_TAG`, and the eight `s_at_diag_hist[...]++; // TEMP
  DIAGNOSTIC` increments inside `adaptive_tune_zone_tick()`.
- `firmware/KilnFW/App/test/sim_scenarios_adaptive.c`: the `adaptive_tune_
  diag_reset()` call near the top of `run_chain()` and the `ADAPTIVE_DIAG`
  snapshot-and-print block near the bottom of the same function, both
  tagged `TEMP DIAGNOSTIC`.

After hand-reverting, both files are rebuilt from a clean, private
`-OutDir` (never the shared `App/test/build`) and `kilnctl_sim_scenarios_
adaptive.exe` is re-run to confirm it still reports PASS with the
pre-instrumentation output shape.
