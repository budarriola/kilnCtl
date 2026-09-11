# Fuzzy controller: offline 9-cell rule-table probe (2026-09-11)

Stage 0 of `docs/FUZZY_CONTROLLER_PLAN.md` sec 5 ("(v-a) offline rule-cell
probe") called for feeding synthetic error/rate through `pid_fuzzy_adjust()`
covering all 9 cells and recording the gain triple per cell at strength 50,
as a cheap, no-kiln-time prerequisite to the delete/keep/schedule decision
in that plan. This is that probe: `firmware/KilnFW/App/test/fuzzy_nine_cell_probe.c`,
wired into `build_host_tests.ps1` (37th `Invoke-HostTestExe` call) and run on
every host-test build.

It links the real, unmodified `pid_fuzzy_adjust()` (`firmware/KilnFW/App/drivers/control/pid_fuzzy.c`,
commit `5a16e950`/`3ad2c787` context) — never a reimplementation — and drives
each of the 9 cells to fire alone at membership weight 1.0 by setting error
and rate to the exact triangular-membership band edges (`-band`, `0`,
`+band`), where `triangular_memberships()` returns `{1,0,0}` / `{0,1,0}` /
`{0,0,1}` exactly.

This complements, and does not duplicate, `sim_fuzzy_closedloop.c`
(`fbdc5bd0`): that harness answers whether a closed-loop trajectory can
*reach* all 9 cells (via injected disturbances); this probe answers what
each cell actually *does* to the gains, in physical units, once it fires.

## Per-cell table

Bands used: `error_band_c = 20.0`, `rate_band_c_per_s = 0.5` (documented
firmware defaults, `ERROR_BAND_C_DEFAULT`/`RATE_BAND_C_PER_S_DEFAULT`).
Factor = actual `out_gain / base_gain` from the real function (base gains
set to 1.0 so the returned value is the factor directly), not the rule
table's raw `-1/0/+1` direction integers.

| # | error (degC) | rate (degC/s) | rule (Kp,Ki,Kd dir) | kp x @25% / @50% | ki x @25% / @50% | kd x @25% / @50% | Reachability |
|---|---|---|---|---|---|---|---|
| 1 | -20 (NEG/overshoot) | -0.5 (FALLING) | +,-,+ (attack, overshoot growing) | 1.125 / 1.25 | 0.875 / 0.75 | 1.125 / 1.25 | disturbance only |
| 2 | -20 (NEG/overshoot) | 0 (STEADY) | +,=,= (push harder) | 1.125 / 1.25 | 1.0 / 1.0 | 1.0 / 1.0 | normal operation |
| 3 | -20 (NEG/overshoot) | +0.5 (RISING) | -,+,- (recovering, ease off) | 0.875 / 0.75 | 1.125 / 1.25 | 0.875 / 0.75 | disturbance only |
| 4 | 0 (ZERO) | -0.5 (FALLING) | -,-,+ (crossing fast, damp) | 0.875 / 0.75 | 0.875 / 0.75 | 1.125 / 1.25 | disturbance only |
| 5 | 0 (ZERO) | 0 (STEADY) — **CENTRE** | -,+,- (settled, coast on I) | **0.875 / 0.75** | **1.125 / 1.25** | **0.875 / 0.75** | **normal operation — the only cell ever observed on hardware** |
| 6 | 0 (ZERO) | +0.5 (RISING) | +,-,+ (just left target, catch it) | 1.125 / 1.25 | 0.875 / 0.75 | 1.125 / 1.25 | disturbance only |
| 7 | +20 (POS/undershoot) | -0.5 (FALLING) | -,+,- (closing in, ease off) | 0.875 / 0.75 | 1.125 / 1.25 | 0.875 / 0.75 | disturbance only |
| 8 | +20 (POS/undershoot) | 0 (STEADY) | +,=,= (push harder) | 1.125 / 1.25 | 1.0 / 1.0 | 1.0 / 1.0 | normal operation |
| 9 | +20 (POS/undershoot) | +0.5 (RISING) | +,-,+ (far and worsening, attack) | 1.125 / 1.25 | 0.875 / 0.75 | 1.125 / 1.25 | disturbance only |

All 9 factors above were produced by the probe calling the real
`pid_fuzzy_adjust()`, not computed by hand and asserted separately — the
probe's own hand-derived expected literals (from `pid_fuzzy.h`'s documented
rule-table comment, not from the private `RULE_TABLE` array itself) matched
the real function's output to within float rounding on every cell, at both
strength 25 and 50.

## Centre cell — the entire live behaviour

