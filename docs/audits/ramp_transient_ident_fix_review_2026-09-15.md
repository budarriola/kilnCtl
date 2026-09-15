# Adversarial review of `a6c040a9` — the `ramp_transient_ident` ambient fix

Date: 2026-09-15
Subject commit: `a6c040a9` "Fix ramp_transient_ident ambient-reference defect; make RTI_FLAT_COST reachable"
Prior review being adjudicated: `docs/audits/ramp_transient_ident_review_2026-09-14.md` (`59fce1ce`)
Module under review: `firmware/KilnFW/App/drivers/control/ramp_transient_ident.c` / `.h`

## Verdict

**The fix is correct and complete for the defect it names.** `simulate_sse()`'s
relaxation target is now the caller-supplied `ambient_c` rather than the
segment's own start temperature, and with that one line changed the fitted
`tau` is right across a much wider range of segment start temperatures than the
original review ever tested. The structural cause — every test holding
`T_AMB = 25` AND starting the segment at 25 C, so the two references were
indistinguishable — is genuinely closed by the new `build_closed_loop_ramp_from()`
helper, verified by independent sabotage rather than by trusting the commit's
report.

**Two real, previously unreported defects were found and fixed** during the
review (a non-finite `ambient_c` mis-refusing as `RTI_NO_IMPROVEMENT` and
blaming the segment; a saturated search returning `RTI_OK` with an empty
refusal reason). **One further defect was found and deliberately reported
rather than fixed** (an overstated caller gain passes every gate and returns a
badly wrong tau; the gate that would close it needs a threshold calibrated
against real telemetry noise, not a guess). **One new false claim in the
corrected design doc was found and fixed** (the replacement ambient table's
offsets were shifted one row).

No defect in this report is manufactured; where the fix is sound, it is said
plainly.

## Method

The production `.c` was linked directly into standalone MSVC harnesses rather
than mirrored in Python — see `project_binding_a_python_mirror_to_c`; a mirror
would have proved nothing about the shipped code. All synthetic traces are
quantized to 0.1 C to match telemetry (`project_idealized_test_input_bug_class`);
the resulting RMS quantization floor is about 0.029 C, which is the reference
against which every residual number below is read.

For negative tests, **sabotaged copies** of `ramp_transient_ident.c` were built
in the scratchpad and linked against the real, unmodified
`test_ramp_transient_ident.c`. No production file was ever broken in the shared
tree, so no hand-restore was required and no `git checkout`/`restore`/`stash`
was run — other sessions have live uncommitted work here
(`profile_executor_pid_tick.c`, `sim_factorial_driver.c`, `sim_scenarios*.c`,
`kiln_cfg_store.c`). Every measurement below was taken from a freshly built
binary, never a prebuilt one (`project_negative_test_leaves_poisoned_binary`).

## Item 1 — Is the fix correct, or merely compensating? **CORRECT.**

Re-running the prior review's own ambient-offset table against the fixed code,
and extending it past the tested range and into negative offsets:

| segment start vs ambient | fitted tau (defect) | fitted tau (fixed) |
|---|---|---|
| −20 C | — | 279.9 |
| −10 C | — | 279.0 |
| −5 C | — | 276.8 |
| −2 C | — | 279.8 |
| −1 C | — | 279.8 |
| 0 C | 278.1 | 279.8 |
| +2.5 C | 367.0 | 279.8 |
| +5 C | 455.9 | 279.8 |
| +10.2 C | 640.0 | 279.8 |
| +20 C | 994.7 | 279.8 |
| +35 C | 1200.0 (saturated) | 279.8 |
| +65 C | 1200.0 (saturated) | 280.0 |
| +100 C | — | 279.8 |

True `tau` is 280 s. The fitted value is flat at 279.8 s from −20 C to +100 C —
not merely "better near where it was tested". The model does admit segments
starting below ambient and fits them correctly.

Past roughly +125 C the synthetic `K = 200` plant's own duty saturates and the
segment is refused `RTI_INSUFFICIENT_DUTY_EXCITATION`. That is a property of
the constructed plant, not of the estimator, and it is the correct refusal.

The module's honestly-documented "slow decay" limitation was indeed a
misdiagnosis of this same bug: the mid-decay regression case (start 35.177 C)
now fits 279.0 s, an error of −0.4 %, against the doc's claimed "within roughly
25 %".

