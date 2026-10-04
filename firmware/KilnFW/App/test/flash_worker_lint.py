#!/usr/bin/env python3
"""flash_worker_lint.py -- direct flash/NVS write calls, outside the
allowlist below, have panicked real hardware three times: a PSRAM-stacked
task's NVS write asserted esp_task_stack_is_sane_cache_disabled() (see
kiln_cfg_store.c's/safety_cfg_store.c's caller_stack_is_external() guard
comments and DRAM_PSRAM_STATUS.md section 7.2), and a handler already running
ON the flash-safe worker deadlocked the whole board when it tried to
dispatch a SECOND flash-safe call through the normal path (uart_bridge_ext.c
commit 7c47683, see that file's "RE-ENTRANCY" comment). Host tests cannot
see either bug -- the host build stubs nvs_set_*()/nvs_commit()/
hal_kv_set_*()/hal_kv_commit() as no-ops with no stack or re-entrancy model
at all (see App/test/stubs/nvs.h and App/test/fake_kv.h) -- so this is a
static grep-based lint, not a test.

THE SANCTIONED PATTERN (read uart_bridge_ext.c's own header comment and
kiln_cfg_store.c's nvs_save_store()/caller_stack_is_external() for the full
story before changing this allowlist):

  1. Dispatch the write onto the single shared flash-safe worker task via
     uart_bridge_ext_run_on_flash_worker() (internal-SRAM stack, so a
     PSRAM-stacked caller is never the one doing the write), checking
     uart_bridge_ext_is_on_flash_worker() first so a caller already ON that
     worker does the write inline instead of deadlocking itself waiting for
     its own job to drain the queue it is blocking.
  2. OR guard the write with a local caller_stack_is_external() check (the
     same predicate, copied into each file rather than shared, per those
     files' own comments) so a PSRAM-stacked caller is refused loudly
     instead of taking the board down -- used by files whose own callers
     are expected to have already dispatched onto the worker (or reached
     this code from app_main's own internal-SRAM-stack task before the
     scheduler introduced any other caller).
  3. OR run ONLY from app_main's own task, once, before the scheduler starts
     any other task that could contend for the worker or run on a PSRAM
     stack -- the same "no concurrency yet" argument boot_guard.c/
     crash_report.c/ota_record.c/time_sync.c/touch_cal_store.c/watchdog_cfg.c/
     wifi_prov.c/ota_http.c each make in their own init-time comments. A
     SIBLING justification under this same pattern number, for a file whose
     write call site is instead reached live from an httpd handler's or the
     LVGL task's own internal-SRAM stack (never init-time, never PSRAM) --
     profiles_builtin.c/ramp_assist_cfg.c/unit_pref.c/zones_config_store.c
     each make this version in their own entries below.

A file not on the allowlist below that starts calling nvs_set_*()/
nvs_commit()/hal_kv_set_*()/hal_kv_commit()/hal_kv_erase_*()/
esp_partition_write()/esp_partition_erase_range() directly has, by
definition, not been through that reasoning -- it fails this lint naming
the exact file:line, rather than shipping a fourth hardware incident.

EXTENDED 2026-09-07 (filesystem write paths): the FILESYSTEM_PLAN.md work
added a second write surface, `cfg_fs` (LittleFS-backed, cfg_fs.c), sitting
ON TOP of the flash worker rather than replacing it -- cfg_fs_write_atomic()/
cfg_fs_delete()/cfg_fs_format() are themselves just filesystem calls, so a
caller reaching them directly is exactly as unsafe on a PSRAM stack or a
small stack as calling nvs_set_*()/hal_kv_set_*() directly was. Two more
rules cover this surface (see CFG_FS_CALL_RE and scan_reentrancy() below):
one extends the same allowlist-or-justify scan to the cfg_fs_* call names,
the other is new -- it catches the RE-ENTRANCY half of the hazard
specifically, because a `cfg_fs_*` write is nearly always reached through a
*_set_write_fn()-installed callback (pref_cfg_fs.c/profiles_cfg_fs.c/
zones_config_cfg_fs.c's `s_write_fn`), so the caller who dispatches onto the
flash worker and the eventual cfg_fs_write_atomic() call are in DIFFERENT
files, and neither file's own text shows the hazard -- only the fact that
SOME dispatcher exists with no visible re-entrancy guard nearby is visible
from either side.

This is exactly the "static analysis genuinely cannot decide" case: whether
a given uart_bridge_ext_run_on_flash_worker() call site can ever be reached
from a caller already on the worker depends on the whole call graph (who
calls the function that dispatches, transitively), which this line-oriented
lint does not build and should not try to fake. So scan_reentrancy() does
not try to prove reachability either way -- it requires every dispatch call
site to carry ONE of: (a) a nearby uart_bridge_ext_is_on_flash_worker() check
(the sanctioned re-entrant-safe pattern), or (b) the explicit justification
phrase "not reachable on-worker" in a comment near the call, the same
phrase log_store_mount.c's and autotune_engine_step_identify.c's own
pre-existing dispatch sites already used before this rule existed (kept
verbatim rather than inventing a new tag, so those two sites needed no
change to pass). A dispatch site with neither is flagged BY NAME, not
guessed at -- this is how it caught cfg_fs_write_atomic_device() (cfg_fs_
mount.c) missing the guard every other generic-callback dispatcher in this
codebase carries: see this script's own test suite / the audit that added
this rule for the live re-entrancy path that makes it a real bug, not a
theoretical one (CONTROL_CMD_SET_UNIT_PREF, dispatched onto bx_flash_worker
by uart_bridge_ext_control.c's control_task, ends in unit_pref_set() ->
pref_cfg_fs_save() -> the installed cfg_fs_write_atomic_device() write_fn,
which dispatches onto bx_flash_worker AGAIN with no re-entrancy check --
deadlock).

Usage: python flash_worker_lint.py [drivers_dir]
Exit 0: clean. Exit 1: violation(s) found (printed as file:line).
Exit 3: SKIPPED -- drivers dir not found (missing prerequisite, not a pass).
"""
import re
import sys
from pathlib import Path

