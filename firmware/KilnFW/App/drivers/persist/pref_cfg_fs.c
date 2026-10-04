// See pref_cfg_fs.h for the full design/rationale.
#include "pref_cfg_fs.h"

#include <string.h>

#include "esp_log.h"

#include "cfg_fs.h"

static const char *PREF_FS_TAG = "pref_cfg_fs";

static pref_cfg_fs_write_fn_t s_write_fn = cfg_fs_write_atomic;

void pref_cfg_fs_set_write_fn(pref_cfg_fs_write_fn_t fn)
{
    s_write_fn = fn ? fn : cfg_fs_write_atomic;
}

void pref_cfg_fs_reset_write_fn_for_test(void)
{
    s_write_fn = cfg_fs_write_atomic;
}

pref_cfg_fs_write_fn_t pref_cfg_fs_get_write_fn(void)
{
    return s_write_fn;
}

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

void pref_cfg_fs_load_raw(const char *rel_path, size_t item_size, pref_cfg_fs_validate_fn_t validate,
                           void *out_bytes, uint32_t *out_rev, bool *out_valid)
{
    if (out_bytes) {
        memset(out_bytes, 0, item_size);
    }
    if (out_rev) {
        *out_rev = 0;
    }
    if (out_valid) {
        *out_valid = false;
    }
    if (!rel_path || !out_bytes || !out_rev || !out_valid || item_size == 0 || item_size > PREF_CFG_FS_MAX_ITEM) {
        return;
    }
    if (!cfg_fs_is_available()) {
        return;
    }

    uint8_t raw[4 + PREF_CFG_FS_MAX_ITEM];
    size_t len = 0;
    esp_err_t err = cfg_fs_read(rel_path, raw, sizeof(raw), &len);
    if (err != ESP_OK) {
        // ESP_ERR_NOT_FOUND (never migrated yet) is the normal state on
        // every board today -- not logged, same convention as
        // zones_config_cfg_fs_load_raw(). Any other read failure is quiet
        // here too; the caller's resolve() decides whether it is worth a
        // divergence warning.
        return;
    }
    if (len != 4 + item_size) {
        ESP_LOGW(PREF_FS_TAG, "%s is %u bytes, expected %u (4-byte rev + %u-byte item) -- ignoring", rel_path,
                 (unsigned)len, (unsigned)(4 + item_size), (unsigned)item_size);
        return;
    }

    uint32_t rev = get_u32_le(raw);
    const void *item_bytes = raw + 4;
    if (validate && !validate(item_bytes, item_size)) {
        ESP_LOGW(PREF_FS_TAG, "%s (rev %lu) REJECTED by validator -- ignoring file, NVS candidate decides", rel_path,
                 (unsigned long)rev);
        return;
    }

    memcpy(out_bytes, item_bytes, item_size);
    *out_rev = rev;
    *out_valid = true;
}

