# Setpoint-weight `b` sweep, single-zone sim (2026-09-22)

Verifying the "NOT YET TESTED" candidate recorded in `docs/FUZZY_CONTROLLER_PLAN.md`
sec 0.0.2 and `ROADMAP.md` (~line 833): `pid.c`'s `p_term = kp*(b*setpoint -
measurement)`, with `b` hardcoded to `1.0` via `PID_SETPOINT_WEIGHT_B`
(`firmware/KilnFW/App/drivers/control/profile_executor_internal.h:253`, one call
site, `profile_executor_run.c:623`). Confirmed still `1.0` and still untested
before running anything (`git log -S PID_SETPOINT_WEIGHT_B` shows no commit has
ever changed the literal; no test file references the constant).

## Method

One-off scratch harness (not committed; deleted after this run), built fresh
into a private `-OutDir` before every measurement and never re-measured from a
prebuilt binary. It duplicates `firmware/KilnFW/App/test/sim_fuzzy_overshoot.c`'s
own ramp-to-dwell scenario, plant wiring, and metric definitions exactly (same
three dwell phases, same zone-0 measured constants, same
`FIRING_SUBSCORE_ENTRY_PEAK_C`/`FIRING_SUBSCORE_LAG_S`/`FIRING_SUBSCORE_SETTLE_S`-
equivalent windows), with fuzzy strength permanently 0 (`pid_fuzzy_adjust()` is
never linked or called) and `b` swept instead of fuzzy strength/bands. Links
real production `pid.c` (`pid_update`/`pid_reset`) and `sim_plant.c`
unmodified. Base gains: `kp=0.0318 ki=0.0001 kd=0.8401` (same baseline
`sim_fuzzy_overshoot.c` uses). Ramp rate: 100 degC/hr. `b` in {1.0, 0.8, 0.6,
0.4} per the task brief.

Scope limits carried over from `sim_fuzzy_overshoot.c` unchanged: single-zone
only, bench-scale plant (max ~40C above ambient, not kiln-scale ~1200C).

Differences under 0.5 degC (or 30s for lag/settle-time figures, matching that
file's own materiality bar) are ignored per instructions.

## Results

| b | dwell | overshoot (degC) | undershoot, entry-window (degC) | ramp-lag median (s) | settle time (s) | steady RMS (degC) |
|---|---|---|---|---|---|---|
| 1.0 | 0 (→39C, from below) | 0.000 | 6.313 | 183.5 | 245 | 0.140 |
| 1.0 | 1 (→54C, from below) | 0.000 | 6.314 | 183.5 | 245 | 0.140 |
| 1.0 | 2 (→36C, from above)* | 6.475 | 0.000 | 198.8 | 255 | 0.146 |
| 0.8 | 0 | 0.000 | 9.195 | 264.0 | 485 | 0.510 |
| 0.8 | 1 | 0.000 | 7.330 | 197.9 | 490 | 1.887 |
| 0.8 | 2* | 7.010 | 0.000 | 167.4 | 345 | 0.305 |
| 0.6 | 0 | 0.000 | 12.433 | 272.5 | 705 | 0.992 |
| 0.6 | 1 | 0.000 | 9.441 | 212.3 | **NEVER** | 7.879 |
| 0.6 | 2* | 4.998 | 0.000 | 111.3 | 275 | 0.259 |
| 0.4 | 0 | 0.000 | 14.902 | 272.5 | **NEVER** | 2.484 |
| 0.4 | 1 | 0.000 | 14.802 | 323.3 | **NEVER** | 13.875 |
| 0.4 | 2* | 1.048 | 0.096 | 211.7 | 0 | 0.042 |

IAE over the full scenario (ranking only, not one of the scored objectives):
b=1.0: 12225.9 degC*s; b=0.8: 18281.9 degC*s; b=0.6: 31271.8 degC*s; b=0.4:
49833.1 degC*s — monotonically worse as `b` drops.

\* Dwell 2 approaches from above (a ramp-down); per `sim_fuzzy_overshoot.c`'s
own documented finding (opus review, `docs/audits/fuzzy_overshoot_measurement_2026-09-13.md`
Finding 3), its "overshoot" reading is a ramp-down residual (the measurement
is still above target when the dwell begins), not real overshoot — the same
artifact that file already excludes from its own aggregation. Read this row's
overshoot figure as "how fast the trailing residual decayed," not as an
overshoot event.

## Finding

**Lowering `b` does not reduce overshoot in this scenario and materially
regresses every other objective measured, with two dwells failing to settle
at all at `b<=0.6`.** Dwells 0 and 1 (the two "approach from below" cases,
where real overshoot risk actually lives) show **zero** overshoot at every
tested `b`, including the shipped `b=1.0` — there is no overshoot on this
scenario/gain pair for `b` to fix. What `b<1.0` does instead:

- Undershoot at dwell entry roughly **doubles to triples** (6.3C → 14.9C at
  dwell 0 going from b=1.0 to b=0.4).
- Ramp-lag median rises (183.5s → 272.5s at dwell 0).
- Settle time balloons (245s → 705s at dwell 0, b=0.6) and **two dwells never
  settle at all** within the 2000s window at b=0.6 (dwell 1) and b=0.4
  (dwells 0 and 1).
- Steady-state RMS error grows by over an order of magnitude in the worst
  case (0.140C → 13.875C, dwell 1, b=1.0 vs b=0.4).
- Dwell 2's ramp-down residual does shrink monotonically with `b` (6.475C →
  1.048C) — but per the artifact note above this is not a real overshoot
  reduction, and it is bought at the cost of every other number above.

This is not a "no material effect" (sub-0.5C) result — it is a clear,
multi-objective regression, mechanically explained by the reduced P-term's
setpoint response: with this baseline's very small `ki` (0.0001), the
integral term is too slow to make up the ground the shrunken P-term gives up
at each new dwell target, so the loop takes far longer to reach and then hold
the target, and at `b<=0.6` it does not converge within the window at all for
some dwells.

## Recommendation

**Do not adopt `b<1.0` with the current gain set.** The candidate is falsified
for this plant/gain pair: there is no overshoot here to fix, and every value
tested below 1.0 regresses ramp-lag, settle time, and steady-state accuracy,
non-trivially. This closes the "NOT YET TESTED" status for the shipped
default (`b=1.0` stays correct as shipped) — it does not close the setpoint-
weighting idea outright, since a materially larger `ki` might change this
picture, but that is a different, untested combination and not represented by
this sweep.
