# Adversarial review: four 2026-09-11 sim/fuzzy commits (2026-09-13)

Scope: `fbdc5bd0` (sim_fuzzy_closedloop harness), `ed854ac5` (nine-cell probe),
`ba230bca` (fuzzy-vs-fixed-retune comparison doc), `c9ce6b7c` (flat-coupling-scale
discriminator). All four landed the same day without an opus review pass and feed
a decision chain that has already reached the owner as conclusions.

Every hash above was verified with `git cat-file -t`. No production code, and none
of the four commits' own files, were edited by this review. All reproduction
builds were done in the session scratch directory against the unmodified tracked
sources; `git diff` on `firmware/KilnFW/App/drivers/control/pid_fuzzy.c`,
`pid.c`, `firmware/KilnFW/App/test/sim_fuzzy_closedloop.c` and
`firmware/KilnFW/App/test/sim_plant.c` is empty.

---

## Headline: `ba230bca`'s two fuzzy-ON arms were measured on a binary built from a deliberately broken rule table

This is the single most important finding, and it inverts `ba230bca`'s verdict.

### What was reported

`ba230bca` flagged a discrepancy against `fbdc5bd0` and resolved it the wrong way
round. It reported fuzzy-ON at strength 50 as IAE 11796.0 / MAE 2.8356, said the
brief's 12675.2 / 3.0469 "appear to be from a different run or a stale/illustrative
source," and built its verdict on 11796.0.

### What is actually true

Rebuilding `sim_fuzzy_closedloop.c` from tracked sources at HEAD reproduces
`fbdc5bd0`'s numbers **exactly**, to the last printed digit:

```
strength 0 : IAE=13749.7  MAE=3.3052
strength 25: IAE=13089.4  MAE=3.1465
strength 50: IAE=12675.2  MAE=3.0469
=== sim_fuzzy_closedloop: PASS ===
```

Running the prebuilt executable still sitting in
`firmware/KilnFW/App/test/build/kilnctl_sim_fuzzy_closedloop.exe`
(mtime 2026-09-11 10:34:46, i.e. built ~10 minutes before `ed854ac5` was committed
at 10:44:54) reproduces `ba230bca`'s numbers instead:

```
strength 0 : IAE=13749.7  MAE=3.3052   <- identical to the fresh build
strength 25: IAE=12716.0  MAE=3.0567
strength 50: IAE=11796.0  MAE=2.8356
```

The strength-0 row is bit-identical between the two binaries and only the
fuzzy-active rows differ. That localises the difference to the fuzzy path alone.

`ed854ac5`'s own commit message states its negative test was "flipped
`RULE_TABLE[1][1]`'s Kp direction." Reproducing exactly that — a scratch copy of
`pid_fuzzy.c` with the centre cell's `kp` direction changed from `-1.0f` to
`+1.0f`, production file untouched — and rebuilding the harness gives:

```
strength 0 : IAE=13749.7  MAE=3.3052
strength 25: IAE=12716.0  MAE=3.0567
strength 50: IAE=11796.0  MAE=2.8356
```

An exact match on all six numbers. **Root cause: `build_host_tests.ps1` rebuilt
`kilnctl_sim_fuzzy_closedloop.exe` while `pid_fuzzy.c` was mid-negative-test for
`ed854ac5`, and that binary was left in the build directory. `ba230bca`
deliberately used that prebuilt executable for arms (b) and (b25) rather than
rebuilding, so both of its fuzzy-ON arms measure a controller whose centre rule
cell had its Kp direction inverted.**

This is **not** a determinism defect in the harness — the harness is fully
deterministic and reproduces byte-identically from source. It is a stale-artifact
defect, the same class as `project_move_item_force_not_atomic.md` and
`project_idf_build_silently_noops_under_msys.md`: a build product outliving the
source state that produced it, with no signal. It is made worse by the harness
being a `gating` test: a negative test that rebuilds all 37 host-test binaries
leaves at least one poisoned artifact behind whenever the tested revert happens
after the build.

