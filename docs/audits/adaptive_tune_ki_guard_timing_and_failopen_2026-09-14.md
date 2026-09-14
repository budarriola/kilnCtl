# adaptive_tune_ki.c: guard timing and fail-open, 2026-09-14

Two defects in the effective-vs-reference PID_FUZZY guard added by
`e78fbc5b` (see `docs/audits/adaptive_tune_ki_effective_reference_loop_2026-09-13.md`),
found by an opus review (`a3803057`, appended to that same doc) and
deliberately parked rather than bundled into the truncation-fix commit
(`83627343`, which fixed the `ki_refusal_reason` truncation bug and added
the length assertion this fix keeps satisfied). Both are confirmed real and
both are fixed here.

## Defect 1: the guard read its inputs at the wrong time

`adaptive_tune_refine_ki_locked()` (`adaptive_tune_ki.c`) used to read
`zones_config_get_control_mode()`/`get_fuzzy_strength_pct()` **live**, at
refine time -- i.e. inside `adaptive_tune_run_end()`, which runs after the
firing whose trace is being diagnosed has already ended and its interlocks
released. The trace it is protecting
(`z->trace_actual_c[]`/`z->trace_duty[]`) was captured **during** the dwell,
earlier. If fuzzy was active for the whole traced dwell and switched off
before refine ran, the live read saw plain PID and missed exactly the case
the guard exists for -- the reference-Ki ratchet
`test_ki_diagnosis_withholds_correction_when_zone_is_pid_fuzzy()` proves is
otherwise real came right back.

Confirmed real: the guard's only reason this was safe *today* is that the
two writers of `control_mode`/`fuzzy_strength_pct`
(`zones_post_handler`, `backup_import.c:1045/1082`) both sit behind
`ota_http_check_interlocks()`, which refuses while firing/hot/heater-
commanded -- true for the whole time a dwell is being traced -- and `POST
/api/zones/pid` (the one mid-firing config exception) has no key for
either field. Nothing in the code links those two facts to the guard's
correctness; this is this repo's documented "two pieces of state joined by
an unexpressed contract" shape (see `CLAUDE.md`'s reset-one-side-of-a-pair
section), a fifth instance.

### Fix: snapshot at dwell entry

Both fields are now read exactly once per dwell, at the same instant the
trace itself is reset for that dwell -- `adaptive_tune_zone_tick()`'s
`dwell_just_entered` branch (`adaptive_tune.c`) -- into three new
`adaptive_tune_zone_t` fields (`adaptive_tune_internal.h`):
`trace_fuzzy_snapshot_valid`, `trace_fuzzy_active`, `trace_fuzzy_pct`. This
is the point the review suggested (`dwell_just_entered`); it is the right
one because it is the earliest and only place this module has "a new trace
window is starting" as an explicit event, and it runs unconditionally
(before the `z->enabled` gate), so a disabled zone's snapshot is simply
never consulted rather than silently stale. `adaptive_tune_refine_ki_locked()`
now reads the snapshot instead of re-querying `zones_config`.

### Can a trace span both states (mid-firing toggle)?

Not today, per the interlock argument above: a trace's dwell runs entirely
under firing/hot/heater-commanded conditions, during which both fields are
immutable. So a single trace is always captured under one, unchanging
mode. If interlock coverage ever changes and a trace *could* genuinely
span both states, the dwell-entry snapshot is still the safe answer: it is
taken before the first tick of the trace runs, so "captured under fuzzy
at any point during this trace" can never be missed by it -- the failure
mode this fix closes is specifically the *gap after* the trace (fuzzy
switched off between dwell-end and refine), not a mid-trace transition,
and snapshotting at entry can only ever be as conservative or more so than
a hypothetical "any tick" read for a window it precedes.

## Defect 2: the guard failed OPEN on an accessor failure

If `zones_config_get_control_mode()` (or `get_fuzzy_strength_pct()`)
returned `false` -- the accessor failing, not reporting a real mode -- the
old `if (get(...) && mode == PID_FUZZY && get(...) && pct > 0.0f)`
short-circuited to false and the correction proceeded exactly as if the
zone were confirmed plain PID. A safety-relevant guard whose failure mode
is "proceed" is backwards.

