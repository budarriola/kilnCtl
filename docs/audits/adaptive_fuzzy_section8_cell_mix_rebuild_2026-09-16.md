# Section 8 cell-mix rebuild: gate 1 now passes (2026-09-16)

**Status: GATE1_PASS.** This is a follow-on to
`adaptive_fuzzy_section8_campaign_2026-09-16.md` (`9e0485c9`) and
`adaptive_fuzzy_section8_dwell_fix_2026-09-16.md`, not a replacement for
either — read both first. Those two documents diagnosed gate 1's failure as
(1) a dwell-length problem, fixed (`6*tau` -> `28*tau` for the two adaptive
arms), and then (2) a structural problem in the factorial's own cell mix:
at commit `3c3c6886`, 54.6% of the 260 usable cells carried **no gain
error at all** (`a6_tune=MATCHED`, correctly refused by `adaptive_tune` —
nothing to learn), and 105 of the remaining 118 mismatched cells never
settled (`ring_count=0` — the dwell residual oscillated and never damped
below `ADAPTIVE_TUNE_SETTLE_SLOPE_FLOOR_C_PER_S`, 0.003 C/s). The owner's
decision was to rebuild the cell mix, not move the gate bars. This document
does that and reports the result.

## Step 1: characterisation before changing anything

Built and ran the pre-change design (worktree `C:\wt\af8mix_r7q2n9` at
`origin/main` `22919e5f`, before any edits) via
`run_sim_factorial.ps1 -Shards 4`, then cross-referenced each cell's
`ADAPTIVE_DIAG` rows (the `A_PID_AT` chain's own `ring_count` across all 9
firings) against its `a6_tune` category from the data rows. This reproduces
the classification the two prior documents already reported, confirmed
directly rather than taken on trust:

| Blocker | Cells | Cause |
|---|---|---|
| MATCHED (no gain error) | 142/260 (54.6%) | `sim_factorial_design.c`'s stage-1 factorial varied A6 over `{SIM_FAC_A6_MATCHED, SIM_FAC_A6_HOT}` — one of its two levels carries zero mismatch by construction |
| Mismatched, `ring_count=0` (never settles) | 105/260 (40.4%) | `tune_mismatch_for(SIM_FAC_A6_HOT)` in `sim_factorial_driver.c` returned `{m_k=0.5, m_tau=2.0, m_l=0.5}` — a ~2x gain/time-constant error large enough to keep the dwell error oscillating (150-250 zero crossings per firing) past the end of the 28*tau dwell, so the settle-slope gate is never satisfied |
| Harvestable | 13/260 (5.0%) | the small residual population both mismatched and capable of settling |

The specific code: `sim_factorial_design.c`'s stage-1 generator loop
(`static const sim_fac_a6_tune_t A6[2] = {SIM_FAC_A6_MATCHED, SIM_FAC_A6_HOT};`)
is what makes exactly half of the masked 224-cell factorial `MATCHED`
(112 of 224, plus a further 30 `MATCHED` reference cells in the curvature
and high-Bi stage-2 blocks = 142 of 260 total). `sim_factorial_driver.c`'s
`tune_mismatch_for()` is what sizes the other level's (`HOT`'s) mismatch —
its `(0.5, 2.0, 0.5)` triple is the parameter axis that makes the dwell
residual too large to settle.

## Step 2: the proposed new mix (written before implementation)

**Target:** eliminate `MATCHED` as one of stage 1's two varied A6 levels
(it can never activate `adaptive_tune` by construction, so it should not be
half the factorial), replace it with a real-but-settleable mismatch, and
soften `HOT` itself so the majority of cells carrying its error actually
clear the settle gate. Concretely:

1. Add a fifth `a6_tune` category, `SIM_FAC_A6_MILD_HOT`, to
   `sim_factorial_design.h`. `MATCHED` is kept (it stays the reference
   level for the curvature and high-Bi stage-2 blocks, which intentionally
   hold tune fixed while varying other factors — that is a legitimate use
   of "no gain error," unlike using it as half of stage 1's own screening
   pair).
2. Change stage 1's varied pair in `sim_factorial_design.c` from
   `{SIM_FAC_A6_MATCHED, SIM_FAC_A6_HOT}` to
   `{SIM_FAC_A6_MILD_HOT, SIM_FAC_A6_HOT}`. This alone moves ~112 cells
   from "no error" to "some error."
