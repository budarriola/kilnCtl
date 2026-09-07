// cfg_fs_mount -- device-only glue between cfg_fs.c (pure, portable,
// host-testable) and the real hardware: registers the `cfg` LittleFS
// partition at "/cfg" and routes writes through
// uart_bridge_ext_run_on_flash_worker() so a PSRAM-stacked caller never
// touches flash directly -- exact same pattern as log_store_mount.c/.h for
// the `logs` SPIFFS partition; see that file's header comment for the
// underlying HAZARD this sidesteps.
//
// The `cfg` partition does not exist in partitions.csv yet (a separate,
// owner-gated step per docs/FILESYSTEM_USER_DATA_PLAN.md section 5 step 1 --
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

#include <stddef.h>

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
 * pass is the foundation only -- see docs/FILESYSTEM_USER_DATA_PLAN.md
 * section 5, steps 3+ move real data through this entry point). */
esp_err_t cfg_fs_write_atomic_device(const char *rel_path, const void *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif // CFG_FS_MOUNT_H
