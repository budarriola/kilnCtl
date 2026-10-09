# Host-test gap audit, firmware changed since 2026-10-04

Date: 2026-10-09. Base: origin/dev `1de8bdd3`. Read-only audit; no code changed.

Scope: `git log --since=2026-10-04 -- firmware/`, restricted to these areas:

- the update stage/fetch chain
- aux outputs and the aux fault-drop
- backup import/export
- the profiles rev floor and junk repair
- the wifi_prov legacy migration
- the CSRF origin check
- the zones generation re-check
- the factory_reset legacy erase

Method: for each branch I checked whether a test in `firmware/KilnFW/App/test/` (`test_*.c`, `build_host_tests.ps1`) actually reaches it. A test that gates out before the branch, or a source-text lint, does not count as reaching it.

All paths below are under `firmware/KilnFW/App/` unless stated otherwise. Line numbers are at `1de8bdd3`.

## Ranked gaps

### 1. HIGH: `update_fetch.c` and `update_http.c` have no host build

`build_host_tests.ps1:246-255` compiles the following, each with a real test:

- `update_semver`
- `stage_header`
- `update_policy`
- `update_stage`
- `update_stale_stage`
- `update_settings`
- `update_url`
- `update_release`
- `update_fetch_heap`

`drivers/update/update_fetch.c` (1134 lines) and `drivers/update/update_http.c` (730 lines) are not compiled into any test. Their only coverage is:

- the source-shape lint `test/update_chain_lint.py`, which checks statement order (claim before mode gate, bounded `wr_done` wait, internal scratch, manifest gate);
- a source-text scan in `test_update_stage.c` (around line 597).

Neither runs any branch.

Untested branches, all added or changed by the review 3, 5, 7 and 8 fixes:

- `update_fetch.c:381-392`: the writer task's cleanup of an abandoned op.
  - A late `WR_FINISH` that succeeded leads to `update_stage_clear()` (`:389`).
  - A late `WR_BEGIN`/`WR_WRITE` leads to `update_stage_upload_abort_owned(st, STAGE_SOURCE_GITHUB)` (`:391`).
  - Missing test: the caller times out, then the writer completes the op. Assert that no verified stage is left behind and that a stage owned by an upload is not aborted.
- `update_fetch.c:446-460`: a `wr_call` timeout sets `wr_wedged` (`:456`).
  - A later job is then refused at `:661` with `writer_wedged_reboot_required`.
  - `fetch_task` leaks the buffers instead of freeing them (`:854-857`).
  - Missing test: a wedged writer refuses the next `stage_begin` and never frees memory the writer may still own.
- `update_fetch.c:806` (`short_body`) and `:819` (`sha256_mismatch`).
  - Then `fetch_task` sends `WR_ABORT` when `stage_begun && !stage_done` (`:849-851`).
  - Missing test: a body whose length or hash differs from the manifest leaves no stage and reports the right error.
- `update_fetch.c:873-875`: `ota_http_update_end()` runs only when `p.claimed`. The same release happens in the `start_job` failure path at `:920`.
  - Missing test: every failure exit releases the update claim exactly once. A leaked claim blocks profiles and autotune until reboot.
- `update_fetch.c:974-1019` (`download_post_handler`) and `:932` (`check_post_handler`).
  - Untested responses: `bad_confirm`/`bad_query` 400, `fetch_busy` 409 (`:950`, `:1004`), and `clock_not_synced` 409 (`:960`, `:1008`).
  - The mode-gate refusal must also reset `busy`.
- `update_fetch.c:1021`: `cancel_post_handler`.
- `update_http.c:314` (`stage_upload_post_handler`), `:426` (`stage_clear_post_handler`), `:192` (`claim_refuses`).
  - Missing tests: the claim, mode-gate and interlock ordering at run time (the lint checks only the text order), and the abort-on-error path of a multipart upload.

