# Review: factory-reset writer fence and otafix (origin/dev, 2026-10-09)

Read-only Opus review against origin/dev `c23ebb76`.

- **A. Factory-reset writer fence:** c6e70459, b1020e56, c2aa85e0, 63b808e8, f293f196.
- **B. otafix:** 2c0ca043, 65276a7b, be451c32.

No HIGH findings.

## Part A: factory-reset writer fence

### What the fence is

- **The mark.** `relay_authority_reset_in_flight_begin()` sets it in `execute_scope()` (`firmware/KilnFW/App/drivers/http/factory_reset.c`, around lines 485-543).
- **The barrier.** `persist_reset_barrier()` (`cfg_save_barrier.c`) takes and gives every lock registered in pref_cfg_fs's lock registry. After it, no save section that started before the mark can still be running.
- **Two central refusals**, both installed in main.c with `relay_authority_reset_refuses_writer()`:
  - `pref_cfg_fs_save()` refuses, which covers `pref_cfg_fs_commit()` too.
  - `cfg_fs_write_job_run()` refuses, i.e. `cfg_fs_write_atomic_device()`.
- **Local refusals** inside the save sections: unit_pref, time_sync, profiles_favorites, zones_config_store (three sites) and profiles_http.

### MED-1: three cfg-file writers bypass both central refusals (fixed in 04848cd9)

Only zones_config_cfg_fs, pref_cfg_fs and profiles_cfg_fs are pointed at `cfg_fs_write_atomic_device` (`persist/cfg_fs_mount.c:61-63`). These three writers call raw `cfg_fs_write_atomic()` instead. They hold no registered save lock and check no reset mark, so neither the barrier nor either refusal stops them.

**(a) Kiln configs.**
- Where: `persist/kiln_cfg_store_cfg_fs.c:14` (`s_write_fn = cfg_fs_write_atomic`), reached from `kiln_cfg_store.c` `nvs_save_store()`.
- Scenario: a `POST /api/kiln_configs` save, rename, clone, delete or apply runs alongside a kiln or all scope reset. If its write lands after `delete_kiln_cfg_files()`, the kiln-config file survives the reset with pre-reset contents.

**(b) Firing stats.**
- Where: `persist/firing_stats_cfg_fs.c:31`, via `firing_stats_persist()` (`control/profile_executor_firing_stats.c:839`).
- Cause: the persist runs after the executor has published IDLE/DONE and outside `s_exec.lock`. The heat-run gate no longer holds off a reset at that point.
- Scenario: a run finishes while a profiles or all scope reset is starting. An `fs_<id>` file is written after the delete or format and survives it.

**(c) Raw cfgfs file restore.**
- Where: `http/diagnostics_http.c:1857`, `cfgfs_file_write_job` (`POST /api/cfgfs/file`, the full-board-backup restore primitive).
- Cause: it runs on the flash worker with no refusal.
- Scenario: a restore job queued behind the reset job writes a file into the freshly deleted or formatted cfg partition.

**Suggested fix.** Two options:
- Install `cfg_fs_write_atomic_device` for kiln_cfg_store_cfg_fs and firing_stats_cfg_fs (cfg_fs_mount.c), and make `cfgfs_file_write_job` check `relay_authority_reset_refuses_writer()`.
- Simpler and closed against future writers: put the refusal inside `cfg_fs_write_atomic()` itself, keeping the reset-job task exemption.

Add one test per writer.

### LOW-1: an aux conversion can leave its journal on a wiped kiln_nvs (fixed in 04848cd9)

**Cause.**
- `factory_reset.c` `reset_other_writer_refuses()` refuses a reset for a zone sweep or a backup restore, but not for a running zone-to-aux conversion (`profiles_http_convert_busy()`).
- The conversion checks only `system_mode_gate` (`zone_aux_convert_http.c`).
- `aux_convert_journal_write()` (`persist/aux_outputs_cfg.c:400`) re-inits kiln_nvs lazily and writes with no reset-mark check.

**Scenario.**
1. A conversion past stage 1 meets a reset that erases kiln_nvs.
2. Its later writes are refused centrally, and the rollback's zone write is refused too, giving `ZONE_AUX_FREE_UNCERTAIN` or "ROLLBACK INCOMPLETE".
3. The journal-kept path leaves the marker written into the re-inited kiln_nvs.
4. After the reboot, readiness (`readiness_http.c:1113`) reports a conversion in progress against factory-default zones.
5. An operator `resume=1` would then act on the defaults.

Resume is manual, so this is LOW.

**Suggested fix.** In `reset_other_writer_refuses()`, also refuse while `profiles_http_convert_busy()` is set. Or make `aux_convert_journal_write()` and `aux_convert_journal_clear()` refuse under the mark.

