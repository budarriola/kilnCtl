// cfg_fs -- foundation storage module for the `cfg` LittleFS partition
// (docs/FILESYSTEM_USER_DATA.md section 3/5, step 1-2). Provides
// mount/unmount, read-to-buffer, atomic write, delete, exists, and list --
// the primitives every later user-data migration step (zones, profiles,
// kiln-config slots, prefs, ...) builds on. This file does NOT itself move
// any of that data; it only stands up the storage seam.
//
// SPLIT the same way log_store.c/log_store_mount.c are split: this file is
// pure stdio (fopen/fread/fwrite/remove/rename) plus stdint/string only --
// no ESP-IDF, no FreeRTOS -- so it host-tests directly against a real temp
// directory on disk (see test/test_cfg_fs.c), no filesystem stub needed.
// The device-only glue -- registering the LittleFS `cfg` partition and
// routing writes through the flash worker -- lives in cfg_fs_mount.c/.h,
// which this file's tests never link.
//
// MOUNT POLICY (docs/FILESYSTEM_USER_DATA.md "mount-failure contract",
// non-negotiable, mirrors log_store's "degrade, don't wedge" discipline but
// with the OPPOSITE format-on-failure choice):
//   - format_if_mount_failed = FALSE on the device side (cfg_fs_mount.c).
//     Silently reformatting tuning data is the worst possible failure --
//     unlike `logs`, a corrupt `cfg` filesystem is reported, never erased.
//   - Mount failure is NEVER fatal and NEVER blocks boot. cfg_fs_init()
//     returns an error, callers fall back to firmware defaults, and every
//     read/write call below becomes a clean no-op-with-error, exactly like
//     log_store_is_init()/log_store_append() already behave when the
//     partition never mounted.
//   - Recovery mode does not mount at all -- see cfg_fs_mount_or_skip()
//     below, which is host-testable directly (a bool parameter stands in
//     for boot_guard_is_recovery_mode(); the device glue in cfg_fs_mount.c
//     is the only place that reads the real boot_guard state).
//
// ATOMICITY (docs/FILESYSTEM_USER_DATA.md section 3, "one helper, no
// exceptions"): cfg_fs_write_atomic() writes to `<base>/.tmp/<name>`, fsyncs
// it, then renames it onto the final path. An interrupted write leaves
// either the old file (rename never happened) or the new one (rename
// completed) -- never a truncated file at the final path. `.tmp/` is swept
// at mount (cfg_fs_init()) to clear any crash residue from an interrupted
// write in a prior boot; the sweep count is returned so a nonzero count
// after a normal boot can be logged as a signal worth noticing (same idea
// as log_store's "degrades safely" discipline, applied to the write path
// instead of the read path).
//
// NOT DONE HERE: this module does not dispatch itself to the flash worker
// (uart_bridge_ext_run_on_flash_worker()) -- that is the device glue's job,
// exactly like log_store_write_event() wraps log_store_append() in
// log_store_mount.c. Calling cfg_fs_write_atomic() directly from a
// PSRAM-stacked task on-device would hit the same flash-write-from-PSRAM-
// stack panic documented for NVS; callers on-device must go through the
// glue, not this file directly.
#ifndef CFG_FS_H
#define CFG_FS_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CFG_FS_STATUS_UNMOUNTED = 0, /* cfg_fs_init() never called, or cfg_fs_deinit() called since */
    CFG_FS_STATUS_MOUNTED,       /* base directory usable; reads/writes go through */
    CFG_FS_STATUS_UNAVAILABLE,   /* init/mount attempted and failed -- callers must use defaults */
} cfg_fs_status_t;

#define CFG_FS_MAX_NAME 48

typedef struct {
    char name[CFG_FS_MAX_NAME]; /* bare filename, no directory component */
} cfg_fs_entry_t;

/* Initializes the pure storage layer against `base_dir` (a directory that
 * must already exist as a mounted filesystem root -- on-device this is
 * "/cfg" after cfg_fs_mount.c's esp_vfs_littlefs_register() succeeds; on
 * host this is any real temp directory the test creates). Sweeps
 * `<base_dir>/.tmp/` of any leftover files (crash residue from an
 * interrupted write in a previous boot) and reports how many were reaped in
 * `*out_tmp_reaped` (may be NULL if the caller does not care).
 *
 * Returns ESP_ERR_INVALID_ARG for a NULL/empty/oversized base_dir.
 * Returns ESP_OK and sets status to MOUNTED otherwise -- this function does
 * NOT itself attempt to create or format `base_dir`; that is the device
 * glue's job (or the test's, on host). If `base_dir` cannot be listed at
 * all (does not exist, not a directory), status becomes UNAVAILABLE and
 * ESP_FAIL is returned; every other cfg_fs_*() call then fails cleanly
 * with ESP_ERR_INVALID_STATE rather than crashing or blocking. */
esp_err_t cfg_fs_init(const char *base_dir, size_t *out_tmp_reaped);

/* Recovery-mode gate, host-testable without linking boot_guard.c: when
 * `recovery_mode` is true, this returns ESP_OK immediately WITHOUT calling
 * cfg_fs_init() at all (status stays UNMOUNTED) -- the real device glue
 * (cfg_fs_mount.c) calls this with boot_guard_is_recovery_mode() as the
 * argument, the same gate pattern main_boot_early.c already applies to
 * profile_executor/autotune_engine. Otherwise behaves exactly like
 * cfg_fs_init(). */
