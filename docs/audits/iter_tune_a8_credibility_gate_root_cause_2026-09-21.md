# iter_tune A8 cross-profile miss: why the gate does not catch it, 2026-09-21

Read-only research pass. Citations are against `origin/main` at `33124aa8`;
`origin/main` had advanced to `a26fd82c` when this was written, and
`git diff 33124aa8 a26fd82c` is empty for every file cited here, so the line
numbers hold at both commits.

## Question

The A8 acceptance criterion of `docs/ITER_TUNE_REDESIGN_PLAN.md` sec 7
("profile independence") fails on its A1 half: re-running the A1 null
experiment with the baseline firing on one profile and the trial firing on a
second, class-matched profile measures 45 false accepts of 660 (6.82%)
against a 38.4-count ceiling, roughly double the same-profile rate. The
question posed was why the credibility gate misses this case, and whether
that is (a) a defect in the gate's logic, (b) a defect in the fixture or the
simulated plant that makes the case undetectable, or (c) a genuine
identification limit already documented and closed.

## What is actually measured, and where

The A8 A1-half experiment is Part 4 of the host harness,
`firmware/KilnFW/App/test/sim_iter_tune.c:1129-1195`. It holds the gains
identical on both sides (`g[i].kp = g_kp_bench[i]` and siblings,
`sim_iter_tune.c:1158`), randomises only `noise_seed` and `start_offset_c`
per side, and then runs side A on `g_profile` and side B on `g_profile_b`
(`sim_iter_tune.c:1161-1162`). The two profiles are defined at
`sim_iter_tune.c:113-143`; they share all six `(rate_bucket, temp_bucket)`
segment classes and differ only in per-segment dwell duration (600/1200/600 s
against 900/900/1200 s), which is exactly what the plan's "shares >= 5
segment classes" wording asks for. The bar itself is computed at
`sim_iter_tune.c:1190-1195`: A1's pinned rate (`A1_PINNED_MAX_ACCEPTS = 24`
of `A1_PINNED_TOTAL = 660`, `sim_iter_tune.c:1076-1077`) widened by three
binomial sampling standard deviations.

The verdict is then deliberately discarded. `sim_iter_tune.c:1297` reads
`(void)a8_pass;`, and the aggregate at `sim_iter_tune.c:1298` is
`a1_pass && a2_pass && a5_pass && a6_pass && a_step4_gate && a8a2_pass` —
every bar except the A8 A1-half. The wrapper check behaves the same way:
`firmware/KilnFW/App/test/check_sim_iter_tune_bars.ps1:2-6` says so in its
header and its PASS banner at lines 232-236 prints the failing 45/660 number
explicitly rather than hiding it. So "the gate misses the case" is, at the
mechanical level, not a miss at all: the case is measured every run, printed
every run, and excluded from the exit code by one visible line with a
thirteen-line justification above it (`sim_iter_tune.c:1273-1288`).

## Why the underlying comparator accepts these pairs

`firing_compare()` makes two dissimilar firings comparable by intersecting
them on class key and aggregating the paired differences by median
(`firmware/KilnFW/App/drivers/control/firing_compare.c:111-112`,
`:136`), with Bar 1 requiring `cnt[s] >= FIRING_COMPARE_BAR1_MIN_N` and a
median normalised improvement at or below `-1.0`. `BAR1_MIN_N` is 3
(`firing_compare.h:172`, with the reasoning at `:165-171`: a firing commonly
yields only three to six matched classes, so a larger minimum would make the
accept path structurally inert). A verdict therefore rests on the median of
as few as three draws.

The statistic that moves under that thin median is already root-caused.
`docs/audits/a1_false_accept_root_cause_2026-09-14.md` traces the
same-profile 3.64% rate to `FIRING_SUBSCORE_ENTRY_PEAK_C`, a raw unsmoothed
worst-sample maximum over the dwell-entry window
(`firmware/KilnFW/App/drivers/control/firing_score.c:206-215`), sampled from
a plant whose inter-zone coupling is an additive source-gain term driven by
the neighbour's *instantaneous binary relay state*. Whether a neighbour's
relay edge happens to land inside a given zone's entry window changes the
recorded peak by an amount unrelated to either side's tuning, and with
`n_pairs == 3` that variance is amplified rather than averaged away.

The cross-profile case adds a second, systematic source of exactly that
phase decorrelation. In the same-profile A1 experiment the two sides share
identical segment boundaries and differ only by a start-temperature offset,
so relay-edge phase at each dwell entry is perturbed but correlated. Changing
the dwell durations shifts every later segment's wall-clock boundary between
side A and side B, so each matched class is scored at an unrelated point in
the neighbours' relay windows. The doubling from 24/660 to 45/660 is the
expected consequence of feeding the same known-sensitive statistic a second
independent phase perturbation — not evidence of a new mechanism.

Two controls in the harness support that reading rather than a fixture
artifact. First, the sanity run with `g_profile_b` set identical to
`g_profile` reproduces 30/660 (4.55%), inside the ceiling, which shows the
ceiling construction and the RNG-offset argument at
`sim_iter_tune.c:1177-1189` are sound and that the excess is attributable to
the profile difference itself. Second, A8's A2 half — the never-worse
criterion, searched on `g_profile` and evaluated on `g_profile_b`, Part 5 at
`sim_iter_tune.c:1197-1250` — measures 0 worse of 660 and *is* gated into the
exit code. A fixture that could not represent cross-profile evaluation at all
could not produce that clean, negatively-tested pass on the same pair of
profiles.

## Verdict

**(c).** This is a genuine property of the plant-and-scoring design, already
root-caused, and closed by explicit owner decision — not a gate-logic defect
and not a fixture defect. The mechanism is the one named in
`docs/audits/a1_false_accept_root_cause_2026-09-14.md` (raw single-sample
entry peak, coupling by raw relay duty, median over three pairs), excited a
second time by the profile shape difference. The closure is recorded in
`docs/ITER_TUNE_REDESIGN_PLAN.md`'s status block ("the A1-half of A8's
cross-profile miss ... is accepted as a known gap, not made green. Do not
re-dispatch work to close it."), and `docs/audits/release_gate_vacuity_audit_2026-09-18.md:195-197`
independently re-examined the bar's exclusion from the exit code and left it
alone as settled.

One nuance worth stating rather than glossing: the memory item
`project_bench_identification_limits` lists four closed negatives (IAE floor,
z2 dead time, ramp fit, load estimator) and A8 is not one of them. A8 is a
fifth, separately closed item, closed in the plan doc rather than in that
audit. Anyone matching the memory summary against that list will not find it
there; that is a bookkeeping gap, not a reopening.

Three things this pass did **not** find, and says so explicitly: no defect in
the Bar 1 / veto arithmetic, no sign or normalisation error in the
cross-profile path, and no way for the exclusion at `sim_iter_tune.c:1297` to
hide the number (it is printed unconditionally by both the harness and the
wrapper script).

## Recommended next action

**Closed, no action** on the algorithm, the bar, or the exit-code wiring. The
only change worth considering is documentary: adding a one-line pointer to
this closure under the bench-identification-limits heading so the fifth
closed negative is findable alongside the other four. No threshold, harness
or comparator change is warranted, and per the plan's own instruction the
line must not be re-dispatched as work to make green.
