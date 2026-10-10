# Unchecked persist result audit, 2026-10-09

Scope: the "logging unchecked success" class named in CLAUDE.md, in
`firmware/KilnFW/App/**` and `firmware/SaftyFW/src/**`, audited at origin/dev
`48e1ba8a`. A hit is an NVS, cfg-fs or flash write whose return code is ignored,
or a success that is reported, logged or returned over HTTP without checking the
result or reading it back.

Audit only. No code was changed.

Prior work this builds on, and does not repeat:
`docs/audits/persist_save_logging_2026-09-07.md` (fixed zone_normals and
relay_names logging, widened `check_safety_call_results_checked.ps1`) and
`docs/audits/NVS_WRITER_INVENTORY_2026-10-09.md` (call-site table of every NVS
writer).

## Method

1. Raw writers. Grep for bare or `(void)`-cast calls to `nvs_set_*`, `nvs_commit`,
   `hal_kv_set_*`, `hal_kv_commit`, `hal_kv_erase_*`, `cfg_fs_write_atomic`,
   `cfg_fs_delete`, `pref_cfg_fs_commit`, `fwrite`, `rename`, `fclose`,
   `lfs_*`, `hal_flash_erase`, `hal_flash_program`. The only hit was
   `boot_guard.c:244` (L8). Every other raw call captures its result.
2. Store setters. Grep for statement-position calls (result discarded) of every
   persisting setter: `zones_config_set_*`, `zone_*_set`, `zone_normals_set`,
   `unit_pref_set`, `watchdog_cfg_set_*`, `relay_cycles_*`, `firing_stats_persist`,
   `firing_shadow_store_persist`, `safety_cfg_store_*`, `kiln_cfg_store_*`,
   `profiles_*_set`, `live_profile_*`, `totp_config_*`, `wifi_prov_*`,
   `touch_cal_store_*`, `boot_guard_*`, `pico_update_attempts_*`, `update_stage_*`,
   `ota_record_append`, and others. Each hit was read in context.
3. Every `pref_cfg_fs_commit()` caller was read, to check that the rev advances and
   RAM is published only on success, and that the error is returned.
4. HTTP and LCD handlers that call a persisting setter were checked for answering
   200/"ok" before the persist result is known.
5. Flash-worker jobs (`uart_bridge_ext_run_on_flash_worker`) were checked for a
   `job.result` that is never surfaced.
6. SaftyFW: `config_store_flash.c` and `tasks/update_task.c` flash program/erase
   paths were checked, and the D2 atomicity defect was re-checked against
   `docs/CONFIG_FILESYSTEM.md`.

## Summary

| Severity | Count |
|---|---|
| H | 0 |
| M | 2 |
| L | 9 |

No finding lets a safety value be silently lost. Both M findings are in the kiln
config swap journal. The safety-relevant one (M2) is backstopped by the standing
divergence check.

## Findings

### M1. Kiln swap: active_id persist failure is ignored at boot and logged only at runtime (user-data-loss)

- `firmware/KilnFW/App/drivers/persist/kiln_cfg_swap.c:890`. In the boot
  ESP_DONE recovery path, the return of
  `kiln_cfg_store_set_active_id_raw(p->target_id, sub, sizeof(sub))` is discarded.
  `clear_pending()` and a "finished" log follow, and the failure is not even
  logged here.
- `kiln_cfg_swap.c:723`. In the normal swap path, `finalized` is checked but only
  `ESP_LOGE`'d. The swap is still reported as a success.
- `kiln_cfg_store.c:1968`: `kiln_cfg_store_set_active_id_raw()` sets the RAM
  `active_id` first and returns `fail_persist(... " -- set live only")` on an NVS
  failure.

Failure scenario: a swap from kiln A to kiln B lands and verifies on both sides,
but the active_id NVS write fails. The pending record is then cleared. After the
next reboot:

- The persisted active_id is still A, while the live zones config and the Pico
  values are B's.
- The kiln-config page names the wrong kiln.
- Later autosaves write B's live settings into A's saved slot. This is the same
  overwrite-the-outgoing-kiln class as the earlier Defect 1.

Because the journal is already cleared, nothing retries.

Fix direction: on a failed active_id persist, keep the pending record at
ESP_DONE so the next boot retries `finish_esp_done_impl`. Surface the failure in
the apply status, not just in the log.

### M2. Kiln swap: the PICO_OPEN / PICO_DONE / ESP_DONE marker persists are unchecked (safety-relevant, backstopped)

- `kiln_cfg_swap.c:612` (`persist_marker(p, KILN_CFG_SWAP_MARKER_PICO_OPEN)`),
  `:634` (PICO_DONE) and `:683` (ESP_DONE) all discard the bool.
  `persist_marker()` (`:165`) does `ESP_LOGE` and return false on failure.
