// kiln_cfg_store_cfg_fs -- read-through/dual-write bridge between the saved
// "kiln config" slots' existing whole-store NVS blob (kiln_cfg_store.c's
// nvs_load_store()/nvs_save_store(), NVS_KEY_STORE="kilncfgs") and the `cfg`
// LittleFS partition (cfg_fs.h/cfg_fs_mount.h), per
// docs/FILESYSTEM_USER_DATA.md section 5's "kiln config slots" item
// (backup audit 031ededb: user-created data with an existing restore path).
//
// SCOPE: this file only decides WHICH bytes win (file vs NVS) and drives the
// file read/write -- it does not touch NVS itself. kiln_cfg_store.c's
// nvs_load_store()/nvs_save_store() still own the NVS side and call into
// this module.
//
// SHAPE: unlike profiles_cfg_fs.c (one file PER SLOT, because profiles are
// individually creatable/deletable NVS entries), the whole kiln config store
// -- every saved slot, active_id, next_id -- is ALREADY persisted as a
// single NVS blob under one key (see kiln_cfg_store.c). This module
// therefore copies zones_config_cfg_fs.c's shape instead: ONE file holding
// the whole document, one rev counter for the whole document. A per-slot
// save/delete/rename/clone/apply all funnel through kiln_cfg_store.c's
// single nvs_save_store(), so bumping the rev there once (see that
// function) already satisfies "bump the rev on both save and delete" for
// every slot at once -- there is no separate per-slot rev to keep in sync.
//
// FILE FORMAT: the file at KILN_CFG_STORE_FILE_PATH holds a 4-byte
// little-endian `rev` counter immediately followed by a byte-for-byte copy
// of kiln_cfg_store_blob_t, always at the CURRENT version -- unlike zones
// config, this store has no versioned wire-format migration chain of its
// own beyond kiln_cfg_store.c's own v1->v2 NVS migration (which only ever
// runs against the NVS blob; a `cfg`-file was never produced by a v1-era
// build, since this bridge module postdates v2). A file found at any OTHER
// version is simply treated as invalid -- ignored, NVS decides -- exactly
// like a corrupt or absent file, not migrated in place.
//
// READ-THROUGH POLICY / DIVERGENCE TIE-BREAK: identical to
// zones_config_cfg_fs.c's documented policy -- reads prefer the file when it
// decodes to a valid, current-version store; otherwise fall back to the
// caller's NVS candidate (and opportunistically migrate it to the file, if
// valid). When both sides are valid and their bytes differ, the file wins
// ONLY on a STRICTLY higher rev (file_rev > nvs_rev, never >=) -- an equal
// rev with differing bytes can only mean an NVS writer that predates the
// rev counter (rolled-back firmware, or a crash between the blob and rev
// writes), and in that case NVS is the newer copy. See
// check_cfg_fs_tie_break.ps1 and
// docs/audits/filesystem_migration_review_2026-09-07.md for the fuller
// version of this reasoning (found there as a real `>=` defect in
// zones_config_cfg_fs.c, since fixed). Always logged (ESP_LOGW) naming both
// revs. The loser is resynced from the winner's bytes so the divergence
// does not persist across boots.
//
// PARTITION ABSENT: cfg_fs_is_available() is false on every board today (no
// `cfg` partition mounted -- see cfg_fs_mount.c). Every function below
// checks it first and degrades to "use the NVS candidate, do nothing else,"
// exactly like zones_config_cfg_fs.c and host-tested the same way.
//
// STACK: kiln_cfg_store_blob_t is large (KILN_CFG_MAX_COUNT slots, each
// carrying a full ZONES_CONFIG_BLOB_MAX_SIZE zones-config blob) -- several
// KB, well past what this codebase's incident history
// (project_screen_idle_brick_real_cause, safety_poll's corrupted-slot panic)
// says is safe as a bare local array on a task stack. Every function in
// this module that needs a whole-document scratch buffer heap-allocates it
// (malloc/free), the same choice kiln_cfg_store.c's own v1-migration path
// already made for the identical reason (see that file's comment next to
// its malloc call) -- never a stack-resident copy of the whole struct.
#ifndef KILN_CFG_STORE_CFG_FS_H
#define KILN_CFG_STORE_CFG_FS_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "kiln_cfg_store_internal.h" /* kiln_cfg_store_blob_t */

#ifdef __cplusplus
extern "C" {
#endif

#define KILN_CFG_STORE_FILE_PATH "kiln_configs.json"

/* Matches cfg_fs_write_atomic()'s signature (cfg_fs.h) and
 * cfg_fs_write_atomic_device()'s (cfg_fs_mount.h) -- same seam
 * zones_config_cfg_fs.h/profiles_cfg_fs.h use. Defaults to
 * cfg_fs_write_atomic() (host-safe, no-op-if-unmounted). No on-device wiring
 * installs the device (flash-worker-dispatching) variant for THIS module
 * yet -- that wiring lives in cfg_fs_mount.c, out of this pass's scope; see
 * this task's report for the pending follow-up. */
typedef esp_err_t (*kiln_cfg_store_cfg_fs_write_fn_t)(const char *rel_path, const void *data, size_t len);
void kiln_cfg_store_cfg_fs_set_write_fn(kiln_cfg_store_cfg_fs_write_fn_t fn);
void kiln_cfg_store_cfg_fs_reset_write_fn_for_test(void);
kiln_cfg_store_cfg_fs_write_fn_t kiln_cfg_store_cfg_fs_get_write_fn(void);

/* Reads and decodes the file only, without any NVS comparison. *out_valid
 * convention matches zones_config_cfg_fs_load_raw(): OK == valid;
 * wrong-version/wrong-size/corrupt/absent/unreadable == not valid
 * (*out_blob zeroed, *out_rev 0). */
void kiln_cfg_store_cfg_fs_load_raw(kiln_cfg_store_blob_t *out_blob, uint32_t *out_rev, bool *out_valid);

/* Writes `blob` (must already be a valid, current-version struct -- this
 * function does not validate it) to the file at `rev`. No-op returning
 * ESP_ERR_INVALID_STATE if cfg_fs never mounted. */
esp_err_t kiln_cfg_store_cfg_fs_save(const kiln_cfg_store_blob_t *blob, uint32_t rev);

/* Core of the whole-document read-through policy described above.
 *
 *   nvs_blob/nvs_valid/nvs_rev -- what kiln_cfg_store.c's nvs_load_store()
 *                                 already decoded this boot, plus its
 *                                 persisted rev counter. Never read or
 *                                 written by this function.
 *   out_blob/out_rev           -- the store this call decided to trust.
 *                                 Always written (zeroed/0 if the return
 *                                 value is false).
 *   out_used_file              -- true if out_blob came from the file.
 *
 * Returns true if out_blob is trustworthy, false if neither side has
 * anything valid (out_blob zeroed, out_rev 0, out_used_file false).
 *
 * May perform a resync WRITE as a side effect (self-heal a stale/missing
 * file from a valid NVS candidate, or vice versa) -- a failed resync is
 * logged and otherwise ignored. */
bool kiln_cfg_store_cfg_fs_resolve(const kiln_cfg_store_blob_t *nvs_blob, bool nvs_valid, uint32_t nvs_rev,
                                    kiln_cfg_store_blob_t *out_blob, uint32_t *out_rev, bool *out_used_file);

#ifdef __cplusplus
}
#endif

#endif // KILN_CFG_STORE_CFG_FS_H