Cell 5 (error=0, rate=0) is the only cell the real system has ever occupied:
100% of `fuzzy_ab_20260904d_s50_run1.jsonl`'s 2178 zone-samples landed here.
Its real multiplicative effect:

- **strength_pct = 25: kp x0.875, ki x1.125, kd x0.875**
- **strength_pct = 50: kp x0.75, ki x1.25, kd x0.75**

Since `strength_pct = 0.0` on all three zones today (`project_fuzzy_ab_inert_control_mode.md`),
this rescale is currently applied nowhere — but it is the entirety of what
the fuzzy layer would do if `strength_pct` were raised without changing
anything else about how a firing behaves (i.e., without ever leaving the
centre cell). It is a constant detune of Kp/Kd and boost of Ki at steady
state, not adaptive control, exactly as `pid_fuzzy_controller_improvement_scoping_2026-09-11.md`'s
finding (A) already argued from the hardware capture — this probe confirms
the same number from the rule table's own math, independent of that one
capture.

## Reachability verdicts (against this plant's measured envelope)

- Max real ramp rate observed on this hardware: ~0.083 degC/s
  (`pid_fuzzy.c`'s own header comment; a brisk 300 degC/hr profile ramp).
- Rate band half-width: 0.5 degC/s — roughly 6x the fastest ramp this plant
  produces. Reaching a non-zero rate bucket (FALLING or RISING) pure
  requires `|rate| >= 0.5`, which an ordinary ramp cannot do; only a genuine
  disturbance (a stuck lid, a runaway element, a thermocouple snap) can, per
  `pid_fuzzy.c`'s own header comment and `sim_fuzzy_closedloop.c`'s
  coverage scenario, which needed synthetic +-6/+-8 degC shocks to reach
  these cells at all.
- Max bench rise: ~40 degC above ambient (`project_bench_is_a_4w_test_fixture.md`).
  The error band half-width (20 degC) sits well inside that range, so a
  large error (NEG or POS bucket, e.g. early in a heat-up before the loop
  has closed in) is reachable in ordinary operation.

Net: **6 of 9 cells (every cell with a non-zero rate bucket) are reachable
only under disturbance, not in normal operation on this plant.** The
remaining 3 (both error extremes at STEADY rate, plus the centre cell) are
reachable in normal operation. None of the 9 cells is flatly unreachable —
error and rate are both open to fault-driven excursions (see the "closing
in"/"just left target" language in `pid_fuzzy.h`'s own rule-table comment,
which anticipates exactly this class of event) — but 8 of 9 have never
actually fired on this hardware to date (`fuzzy_controller_improvement_scoping_2026-09-11.md`),
consistent with 6 of them requiring a disturbance this bench has not
produced and the other 2 (large-error/steady-rate) simply never having
occurred during the one real mode-3 capture.

## Assertions this probe makes (not just prints)

1. `strength_pct == 0` reproduces base gains bit-for-bit at all 9 cells'
   own coordinates (the safety contract `pid_fuzzy.h` documents).
2. Each of the 9 cells' actual kp/ki/kd factor at strength 25 and 50 matches
   an independently hand-derived expected value (from the documented rule
   table, not from `pid_fuzzy.c`'s private array) to within `1e-5`.
3. The centre cell's factor is asserted exactly against the documented
   0.75/1.25/0.75 (strength 50) and 0.875/1.125/0.875 (strength 25) values.

**Negative-tested**: `RULE_TABLE[1][1]`'s Kp direction was flipped from
`-1.0f` to `1.0f` in `pid_fuzzy.c` (the centre cell). The probe failed with
4 named assertion failures (cell 5 and centre-cell kp factors at both
strengths, e.g. "cell 5 strength=50: kp factor 1.250000 != expected
0.750000"). The change was reverted by hand; `git diff
firmware/KilnFW/App/drivers/control/pid_fuzzy.c` is empty after the revert.

## What this changes

Nothing in controller behaviour. `pid_fuzzy.c`/`.h` are unmodified (confirmed
by the empty diff above). This adds one new host-test executable and this
document. `strength_pct` remains 0 on all three zones.

## Bearing on the plan's decision

Per `FUZZY_CONTROLLER_PLAN.md` sec 4.3: this probe does not show the 9
cells are "individually sensible" in a way that weakens finding (A) — the
centre cell's detune stands exactly as measured (0.75/1.25/0.75 at
strength 50, not `{0,0,0}`), and 6 of 9 cells are confirmed reachable only
under disturbance, not through ordinary operation. This is consistent with,
not a rebuttal of, the plan's finding (A) and its recommendation to proceed
to Stage 1 ((v-b), the `effective_target_c` alignment fix) rather than
reopening option (i) or (ii).
