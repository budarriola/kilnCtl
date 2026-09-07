// cfg_fs_status -- observability for the `cfg` LittleFS partition
// (docs/FILESYSTEM_USER_DATA_PLAN.md). Builds the JSON body served at
// GET /api/cfgfs (diagnostics_http.c). Pure and host-testable: it calls
// ONLY cfg_fs.h's public API (cfg_fs_get_status()/is_available()/list()),
// plus a plain stat() on paths this module builds itself from a base
// directory the caller already knows -- it never edits or reaches into
// cfg_fs.c's/cfg_fs_mount.c's internals, which are foundation-module code
// owned elsewhere (docs/FILESYSTEM_USER_DATA_PLAN.md's mount-failure
// contract). On device the caller passes "/cfg" (cfg_fs_mount.c's fixed
// mount point); host tests pass whatever scratch directory their own
// cfg_fs_init() call used -- same on-disk shape either way, so this file's
// host coverage IS the code that runs on the board.
//
// Capacity (total/used bytes) and the dual-write (file vs NVS rev) picture
// are supplied by the CALLER, not read here: capacity needs
// esp_littlefs_info(), an ESP-IDF-only API, and the dual-write rev compare
// needs a read of zones config's NVS key -- both device/NVS specific and
// deliberately kept out of this pure module so it stays host-testable
// without pulling in ESP-IDF headers or the NVS stub. Pass NULL for either
// to omit that section of the JSON (reported as "unknown", not silently
// zeroed -- an unknown capacity must never render as "0 bytes free").
#ifndef CFG_FS_STATUS_H
#define CFG_FS_STATUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool   known;       /* false => caller could not determine capacity (e.g. esp_littlefs_info() failed) */
    size_t total_bytes;
    size_t used_bytes;
} cfg_fs_capacity_info_t;

/* Dual-write picture for one migrated item (today: zones config only, per
 * docs/FILESYSTEM_USER_DATA_PLAN.md section 5 -- steps 3/4/6 have not
 * landed, so every other MOVE item is still NVS-only and is reported as
 * such by cfg_fs_status_build_json() without needing an entry here). */
typedef struct {
    bool     file_valid;  /* zones_config_cfg_fs_load_raw()'s out_valid */
    uint32_t file_rev;    /* zones_config_cfg_fs_load_raw()'s out_rev */
    uint32_t nvs_rev;     /* the "zones_rev" NVS key, read by the caller */
} cfg_fs_zones_dualwrite_info_t;

/* Builds the full /api/cfgfs JSON body into `buf` (capacity `buf_cap`).
 * `base_dir_for_sizes` may be NULL to omit per-file sizes (e.g. cfg_fs is
 * not mounted, so there is nothing to stat). `cap`/`dual` may be NULL to
 * omit their sections (reported as unknown/absent, not defaulted).
 *
 * Returns ESP_OK and sets *out_len to the JSON length on success.
 * ESP_ERR_INVALID_ARG for NULL buf/out_len or buf_cap == 0.
 * ESP_ERR_INVALID_SIZE if the JSON did not fit `buf_cap` -- *out_len is
 * unset in that case, exactly like cfg_fs_read()'s own truncation-detection
 * convention, so a caller can grow its buffer and retry rather than ship a
 * silently-truncated response. */
esp_err_t cfg_fs_status_build_json(const char *base_dir_for_sizes, const cfg_fs_capacity_info_t *cap,
                                    const cfg_fs_zones_dualwrite_info_t *dual, char *buf, size_t buf_cap,
                                    size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif // CFG_FS_STATUS_H