Risk: a valid-looking stage left behind after a failed or cancelled fetch, or an update claim that leaks and silently disables firing. These are the paths every recent review touched.

Suggested route: a `test_update_fetch.c` that `#include`s `update_fetch.c`. It needs stubs for these:

- `esp_http_client`
- `psa_hash`
- `xTaskCreate*`
- `ota_http_update_begin`/`ota_http_update_end`

It should drive `run_job()`/`fetch_task()` synchronously, with the writer replaced by a direct call or a controllable fake.

### 2. ADDRESSED: the aux fault-drop was never driven through the executor task loop

**Addressed:** `test_aux_fault_drop_via_task_tick()` in `test_profile_executor_prestart.c` drives `executor_task_entry()` with a controllable `safety_link_get_status()` stub. Negative-tested with `tools/negtest.ps1`: deleting the call and inverting the stale check are both CAUGHT. The gap text below is the original finding.

`profile_executor_aux_fault_drop()` (`drivers/control/profile_executor_relay_io.c:598`) is well tested by calling it directly in `test_profile_executor_prestart.c` (around lines 10113-10240). Covered cases:

- F1: PAUSED
- F2: idle manual output plus a Pico trip
- LOW-2: a failed write is retried
- M1: disabled, raw spare and zone relays are excluded
- L1: a failed write is not counted

The production caller is not covered:

- `profile_executor.c:768` makes the call from the not-RUNNING branch of the task.
- `pre_lock_pico_tripped` is derived at `profile_executor.c:727-734` as `diag_ever_received && !stale(age_ms, SAFETY_LINK_STALE_MS) && diag_state == TRIPPED`.

No test reaches the derivation through `aux_test_run_task_ticks()` (test around line 10805), which only the `aux_off_pending` tests use. No test or check references `pre_lock_pico_tripped`.

Deleting the call, inverting the stale check, or using a stale snapshot as tripped would pass every host test.

Missing test: idle executor, aux output ON, safety snapshot reporting a fresh TRIPPED. After one task tick through the real loop, assert the aux relay is OFF. Repeat with these snapshots and assert the aux relay stays ON:

- a stale snapshot (`age_ms > SAFETY_LINK_STALE_MS`);
- one with `diag_ever_received=false`;
- one that is not tripped.

### 3. FIXED: the zones 409 lost-update leaves the Pico ceiling raised (defect plus gap)

**Fixed (2026-10-09):** the 409 path now snapshots the live zone maxima under the lock and calls `zones_post_track_ceiling_lower()` (the same helper as the post-commit lower), which lowers the Pico back to exactly the live maximum. `test_zones_post_refuses_lost_update_on_concurrent_generation_bump` now asserts the second write targets the live max and the cached Pico ceiling reads it back. Negative-tested with `tools/negtest.ps1` (restore call removed: CAUGHT).

Original finding (MEDIUM):

This is a code defect, not only a missing test. I confirmed it by reading the code; it has not been reproduced on the board.

In `drivers/http/zones_http_post.c`:

- `:616`/`:619` raise the Pico `abs_max_temp_c` via `safety_ceiling_sync_guard_raise()` before the commit.
- The lost-update re-check at `:663-672` then answers 409 `zones_config_changed_concurrently` and returns, leaving the live config unchanged.
- `safety_ceiling_sync_apply_lower()` runs only on the post-commit path (`:732`), so the raise is never undone.

The consequences follow from `drivers/safety/safety_ceiling_sync.c`:

- `enforce_ceiling_divergence()` compares the ESP target with the Pico ceiling by exact normalized equality (`config_divergence.c:106-108`).
- On divergence it forces all relays off and halts any run (`safety_ceiling_sync.c` around lines 566-575).
- The link-up reconcile only ever raises (`reconcile_on_link_up_impl` around line 882).

So after this 409 the Pico stays above the ESP max. Heat is disabled with an ALARM log until the next successful zones save. The direction is fail-safe (heat off), but it is a silent loss of availability that the operator did not cause.