esp_err_t pref_cfg_fs_save(const char *rel_path, const void *bytes, size_t item_size, uint32_t rev)
{
    if (!rel_path || !bytes || item_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (item_size > PREF_CFG_FS_MAX_ITEM) {
        ESP_LOGE(PREF_FS_TAG, "%s item is %u bytes, exceeds PREF_CFG_FS_MAX_ITEM (%u) -- refusing to write",
                 rel_path, (unsigned)item_size, (unsigned)PREF_CFG_FS_MAX_ITEM);
        return ESP_ERR_INVALID_SIZE;
    }
    if (!cfg_fs_is_available()) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t raw[4 + PREF_CFG_FS_MAX_ITEM];
    put_u32_le(raw, rev);
    memcpy(raw + 4, bytes, item_size);

    esp_err_t err = s_write_fn(rel_path, raw, 4 + item_size);
    if (err != ESP_OK) {
        ESP_LOGW(PREF_FS_TAG, "%s write (rev %lu) failed: %s", rel_path, (unsigned long)rev, esp_err_to_name(err));
    }
    return err;
}

bool pref_cfg_fs_resolve(const char *rel_path, const void *nvs_bytes, size_t item_size, bool nvs_valid,
                          uint32_t nvs_rev, pref_cfg_fs_validate_fn_t validate, void *out_bytes, uint32_t *out_rev,
                          bool *out_used_file)
{
    if (out_bytes) {
        memset(out_bytes, 0, item_size);
    }
    if (out_rev) {
        *out_rev = 0;
    }
    if (out_used_file) {
        *out_used_file = false;
    }
    if (!rel_path || !nvs_bytes || !out_bytes || !out_rev || !out_used_file || item_size == 0 ||
        item_size > PREF_CFG_FS_MAX_ITEM) {
        return false;
    }

    uint8_t file_bytes[PREF_CFG_FS_MAX_ITEM];
    uint32_t file_rev = 0;
    bool file_valid = false;
    pref_cfg_fs_load_raw(rel_path, item_size, validate, file_bytes, &file_rev, &file_valid);

    if (!file_valid) {
        // No usable file -- fall back to the NVS candidate, and if it is
        // itself trustworthy, opportunistically write it out (lazy,
        // one-item-at-a-time migration: the first successful load after
        // `cfg` becomes available, or after a corrupt file is detected,
        // writes a fresh file).
        memcpy(out_bytes, nvs_bytes, item_size);
        *out_rev = nvs_rev;
        *out_used_file = false;
        if (nvs_valid) {
            esp_err_t werr = pref_cfg_fs_save(rel_path, nvs_bytes, item_size, nvs_rev);
            if (werr != ESP_OK && werr != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(PREF_FS_TAG, "could not migrate NVS %s to file: %s", rel_path, esp_err_to_name(werr));
            }
        }
        return nvs_valid;
    }

    if (!nvs_valid) {
        // File is good, NVS side has nothing trustworthy -- use the file
        // outright. Not logged as a divergence: nothing on the NVS side to
        // disagree WITH.
        memcpy(out_bytes, file_bytes, item_size);
        *out_rev = file_rev;
        *out_used_file = true;
        return true;
    }

    bool differs = memcmp(file_bytes, nvs_bytes, item_size) != 0;
    if (!differs) {
        memcpy(out_bytes, file_bytes, item_size);
        *out_rev = file_rev > nvs_rev ? file_rev : nvs_rev;
        *out_used_file = true;
        return true;
    }

    // DIVERGENCE TIE-BREAK: STRICTLY higher rev wins, same rule
    // zones_config_cfg_fs.c uses (docs/audits/filesystem_migration_review_
    // 2026-09-07.md section 2). Under normal dual-write operation (file
    // written first, then NVS, same rev on both) file_rev > nvs_rev always
    // holds after a successful save; EQUAL revs with differing bytes can
    // only mean an NVS-only writer (firmware from before this dual-write
    // existed, rolled back to) wrote the blob without touching the rev --
    // the NVS side is then the newer one, never the file. `>=` here would
    // silently discard that edit and then overwrite it permanently on the
    // next save.
    if (file_rev > nvs_rev) {
        ESP_LOGW(PREF_FS_TAG, "%s file/NVS DIVERGED (file rev %lu, NVS rev %lu) -- adopting FILE (strictly higher rev)",
                 rel_path, (unsigned long)file_rev, (unsigned long)nvs_rev);
        memcpy(out_bytes, file_bytes, item_size);
        *out_rev = file_rev;
        *out_used_file = true;
        // NVS resync happens on the caller's next save() call (mirrors
        // zones_config_cfg_fs_resolve()'s identical note) -- this module
        // never touches NVS directly.
    } else {
        ESP_LOGW(PREF_FS_TAG,
                 "%s file/NVS DIVERGED (file rev %lu, NVS rev %lu) -- adopting NVS (higher rev), resyncing file",
                 rel_path, (unsigned long)file_rev, (unsigned long)nvs_rev);
        memcpy(out_bytes, nvs_bytes, item_size);
        *out_rev = nvs_rev;
        *out_used_file = false;
        esp_err_t werr = pref_cfg_fs_save(rel_path, nvs_bytes, item_size, nvs_rev);
        if (werr != ESP_OK && werr != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(PREF_FS_TAG, "could not resync %s from NVS: %s", rel_path, esp_err_to_name(werr));
        }
    }
    return true;
}