### Corrected comparison table

Arms (c) and (d) reproduce exactly as published (verified: scratch copies of
`sim_fuzzy_closedloop.c` with only `BASE_KP/KI/KD` pre-multiplied, compiled
against unmodified `pid.c`/`pid_fuzzy.c`/`sim_plant.c`, strength-0 row read):

| Arm | Config | IAE | MAE | status |
|---|---|---|---|---|
| (a) control | fuzzy OFF, current gains | 13749.7 | 3.3052 | reproduced exactly |
| (b) | fuzzy ON, strength 50 | **12675.2** | **3.0469** | **corrected** (doc said 11796.0 / 2.8356) |
| (b25) | fuzzy ON, strength 25 | **13089.4** | **3.1465** | **corrected** (doc said 12716.0 / 3.0567) |
| (c) decisive | flat x0.75 / x1.25 / x0.75 | 12721.1 | 3.0579 | reproduced exactly |
| (d) | flat x0.875 / x1.125 / x0.875 | 13033.7 | 3.1331 | reproduced exactly |

### What this does to `ba230bca`'s verdict

The doc's decisive comparison was (c) vs (b). With the corrected (b):

- **Strength 50:** (c) 12721.1 vs (b) 12675.2 — the flat retune is **0.36% worse**,
  not 7.8% worse. MAE gap **0.011 degC**, not 0.2223 degC.
- **Strength 25:** (d) 13033.7 vs (b25) 13089.4 — the flat retune is **0.43%
  better** than fuzzy-ON. The sign of the effect **reverses** between the two
  strength settings.

**`ba230bca`'s verdict 1 — "fuzzy inference is not doing nothing beyond a retune"
— is refuted by its own method once the correct binary is used.** On this scenario
a flat, always-on application of the centre-cell multiplier triple is
indistinguishable from the adaptive fuzzy layer: 0.011 degC MAE apart at strength
50, and better at strength 25. The doc's "most likely mechanism" paragraph (ramp
legs keeping the stronger original kp/kd) is an explanation constructed for an
effect that does not exist.

Verdict 2 ("nothing here clears 0.5 degC") survives, and in fact strengthens
sharply: the largest gap in the corrected table is (a) vs (b) at 0.258 degC — which
is exactly the "~0.26 degC the task brief anticipated" that the doc dismissed as
not matching. Every other gap is between 0.011 and 0.013 degC, i.e. roughly
**40x below** the project's materiality line.

Incidental, and worth recording because it is the one genuinely interesting number
in the exercise: the *broken* rule table (centre cell Kp direction inverted — i.e.
"push harder when settled" rather than "coast on I") was the **best-performing**
arm of all six configurations tried, at MAE 2.8356. That is still only 0.21 degC
better than the shipped table and therefore below the line, but it is a datum
against the shipped centre-cell rule's direction, not for it.

### Recommended follow-ups (not done here — they touch files other agents own)

1. `ba230bca`'s doc needs its results table and verdict 1 corrected. Owner of
   `docs/audits/fuzzy_vs_fixed_retune_comparison_2026-09-11.md` should do it; this
   review deliberately did not edit it.
2. `build_host_tests.ps1` should be treated as producing disposable artifacts:
   any later experiment that reads a prebuilt `kilnctl_*.exe` instead of
   rebuilding is unsound. A cheap mitigation is for negative-test procedure to
   end with a full rebuild after the hand-revert, not just a `git diff` check.
   `ed854ac5`'s transcript verified the source revert but not the binaries.

---

## Commit 1 — `fbdc5bd0`, `sim_fuzzy_closedloop.c`

**"Links the real `pid_fuzzy_adjust()`, `pid_update()` and `sim_plant.c` with no
reimplementation of control math" — VERIFIED.** The build line in
`build_host_tests.ps1` compiles `sim_fuzzy_closedloop.c` together with
`test/sim_plant.c`, `drivers/control/pid.c` and `drivers/control/pid_fuzzy.c`.
The only mirrored code in the harness is `classify_memberships()`, which is used
solely for the coverage *count* and never feeds a gain, a duty, or an assertion —
the file says so itself and the code bears it out.