### Fix: fail closed, named distinctly

The dwell-entry snapshot now also records `trace_fuzzy_accessor_failed`
(true if either accessor call failed at snapshot time).
`adaptive_tune_refine_ki_locked()` checks
`!trace_fuzzy_snapshot_valid || trace_fuzzy_accessor_failed` **before** the
`trace_fuzzy_active` check and withholds the correction on either, through
the same `z->ki_refusal_reason` surface every other guard in this module
uses -- reusing the existing refusal path rather than adding a new one, per
this task's requirement. The message ("Ki accessor failed at dwell
capture: withholding correction (fail-closed, not fuzzy)") is worded
distinctly from the ordinary fuzzy-active refusal ("zone PID_FUZZY %.0f%%
at capture: withholding Ki -- trace reflects fuzzy, not reference") so the
two are distinguishable in the field. `!trace_fuzzy_snapshot_valid` (the
snapshot never having been taken -- defensive only, since
`adaptive_tune_refine_ki_locked()` never runs without a dwell having
entered first) is folded into the same fail-closed branch for the
identical reason: "we don't actually know" must never read as "proceed".

## Buffer-length discipline (K7, `83627343`)

`ki_refusal_reason` is `char[96]`. Both new/changed messages were sized
and measured, not guessed:
- Fuzzy-active message, worst case (`trace_fuzzy_pct` = 100): "zone
  PID_FUZZY 100% at capture: withholding Ki -- trace reflects fuzzy, not
  reference" = 85 bytes.
- Accessor-failure message (fixed text, no format args): "Ki accessor
  failed at dwell capture: withholding correction (fail-closed, not
  fuzzy)" = 84 bytes.

Both are well inside the 96-byte buffer with room for the NUL.
`test_ki_diagnosis_withholds_when_fuzzy_active_during_capture_but_off_at_refine()`
and `test_ki_diagnosis_withholds_when_control_mode_accessor_fails_at_capture()`
(`test_adaptive_tune_ki_bounds.c`) each assert `strlen(...) <
sizeof(...) - 1`, extending the same truncation check `83627343` added for
the pre-existing fuzzy-active message.

## Tests added (`test_adaptive_tune_ki_bounds.c`, wired via
`run_test_adaptive_tune()` in `test_adaptive_tune.c`)

- `test_ki_diagnosis_withholds_when_fuzzy_active_during_capture_but_off_at_refine()`:
  fuzzy ON for the entire traced dwell, then switched OFF before
  `adaptive_tune_run_end()` runs -- reproduces the exact failure the review
  described. Proves the guard still withholds (reference Ki unchanged
  across the same constant-offset trace shape that the sibling runaway
  fixture proves *does* ratchet a genuinely-plain-PID zone), names fuzzy
  in the refusal reason, and stays inside the 96-byte buffer.