# ---- allowlist --------------------------------------------------------
# Seeded from current reality (2026-09-04 audit: every drivers/*.c file
# that calls nvs_set_*()/nvs_commit()/hal_kv_set_*()/hal_kv_commit()/
# hal_kv_erase_*()/esp_partition_write()/esp_partition_erase_range()
# today). Adding a file here is not "fixing a
# lint failure" -- it is asserting, with the one-line justification below,
# that the new call site follows one of the three sanctioned patterns
# above. Say which pattern and why in the comment, the same way every
# existing entry does.
ALLOWLIST = {
    # Pattern 1 (worker dispatch): save_kibase_job() runs via
    # uart_bridge_ext_run_on_flash_worker(), checking
    # uart_bridge_ext_is_on_flash_worker() first for the re-entrant case
    # (autotune_handle_accept() already on the worker).
    "adaptive_tune.c",
    # Pattern 3 (init-time only): boot_guard_record_boot() is called once
    # from app_main's own task before the scheduler starts any other task.
    "boot_guard.c",
    # Pattern 3 (init-time only): crash_report_save() runs from the panic/
    # boot path, before normal task concurrency exists.
    "crash_report.c",
    # Pattern 3 (internal-SRAM-stack caller, reached live, not init-time) --
    # same shape as display_power_cfg.c's/profiles_builtin.c's own entries.
    # estop_verification_confirm()'s only call site is diagnostics_http.c's
    # POST /api/estop/verify handler; estop_verification_clear()'s only call
    # sites are safety_cfg_http.c's apply_pairs() (a commit of param 0x0212,
    # reached from a POST /api/safety/config-family commissioning handler)
    # and factory_reset.c's partition erase does not call this file at all
    # (the record is invalidated for free by KILN_NVS_PARTITION being erased
    # whole -- see estop_verification.h). Both real call sites are httpd
    # handlers running on wifi_provision_http.c's httpd task -- internal-
    # SRAM stack, never PSRAM, never the flash worker -- so neither
    # dispatches through uart_bridge_ext_run_on_flash_worker() and the
    # re-entrancy half of this lint does not apply here.
    "estop_verification.c",
    # Pattern 2 (local caller_stack_is_external() guard), same shape and
    # same reasoning as run_state.c's/kiln_cfg_store.c's own entries:
    # persist_locked() checks it before every hal_kv_set_blob()/
    # hal_kv_commit(). Write call sites are dualwrite_window_boot_check()
    # (from dualwrite_window_http_start(), itself called from
    # main_network_http.c's httpd bring-up -- internal-SRAM-stack task, not
    # PSRAM), dualwrite_window_note_mount_failure() (not yet wired to any
    # call site -- see this file's own header comment), and
    # dualwrite_window_note_firing_complete()/_note_restore_verified()
    # (profile_executor.c's tick task and the dualwrite_window_http.c POST
    # handler, respectively -- same internal-SRAM-stack story). None of
    # these call sites dispatch through uart_bridge_ext_run_on_flash_worker(),
    # so the re-entrancy half of this lint does not apply here -- only the
    # direct-hal_kv-write half, which the guard covers.
    "dualwrite_window.c",
    # Pattern 3 (internal-SRAM-stack caller, reached live, not init-time) --
    # same shape as display_power_cfg.c's entry immediately below.
    # ct_verify_store_save()'s only caller is
    # zone_sweep_record_ct_attribution() (zones_current_sweep_task.c), which
    # runs on the sweep task's own internal-SRAM stack at the end of a sweep
    # run; ct_verify_store_start()/_get() never write at all. That task does
    # not dispatch through uart_bridge_ext_run_on_flash_worker(), so only the
    # direct-hal_kv-write half of this lint applies. ct_verify_store.h states
    # the PSRAM-stack restriction explicitly, so a future second caller has
    # to confront it rather than discover it on hardware.
    "ct_verify_store.c",
    # Pattern 3 (init-time only): display_power_cfg_set() runs from
    # settings_http.c's POST /api/settings/display_power handler, on that
    # handler's own internal-SRAM-stack httpd task -- same story as
    # unit_pref.c/zones_config_store.c's identical entries below.
    "display_power_cfg.c",
    # Spare-relay aux outputs (docs/SPARE_RELAY_ONOFF_PLAN.md WP-1):
    # aux_outputs_cfg_set() is a per-relay config write called from the
    # zones HTTP handler / MCP path on an httpd task's internal-SRAM stack,
    # same story as display_power_cfg.c's entry immediately above.
    "aux_outputs_cfg.c",
    # docs/FILESYSTEM_USER_DATA_PLAN.md section 5 item 7 (firing stats/
    # history cfg-filesystem bridge, 2026-09-08): firing_stats_cfg_fs_
    # write_rev()'s hal_kv_set_u32()/hal_kv_commit() calls are this file's
    # ONLY write call site, and its ONLY caller anywhere in the codebase is
    # profile_executor_firing_stats.c's firing_stats_persist() -- which
    # already runs caller_stack_is_external() (Pattern 2) as the very first
    # thing it does, before this file's function is ever reached, and is
    # itself on this same allowlist for that reason. No new PSRAM-stack
    # exposure: this is the same guarded call path, one file further down
    # the same call chain, not a second independent write path.
    "firing_stats_cfg_fs.c",
    # UPDATED 2026-09-07 (stack-margin review after system_uart_bridge's
    # get_stack_margin() reading came back LOW, 28.8% headroom on a 3072 B
    # task at idle). The 2026-09-06 entry this replaces reasoned only about
    # reset_post_handler()'s httpd caller (8192 B, comfortable) and missed
    # that execute_scope() has a SECOND caller: uart_bridge_system.c's
    # SYSTEM_CMD_FACTORY_RESET, on system_uart_bridge's much smaller stack --
    # a destructive, rarely-exercised path whose true depth
    # (hal_kv_erase_partition()'s underlying nvs_flash_erase_partition()) had
    # never actually run on that task during the idle soak. Now Pattern 1
    # (worker dispatch): execute_scope() dispatches the erase onto
    # bx_flash_worker via uart_bridge_ext_run_on_flash_worker(), checking
    # uart_bridge_ext_is_on_flash_worker() first for the (currently
    # theoretical) re-entrant case, same shape as adaptive_tune.c's entry
    # above -- removes the dependency on which caller's stack is big enough.
    "factory_reset.c",
    # Pattern 2 (local caller_stack_is_external() guard): nvs_save_store().
    "kiln_cfg_store.c",
    # Pattern 2 (local caller_stack_is_external() guard, via the SAME shared
    # hal_kv_write_safe_here() predicate kiln_cfg_store.c's entry above
    # uses -- not a re-derived copy): save_pending()'s hal_kv_set_blob()/
    # hal_kv_commit() calls (the pending-swap crash-recovery record, docs/
    # audits/kiln_swap_transaction_2026-09-14.md H6/H10) are guarded by
    # `if (!hal_kv_write_safe_here()) { ... refuse ... }` as the very first
    # thing save_pending() does, same shape as kiln_cfg_store.c's own
    # nvs_save_store(). kiln_cfg_swap_apply() itself is documented (see its
    # own header's TASK PLACEMENT note) to run on a dedicated, internal-
    # SRAM-stacked worker task, never the httpd worker or the flash worker
    # itself -- this guard is the defensive backstop if that is ever
    # violated, not the primary safety argument.
    "kiln_cfg_swap.c",
    # Pattern 2 (local caller_stack_is_external() guard, via the SAME shared
    # hal_kv_write_safe_here() predicate kiln_cfg_store.c's/kiln_cfg_swap.c's
    # entries above use -- not a re-derived copy). docs/LIVE_PROFILE_EDIT_
    # PLAN.md pass 1: live_profile_save_record()/_save_working()/_clear()
    # each guard with `if (caller_stack_is_external()) { ... refuse ... }`
    # as the first thing they do. Callers today are host tests only; from
    # pass 2 on, the profile-edit HTTP handler and profile_executor's own
    # end-of-firing decision path call in -- neither a PSRAM-stacked task
    # nor the flash worker itself, but this guard is the defensive backstop
    # rather than the primary argument, same shape as kiln_cfg_swap.c's
    # entry above.
    "live_profile.c",
    # Pattern 3 (init-time / recovery path): ota_http.c's pico-firmware
    # esp_partition_erase_range()/esp_partition_write() calls run from the
    # single-threaded OTA apply sequence, not a PSRAM-stacked handler task.
    "ota_http.c",
    # Same call sites as ota_http.c above -- ota_pico_do_stage() (and its
    # esp_partition_erase_range()/esp_partition_write() calls) moved here
    # verbatim in the 2026-09-04 ota_http.c split (a6ab73b); still called
    # only from ota_pico_post_handler()'s single-threaded httpd handler, not
    # a PSRAM-stacked task.
    "ota_http_pico.c",
    # Owner decision 2026-09-20, task 3: the esp_partition_erase_range()/
    # esp_partition_write() call sites that used to live inline in
    # ota_http_pico.c's ota_pico_do_stage() (see that file's entry just above)
    # moved into this new shared module, pico_img_stage.c, so
    # net/pico_auto_update_boot.c's embedded-image writer and
    # ota_http_pico.c's HTTP-streaming writer call the same
    # begin/write_chunk/finish() sequence instead of each keeping its own
    # copy. Same reasoning as the entry above: both callers run on the httpd
    # task or the boot task respectively -- internal-SRAM stack, never the
    # flash worker, never PSRAM-stacked -- so moving the calls here changes
    # nothing about which stack they run on, only which file owns the code.
    "pico_img_stage.c",
    # Pattern 3 (init-time only): ota_record_save() runs once from
    # app_main's boot-time OTA-verify sequence.
    "ota_record.c",
    # docs/PICO_AUTO_UPDATE_PLAN.md -- persisted per-pair attempt-budget
    # counter for the Pico auto-update decision (pico_auto_update.h). Same
    # mechanics as boot_guard.c (copied deliberately, see this file's own
    # header), and same allowlist reasoning: as of this pass its write
    # functions (pico_update_attempts_record_attempt()/_record_failure()/
    # _clear()) have no call site yet -- the boot-time glue
    # (pico_auto_update_state.c, docs/PICO_AUTO_UPDATE_PLAN.md step 3) that
    # will call them is a separate, later commit. When wired, its only
    # caller is the synchronous boot path in main_control_bringup.c, before
    # normal task concurrency exists -- Pattern 3 (init-time only), same
    # shape as boot_guard.c's own entry above. Until then this is dead code
    # with no PSRAM-stack exposure at all (unreachable).
    # PICO_AUTO_UPDATE_PLAN.md G1 (2026-09-18). Pattern 3 (internal-SRAM-
    # stack caller, reached live, not init-time) -- same shape as
    # estop_verification.c's entry above. pico_image_manifest_store() has
    # exactly one caller, ota_http_pico.c's ota_pico_do_stage(), which runs
    # on the httpd task (internal SRAM, never PSRAM, never the flash worker),
    # so it neither dispatches through uart_bridge_ext_run_on_flash_worker()
    # nor can be re-entered from it. pico_image_manifest_load() (the boot
    # task's only use) writes nothing. _clear() has no caller yet. The record
    # is 16 bytes written once per staged image, not a hot write path.
    "pico_image_manifest.c",
    "pico_update_attempts.c",
    # Pattern 2 (local caller_stack_is_external() guard), see this file's
    # own comment mirroring kiln_cfg_store.c's.
    "profile_executor_firing_stats.c",
    # Pattern 2, borrowed rather than duplicated: firing_shadow_finish_firing()
    # (ITER_TUNE_REDESIGN_PLAN.md step 8) has no caller_stack_is_external()
    # check of its own -- its one call site, profile_executor_firing_stats.c's
    # firing_stats_persist(), already refuses and returns BEFORE calling it
    # when the calling task's stack is external (see that function's own
    # guard, just above firing_shadow_finish_firing()'s call). Adding a
    # second, redundant check here would just be pattern 2 duplicated across
    # a call boundary with no additional caller.
    "firing_shadow.c",
    # RE-JUSTIFIED 2026-09-06 (flash-safety review of the hal_kv migration):
    # the "init-time only" claim below was FALSE -- profiles_builtin_start()
    # is init-time, but profiles_builtin_set_hidden()/_restore_all() (this
    # file's OTHER two write call sites) are reached live, long after boot,
    # from profiles_edit_http.c:505/520 (POST /api/profiles/builtin/hidden,
    # POST .../restore) and ui_page_profiles.c:50 (the "restore all" LCD
    # button). Actually Pattern 3 (internal-SRAM-stack caller, not init-time
    # concurrency-free-ness): profiles_edit_http.c's two call sites run on
    # the httpd task -- internal-SRAM stack, same established fact
    # zones_config_store.c's/unit_pref.c's/display_power_cfg.c's own entries
    # below rely on -- and ui_page_profiles.c's call site runs on the LVGL
    # task, whose stack is `static StackType_t s_lvgl_task_stack[...]`
    # (lvgl_port.c, xTaskCreateStaticPinnedToCore) -- a plain static array,
    # .bss-resident, never PSRAM. No caller of either write function reaches
    # it from a PSRAM-stacked task.
    "profiles_builtin.c",
    # Pattern 3 (internal-SRAM-stack caller, reached live, not init-time),
    # the same shape and the same established facts as profiles_builtin.c's
    # entry directly above -- this file stores the favorite masks the way
    # that one stores the hidden mask. Three write call sites, one per
    # caller: profiles_favorites_start() is init-time, called once from
    # main_network_http.c's bringup on app_main's own task before the httpd
    # task exists; profiles_favorites_set() is reached live from
    # profiles_edit_http.c's POST /api/profile/favorite handler and from the
    # dangling-favorite cleanup at the end of its profile_delete_post_handler
    # -- both on the httpd task, internal-SRAM stack, the same fact
    # profiles_builtin.c's/zones_config_store.c's entries already rely on.
    # No caller reaches either function from a PSRAM-stacked task, and
    # neither runs on the flash worker, so there is no re-entrancy path.
    "profiles_favorites.c",
    # Pattern 2 (local caller_stack_is_external() guard), added after the
    # "earlier pass treated this file as always-internal-stack" incident --
    # see this file's own comment.
    "profiles_http.c",
    # RE-JUSTIFIED 2026-09-06 (flash-safety review of the hal_kv migration):
    # the "init-time only" claim below was FALSE for one of this file's two
    # write call sites -- ramp_assist_cfg_start() runs at boot from
    # app_main, but ramp_assist_cfg_set_enabled() is reached live from
    # diagnostics_http.c:455 (POST the ramp-assist debug toggle), long after
    # boot. Actually Pattern 3 (internal-SRAM-stack caller): the httpd task
    # diagnostics_http.c's handler runs on has an internal-SRAM stack, the
    # same established fact zones_config_store.c's/unit_pref.c's/
    # display_power_cfg.c's/profiles_builtin.c's own entries in this list
    # rely on -- not a PSRAM-stacked task.
    "ramp_assist_cfg.c",
    # Pattern 2 (local caller_stack_is_external() guard), added when the
    # guard was introduced -- see this file's own comment; also called once
    # from app_main's own task before the scheduler starts.
    "relay_cycles.c",
    # Pattern 2 (local caller_stack_is_external() guard), same story as
    # relay_cycles.c -- see this file's own comment.
    "run_state.c",
    # Pattern 2 (local caller_stack_is_external() guard); dispatches
    # through uart_bridge_ext_run_on_flash_worker() otherwise -- see this
    # file's own comment.
    "safety_cfg_store.c",
    # Pattern 3 (internal-SRAM-stack httpd task, not init-time): the only
    # write call site is setup_wizard_progress_set_step()'s persist_all(),
    # reached exclusively from setup_progress_http.c's POST
    # /api/setup/progress handler -- registered on the same httpd instance
    # (wifi_provision_http_get_server()) settings_http.c's POST handlers
    # run on, same internal-SRAM-stack story as display_power_cfg.c's/
    # unit_pref.c's/time_sync.c's own entries in this list. Never called
    # from app_main/boot, never dispatched through
    # uart_bridge_ext_run_on_flash_worker(), so neither the PSRAM-stack nor
    # the re-entrancy half of this lint applies -- only the direct-hal_kv-
    # write half, which this entry covers the same way those three files'
    # entries do.
    "setup_wizard_progress.c",
    # Pattern 3 (init-time only): time zone save runs from the settings
    # HTTP handler's own internal-SRAM-stack httpd task, no PSRAM stack
    # involved in this handler's call chain.
    "time_sync.c",
    # Pattern 3 (init-time only): touch calibration is saved once from the
    # commissioning flow's own internal-SRAM-stack task.
    "touch_cal_store.c",
    # Pattern 3 (init-time only): unit preference save runs from the
    # settings HTTP handler's internal-SRAM-stack httpd task.
    "unit_pref.c",
    # Pattern 3 (internal-SRAM-stack caller, reached live, not init-time) --
    # same shape as display_power_cfg.c's/unit_pref.c's/zones_config_store.c's
    # own entries. iter_tune_store_set_zone()'s only write call site is
    # iter_tune_restore_post_handler() (drivers/http/iter_tune_http.c, POST
    # /api/iter_tune/restore_commissioned), registered on
    # wifi_provision_http_get_server()'s httpd instance -- config.stack_size
    # = 8192 (wifi_provision_http.c:1151), a plain stack_size on
    # HTTPD_DEFAULT_CONFIG() with no MALLOC_CAP_SPIRAM task-creation flag, the
    # same internal-SRAM-stack fact those other files' entries rely on.
    # iter_tune_store_start() (the file's other write site, via
    # nvs_save_raw() inside its file/NVS rev-reconciliation) runs once from
    # iter_tune_http_start(), called synchronously during bringup before this
    # httpd server itself is even created -- no concurrency, no PSRAM stack.
    # Neither call site dispatches through
    # uart_bridge_ext_run_on_flash_worker(), so the re-entrancy half of this
    # lint does not apply; this covers only the direct-hal_kv-write and
    # cfg_fs-write halves (see CFG_FS_ALLOWLIST's twin entry below).
    "iter_tune_store.c",
    # Pattern 3 (internal-SRAM-stack caller, reached live, not init-time) --
    # same shape as estop_verification.c's/display_power_cfg.c's own
    # entries. web_auth_store_set_password()/_set_pin()/_set_policy() (via
    # set_blob_verified()) are called only from the password/PIN-entry
    # httpd handlers (owned by the web-auth password-page and LCD-keypad
    # slices) and from the physical-credential-reset handler -- all
    # internal-SRAM-stack httpd/LVGL tasks, never PSRAM, never the flash
    # worker, so neither the caller_stack_is_external() guard nor worker
    # dispatch applies here.
    "web_auth_store.c",
    # Pattern 3 (internal-SRAM-stack caller, reached live, not init-time) --
    # same shape as web_auth_store.c's own entry just above.
    # docs/TOTP_PASSWORD_RESET_PLAN.md WT-A part 2: set_blob_verified()/
    # erase_key()'s hal_kv_set_blob()/hal_kv_erase_key()/hal_kv_commit()
    # calls are reached only from security_http.c's totp_enroll_confirm/
    # totp_disable cmd= handlers, auth_totp_http.c's POST /api/auth/forgot
    # handler (totp_config_verify_and_consume() persists the used counter),
    # and auth_reset_gesture_wiring.c's physical four-corner reset path --
    # all running on the same
    # wifi_provision_http_get_server() httpd instance's worker task
    # (config.stack_size = 8192, wifi_provision_http.c:1151, a plain
    # internal-DRAM stack_size, never PSRAM) or, for the gesture path, the
    # LVGL task's static `s_lvgl_task_stack[]` (lvgl_port.c), also never
    # PSRAM. Never called from app_main/boot, never dispatched through
    # uart_bridge_ext_run_on_flash_worker(), so neither the PSRAM-stack nor
    # the re-entrancy half of this lint applies here.
    "totp_config.c",
    # Pattern 3 (init-time only): watchdog config save runs once from
    # app_main's own task before the scheduler starts.
    "watchdog_cfg.c",
    # Pattern 3 (init-time only): wifi_prov_start() deliberately stays a
    # plain call from app_main's own task -- see this file's own comment.
    "wifi_prov.c",
    # NOT actually init-time-only, unlike the entry above: nvs_save_*()
    # here (nvs_save_saved_nets()/nvs_save_mode()/nvs_save_ap_ssid()/
    # nvs_save_ap_password()/nvs_save_ip_config()) run from command
    # handlers in wifi_prov_api.c, on every add/forget-network, mode, AP-
    # identity and IP-mode change -- moved verbatim from wifi_prov.c's own
    # command handlers in the 2026-09-04 split (f9341e9), where they
    # already worked the same way. Safe because every one of them, and
    # every caller in wifi_prov_api.c/wifi_prov_link.c, runs exclusively on
    # owner_task() (see wifi_prov.c's DEADLOCK RULE comment and
    # wifi_prov.c:399's xTaskCreatePinnedToCore(owner_task, ...) -- a
    # plain, non-WithCaps create, so its stack is internal SRAM, never
    # PSRAM), which is wifi_prov's single serializing writer task, not a
    # PSRAM-stacked httpd/handler task.
    "wifi_prov_nvs.c",
    # Pattern 3 (init-time only): zones/relay-name/zone-normals config
    # loads/saves run from the settings HTTP handler's own
    # internal-SRAM-stack httpd task.
    "zones_config_store.c",
    # cfg_fs.c DEFINES cfg_fs_write_atomic()/cfg_fs_delete() -- it is the
    # primitive layer CFG_FS_CALL_RE exists to gate callers of, not a caller
    # of them itself (its own writes go through LittleFS's fopen/fwrite/
    # rename, which this lint has no reason to model). Its two definition
    # lines are excluded from CFG_FS_CALL_RE matching by
    # CFG_FS_DEFINITION_RE below (so this allowlist entry is documentation,
    # not the thing doing the work) -- listed anyway so a future direct
    # cfg_fs_write_atomic()/cfg_fs_delete() CALL added to this file (as
    # opposed to another definition) still has to be reasoned about like any
    # other caller, the same way kiln_cfg_store.c's own nvs_save_store()
    # entry above does not exempt a second, different call site in that file.
    "cfg_fs.c",
    # Pattern 1 (worker dispatch): cfgfs_file_post_handler()'s
    # cfg_fs_write_atomic() call (job.name/job.bytes/job.len, dispatched via
    # cfgfs_file_write_job()) checks uart_bridge_ext_is_on_flash_worker()
    # first, same shape as adaptive_tune.c's entry above -- see this file's
    # own comment on cfgfs_file_post_handler() ("Same flash-worker dispatch
    # shape relay_cycles_reset()/factory_reset.c use").
    "diagnostics_http.c",
    # profiles_cfg_fs_delete() (profiles_cfg_fs.c) is the sanctioned
    # generic wrapper -- see profiles_cfg_fs.c's own entry below for the
    # write_fn/delete_fn indirection story; this file's only cfg_fs-surface
    # call site is THROUGH that wrapper, never the bare cfg_fs_delete().
    "profiles_http.c",
}

