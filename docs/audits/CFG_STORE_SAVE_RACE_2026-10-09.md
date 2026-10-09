# cfg store save-race audit, 2026-10-09

Bug class: `new_rev = s_*rev + 1`, commit, `s_*rev = new_rev` with no lock lets two tasks
mint the same rev (the later file then ties the earlier one). Scope: every store except
`zones_config_store.c` / `profiles_http.c` (owned elsewhere). Callers found by grep of
`App/` (non-test). httpd is ONE task (`wifi_provision_http.c`), `backup_import.c` runs on it.

| Store | Saver | Callers (tasks) | Verdict |
|---|---|---|---|
| ramp_assist_cfg.c | `ramp_assist_cfg_set_enabled` | diagnostics_http, backup_import (both httpd) | single task, no lock |
| net/time_sync.c | `time_sync_set_tz` | settings_http, backup_import (httpd) | single task, no lock |
| display_power_cfg.c | `display_power_cfg_set` | settings_http, backup_import (httpd) | single task, no lock |
| unit_pref.c | `unit_pref_set` | httpd, LCD task (ui_page_config), UART bridge task (uart_bridge_ext_control) | MULTI-TASK: locked |
| profiles_favorites.c | `profiles_favorites_set` | httpd (profiles_edit_http, profiles_http delete), LCD task (ui_page_profile_picker) | MULTI-TASK: locked |
| profiles_builtin.c | `hidden_mask_save` | profiles_edit_http, backup_import (httpd) | single task, no lock |
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
