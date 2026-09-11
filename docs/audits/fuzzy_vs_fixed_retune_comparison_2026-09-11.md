# Fuzzy vs. fixed-gain retune: a controlled comparison in simulation (2026-09-11)

## Question

`docs/audits/fuzzy_nine_cell_offline_probe_2026-09-11.md` established that the
centre rule cell -- the only one a real firing has ever visited -- reduces to a
fixed multiplier triple on the base gains: at `strength_pct` 50, `kp x0.75, ki
x1.25, kd x0.75` (at 25: `x0.875, x1.125, x0.875`). The open question: is the
tracking-error improvement the `sim_fuzzy_closedloop.c` harness (`fbdc5bd0`)
reports at `strength_pct` 50 actually caused by fuzzy *inference*, or is it
simply because that fixed multiplier triple is a better PID tuning than the
current gains, with the "fuzzy" switching contributing nothing?

## Method

All four arms run `firmware/KilnFW/App/test/sim_fuzzy_closedloop.c`'s
`run_tracking_scenario()` -- the ordinary ramp/dwell/ramp-down scenario, **no
injected disturbances**, same seed, same 5 s tick, same plant (zone 0's real
measured FOPDT constants), same duration for every arm. This is the scenario
the file's own header says is the meaningful ranking metric; the harness's
separate coverage scenario injects synthetic +-8C shocks specifically to
reach otherwise-unreachable rule cells and does not report IAE/MAE at all, so
**there is no disturbance-bearing IAE/MAE data source in this harness** to
report as a second column -- only the without-disturbance case exists here,
and that is also the case this repo's own guidance says describes the real
plant (ordinary ramps top out at ~0.083 degC/s, far under the 0.5 degC/s rate
band).

Arms (a) and (b) are the harness's own built executable
(`firmware/KilnFW/App/test/build/kilnctl_sim_fuzzy_closedloop.exe`), run
unmodified -- no edits made to any tracked file for these two.

Arms (c) and (d) needed gains pre-multiplied with the fuzzy layer entirely out
of the loop. Rather than edit the production/test file in place, two throwaway
copies of `sim_fuzzy_closedloop.c` were made in the session scratch directory
with only the three `#define BASE_K{P,I,D}` constants changed to the
pre-multiplied values, then compiled against the **unmodified** production
`pid.c`, `pid_fuzzy.c`, and `sim_plant.c` using the same
`host_tests_common_flags.rsp` the real build produces. At `strength_pct=0`
`pid_fuzzy_adjust()` is a documented, verified passthrough (the harness's own
safety-contract assertion), so reading that arm's `strength_pct=0` row is
exactly "gains pre-multiplied, fuzzy layer never touched." No tracked file was
edited for this experiment; confirmed with:

```
$ git status --porcelain -- firmware/KilnFW/App/test/sim_fuzzy_closedloop.c \
    firmware/KilnFW/App/drivers/control/pid_fuzzy.c \
    firmware/KilnFW/App/drivers/control/pid.c
(no output)
```

## Results

| Arm | Config | IAE (degC*s) | MAE (degC) |
|---|---|---|---|
| (a) control | fuzzy OFF, current gains | **13749.7** | **3.3052** |
| (b) | fuzzy ON, `strength_pct=50`, current gains | 11796.0 | 2.8356 |
| (b25) | fuzzy ON, `strength_pct=25`, current gains | 12716.0 | 3.0567 |
| (c) decisive arm | fuzzy OFF, gains x(0.75 kp, 1.25 ki, 0.75 kd) always on | 12721.1 | 3.0579 |
| (d) | fuzzy OFF, gains x(0.875 kp, 1.125 ki, 0.875 kd) always on | 13033.7 | 3.1331 |

**Reproduction check (must-pass gate):** (a) reproduces the task's quoted
control figures (IAE 13749.7 / MAE 3.3052) **exactly**, confirming the setup
is correct.

**Important discrepancy to flag:** the task brief's quoted figures for
fuzzy-ON at strength 50 (IAE 12675.2 / MAE 3.0469) and strength 25 (IAE
13089.4) do **not** match what this unmodified, already-built harness
actually reports (11796.0/2.8356 and 12716.0 respectively) -- confirmed by
running the existing built executable
(`kilnctl_sim_fuzzy_closedloop.exe`, current HEAD, `fbdc5bd0` unchanged since
it shipped, `git log` shows a single commit on this file). Since (a)
reproduces exactly and (b)/(b25) do not, the brief's fuzzy-ON numbers appear
to be from a different run or a stale/illustrative source, not this repo's
current state. All conclusions below use the numbers this harness actually
produces, not the brief's assumed ones.