# ---- cfg_fs (LittleFS-backed) write/delete/format surface -------------
# Extended 2026-09-07 (FILESYSTEM_PLAN.md's `cfg_fs` write paths) -- see
# this file's module banner "EXTENDED" section for the full story. Kept as
# its own regex/allowlist pair, not folded into WRITE_CALL_RE/ALLOWLIST
# above, because the two surfaces have different definers: nvs_set_*()/
# hal_kv_*()/esp_partition_*() are never DEFINED anywhere under drivers/ (so
# WRITE_CALL_RE never needs a definition exclusion), but cfg_fs_write_atomic()/
# cfg_fs_delete() ARE defined in-tree (cfg_fs.c) -- folding the two together
# would require every future WRITE_CALL_RE addition to also worry about
# definition-line exclusion, which today it correctly does not have to.
# Excludes cfg_fs_format_is_stalled() specifically -- found 2026-09-08 as a
# genuine false positive, not a needs-allowlisting case. It is a pure
# predicate (cfg_fs_status.c/.h -- this module's own header banner: "Pure
# and host-testable... it never edits or reaches into cfg_fs.c's/
# cfg_fs_mount.c's internals") that takes already-computed in_progress/
# elapsed_ms values and returns a bool; it performs no cfg_fs I/O of any
# kind, so it is not a member of the write/delete/format surface this regex
# exists to catch at all. Allowlisting the whole file (cfg_fs_status.c) the
# way a real dispatched-write call site would be would misrepresent a
# non-hazard as a reviewed bypass, so this is a precise name exclusion
# instead -- not a widened `\w*` -- so a real FUTURE cfg_fs_format*() call
# added anywhere is still caught.
CFG_FS_CALL_RE = re.compile(
    r"\b(cfg_fs_write_atomic(?:_device)?|cfg_fs_delete|"
    r"cfg_fs_format(?!_is_stalled\b)\w*)\s*\("
)