- Only the STAGED write at `:573` is checked; a failure there refuses the swap.
- Boot recovery (`:989`) treats a STAGED record as "crashed before the Pico was
  touched" and only runs `clear_pending()`.

Failure scenario:

1. The PICO_OPEN marker write fails, and the record still reads STAGED.
2. Step 6 installs B's values on the Pico as volatile.
3. The ESP crashes or resets while the Pico keeps running.
4. At boot, the ESP discards the record as STAGED and does not re-apply R. The
   ESP is on R, but the Pico still holds B's volatile values.

The standing divergence check (`safety_ceiling_sync.c`) detects the mismatch and
raises an alarm, so this is not silent. The automatic re-apply-R recovery that
PICO_OPEN exists to trigger is skipped, and the operator must resolve it by hand.

A failed PICO_DONE or ESP_DONE write leaves an older marker in place. Boot then
takes the more conservative re-apply or rollback branch, which is safe.

A related note, independent of persist failure: the step-4 raise-first ceiling
write (`:600-607`, volatile) already touches the Pico before the PICO_OPEN marker
is written. The STAGED comment at `:990` ("both sides are still on R (nothing was
written)") is therefore slightly inaccurate. The only change in that window is a
ceiling that is the same or looser, which the "abs_max same or looser" rule
permits, so this is a doc fix.

Fix direction: check the result at `:612`. On failure, refuse before
`push_and_verify_pico()`; nothing but the raised ceiling has changed yet. Then
correct the STAGED comment.

### L1. Retry after a failed save short-circuits to ESP_OK: favorites and hidden builtins (user-data-loss) -- FIXED in 65c4f5da

- `firmware/KilnFW/App/drivers/persist/profiles_favorites.c:263`
  (`profiles_favorites_set`): the `if (!changed) return ESP_OK;` comparison runs
  against RAM, which was already flipped by the earlier, failed call.
- `profiles_builtin.c:310` (`profiles_builtin_set_hidden`,
  `if (updated == s_hidden_mask) return ESP_OK`) and `:323`
  (`profiles_builtin_restore_all`, `s_hidden_mask == 0`) work the same way.

Failure scenario: the first save fails, and the caller correctly sees and
reports the error. RAM keeps the new value. The user retries, the setter finds
"no change", returns ESP_OK and writes nothing. The UI shows success and the
change is lost at reboot.

`update_settings.c` already solves this with an `s_persist_dirty` flag. Use the
same pattern here, or roll RAM back on a failed save.

### L2. Current sweep: ESP-side CT provenance writes are unchecked (cosmetic / provenance)

`firmware/KilnFW/App/drivers/control/zones_current_sweep_task.c` discards these
results:

- `:265` and `:383`: `zone_normals_set(...)`
- `:718`: `zone_ct_map_set(...)`
- `:1252` and `:1709`: `(void)zone_k_ct_set(...)`
- `:1855-1856`: `zone_ct_map_clear()` and `zone_k_ct_clear()` (void; failure
  swallowed in `zones_config_store.c:1663/1692`)

`pref_cfg_fs_commit()` logs each failure with `ESP_LOGE`, but the sweep result
still reports `k_ct_derived_mask` and the derived map as a success. The Pico holds
the values that matter (pushed through COMMIT_CONFIG with read-back).

The i_normal_a rescale at `:1730` already surfaces the same failure as "WARNING:
ESP save failed, reboot first", so the other sites are inconsistent with it.

Failure scenario:

- After reboot, the ESP's record of the derived map and k_ct is stale or missing,
  so the zones page and the backup export show stale provenance.
- If the clear at the start of a sweep fails, the old channel claims come back
  after reboot.
- `zone_sweep_plan_i_normal()` reads the ESP record, so a later partial re-sweep
  pushes only what that record still holds. The Pico keeps its own values.

### L3. firing_stats_persist() ignores firing_stats_load()'s result before overwriting (user-data-loss) -- FIXED in 65c4f5da

`firmware/KilnFW/App/drivers/control/profile_executor_firing_stats.c:810`
discards `firing_stats_load()`'s bool: "empty blob on any failure -- still safe
to prepend into". The prepended one-entry blob is then written with a higher rev.

The layout-change case is already documented (`profile_executor_internal.h:1040-1060`).
Two other cases are not:

- a transient `heap_caps_malloc` failure inside `firing_stats_load()` (`:574`)
- an unreadable or corrupt record

Either one silently replaces a profile's whole firing-history ring, which is
adaptive-tune training data, with a single entry.

Fix direction: on a false return from the load, skip the persist, or refuse it
unless the record is genuinely absent.

### L4. SaftyFW update metadata write: return code checked, but no read-back or erased-slot check (cosmetic / update reliability) -- FIXED (see Fix status below)