- `test_ki_diagnosis_withholds_when_control_mode_accessor_fails_at_capture()`:
  forces `zones_config_get_control_mode()` to fail (via a new
  `s_fake_control_mode_fail` injection knob added to the shared fakes in
  `test_adaptive_tune.c`, needed because the existing fakes only fail on an
  out-of-range zone index, which no real zone index ever hits) with the
  zone otherwise configured as plain PID. Proves the correction is
  withheld (not applied because "as far as anything else can tell, this is
  plain PID"), the reason names the accessor failure and the fail-closed
  path distinctly from the fuzzy-active wording, and Ki is left unchanged.

Both tests are exercised through the real production functions
(`adaptive_tune_zone_tick()`, `adaptive_tune_run_end()`,
`adaptive_tune_refine_ki_locked()`), not a standalone mirror.

## Negative test (by hand, per this task's requirement)

Both fixes were reverted together in place (the fail-closed branch and the
snapshot read replaced with the old `zones_config_get_control_mode()`/
`get_fuzzy_strength_pct()` live-read, ignoring the accessor's own success
flag -- i.e. reproducing both defects at once), full clean rebuild
(`Remove-Item -Recurse -Force .../test/build`, then
`build_host_tests.ps1`). Both new tests failed as expected (8 individual
`TEST_CHECK` failures across the two tests, including the Ki-unchanged
assertions catching the ratchet actually happening: `got 1.2000, want
1.0000`). The break was restored by hand back to the fixed code (confirmed
by diffing against the pre-break source, no `neg_test_*` identifiers left
behind), the build directory deleted again, and a full clean rebuild
confirmed **1626/1626** checks passing in the `adaptive_tune` host-test
executable.

## Why this is safe to land now

The live board has `fuzzy_strength_pct = 0.0` and `adaptive_tune`
`enabled=False` on all three zones today, so neither the old nor the new
code path in `adaptive_tune_refine_ki_locked()`'s fuzzy guard executes on
real hardware right now. This is a correctness fix for the guard's logic,
not a behavior change under current configuration.

## Verification

- KilnFW host tests (`build_host_tests.ps1`): `adaptive_tune` executable
  1626/1626 checks passed. Two unrelated pre-existing failures elsewhere in
  the host-test run (`test_iter_tune.c`, `test_zones_http.c` -- missing
  `model_fit_temp_c`/`model_fit_ambient_c` fields, part of another
  session's in-flight `zone_model_at()`/`coupling_at()` schedule-seam work
  per commit `5d3bc85`) and one unrelated build failure
  (`run_test_sim_plant_three_node` unresolved symbol) are outside this
  file's scope and untouched by this change.
- KilnFW target build (`build_kilnfw` MCP tool): OK in 82.0s.
- `tools/run_all_checks.ps1`: 92 passed, 0 skipped, 2 failed. Both
  failures (`check_00_kilnfw_target_build.ps1`,
  `check_wire_protocol_fingerprint.ps1`) trace to an untracked file left by
  another concurrent session
  (`firmware/CommonFW/src/kilnlink_get_stack_margin.c`, referenced by
  `components/kilnlink/CMakeLists.txt` but not yet committed, and a
  `KILNLINK_PROTOCOL_VERSION` bump 12 -> 13 with a stale fingerprint
  manifest) -- unrelated to `adaptive_tune_ki.c` and outside this task's
  ownership.
- `tools/check_test_c_files_wired.ps1` / `tools/check_no_orphaned_checks.ps1`:
  both ran clean as part of the `run_all_checks.ps1` pass above, confirming
  the two new tests are wired in, not orphaned.
- `tools/check_doc_hash_citations.ps1`: run separately below; all commit
  hashes cited in this document (`e78fbc5b`, `a3803057`, `83627343`) were
  confirmed with `git cat-file -t <hash>` to be real commits before citing
  them here.

## Files changed

- `firmware/KilnFW/App/drivers/control/adaptive_tune_internal.h` -- new
  snapshot fields on `adaptive_tune_zone_t`.
- `firmware/KilnFW/App/drivers/control/adaptive_tune.c` -- snapshot taken
  at `dwell_just_entered` in `adaptive_tune_zone_tick()`.
- `firmware/KilnFW/App/drivers/control/adaptive_tune_ki.c` -- guard in
  `adaptive_tune_refine_ki_locked()` now reads the snapshot and fails
  closed on an accessor failure.
- `firmware/KilnFW/App/test/test_adaptive_tune.c` -- fail-injection knobs
  (`s_fake_control_mode_fail`/`s_fake_fuzzy_pct_fail`) for the shared
  `zones_config_get_control_mode()`/`get_fuzzy_strength_pct()` fakes, reset
  in `reset_module_state()`; both new tests wired into
  `run_test_adaptive_tune()`.
- `firmware/KilnFW/App/test/test_adaptive_tune_ki_bounds.c` -- the two new
  tests described above.

## Note for the separate research pass (not acted on here)

A separate research pass is examining whether the Ki trace-shape heuristic
this guard protects should exist at all, given
`adaptive_tune_model.c:204` already computes Kp/Ki/Kd absolutely from a
fresh `K_dc` fit via SIMC. If that concludes the heuristic is redundant,
this fix (and the guard it improves) may be superseded by its deletion --
noted here rather than pre-empted.