# Excludes the two lines in cfg_fs.c that DEFINE cfg_fs_write_atomic()/
# cfg_fs_delete() (esp_err_t cfg_fs_write_atomic(const char *rel_path, ...)
# -- a bare return-type-then-name-then-'(' on its own line, never how a call
# site is written in this codebase's style) from being mistaken for a call
# to themselves. Deliberately narrow (anchored at line start, requires a
# recognizable C type token first) rather than a blanket "this file is
# exempt" special case, so a real call site later ADDED to cfg_fs.c (e.g. if
# it grew a second internal helper that calls cfg_fs_write_atomic()) would
# still be caught.
CFG_FS_DEFINITION_RE = re.compile(
    r"^\s*(?:static\s+)?[A-Za-z_]\w*\s+(?:cfg_fs_write_atomic(?:_device)?|cfg_fs_delete|cfg_fs_format\w*)\s*\("
)

CFG_FS_ALLOWLIST = {
    # See CFG_FS_ALLOWLIST's twin entries in ALLOWLIST above for the same
    # filenames -- kept as a separate set (not merged) because a file can be
    # justified for the nvs_set_*/hal_kv_* surface without having reasoned
    # about the cfg_fs_* surface at all, or vice versa, and merging them
    # would let a justification for one silently cover the other.
    "cfg_fs.c",
    "cfg_fs_mount.c",
    "diagnostics_http.c",
    "profiles_http.c",
    "pref_cfg_fs.c",
    "profiles_cfg_fs.c",
    "zones_config_cfg_fs.c",
    # cfg_fs_save_raw()'s cfg_fs_write_atomic() call -- see ALLOWLIST's twin
    # entry above for the httpd-task/internal-SRAM-stack reasoning; same call
    # site, same caller (iter_tune_store_set_zone()), just the cfg dual-write
    # half of the same write instead of the NVS half.
    "iter_tune_store.c",
    # firing_stats_cfg_fs_delete() (PROFILE_SLOTS_100_PLAN.md sec 7 task 10):
    # deletes the firing-history mirror file for a profile id being erased,
    # called only from profile_executor_firing_stats.c's firing_stats_erase(),
    # itself only reached through profiles_http.c's nvs_erase_slot() -- same
    # caller-guarded shape as profiles_cfg_fs.c's own delete path above.
    "firing_stats_cfg_fs.c",
    # Factory-reset scope mirror sweeps (kiln_scope_cfg_files_delete(),
    # profiles_scope_cfg_files_delete(), profiles_builtin_discard_file()).
    # Pattern 1, by transitivity: each is called ONLY from factory_reset.c's
    # execute_scope_job() (the three call sites at its delete_kiln_cfg_files/
    # delete_profiles_cfg_files/restore_builtin_profiles branches), and that
    # job runs ON the flash worker -- execute_scope() either calls it inline
    # after uart_bridge_ext_is_on_flash_worker() or dispatches it via
    # uart_bridge_ext_run_on_flash_worker(), whichever of its two callers
    # (the httpd reset handler, the UART SYSTEM bridge task) reaches it. The
    # worker has an internal-SRAM stack and the re-entrancy guard is at the
    # dispatcher, so these helpers neither dispatch nor run on a caller's
    # (possibly PSRAM) stack. The same job already calls
    # cfg_fs_confirm_format_device() the same way. These helpers are public
    # (exported through their headers) but must not gain another caller
    # without its own review.
    "kiln_scope_cfg_files.c",
    "profiles_scope_cfg_files.c",
    # profiles_builtin.c is already justified for the hal_kv_* surface (see
    # ALLOWLIST: httpd/LVGL internal-SRAM callers); its cfg_fs_delete() is the
    # one bare cfg_fs call in the file, profiles_builtin_discard_file(), whose
    # only caller is factory_reset.c's execute_scope_job() on the flash worker
    # (same reasoning as the two entries above). Its other file writes go
    # through pref_cfg_fs_save(), not a bare cfg_fs_* call. NOTE: this
    # allowlist is per FILE, so this entry also exempts any FUTURE bare
    # cfg_fs_* write added anywhere in profiles_builtin.c; such an addition
    # needs its own justification here.
    "profiles_builtin.c",
}

