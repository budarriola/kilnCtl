// profiles_cfg_fs -- read-through/dual-write bridge between user fire
// profiles' existing per-slot NVS blob (profiles_http.c's nvs_save_slot()/
// nvs_erase_slot()/nvs_load_all_from()) and the `cfg` LittleFS partition
// (cfg_fs.h/cfg_fs_mount.h), per docs/FILESYSTEM_USER_DATA_PLAN.md section 5
// step 4 ("user data move" plan) -- copies the shape of
// zones_config_cfg_fs.c/.h (see that file's header comment for the design
// this mirrors) but per-SLOT (id 0..PROFILES_MAX_COUNT-1) rather than a
// single document, because profiles are individually creatable/deletable.
//
// SCOPE: this file only decides WHICH bytes win (file vs NVS) for one slot
// at a time, and drives that slot's file read/write/delete -- it does not
// touch NVS itself. profiles_http.c's nvs_save_slot()/nvs_erase_slot()/
// nvs_load_all_from() still own the NVS side and call into this module once
// per slot.
//
// FILE LAYOUT: one file per slot, "profiles/prof<N>.json" (matches
// docs/FILESYSTEM_USER_DATA_PLAN.md section 3's `/cfg/profiles/<id>.json`
// path shape). Like zones_config_cfg_fs.c, the file is NOT hand-written JSON
// text yet -- it is a 4-byte little-endian `rev` counter followed by the
// EXACT SAME versioned blob bytes profile_decode_blob() (profiles_http.c,
// widened non-static via profiles_http_internal.h) already knows how to
// migrate -- byte 0 of the blob is still the on-flash PROFILE_VERSION. This
// means a file written by an older firmware is migrated by the SAME
// version-1/2/3 conversion chain a stored NVS blob already goes through, with
// zero new parser code (requirement: "validate on load exactly as the NVS
// path does").
//
// READ-THROUGH POLICY, per slot:
//   - file valid, NVS invalid, file rev > nvs rev  -> adopt FILE (a prior
//     save wrote the file but the NVS write failed after it).
//   - file valid, NVS invalid, file rev <= nvs rev -> adopt NVS's "empty"
//     (the file is a STALE LEFTOVER of a slot that was legitimately deleted
//     -- nvs_rev is bumped on delete too, specifically so this comparison
//     can tell a stale leftover from a failed-NVS-write save). The stale
//     file is best-effort deleted so the divergence does not persist.
//   - file invalid, NVS valid  -> adopt NVS, opportunistically write the file
//     (lazy one-slot-at-a-time migration, no separate migration task).
//   - both invalid -> slot is genuinely empty/unused.
//   - both valid, content differs -> higher rev wins (dual-write always
//     bumps rev and writes the file first, so file_rev >= nvs_rev under
//     normal operation); the loser is resynced when possible. ALWAYS logged
//     (ESP_LOGW) naming both revs -- never a silent disagreement.
//
// `nvs_rev` must be a rev this module can trust to reflect "the last known
// state of this slot on the NVS side," INCLUDING deletions -- profiles_http.c
// persists an 8-slot rev array (NVS_KEY_PROFILE_REV) that is bumped on both
// nvs_save_slot() and nvs_erase_slot(), not just on save, precisely so the
// "stale file after a legitimate delete" case above is distinguishable from
// "file is ahead because the NVS write half of a save failed."
//
// PARTITION ABSENT: cfg_fs_is_available() is false on every board today (no
// `cfg` partition mounted -- see cfg_fs_mount.c). Every function below checks
// it first and degrades to "use the NVS candidate, do nothing else," exactly
// like zones_config_cfg_fs.c and host-tested the same way.
#ifndef PROFILES_CFG_FS_H
#define PROFILES_CFG_FS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "profiles_types.h" /* profile_t, PROFILES_MAX_COUNT */

