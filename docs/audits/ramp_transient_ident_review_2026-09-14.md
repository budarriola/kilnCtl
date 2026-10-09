# Adversarial review — `83e04785`, ramp-transient identification

Reviewer: Claude Opus 5, 2026-09-14. Subject: `ramp_transient_ident.{h,c}`,
`test_ramp_transient_ident.c`, `docs/RAMP_TRANSIENT_IDENT_DESIGN.md`.

Method: the production `ramp_transient_ident.c` was compiled **unmodified**
into a standalone MSVC harness (per `project_binding_a_python_mirror_to_c`:
link the real `.c`, never a mirror) and driven with synthetic closed-loop
segments. Production sources were never edited; one root-cause experiment
used a `sed`-patched **copy** in the scratchpad. `git diff` on both
production files is empty.

## Verdict in one line

**The estimator is real — it genuinely measures the plant, and the
`ramp_ident.c` artifact is not reproduced.** But it carries a single
one-line modelling defect that makes it return a confident, badly wrong
`tau` on any segment that does not start at true ambient — which is most
segments in a real firing, and which the four gates do not catch. The
limitation the design doc documents as a "non-rested slow decay" bias is a
misdiagnosis of this same defect, and the defect is considerably worse than
the doc states.

---

## 1. Does the estimator carry plant content? YES — verified both directions

The agent's immunity-by-construction argument is correct, and unlike the
previous attempt it survives measurement. Both decisive experiments were
run (they were not in the commit).

**E1 — hold the command fixed, vary the TRUE plant.** Same 100 C/hr ramp,
same controller internal model (`tau_model=100`), `K=200`, 3000 s, 0.1 C
quantized:

| true tau | fitted tau | error |
|---|---|---|
| 80 | 78.6 | −1.7% |
| 150 | 146.4 | −2.4% |
| 280 | 278.1 | −0.7% |
| 450 | 448.8 | −0.3% |
| 700 | 698.8 | −0.2% |

The estimate tracks the plant across a ~9x range.

**E2 — hold the plant fixed, vary the commanded ramp rate.** `tau_true=280`
throughout, ramp swept 40 → 220 C/hr:

fitted tau = 279.0, 279.0, 278.1, 278.3, 279.6. **Flat.** A 5.5x change in
commanded ramp rate moves the estimate by 0.5%.

**E2b — vary the controller's internal model** (`tau_model` 50/100/200/400,
which changes the feedforward and hence the whole duty trace shape): fitted
tau = 279.8, 278.1, 278.3, 278.5. Also flat.

This is the exact test `ramp_ident.c` would have failed catastrophically
(its output was `0.524*K*delta_duty/ramp_rate`, so E2 would have produced a
5.5x spread and E1 a flat line). **The commit's central claim is sound and
now has the measurement behind it, not just the argument.**

## 2. The four gates — and a segment that passes all four with a meaningless answer

| gate | justified? | test coverage | reachable? |
|---|---|---|---|
| duty excitation (`RTI_MIN_DUTY_STD`) | yes, principled (constant input → same steady state ∀tau) | covered | yes |
| minimum duration | trivial floor, fine | covered | yes |
| **cost curvature (`RTI_FLAT_COST`)** | **unproven — see below** | **NONE** | **could not demonstrate** |
| improvement-over-null (`RTI_NO_IMPROVEMENT`) | yes | covered | yes, reproduced from real input |

### 2a. `RTI_FLAT_COST` appears to be a vacuous gate