### LOW-2: the fence's two new hooks have no tests (fixed in 04848cd9)

**Gaps.**
- No test exercises `relay_authority_reset_refuses_writer()`, i.e. the reset-job task exemption.
- No test exercises the `cfg_fs_write_job_run()` refusal.

**Impact.**
- If the exemption broke, only the reset job's own `profiles_builtin_restore_all()` hidden-mask commit would be refused, and that file is deleted anyway, so impact is low.
- If the worker hook were lost, nothing would fail.

**Suggested fix.**
- A relay_authority host test: depth 0, job task, other task.
- A cfg_fs_mount test in which the hook returns true and the job returns an error without writing.

### LOW-3: unit_pref's local refusal is redundant (negtest MISSED it)

The negtest MISSED the local `cfg_save_lock_reset_refused()` check in `unit_pref_set_ex()` (`persist/unit_pref.c:164`). This is expected and harmless:
- `pref_cfg_fs_commit()` already hits the central refusal.
- unit_pref publishes RAM only on success, so behavior is identical.

The local check is defense in depth. The test proves the central fence, not the local line, and that is acceptable. A comment in the test saying so would stop a future reader from thinking the local check is covered.

### Checked, no finding

- **Deadlock.**
  - The barrier caller (an httpd task, or the UART SYSTEM task via `uart_bridge_system.c:107`) holds no lock while taking each registered lock.
  - Every save section takes the flash-worker reservation (`s_bx_lock`) before its mutex, and the barrier follows the same order.
  - Worker jobs run only under their dispatcher's reservation, so the reset job and off-worker save sections are already mutually exclusive.
- **Mark lifetime.** The mark is cleared on the late mode-gate refusal and on a dispatch failure. After the erase it deliberately stays set until reboot (`reboot_task`, or the inline `factory_reset_reboot_fallback`). `execute_scope_job()` is linear: enter at line 226, exit at line 424, no early return.
- **NULL hook.** Host builds and the recovery image (`firmware/KilnFW_recovery` links neither factory_reset.c, pref_cfg_fs.c nor cfg_save_barrier.c) do not install the hooks, so nothing is refused there. That is correct: there is no factory-reset path in those builds.
- **Registry capacity.** 32 slots, about 11 used. Overflow only logs. Informational.
- **NVS stores outside the fence** (wifi_prov, relay_cycles, touch_cal, safety_cfg_store, boot_guard): the partition erase de-inits NVS, so later writes fail. ota_record and run_state re-init lazily. Benign: they hold diagnostics and run state, not reset-scoped user data.

### Review of fix 04848cd9 (MED-1, LOW-1, LOW-2)

Read-only Opus review of origin/dev `04848cd9`. The fix holds for MED-1, LOW-1 and LOW-2. No HIGH or MED findings.

**What the fix does.**
- `cfg_fs_set_write_refuse_hook()` puts the refusal inside `cfg_fs_write_atomic()` itself. It returns `ESP_ERR_INVALID_STATE`, the same code as "not mounted". This is the closed-against-future-writers option from MED-1.
- main.c installs it next to the other two hooks.
- `reset_other_writer_refuses()` (factory_reset.c) now also refuses while `profiles_http_convert_busy()`.
- `aux_convert_journal_write()` refuses under the fence.
- Tests are added for each hook.

**1. Every `cfg_fs_write_atomic` caller treats a refusal as "not persisted".**
- The `POST /api/cfgfs/file` job (`diagnostics_http.c` `cfgfs_file_post_handler`) answers 500 `{"ok":false,"error":"ESP_ERR_INVALID_STATE"}`, never success.
- The kiln-config store (`kiln_cfg_store.c` `nvs_save_store()`) bumps `s_kiln_cfg_rev` only on ESP_OK. Its mutators roll RAM back on failure.
- `firing_stats_persist()` returns without updating its cache.
- The zones, profiles and prefs writers already went through the device path, and their failure handling is unchanged.
- Several call sites treat `INVALID_STATE` as benign (it already meant "unmounted"). All are safe:
  - Boot-resolve write-backs in kiln_cfg_store_cfg_fs, firing_stats_cfg_fs and the pref, profiles and zones cfg_fs resolvers. The mark is always 0 at boot.
  - The relay-names v1 upgrade (`zones_config_store.c:1252`), which also runs at boot.
  - A delete in `profiles_http.c:1498`.
  - `backup_import.c:3811`, which reports "not written".

**2. Hook install order.** main.c:226-228 installs all three hooks at the very start of `app_main`, before `main_boot_early` and before any task exists. That also covers boot_guard recovery mode.

The recovery image (`firmware/KilnFW_recovery`) links neither cfg_fs, relay_authority nor factory_reset, so it needs no hook.

