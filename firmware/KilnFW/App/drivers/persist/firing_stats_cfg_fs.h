// firing_stats_cfg_fs -- read-through/dual-write bridge between the
// per-profile firing-history ring (profile_executor_firing_stats.c's
// firing_stats_load()/firing_stats_persist(), NVS key "fs_<id>" in
// profiles_nvs/fire_stats) and the `cfg` LittleFS partition (cfg_fs.h/
// cfg_fs_mount.h), per docs/FILESYSTEM_USER_DATA_PLAN.md section 5 item 7
// ("firing stats / history").
//
// SHAPE: copies profiles_cfg_fs.c/.h's per-id file design (one file per
// profile id, since ids are individually written/never deleted) but for a
// FIXED-SIZE, VERSIONLESS blob (profile_firing_history_blob_t, 1364 bytes,
// see profile_executor_internal.h's own "VERSIONLESS HAZARD" comment) rather
// than profiles' versioned/decoded one. There is no decode function to
// share here -- the file holds the exact same raw bytes
// firing_stats_load()/firing_stats_persist() already read/write via
// hal_kv_get_blob()/hal_kv_set_blob(), so a size mismatch is treated
// identically on both sides: NOT valid, never partially trusted. This
// respects the existing "a move must not silently discard firing history"
// requirement by construction -- a wrong-sized file is simply invalid and
// the resolve falls back to the NVS candidate exactly as
// firing_stats_load()'s own tolerant NVS path (including its tail-append
// v1 migration) already does; the file bridge never widens or narrows that
// tolerance.
//
// FILE LAYOUT: "stats/fs<id>.dat" -- a 4-byte little-endian `rev` counter
// followed by the raw profile_firing_history_blob_t bytes, unchanged.
//
// READ-THROUGH POLICY / DIVERGENCE TIE-BREAK: identical shape to
// profiles_cfg_fs.c's (see that header for the full four-way table) --
// STRICT file_rev > nvs_rev to adopt the file, content-equal short-circuits
// before any log, both-invalid means "never fired," NVS-valid/file-invalid
// migrates lazily. There is no DELETE for this item (a profile's history
// is only ever appended-to via firing_stats_persist(), never erased), so
// unlike profiles_cfg_fs.c's rev array there is nothing to distinguish
// "legitimately removed" from "failed NVS write" -- unused simplifies to
// "nvs_valid == false, trust the file if present."
//
// REV STORAGE: one NVS key per id ("fsr_<id>", under FIRING_STATS_NVS_
// NAMESPACE/PARTITION -- profile_executor_firing_stats.c's own constants,
// duplicated here rather than shared via a header since that file keeps
// them file-scope-static) bumped every firing_stats_persist() call for that
// id, alongside the blob write in the SAME NVS transaction so a torn write
// can never leave rev ahead of a blob that was never actually committed.
//
// PARTITION ABSENT: cfg_fs_is_available() is checked first in every
// function below; false degrades to "use the NVS candidate, do nothing
// else" -- the path every board runs today.
#ifndef FIRING_STATS_CFG_FS_H
#define FIRING_STATS_CFG_FS_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "profile_executor_internal.h" /* profile_firing_history_blob_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Matches cfg_fs_write_atomic()'s signature (host-safe, no-op-if-unmounted
 * default) -- same seam profiles_cfg_fs.h/zones_config_cfg_fs.h document. */
typedef esp_err_t (*firing_stats_cfg_fs_write_fn_t)(const char *rel_path, const void *data, size_t len);
void firing_stats_cfg_fs_set_write_fn(firing_stats_cfg_fs_write_fn_t fn);
void firing_stats_cfg_fs_reset_write_fn_for_test(void);
firing_stats_cfg_fs_write_fn_t firing_stats_cfg_fs_get_write_fn(void);

/* Builds "stats/fs<id>.dat" into `out` (capacity `out_cap`). */
void firing_stats_cfg_fs_path(uint8_t id, char *out, size_t out_cap);

/* Reads and validates id's file only, no NVS comparison. *out_valid is true
 * only if the file exists and is EXACTLY 4 + sizeof(profile_firing_history_
 * blob_t) bytes -- any other size (including a smaller, "older-layout"
 * size) is NOT valid here, matching the blob's own versionless "any
 * mismatch is untrusted" contract. */
void firing_stats_cfg_fs_load_raw(uint8_t id, profile_firing_history_blob_t *out_blob, uint32_t *out_rev,
                                   bool *out_valid);

/* Writes id's file at `rev`. No-op returning ESP_ERR_INVALID_STATE if
 * cfg_fs never mounted. */
esp_err_t firing_stats_cfg_fs_save(uint8_t id, const profile_firing_history_blob_t *blob, uint32_t rev);

/* Reads id's persisted rev counter ("fsr_<id>" in FIRING_STATS_NVS_NAMESPACE/
 * PARTITION). Missing (never saved through the bridge yet) reads as 0. */
uint32_t firing_stats_cfg_fs_read_rev(uint8_t id);

/* Writes id's rev counter. Called by profile_executor_firing_stats.c's
 * firing_stats_persist() in the SAME NVS read-modify-write transaction as
 * the blob write, so a torn write can never leave rev ahead of a blob that
 * was never actually committed. */
esp_err_t firing_stats_cfg_fs_write_rev(uint8_t id, uint32_t rev);

/* Core of the read-through policy (see header comment above for the table).
 * nvs_blob/nvs_valid/nvs_rev are whatever firing_stats_load() already
 * decoded (or tolerantly migrated) for this id this call -- never read or
 * written by this function. Returns true if out_blob is trustworthy (this
 * id has fired at least once); false if neither side has anything (out_blob
 * zeroed, out_rev 0, out_used_file false) -- the "never fired" case
 * firing_stats_load() already treats as non-error. May perform a resync
 * WRITE as a side effect (self-heal a stale/missing file from a valid NVS
 * candidate, or vice versa); a failed resync is logged and otherwise
 * ignored. */
bool firing_stats_cfg_fs_resolve(uint8_t id, const profile_firing_history_blob_t *nvs_blob, bool nvs_valid,
                                  uint32_t nvs_rev, profile_firing_history_blob_t *out_blob, uint32_t *out_rev,
                                  bool *out_used_file);

#ifdef __cplusplus
}
#endif

#endif // FIRING_STATS_CFG_FS_H