Backup import handles the same situation by calling `backup_import_track_ceiling_lower()` (`backup_import.c:2952`) on every exit.

The test `test_zones_post_refuses_lost_update_on_concurrent_generation_bump` (`test/test_zones_http.c:1777`) deliberately drives this exact path: it raises the ceiling, then bumps the generation. It asserts the 409, the untouched config and the released guard, but it does not assert the Pico ceiling afterwards.

- Missing assertion: after the 409, the ceiling writer was called again with the old max, or the divergence latch reads clear.
- Fix direction: call the same lower-to-live-max helper on the 409 path.

### 4. MEDIUM: the wifi legacy migration failure branches are untested (ADDRESSED)

**Addressed 2026-10-09:** `test_wifi_prov.c` `test_legacy_migration_failures_never_lose_credential` (save failure, lossy read-back mismatch, legacy erase failure; credential never lost). Negative-tested: erase-legacy-despite-failed-save is CAUGHT.

In `drivers/net/wifi_prov_nvs.c`, legacy keys are erased only after a verified copy:

- first stage: `:595-611`;
- `nvs_load_saved_nets()`: `:627-666`.

`test_wifi_prov.c` covers these cases:

- the one-shot erase (around line 614);
- an interrupted migration (around line 631), simulated by clearing `s_legacy_erase_pending` by hand;
- forgetting the last network (around line 655);
- a mode change surviving (around line 668).

It never uses the `fake_kv` write-fail injection (`firmware/hwAbstraction/host/fake_kv.c:73-88`). So these branches are never reached:

- `save_err != ESP_OK` (`:608`, `:641-645`);
- a read-back mismatch (`:603-606`, `:647-653`);
- a `legacy_default_nvs_erase_wifi()` failure (`:661-664`).

Missing test: arm a write failure on the `saved_nets` save. Assert that:

- the legacy keys are still present;
- the in-RAM list still holds the network;
- a second boot (re-running the load) completes the migration and only then erases.

Repeat with a lossy write (`s_silent_set_noops`) for the read-back path.

Risk: loss of the Wi-Fi credential. A board that loses it falls back to AP mode and needs a site visit.

### 5. MEDIUM: the CSRF pre-handler wiring has no host test (ADDRESSED)

**Addressed 2026-10-09:** auth pre-handler wiring in `test_readiness_crash_disclosure.c` `test_csrf_prehandler_wiring` (cross-origin POST 403 and handler not run, same-origin passes, foreign-Origin GET passes). The recovery image has no host harness, so `tools/check_csrf_origin_wiring.ps1` checks the `origin_guard` wiring at source level instead. Negative-tested: dropped cross-origin check CAUGHT; unwrapped recovery routes and a guard that drops the check both CAUGHT by the script.

The decision helpers are tested: `http_origin_is_cross_origin` and `http_origin_request_is_cross_origin` (`test/test_http_auth_enforce.c:614`, `:637`).

The places that apply them are not:

- `drivers/http/http_auth_http.c:312-320` refuses a cross-origin request whose method is not GET or HEAD, before any handler work.
- In `firmware/KilnFW_recovery/main/recovery_http.c`, `origin_guard` (`:254`) is wrapped around every route at `:1046`.

Removing the `request_is_cross_origin()` call, or widening the method check, passes every test.

Missing tests:

- through the auth pre-handler, a cross-origin POST gets 403 and the handler is not called;
- a same-origin POST passes;
- a GET with a foreign Origin passes;
- the same three cases through the recovery `origin_guard` wrapper.

This needs header injection in the `esp_http_server` host stub.

### 6. LOW-MEDIUM: backup import commit-phase preference setter failure (ADDRESSED)

**Addressed 2026-10-09:** `test_backup_import.c` `test_prefs_commit_failure_is_partial_write_profiles_untouched` (tz, hidden-builtin and relay-name setter failures; partial write, error text, no profile saved). Negative-tested: swallowed tz failure CAUGHT.