**3. The reset-job exemption cannot leak.**
- `s_reset_job_task` is set only between `relay_authority_reset_job_enter()` and `_exit()`.
- Both calls sit inside the linear `execute_scope_job()` on the persistent flash worker, which runs one job at a time.
- Exit clears the exemption. After exit every task is refused until reboot.

A reused task handle cannot pick it up, because no task is created or deleted while the exemption is set.

**4. Leaving `cfg_fs_delete()` and `aux_convert_journal_clear()` unfenced is safe.**
- Every delete caller moves state toward the post-reset state: the kiln and profiles scope deletes, `pref_cfg_fs`, `profiles_builtin`, `profiles_cfg_fs` and `firing_stats_cfg_fs`.
- A journal clear only removes the marker.
- Every journal write and clear runs inside convert busy, which the reset now refuses.
- The convert sets busy (`_Atomic`, seq_cst) before its mode check. The reset sets the mark and then reads busy. That Dekker-style pairing means at least one side refuses.

So the busy refusal is the real LOW-1 fix. The `journal_write` fence is defence in depth. Its check sits outside any lock, but the pairing above closes that TOCTOU.

**5. The NVS-side writers are not fenced.** See LOW-6.

**Negative test.** I ran `tools\negtest.ps1` against `build_host_tests.ps1 -Only 'test_cfg_fs|aux_outputs_store|ota_http|link_watchdog|prestart'` (8 executables, baseline passed), with expect pattern `RUN FAILURES`.

| Mutation | Result |
|---|---|
| `cfg_fs_write_atomic` refusal disabled (`if (0 && refuse && refuse())`) | CAUGHT |
| `aux_convert_journal_write` fence disabled | CAUGHT |
| factory_reset convert-busy refusal disabled | CAUGHT |
| exemption dropped (`refuses_writer` returns true for the job task too) | CAUGHT |
| exemption ignores task identity (`s_reset_job_task == NULL` only, so nobody is refused while the job is entered) | MISSED |

The MISSED row is expected, because the host tests are single-threaded. See INFO-4.

#### LOW-6: a lazily re-inited kiln_nvs lets unfenced NVS writers survive a kiln or all reset

**Cause.**
- The partition erase de-inits kiln_nvs, and `hal_kv_open()` does not re-init it.
- Several readers do re-init kiln_nvs lazily, though: the dual-write status readers in ramp_assist_cfg, time_sync, display_power_cfg and aux_outputs_cfg, plus `run_state` and `aux_convert_journal_clear()`.
- Once one of them runs, kiln_nvs writers that check no reset mark can land in the roughly 500 ms before `reboot_task` restarts the board.

**Writers that check no mark.**
- `persist()` in `drivers/safety/estop_verification.c:76`, reached by `POST /api/estop/verify`. Its own header says a kiln or all reset is exactly what must invalidate the record. A surviving record would show "E-stop verified" after the reset.
- `firing_shadow_store_persist()` in `drivers/control/firing_shadow.c:136`. This is the diagnostic verdict summary.
- The swap-pending record write at `persist/kiln_cfg_swap.c:196`. It could raise a spurious boot-time "swap interrupted" banner.
- adaptive_tune and run_state writes.

This is LOW because it needs a concurrent request inside a sub-second window after the erase.

**Suggested fix.** Make `estop_verification` `persist()` check `relay_authority_reset_refuses_writer()`, at minimum. Or fence `hal_kv_set_*` for the kiln_nvs partition centrally, the same way 04848cd9 fenced `cfg_fs_write_atomic()`.

#### LOW-7: nothing pins the main.c hook installs

All three refusals rely on the three setter calls at main.c:226-228. No check script or test fails if one is removed. The host tests install the hook themselves, so they would stay green.

The gap already existed for the two earlier hooks.

**Suggested fix.** Add a source-text check, or a line in an existing check, that requires all three calls in `app_main`.

#### INFO

- **INFO-1.** Under the fence, `aux_outputs_http_core.c:152` maps `INVALID_STATE` to 409 "zone conflict or quarantined store". That text is misleading, but it is not a success. It predates 04848cd9.
- **INFO-2.** The refused `POST /api/cfgfs/file` answers 500, not 409. A restore client sees a server error rather than "try later".
- **INFO-3.** `firing_stats_persist()` is refused cleanly. The `firing_shadow_finish_firing()` NVS write just before it is not (LOW-6).
- **INFO-4.** Only a multi-task test could cover the task identity in the exemption (the MISSED mutation). Because of item 3, the practical risk is nil.

## Part B: otafix

### MED-2: the new fault-kind name does not fit `/api/status`'s 24-byte buffer, so the web banner reads as an alarm