The design doc calls this "the discriminator this gate actually rests on"
and claims simulation validation case 3 exercises it ("rejected at the
curvature gate"). **That is false**: the corresponding test asserts
`RTI_NO_IMPROVEMENT`, not `RTI_FLAT_COST`. `grep` over
`firmware/KilnFW/App/test/` finds **zero** references to `RTI_FLAT_COST`.

I then tried to trip it from outside: three hand-built no-information
segments, then a brute-force sweep of **1200 combinations** (tau_true
10→3000 s, segment length 20→2000 samples, duty amplitude 0.011→0.5,
`current_tau` 5→1200). **0 hits.** Observed curvature fractions were
typically 1600–3400 against a 0.05 floor — four to five orders of magnitude
clear.

I cannot prove it is unreachable, but on present evidence it is the repo's
documented vacuous-check pattern (`feedback_negative_test_every_check`,
three prior instances). The reason is structural: over a whole-segment
open-loop simulation of thousands of samples, a ±20% change in tau moves the
simulated trajectory by degrees, so the SSE always moves. A cost surface
flat enough to trip a 5% floor essentially cannot occur once the duty gate
has already passed. The gate the design rests its safety case on is, in
practice, always true.

`RTI_TREND_RESIDUAL_TOO_LARGE` also has no test, but I **did** trip it from
real input (a step inside the trend window → residual 2.971 C vs the 0.6 C
floor), so it is non-vacuous, merely untested.

### 2b. A segment that passes all four gates and yields a meaningless number

Asked for; constructed; reproduced across a sweep. See §3 — it is not an
exotic shape, it is an ordinary mid-firing ramp.

## 3. The documented limitation is misdiagnosed, and much worse than stated — **DEFECT**

The doc attributes a measured `tau=658 s` against a true `280 s` to a *slow
non-rested decay tail* that the trend-residual gate cannot see, and argues
no window size can catch it. **The decay is not the cause.**

`simulate_sse()` integrates `dT/dt = (K·duty − (T − t_free))/tau` with
`t_free = trend_intercept`, i.e. **the segment's own starting temperature**.
The real plant relaxes toward **ambient**. Writing `T = T_start + x`, the
true dynamics are `dx/dt = (K·(u − u_hold) − x)/tau` where
`u_hold = (T_start − T_ambient)/K` is the standing duty needed just to hold
`T_start`. The module feeds the **full** duty `u`, not the excess
`u − u_hold`, so it sees a constant phantom heating term of `K·u_hold/tau`.
The only free parameter that can absorb it is `tau`, which inflates.

**E4 — truly rested, fully settled, zero-curvature segments** (no decay tail
whatsoever, trend residual ~0), `tau_true = 280`, `K = 200`, ambient 25 C:

| segment start | fitted tau | result |
|---|---|---|
| 25.0 C (= ambient) | 278.1 | OK, correct |
| 27.5 C | 367.0 | OK, +31% |
| 30.0 C | 455.9 | OK, +63% |
| **35.2 C** | **640.0** | OK, **+129%** |
| 45.0 C | 994.7 | OK, +255% |
| 60.0 C | **1200.0** (search ceiling, saturated) | OK |
| 90.0 C | **1200.0** (saturated) | OK |

The 35.2 C row reproduces the doc's "658 s" almost exactly — and the doc's
pinned "mid-decay" test case starts at **35.2 C** above a 25 C ambient. The
decay tail contributes essentially nothing; the 10.2 C offset contributes
everything.

**Confirmation experiment.** A `sed`-patched *copy* of the module, changed
only to set `t_free` to the true ambient instead of `trend_intercept`:

| segment start | as shipped | `t_free` = true ambient |
|---|---|---|
| 25 C | 278.1 | 279.8 |
| 30 C | 455.9 | **279.8** |
| 35.2 C | 640.0 | **279.8** |
| 45 C | 994.7 | **279.8** |
| 60 C | 1200.0 | **279.8** |
| 90 C | 1200.0 | **280.0** |
| 150 C | 1200.0 | **280.0** |

One line, and the error is gone at every start temperature. The diagnosis is
not in doubt.

**Severity.** Every gate passes. Curvature reads 2300–3400 (floor 0.05).
Null-SSE ratio reads 0.000–0.39 (ceiling 0.70) — so the
improvement-over-null gate actively *endorses* the wrong answer, because the
null model is evaluated with the same broken baseline. The module returns
`RTI_OK`, `valid=true`, and an empty `refusal_reason`. **This is precisely
the "confident wrong tau" the feature exists to refuse.**

**Practical reach.** Only segment 1 of a firing starts at ambient. Every
subsequent ramp starts hot. On a real kiln (extrapolation — the bench is a
~4 W fixture capped near ambient+40 C) a segment starting at 600 C with
ambient 25 C is a 575 C offset; the fit saturates at the 1200 s ceiling with
no complaint. Downstream, a 2–4x tau error fed to SIMC gains
(`adaptive_tune`'s gain writer) inflates the derived integral time and
collapses the proportional gain by roughly the same factor — a badly
detuned, sluggish loop, applied with full confidence and no refusal message.

**Cheap partial mitigation that is missing:** the fit is accepted while
sitting exactly on `RTI_TAU_MAX_S`. Any optimiser landing on a search bound
should be refused on principle. That would catch the ≥60 C rows above, but
*not* the 27.5–45 C rows, which stay in range and are still 31–255% wrong —
so a bound-guard alone would make the module look safer without being safer.

**Recommended fix (not applied — see §7):** give `rti_fit()` an explicit
`ambient_c` parameter and use it for `t_free`; keep `trend_intercept` only
for the rested-baseline gate. Then re-run E1/E2 to confirm nothing regressed
and delete the misdiagnosed "non-rested slow decay" section of the design
doc, replacing the pinned regression test with a correctness assertion.

## 4. The mid-implementation slope-extrapolation fix — correct, one doc residue

The fix is right. `simulate_sse()` takes a scalar `t_free`; the slope is
never fed into the dynamics anywhere; the +43 C blow-up cannot recur. The
long comment at lines 114–129 explaining why is good practice.

One residue: `trend_slope` (line 217) is now **dead** — computed, passed out
of `fit_trend()`, and never read. The design doc states "the slope is still
computed and still drives the rested-baseline GATE below." It does not; the
gate uses `trend_residual`. Cosmetic, but it is a stale claim in a doc whose
other stale claim (§2a) hid a vacuous gate.

Also stale: the doc names `RTI_MIN_IMPROVEMENT_FRAC`, which does not exist
(the constant is `RTI_MAX_SSE_RATIO_VS_NULL`).

## 5. Negative tests

The agent's single sabotage/catch/hand-restore/rebuild cycle on the
`RTI_NO_IMPROVEMENT` gate reached a real gate — I confirmed independently
and more strongly by tripping `RTI_NO_IMPROVEMENT` from **real input** on
the unmodified production function (a small-sinusoid-duty segment: "best-fit
SSE is 90.6% of the current-model SSE"). An input-driven trip is better
evidence than a sabotage cycle: it proves the gate fires on data, not merely
that a mutated constant changes an outcome.

The other gates: duty-excitation and duration are covered by tests;
`RTI_TREND_RESIDUAL_TOO_LARGE` is uncovered but I demonstrated it reachable;
`RTI_FLAT_COST` is uncovered and I could not demonstrate it reachable at all
(§2a). **`RTI_FLAT_COST` needs either a test that fires it or an honest
admission in the doc that it is belt-and-braces, not "the discriminator this
gate rests on."**

## 6. Zero callers

Confirmed. `grep` for `rti_fit` / `ramp_transient_ident` outside the module
and its own test finds only build wiring (`drivers/CMakeLists.txt:448`,
`build_host_tests.ps1:98,173`, `test_main.c:81,172`). Nothing in
`adaptive_tune.c` or `profile_executor.c`.

**Is this the consumer-without-producer risk?** Not in the dangerous
direction. That class (`project_consumer_without_producer_class`) is a
*reader with no writer*, where a host test hand-supplies a value and passes
while the live path reads garbage. This is the inverse: a self-contained
pure function with a fully-specified input contract and no live path at all.
It is dead code, not a silent lie. Parking it mirrors `ramp_ident.c` and is
defensible.

**But** the zero-caller state is the only reason the §3 defect is not
currently a live hazard, and that is luck rather than design — the defect is
invisible from the test suite, so wiring it up later without re-reading this
audit would ship it. Wiring would need: (a) the ambient fix from §3, without
which every segment past the first is wrong; (b) the minimum-quiet-period
check the doc already calls for; (c) a harvester at a safe run boundary that
does not touch `adaptive_tune_run_end()` while
`project_profile_executor_panic_at_stop` is open; (d) a bound-hit refusal;
(e) a sanity clamp before any gain derivation. Until (a) lands, **this module
must not be wired to anything.**

## 7. Fixed vs reported

**Reported, not fixed.** Rationale: the correct fix for §3 changes
`rti_fit()`'s signature (an `ambient_c` parameter), invalidates the design
doc's non-rested section and its pinned regression test, and wants a
re-validation pass — that is the author's change to make coherently, not a
reviewer's patch. A partial fix (bound-hit refusal only) was deliberately
**not** applied because it would suppress the loud saturated cases while
leaving the quiet 31–255% cases, which is worse than the current honest
breakage. No production file was modified by this review.

## 8. The two check failures — adjudicated

The agent reported `92 passed, 2 failed` on
`check_executor_task_stack_budget.ps1` and
`check_httpd_task_stack_budget.ps1`; earlier passes reported 94/94.

**Measured at current HEAD:** both **PASS**.

```
check_executor_task_stack_budget: OK (LOW honest headroom -- worth a look, not yet failing)
  deepest 1936 B of 4096 B; honest free 940 B (22.9%) -- classified LOW
check_httpd_task_stack_budget: OK (honest headroom is LOW -- worth a look, not yet failing)
  deepest cfgfs_status_get_handler = 4304 B of 8192 B; honest free 2088 B (25.5%) -- LOW
```

Both sit in the **LOW** band, i.e. one classification step from failing.
They read `firmware/KilnFW/build/KilnCtrl.elf` (mtime 2026-09-14 22:58, three
minutes after this commit at 22:55), so their result tracks whatever the
shared build directory holds at the moment they run — and this tree is shared
across concurrent sessions.

**Conclusion:** the failures were real but **not caused by `83e04785`**. The
zero-callers argument is sound as far as it goes, and it is now backed by a
measurement rather than only an argument. The likeliest cause is the
immediately preceding commit `8ac578a7` ("Fix adaptive_tune duty-stability
window"), whose deepened `adaptive_tune` path is exactly the kind of change
that tips a LOW-band budget over, combined with the ELF being rebuilt
between the two runs. I could not reproduce the failure and therefore cannot
prove attribution — stated as a limit of this review, not a finding.

**`adaptive_tune_run_end` relation to the `profiles_stop` panic:** the symbol
does **not** appear in the current executor budget's deepest path at all
(`grep` over the full check output: no match). On this evidence the two are
unrelated — the budget check's flap is a build-artifact/classification-band
issue, while the panic remains an adjacent-stack-corruption suspect
(`project_profile_executor_panic_at_stop`). No connection found.

**Caveat carried forward:** these budgets read INDETERMINATE for tasks with
unresolved `callx4/8/12` dispatch, so every total above is a **lower bound**.
A LOW classification on a lower bound is weaker reassurance than it looks.

## 9. Smaller observations

- `RTI_MAX_DEAD_TIME_SAMPLES` is 8. At 1 s sampling that caps dead time at
  8 s; at 3 s sampling, 24 s. A real kiln's transport delay is minutes
  (extrapolation). Every hot-start case in §3 pinned `L` at the cap (8), so
  dead time is effectively unidentifiable here and the reported
  `dead_time_s` should be treated as a floor, not an estimate.
- `simulate_sse()` clamps the delayed duty index to 0, so the first
  `dead_samples` steps replay `duty[0]` rather than the true pre-segment
  duty. Negligible against §3, but it is another place where "the segment is
  all we can see" silently substitutes a wrong value.
- Tests are correctly 0.1 C quantized per
  `project_idealized_test_input_bug_class` — good, and they are the reason
  the harness above could be trusted to be comparable.
- Every test in the suite uses `T_AMB = 25` **and** starts the segment at
  25 C. The one degree of freedom that breaks the module is the one the
  whole test suite holds constant. That is the structural reason this
  shipped: not a weak assertion anywhere, a missing axis.

## What I could not verify

- Whether `RTI_FLAT_COST` is reachable by *any* input (1200 constructed
  combinations found none; absence of proof, not proof of absence).
- Attribution of the two stack-budget failures to a specific commit — they
  do not reproduce at current HEAD.
- Anything on hardware. No board was touched, no `debug_*` call made, no
  heating run started, per the review's ground rules. All numbers here are
  from a synthetic FOPDT plant; kiln-temperature statements are explicitly
  extrapolation from a ~4 W bench fixture
  (`project_bench_is_a_4w_test_fixture`).
