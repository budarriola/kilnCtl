// See firing_stats_cfg_fs.h for the full design/rationale.
#include "firing_stats_cfg_fs.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "cfg_fs.h"
#include "hal_kv.h"
#include "hal_esp_common.h" /* hal_status_to_esp_err() */
#include "nvs_key_check.h"

static const char *FSCF_TAG = "firing_stats_cfg_fs";

// Duplicated from profile_executor_firing_stats.c's own file-scope-static
// constants (not shared via a header -- those stay `static` there on
// purpose) so this module can read/write the SAME rev key namespace its
// blob lives in.
#define FIRING_STATS_NVS_PARTITION "profiles_nvs"
#define FIRING_STATS_NVS_NAMESPACE "fire_stats"
NVS_KEY_LEN_CHECK(FIRING_STATS_NVS_PARTITION);
NVS_KEY_LEN_CHECK(FIRING_STATS_NVS_NAMESPACE);
// "fsr_%u" -- <=9 chars even for a 3-digit id, well under the 15-char limit
// (the key itself is built at runtime, so no NVS_KEY_LEN_CHECK literal here,
// same as "fs_%u" in profile_executor_firing_stats.c).

static firing_stats_cfg_fs_write_fn_t s_write_fn = cfg_fs_write_atomic;

void firing_stats_cfg_fs_set_write_fn(firing_stats_cfg_fs_write_fn_t fn)
{
    s_write_fn = fn ? fn : cfg_fs_write_atomic;
}

void firing_stats_cfg_fs_reset_write_fn_for_test(void)
{
    s_write_fn = cfg_fs_write_atomic;
}

firing_stats_cfg_fs_write_fn_t firing_stats_cfg_fs_get_write_fn(void)
{
    return s_write_fn;
}

void firing_stats_cfg_fs_path(uint8_t id, char *out, size_t out_cap)
{
    snprintf(out, out_cap, "stats/fs%u.dat", (unsigned)id);
}

#define FSCF_FILE_BUF_MAX (4 + sizeof(profile_firing_history_blob_t))

static void put_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t get_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Reads this id's persisted rev counter ("fsr_<id>"). Missing (never saved
// through the bridge yet) reads as 0, matching every other item's
// "un-set == rev 0" convention.
uint32_t firing_stats_cfg_fs_read_rev(uint8_t id)
{
    char key[16];
    snprintf(key, sizeof(key), "fsr_%u", (unsigned)id);
    hal_kv_handle_t h;
    if (hal_kv_open(&h, FIRING_STATS_NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, FIRING_STATS_NVS_PARTITION) != HAL_OK) {
        return 0;
    }
    uint32_t rev = 0;
    hal_kv_get_u32(&h, key, &rev);
    hal_kv_close(&h);
    return rev;
}

void firing_stats_cfg_fs_load_raw(uint8_t id, profile_firing_history_blob_t *out_blob, uint32_t *out_rev,
                                   bool *out_valid)
{
    if (out_blob) {
        memset(out_blob, 0, sizeof(*out_blob));
    }
    if (out_rev) {
        *out_rev = 0;
    }
    if (out_valid) {
        *out_valid = false;
    }
    if (!out_blob || !out_rev || !out_valid) {
        return;
    }
    if (!cfg_fs_is_available()) {
        return;
    }

    char path[40];
    firing_stats_cfg_fs_path(id, path, sizeof(path));

    uint8_t raw[FSCF_FILE_BUF_MAX];
    size_t len = 0;
    esp_err_t err = cfg_fs_read(path, raw, sizeof(raw), &len);
    if (err != ESP_OK) {
        return; /* absent/unreadable -- not "found but bad" on its own */
    }
    // VERSIONLESS blob: exact size or nothing. No tail-append tolerance
    // here (unlike firing_stats_load()'s own NVS-side v1 migration) -- a
    // file this bridge itself never wrote at an old size cannot exist, so
    // any size mismatch is corruption, not a legitimate older layout.
    if (len != FSCF_FILE_BUF_MAX) {
        ESP_LOGW(FSCF_TAG, "fs%u file is %u bytes, expected exactly %u -- ignoring, NVS candidate decides", id,
                 (unsigned)len, (unsigned)FSCF_FILE_BUF_MAX);
        return;
    }

    *out_rev = get_u32_le(raw);
    memcpy(out_blob, raw + 4, sizeof(*out_blob));
    *out_valid = true;
}

esp_err_t firing_stats_cfg_fs_save(uint8_t id, const profile_firing_history_blob_t *blob, uint32_t rev)
{
    if (!blob) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!cfg_fs_is_available()) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t raw[FSCF_FILE_BUF_MAX];
    put_u32_le(raw, rev);
    memcpy(raw + 4, blob, sizeof(*blob));

    char path[40];
    firing_stats_cfg_fs_path(id, path, sizeof(path));
    esp_err_t err = s_write_fn(path, raw, sizeof(raw));
    if (err != ESP_OK) {
        ESP_LOGW(FSCF_TAG, "fs%u file write (rev %lu) failed: %s", id, (unsigned long)rev, esp_err_to_name(err));
    }
    return err;
}