WRITE_CALL_RE = re.compile(
    r"\b(nvs_set_\w+|nvs_commit|hal_kv_set_\w+|hal_kv_commit|hal_kv_erase_\w+|"
    r"esp_partition_write(?:_raw)?|esp_partition_erase_range)\s*\("
)

# ---- flash-worker dispatch re-entrancy scan ----------------------------
# Extended 2026-09-07. Every uart_bridge_ext_run_on_flash_worker() call site
# must show ONE of: (a) a nearby uart_bridge_ext_is_on_flash_worker() check
# (the sanctioned re-entrant-safe shape adaptive_tune.c/factory_reset.c/
# relay_cycles.c/safety_cfg_store.c/diagnostics_http.c all use), or (b) the
# justification phrase "not reachable on-worker" in a nearby comment (the
# phrase log_store_mount.c's and autotune_engine_step_identify.c's dispatch
# sites already used before this rule existed -- reused verbatim rather than
# inventing a new tag). See the module banner for why this is a "static
# analysis cannot decide, require an explicit comment" rule rather than an
# attempt at real call-graph reachability analysis.
DISPATCH_CALL_RE = re.compile(r"\buart_bridge_ext_run_on_flash_worker\s*\(")
# Several files hand-declare this function's prototype rather than
# #include-ing flash_worker.h (relay_cycles.c/safety_cfg_store.c/
# factory_reset.c/diagnostics_http.c all do this -- see e.g. factory_reset.c's
# own comment on why). That declaration line, `esp_err_t
# uart_bridge_ext_run_on_flash_worker(void (*fn)(void *arg), void *arg);`
# (with or without a leading `extern`), textually matches DISPATCH_CALL_RE
# but is not a call site -- its parameter list is always a bare function-
# pointer TYPE, never an actual `(some_fn, &some_arg)` argument pair the way
# every real call site in this codebase is written. Anchored on that shape
# specifically (not just "any line with this name") so a real call that
# happened to start a line the same way would still be caught.
DISPATCH_DECLARATION_RE = re.compile(
    r"^\s*(?:extern\s+)?esp_err_t\s+uart_bridge_ext_run_on_flash_worker\s*\(\s*void\s*\(\s*\*"
)
REENTRANCY_GUARD_RE = re.compile(r"uart_bridge_ext_is_on_flash_worker\s*\(")
REENTRANCY_JUSTIFICATION_RE = re.compile(r"not reachable on-worker", re.IGNORECASE)
REENTRANCY_CONTEXT_LINES = 15  # lines of lookback for the guard/justification