## Item 2 — Is the structural cause closed? **YES, verified independently.**

`build_closed_loop_ramp_from()` takes `start_c` and `t_amb` as separate
arguments and the new tests pass genuinely different values, so the two
references are no longer aliased.

Sabotage s1 restored `float t_free = trend_intercept;` in a scratchpad copy and
rebuilt against the real test file. It reproduced **455.9 / 640.0 / 994.7 /
1200 / 1200** exactly — the commit's reported numbers are accurate — and failed
6 checks (the 5 ambient-sweep rows plus the mid-decay regression). The new
tests are not decorative: they fail loudly on a reintroduced relaxation
defect.

## Item 3 — Does `ambient_c` have a real producer? **YES — but the contract is the hazard.**

This is **not** an instance of `project_consumer_without_producer_class`. The
producer exists: `profile_executor`'s `s_exec.ambient_c`
(`profile_executor_internal.h:714-715`), a MAX31856 cold-junction reading
captured at run start, alongside `s_exec.ambient_from_cj` which is false when
`FALLBACK_AMBIENT_C` was substituted. `autotune_engine.c:660` does the
analogous `AUTOTUNE_FALLBACK_AMBIENT_C` substitution. The module has zero
callers today, so nothing is broken in the field; the question is what a
harvester must do when it is wired up.

What a wrong value does (true tau 280 s, all rows `RTI_OK` with an empty
refusal reason and every gate passing):

| `ambient_c` error | fitted tau | error | curvature (floor 0.05) | null ratio (ceiling 0.70) |
|---|---|---|---|---|
| −2.5 C | 190.1 | −32 % | 489 | 0.00 |
| 0 | 279.8 | −0.1 % | 2385 | 0.00 |
| +2.5 C | 368.8 | +32 % | 2793 | 0.00 |
| +5 C | 456.6 | +63 % | 2730 | 0.00 |
| +10 C | 635.5 | +127 % | 2853 | 0.00 |
| +15 C | 811.4 | +190 % | 3126 | 0.00 |

Notice the curvature gate reads *higher* on the wrong rows than on the right
one. It is not merely blind here; it is anti-correlated.

The tau error is a function of the ambient error **alone**, independent of
where the segment starts — three different segment starts gave identical rows.
Roughly 12 % of tau per degC.

