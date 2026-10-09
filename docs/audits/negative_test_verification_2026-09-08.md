# Negative-test verification pass, 2026-09-08

Scope: re-verify a representative sample of the ~20 negative-test claims made
today, per the standard that a check/test that has never been shown to fail
is worthless. For each claim: actually break the production code the report
describes, run the real test, confirm it fails naming the quoted evidence,
then restore by hand and confirm `git diff` is empty. Where the owning file
is currently dirty under a live agent, or is on this pass's do-not-edit list
(`cfg_fs*`, `cfg_fs_mount.c`, `boot_guard.c`, SaftyFW `config_store`, wizard
files, the main-task stack fix), the claim is judged by inspection instead
and marked as such — never executed against code another session may be
mid-edit on.

## Results

| # | Claim (commit) | Method | Verdict | Evidence |
|---|---|---|---|---|
| 1 | `bf1db47f` — on/off zone fail-safe bypasses the actuation hold on every run-ending path | Execution | **Holds** | Set `bypass_hold`'s branch condition to `false` in `profile_executor_on_off_actuation_gate()` (`firmware/KilnFW/App/drivers/control/profile_executor_relay_io.c`). Host suite (`kilnctl_host_tests_profile_executor.exe`) went from 4887/4887 to **4881/4887, 6 failures**, all naming the exact run-ending paths the commit lists (FAULTED, authority-block, guard 5/6, IDLE/DONE, PAUSED-with-failsafe) at `test_profile_executor_prestart.c:7354`/`:7469` — matches the commit message's "6 failures" exactly. Reverted by hand; `git diff` on the file is empty. |
| 2 | `d58492c9` — guard 1 (heating-failed) is disabled for an on/off zone because its trip condition is a healthy vent's normal signature | Execution | **Holds** | Forced the `in->on_off_zone` guard-1 gate in `thermal_guard.c` to never take effect. `kilnctl_host_tests.exe` dropped from all-green to **2 failures** at `test_thermal_guard.c:1063` (false HEATING_FAILED on a healthy vent) and `:1084` (false guard-2 trip on the intended cooling case) — exactly the failure the commit claims a hand-run of this negative test produced. Reverted by hand; `git diff` empty. |
| 3 | `24090c9a` — SaftyFW config_store A/B sectors: a negative test that reintroduces erase-in-place shows zero valid copies at that instant | Inspection (file is on the do-not-edit list: SaftyFW `config_store`) | **Holds, by inspection** | The negative test itself was run by hand and reverted at commit time (not left in the tree — commit message confirms this), so it cannot be re-executed without editing off-limits production code. Confirmed instead: the A/B mechanism is real (config_store_flash.c's `needs_erase`/sector-switch state machine, not a mirror), and the **currently-committed** 169/169 `test_config_store_flash.c` suite — including the mid-erase/mid-program/between-erase-and-program power-cut fixtures — passes as-built (ran `firmware/SaftyFW/test/build_host_tests.ps1`, got 169/169). The claimed instant-of-failure scenario is consistent with the erase/program state machine actually implemented. |
| 4 | `3c36b7e1` / `check_main_task_stack_budget` — the 16 KB-on-8 KB stack overflow that panicked the board at boot | Execution (of the check script itself; production `cfg_fs_mount.c`/`kiln_cfg_store*.c` are on the do-not-edit list, not touched) | **Holds, but see finding below** | `check_main_task_stack_budget.py` computes worst-case stack depth from a real disassembly of the built ELF (frame sizes from `entry a1,N`, real static call graph from `call4/8/12` edges) — not a value derived from itself. **Separate finding**: run with no ELF present (`--elf /nonexistent/path.elf`, and true today — no `build/KilnCtrl.elf` exists in this tree), it prints `SKIPPED` but **exits 0**, confirmed by direct execution. The project's own stated standard is that a missing-prerequisite skip must be a real SKIP (exit code 3), and "anything still exiting 0 on a missing prerequisite is the old vacuous shape." This check (and its `.ps1` wrapper, which passes the exit code through unchanged and documents the exit-0 skip explicitly as intentional) currently ships the **old vacuous shape**: a checkout that has never built KilnFW reports full green from `run_all_checks.ps1` for this check with zero measurement performed. |
| 5 | `a24fa033` — zones per-zone field drift: `zones_http_client`'s POST table missing `zone_type`/`failsafe_state`/`hyst_c`/`min_on_s`/`min_off_s` (and earlier `relay_type`) | Execution | **Holds** | Added a real new per-zone field (`verify_fake_field`) to `zones_get_handler`'s `APPEND()` in `firmware/KilnFW/App/drivers/http/zones_http_get.c` (real production C, not a mirror — the check's own extraction walks the actual `APPEND()`/`http_form_find_field()` call sites by regex over the real source files). `zones_per_zone_field_drift_check.py` went from exit 0 ("no drift from firmware") to **exit 1**, correctly naming `verify_fake_field` as missing from the client. Reverted by hand; `git diff` on the file is empty. |
| 6 | `456b0fa4` — recovery-mode dashboard banner (`app.js`) | Inspection (`app.js`/`theme.css` are currently dirty under another live session) | **Holds, by inspection** | `test_recovery_banner.js` extracts the real `buildRecoveryBanner`/`setRecoveryBanner`/`pollRecoveryMode` source out of the live `app.js` by marker line (same pattern as the pre-existing `test_lag_banner.js`), not a hand-copied mirror, and runs it in a Node VM against a fetch stub. Ran the test as-committed (no edits, to avoid colliding with the other session's in-progress changes to this exact file): 14/14 pass today. The extraction-by-marker technique is sound and does test the real function bodies. |
| 7 | `d5d23b98` — safety-processor TC "not_converting" fault state renders distinctly, not as "OK" | Execution | **Vacuous claim, as stated** | The commit message says: "Negative-tested: removing the `not_converting` branch makes it render as `OK` — `test_safety_tc_diagnostics.js` catches it." Removed exactly that branch in the real production C (`diagnostics_http.c`: `faulted ? "faulted" : (temp_valid ? "ok" : "not_converting")` → `faulted ? "faulted" : "ok"`). **The JS test suite still reported 19/19 passing** — it does not call the real handler at all; it feeds a hand-constructed fixture object with `state: 'not_converting'` directly to the page's rendering code and only checks that rendering. It correctly guards the **page's** handling of an already-computed `not_converting` state, but cannot and does not catch a regression in the **server-side computation** of that state, contrary to what the commit message claims. No C-level test of `diagnostics_http.c`'s state computation exists. Reverted the C change by hand; `git diff` on the file is empty. |
| 8 | `0d1efde7` — `boot_guard_reset_counter()` is additive, not a general weakening (a genuinely failing boot must still trip recovery) | Inspection (`boot_guard.c` is on the do-not-edit list) | **Holds, by inspection** | `test_a_genuinely_failing_boot_still_trips_recovery()` drives `RECOVERY_MODE_BOOT_THRESHOLD` real reboots through the real `boot_guard_init()`/`boot_guard_is_recovery_mode()` with nothing ever calling `boot_guard_reset_counter()`/`mark_healthy()`, asserting recovery mode is still reached — this exercises the actual production functions against a real named constant (`RECOVERY_MODE_BOOT_THRESHOLD`), not a self-derived expectation. Sound design; not re-executed only because the file is off-limits for editing this pass. |

## Coverage note

8 claims were checked in this pass: 5 verified by actually breaking and
restoring production code and re-running the real host/JS test suite, 3 by
inspection only (files off-limits for editing on this pass — SaftyFW
`config_store`, `boot_guard.c`, and `app.js`/`theme.css`, the last dirty
under another live session rather than on the fixed off-limits list). This
is not exhaustive of today's ~20 claims; it is a deep check of the
highest-consequence subset (fail-safe relay energization, guard 1 exclusion,
config-store erase window, boot stack overflow, zones field drift, recovery
banner, TC fault rendering, and recovery-mode escape-hatch scoping).

## Findings

1. **Vacuous negative-test claim (`d5d23b98`)**: the commit's stated
   negative test does not and cannot catch the regression it describes,
   because it never calls the production C function that computes the
   `state` field — it hands the JS renderer an already-decided
   `not_converting` string. The page-rendering behavior it does test is
   real and correct; the safety claim about catching a server-side
   regression is not.
2. **Old vacuous SKIP shape (`check_main_task_stack_budget.py`/`.ps1`)**:
   confirmed by direct execution — a missing ELF or missing Xtensa
   objdump exits 0, not the harness's exit-code-3 SKIP convention. A fresh
   or not-yet-built checkout reports this check as passing having measured
   nothing.
3. No self-referential assertions were found among the 8 claims checked —
   every production-side check inspected parses/exercises the real
   source (regex-extracted from the actual `.c`/`.js` files, or calling
   the real compiled functions) rather than comparing a value against
   itself or a hand-typed mirror.
4. No silent 0-exit skip was found in the 5 execution-verified checks
   beyond finding 2 above.

## What would make this stronger

- Re-run claims 3, 6, 8 once the owning files are no longer off-limits/dirty
  under another session, this time by actually breaking and restoring the
  production code rather than inspection alone.
- Add a C-level host test for `diagnostics_http.c`'s `state` computation
  (finding 1) so the "not_converting" claim becomes true rather than only
  aspirational.
- Fix `check_main_task_stack_budget.py`/`.ps1` to exit 3 (real SKIP) instead
  of 0 when the ELF or objdump is missing (finding 2).

## Fixes applied (this pass)

1. **Finding 1 (`d5d23b98`, vacuous TC-state negative test) — FIXED.** Pulled
   the state-selection logic out of `thermo_faults_get_handler()`
   (`firmware/KilnFW/App/drivers/http/diagnostics_http.c`) into a `static
   inline` pure function, `diag_safety_tc_state(double tc_temp_c, uint32_t
   tc_fault)`, in `diagnostics_http.h` — same pattern as this header's
   existing `dashboard_safety_ready()` in `dashboard_http.h`, chosen so it is
   host-testable without standing up httpd/safety_link/thermo_owner. New host
   test `firmware/KilnFW/App/test/test_diagnostics_safety_tc_state.c` drives
   this REAL production function directly (registered in `test_main.c` and
   `build_host_tests.ps1`), covering ok / faulted / the not_converting case
   (NaN + fault==0) / NaN-with-fault-bit priority. Negative-tested: collapsed
   the function to `faulted ? "faulted" : "ok"` (dropping the
   `not_converting` branch) — the new host suite failed at
   `test_diagnostics_safety_tc_state.c:62`: "NaN temperature with
   fault_status==0 must read not_converting, not ok and not faulted -- a
   dead chip must never render as healthy". Reverted by hand; `git diff` on
   `diagnostics_http.h` is empty relative to the fix. The pre-existing
   `test_safety_tc_diagnostics.js` is left in place (it still legitimately
   covers the page-rendering half); the new C test is what closes the gap
   the commit message overclaimed.
2. **Finding 2 (`check_main_task_stack_budget`, silent exit-0 skip) —
   FIXED.** `check_main_task_stack_budget.py`'s two prerequisite branches
   (missing ELF, missing `xtensa-esp32s3-elf-objdump`) now `return 3` with a
   message containing `SKIP:`, matching `run_all_checks.ps1`'s reserved
   SKIP exit code; the `.ps1` wrapper's header comment updated to match
   (it already passed `$LASTEXITCODE` through unchanged, so no wrapper
   logic change was needed). Chose SKIP over FAIL for the missing-ELF case:
   an unbuilt checkout has nothing to measure and this is not a defect in
   the checkout itself, matching `check_stub_signature_drift.ps1`'s
   documented reasoning for the same shape ("a missing ESP-IDF/build
   artifact is a legitimate, expected state on plenty of machines"); a
   missing `sdkconfig` value or missing `app_main` symbol in a real ELF
   still `return 1` (FAIL) unchanged, since those indicate something wrong
   in a build that DID happen. Negative-tested both paths: `--elf
   nonexistent.elf` now prints `SKIP: no ELF at ...` and exits 3 (was exit
   0); with a real ELF present, `-StackBytes 6000` (budget 4500 B against
   the measured 5792 B path) still correctly FAILs with exit 1, naming the
   same call path. No file changes needed reverting (the fix is the
   intended final state, not a broken-then-restored probe).
3. **Sweep of other `check_*.ps1`/`.py` scripts**: grepped every
   `check_*.ps1` for a missing-prerequisite branch and its exit code.
   `check_stub_signature_drift.ps1` already uses exit 3 correctly (fixed in
   an earlier pass per its own header, `docs/audits/
   check_independence_2026-09-07.md`). Every other check with a
   prerequisite guard (`check_flash_worker_lint.ps1` and the mirror-drift
   family: `check_approach_rate_cap_mirror_drift.ps1`,
   `check_fuzzy_gain_mirror_drift.ps1`, `check_heater_output_pwm_drift.ps1`,
   `check_pid_fuzzy_drift.ps1`, `check_power_diag_flag_mirror_drift.ps1`,
   `check_ramp_lock_decision_mirror_drift.ps1`,
   `check_ramp_stepping_gate_mirror_drift.ps1`,
   `check_source_path_drift.ps1`, `check_wire_protocol_fingerprint.ps1`,
   `check_frame_a_offset_drift.ps1`, `check_cfg_fs_tie_break.ps1`,
   `check_ui_shell_layout.ps1`) already treats its missing prerequisite as
   exit 1 (FAIL), not exit 0 — `check_main_task_stack_budget` was the only
   script still shipping the old vacuous shape. No second instance of the
   "asserts against a mirror instead of the real production function" shape
   was found among today's other JS/C test additions in the time available
   for this pass; a full re-audit of every test added 2026-09-08 was out of
   scope here (see "What would make this stronger" above, which still
   applies for claims 3/6/8).