# Matches this file's own definitions/declarations of the sanctioned
# dispatch wrappers so a match inside uart_bridge_ext.c itself (which never
# calls nvs_*()/esp_partition_*() directly -- it only dispatches to them)
# is never a false hit in the first place; kept as a comment, not code,
# since nothing here currently needs it -- uart_bridge_ext.c has zero
# nvs_*/esp_partition_* call sites of its own (verified 2026-09-04).



# Matches a C string literal, "..." with \" and \\ escapes handled, so a
# log-message mention of a function name (e.g. ESP_LOGE(TAG, "hal_kv_erase_
# partition('%s') failed: %s", ...)) is never mistaken for a real call site.
# Deliberately does not also strip char literals ('x') -- none of this
# lint's target function names could ever appear inside one, so there is
# nothing for that to protect against, and handling it would only add an
# unused edge case (an unescaped `'` inside a string, which the regex below
# does not need to worry about since it only enters string mode on `"`).
STRING_LITERAL_RE = re.compile(r'"(?:\\.|[^"\\])*"')


def strip_string_literals(line: str) -> str:
    return STRING_LITERAL_RE.sub('""', line)


def stripped_lines(path: Path):
    """Yields (lineno, stripped_line, raw_line) with block/line comments and
    string literals removed -- shared by every scan below so the three
    checks (WRITE_CALL_RE, CFG_FS_CALL_RE, DISPATCH_CALL_RE/guard search) see
    an identical view of the file rather than three slightly-different
    comment-stripping implementations drifting apart over time."""
    text = path.read_text(encoding="utf-8", errors="replace")
    in_block_comment = False
    for lineno, raw_line in enumerate(text.splitlines(), start=1):
        line = raw_line
        # Strip block comments (best-effort, line-oriented -- good enough
        # for this repo's style, which never straddles a call across a
        # /* */ boundary).
        if in_block_comment:
            end = line.find("*/")
            if end == -1:
                yield (lineno, "", raw_line)
                continue
            line = line[end + 2:]
            in_block_comment = False
        start = line.find("/*")
        if start != -1:
            end = line.find("*/", start + 2)
            if end == -1:
                in_block_comment = True
                line = line[:start]
            else:
                line = line[:start] + line[end + 2:]
        # Strip line comments.
        cpos = line.find("//")
        if cpos != -1:
            line = line[:cpos]
        # Strip string literals (2026-09-06 flash-safety review): the widened
        # regex + rglob pass started matching function-name mentions inside
        # ESP_LOGE()/ESP_LOGW() message text (e.g. factory_reset.c's
        # "hal_kv_erase_partition('%s') failed: %s") as if they were real
        # call sites -- a log message naming a function is not a call to it.
        line = strip_string_literals(line)
        yield (lineno, line, raw_line)