// Persists just the rev counter, called alongside the caller's own NVS blob
// write so both land in the same read-modify-write transaction. Exposed as
// a small helper rather than folded into firing_stats_cfg_fs_save() itself,
// since the rev lives in NVS (like every other item's rev key) while the
// blob lives in the file -- the two are written by different callers
// (profile_executor_firing_stats.c's firing_stats_persist() owns the NVS
// side) at slightly different points in that function.
esp_err_t firing_stats_cfg_fs_write_rev(uint8_t id, uint32_t rev)
{
    hal_kv_handle_t h;
    hal_status_t err =
        hal_kv_open(&h, FIRING_STATS_NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, FIRING_STATS_NVS_PARTITION);
    if (err != HAL_OK) {
        return hal_status_to_esp_err(err);
    }
    char key[16];
    snprintf(key, sizeof(key), "fsr_%u", (unsigned)id);
    err = hal_kv_set_u32(&h, key, rev);
    if (err == HAL_OK) {
        err = hal_kv_commit(&h);
    }
    hal_kv_close(&h);
    return hal_status_to_esp_err(err);
}

bool firing_stats_cfg_fs_resolve(uint8_t id, const profile_firing_history_blob_t *nvs_blob, bool nvs_valid,
                                  uint32_t nvs_rev, profile_firing_history_blob_t *out_blob, uint32_t *out_rev,
                                  bool *out_used_file)
{
    if (out_blob) {
        memset(out_blob, 0, sizeof(*out_blob));
    }
    if (out_rev) {
        *out_rev = 0;
    }
    if (out_used_file) {
        *out_used_file = false;
    }
    if (!nvs_blob || !out_blob || !out_rev || !out_used_file) {
        return false;
    }

    profile_firing_history_blob_t file_blob;
    uint32_t file_rev = 0;
    bool file_valid = false;
    firing_stats_cfg_fs_load_raw(id, &file_blob, &file_rev, &file_valid);

    if (!file_valid) {
        if (!nvs_valid) {
            return false; /* this profile has never fired, on either side */
        }
        // File missing/corrupt, NVS has real history -- adopt NVS (never
        // discard it) and lazily migrate a fresh file, same convention as
        // every other *_cfg_fs module in this codebase.
        *out_blob = *nvs_blob;
        *out_rev = nvs_rev;
        *out_used_file = false;
        esp_err_t werr = firing_stats_cfg_fs_save(id, nvs_blob, nvs_rev);
        if (werr != ESP_OK && werr != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(FSCF_TAG, "could not migrate fs%u history to file: %s", id, esp_err_to_name(werr));
        }
        return true;
    }

    if (!nvs_valid) {
        // File decodes fine but NVS has nothing -- there is no delete path
        // for this item (history is only ever appended-to), so this can
        // only mean a prior save's file write landed and its NVS write
        // failed. Trust the file -- discarding it here would be exactly
        // the "silently discard firing history" outcome this item's brief
        // forbids.
        ESP_LOGW(FSCF_TAG, "fs%u file/NVS DIVERGED (file rev %lu valid, NVS unused) -- adopting FILE (no delete "
                           "path exists for this item, so this must be a prior failed NVS write)",
                 id, (unsigned long)file_rev);
        *out_blob = file_blob;
        *out_rev = file_rev;
        *out_used_file = true;
        return true;
    }

    // Both valid -- compare content, not just rev, so two independently
    // identical histories never log a spurious divergence.
    bool differs = memcmp(&file_blob, nvs_blob, sizeof(file_blob)) != 0;
    if (!differs) {
        *out_blob = file_blob;
        *out_rev = file_rev > nvs_rev ? file_rev : nvs_rev;
        *out_used_file = true;
        return true;
    }

    if (file_rev > nvs_rev) {
        // STRICTLY higher, not >= -- same reasoning check_cfg_fs_tie_break.ps1
        // enforces on every other *_cfg_fs module in this codebase.
        ESP_LOGW(FSCF_TAG, "fs%u file/NVS DIVERGED (file rev %lu, NVS rev %lu) -- adopting FILE (strictly higher rev)",
                 id, (unsigned long)file_rev, (unsigned long)nvs_rev);
        *out_blob = file_blob;
        *out_rev = file_rev;
        *out_used_file = true;
    } else {
        ESP_LOGW(FSCF_TAG,
                 "fs%u file/NVS DIVERGED (file rev %lu, NVS rev %lu) -- adopting NVS (higher-or-equal rev), "
                 "resyncing file",
                 id, (unsigned long)file_rev, (unsigned long)nvs_rev);
        *out_blob = *nvs_blob;
        *out_rev = nvs_rev;
        *out_used_file = false;
        esp_err_t werr = firing_stats_cfg_fs_save(id, nvs_blob, nvs_rev);
        if (werr != ESP_OK && werr != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(FSCF_TAG, "could not resync fs%u file from NVS: %s", id, esp_err_to_name(werr));
        }
    }
    return true;
}
