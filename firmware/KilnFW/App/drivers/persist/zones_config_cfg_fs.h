// zones_config_cfg_fs -- read-through/dual-write bridge between zones
// config's existing NVS blob (zones_config_store.c's nvs_load()/nvs_save())
// and the `cfg` LittleFS partition (cfg_fs.h/cfg_fs_mount.h), per
// docs/FILESYSTEM_USER_DATA_PLAN.md section 5 step 5 -- the highest-risk
// data move in that plan (PID gains, coupling matrix, guard limits,
// progress_band_c).
//
// SCOPE: this file only decides WHICH bytes win (file vs NVS) and drives the
// file read/write -- it does not touch NVS itself. zones_config_store.c's
// nvs_load()/nvs_save() still own the NVS side and call into this module.
//
// FILE FORMAT: the file at ZONES_CFG_FILE_PATH holds a 4-byte little-endian
// `rev` counter immediately followed by the EXACT SAME versioned blob bytes
// zones_config_json_decode_blob() already knows how to migrate (byte 0 of
// the blob is still the on-flash zones_cfg version). This is deliberate: it
// means the file is migrated by the SAME chain a stored NVS blob is --
// zones_config_json_decode_blob() is called unchanged, with no separate
// "file schema" parser to keep in sync with 22 versions of history. A future
// firmware reading an older file migrates it exactly as it migrates an
// older NVS blob today (requirement 2 of the zones-config-move task).
//
// READ-THROUGH POLICY: reads prefer the file when it decodes to a valid,
// current-format config. If the file is absent, unreadable, or fails
// validation (zones_config_json_decode_blob() returns CORRUPT or NEWER),
// the caller's NVS candidate is used instead -- and if THAT is valid, this
// module opportunistically writes it out to the file, both to migrate a
// board that has never had a `cfg` partition mount before and to repair a
// file that just failed validation.
//
// DIVERGENCE TIE-BREAK: when both sides decode to a valid config and their
// bytes differ, the file wins only on a STRICTLY higher rev; an EQUAL rev
// goes to NVS. Dual-write stamps the SAME new rev on both sides and writes
// the file first, so file_rev > nvs_rev means the NVS write never landed
// (file is newer) and nvs_rev > file_rev means the file write failed (NVS
// is newer). Equal revs with differing bytes can only come from an NVS
// writer that does not know about `zones_rev` -- rolled-back firmware, or a
// crash between nvs_save()'s blob and rev writes -- and in both of those
// the NVS copy is the newer one. See zones_config_cfg_fs.c's own comment
// on that branch. Either way this is logged (ESP_LOGW) naming which side
// won and which rev each side reported -- the two are never left silently
// disagreeing. The losing side is resynced from the winner's bytes so the
// divergence does not persist across boots.
//
// PARTITION ABSENT: cfg_fs_is_available() is false on every board today (no
// `cfg` partition in partitions.csv yet). Every function below checks it
// first and falls through to "use the NVS candidate, do nothing else" --
// this is the code path that actually runs today and is host-tested
// explicitly (see test_zones_config_cfg_fs.c's "partition absent" case).
#ifndef ZONES_CONFIG_CFG_FS_H
#define ZONES_CONFIG_CFG_FS_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "zones_config_json.h" /* zones_cfg_t */

#ifdef __cplusplus
extern "C" {
#endif

#define ZONES_CFG_FILE_PATH "zones.json"

/* Matches cfg_fs_write_atomic()'s signature (cfg_fs.h) and
 * cfg_fs_write_atomic_device()'s (cfg_fs_mount.h) -- both fit this
 * typedef, which is exactly what makes the seam below work: host tests
 * install cfg_fs_write_atomic() directly (no flash worker on host), device
 * boot installs cfg_fs_write_atomic_device() (routes through the flash
 * worker, HAZARD: never call this typedef's target while already running ON
 * the flash worker task -- project_flash_worker_reentrancy). Defaults to
 * cfg_fs_write_atomic() (host-safe no-op-if-unmounted) if never set, so a
 * forgotten wire-up degrades to "writes go straight to cfg_fs" rather than
 * crashing on a NULL call. */
typedef esp_err_t (*zones_cfg_fs_write_fn_t)(const char *rel_path, const void *data, size_t len);
void zones_config_cfg_fs_set_write_fn(zones_cfg_fs_write_fn_t fn);

/* Test-only reset back to the default write function (cfg_fs_write_atomic).
 * No on-device call site -- device glue sets its own function once at boot
 * and never needs to un-set it. */
void zones_config_cfg_fs_reset_write_fn_for_test(void);

/* Read the currently-installed write function -- used by
 * cfg_fs_mount.c's real-build assert that the device (flash-worker-
 * dispatching) writer is installed whenever the filesystem is mounted
 * (docs/audits/filesystem_migration_review_2026-09-07.md section 1). */
zones_cfg_fs_write_fn_t zones_config_cfg_fs_get_write_fn(void);

/* Core of the read-through policy described above.
 *
 *   nvs_cfg/nvs_valid/nvs_rev  -- what zones_config_store.c's nvs_load_from()
 *                                 (+ the NVS rev key) already produced this
 *                                 boot. Never read or written by this
 *                                 function -- it is a pure decision given
 *                                 these inputs plus whatever is on the file.
 *   out_cfg/out_rev            -- the config + rev this call decided to
 *                                 trust. Always written.
 *   out_used_file              -- true if out_cfg came from the file
 *                                 (informational, e.g. for a status/debug
 *                                 field).
 *
 * Returns true if out_cfg is trustworthy (ready to run a kiln against),
 * false if neither side produced anything valid (out_cfg is zeroed,
 * out_rev is 0, out_used_file is false) -- exactly the same "nothing to
 * adopt" case nvs_load_from() itself already returns via *out_valid.
 *
 * May perform a resync WRITE as a side effect (self-heal a stale/missing
 * file from a valid NVS candidate, or vice versa) -- see the tie-break
 * doc above. That write goes through whatever function
 * zones_config_cfg_fs_set_write_fn() installed; a failed resync write is
 * logged and otherwise ignored (the in-RAM decision already made is not
 * rolled back over a write failure -- same "degrade, don't wedge"
 * discipline cfg_fs itself follows). */
bool zones_config_cfg_fs_resolve(const zones_cfg_t *nvs_cfg, bool nvs_valid, uint32_t nvs_rev, zones_cfg_t *out_cfg,
                                  uint32_t *out_rev, bool *out_used_file);

/* Writes `cfg` (must already be a valid, current-version struct -- this
 * function does not validate) to the file at `rev`. No-op returning
 * ESP_ERR_INVALID_STATE if cfg_fs never mounted -- callers must treat that
 * as expected on every board today, not as a surfaced error beyond a debug
 * log (mount-failure contract, docs/FILESYSTEM_USER_DATA_PLAN.md). */
esp_err_t zones_config_cfg_fs_save(const zones_cfg_t *cfg, uint32_t rev);

/* Reads and decodes the file only, without any NVS comparison -- used by
 * zones_config_cfg_fs_resolve() internally and exposed for tests/diagnostics
 * (e.g. a future /api/cfgfs status field wanting to report the file's own
 * rev independent of the resolve decision). *out_valid is set the same way
 * zones_config_json_decode_blob() would judge it (OK == valid; CORRUPT/NEWER
 * or file absent/unreadable == not valid, *out_cfg zeroed, *out_rev 0). */
void zones_config_cfg_fs_load_raw(zones_cfg_t *out_cfg, uint32_t *out_rev, bool *out_valid);

#ifdef __cplusplus
}
#endif

#endif // ZONES_CONFIG_CFG_FS_H
