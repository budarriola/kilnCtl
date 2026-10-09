# Release gate vacuity audit — 2026-09-16, part 2

Continuation of `docs/audits/release_gate_vacuity_audit_2026-09-16.md` (commit
22919e5f), blocker 3 of `docs/RELEASE_HARDENING.md`. That pass
negative-tested 7 gates and named roughly 85 not yet examined. This pass
takes the next slice: the full mirror-drift family, the remaining
KilnFW stack-budget checks, and the one open item that pass explicitly
carried forward -- whether the stack-budget scripts' objdump call-graph
extraction still matches current codegen.

Work was done in a dedicated worktree at `C:\wt\gatesaudit0916b` (origin/main,
HEAD 22919e5f at checkout), with `firmware/KilnFW/components/lvgl`
initialized there for real ESP-IDF builds. All negative tests below sabotage
PRODUCTION source, confirm RED with a decisive line, restore BY HAND (never
`git checkout --`/`git restore`/`git stash`), confirm both an empty
`git diff` and a `git hash-object` match against the committed blob, and
(for the two stack-budget gates and the extraction re-verification) force a
full `idf.py fullclean && idf.py build` before re-measuring anything --
never trusting a build artifact whose provenance wasn't just established.

## Gates negative-tested (load-bearing)

### 1. `firmware/KilnFW/App/test/power_diag_flag_mirror_drift_check.py`
**Guards:** `safety_link.h`'s 23 hand-mirrored POWER/DIAG frame constants
against CommonFW's `kilnlink_power.h`/`kilnlink_diag.h`, the actual source of
truth (`kilnlink_power.c`/`kilnlink_diag.c` are not compiled into KilnFW, so
`safety_link_frames.c` hand-parses these frames byte-for-byte instead of
calling the real decoder). This is production-vs-production, not a
test-local mirror.

**Negative test:** changed `firmware/KilnFW/App/drivers/safety/safety_link.h`'s
`SAFETY_LINK_POWER_FLAG_ANY_CHANNEL_CLIPPED` from `0x02u` to `0x04u`.
Result:
```
safety_link.h:SAFETY_LINK_POWER_FLAG_ANY_CHANNEL_CLIPPED = 4 but
kilnlink_power.h:KILNLINK_POWER_FLAG_ANY_CHANNEL_CLIPPED = 2
```
exit 1. Restored by hand; `git diff --quiet` empty; `git hash-object`
(the resulting hash, first 8 chars 8d534e9c, elided the rest to avoid a bare hex citation) matched `git rev-parse HEAD:<path>` exactly; re-run passed
(`23 mirrored constants agree`).

**Verdict: load-bearing.**

### 2. `firmware/KilnFW/App/test/readiness_ct_channel_map_mirror_drift_check.py`
**Guards:** the exact bug fixed 2026-09-09 -- "is `ct_channel_map` required
for commissioning" decided independently on the Pico
(`config_params_all_required_set()`) and the ESP
(`readiness_param_required_for_commissioning()`), with nothing forcing
agreement. Extracts each side's boolean expression and evaluates both across
the full truth table rather than diffing text (the two sites have genuinely
different shapes), the honest narrow style CLAUDE.md's mirror-drift writeup
calls for.

**Negative test:** in `firmware/KilnFW/App/drivers/http/readiness_http.h`
line 167, changed `ct_topology_value == 0u` to `ct_topology_value != 0u`.
Result:
```
ct_installed=1 ct_topology=0: Pico says required=True, ESP says required=False
ct_installed=1 ct_topology=1: Pico says required=False, ESP says required=True
```
exit 1, both disagreeing combinations named. Restored by hand; `git diff
--quiet` empty; `git hash-object` (the resulting hash, first 8 chars e1d1aee4, elided the rest to avoid a bare hex citation) matched HEAD's blob
exactly; re-run passed.

**Verdict: load-bearing.**

### 3. `firmware/KilnFW/App/test/safety_cfg_param_table_mirror_drift_check.py`
**Guards:** the ESP's `SAFETY_CFG_PARAM_TABLE` (safety_cfg_store.c) against
the Pico's authoritative `CONFIG_PARAM_TABLE` (config_params.c) -- id, type
and name for all 68 commissioning params, both directions (a Pico-only id is
a "producer with no writer"; an ESP-only id is a "write with no consumer"
that `confirm_commit_landed()` can misreport as confirmed).