## Interpretation

**(c) vs (b), the decisive comparison:** (c) is *worse* than (b) -- IAE
12721.1 vs 11796.0, about **7.8% worse**; MAE 3.0579 vs 2.8356, a **+0.2223
degC absolute** gap (also ~7.8% relative). A fixed, always-on application of the
centre cell's own multiplier triple does **not** reproduce fuzzy-ON's
improvement -- it is measurably worse than letting the fuzzy layer switch. The
same pattern holds at the 25% point: (d) 13033.7 vs (b25) 12716.0, ~2.5%
worse.

So the answer to the literal question is: **fuzzy switching is doing
something the flat retune does not**, on this specific scenario. The most
likely mechanism, given the scenario shape (a ramp then a dwell then a
ramp-down): during the ramp and ramp-down legs the trajectory sits away from
the ZERO/STEADY centre cell (large sustained error while climbing/descending
toward the dwell target), so `pid_fuzzy_adjust()` there applies close to
*no* gain change (near-1.0 multiplier), leaving the stronger original kp/kd
in effect exactly when tracking a moving target needs them; only near the
dwell plateau does the centre-cell reduced-kp/kd, boosted-ki mix take over.
The always-on arm applies that reduced-kp/boosted-ki mix for the whole
trajectory, including the ramp legs where it apparently hurts. This is
consistent with, not contradictory to, the nine-cell probe's finding that
the centre cell dominates in practice -- it dominates by tick-count on the
one real capture, but the ramp legs (rare in tick-count, since the dwell is
long) are where the *fixed-vs-adaptive* difference actually shows up.

**Materiality -- the more important question.** Per this repo's own standing
instruction not to chase sub-0.5 degC effects:

- (a) vs (b), i.e. the full effect fuzzy-ON at strength 50 has over doing
  nothing at all: MAE difference is **0.4696 degC** (3.3052 - 2.8356) --
  under the 0.5 degC line, but only barely; this is not the ~0.26 degC the
  task brief anticipated, it is close enough to the threshold that "clearly
  negligible" is not an honest characterization.
- (c) vs (b), the decisive arm itself: MAE difference is **0.2223 degC** --
  comfortably under the 0.5 degC line even though the *relative* IAE gap
  (7.8%) sounds larger than that.

**Which metric matters for a decision:** MAE is the one to use here, not IAE.
IAE integrates the same per-tick error over ~2800+ ticks in this scenario, so
a per-sample difference that individually would be called immaterial
accumulates into an IAE gap that *looks* large (hundreds to ~2000 degC*s) purely
from run length, not from any single moment being far off. MAE is the
apples-to-apples per-sample number the 0.5 degC standing rule is written
against, and every MAE gap measured here (0.47, 0.22, 0.08 degC across the
three comparisons) sits at or under that line.

## Verdict

1. **Fuzzy inference is not "doing nothing beyond a retune."** The decisive
   arm (c) is measurably worse than fuzzy-ON (b) on this plant/scenario, so
   the adaptive switching is contributing something real relative to a naive
   always-on application of the same multiplier -- this is a point *against*
   simply deleting the layer and hardcoding the centre-cell numbers as a flat
   retune (plan option iv), at least not without re-tuning that flat retune
   properly (which was never attempted here -- only the literal centre-cell
   triple was tested statically, which is a different exercise than
   re-deriving an optimal fixed PID from scratch).
2. **But none of these differences clear this repo's own 0.5 degC
   materiality bar on a per-sample basis.** The largest MAE gap found (fuzzy
   OFF-vs-ON at strength 50, 0.47 degC) is close to but still under the
   threshold; the decisive (c)-vs-(b) gap that actually answers "does fuzzy
   contribute anything" is smaller still (0.22 degC). Per the standing
   instruction, this whole effect -- in either direction -- is not something
   to act on: not a strong case for investing further in the fuzzy layer,
   and not a strong case for deleting it in favor of a flat retune either.
   The honest characterization is "measurable in this simulation, below the
   line this project uses to decide what is worth chasing on hardware."
3. Caveat carried from the harness's own header and repeated here: this is a
   single-zone, disturbance-free ranking metric only, never compared to a
   hardware capture, and the harness's fuzzy-ON figures at strength 25/50
   used in this report do not match the numbers assumed in the task that
   requested this comparison (see "Important discrepancy" above) --
   worth resolving before this result is used to justify anything on
   hardware.