**FIXED 2026-10-09:** `update_task_metadata_write_verified()` (`update_task_metadata_write.c`, host-tested by `test_update_task_metadata_write.c`) adds the erased-slot check and byte-for-byte read-back; `update_metadata_write_cb` maps any failure to `HAL_IO`, so no COMPLETE is reported.

`firmware/SaftyFW/src/tasks/update_task.c:534-549` (`update_metadata_write_cb`)
and `:584-590` check `hal_flash_safe_execute()` and `args.result`. Unlike the D2
fix in `config_store_flash.c` (`:1272` erased-slot check, `:1316-1329` read-back),
this path never verifies the slot was erased and never reads the record back.
`hal_flash_program()` AND-programs, so a torn leftover slot silently corrupts the
new record while still returning `HAL_OK`.

Failure scenario:

1. The UPDATE_END PENDING_VERIFY record lands corrupted, and the update reports
   COMPLETE.
2. The bootloader's metadata CRC rejects the record and falls back to the
   previous one. The old image keeps booting.
3. The ESP's post-update version check sees the old version. The failure is
   visible, but it looks like an update that did not take rather than a write
   failure.

The image chunks themselves are covered by the whole-image CRC read-back at
`:1169`.

### L5. Autotune coupling-cell persist failure is log-only (cosmetic) -- FIXED in 65c4f5da

`firmware/KilnFW/App/drivers/control/autotune_engine_coupling.c:63-67` logs
`ESP_LOGW` for a failed submit or for "N of M coupling cell(s) failed to persist".
The autotune run still reports success. RAM holds the cells until reboot, then
they revert. `control_get_zones` shows the cells until that reboot.

### L6. Kiln swap save_pending() has no read-back (cosmetic)

`kiln_cfg_swap.c:130-151` checks `hal_kv_set_blob` and `hal_kv_commit` return
codes but never reads back. boot_guard's 2026-09-08 failure was exactly an NVS
write reporting `HAL_OK` while the value never landed. A journal record that
lies would undermine M2's recovery further. The defence is worth the same
read-back discipline `totp_config_clear()` and `boot_guard_mark_healthy()` use.

### L7. UART SET_WATCHDOG_PANIC_DISABLED discards the persist result (cosmetic) -- FIXED in 65c4f5da

`firmware/KilnFW/App/drivers/bridge/uart_bridge_system.c:151` drops the
`esp_err_t` from `watchdog_cfg_set_panic_disabled()` and sends no reply. The
setter logs `persist=` itself, and the web path (`diagnostics_http.c:775`)
surfaces it. Over UART, a PC tool cannot tell that the flag will not survive a
reboot. The hazard is mild: the default after a reboot is panic enabled, which
is the safe default.

### L8. boot_guard legacy-key erase commit unchecked (cosmetic) -- FIXED in 65c4f5da

`firmware/KilnFW/App/drivers/persist/boot_guard.c:244` uses
`(void)hal_kv_commit(&lh)` after erasing the legacy `count` key once its value
has been migrated. If it fails, the legacy key survives, and the next boot finds
it and migrates again. The migration is idempotent. Adding a one-line log would
make this visible.

### L9. Abandoned fetch: update_stage_clear() result ignored (cosmetic) -- FIXED in 65c4f5da

`firmware/KilnFW/App/drivers/update/update_fetch.c:389` calls
`(void)update_stage_clear(st)` when a late WR_FINISH lands after the job was
already reported FAILED. If the clear fails, a valid staged image (sha-verified)
is left behind for a fetch the user saw fail. It is a real image, so this is not
a safety issue, but `GET /api/update/status` then disagrees with the job result.
Log the failure.

## Not findings: checked and justified

- `zones_config_store.c:727/731` `(void)zones_config_persist_migrated_blob_verified(...)`:
  the function latches a migration persist fault that the dashboard and LCD
  display.
- `safety_cfg_http.c:922/2365` `safety_cfg_store_set_rate_guard_meta(..., &nvs_err)`:
  the provenance label only. The setter logs `ESP_LOGE`, and the comment at
  `:2357-2361` documents that a failure costs a UI label, never the safety value.
  Same for `:2012` `safety_cfg_store_clear_rate_guard_meta()`.
- `ota_record_append()` at `ota_http_esp.c:425/800`, `ota_http_pico.c:573`,
  `ota_http_recovery.c:337` and `ota_pico_relay.c:700`: best-effort history that
  logs its own failure, by contract in `ota_record.h`.
- `profiles_http.c:1822` and `profiles_edit_http.c:782`
  `(void)profiles_favorites_set(id, false)`: favorite cleanup on profile delete.
  The profile delete itself is checked.
- `security_http.c:335` `totp_config_clear()` after a failed enroll persist: the
  response is already `{"ok":false}`, and `totp_config_clear()` verifies its own
  read-back.
- `safety_link_poll.c:170` `(void)safety_cfg_store_maybe_refetch()`: a read, not a
  write.