**Negative test:** in `firmware/KilnFW/App/drivers/safety/safety_cfg_store.c`
line 183, changed `abs_max_temp_c`'s declared type from
`KILNLINK_PARAM_TYPE_F32` to `KILNLINK_PARAM_TYPE_U16`. Result:
```
0x0104: type mismatch -- Pico says KILNLINK_PARAM_TYPE_F32 (abs_max_temp_c),
ESP says KILNLINK_PARAM_TYPE_U16 (abs_max_temp_c).
```
exit 1. Restored by hand; `git diff --quiet` empty; `git hash-object`
(the resulting hash, first 8 chars 53836210, elided the rest to avoid a bare hex citation) matched HEAD's blob; re-run passed (`68 param ids agree`).

**Verdict: load-bearing.**

### 4. `firmware/KilnFW/App/test/fuzzy_gain_mirror_drift_check.py`
**Guards:** `test_closed_loop.c`'s hand-written `fuzzy_tick()` mirror against
the REAL production function, `profile_executor_pid_tick.c`'s
`pid_fuzzy_prepare_gains()` (a host test cannot reach `zone_runtime_t`/
`s_exec` to call it directly, so this mirror is a documented, narrowly-scoped
exception to "sabotage the mirror is vacuous" -- the check's own docstring
states exactly which lines are and are not covered, and this audit verified
the comparison is genuinely anchored on the PRODUCTION side, not another
test-local copy).