def scan_file(path: Path):
    """WRITE_CALL_RE violations (nvs_set_*/hal_kv_*/esp_partition_*)."""
    violations = []
    for lineno, line, raw_line in stripped_lines(path):
        if WRITE_CALL_RE.search(line):
            violations.append((lineno, raw_line.strip()))
    return violations


def scan_cfg_fs(path: Path):
    """CFG_FS_CALL_RE violations (cfg_fs_write_atomic()/cfg_fs_delete()/
    cfg_fs_format*()), excluding the in-tree DEFINITION lines (cfg_fs.c)."""
    violations = []
    for lineno, line, raw_line in stripped_lines(path):
        if CFG_FS_DEFINITION_RE.match(line):
            continue
        if CFG_FS_CALL_RE.search(line):
            violations.append((lineno, raw_line.strip()))
    return violations


def scan_reentrancy(path: Path):
    """Every uart_bridge_ext_run_on_flash_worker() dispatch call site must
    show a nearby is_on_flash_worker() guard or "not reachable on-worker"
    justification within REENTRANCY_CONTEXT_LINES lines above it (comments
    included -- the justification IS a comment, so this check does not use
    stripped_lines() for the lookback, only for finding the dispatch call
    itself, matching the other two scans' comment-immune call detection)."""
    violations = []
    raw_lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    stripped = list(stripped_lines(path))
    for lineno, line, raw_line in stripped:
        if not DISPATCH_CALL_RE.search(line):
            continue
        if DISPATCH_DECLARATION_RE.match(line):
            continue
        lo = max(0, lineno - 1 - REENTRANCY_CONTEXT_LINES)
        hi = lineno  # 0-based slice end == this line's own index+1
        context = "\n".join(raw_lines[lo:hi])
        if REENTRANCY_GUARD_RE.search(context) or REENTRANCY_JUSTIFICATION_RE.search(context):
            continue
        violations.append((lineno, raw_line.strip()))
    return violations


