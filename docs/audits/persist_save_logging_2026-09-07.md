# Persist/save logging audit -- 2026-09-07

Motivated by `zone_normals_save()` (`firmware/KilnFW/App/drivers/persist/zones_config_store.c`)
logging nothing on a failed `nvs_set_blob`/commit, which hid the 16-char NVS-key bug
(`project_nvs_key_too_long_zone_normals`) for weeks. Every persist/save path reachable from
`firmware/KilnFW/App` was audited for: (a) return value discarded entirely, (b) return value
checked but no `ESP_LOGW/E` naming the module + `esp_err_to_name`/`hal_status_to_name`, (c) an
unconditional "saved" success log regardless of the result (the "safety calls logging unchecked
success" class).

`firmware/KilnFW/App/drivers/persist/relay_cycles.c` and `.../persist/unit_pref.c` were **skipped**
-- both were locally modified by another session (NVS key-length work) at audit time and were left
untouched per instructions.

## Findings

| Module | Function | Line(s) | Class | Fixed? |
|---|---|---|---|---|
| `persist/zones_config_store.c` | `zone_normals_save()` | 594 (now ~598) | b (checked, no log) -- the seed bug named in the task | Yes -- added `ESP_LOGW` with `hal_status_to_name` |
| `persist/zones_config_store.c` | `relay_names_save()` | 428-442 | b (checked, no log; one call site in `zones_config_accessors.c:347` had no logging at all) | Yes -- added `ESP_LOGW` inside the function so every caller is covered |
| `persist/zones_config_store.c` | migration rewrite after v1->current | 248 | a (result discarded, bare call) | Yes -- captured into `save_err`, logged `ESP_LOGW` |
| `persist/zones_config_store.c` | clear invalid/missing active id (2 sites) | 492, 502 (pre-fix) | a (`(void)nvs_save_store()`) | Yes -- captured + `ESP_LOGW` |
| `persist/zones_config_store.c` | `zone_ct_map_clear()` | 649 (pre-fix) | a (`(void)zone_normals_save()`) | Yes -- captured into a named var (log now lives inside `zone_normals_save()`) |
| `persist/zones_config_store.c` | `zone_k_ct_clear()` | 673 (pre-fix) | a (`(void)zone_normals_save()`) | Yes -- same fix |

## Everything else checked (no defect found)

All other `hal_kv_set_*`/`hal_kv_commit`/`nvs_set_*`/`nvs_commit` call chains under
`firmware/KilnFW/App/drivers` were traced to their outer caller and confirmed to either:
- log `ESP_LOGE`/`ESP_LOGW` naming the operation and `esp_err_to_name()`/`hal_status_to_name()` on
  failure, or
- propagate the `esp_err_t`/`hal_status_t` via `return` to a caller that does the same (verified
  transitively, e.g. `adaptive_tune.c`'s `save_kibase_job()` -> `job->result` -> three call sites
  that all log it).

Covers: `control/adaptive_tune.c`, `control/profile_executor_firing_stats.c`,
`control/ramp_assist_cfg.c`, `control/run_state.c`, `http/profiles_http.c` (`nvs_save_slot`,
`nvs_erase_slot`), `net/time_sync.c`, `net/wifi_prov_nvs.c` (5 setters + 2 migration call sites),
`persist/boot_guard.c`, `persist/display_power_cfg.c`, `persist/kiln_cfg_store.c` (all 5 outer
`nvs_save_store()` callers besides the 3 findings above), `persist/ota_record.c`,
`persist/profiles_builtin.c` (`hidden_mask_save()`), `persist/touch_cal_store.c`,
`safety/crash_report.c`, `safety/safety_cfg_store.c` (`nvs_save_store`/`save_safety_relay_type`/
`save_ct_cal`, all routed through `safety_cfg_store_flush_if_dirty()` which itself logs),
`safety/watchdog_cfg.c`.

No class-(c) unconditional-success-log instance was found this pass (the earlier
`safety_calls_logging_unchecked_success` audit's pattern was every-log-is-real for the
persist-save surface).

## Check widened

`tools/check_safety_call_results_checked.ps1` (previously scoped to relay-off/heat-enable calls)
was widened to add `nvs_save_store` and `zone_normals_save` to its tracked function-name patterns,
so a future persisted-state write whose `esp_err_t`/`hal_status_t` result is dropped on the floor
(`fn();` or `(void)fn();`) fails the check the same way an unchecked relay write does.

Negative-tested: reverted `zone_ct_map_clear()`'s fix back to `(void)zone_normals_save();` and
re-ran the check, which failed with:

```
firmware/KilnFW/App/drivers/persist/zones_config_store.c:650: call to zone_normals_save() does not
capture its return value into a variable -- (void)zone_normals_save();
```

then hand-restored the fix (re-applied the same edit; no `git checkout`/`restore` used) and
confirmed `git diff` on the file... the check then re-passed (38 call sites, 0 violations).

## SaftyFW / CommonFW pass (2026-09-07)

Same three classes, applied to `firmware/SaftyFW` and `firmware/CommonFW`: every
`config_store_*`/`hal_flash_erase`/`hal_flash_program`/`_save(`/`_store(`/`_commit(`/`_flush(`/
`record_write`/CRC-write call path was traced to its outer caller. `firmware/CommonFW` has no
flash/config persistence of its own (only CRC/frame math, no writes) -- nothing to audit there.

### Findings

| Module | Function | Line(s) | Class | Fixed? |
|---|---|---|---|---|
| `src/config_store_flash.c` | `config_store_write_cb()` / `config_store_write()` | 334-343, 385-407 (pre-fix) | a (`hal_flash_erase()`/`hal_flash_program()` results discarded with bare `(void)`) -- masked by `hal_flash_safe_execute()` returning `HAL_OK` regardless, since the host fake's own doc comment confirms "any erase/program failure the callback triggers is surfaced through ITS own return path ... not through this function's return value". The near-identical `update_metadata_write_cb()` in `update_task.c` was already fixed for this exact shape on 2026-09-06; `config_store_flash.c`'s twin was missed. | Yes -- added a `hal_status_t result` field to `config_store_write_args_t`, captured into it, and `config_store_write()` now checks `status != HAL_OK \|\| args.result != HAL_OK` before reporting success |
| `src/tasks/update_task.c` | `update_task_revert_target_slot()` | 625 (pre-fix) | a (`(void)update_task_persist_metadata(...)`, "best-effort") | Yes -- kept best-effort (no control-flow change) but logs `LOG_LEVEL_WARN` on failure |
| `src/tasks/update_task.c` | `update_task_process_begin()` (erase) | 717 | b (checked, wire-status-only, no on-device log -- file's own comment noted "no log_task call site wired up in this file yet") | Yes -- added `LOG_LEVEL_ERROR` |
| `src/tasks/update_task.c` | `update_task_process_begin()` (STAGED persist) | 730 | b (same) | Yes -- added `LOG_LEVEL_ERROR` |
| `src/tasks/update_task.c` | `update_task_process_end()`-equivalent (PENDING_VERIFY persist) | 867 | b (same) | Yes -- added `LOG_LEVEL_ERROR` |
| `src/tasks/update_task.c` | `update_task_confirm_tick()` (VALID persist) | 1026 | b (checked, retried silently forever on failure, no log) | Yes -- added `LOG_LEVEL_WARN` |

### Everything else checked (no defect found)

- `src/tasks/link_task.c`'s three `config_store_write()` call sites (`SET_CONFIG`, `SET_CT_CAL`,
  `COMMIT_CONFIG`) all already log `LOG_LEVEL_INFO`/`LOG_LEVEL_WARN` naming the handler tag and the
  refusal reason string on every path.
- `update_task.c`'s rollback call site (`update_task_request_rollback()`, persist at ~1128)
  propagates `*out_reason`/`*out_reason_code` to its one caller, `link_task_handle_rollback()`,
  which logs `LOG_LEVEL_WARN` naming the reason on refusal (acceptance is logged before the call).
- `bootloader/persist.c` and `bootloader/recovery_update.c` call pico-sdk's `flash_range_erase()`/
  `flash_range_program()` directly -- both are `void`, pre-scheduler, no logging facility exists
  in the bootloader at all, nothing to check or discard.
- `src/config_store_flash.c`'s boot-time load path (`config_store_boot_load()`) already has a loud
  `console_uart_puts()` block naming the field/rule on a rejected record (2026-08-27 fail-open fix,
  predates this audit).
- No class-(c) unconditional-success-log instance was found.

### Negative test

`config_store_write_cb()`'s fix is pinned by a new host test,
`test_program_failure_is_not_masked_by_safe_execute_ok()`
(`firmware/SaftyFW/test/test_config_store_flash.c`), using the host fake's
`fake_flash_script_next_op_status(FAKE_FLASH_OP_PROGRAM, HAL_IO)` injection -- this is the one
failure shape `hal_flash_safe_execute()`'s own return value cannot surface (its `HAL_OK` means only
"the callback ran"), so it is the direct regression test for this bug. No existing `check_*.ps1`
covers this class on the SaftyFW side: `tools/check_safety_call_results_checked.ps1` is scoped to
`firmware/KilnFW/App` only (its own guard throws if that directory is not found), and no SaftyFW-
side equivalent exists, so a host test was added instead of widening a check.

Negative-tested by hand: reverted `config_store_write_cb()`'s fix back to the bare
`(void)hal_flash_erase(...)`/`(void)hal_flash_program(...)` calls and re-ran
`build_host_tests.ps1`, which failed with:

```
FAIL ...test_config_store_flash.c:214: write fails when the underlying hal_flash_program() fails,
     even though hal_flash_safe_execute() itself returns HAL_OK
FAIL ...test_config_store_flash.c:216: the reason string is not the success sentinel on a masked failure
FAIL ...test_config_store_flash.c:222: a program failure the safe_execute wrapper did not itself
     report still leaves the cache at its pre-write state
57/60 checks passed, 3 FAILURE(S)
```

then hand-restored the fix (re-applied the same edit; no `git checkout`/`restore` used) and
confirmed `git diff` against the pre-revert version was empty -- the check then re-passed
(60/60). `build_saftyfw` (target build, all three images: `SaftyFW`/`SaftyFW_slotA`/
`SaftyFW_slotB`) and `tools/run_all_checks.ps1` (56/56) both green afterward.