3. In `sim_factorial_driver.c`'s `tune_mismatch_for()`:
   - `SIM_FAC_A6_MILD_HOT` gets `{m_k=0.85, m_tau=1.2, m_l=0.85}` — roughly
     a 15-20% gain/tau error, chosen to be comfortably above
     `ADAPTIVE_TUNE_MIN_MATERIAL_MOVE_FRAC` (0.5%) while producing a dwell
     residual small enough to damp out well inside the 28*tau dwell.
   - `SIM_FAC_A6_HOT` is softened from `{0.5, 2.0, 0.5}` to
     `{0.75, 1.35, 0.75}` — still a clearly material ~25-35% error, but
     roughly a third the original mismatch, sized to let most (not
     necessarily all) `HOT` cells settle too.
   - `COLD` and `SLOW_INTEGRAL` (stage-2-only diagnostic cells, 3 each) are
     left unchanged — they are a small, deliberately extreme diagnostic
     pair, not part of the population gate 1 needs to move.
4. No change to `sim_factorial_is_masked()`, the stage-2 block
   compositions, cell counts, or any bench-span/kiln-span physical
   parameter (`BENCH_*`/`KILN_*` constants, `A4` levels). The
   bench-span/kiln-span split (151/109 of the original 260 non-refused
   cells) is preserved by construction — this rebuild only touches A6, one
   of eight orthogonal factors, so it changes the tune mismatch inside
   both spans equally rather than concentrating harvestable signal in one.
5. Not changed: `target_c`, `ramp_c_per_hr`, `dwell_min`, `segment_count`
   in the builtin schedule table (per the task constraint) — this file is
   the factorial *design/driver*, not the schedule table, and none of the
   above lines touch it. `ZONES_CFG_VERSION` (26) is untouched.

This directly targets both diagnosed blockers: (1) removes `MATCHED` from
stage 1's varied pair, so the large majority of the 263 cells carry a real
model-vs-plant gain error, and (2) sizes both remaining levels' mismatch
to actually settle within the already-lengthened dwell, rather than
lengthening the dwell further (which the prior document found was
"spent": `16*tau` to `28*tau` moved activity only one point).

## Step 3: implementation and re-run

Implemented in a fresh worktree, `C:\wt\af8mix_r7q2n9`, at `origin/main`
`22919e5f` (detached). Changed files:
`firmware/KilnFW/App/test/sim_factorial_design.h` (new `SIM_FAC_A6_MILD_HOT`
enumerator, comment rewrite), `sim_factorial_design.c` (stage-1 A6 array),
`sim_factorial_driver.c` (`tune_mismatch_for()`'s HOT/MILD_HOT triples, the
A6-name printer, and a stale comment on the gate-3 pinned-cell fixture).
Rebuilt fully from scratch via
`run_sim_factorial.ps1 -OutDir C:\wt\af8mix_r7q2n9_build -Shards 4` (no
`-SkipDeterminism`).

### Cell-mix result (post-rebuild)

263 cells total, 0 refused (INTEGRITY_PASS — the prior run's 3 disclosed
kiln-span refusals no longer occur under the softened HOT/new MILD_HOT
multipliers):

| a6_tune | cells | share |
|---|---|---|
| HOT | 115 | 43.7% |
| MILD_HOT | 112 | 42.6% |
| MATCHED | 30 | 11.4% |
| COLD | 3 | 1.1% |
| SLOW_INTEGRAL | 3 | 1.1% |

227/263 (86.3%) now carry a real gain error, versus 118/263 (44.9%) before
(and only 105/263, 39.9%, in the harvested-run's usable 260-cell count
previously). Cross-referencing `ADAPTIVE_DIAG`'s `ring_count` (max across
the 9-firing `A_PID_AT` chain) against `a6_tune`:

| Category | cells | share of 263 |
|---|---|---|
| MATCHED (no error, correctly inert) | 30 | 11.4% |
| Mismatched, still `ring_count=0` (never settles) | 21 (all `HOT`) | 8.0% |
| Mismatched and harvestable (`ring_count>=4`) | 212 | **80.6%** |