def main(argv):
    default_drivers = Path(__file__).resolve().parent.parent / "drivers"
    drivers_dir = Path(argv[1]) if len(argv) > 1 else default_drivers
    if not drivers_dir.is_dir():
        # SKIP (exit 3), not FAIL -- tools/run_all_checks.ps1's reserved
        # skip status for "missing prerequisite" (see
        # stub_signature_drift_check.py's identical convention). A missing
        # drivers_dir means this check has nothing to scan, which is a
        # different fact than "scanned it and found nothing wrong" -- the
        # old exit-1-on-missing-dir behavior collapsed those two into one
        # code, and exit 1 for "wrong argument" reads identically to exit 1
        # for "found a violation" to anything just checking $LASTEXITCODE.
        print(f"flash_worker_lint: SKIP -- drivers dir not found: {drivers_dir}", file=sys.stderr)
        return 3

    # rglob, not glob: the 2026-09 drivers/ layering reorg (tools/drivers_reorg/)
    # split every file that used to live flat in drivers/*.c into subdirectories
    # (drivers/persist/, drivers/control/, drivers/http/, ...) -- drivers_dir
    # itself now contains ZERO .c files directly. A plain glob("*.c") silently
    # scans nothing and this lint reports "clean" no matter what any driver
    # file does, exactly the "splits break filename-keyed checks" class (see
    # project memory / b9a5112) this repo has hit before. Found and fixed
    # 2026-09-06 auditing HW_ABSTRACTION.md Phase 3 item 3 (nvs.h ->
    # hal_kv.h migration) -- this lint had been vacuously passing since the
    # reorg landed.
    all_driver_files = sorted(drivers_dir.rglob("*.c"))
    write_violations = []
    cfg_fs_violations = []
    reentrancy_violations = []
    for c_file in all_driver_files:
        rel = c_file.relative_to(drivers_dir.parent)
        if c_file.name not in ALLOWLIST:
            for lineno, text in scan_file(c_file):
                write_violations.append(f"{rel}:{lineno}: {text}")
        if c_file.name not in CFG_FS_ALLOWLIST:
            for lineno, text in scan_cfg_fs(c_file):
                cfg_fs_violations.append(f"{rel}:{lineno}: {text}")
        # Re-entrancy scan runs on EVERY file, allowlisted or not: the
        # allowlists above are about whether a stack is safe to write flash
        # from, which is orthogonal to whether a given dispatch call risks
        # deadlocking a caller already on the worker -- a file can be
        # correctly allowlisted for the first and still be missing the
        # second (that is exactly what this rule caught in cfg_fs_mount.c).
        for lineno, text in scan_reentrancy(c_file):
            reentrancy_violations.append(f"{rel}:{lineno}: {text}")

    if write_violations or cfg_fs_violations or reentrancy_violations:
        if write_violations:
            print("FLASH WORKER LINT: direct flash/NVS write(s) outside the allowlist:")
            for v in write_violations:
                print(f"  {v}")
            print("See this file's header comment for the three sanctioned patterns")
            print("and how to add a justified allowlist entry.")
        if cfg_fs_violations:
            print("FLASH WORKER LINT: direct cfg_fs write/delete/format call(s) outside")
            print("the CFG_FS_ALLOWLIST:")
            for v in cfg_fs_violations:
                print(f"  {v}")
            print("Add a CFG_FS_ALLOWLIST entry naming the sanctioned pattern, same as")
            print("ALLOWLIST above.")
        if reentrancy_violations:
            print("FLASH WORKER LINT: uart_bridge_ext_run_on_flash_worker() dispatch(es)")
            print("with no nearby is_on_flash_worker() guard or \"not reachable on-worker\"")
            print("justification comment -- a caller already on the worker at this call")
            print("site deadlocks the board:")
            for v in reentrancy_violations:
                print(f"  {v}")
            print("Add the guard, or a comment containing the exact phrase")
            print("\"not reachable on-worker\" explaining why this dispatch's caller can")
            print("never already be on the worker.")
        return 1

    print(f"flash_worker_lint: clean ({len(all_driver_files)} driver files scanned, "
          f"{len(ALLOWLIST)} write-allowlisted, {len(CFG_FS_ALLOWLIST)} cfg_fs-allowlisted)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