`drivers/http/backup_import.c` (around lines 3296-3330) returns `"a preference could not be persisted -- the rest of the restore already landed"` (`:3329`) when one of these commit-time setters fails:

- `unit_pref_set`
- `ramp_assist_cfg_set_enabled`
- `display_power_cfg_set`
- `profiles_builtin_set_hidden`
- `time_sync_set_tz`
- `zones_config_set_relay_name`/`zones_config_set_relay_device_type`

That result becomes a partial write (500), and profiles are not written.

`test_backup_import.c` covers the matching failures for:

- aux (around lines 6540 and 6568)
- update_repo (around line 6036)
- relay_cycles (around line 6717)
- invalid preferences in pass 1 (around line 6807)

No test fails a preference setter at commit time.

Missing test: inject a failure into one setter. Assert the 500, `partial_write`, that profiles are untouched, and the error text.

### 7. LOW (ADDRESSED)

**Partly addressed 2026-10-09:** the `erase_keys()` mid-loop erase and commit failures (`test_ota_http.c` `test_legacy_erase_mid_loop_and_commit_failures_are_reported`) and the `rev_repair_junk` PSRAM refusal (`test_profiles_http.c` `test_pcfg_junk_rev_repair_refuses_on_external_ram_stack`) are covered and negative-tested (commit-swallow and guard-removal both CAUGHT). Still open: the `backup_import.c` PSRAM candidate-buffer allocation failures and the truncation/`thermo_count` cases (bullets 3 and 4); closing them needs edits near backup_import.c savers, which another agent owns.

- `drivers/persist/legacy_default_nvs.c:41`/`:49`: in `erase_keys()`, a `hal_kv_erase_key` failure in mid-loop, or a `hal_kv_commit` failure, is not tested. An open failure is tested (`test_ota_http.c` around line 1507). Factory reset would report success or failure according to this path.
- `drivers/http/profiles_http.c`: the `caller_stack_is_external()` PSRAM refusal in `rev_repair_junk` (commit `7d155f5a`) has no test. On the target it is a defensive refusal only.
- `drivers/http/backup_import.c` (around lines 3884-3920): the PSRAM allocation failures for candidate buffers are untested. They fail closed before any write.
- `backup_import.c` changes in `7e08005d` (L6 `thermo_count`, rename of `apply_two_pass`) and `7716998e` (truncation/malloc) were not accompanied by test changes. The existing suite exercises the renamed path, but no test specifically covers the truncation cases.

## Areas checked and found covered

- Profiles rev floor and junk repair (`profiles_http.c`). Every fix commit since 2026-10-04 added tests in `test_profiles_http.c`, including:
  - review 8 L3/L4;
  - OOM fallback;
  - a cfg read error failing closed;
  - unknown-floor recovery;
  - preserving the newer-firmware `prof_rev` tail.
- The aux outputs store (`aux_outputs_cfg.c`). `test_aux_outputs_store.c` covers:
  - F3: RAM unchanged after a failed save (around lines 507-513);
  - LOW-3: loading into locals;
  - the quarantine paths.
- The fault-drop function itself (see gap 2 for its caller).
- Factory reset legacy erase per scope: kiln, profiles, wifi and all (`test_ota_http.c` around line 1480), and an open failure (around line 1507).
- relay_cycles and run_state migration erase-after-verified-copy (`test_relay_cycles.c`, `test_run_state.c`).
- The zones generation re-check: the 409, the untouched config, the guard release and (since the gap 3 fix) the Pico ceiling restore (`test_zones_http.c:1777`).
- The core update modules: stage, policy, settings, url, release, fetch_heap, and the writer/timeout arbiter `update_wr_arb.h` (through `test_update_stage.c`).
- Backup import:
  - aux phase-1 revert;
  - aux, update_repo and relay_cycles persist failures;
  - the hidden mask;
  - an oversized catalogue;
  - invalid preferences.