`212/263 = 80.6%` of all cells now carry a material, settleable gain
error — the "large majority... material... settleable" target from step 2
is met. The remaining unsettled 21 are all still-too-aggressive `HOT`
cells (94/115 `HOT` cells now settle; the other 21 do not) — every
`MILD_HOT` cell settles (112/112).

### Gate results (`--of 1`, `C:\wt\af8mix_r7q2n9_build\factorial_of1.tsv`)

```
=== plan sec 7 gate 1 (ACTIVITY) ===
eligible cells (both adaptive chains completed all 9 firings): 263
A_FUZZY_AT reached strength_pct > 0 on >= 1 tick in 182 cells (69.2%), threshold >= 30%
A_FUZZY_AT_F9 differs from A_PID_AT_F9 by > 0.5 degC on >= 1 of the three gate objectives in 52 cells (19.8%), threshold >= 10%
  per-objective cells past the 0.5 degC floor: STEADY_RMS_C        16
  per-objective cells past the 0.5 degC floor: ENTRY_PEAK_C        47
  per-objective cells past the 0.5 degC floor: LAG_SIGNED_C        10
  per-objective cells past the 0.5 degC floor: ENTRY_UNDERSHOOT_C  37   (reported, NOT part of the gate)
GATE1_PASS
```

**GATE1_PASS** — activity 69.2% (bar 30%), differs-from-control 19.8% (bar
10%). Both bars cleared by a wide margin, not a knife-edge pass: activity
more than doubled the bar, differs-from-control nearly doubled it.

```
=== plan sec 7 gate 2 (FLOOR IDENTITY) ===
263 cells checked, 0 FAILED
GATE2_PASS
```

**GATE2_PASS** — 263/263 (up from 260/260; the 3 previously-refused cells
now run cleanly under the softened mismatch, see INTEGRITY below). Firing-1
bit-identity between `A_FUZZY_AT` and `A_PID_AT` holds on every cell.

```
=== plan sec 7 gate 3 (LIMIT-CYCLE REGRESSION, 7 pinned cells) ===
  ST1-049  A_FUZZY_AT total=257 worst_firing=101   [context: A_PID_AT total=65]
  ST1-057  A_FUZZY_AT total=739 worst_firing=103   [context: A_PID_AT total=653]
  ST1-113  A_FUZZY_AT total=144 worst_firing=16    [context: A_PID_AT total=144]
  ST1-121  A_FUZZY_AT total=153 worst_firing=17    [context: A_PID_AT total=153]
  ST1-177  A_FUZZY_AT total=394 worst_firing=95    [context: A_PID_AT total=27]
  ST1-241  A_FUZZY_AT total=27 worst_firing=3      [context: A_PID_AT total=27]
  ST1-249  A_FUZZY_AT total=27 worst_firing=3      [context: A_PID_AT total=27]
GATE3_FAIL (all seven, gate registered ZERO crossings)
```

