# Dev firmware review 10 (2026-10-09)

Reviewer: Opus agent. Read-only review. No code was changed and no board was touched.
Line numbers refer to origin/dev at `937af9cc`/`f354981b`.

## Scope

This review covers firmware commits on origin/dev since origin/main `129586d4` that no other review doc covers:

- `d7d11578`: factory_reset refuses during a sweep or restore, guards the pref writers, adds a reboot-failure path.
- `6fbbb28b`: kiln_cfg_swap rollback keeps the journal when the active_id restore fails.
- `2cb5499b`: recovery image clears boot_guard before erase/select, plus the stall deadline and stage clear.
- `b9952996`: HTTP LOWs (custom_err 503, recovery stall is 400, shared zones OOM constant).
- `82f1dafc`: XSS escaping at the audited innerHTML sinks, and loginReturnPath.
- `72555993`: drop the USB-serial path. Firmware changes are comment-only.
- `937af9cc`: swap fault kind buffer 24 -> 32, and the unit_pref `adopted` flag.
- `f354981b`: update fetch heap precheck set to 29556 B.

Skipped because another agent is reviewing them: `f8dffd8d`, `7452e63b`, `c094c089`. Skipped because reverted by `cececc45`, which review 9 covered: `0a1cdc7d`.

Already covered elsewhere, so not repeated here:

- `4e3282fb` and `b813b027` are covered by `DEV_FIRMWARE_REVIEW_9_2026-10-09.md` "Fix review" (`d4562124`). This review found the same two issues independently:
  - The Accept vs run-end apply TOCTOU is F1 there.
  - The BUSY refusals reported as generic failures, and the half-applied model/PID pair, are F3 there.
  - Accept's later unchecked `zones_config_set_model()` (`control/autotune_engine_guard.c` ~:569) can land after the adaptive writes. The zone then holds adaptive PID gains with the autotune model while Accept reports success. That is a wider form of F1.
- `3ade435b`, `aba69118`, `b744de75` and `762a1f2a` are covered by `DEV_WEBFIX_REVIEW_2026-10-09.md` (`4b153a18`).
  - Its CRITICAL-1 is still present at this tip. `drivers/http/auth_totp_http.c:351-352` in `forgot_post_handler()` uses `RESET_BODY_MAX`, which is not defined until :444, and calls `free()` on the stack array `body[FORGOT_BODY_MAX]`. The target build breaks.
  - Its HIGH-1 is also still present. The read-failure return in `reset_post_handler()` (:473-476) leaks the 512 B heap body and does not zero it.

## Findings

### LOW-1: a kept swap journal re-imports the rollback blob on every boot and silently reverts zone edits (`6fbbb28b`)

**FIXED in 462eb838:** refused POST /api/zones (409, names the pending rollback) while a kept rollback journal would re-import the pre-swap zones; banner and id note now say saved edits are at risk; host tests in test_zones_http.c and test_kiln_cfg_swap.c.

Location: `drivers/persist/kiln_cfg_swap.c:510-527`, `:556-567` and `:1343-1358`.

When `kiln_cfg_store_set_active_id_raw()` fails during a rollback, `rollback_ex()` now keeps the journal. The record's marker is PICO_DONE, or ESP_DONE via the fallback path at :1123. Each later boot runs `kiln_cfg_swap_boot_recover_impl()` -> `rollback_ex(..., esp_was_committed=true)`. That path calls `zones_config_import_blob(p->rollback_blob, ...)` unconditionally (:520), and then retries the id.

Failure scenario:

1. The active_id write fails persistently (for example, a kiln_cfg store write fault).
2. The operator edits zones through `POST /api/zones`. The edit persists normally, because only kiln-config autosave is suppressed.
3. At the next boot, the edit is overwritten by the pre-swap blob. This repeats on every boot until the id write succeeds.

The latched boot-fault text (:1356) says only that "zone edits are not auto-saved until it is retried at the next boot". `KILN_CFG_SWAP_ROLLBACK_ID_NOTE` (:106) says only "journal kept, next boot retries". Neither tells the operator that saved zone edits will be reverted.

The UNCLEARED note (:104) does warn about this effect. The id-failure note and banner should too. A better fix is to skip the re-import when the live blob already matches the rollback blob or differs from it on purpose. The `esp_on_r` comparison at :1152 already has that shape.

No test covers a zones edit made between the kept rollback and the next boot.

### LOW-2: update fetch precheck sits only 91 B below the observed idle minimum, and its comments are damaged (`f354981b`)

**FIXED in 462eb838:** comments repaired (merged line, 17524 B); 91 B margin and pending bench re-measure noted; 29556 unchanged.

Location: `drivers/update/update_fetch_heap.h:40` and `:75`.

`FETCH_HEAP_PRECHECK_MIN` is 29556 B. The idle minimum recorded in `logs/sk04_sampling` is 29647 B. Any internal-DRAM growth over about 91 B makes `update_check` refuse on an idle board again, which is the bench failure this commit fixed. An unexplained ~30 kB internal DRAM growth is already under investigation. There is no check that ties this constant to a measured idle figure. Gate (b), the bench run, is the only confirmation.

