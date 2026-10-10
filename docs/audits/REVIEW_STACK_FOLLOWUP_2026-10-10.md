# Review: stack-review follow-up batch (2026-10-10)

Scope: origin/dev commits `479f3a473` and `097aef73c`. They address M1, M2,
L1, L2 and I1 from `docs/audits/STACK_FIX_BATCH_REVIEW_2026-10-09.md`:

- `profiles_http.c`: ESP_LOGE on the OOM paths, plus the new
  `test_pcfg_boot_profile_scratch_oom_fails_closed`.
- `stack_budget_lib.py`: a non-entry, non-ROM long-call literal now marks the
  caller indirect (new test in `test_stack_budget_symbol_bounds.py`).
- Comment and doc corrections.

Reviewer: Opus. This review reports findings only and fixes nothing. It was
run in a detached worktree at `097aef73c`.

## Summary

| Id | Severity | Finding |
|----|----------|---------|
| S1 | MED | dev tip fails three stack-budget checks against a fresh target build. The cause is code growth since the ceilings were re-baselined, not this batch. |
| S2 | LOW | L2 "surface" is met by the UART log only. Neither HTTP nor readiness shows a degraded profile load. |
| S3 | LOW | The legacy name-keyed parsers (main, executor, httpd budget checks) still drop non-entry long-call literals silently, so L1 is fixed only in `stack_budget_lib.parse()`. |
| S4 | LOW | The new test does not cover `profiles_boot_load_body()`'s double-OOM path, or an end-to-end save refusal while slots are rev-unknown. |
| S5 | INFO | The live save_as path returns true when the persist fails (pre-existing). |
| S6 | INFO | `rev_repair_junk` can still run on the `nvs_load_all_from` OOM path. It only raises revs, so it stays fail-closed. |

The fixes themselves are correct and fail closed. My own mutations of the
OOM fail-closed logic and of the L1 analyser change were all caught.

## Verification performed

### Negative tests (`tools\negtest.ps1`, worktree `C:\wt\stkfu_8tb1mr`)

The C test command was `build_host_tests.ps1 -Only 'host_tests_profiles_http\.exe'`.
The baseline passed, and all 3 mutations were CAUGHT:

- `allfrom_oom_no_slot_res_err`: the OOM branch in `nvs_load_all_from` no
  longer sets `slot_res_err[]`. Caught at `test_profiles_http.c:1595`
  ("every slot marked rev-unknown").
- `filesonly_oom_returns_ok`: the files-only OOM branch returns ESP_OK.
  Caught at `:1609`.
- `allfrom_oom_skips_any_resolve_err`: the OOM branch leaves
  `any_resolve_err` false. Caught at `:1590`.