**Reported numbers — CORRECT.** `fbdc5bd0`'s commit-message figures reproduce
exactly from source. It was `ba230bca` that carried wrong numbers, not this
commit. Both `ba230bca`'s "Important discrepancy" note and the review brief's
framing had this backwards.

**strength_pct == 0 bit-for-bit contract — VERIFIED independently.** Reading
`pid_fuzzy.c`, the `strength_pct == 0` branch returns the `sanitize_base()` copies
before any fuzzy arithmetic. `sanitize_base()` is the identity for any finite
non-negative gain, so `==` comparison is the right test and it is checked on every
tick of both scenarios plus a dedicated 200-tick run. It also fails loudly
(`return 1`). The harness's strength-0 row was, notably, identical between the
correct and the sabotaged binary — which is itself confirmation the short-circuit
is real.

**9/9 rule-cell coverage via +-8 degC injections — the mechanism is NOT physically
coherent, and the file is honest about it.** `sim_tick()` adds the disturbance
directly to both `pstate->element_c` *and* `pstate->sensor_c`, deliberately
bypassing `sensor_delay_s` so the controller sees it the same tick. There is no
energy balance behind it: it is a state poke. The file's own header says this
plainly ("a direct, non-physical perturbation"), so this is not a concealed flaw.

But the consequence matters and is understated downstream: **cell occupancy counts
from the coverage scenario are a property of the injection script, not of the
plant.** The six off-centre cells are reached because the script was written to
reach them, phase by phase, with the target setpoint chosen per phase to land the
error axis where wanted. A "9/9 cells reached" result therefore proves the
harness can *drive* all nine rules, which is a useful regression property, and
proves nothing at all about whether a kiln ever visits them. `ed854ac5`'s
reachability verdict (6 of 9 need a fault) is the honest reading, and the two are
consistent — but anyone quoting "9/9 coverage" as evidence the fuzzy layer is
exercised in practice would be wrong.

The tracking scenario is correctly kept separate and disturbance-free, so
`ba230bca` using it (rather than the coverage scenario) as the ranking metric was
the right choice of scenario.

**Minor:** `run_tracking_scenario()` and `run_coverage_scenario()` both start with
`prev_effective_ki = 0.0f`, and `pid_rescale_integral_for_new_ki()` early-returns
when `old_ki <= 0`, so the first tick's rescale is a deliberate no-op. That is
correct (the integral is zero at that point anyway) but is not commented.

---

## Commit 2 — `ed854ac5`, `fuzzy_nine_cell_probe.c`

**Per-cell multipliers — VERIFIED by hand.** `factor = 1 + (strength/100) *
MAX_NUDGE_FRACTION * dir` with `MAX_NUDGE_FRACTION = 0.5f` gives 0.75 / 1.25 at
strength 50 and 0.875 / 1.125 at strength 25 for `dir = -1 / +1`, and exactly 1.0
for `dir = 0`. Hand-comparing the probe's `CELLS[9]` direction table against
`RULE_TABLE[3][3]` in `pid_fuzzy.c` row by row: all nine cells match, and both
match the documented table in `pid_fuzzy.h`'s header. The cell-edge coordinates
(`+-band`, `0`) do drive exactly one bucket at weight 1.0 per
`triangular_memberships()`'s boundary cases, so each cell fires in isolation as
claimed. No error found.

Caveat on independence: the probe's claim to be an "independent second source"
rests on transcribing `pid_fuzzy.h`'s comment rather than `pid_fuzzy.c`'s array.
Those are two files but one author in one pass. The protection is real but
thinner than the comment implies — it catches a later edit to the `.c` that
forgets the `.h`, which is the realistic regression, and that is worth having.