Specific wrong inputs:
- **NaN / ±Inf** — previously fell out of the bottom as `RTI_NO_IMPROVEMENT`
  with a reason blaming the SEGMENT ("tells us nothing the existing model
  doesn't already predict"), because every SSE was NaN and every
  "is this better" comparison against NaN is false, so the tau search never
  updated its running best. **Fixed**: new `RTI_INVALID_AMBIENT`, which names
  the caller. `k_gain_c_per_duty` was hardened to `isfinite` at the same time
  (it previously only checked `<= 0`, which NaN passes).
- **`ambient_c = 0`** — returns `RTI_FLAT_COST`. Refused, by luck rather than
  by design.
- **a zone temperature instead of ambient** (the old defect's own shape) —
  returns `RTI_OK` with tau 1200.
- **a stale value** — indistinguishable from a correct one, priced by the table
  above.

**Should there be a value guard? There cannot be one.** Whole-segment RMS
residual moves only 0.029 C → 0.060 C across the entire 0 → +15 C ambient
sweep, i.e. it stays within about 2x the 0.1 C quantization floor. `tau` and
ambient are near-degenerate — both enter only through the standing heat balance
`K*duty − (T − ambient)` — so the model still *explains the data* with a wrong
ambient. No residual threshold, curvature floor or null comparison can separate
the rows above without also rejecting good fits.

This is therefore recorded as an explicit **caller contract** in the header,
with the measured table, and with the operational consequence stated: a
harvester must refuse to run when `s_exec.ambient_from_cj` is false, because a
fallback constant is exactly the several-degC error this table prices.

## Item 4 — Is `RTI_FLAT_COST` genuinely non-vacuous? **YES, and more reachable than claimed — but one-sided.**

Sweeping the caller's `k_gain_c_per_duty` against a true `K` of 200,
`RTI_FLAT_COST` fires continuously from `k_gain` = 10 up to `k_gain` = 170 —
i.e. for any understatement of about 15 % or more, not only the "understated by
20x" contrivance the design doc describes. The condition is reachable from
plausible real inputs (a zone whose gain has drifted, or a stale tuned value),
and this is a real improvement over the pre-commit state.

Sabotage s2 set `RTI_MIN_CURVATURE_FRAC` to `0.0f` in a scratchpad copy; the
new test fails. The test is not vacuous.

The gate is **strictly one-sided**, which is a separate defect from the one
reported fixed — see item 5.

## Item 5 — Would the gates catch a *different* wrong answer? **Partly. A large hole remains, and it is reported, not fixed.**

Constructed inputs that return `RTI_OK`, pass all four gates with an empty
refusal reason, and yield a badly wrong tau:

| `k_gain_c_per_duty` error | fitted tau | error | result |
|---|---|---|---|
| −5 % | 186.9 | −33 % | `RTI_OK` |
| +5 % | 365.7 | +31 % | `RTI_OK` |
| +10 % | 451.4 | +61 % | `RTI_OK` |
| +20 % | 621.2 | +122 % | `RTI_OK` (curvature 0.99 vs floor 0.05; ratio 0.036 vs ceiling 0.70) |
| +50 % | 1110.6 | +297 % | `RTI_OK` |

The safe caller-gain band is roughly ±2 %. So the gates are not decorative —
they catch flat segments, dwells, non-rested baselines and understated gains —
but they do not catch an overstated gain at all.

**Unlike ambient, this one is detectable.** Absolute whole-segment RMS residual
goes 0.029 C → 0.86 C at +5 % gain error and 1.55 C at +10 %, a 30–50x
separation from the quantization floor. An **absolute-residual gate** — refuse
when `sqrt(best_sse/(n−1))` exceeds a few multiples of the telemetry noise
floor — closes it cleanly.

It is **deliberately not implemented**. Its threshold must be calibrated
against real telemetry noise on a real firing; a constant chosen to make a
synthetic test pass is a guard justified against a test constant, which is a
guard justified against nothing (`project_bound_relative_to_persisted_state`).
This is written into the header and the design doc as required work for
whoever wires the module up.

Second-order true plants with correct `K` and ambient degrade gracefully rather
than lying: a true second pole of 60 s fits 361 s, of 280 s fits 709 s, with
`L` pinned at the search cap of 8 samples in every case.

### New gate added: `RTI_TAU_AT_SEARCH_BOUND`

A fit that settles exactly on `RTI_TAU_MIN_S` or `RTI_TAU_MAX_S` previously
returned `RTI_OK`, `valid=true`, `tau_s=1200.0` and an **empty**
`refusal_reason`. That is not a measurement of tau; it is the bound, and it
carries no information about how far outside the searched interval the true
optimum lies.

The 2026-09-14 review recommended this gate and then deliberately withheld it,
correctly: adding it while the ambient defect stood would have silenced that
defect's loud saturated rows (+35, +65) while leaving its quiet 31–255 %-wrong
rows untouched, making the module look safer without being safer. With the
ambient defect fixed that objection is void, so the gate is now in.

It runs **last**, immediately before `out->result = RTI_OK;`. An earlier
placement (before the curvature gate) made the existing `RTI_FLAT_COST` test
return `RTI_TAU_AT_SEARCH_BOUND` instead — a badly understated gain also lands
its optimum on a bound, and `RTI_FLAT_COST` is the more specific and more
actionable diagnosis there. Ordering matters and is now deliberate.

## Item 6 — Negative tests. **All pass; every test claimed to cover something does.**

Each row is a sabotaged scratchpad copy of the production `.c`, built against
the real test file:

| # | sabotage | checks failed |
|---|---|---|
| s1 | `t_free = trend_intercept` (the original defect) | 6 |
| s2 | `RTI_MIN_CURVATURE_FRAC` → 0.0 | 1 |
| s3 | trend-residual gate disabled | 1 |
| s4 | `RTI_TAU_MAX_S` → 300 | 1 (E1) |
| s5 | drop the relaxation term entirely | 14 |
| s6 | duty-std floor → 0 | 5 |
| s7 | null ratio → 1.10 | 1 |
| s8 | `reset_out()` no longer memsets | 2 |
| s9 | remove the new search-bound gate | 3 |
| s10 | drop the new `isfinite(ambient_c)` check | 3 |
| s11 | revert the `isfinite` hardening of the gain check | 1 |
| s12 | `out->tau_s = best_tau * 2.0f` | 14 |

s12 is the interesting one. It failed 14 checks **including the absolute
anchor this review added to the E2 ramp-rate-invariance test**, while E2's
original mutual-flatness assertions stayed green — E2 compared the three
estimates only against each other, so a uniformly-wrong estimator satisfied it.
That was a vacuous check of exactly the shape this repo has shipped three times
before (`feedback_negative_test_every_check`), and it is now anchored:

```c
TEST_CHECK_NEAR(fit.tau_s, tau_true, tau_true * 0.15f,
                 "E2 ramp-rate sweep: each estimate is also individually near the true "
                 "plant tau, not merely equal to its siblings");
```

## Item 7 — Design-doc accuracy. **Corrected, and one NEW false claim was found.**

Verified true against the code as it now stands:
- `ambient_c` is caller-supplied and is the relaxation target — true.
- The gate constant is named `RTI_MAX_SSE_RATIO_VS_NULL` — the prior doc's
  wrong name is fixed, and the name now matches the header.
- `fit_trend()`'s `out_slope` is nullable, `rti_fit()` passes `NULL`, and the
  slope is read nowhere — so the prior doc's claim that `trend_slope` drives
  the gate is correctly retracted; the gate reads the residual.
- The trend-residual gate now genuinely has a test — proved by s3 failing.

**New defect found in the corrected doc**: the replacement ambient-offset table
introduced by `a6c040a9` has every offset shifted one row. The commit converted
the source review's *absolute start temperatures* into offsets incorrectly, so
each row understates the cost of that offset by roughly 2x (e.g. the row
labelled +5 C carries the number actually measured at +10.2 C). The same error
is in the commit message, where it cannot be corrected. The doc's table is
fixed in this pass and carries a note recording the mis-read.

Also added to the doc in this pass: a "Sensitivity to the two caller-supplied
constants" section carrying both tables above, the RMS-residual separation
(0.060 C vs 0.86 C) that explains why one is gateable and the other is not, the
proposed-but-unimplemented absolute-residual gate, and the two new gates.

## What was fixed vs. what was reported

**Fixed in this pass:**
- `RTI_INVALID_AMBIENT` — non-finite ambient now names the caller instead of
  blaming the segment.
- `isfinite()` hardening of the `k_gain_c_per_duty` check (NaN previously
  passed `<= 0`).
- `RTI_TAU_AT_SEARCH_BOUND` — a saturated search no longer returns `RTI_OK`
  with an empty refusal reason.
- The E2 ramp-rate test's vacuous mutual-flatness-only assertion.
- The design doc's shifted ambient table, plus the `RTI_FLAT_COST`
  reachability claim (understated by 20x → understated by ~15 % or more).

**Reported, deliberately not fixed:**
- The overstated-`k_gain_c_per_duty` hole. The closing gate is designed and
  documented; its threshold must come from real telemetry, not a guess.
- The `ambient_c` value contract. Not fixable inside the module at all
  (proved by the residual measurement); it is a hard requirement on the
  future harvester, recorded in the header and the doc.

## What could not be verified

- **No hardware measurement.** Every number here is from synthetic
  closed-loop traces with a known true plant. The real kiln is not a
  first-order plant, and the second-order rows above suggest the estimator
  will read high on a real segment even with a perfect `K` and ambient. The
  absolute-residual gate's threshold in particular cannot be chosen without a
  real firing.
- **No caller exists**, so the ambient contract is verified only against
  `profile_executor`'s field and flag as they read today. Whether a future
  harvester actually honours `ambient_from_cj` is unenforced by anything
  mechanical; this doc and the header comment are the only record.
- One `build_host_tests.ps1` run reported `RUN FAILURES (1): kiln_package`;
  the same executable passes rc=0 standalone from two working directories on
  re-run, and `kiln_cfg_store.c` (which it links) is another session's live
  uncommitted work. Treated as not-mine and not further chased.

## State after this pass

- KilnFW host suite: **7816/7816 checks passed** (the module's own block grew
  from 42 to 55 checks).
- KilnFW target build: green, `KilnCtrl.bin` 0x22c970 bytes, 28 % free.
- `tools/run_all_checks.ps1`: **94 passed, 0 skipped, 0 failed.** The
  pre-existing reds noted at the start of this pass
  (`check_00_kilnfw_target_build.ps1`, `check_fuzzy_gain_mirror_drift.ps1`,
  `check_doc_hash_citations.ps1`, all other sessions') have cleared.
