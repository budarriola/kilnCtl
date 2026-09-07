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