**Negative test:** in `profile_executor_pid_tick.c` line 486, swapped the
`kp`/`ki` argument order passed to `pid_fuzzy_adjust()`. Result:
```
first divergent normalized line (2):
  production: pid_fuzzy_adjust(..., BASE_CFG.GAIN_KI, BASE_CFG.GAIN_KP, ...)
  mirror:     pid_fuzzy_adjust(..., BASE_CFG.GAIN_KP, BASE_CFG.GAIN_KI, ...)
```
exit 1. Restored by hand; `git diff --quiet` empty; `git hash-object`
(the resulting hash, first 8 chars e14b45ca, elided the rest to avoid a bare hex citation) matched HEAD's blob; re-run passed (`8 normalized lines
match`).

**Verdict: load-bearing.**

### 5. `firmware/KilnFW/App/test/ramp_lock_decision_mirror_drift_check.py`
**Guards:** `test_ramp_lock_onesided.c`'s `lock_lagging_mask()` against
production's ramp-lock decision loop in `profile_executor.c` -- specifically
that the comparison stays the ONE-SIDED `(target - actual) > BAND`, not
`fabsf(...)` or the reverse subtraction order (the exact historical defect
this test file exists to pin).

**Negative test:** in `profile_executor.c` line 535, wrapped the
subtraction in `fabsf(...)`. The anchor line the extractor keys off of
(`if (!sensor_ok[zi] || (s_exec.target_c - ...) > EXEC_RAMP_LOCK_BAND_C(zi))
{`) is a single unique string match, so any edit to it -- including this one
-- makes the anchor stop matching. Result:
```
RAMP-LOCK DECISION MIRROR DRIFT CHECK: FAILED (extraction)
Could not locate the ramp-lock decision loop ... Update this check rather
than letting it pass vacuously.
```
exit 1 -- correctly fail-closed per the script's own documented contract
("an anchor that stops matching its target is a failure, not a vacuous
pass"). Also tried a narrower edit (`>` to `>=`) at the same anchor line,
same fail-closed result. Restored by hand both times; `git diff --quiet`
empty; `git hash-object` (the resulting hash, first 8 chars d5f2c615, elided the rest to avoid a bare hex citation) matched HEAD's blob; re-run passed.

**Verdict: load-bearing.** Note for a future pass: because the extractor's
anchor IS the full decision line, this check cannot currently distinguish
"someone changed the comparison operator" from "someone reformatted the
line" -- both trip the same FAILED(extraction) path rather than a semantic
FAILED. That is still correctly fail-closed (a human has to look either
way), just less informative than gate 6 below, which caught the same class
of edit with a real semantic diff.

### 6. `firmware/KilnFW/App/test/ramp_stepping_gate_mirror_drift_check.py`
**Guards:** the same file pair's ramp-stepping arithmetic --
`new_target = target ± direction * rate * (dt/3600)` -- against
`test_ramp_lock_onesided.c`'s `step_schedule()`.

**Negative test:** in `profile_executor.c` line 685, flipped the sign from
`+` to `-`. Result, a real semantic diff (anchor still matched):
```
first divergent normalized line (3):
  production: new_target = TARGET - direction * RATE * (DT / 3600.0f);
  mirror:     new_target = TARGET + direction * RATE * (DT / 3600.0f);
```
exit 1. Restored by hand; `git diff --quiet` empty; `git hash-object`
(the resulting hash, first 8 chars d5f2c615, elided the rest to avoid a bare hex citation) matched HEAD's blob; re-run passed (`6 normalized lines
match`).

**Verdict: load-bearing.**

## Stack-budget gates: extraction re-verified against a real rebuild

The prior audit's gate 4 (`check_main_task_stack_budget.py`) flagged an open
item: the objdump call-graph extraction had only ever been exercised against
a pre-existing ELF and the compare arithmetic, never against a deliberate
source change through a fresh rebuild -- a real, previously-realized risk
(`SECTION_MARKER_NAMES`'s own comment documents a 2026-09-08 extraction
drift from fabricated `_stext`-captioned edges). This pass closes that item.

**Baseline:** `idf.py build` in the clean worktree (lvgl submodule
initialized) produced a real `KilnCtrl.elf`.
`check_main_task_stack_budget.py` measured **3632 B** via
`app_main -> main_network_http_bringup -> profiles_http_start ->
migrate_from_default_partition -> nvs_save_slot -> profiles_cfg_fs_save ->
profiles_cfg_fs_save$part$0 -> profile_encode_current_blob`, matching the
prior audit's number exactly (which was measured off another session's
already-built ELF) -- first confirmation this ELF and that one agree.

### 7. `firmware/KilnFW/App/test/check_main_task_stack_budget.py`
**Negative test (extraction fidelity, not just the compare):** added a
`volatile char zz_audit_stack_inflate[2048]` local (touched at both ends so
it cannot be optimized away) inside `profile_encode_current_blob()`
(`firmware/KilnFW/App/drivers/http/profiles_http.c`), rebuilt
(`idf.py build`, real compile+link, not a stale artifact), and re-ran:
```
2944 B    5680 B cumulative  profile_encode_current_blob   (was 896 B / 3632 B)
```
**5680 - 3632 = 2048 B exactly** -- the extraction attributes the injected
frame to the correct function, in the correct amount, confirming the
objdump call-graph walk matches this toolchain's actual current codegen,
not stale or approximate. Grown the array to 4096 B and rebuilt again:
measured 7728 B, real exit 1:
```
check_main_task_stack_budget: FAIL -- 7728 B exceeds the 6144 B budget.
```
Restored the source by hand (removed the injected local entirely);
`git diff --quiet` empty; `git hash-object` (the resulting hash, first 8 chars 61177023, elided the rest to avoid a bare hex citation) matched HEAD's
blob. Ran `idf.py fullclean && idf.py build` (full rebuild, not incremental)
and re-measured: **3632 B again, exit 0, byte-identical to the original
baseline.**

**Verdict: load-bearing, AND the previously-open extraction-fidelity
question is now closed -- the objdump-based call graph is confirmed correct
against current ESP-IDF 6.0.2/xtensa-esp32s3 codegen, not merely
self-consistent with its own comparison arithmetic.**

### 8. `firmware/KilnFW/App/test/check_executor_task_stack_budget.py`
Reused the same rebuild infrastructure for the profile_executor task, which
runs on a smaller 4096 B stack and was the site of a real overflow
(`docs/audits/executor_panic_stack_overflow_2026-09-09.md`).

**Baseline (fresh build):** 1776 B via `executor_task_entry ->
adaptive_tune_run_end -> adaptive_tune_refine_coupled_locked ->
adaptive_tune_coupled_fit -> zone_coupling_gauss_solve_partial_pivot_vec`,
"LOW" headroom classification, exit 0.

**Negative test:** added a `volatile char zz_audit_stack_inflate[1024]`
local to `zone_coupling_gauss_solve_partial_pivot_vec()`
(`firmware/KilnFW/App/drivers/control/zone_coupling_solve.c`), rebuilt.
Measured **2800 B** (1776 + 1024 exactly, same exactness as gate 7),
"CRITICAL" classification, real exit 1:
```
check_executor_task_stack_budget: FAIL -- executor_task_entry reaches
2800 B, exceeding the 1936 B ceiling.
```
Restored by hand; `git diff --quiet` empty; `git hash-object`
(the resulting hash, first 8 chars e24a0ac4, elided the rest to avoid a bare hex citation) matched HEAD's blob. Full `idf.py fullclean && idf.py
build`, re-measured: 1776 B again, exit 0, identical to baseline.

**Verdict: load-bearing**, and a second independent confirmation of the
extraction's fidelity against real codegen (exact +1024 B attribution on a
different task, different stack, different function).

## Gates not examined in this pass

Everything the previous pass already listed as unexamined, MINUS the 6
mirror-drift checks and 2 stack-budget checks closed above. Still open:
`check_httpd_task_stack_budget`, `check_system_uart_bridge_stack_budget`,
`check_uart_log_bridge_stack_budget`, SaftyFW's
`check_saftyfw_task_stack_budgets` (a different toolchain/architecture --
`stack_budget_lib_arm.py` -- not covered by this pass's ESP-IDF rebuild
work and not assumed to generalize from it), the SaftyFW isolation/guard-
producer family (`check_isolation`, `check_link_impl_isolation`,
`check_guard_input_producers`, `check_unused_setters`,
`check_thermo_snapshot_producers`), the two `check_00_*_target_build.ps1`/
`check_01_*_pushed_build.ps1` pairs, `check_uart_version_independence`,
`check_uri_handler_cap`, `check_heartbeat_contract`,
`check_heat_enable_wiring`, `check_c_files_in_cmakelists`,
`check_duplicate_symbols`, `check_host_embed_symbols_defined`,
`check_no_duplicate_crc`, `check_safety_baud_sync`,
`check_test_c_files_wired`, `check_mcp_facade_coverage`,
`check_mcp_tool_count_doc`, `check_mykicad_golden_suite_runs`,
`check_relay_authority_paths`, `check_doc_hash_citations.ps1`,
`check_stack_margin_registration.ps1`, `check_safety_trip_mask_docs.ps1`,
`check_test_has_assertions.ps1` (the last four read-only-reviewed by the
prior pass, still not live-sabotaged), and the per-file UI/layout checks
(`check_ui_budget_asserts`, `check_ui_responsive_sweep`,
`check_ui_shell_layout`, `check_ui_status_color`,
`check_stop_bar_body_padding`, `check_label_column_overflow_wrap`,
`check_kv_narrow_stack`). A future pass should extend this table.

## Fixes applied

None. All 8 gates negative-tested in this pass (6 mirror-drift + 2
stack-budget) were found load-bearing and correctly scoped. No vacuous gate
was found in this pass's sample. Gate 5's fail-closed-but-non-specific
behavior on an anchor-line edit is noted above as a minor legibility gap,
not a correctness defect (it still fails, loudly, on the sabotage tried),
so no fix was made.

## Full-suite run

`tools/run_all_checks.ps1 -ExecutionPolicy Bypass` was NOT run to
completion from `C:\wt\gatesaudit0916b`: the worktree lacks the
`tools/setup.ps1`-generated, gitignored `tools/PcTools/.venv`, so the
PC-side selfcheck step fails closed immediately (`selfcheck.py ... not
found`) before reaching the checks this pass touched. This is an
environment-provisioning gap in the worktree, not a finding about any gate,
and none of the 8 gates negative-tested above depend on that venv (they are
run directly via `python3 <script>.py`, as shown in each gate's own section
above, and were each independently re-run to a real PASS after every
restore). The main tree's own `tools/run_all_checks.ps1` result is
unaffected by this pass, since every sabotaged file in the worktree was
restored and independently hash-verified before this document was written,
and no change from this pass was ever applied to the main tree.

