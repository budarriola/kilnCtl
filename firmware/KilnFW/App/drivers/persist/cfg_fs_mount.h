// cfg_fs_mount -- device-only glue between cfg_fs.c (pure, portable,
// host-testable) and the real hardware: registers the `cfg` LittleFS
// partition at "/cfg" and routes writes through
// uart_bridge_ext_run_on_flash_worker() so a PSRAM-stacked caller never
// touches flash directly -- exact same pattern as log_store_mount.c/.h for
// the `logs` SPIFFS partition; see that file's header comment for the
// underlying HAZARD this sidesteps.
//
// The `cfg` partition does not exist in partitions.csv yet (a separate,
// owner-gated step per docs/FILESYSTEM_USER_DATA.md section 5 step 1 --
// adding it requires an otadata erase + bootloader reflash and is out of
// scope for this pass). esp_vfs_littlefs_register() below is written to
// FAIL GRACEFULLY when the partition is absent (ESP_ERR_NOT_FOUND, handled
// like any other mount failure per the plan's mount-failure contract) --
// that is today's real state on every board, not a hypothetical error path.
//
// Not part of any host test build -- it #includes esp_littlefs.h and
// uart_bridge.h, neither of which exists off-target. cfg_fs_mount_or_skip()
// (cfg_fs.h), the recovery-mode gate this file delegates to, IS host-tested
// directly with a bool in place of boot_guard_is_recovery_mode().
#ifndef CFG_FS_MOUNT_H
#define CFG_FS_MOUNT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers the `cfg` LittleFS partition at "/cfg"
 * (format_if_mount_failed = FALSE -- opposite of log_store_mount()'s `logs`
 * policy on purpose, see cfg_fs.h's file banner) and calls
 * cfg_fs_mount_or_skip(boot_guard_is_recovery_mode(), "/cfg", ...).
 * Idempotent. Never blocks boot: a missing partition, a corrupt filesystem,
 * or recovery mode all leave cfg_fs in a state where every cfg_fs_*() call
 * fails cleanly with ESP_ERR_INVALID_STATE rather than crashing or hanging,
 * exactly like log_store's own contract when `logs` fails to mount. Logs
 * loudly (ESP_LOGE) on failure so a mount problem is visible in the boot
 * log, and reports the `.tmp/` sweep count from cfg_fs_init() at INFO. */
esp_err_t cfg_fs_mount_device(void);

/* Thin wrapper that hands the write to cfg_fs_write_atomic() on the flash
 * worker task (uart_bridge_ext_run_on_flash_worker()) rather than the
 * caller's own stack/task -- same reasoning as log_store_write_event().
 * `data` may point at the caller's own stack buffer: the caller is blocked
 * for the whole call, so that storage stays valid throughout.
 *
 * HAZARD: never call this from a handler already running ON the flash
 * worker task -- that deadlocks the board (project_flash_worker_reentrancy).
 * No caller does that today; this module has no callers yet at all (this
 * pass is the foundation only -- see docs/FILESYSTEM_USER_DATA.md
 * section 5, steps 3+ move real data through this entry point). */
esp_err_t cfg_fs_write_atomic_device(const char *rel_path, const void *data, size_t len);

/* AUTO-FORMAT / ASK-FIRST (docs/FILESYSTEM_USER_DATA.md section 5 step
 * 1, owner decision 2026-09-07): cfg_fs_mount_device() no longer just reports
 * a mount failure and stops. When esp_vfs_littlefs_register() fails, it reads
 * the raw partition back (cfg_fs_format_gate.h, host-tested) and:
 *   - if the region shows no evidence of real content (reads as erased, give
 *     or take a handful of stray bits), formats it automatically and retries
 *     the mount -- this is today's REAL state on every board (the `cfg`
 *     partition is flashed but has never been written), so this path is what
 *     actually makes the config filesystem live for the first time;
 *   - if the region shows a LittleFS superblock signature or a meaningful
 *     fraction of non-erased bytes, it does NOT format -- it sets the
 *     "awaiting confirmation" flag below and leaves cfg_fs UNAVAILABLE,
 *     exactly like any other mount failure, so nothing is silently
 *     destroyed.
 * Either way this never blocks boot and never touches any partition other
 * than `cfg`. */
bool cfg_fs_mount_format_confirmation_pending(void);

/* Human-readable reason the last mount attempt refused to auto-format (e.g.
 * "LittleFS superblock signature found"), or "" if nothing is pending.
 * Surfaced by cfg_fs_format_http.c's GET /api/cfgfs/format_pending for the
 * web UI banner. */
const char *cfg_fs_mount_format_pending_reason(void);

/* Explicit operator confirmation: unconditionally erases and reformats the
 * `cfg` partition (regardless of what a prior scan found -- calling this IS
 * the confirmation) and mounts it fresh. Clears the awaiting-confirmation
 * flag on success. Used by two callers: cfg_fs_format_http.c's
 * POST /api/cfgfs/format_confirm (an operator explicitly acknowledging the
 * "appears to contain data" banner) and factory_reset.c's "all" scope (the
 * factory-reset button already IS the explicit operator action the plan
 * doc's mount-failure contract calls for -- see cfg_fs.h's file banner).
 *
 * Runs the actual erase/format on the flash worker task
 * (uart_bridge_ext_run_on_flash_worker()), same reasoning as
 * cfg_fs_write_atomic_device() above and factory_reset.c's execute_scope():
 * a PSRAM-stacked or small-stacked caller (httpd_worker) must never touch
 * flash directly. HAZARD: never call this from a handler already running ON
 * the flash worker task -- see cfg_fs_write_atomic_device()'s doc comment
 * for the deadlock this avoids (project_flash_worker_reentrancy). Never
 * blocks boot: this has no boot-time caller, only HTTP-triggered ones. */
esp_err_t cfg_fs_confirm_format_device(void);

/* DEFERRED AUTO-FORMAT (docs/audits/boot_hang_2026-09-08.md follow-up,
 * 2026-09-08): a blank `cfg` partition used to be formatted INLINE, on the
 * main boot task, inside cfg_fs_mount_device() -- a real esp_littlefs_format()
 * of the whole 512 KiB partition, with no bound and nothing feeding
 * rtc_watchdog.h's RTC watchdog until monitor_task.c starts several boot
 * phases later. A format anywhere near its ~51 s worst-case (128 4-KiB
 * sectors x ~400 ms datasheet-max erase each) blows past
 * RTC_WATCHDOG_TIMEOUT_MS (20 s) with nothing to feed it, so the board
 * resets mid-format and repeats -- a reset loop indistinguishable from a
 * true hang to anyone polling the board from outside.
 *
 * Fix: cfg_fs_mount_device() still runs the (fast, bounded -- a few ms) scan
 * inline, but when the scan concludes SAFE_TO_FORMAT it no longer formats
 * itself. It starts a dedicated low-priority background task (internal RAM
 * stack, never PSRAM -- see project_psram_stack_nvs_panic) and returns
 * immediately with cfg_fs still UNAVAILABLE; boot proceeds exactly like any
 * other mount failure. That task dispatches the actual format+register+
 * mount-finish onto the flash worker (uart_bridge_ext_run_on_flash_worker())
 * -- same call cfg_fs_confirm_format_device() uses -- blocking ITSELF, never
 * the boot task, for however long the erase actually takes. When it
 * completes (success or failure) it installs the device write functions on
 * success exactly like any other mount path, records start/end timestamps
 * and the result, and deletes itself.
 *
 * These getters back GET /api/cfgfs's "format" section (cfg_fs_status.h's
 * cfg_fs_format_progress_t) so a slow-but-progressing format is visible and
 * distinguishable from a stall, instead of nobody being able to tell the
 * difference (exactly what happened the night this was written). None of
 * these functions block or touch flash. */
bool cfg_fs_mount_format_ever_started(void);
bool cfg_fs_mount_format_in_progress(void);
bool cfg_fs_mount_format_completed(void);
esp_err_t cfg_fs_mount_format_result(void); /* meaningful only once cfg_fs_mount_format_completed() is true */
uint32_t cfg_fs_mount_format_elapsed_ms(void); /* time so far if in progress, final duration once completed */

#ifdef __cplusplus
}
#endif

#endif // CFG_FS_MOUNT_H