**Asserts rather than prints — VERIFIED.** `CHECK()` increments `g_failures` and
`main()` returns 1 when non-zero; it is wired through `Invoke-HostTestExe`, which
gates the build. This is not a `project_harness_prints_verdict_exits_zero.md`
case.

**The 0.083 degC/s figure — NOT a measured plant maximum. This is the weakest
load-bearing number in the four commits.**

`fuzzy_nine_cell_probe.c` calls it "this plant's own measured ~0.083 degC/s max
ramp rate" and cites `pid_fuzzy.c`'s header. That header shows where it actually
comes from: `300 degC/hr / 3600 = 0.0833 degC/s`. It is an arithmetic conversion
of an assumed "fast" *profile setting*, not a measurement of anything, and not a
plant limit. Three independent reasons it does not support the conclusion built on
it:

1. **The same header records a contradicting measurement.** The one real mode-3
   capture (`fuzzy_ab_20260904d_s50_run1.jsonl`, 2178 zone-samples) has
   "peak |rate| 0.110 degC/s, 22% of RATE_BAND_C_PER_S" — 33% *above* the 0.083
   figure the probe calls this plant's maximum. The probe's own claimed maximum is
   refuted by the only real data the module cites.
2. **Config allows far more.** `ZONE_MAX_RAMP_C_PER_HR_MAX` is 1000.0 degC/hr =
   0.278 degC/s, 3.3x the 0.083 figure and only 1.8x below the 0.5 band. Nothing
   pins a firing to 300 degC/hr.
3. **The probe's test is knife-edge.** `rate_needs_disturbance` is
   `RATE_BAND_C_PER_S > 6.0f * MAX_REAL_RAMP_RATE_C_PER_S`, i.e. `0.5 > 0.498`.
   A 0.4% margin. Substituting the measured 0.110 makes it `0.5 > 0.66`, which is
   **false**, and the probe would then print "REACHABLE IN NORMAL OPERATION" for
   all nine cells. The headline finding — "6 of 9 cells unreachable, bands are ~6x
   too wide" — is a step function on a constant that was never measured and is
   contradicted by the project's own capture.

Secondary defects in the same block: `error_reachable` is `20.0 <= 40.0`, a
compile-time true, so the third verdict branch ("UNREACHABLE ON THIS PLANT") is
dead code; and neither verdict input actually varies per cell beyond
`rate_bucket != 1`, so the nine-row verdict column carries exactly one bit of
information.

