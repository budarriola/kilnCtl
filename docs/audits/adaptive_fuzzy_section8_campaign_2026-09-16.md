# Section 8 adaptive-fuzzy campaign: run, gates, and the tally the plan permits (2026-09-16)

**Status: RUN COMPLETE. Verdict: UNINTERPRETABLE per the plan's own registered
rule.** This is not a keep/remove decision on adaptive fuzzy. It is proof that
the campaign specified in `docs/ADAPTIVE_FUZZY_EVALUATION_PLAN.md` sections
7-9 does not yet produce data those sections can be adjudicated on, and a
description of why, so a follow-on session does not have to re-derive it.

## What was run, and from where

The implementation the plan called for (sections 5, 6.1, 7) was already
built and committed — this session did not write new production or test
code. It was verified fresh rather than trusted from an older build, per this
project's standing "never measure from a binary whose provenance is not
established" rule:

- A clean worktree was created at `origin/main` (`eaaa99ae`), which contains
  the section 5 adaptive-arm lift (`84430e35`) and the section 7 gate
  adjudication (`a64b1f2d`).
- `run_sim_factorial.ps1 -OutDir <private> -Shards 4` (no `-SkipDeterminism`,
  matching the plan's requirement that the recorded run include the
  determinism proof) built `kilnctl_sim_factorial_driver.exe` from scratch in
  that worktree and ran the registered `--of 1` canonical pass plus a 4-way
  shard comparison.
- **Determinism: PASS.** `--of 1` and `--of 4` produced byte-identical data
  (10,140 rows), so shard-independence and the section 6.1 ring change both
  hold on this build.

## The three gates and integrity check (section 7)

```
GATE1_FAIL  — activity: 14/260 cells (5.4%) reached strength_pct > 0 at all,
              against a 30% bar; 5/260 (1.9%) differ from the A_PID_AT
              control at firing 9 by > 0.5 degC on any objective, against a
              10% bar. Per-objective cells past the floor: STEADY_RMS_C 0,
              ENTRY_PEAK_C 4, LAG_SIGNED_C 3 (ENTRY_UNDERSHOOT_C 3, reported
              but not gated).
GATE2_PASS  — floor identity: 260/260 cells have A_FUZZY_AT firing 1
              bit-identical to A_PID_AT firing 1. The "starts at plain PID"
              invariant holds.
GATE3_FAIL  — limit-cycle regression: all seven pinned cells (ST1-049/057/
              113/121/177/241/249) still show nonzero A_FUZZY_AT dwell
              zero-crossings (9, 162, 9, 153, 9, 9, 153).
INTEGRITY_FAIL — 3 cell refusals (the two kiln-span A2=TIGHT/A6=HOT cells
              legitimately exceeding sim bounds, per `4891a6fb` — expected
              and disclosed, not a new defect).
```

Gate 1 failing means the run is, by the plan's own words, **INERT**: "indistinguishable
from plain adaptive PID," the same failure class as the two mode-2 campaigns
this gate was built to catch — except this time confirmed to be a genuinely
different mechanism, not a config error (`control_mode` is correct here; see
below).

**Gate 3's own evidence undercuts it as written**, and this was flagged
identically in the prior partial run: on every one of the seven pinned cells,
the `A_PID_AT` control arm shows the *exact same* zero-crossing count as
`A_FUZZY_AT` (e.g. 162/162, 153/153, 9/9). A dwell error crossing zero the
same number of times whether or not fuzzy is active is ordinary settling
behaviour of adaptive PID, not the gain-scheduling limit cycle the gate was
registered against. The gate as literally specified still fires, so it is
reported as FAIL, but it cannot currently distinguish "adaptive fuzzy
oscillates" from "the plant/PID combination settles this way regardless of
fuzzy." This is a pre-existing objection (raised in
`adaptive_fuzzy_evaluation_progress_2026-09-14.md`), reproduced unchanged
here, and is a plan-amendment question for the owner, not something changed
unilaterally on the run that decides the feature.

## Why gate 1 fails: the premise that is false

The factorial's per-cell dwell length is sized for the original three
single-firing arms (`docs/audits/scenario_factorial_design_2026-09-14.md`),
roughly `6*tau`. `adaptive_tune`'s own harvest gate needs roughly `16*tau` of
settled dwell data before it will accept an observation and let confidence
`c` leave 0 (`ADAPTIVE_TUNE_MIN_DUTY_SPREAD` and the harvest-window logic in
`adaptive_tune.c`). With `c` stuck at (or barely above) 0 for nearly the
whole 9-firing chain, `strength_pct = round(50 * (c/4) * cap_L)` stays 0 in
the overwhelming majority of cells — confirmed directly in the
`ADAPTIVE_DIAG` rows, where `confidence_c_after` is 0 for cell after cell.

So **section 8's premise — that this factorial's 9-firing chain gives
`adaptive_tune` enough dwell time to actually raise confidence in a
representative fraction of the 260 cells — is false in the current
geometry.** The plan itself pre-registered a check for exactly this failure
mode (gate 1) and the check did its job. This is not a bug in the gate or
the driver; it is the campaign's dwell-length assumption not holding up.