The Python test was `test_stack_budget_symbol_bounds.py` (unittest). Mutation
`nonentry_literal_dropped` turns the new `else: indirect[fn] = True` into
`pass`, which is the pre-L1 behaviour. CAUGHT ("AssertionError: False is not
true").

### Fail-closed reading

- **Files-only OOM:** each slot gets `s_profile_rev[id] = floors[id]` and
  `s_profile_rev_unknown[id] = true`, and the function returns
  ESP_ERR_NO_MEM. `floors[]` is always initialised by
  `nvs_read_rev_floors`. Saves and deletes then refuse with
  ESP_ERR_INVALID_STATE.
- **`nvs_load_all_from` OOM:** sets `any_resolve_err` and every
  `slot_res_err[id]`, and returns ESP_ERR_NO_MEM. The caller memsets the NVS
  content and falls back to files-only. The corrected I1 comment now says
  so.
- **`profiles_boot_load_body`:** now checks the files-only result and logs
  "files-only profile load also failed" instead of discarding it with
  `(void)`.

### Analyser change cannot weaken grading

`indirect` only changes the INDETERMINATE label in
`check_all_task_stack_budgets.py`. Ceiling grading (`total > ceiling`) does
not read it. I ran the check with `-DumpCeilings` twice on the same fresh ELF:
once with the current analyser, and once with the L1 branch reverted (the
negtest mutation above, used as an A/B harness). Both runs gave identical
totals (`kiln_io_owner` 2064, `http_async_job` 7712). The change therefore
alters no frame or edge, and it cannot hide or cause a FAIL.

## Findings

### S1 (MED): dev tip is over three pinned stack ceilings

Build: `check_00_kilnfw_target_build.ps1` at `097aef73c` (gated, ccache)
passed. It produced `firmware/KilnFW/build/KilnCtrl.elf` (23,372,588 B,
2026-10-10 01:36).

| Check | Result |
|-------|--------|
| check_all_task_stack_budgets | FAIL: "2 of 33 tasks over budget: kiln_io_owner, http_async_job" |
| check_system_uart_bridge_stack_budget | FAIL: "3152 B exceeds the 3136 B ceiling" |
| check_main_task_stack_budget | pass |
| check_executor_task_stack_budget | pass |
| check_httpd_task_stack_budget | pass |
| check_uart_log_bridge_stack_budget | pass (2080 B) |

Headroom against the pinned ceilings (the task asked to flag anything under
64 B):

| Task | Static peak | Ceiling | vs ceiling | Stack | Honest free |
|------|-------------|---------|------------|-------|-------------|
| kiln_io_owner | 2064 B | 1984 B | -80 B (over) | 4096 B | 1732 B |
| system_uart_bridge | 3152 B | 3136 B | -16 B (over) | 4096 B | 644 B after the 300 B allowance |
| http_async_job | 7712 B | 7632 B | -80 B (over) | -- | -- |

The deepest paths run through shared IDF code:

- `kiln_io_owner`: `kiln_io_reinit` to sx1509 to i2c_master to `esp_log` ...
  `__assert_func`.
- `system_uart_bridge`: `factory_reset_execute` to `execute_scope` to
  `cfg_fs_confirm_format` to littlefs format to `lfs_dir_fetchmatch` to
  `esp_log` ... `__assert_func` to `panic_abort`.

The ceilings were last re-baselined in `0a1cdc7dc` (2026-10-09 19:16), and
262 commits separate it from `097aef73c`. Because the A/B run above shows the
analyser is not the cause, the growth is code that landed in that window. I
did not bisect it to one commit. None of the three tasks is near
stack exhaustion; the ceilings are tripwires.

Recommendation: find the commit that grew the shared `esp_log`/assert chain
(the same +80 B on two unrelated tasks points at a common callee). Then either
justify it and re-pin the three ceilings, or shrink it. Until then, a full
`run_all_checks` on the dev tip will report these three as NEW failures.

### S2 (LOW): L2 surfaced only to the UART log

The new ESP_LOGE lines reach only the PC UART log bridge. `/api/logs` carries
firing and autotune logs only. `GET /api/profiles` has no field, and
`/api/readiness` has no item, that shows a degraded or rev-unknown load.

A user first learns of the condition when a save is refused. Even then:

- The profile POST (`profiles_edit_http.c:719-726`) answers
  `cfg_fs_http_persist_failed` only after RAM has already been assigned
  ("applied live").
- The reply does not say that the cause is an unknown rev floor.

The fixer's note that the GET reply has no degraded-state signal is accurate.
L2 is therefore partly met.

Recommendation: add a `load_degraded` / `rev_unknown_slots` field to
`GET /api/profiles`, or a readiness item.

### S3 (LOW): L1 not applied to the legacy parsers

The main, executor and httpd budget checks still use their own name-keyed
disassembly parsing, which has no indirect concept. A non-entry long-call
literal there is dropped without any note. Those three checks pass today, so
this is a coverage gap, not a live miss.

### S4 (LOW): test coverage gaps

`test_pcfg_boot_profile_scratch_oom_fails_closed` drives `nvs_load_all_from`
and `nvs_load_files_only` directly. It does not cover:

- `profiles_boot_load_body()` with both allocations failing (the path whose
  `(void)` was removed);
- a save or delete attempted afterwards, to show the ESP_ERR_INVALID_STATE
  refusal end to end.

Both behaviours are correct by reading.

### S5 (INFO): live save_as returns true on persist failure

In `profiles_http.c`, about line 1919, the live-edit save_as reports success
even when the persist step fails. This is pre-existing and is how live edits
work ("applied live"). It is listed only because a rev-unknown slot would
reach it.

### S6 (INFO): rev_repair_junk on the OOM path

On the `nvs_load_all_from` OOM path, `rev_repair_junk` can still run against
the NVS-decoded bitmap before the fallback. It can only raise revs, and the
slots are forced rev-unknown afterwards, so the result stays fail-closed.

## Doc corrections

The M1/I1 comment corrections and the status marks in
`STACK_FIX_BATCH_REVIEW_2026-10-09.md` and
`DEV_STACK_ANALYSER_REVIEW_2026-10-09.md` match the code.