**Where.**
- `http/dashboard_http.h:186` declares `char kiln_cfg_swap_boot_fault_kind[24];`.
- `http/dashboard_http.c:501` fills it with `snprintf(..., "%s", kiln_cfg_swap_boot_fault_kind_name(kind))`.
- The new name `"rollback_active_id_unsaved"` is 26 characters (27 bytes with the NUL), so it is truncated to `"rollback_active_id_unsa"`.

**Scenario.**
1. The board latches `KILN_CFG_SWAP_BOOT_FAULT_ROLLBACK_ACTIVE_ID_UNSAVED`.
2. `/api/status` emits the truncated string.
3. `main_page.html`'s `renderKilnCfgSwapBootFault()` matches neither `'rollback_active_id_unsaved'` nor `'active_id_unsaved'`.
4. The dashboard shows the red **"KILN CONFIG SWAP INTERRUPTED -- NOT RECOVERED"** banner.

That is worse than before this commit, when the same condition latched `active_id_unsaved` and got the neutral banner. The LCD is unaffected because it reads the enum directly. Heat is not affected.

Tests check `kiln_cfg_swap_boot_fault_kind_name()` but never the dashboard copy.

**Suggested fix.**
- Size the field from the longest name (32 bytes) and add a `_Static_assert` or host test that every `kiln_cfg_swap_boot_fault_kind_name()` value fits.
- Check the other UI-side `dashboard_status_t` copies of this field listed in `UNCHECKED_PERSIST_RESULT_AUDIT_2026-10-09.md`.

### LOW-4: the commit message overstates the lint change, and the build-directory exclusion fails on Windows

**Commit message.** 2c0ca043 says the page lint "skips non-JS script types", but `lint_pages.js` has no type-based skip. Its script regex (`/<script(?![^>]*\bsrc=)[^>]*>/gi`) still feeds every inline block to `new vm.Script()`. So the lint cannot hide a real JS block of any type. A `type="module"` or `application/json` block would be reported as a false positive, not hidden. No page has either today.

**Build-directory exclusion.** The new filter `!/[\/](build|managed_components)[\/]/.test(p)` matches only forward slashes. `walk_files()` builds paths with `path.join`, which uses backslashes on Windows, so `firmware/KilnFW_recovery/build/**` is walked and not excluded. It holds no .html/.js/.css today, so the cost is only walk time, but a future build output (an embedded web asset copy, for example) would be linted.

**Suggested fix.**
- Use `[\\/]` in the regex, or skip the directories in `walk_files()` by entry name the way it already skips `node_modules`.
- Correct the wording in the next commit touching this file.

### LOW-5: the HTTP reply does not tell an adopted save from a plain failure

**Where.** `http/dashboard_settings_http.c` `unit_pref_post_handler()` returns `cfg_fs_http_persist_failed(req)` in both cases. Only the log line differs. The UART reply (`uart_bridge_ext_control.c`) and the LCD do distinguish them.

**Impact.** A web client cannot learn that the new unit is live and on file. The page will likely re-read and show the live unit anyway.

**Suggested fix.** Optionally add an `"adopted":true` field to the error body. Otherwise no change is needed.

### Checked, no finding

- **`out_adopted` in `unit_pref_set_ex()`** is correct on every return path:
  - It is set false before the argument check.
  - It stays false on the reset refusal and on success.
  - It is set true only in the read-back branch, where `f_valid && f_rev == new_rev`. That branch runs under the save lock, so `f_raw` must be this save's value.
  - The test stub in `test_uart_bridge_ext_control_gate.c` matches the contract.
- **Fault-kind coverage.** The C enum, the `kind_name()` switch, the header doc list, the web banner, the LCD strip and both latch sites (PICO_DONE recovery and the ESP_DONE fallback with a kept journal) are updated.
  - No PcTools reader or status-schema doc names the kind strings, apart from the audit doc.
  - `kiln_configs_page.html` keys on the reason prefix, not the kind, which is fine.
  - The only miss is MED-2's buffer.
- **LCD text.**
  - The new strip text is 58 characters, one more than the existing 57-character "CONFIG SWAP INTERRUPTED -- see dashboard, re-apply config".
  - The strip is one line, 22 px high, `LV_LABEL_LONG_DOT`, full width on the 480-px landscape panel.
  - At worst the tail ("retried at boot") truncates with an ellipsis, as the existing strings already risk, while the leading "KILN ROLLED BACK" stays visible.
- **ota_page.html comment merge.** The split `*/` that ended the comment early, leaving the following line as stray tokens in the inline script, is now one comment. The page parses.
- **be451c32** only clears the latched fault left by the preceding test section, so the ESP_DONE-fallback assertion is not satisfied by stale state. Correct.