- `pico_auto_update_boot.c:474` `(void)pico_update_attempts_next_slot()`: a slot
  choice with a documented default. The counter writes (`:151`, `:486`) are
  checked.
- SaftyFW `(void)hal_scratch_write_u32/clear` in `boot_reason.c:53-59`,
  `clear_trip_diag.c:27/49`, `main.c:593-594` and
  `watchdog_overdue_diag.c:89/123/144`: RAM scratch registers, diagnostic only,
  not flash.
- `adaptive_tune.c:624`: `++s_kibase_rev` advances even when the commit fails.
  This leaves only a rev gap, which is harmless. `job.result` is checked at
  `:858`.

## Modules checked and found clean

KilnFW:

- `persist/cfg_fs.c`: `cfg_fs_write_atomic` does write, fflush, fsync, fclose,
  rename and a strict read-back, and every failure returns ESP_FAIL.
  `ensure_dir()` is ignored, but a failure makes the following fopen fail anyway.
  `cfg_fs_delete` is clean.
- `persist/pref_cfg_fs.c`: `pref_cfg_fs_commit` logs `ESP_LOGE` and returns the
  error. Clear is idempotent.
- These `pref_cfg_fs_commit` callers advance the rev only on success and return
  the error:
  - `ramp_assist_cfg.c`, `time_sync.c`
  - `aux_outputs_cfg.c` (RAM committed only on success)
  - `ct_verify_store.c`, `display_power_cfg.c`, `iter_tune_store.c`
  - `profiles_builtin.c:115`, `profiles_favorites.c:275`
  - `setup_wizard_progress.c`
  - `zones_config_store.c:1324/1506`
  - `update_settings.c` (with `s_persist_dirty`)
  - `relay_cycles.c:748`
  - `live_profile.c:457/514` (plus read-back)
  - `adaptive_tune.c` kibase
- cfg deletes in `kiln_scope_cfg_files.c` and `profiles_scope_cfg_files.c` record
  the first error and return it.
- `relay_cycles.c`: `relay_cycles_maybe_persist` keeps the counts dirty and
  retries. `relay_cycles_flush` returns the error.
- `profile_executor_firing_stats.c`: the file write itself is checked, and the
  cache is updated only on success (see L3 for the load side).
- `zones_http_pid.c:164`: `zones_config_set_pid` false is surfaced.
- `adaptive_tune.c:1245`: model and PID set are checked.
- `diagnostics_http.c`: the cfgfs file POST answers ok only on `job.result ==
  ESP_OK`. The watchdog POST (`:775`) surfaces the error.
- `uart_bridge_ext_control.c:177`: `unit_pref_set` checked.
- LCD: `ui_page_config.c:150` (unit), `ui_page_network.c:252/323/328` (Wi-Fi
  mode, AP identity), `ui_page_network_manage.c:293` (forget),
  `ui_page_profile_picker.c:219`, `ui_page_touch_cal.c:185`, and
  `ui_page_profile_builder_review.c` (`profiles_http_save` false shows "Cannot
  Save").
- `security_http.c:332`: TOTP enroll persist is checked, and the HTTP response
  reflects it.
- `pico_auto_update_boot.c`: the attempt and clear counters are checked.
- `kiln_cfg_swap.c`: the STAGED marker (`:573`), the rollback active_id (`:423`)
  and the swap content writes are checked (see M1, M2 and L6 for the exceptions).

SaftyFW:

- `config_store_flash.c`: the D2 atomicity defect stays fixed at `48e1ba8a`.
  `config_store_flash_slot_is_erased()` (`:1272`) is checked before programming
  (`:1482`). `config_store_write_cb()` checks the erase result before
  programming (`:1292-1295`) and does a byte-for-byte read-back after every
  program (`:1316-1329`), mapping a mismatch to `HAL_IO`. This matches
  `docs/CONFIG_FILESYSTEM.md` (2026-09-14 fix, bench-verified at `88bb4333`).
  CLAUDE.md's "real unfixed atomicity defect" wording in its cfg-partition note is
  stale on this point.
- `tasks/update_task.c`: image chunk program and erase results are checked, and
  the whole image is CRC-read-back at UPDATE_END (`:1169`). The metadata write is
  covered in L4.

## Recommended follow-ups, in priority order

1. M1 and M2, `kiln_cfg_swap.c`. Keep the journal open on a failed active_id
   persist, and refuse the swap when the PICO_OPEN marker fails to persist.
2. L1: dirty-flag retry in `profiles_favorites_set` and
   `profiles_builtin_set_hidden/restore_all`.
3. L3: do not overwrite the firing history after a failed load.
4. L4: give the SaftyFW metadata write the same erased-slot check and read-back
   as config_store.
5. The remaining L findings are cosmetic. Fix them opportunistically.