Two comment defects:

- At :40 a missing newline merges two comment lines: `...with scratch 1024.// The slack is deliberately NOT larger...`.
- At :75 the comment still says the full worst draw is `(18548 B)`. It is now 17524 B.

Both are cosmetic, but this header is the record of how the budget was derived.

### LOW-3: two pref writers change RAM before the new reset-mark refusal, so live state diverges after a refused reset (`d7d11578`)

**FIXED in 462eb838:** both writers refuse on reset-in-flight before changing RAM; host tests in test_ramp_assist_cfg.c and test_setup_wizard_progress.c.

Location: `drivers/control/ramp_assist_cfg.c:157-166` and `drivers/persist/setup_wizard_progress.c:632-641` (via `persist_all()` :578-585).

`ramp_assist_cfg_set()` assigns `s_ramp_assist_enabled` before the new `relay_authority_reset_in_flight()` check. `setup_wizard_progress_set_step()` writes `s_steps[]` before `persist_all()` checks the mark. If the mark is set, the save is refused with `ESP_ERR_INVALID_STATE` and the caller reports an error, but RAM already holds the new value.

This normally does not matter, because the reset erases storage and reboots. It does matter when `execute_scope()` sets the mark and then refuses at its re-check (`factory_reset.c:492-496`, a run, sweep or restore started meanwhile). In that case the mark is cleared with nothing erased and no reboot, and the board keeps a RAM value that the HTTP response called unsaved. The live value then differs from the file until the next boot.

This is the same RAM-first pattern these writers already had for an ordinary commit failure, but the commit adds a new path into it. The other guarded writers (aux_outputs_cfg, iter_tune_store, display_power_cfg, update_settings, zones coupling) refuse before touching RAM or commit RAM only on success.

### INFO

- `937af9cc`: the `_Static_assert` in `dashboard_http.c` compares `kiln_cfg_swap_boot_fault_kind[]` against `KILN_CFG_SWAP_BOOT_FAULT_KIND_NAME_MAX`, which is a constant the header declares. It does not compare against the real name lengths. Only the commit's host test ties the names to it. The longest name today is `rollback_active_id_unsaved` (27 B with the NUL), so it fits.
- `937af9cc`: the new `"adopted"` field in the unit_pref 500 JSON is not read by any web page or PcTools client. Adding the field is harmless. The UI still shows a generic save failure.
- `d7d11578`: `/api/status` gains `factory_reset_in_flight`. It is additive.
- `d7d11578`: on `FACTORY_RESET_ERR_REBOOT_FAILED`, `factory_reset_reboot_fallback()` runs `vTaskDelay(500)` and then `hal_wdt_reboot()` on the httpd task or UART bridge task. This is acceptable, because the board is rebooting.

## Checked, no defect

- `d7d11578`, sweep vs reset pairing: the sweep publishes `s_sweep.active` and the sweep heat claim, then reads the reset mark. The reset sets the mark, then reads `relay_authority_heat_sweep_active()`. Both use the leaf spinlock (`zones_current_sweep_task.c` ~:2444-2460, `factory_reset.c:445-497`), so at least one side refuses.
- `d7d11578`, restore vs reset pairing: `backup_import_job_recheck_refused()` reads the mark after `s_backup_restore_in_flight` is published (`backup_import.c` ~:4047-4059). The reset reads that flag after setting its mark. The pairing is correct.
- `d7d11578`, writer barrier: the reset takes and gives each save lock once with no nesting, and skips the barrier on the flash worker. This is consistent with the `cfg_save_lock.h` order.
- `2cb5499b`: boot_guard is cleared before `recovery_boot_partition_set_and_verify()` and before the first `app` erase. A failed clear refuses with no set_boot. Treating an unavailable kiln_nvs as "not applicable" matches the application. Opening the namespace READONLY first avoids creating it on a full partition. `body[224]` holds the longest message (about 198 B). The stall deadline is wall-clock based.
- `b9952996`: `httpd_resp_send_custom_err()` gives the 503 status line. The upload-too-slow callers still return through `ota_http_refusal_drain()`, which is time-capped (`OTA_REFUSAL_DRAIN_CAP_MS`) and returns ESP_FAIL (close) on cap, so dropping the ignored `Connection: close` does not hold the httpd task. A recovery stall now returns 400 rather than 504, and the check script pins this.
- `82f1dafc`: every changed sink escapes through `kcEscapeHtml` or `kgEsc`, and each page that uses `kcEscapeHtml` loads `app.js`. The loginReturnPath change rejects control characters and whitespace, resolves the path against the page's own origin and requires the same origin back.
- `72555993`: the firmware and partition-table changes are comment-only doc renames. No offsets or sizes changed.
- `f354981b`: `FETCH_SCRATCH_LEN` follows `FETCH_HEAP_SCRATCH_BYTES` (`update_fetch.c:62`). 1024 is at least `UPDATE_STAGE_SCRATCH_MIN` (256).