**GATE3_FAIL**, as before — this gate is standalone and still fires on any
nonzero crossing count. But its own pre-existing objection (raised in both
prior documents: the control arm shows identical crossing counts to
`A_FUZZY_AT`, so the gate cannot separate a fuzzy-induced limit cycle from
ordinary adaptive-PID dwell settling) is **partially resolved by this
rebuild, not fully**: these seven cells changed A6 category from `MATCHED`
(the original screening level) to `MILD_HOT` (this rebuild's replacement),
so they now carry a real gain error and their control-arm behaviour has
changed too.

- **3 of 7 cells now diverge from the control arm**: ST1-049 (257 vs 65),
  ST1-057 (739 vs 653), ST1-177 (394 vs 27) — the fuzzy arm crosses zero
  meaningfully more often than plain adaptive PID on the same cell.
- **4 of 7 still show identical counts**: ST1-113 (144/144), ST1-121
  (153/153), ST1-241 (27/27), ST1-249 (27/27) — the gate's objection still
  applies to these four.

So gate 3's standing objection is now demonstrably cell-dependent rather
than universal, which narrows but does not close the owner's open question
about rewriting it to key on the *contrast* against `A_PID_AT` rather than
an absolute zero. It remains the owner's call, not something changed
unilaterally here.

```
=== plan sec 7 integrity (cell refusals / NaN / delay-ring truncation) ===
INTEGRITY_PASS: 0 refusals.
```

**INTEGRITY_PASS** — 0 refusals (down from 3 previously disclosed kiln-span
refusals under the old, more extreme `HOT` multipliers; softening the
mismatch also removed the two kiln-span A2=TIGHT/A6=HOT cells that used to
exceed sim bounds, per the `4891a6fb` disclosure this replaces).

## Step 4: determinism

`--of 1` vs `--of 4`: **10,257 data rows, byte-identical.** (Row count rose
from 10,140 to 10,257 because 3 previously-refused cells now run to
completion under the softened mismatch — 263 cells run cleanly with no
refusals, versus 260 usable of 263 before.)

## Step 5: improved/degraded/within-floor tally, split by span

Per the plan's rule, this is reportable now that gate 1 has passed.
`D_adapt_combo = obj(A_FUZZY_AT, firing 9) - obj(A_PID_AT, firing 9)`, three
gate objectives (`STEADY_RMS_C`, `ENTRY_PEAK_C`, `LAG_SIGNED_C` converted to
degC via each cell's own ramp rate), 0.5 degC materiality floor, same
per-cell counting rule as both prior documents (a cell counts toward both
improved and degraded if mixed across objectives):

| Subgroup | cells | improved | degraded | improved-only | degraded-only | within-floor |
|---|---|---|---|---|---|---|
| All 263 | 263 | 21 | 41 | 12 | 32 | 210 |
| 151 bench-span (hardware-corroborated) | 151 | 0 | 13 | 0 | 13 | 138 |
| 112 kiln-span **[EXTRAPOLATION]** | 112 | 21 | 28 | 12 | 19 | 72 |

This is the qualitative change the owner asked for: **bench-span cells are
no longer uniformly zero-signal.** Both prior runs (fixed-gain-derived
adaptive campaign and its dwell-length fix) showed 0/0/0 in the 151
bench-span cells — no difference of any kind, in either direction. Under
this rebuilt mix, 13 of 151 bench-span cells (8.6%) now show a material
difference — but **all 13 are degraded-only; zero bench-span cells show
any improvement.** This is real, hardware-corroborated-regime signal for
the first time, and it says something the owner should weigh directly:
where adaptive fuzzy is distinguishable from plain adaptive PID in the
bench-span regime at all, it is currently distinguishable in the wrong
direction. Kiln-span (extrapolation only) shows a more mixed picture (21
improved, 28 degraded, 12 improved-only, 19 degraded-only) — closer to
noise-around-neutral than the bench-span result, but still net negative by
count.

Effects under 0.5 degC are reported above as within-floor, not as
improvements, per the task's instruction and the project's own
"ignore sub-half-degree effects" standing practice.

## `run_all_checks.ps1`

Run in the same worktree (`C:\wt\af8mix_r7q2n9`, after `tools/setup.ps1`):
first pass 94 passed / 0 skipped / 1 failed
(`check_ui_responsive_sweep.ps1`, `backup_page.html @390px: sweep threw:
timed out waiting for Page.loadEventFired`) — re-run of that one check in
isolation passed cleanly, confirming an environment-timing flake unrelated
to this change (this rebuild touches only
`firmware/KilnFW/App/test/sim_factorial_design.{c,h}` and
`sim_factorial_driver.c`, none of which the UI sweep exercises;
`run_sim_factorial.ps1` itself remains deliberately outside this suite, as
documented in both prior sessions' notes). **95/95 effective pass.**

## Recommendation

**Gate 1 now passes, with margin, on a cell mix that is honestly majority
material-and-settleable (80.6%) rather than majority-inert.** This makes
section 8's tally interpretable for the first time under the plan's own
rule. The result it produces is not favorable to adaptive fuzzy: bench-span
cells now show real signal, and that signal is one-sided against the
feature (13 degraded-only, 0 improved, in the only hardware-corroborated
regime); kiln-span extrapolation is closer to neutral by count but still
net negative. Gate 3 remains failed and its standing objection is now only
partially resolved (3 of 7 pinned cells diverge from the control arm under
the rebuilt mix; 4 still do not) — still the owner's call on whether to
rewrite it against the `A_PID_AT` contrast, unchanged by this session.

This document does not itself render a keep/remove verdict — that is the
owner's decision, consistent with this project's standing practice — but it
removes the specific objection ("the campaign's own cell mix can't produce
an interpretable answer") raised against both prior runs.