#ifdef __cplusplus
extern "C" {
#endif

/* Matches cfg_fs_write_atomic()'s signature (host-safe, no-op-if-unmounted
 * default) and cfg_fs_write_atomic_device()'s (routes through the flash
 * worker on-device) -- same seam zones_config_cfg_fs.h documents. No
 * on-device wiring installs the device variant yet (matching
 * zones_config_cfg_fs's own current state: nothing calls
 * zones_config_cfg_fs_set_write_fn() in production boot code either); this
 * seam exists so that wiring, whenever it lands for one, lands for both the
 * same way. */
typedef esp_err_t (*profiles_cfg_fs_write_fn_t)(const char *rel_path, const void *data, size_t len);
void profiles_cfg_fs_set_write_fn(profiles_cfg_fs_write_fn_t fn);
void profiles_cfg_fs_reset_write_fn_for_test(void);

/* Read the currently-installed write function -- used by cfg_fs_mount.c's
 * real-build assert that the device writer is installed whenever the
 * filesystem is mounted (docs/audits/filesystem_migration_review_2026-09-07.md
 * section 1). */
profiles_cfg_fs_write_fn_t profiles_cfg_fs_get_write_fn(void);

/* Matches cfg_fs_delete()'s signature. Defaults to cfg_fs_delete. */
typedef esp_err_t (*profiles_cfg_fs_delete_fn_t)(const char *rel_path);
void profiles_cfg_fs_set_delete_fn(profiles_cfg_fs_delete_fn_t fn);
void profiles_cfg_fs_reset_delete_fn_for_test(void);

/* cfg_fs directory and per-id path format (one place; profiles_scope_cfg_files.c
 * reads these to sweep every slot file on a profiles-scope factory reset). */
#define PROFILES_CFG_FS_DIR "profiles"
#define PROFILES_CFG_FS_PATH_FMT "profiles/prof%u.json"

/* Builds "profiles/prof<id>.json" into `out` (capacity `out_cap`). Exposed
 * for tests/diagnostics; internal callers in this module use it too. */
void profiles_cfg_fs_path(uint8_t id, char *out, size_t out_cap);

/* Reads and decodes slot `id`'s file only, without any NVS comparison. Same
 * *out_valid convention as zones_config_cfg_fs_load_raw(): OK == valid;
 * CORRUPT/NEWER or file absent/unreadable == not valid (*out_profile zeroed,
 * *out_rev 0). */
void profiles_cfg_fs_load_raw(uint8_t id, profile_t *out_profile, uint32_t *out_rev, bool *out_valid);

/* Same, plus *out_error (may be NULL): true when the file could not be examined at all (scratch
 * allocation failed). That is NOT "absent" -- *out_valid is false but the caller must not act on it
 * (no migrate-over, no slot-free). Review 7 L3. */
void profiles_cfg_fs_load_raw_ex(uint8_t id, profile_t *out_profile, uint32_t *out_rev, bool *out_valid, bool *out_error);

/* Writes slot `id`'s file at `rev`. No-op returning ESP_ERR_INVALID_STATE if
 * cfg_fs never mounted. `profile` must already be a valid, current-version
 * struct -- this function does not validate it. */
esp_err_t profiles_cfg_fs_save(uint8_t id, const profile_t *profile, uint32_t rev);

/* Deletes slot `id`'s file, if any. ESP_OK if the file did not exist either
 * (same "erase is idempotent" convention as profiles_http.c's
 * nvs_erase_slot(), which tolerates HAL_NOT_FOUND). No-op returning
 * ESP_ERR_INVALID_STATE if cfg_fs never mounted. */
esp_err_t profiles_cfg_fs_delete(uint8_t id);

/* Core of the per-slot read-through policy described above.
 *
 *   nvs_profile/nvs_valid/nvs_rev -- what profiles_http.c's
 *                                    nvs_load_all_from() already decoded for
 *                                    this slot this boot, plus the slot's
 *                                    persisted rev counter (valid or not --
 *                                    see the header comment on why the rev
 *                                    must survive a delete). Never read or
 *                                    written by this function.
 *   out_profile/out_rev            -- the profile + rev this call decided to
 *                                     trust. Always written (zeroed/0 if
 *                                     the return value is false).
 *   out_used_file                  -- true if out_profile came from the
 *                                     file.
 *
 * Returns true if out_profile is trustworthy (this slot is used), false if
 * neither side has anything valid for this slot (out_profile zeroed, out_rev
 * 0, out_used_file false) -- exactly the "unused slot" case
 * profiles_http.c's own decode already produces via its used_bitmap.
 *
 * May perform a resync WRITE or DELETE as a side effect (self-heal a
 * stale/missing file from a valid NVS candidate, or clean up a stale file
 * left behind by an interrupted delete) -- see the tie-break doc above. A
 * failed resync is logged and otherwise ignored -- the in-RAM decision
 * already made is not rolled back over a write/delete failure. */
bool profiles_cfg_fs_resolve(uint8_t id, const profile_t *nvs_profile, bool nvs_valid, uint32_t nvs_rev,
                              profile_t *out_profile, uint32_t *out_rev, bool *out_used_file);

/* resolve() with an error channel: *out_error true => the slot's file could not be examined; nothing
 * was written or deleted, the return is false, and the caller MUST treat the slot as unknown (refuse
 * saves/deletes), not as free. profiles_cfg_fs_resolve() is this with out_error == NULL. */
bool profiles_cfg_fs_resolve_ex(uint8_t id, const profile_t *nvs_profile, bool nvs_valid, uint32_t nvs_rev,
                                 profile_t *out_profile, uint32_t *out_rev, bool *out_used_file, bool *out_error);

#ifdef __cplusplus
}
#endif

#endif // PROFILES_CFG_FS_H