**Judgement:** the probe's *assertions* (the multiplier contract) are sound and
worth keeping. Its *reachability verdict* should be treated as unsupported until
`MAX_REAL_RAMP_RATE_C_PER_S` is replaced with a figure derived from capture data
rather than from a profile-rate conversion. The qualitative direction ("the rate
band is wider than ordinary firings reach") is probably still right — 0.110 is
22% of the band — but the quantitative "6x" and the binary "unreachable" are not
established. Not fixed here: `fuzzy_nine_cell_probe.c` is adjacent to the fuzzy
work another agent currently owns.

---

## Commit 3 — `ba230bca`, method assessment

Beyond the stale-binary finding above, the review brief asked specifically whether
the `#define BASE_K{P,I,D}` method confounds the comparison — in particular
whether integral rescaling or bumpless seeding makes the fixed arm and the fuzzy
arm non-equivalent.

**The `#define` method is sound. It is NOT the confound.** Reasoning:

- `pid_rescale_integral_for_new_ki()` early-returns unless
  `old_ki > 0 && new_ki > 0 && old_ki != new_ki`. In arms (c) and (d) `ki` is
  constant for the whole run, so the rescale is a no-op on every tick after the
  first. The fixed arm is therefore a clean constant-gain run with no rescaling
  artifact.
- In the fuzzy arm the rescale does fire whenever the blended `ki` moves, and its
  effect is exactly to hold `ki * integral` invariant across the change — i.e. it
  preserves the I contribution to duty. That is the correct bump-transfer
  semantics, and it means the fuzzy arm's I-term is continuous rather than
  stepping. So the two arms differ only in the gain trajectory, which is precisely
  the variable under test.
- Bumpless seeding is not in play at all: both arms start from `pid_reset()`, not
  `pid_seed_bumpless()`, with a zero integral.

So the brief's hypothesised confound ("fuzzy changes ki mid-run while the fixed
arm starts with the changed ki") does not bite here, because the rescale is
specifically designed to make a mid-run ki change duty-neutral at the instant it
happens. The arms are comparable configurations. The comparison was invalidated by
which *binary* was run, not by how the arms were constructed.

**Reproduction of the (a) control arm — VERIFIED.** IAE 13749.7 / MAE 3.3052,
exact.

**Does the "fuzzy beats a flat retune by 0.22 degC MAE" conclusion survive?**
No. The 0.2223 degC gap is an artifact of the sabotaged binary. The real gap is
0.011 degC at strength 50 and -0.013 degC (fuzzy worse) at strength 25. Both are
~40x under the project's 0.5 degC line, and they disagree in sign, which is the
signature of a difference at or below the metric's own resolution.

One further methodological weakness independent of the binary: the whole
comparison is `n = 1` scenario. A single ramp/dwell/ramp-down on one synthetic
plant produces one IAE number per arm, with no ensemble and no noise, so there is
no way to say whether a 0.36% IAE gap means anything even within the simulator.
`sim_iter_tune.c`'s 220-plant ensemble exists in this same tree; a ranking claim
of this size needs that kind of spread, not a single deterministic trajectory.

---

## Commit 4 — `c9ce6b7c`, flat-coupling-scale discriminator

**Factor-1.0 control reproduces the pin exactly — VERIFIED.** Running
`firmware/KilnFW/App/test/check_sim_iter_tune_bars.ps1` at HEAD:

```
660 null comparisons: ACCEPT 24 (3.64%)  REJECT 21  INSUFFICIENT 615  NO_PAIRS 0
A1 bar: PINNED KNOWN-FAILURE CEILING <= 24/660 -> PASS
```

ACCEPT 24 / REJECT 21 / INSUFFICIENT 615 matches `c9ce6b7c`'s factor-1.0 row
field for field. The revert was clean and the baseline is trustworthy.

**Are 18 / 20 / 24 / 38 separable? Mostly not.**

The simulator is deterministic, so these counts carry no run-to-run noise — the
question is sampling noise over the 660 trials, i.e. whether a different trial set
would give the same answer. Treating each trial as an independent Bernoulli draw
at the pin's rate `p = 24/660 = 0.0364`:

- `sd = sqrt(660 * 0.0364 * 0.9636) = 4.8 counts`.
- 20 vs 24 is 0.8 sd. **Not separable.**
- 18 vs 24 is 1.25 sd. **Not separable.**
- 38 vs 24: difference 14, sd of the difference `sqrt(23.1 + 35.2) = 7.6`, so
  1.8 sd, two-sided p ~ 0.07. **Suggestive, not established.**

So the honest picture is one indistinguishable cluster {18, 20, 24} plus a single
marginally elevated point at 1.173. The doc's "reject count is monotone in
|factor - 1|, accept count is not... a three-way split moving through a threshold"
reads a mechanism into a pattern the data cannot resolve; the doc even notices
mid-sentence that its INSUFFICIENT column is non-monotone too, and continues
anyway. The non-monotonicity is most simply explained as noise around a weak
monotone trend, not as a genuine threshold effect.

**The cluster-signature argument is well below resolution.** The PARTIAL verdict
turns on subscore splits *within* those counts: `LAG_S` 0 / 2 / 11 / 3 and
`ENTRY_PEAK_C` 20 / 22 / 27 / 15, plus a z2 column that is 0 everywhere in this
experiment. Counts of 2 and 11 have Poisson sd of roughly 1.4 and 3.3. Declaring
that a signature "moved" because `ENTRY_PEAK_C` went 22 -> 27 (about 1 sd) and
`LAG_S` went 2 -> 11 while comparing against a *different experiment's* split is
not a measurement this data supports. The "no z2 spread" observation is the
strongest of the three, since z2 appearing at all was novel in the schedule
attempt — but a single zone appearing in 6 of 38 accepts in one run and 0 of 38 in
another is itself about a 2 sd event, not a qualitative difference in kind.

**Verdict on the verdict:** PARTIAL is overstated in both directions, as the brief
suspected. What the experiment actually establishes is: (i) the control
reproduces, so the harness is sound; (ii) a flat 1.173x scale *may* elevate A1
accepts, at about 1.8 sd — worth one more run at a larger trial count, not worth
a recorded adjudication; (iii) 0.85 and 1.35 are indistinguishable from the pin
and support nothing. Neither "magnitude matters" nor "shape is not fungible with
it" is demonstrated. The pin staying at 24/660 is the right call regardless, since
nothing here justifies loosening it.

The cheap fix, if this question is worth resolving: the 660 trials are paired
(same trial set, perturbed model), so a per-trial verdict-change analysis — how
many individual trials flipped, and in which direction — is far more powerful than
comparing aggregate counts, and costs one more instrumented run.

---

## Cross-cutting findings

**Numbers carried forward without re-derivation.** Three instances, in a chain:

1. `pid_fuzzy.c`'s 300 degC/hr -> 0.083 degC/s conversion became "this plant's own
   measured max ramp rate" in `fuzzy_nine_cell_probe.c`, then the load-bearing
   constant in `ed854ac5`'s "6 of 9 unreachable" headline, then the justification
   in `ba230bca`'s Method section for why the disturbance-free scenario "is also
   the case this repo's own guidance says describes the real plant." At no point
   was it re-derived, and the module that originated it also records a measurement
   that contradicts it.
2. `fbdc5bd0`'s strength-50 numbers were quoted into a task brief, contradicted by
   `ba230bca` against a stale binary, and the stale numbers then became the basis
   of a verdict — which was itself then quoted into the brief for this review as
   established fact. The correction took a rebuild that nobody performed for two
   days.
3. `c9ce6b7c`'s "38/660 matches the first schedule attempt's 38/660" compares a
   count to a count from a different experiment without any uncertainty attached
   to either.

**Conclusions stated more confidently than the evidence supports.**

- `ba230bca` verdict 1 ("fuzzy inference is not doing nothing beyond a retune") —
  refuted; the effect it rests on is an artifact.
- `ed854ac5`'s reachability column — a binary verdict resting on a 0.4% margin
  against an unmeasured constant.
- `c9ce6b7c`'s PARTIAL — built on differences that are 1 to 2 sd at best and on
  subcounts well below resolution.
- `fbdc5bd0`'s "9/9 rule cells reached" — true of the harness, but it is a
  property of the injection script, and it should never be cited as evidence about
  what a firing does.

**The 0.5 degC standing rule** (`project_ignore_sub_half_degree_effects.md`)
disposes of essentially the whole fuzzy thread. The corrected numbers are: fuzzy
ON vs OFF at strength 50 is 0.258 degC MAE; fuzzy vs a flat retune is 0.011 degC;
the best arm found (including the accidentally-sabotaged one) beats the shipped
configuration by 0.21 degC. Every one of those is under the line, and the two that
answer "is the fuzzy layer earning its complexity" are under it by more than an
order of magnitude. `ba230bca`'s verdict 2 already said this; with the corrected
numbers it is no longer a close call, and it is the only conclusion of the four
commits that should be carried forward to the owner.

## What was and was not changed

No production code, no test source, and none of the four commits' own documents
were modified. Findings requiring edits to `pid_fuzzy.c`, `fuzzy_nine_cell_probe.c`
or `docs/audits/fuzzy_vs_fixed_retune_comparison_2026-09-11.md` are recorded above
for their owning agents rather than applied, per this review's concurrency
constraints.