esp_err_t cfg_fs_mount_or_skip(bool recovery_mode, const char *base_dir, size_t *out_tmp_reaped);

/* Resets to CFG_FS_STATUS_UNMOUNTED and forgets `base_dir`. Test/teardown
 * use only -- there is no on-device call site for this today. */
void cfg_fs_deinit(void);

cfg_fs_status_t cfg_fs_get_status(void);
bool cfg_fs_is_available(void); /* true only when status == MOUNTED */

/* The one user-facing message for "a save was refused because cfg is not
 * mounted". cfg is the only save target (NVS is a read-only legacy source),
 * so every save route, and the readiness/LCD prompts, point at the same
 * remedy. A string literal on purpose: HTTP handlers concatenate it into JSON
 * bodies without a stack buffer. */
#define CFG_FS_NOT_MOUNTED_TEXT     "settings storage (cfg) not mounted - confirm format via POST /api/cfgfs/format_confirm"

/* Recovery-mode variant, chosen when cfg_fs_skipped_for_recovery() is true:
 * the partition was deliberately left unmounted and still holds the saved
 * config, so this text must NOT advise a format. */
#define CFG_FS_RECOVERY_SKIPPED_TEXT "settings storage (cfg) not mounted in recovery mode - leave recovery mode (POST /api/ota/esp/recovery_exit) to save settings; do not format"

/* True when this boot skipped the mount because boot_guard RECOVERY MODE is
 * active (cfg_fs_mount_or_skip(true, ...)) and cfg is still not mounted.
 * Since the NVS dual-write close the partition holds the board's only
 * up-to-date config, so leaving recovery mode is the remedy, never a format.
 * Cleared by cfg_fs_deinit() and by a non-recovery cfg_fs_mount_or_skip(). */
bool cfg_fs_skipped_for_recovery(void);

/* The refusal text for the current unmounted state (either literal above). */
static inline const char *cfg_fs_not_mounted_text(void)
{
    return cfg_fs_skipped_for_recovery() ? CFG_FS_RECOVERY_SKIPPED_TEXT : CFG_FS_NOT_MOUNTED_TEXT;
}

/* `rel_path` is always relative to the mounted base, e.g. "zones.json" or
 * "profiles/3.json" -- at most one '/' level of nesting is supported (the
 * plan's layout never nests deeper than one directory). Every function
 * below returns ESP_ERR_INVALID_STATE if cfg_fs is not MOUNTED, so a caller
 * that forgets to check cfg_fs_is_available() first still fails safely
 * instead of touching an uninitialized base path. */

esp_err_t cfg_fs_exists(const char *rel_path, bool *out_exists);

/* Reads the whole file into `buf` (capacity `cap`). *out_len is set to the
 * file's actual size whether or not it fit -- a caller can detect
 * truncation by comparing *out_len to cap. Returns ESP_ERR_NOT_FOUND if the
 * file does not exist, ESP_ERR_INVALID_SIZE if it is larger than `cap`
 * (nothing is copied into `buf` in that case -- no silent truncation). */
esp_err_t cfg_fs_read(const char *rel_path, void *buf, size_t cap, size_t *out_len);

/* Atomic write: `<base>/.tmp/<flattened rel_path>` written+fsynced, then
 * renamed onto `<base>/<rel_path>`. On any failure before the rename, the
 * temp file is removed and the ORIGINAL file at `rel_path` (if any) is left
 * completely untouched -- this is the guarantee this whole module exists to
 * provide. Creates the immediate parent directory of `rel_path` (and
 * `.tmp/`) if missing; does not create more than one level.
 *
 * After the rename, this function reopens `rel_path` and compares what is
 * actually on disk against `data`/`len` before returning ESP_OK -- same
 * verify-by-read-back discipline as boot_guard_mark_healthy()'s NVS clear
 * (docs/audits/boot_guard_recovery_loop_2026-09-08.md: a HAL_OK/success
 * return from a flash write was observed NOT to guarantee the bytes
 * actually changed). A mismatch or unreadable read-back returns ESP_FAIL
 * even though the rename itself reported success -- every caller of this
 * function (zones/profiles/prefs/firing-stats/kiln-cfg-store `cfg_fs`
 * dual-write paths) already treats a non-ESP_OK return as "did not
 * persist" and falls back / logs a warning, so this needed no caller-side
 * changes to take effect. */
esp_err_t cfg_fs_write_atomic(const char *rel_path, const void *data, size_t len);

esp_err_t cfg_fs_delete(const char *rel_path);

/* Lists regular files directly inside `rel_dir` ("" for the base directory
 * itself). `.tmp` is always excluded. Writes at most `max_out` entries to
 * `out`, sets `*out_count` to how many were written -- if the real count is
 * larger, `*out_count` is still clamped to `max_out` (no overflow), which a
 * caller can detect by noticing it got exactly `max_out` back. */
esp_err_t cfg_fs_list(const char *rel_dir, cfg_fs_entry_t *out, size_t max_out, size_t *out_count);

#ifdef __cplusplus
}
#endif

#endif // CFG_FS_H
