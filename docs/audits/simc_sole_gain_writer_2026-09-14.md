# SIMC as the sole gain writer: fuzzy + adaptive_tune run concurrently, 2026-09-14

Owner decision 2026-09-14: adopt options A + B of
`docs/audits/concurrent_fuzzy_pid_adaptation_2026-09-14.md` (commit
`652b5737`). That document's fixed-point argument (section 1.1) and its
qualifications (sections 1.3/1.4) are the basis for everything below; read
it first. This pass implements the decision in production code and tests --
it does not re-derive the argument.

## What changed

**Option A -- SIMC is now the sole gain writer.** `adaptive_tune_refine_ki_
locked()` (`firmware/KilnFW/App/drivers/control/adaptive_tune_ki.c`) no
longer writes `zones_config` at all. It still runs `adaptive_tune_diagnose_
ki()` exactly as before (unchanged pure classifier, still host-tested
directly in `test_adaptive_tune_ki_verdict.c`) and still publishes
`ki_verdict`/`ki_correction_pct`/`ki_refusal_reason`, but every verdict that
used to trigger a write (`LIMIT_CYCLE`, `OSCILLATING`, `OFFSET_TOO_SMALL`)
now only reports the diagnosis ("diagnostic only (v%u %.0f%%) -- SIMC is the
sole gain writer") and returns. `ki_applied` is now always `false`.

Removed along with the write path, as dead machinery rather than left as
inert conditionals:
- The effective-vs-reference guard (`e78fbc5b`, hardened by `83627343`/
  `ac5c26a3`) that used to withhold the correction specifically while
  `ZONE_CONTROL_MODE_PID_FUZZY` was active at non-zero strength.
- The dwell-entry snapshot fields it depended on
  (`trace_fuzzy_snapshot_valid`/`trace_fuzzy_accessor_failed`/
  `trace_fuzzy_active`/`trace_fuzzy_pct` on `adaptive_tune_zone_t`,
  `adaptive_tune_internal.h`) and the snapshot-taking code in `adaptive_
  tune_zone_tick()`'s `dwell_just_entered` branch (`adaptive_tune.c`).
- The cumulative bound/floor checks and the per-run-move cap inside
  `adaptive_tune_refine_ki_locked()` (`ADAPTIVE_TUNE_KI_CUMULATIVE_MAX_MULT`
  is kept defined, since it still describes a real "5x an autotuned
  baseline is implausible" plausibility bound independent of which layer
  would enforce it, and the audit's own section 4.1 recommends a Ki
  corrector might be rebuilt on `ku_estimate`/`tu_estimate_s` later -- but
  no production code reads it any more).
- `adaptive_tune_capture_revert_locked()`'s call site inside this function
  (nothing is written here any more, so there is nothing to snapshot for a
  revert).

**This SUPERSEDES `e78fbc5b`/`83627343`/`ac5c26a3`, not a revert-by-
accident.** Those commits fixed real defects (a ratchet, a guard-timing gap,
a fail-open) in a mechanism that protected a write path. The write path
itself is what is gone now, so the guard has nothing left to protect.

**Option B -- freeze fuzzy during the harvest window.** `pid_fuzzy_prepare_
gains()` (`firmware/KilnFW/App/drivers/control/profile_executor_pid_tick.c`)
takes a new `bool harvest_freeze` parameter. When `true` it forces
`strength_pct` to `0` for that tick -- the same bit-for-bit-base-gains path
`strength_pct == 0` and the no-identified-model case already use. The call
site (`profile_executor.c`'s `ZONE_CONTROL_MODE_PID_FUZZY` branch) computes
`harvest_freeze = s_exec.dwelling && adaptive_tune_get_enabled(zi)` --
`s_exec.dwelling` is the same single, module-wide flag `adaptive_tune.c`'s
own `dwell_just_entered` detection already relies on as a proxy for "a new
dwell has begun" (see that file's comment on `adaptive_tune_joint_dwell_
row_committed`). Fuzzy stays fully active on every ramp and approach, and on
any zone not opted into adaptive tuning -- this freezes the harvest window
only, never the whole firing, per the audit's own requirement.

Why: frame-independence is *pointwise*, not *sample-wise* (audit section
1.3) -- fuzzy cannot move a harvested `(duty, rise)` point, but it can
change *which* dwells pass the settle gates (a temperature-slope floor and a
duty-stability gate, both closed-loop-shape tests). Freezing fuzzy for the
whole dwell that might be harvested closes that selection-bias path
structurally, at near-zero cost: at a settled dwell fuzzy already sits near
its centre rule cell with a multiplier close to 1 (measured,
`docs/audits/fuzzy_nine_cell_offline_probe_2026-09-11.md`).

`fuzzy_gain_mirror_drift_check.py` (the statement-for-statement mirror check
against `test_closed_loop.c`'s hand-written `fuzzy_tick()`) was updated to
fold the new `if (harvest_freeze) strength_pct = 0` statement into
`PROD_ONLY_STMT_RES` -- the mirror has no `adaptive_tune`/dwell concept at
all, the same "structurally required difference" the no-model case already
gets, so it is dropped rather than given a fake mirror equivalent.

## What is unchanged

- **`autotune_baseline_k_dc` envelope** (`97288659`/`36f88d62`): untouched.
  `adaptive_tune_refine_ki_locked()` never read or wrote it, and this pass
  did not touch `adaptive_tune_model.c`'s plausibility-ratio check at all.
- **`strength_pct == 0` bit-for-bit contract** (`pid_fuzzy.c`, `233ded79`):
  untouched. `pid_fuzzy_adjust()` itself was not edited; `harvest_freeze`
  reuses the existing short-circuit rather than adding a new code path
  inside it.
- **No-model-runs-plain-PID behaviour**: untouched, same reasoning.
- **The errors-in-variables bias** the concurrent-adaptation audit flagged
  (section 1.4: at a settled dwell, rise is pinned near setpoint while duty
  absorbs the noise, and the fit puts the noisy variable in the
  denominator, biasing `K_dc` low). Not fixed here, not worsened here --
  this pass touches which trace gets fed to the harvest and who writes
  gains, not the fit itself.

## The anti-ratchet proof

`test_ki_diagnosis_never_ratchets_with_fuzzy_and_adaptive_tune_concurrent()`
(`firmware/KilnFW/App/test/test_adaptive_tune_ki_bounds.c`) is the
deliverable this task asked for: a zone configured `ZONE_CONTROL_MODE_
PID_FUZZY` at `fuzzy_strength_pct = 50`, with `adaptive_tune` enabled, run
through 10 sequential simulated `adaptive_tune_run_end()` calls, fed the
*exact* `dwell_err_mean_c = 0.45f` / `dwell_err_max_c = 0.50f`
constant-offset trace shape that the negative test below (and this file's
git history) shows genuinely walks a plain-PID zone's reference Ki to
**4.2998x baseline over 10 runs** under the old write path. It asserts:

- `ki_applied` is `false` every single run.
- Once SIMC has applied once, Ki never moves more than 5% in any later
  single run (the removed write path moved it a fixed, compounding 20%
  every run under this exact trace -- SIMC's own asymptotic convergence,
  per `adaptive_tune_model.c`'s documented permanent residual under
  `ADAPTIVE_TUNE_MIN_MATERIAL_MOVE_FRAC`, is bounded and far smaller).
- SIMC refinement genuinely occurs at least once across the 10 runs --
  proving concurrent operation still adapts, not merely that it fails to
  ratchet because nothing adapts at all.

`fuzzy_strength_pct`/`control_mode` are set on the fixture for documentation
of the concurrent-operation claim, not because `adaptive_tune_ki.c` reads
them any more -- it doesn't, which is the entire point: there is nothing
left in this layer for fuzzy's state to interlock with.

`test_ki_diagnosis_never_applies_any_verdict()` (same file) is the narrower,
single-module companion: it drives the same constant-offset `OFFSET_TOO_
SMALL` trace and a `LIMIT_CYCLE`-shaped oscillating trace, without any
fuzzy configuration at all, and proves the layer never writes for *either*
verdict shape, bit-for-bit, over many runs.

## Negative test (by hand, per this task's requirement)

Production code in `adaptive_tune_ki.c` was temporarily modified to
reintroduce the removed write (the exact pre-fix `zones_config_get_pid()` /
per-run-cap / `zones_config_set_pid()` sequence, re-typed from this file's
own git history, guarded with a `NEG_TEST_K9` comment). Full clean rebuild
(`firmware/KilnFW/App/test/build/at` and `.../pe` object directories
deleted, then `build_host_tests.ps1` from clean). Result: both new tests
failed loudly, exactly as expected --
`test_ki_diagnosis_never_applies_any_verdict()` showed the reference Ki
walking `1.0 -> 1.2 -> 1.44 -> ... -> 4.2998` across the 10 runs (matching
the closed-form `1.2^n` prediction from `docs/audits/adaptive_tune_ki_
effective_reference_loop_2026-09-13.md` section 3 exactly), and the
LIMIT_CYCLE half showed the symmetric decay `100 -> 80 -> 64 -> ... ->
10.7374`. The anti-ratchet test failed on its `ki_applied` assertion on
every run. The break was then reverted BY HAND (the `NEG_TEST_K9` block
removed, restoring the plain diagnostic-only `return` path; diffed against
the pre-break source to confirm no `NEG_TEST_K9`/stray identifiers were
left behind), the same two build directories deleted again, and a full
clean rebuild confirmed all 39/39 host-test executables built and ran clean
before anything else was measured or committed.

## Other test changes

- `test_adaptive_tune_ki_verdict.c`'s `test_ki_diagnosis_skipped_same_run_
  as_model_refine()` used to call `adaptive_tune_refine_ki_locked()`
  directly (bypassing the D5 skip gate) and assert `ki_applied` becomes
  `true` on a genuinely-triggering trace. Updated to assert `!ki_applied`
  (K9's new invariant) plus the verdict/refusal-reason evidence that the
  fixture still genuinely classifies `OFFSET_TOO_SMALL` and reports
  "diagnostic only" -- the fixture-capability proof this test exists for
  (distinguishing a genuinely-capable fixture from the D5 skip gate simply
  never firing) survives unchanged in spirit.
- `test_adaptive_tune_status.c`'s three `H2`/`K1`/`K2`/`K3` "run status
  fields cleared on skip" tests used `ki_applied == true` as their run-2
  setup assertion (proving the Ki layer genuinely touched status before the
  reset-on-skip behaviour under test is checked on run 3). Updated to assert
  `!ki_applied` (K9's invariant) plus `ki_verdict == OFFSET_TOO_SMALL` and a
  nonzero `ki_correction_pct` -- the "genuine, non-default diagnosis this
  run" evidence those tests actually need, now that "applied" no longer
  exists as a thing to prove.
- `test_adaptive_tune_ki_bounds.c`'s reboot-survival (`P1`) and
  clear-and-relatch (`Q3`) tests, which used to latch `ki_baseline` via the
  removed Ki-diagnosis write path, are rewritten (not removed) to latch via
  a genuine SIMC refit instead -- `ki_baseline` is still real, persisted
  state, still written and re-latched by `adaptive_tune_model.c`
  independent of this pass.
- `test_profile_executor_prestart.c` gained
  `test_fuzzy_prepare_gains_harvest_freeze_forces_plain_pid_bit_exact()`:
  proves `harvest_freeze=true` reproduces base gains bit-exactly even with
  an identified model, `strength_pct=100`, and a large error/rate that DOES
  move the gains hard when not frozen (the companion, unfrozen case is the
  pre-existing test immediately before it in the file).

## Verification performed

- KilnFW host tests (`firmware/KilnFW/App/test/build_host_tests.ps1`,
  clean rebuild after the negative test's revert): **39/39 executables
  built and ran clean.**
- KilnFW target build (`idf.py -C firmware/KilnFW build`, via
  `Microsoft.v6.0.2.PowerShell_profile.ps1`): clean link,
  `KilnCtrl.bin` 0x228170 bytes, 28% partition headroom.
- `git cat-file -t` confirmed every commit hash cited in this document
  (`e78fbc5b`, `83627343`, `ac5c26a3`, `97288659`, `36f88d62`, `233ded79`,
  `652b5737`) is a real commit before citing it.
- `tools/run_all_checks.ps1`: see the report accompanying this commit for
  the exact tally; recent runs on this tree read 92/94 with two failures
  owned by the concurrent kilnlink stack-margin work
  (`check_00_kilnfw_target_build.ps1`/`check_wire_protocol_fingerprint.ps1`,
  both traced in `docs/audits/adaptive_tune_ki_guard_timing_and_failopen_
  2026-09-14.md` to an untracked file another session's in-flight work
  depends on) -- not attributed to this pass, not fixed here.

## Files changed

- `firmware/KilnFW/App/drivers/control/adaptive_tune_ki.c` -- write path
  removed from `adaptive_tune_refine_ki_locked()`; now diagnostic-only.
- `firmware/KilnFW/App/drivers/control/adaptive_tune.c` -- dwell-entry
  fuzzy-state snapshot removed from `adaptive_tune_zone_tick()`.
- `firmware/KilnFW/App/drivers/control/adaptive_tune_internal.h` --
  `trace_fuzzy_*` fields removed; `ADAPTIVE_TUNE_KI_CUMULATIVE_MAX_MULT`'s
  comment updated to note it is no longer read by production code.
- `firmware/KilnFW/App/drivers/control/profile_executor_pid_tick.c` --
  `pid_fuzzy_prepare_gains()` gained the `harvest_freeze` parameter.
- `firmware/KilnFW/App/drivers/control/profile_executor_internal.h` --
  matching declaration/doc comment.
- `firmware/KilnFW/App/drivers/control/profile_executor.c` -- call site
  computes and passes `harvest_freeze`.
- `firmware/KilnFW/App/test/fuzzy_gain_mirror_drift_check.py` -- folds the
  new `harvest_freeze` statement.
- `firmware/KilnFW/App/test/test_adaptive_tune_ki_bounds.c` -- obsolete
  write-authority tests removed; reboot/clear tests rewritten onto the SIMC
  path; new diagnostic-only and anti-ratchet tests added.
- `firmware/KilnFW/App/test/test_adaptive_tune_ki_verdict.c`,
  `test_adaptive_tune_status.c` -- setup assertions updated for the new
  `ki_applied` invariant.
- `firmware/KilnFW/App/test/test_adaptive_tune.c` -- test wiring updated to
  match.
- `firmware/KilnFW/App/test/test_profile_executor_prestart.c` -- all
  `pid_fuzzy_prepare_gains()` call sites updated for the new parameter; one
  new test added.
- `firmware/KilnFW/App/test/test_adaptive_tune_model.c` -- the now-orphaned
  `feed_oscillating_trace()` P6/Q2 header comment context is superseded by
  this document; the helper itself is still used by `test_adaptive_tune_ki_
  bounds.c`'s new diagnostic-only LIMIT_CYCLE proof, so it was kept.