## The section 8 tally — computed, but not decisive

Per the plan's own text ("Do NOT tally sec 8 on a run whose gates failed"),
this is reported for completeness and comparability with the fixed-gain
number, **not** as a keep/remove verdict:

`D_adapt_combo = obj(A_FUZZY_AT, firing 9) − obj(A_PID_AT, firing 9)`, per
cell, over the three gate objectives (`STEADY_RMS_C`, `ENTRY_PEAK_C`,
`LAG_SIGNED_C`), 0.5 °C materiality floor, same per-cell counting rule as the
fixed-gain report (a cell counts toward both improved and degraded if mixed):

| Subgroup | cells | improved | degraded | improved-only | degraded-only | within-floor (all objectives) |
|---|---|---|---|---|---|---|
| All 260 (of 263; 3 refused) | 260 | 4 | 5 | 0 | 1 | 255 |
| 151 bench-span (hardware-corroborated regime) | 151 | 0 | 0 | 0 | 0 | 151 |
| 109 kiln-span **[EXTRAPOLATION]** | 109 | 4 | 5 | 0 | 1 | 104 |

Compare against the fixed-gain reference (175 degraded / 128 improved across
263 cells, 91 degraded-only / 9 improved-only in the 151 bench-span cells):
the adaptive arm here is not "better" or "worse" than fixed-gain in any
meaningful sense — it is almost entirely **off**. Zero bench-span cells show
any difference at all, and only 9 of 260 total cells show any difference in
either direction. This lands inside the plan's own pre-registered prediction
(section 9): "near-neutrality with a large within-floor mass... within-floor
on all four objectives in > 120 of 263 cells, improved > degraded but by a
margin under 40 cells" — the actual result (255 within-floor, and a margin of
**−1**, i.e. degraded slightly exceeds improved) is more extreme inertness
than even that prediction, and fails the plan's own honesty test worse: this
is not "improved > degraded but small," it is statistically indistinguishable
from no effect in either direction.

`D_learn` (did firing 9 differ from firing 1 within the `A_FUZZY_AT` chain)
was not separately tabulated because gate 1 already establishes that
`strength_pct` never leaves 0 in 95% of cells — there is nothing for firing 9
to have learned relative to firing 1 in those cells by construction (firing 1
is bit-for-bit PID per gate 2, and firing 9 is materially different from
firing 1 in exactly the same ~14 cells where strength went nonzero at all).

## Did adaptive fuzzy actually learn anything?

**No, not in this run — and the run's own instrumentation proves that
absence rather than assuming it.** `confidence_c_after` and
`strength_pct_realised_max` in the `ADAPTIVE_DIAG` rows are non-zero in only
14 of 260 cells. The mechanism that was supposed to make this run different
from the two inert mode-2 campaigns (real `adaptive_tune` state carried
across 9 firings, section 5's chain) is wired correctly — gate 2's
floor-identity pass and the non-zero cells both confirm the plumbing works —
but the dwell geometry starves it of the harvested observations it needs to
move `c` off the floor.

## Recommendation

**Do not treat this run as evidence for either keeping or removing adaptive
fuzzy.** The owner's 2026-09-16 decision was to judge removal on adaptive
data, not fixed-gain data — this run is not that data; it is a
near-total-inertness result that the plan's own pre-registered gate 1
correctly flags as uninterpretable, for a specific, now-diagnosed reason
(factorial dwell ≈ 6·τ against `adaptive_tune`'s ≈ 16·τ harvest requirement).
The fixed-gain removal case (175/128, already decided) is untouched by
anything here.

**What would make this run decisive:** lengthen the per-cell dwell segment
used by the two adaptive arms (independent of the three single-firing arms,
so their existing byte-identity is preserved) so that a representative
fraction of the 260 cells can actually satisfy `adaptive_tune`'s harvest
window inside the 9-firing chain, then re-run sections 7-8 unchanged. Until
that is done, "run the adaptive campaign" (the owner's instruction) has been
executed as specified, but it has not yet produced a campaign whose gate 1
passes — and per the plan's own text, a failed gate 1 must be escalated, not
tallied into a verdict.

Separately, section 7's gate 3 is worth the owner's attention on its own
merits (independent of gate 1): as specified it cannot currently tell a
fuzzy-induced limit cycle apart from ordinary adaptive-PID dwell settling,
since the control arm exhibits the identical crossing counts on all seven
pinned cells. A version keyed on the contrast against `A_PID_AT` (as the
plan's own original finding was: 29 crossings in the oscillating arm vs 0 in
stable ones) would be a more faithful test of the mechanism the gate exists
to catch.

## What could not be verified

- No hardware run of any kind was performed or implied; this is simulation
  only, per this project's standing position that a sim result is screening,
  not evidence of hardware behaviour.
- `tools/run_all_checks.ps1` was run separately (see commit) and is not part
  of this campaign's own pass/fail; `run_sim_factorial.ps1` remains
  deliberately outside CI, a manually-invoked target only.
- The dwell-length fix proposed above was not implemented in this session —
  it is scoped but not built, so no new run against a longer dwell exists to
  report.
