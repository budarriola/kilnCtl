# cfg store save-race audit, 2026-10-09

Bug class: `new_rev = s_*rev + 1`, commit, `s_*rev = new_rev` with no lock lets two tasks
mint the same rev (the later file then ties the earlier one). Scope: every store except
`zones_config_store.c` / `profiles_http.c` (owned elsewhere). Callers found by grep of
`App/` (non-test). httpd is ONE task (`wifi_provision_http.c`). **Correction (2026-10-09):**
`backup_import.c`'s apply does NOT run on httpd: `POST /api/backup/import` hands it to its own
async task via `http_async_job_try_start()`. So every store backup_import writes has at least
two writer tasks (httpd and the import task), and the "single task" verdicts below were wrong
for those rows; they are now locked (see MED-2 below).

| Store | Saver | Callers (tasks) | Verdict |
|---|---|---|---|
| ramp_assist_cfg.c | `ramp_assist_cfg_set_enabled` | diagnostics_http (httpd), backup_import (async task) | MULTI-TASK: locked (MED-2) |
| net/time_sync.c | `time_sync_set_tz` | settings_http (httpd), backup_import (async task) | MULTI-TASK: locked (MED-2) |
| display_power_cfg.c | `display_power_cfg_set` | settings_http (httpd), backup_import (async task) | MULTI-TASK: locked (MED-2) |
| unit_pref.c | `unit_pref_set` | httpd, LCD task (ui_page_config), flash worker (CONTROL SET_UNIT_PREF job) | MULTI-TASK: locked; RAM assign moved inside (MED-3) |
| profiles_favorites.c | `profiles_favorites_set` | httpd (profiles_edit_http, profiles_http delete), LCD task (ui_page_profile_picker) | MULTI-TASK: locked |
| profiles_builtin.c | `hidden_mask_save` | profiles_edit_http (httpd), backup_import (async task) | MULTI-TASK: locked (MED-2) |
| setup_wizard_progress.c | `persist_all` | setup_progress_http (httpd) | single task, no lock |
| ct_verify_store.c | `ct_verify_store_save` | zones_current_sweep_task only (one sweep at a time) | single writer, no lock |
| iter_tune_store.c | `iter_tune_store_set_zone` | iter_tune_http (httpd) | single task, no lock |
| update/update_settings.c | `update_settings_set_locked` | already under `s_write_lock` | already locked |

Fix: `persist/cfg_save_lock.h`, a header-only lock modeled on `profiles_save_lock()` (static
storage, lazy create under a claim flag outside the critical section, release/acquire
publish, loser waits). Rev read, commit and rev bump are one section; it is a leaf lock
(only `pref_cfg_fs` is entered under it), never held across a producer call, and nothing
takes `profiles_save_lock` while holding it, so `profiles_save_lock -> favorites lock`
(profiles_http delete) has no inversion. `unit_pref_set` logs after release.

Test: `test_unit_pref.c` `test_save_holds_lock_across_rev_read_and_commit` (lock held at the
file write, released after, two saves get consecutive revs). Negative test: commenting out
take/give in `unit_pref.c` fails the lock-held check; restored by hand, forced rebuild green.
Caveat: the host stub is single-threaded, so this proves lock coverage, not a real interleave.

Revisit if any "single task" saver gains a second caller (a new LCD or bridge path).

## Save mutex vs. flash worker (2026-10-09)

The Opus review of dev `ea345348` found this. A save mutex is taken by more than one task, and
one of those tasks is the flash worker (`bx_flash_worker`, `uart_bridge_ext.c`). That deadlocks:

    httpd / import task:  take save mutex -> cfg write -> bx_run_on_internal_stack()
                          -> take s_bx_lock, send job, wait s_bx_done   (forever)
    flash worker:         running a bridge job that is itself a saver
                          -> take the same save mutex                    (forever)

After that, every later flash-worker job waits behind these two.

Two things make the cycle possible:

- Every cfg LittleFS write dispatches to the flash worker.
- These savers also run as jobs on the worker: CONTROL SET_ZONE_PID, SET_ZONE_MODEL and
  SET_UNIT_PREF, PROFILES SAVE and DELETE, and AUTOTUNE_CMD_ACCEPT.

Instances confirmed:

| Mutex | Worker-side taker | Off-worker taker | Where |
|---|---|---|---|
| `profiles_save_lock()` (profiles_http.c) | PROFILES SAVE/DELETE job | httpd profile save/delete, backup import | **real on origin/main and in release v1.0.0-pre.2** |
| zones save mutex (zones_config_store.c) | SET_ZONE_PID/MODEL, AUTOTUNE ACCEPT jobs | httpd POST /api/zones and setters, backup import | dev only |
| `unit_pref` `cfg_save_lock_t` | SET_UNIT_PREF job | httpd, LCD | dev only |
| `profile_executor` `s_exec.lock` | PROFILES DELETE job (`profile_executor_get_active_id()`) | the executor tick, which held it across `relay_cycles_maybe_persist()` (that call dispatches to the worker) | pre-existing, fixed here |

Not a deadlock: the kiln-config autosave in `zones_config_store.c`'s `nvs_save()`.

- When nvs_save runs on the worker (SET_ZONE_PID/MODEL), the autosave also runs there.
- `uart_bridge_ext_run_on_flash_worker()` already ran a call made on the worker inline, so
  this never waited on itself.
- The code comment said nvs_save never runs on the worker. That was wrong.
- The inline branch is now explicit, through `uart_bridge_ext_is_on_flash_worker()`.

### Design: reserve the worker before the save mutex

- `s_bx_lock` is now a recursive mutex.
- `cfg_save_lock_take()` reserves the worker before it takes the save mutex.
  - It calls `pref_cfg_fs_save_section_enter()`.
  - Off the worker, that takes `s_bx_lock` (`bx_reserve_for_save_section()`).
  - On the worker, or before the worker exists, it does nothing.
- `cfg_save_lock_give()` gives the mutex first, then releases the reservation.
- The cfg write inside the section takes `s_bx_lock` again (it is recursive) and dispatches
  as usual.
- The worker try-takes `s_bx_lock`, with no wait, before it runs a posted (fire-and-forget)
  job. So a job runs on the worker only while `s_bx_lock` is held, by its dispatcher or by
  the worker itself.
- So any off-worker task that holds a save mutex also holds `s_bx_lock`. While it does, no
  worker job can be running, and the worker can never block on that mutex.

Lock order:

- The reservation (`s_bx_lock`) is taken before the save mutex.
- `s_bx_lock` is taken before `s_exec.lock`.
- Never take `s_exec.lock` inside a save section. Read executor state before taking the save
  lock.
- Short RAM locks are fine inside a save section if their holders never wait on the worker
  (a portMUX, `zones_cfg_lock()`).

Cost:

- While one task is inside a save section, worker dispatches from other tasks wait. They
  would have waited for its write anyway.
- Two save sections on different stores no longer run at the same time.

Residual: a section entered before `uart_bridge_ext_start_flash_worker()` reserves nothing.
All savers run from tasks started after that call.

What changed:

- `profiles_save_lock()` and the zones save mutex are now `cfg_save_lock_t`.
- `aux_outputs_cfg.c` `s_set_lock` was a raw mutex. It is now `cfg_save_lock_t`.
- The executor tick calls `relay_cycles_maybe_persist()` after giving `s_exec.lock`.
- **MED-1:**
  - `zones_config_set_relay_name()`, `zones_config_set_relay_device_type()` and every
    `zone_normals_*` setter now edit RAM inside the zones save section.
  - `relay_names_save_locked()` commits a snapshot taken under `zones_cfg_lock()`.
- **MED-2:** these now take a `cfg_save_lock_t` around the RAM assign, rev read, commit and
  rev bump:
  - `ramp_assist_cfg_set_enabled`
  - `time_sync_set_tz`
  - `display_power_cfg_set`
  - `profiles_builtin` set_hidden and restore_all
- **MED-3:** `unit_pref_set` assigns RAM after taking the lock.

### Tests

The host FreeRTOS stub runs one thread, so the tests cannot run two real tasks into the
deadlock. Instead, `test/save_section_probe.h` checks the shape that rules it out. It installs
recording section hooks and a recording cfg write function, then checks that:

- the reservation is entered with no lock held, so it comes before the mutex;
- the reservation is exited after the mutex is given;
- every cfg write or delete happens inside it;
- enter and exit calls balance.

For the MED items, the probe also checks that RAM still holds the old value when the section
opens.

Tests that use the probe:

- `test_unit_pref.c`
- `test_ramp_assist_cfg.c`
- `test_display_power_cfg.c`
- `test_time_sync.c`
- `test_profiles_builtin.c`
- `test_aux_outputs_store.c`
- `test_profiles_http.c`: save and delete.
- `test_zones_http.c`: nvs_save, the relay name setter and zone_normals_set. It also checks
  that the autosave runs inline when nvs_save is on the worker.

### Lint

`test/flash_worker_lint.py` flags any driver function that takes a semaphore and then calls a
write that dispatches to the worker before giving the semaphore back. It checks for these
calls:

- `pref_cfg_fs_commit` / `_save`
- `profiles_cfg_fs_save*`
- `zones_config_cfg_fs_save*`
- `cfg_fs_write_atomic*` / `cfg_fs_delete*`
- `uart_bridge_ext_run_on_flash_worker*`
- `relay_cycles_maybe_persist`
- `relay_names_save*` / `zone_normals_save*`

A `cfg_save_lock_t` section is the approved way to hold a lock across such a write.

The lint cannot see a raw mutex hidden behind a wrapper function, as `aux_outputs_cfg.c`'s old
`ao_lock()` was. Review any new wrapper by hand.
