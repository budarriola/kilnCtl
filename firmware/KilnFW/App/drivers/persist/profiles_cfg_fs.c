// See profiles_cfg_fs.h for the full design/rationale.
#include "profiles_cfg_fs.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "cfg_fs.h"
#include "profiles_http_internal.h" /* profile_decode_blob(), profile_encode_current_blob(),
                                      * PROFILE_BLOB_MAX_SIZE -- the exact same encode/decode
                                      * this file's NVS sibling (profiles_http.c) uses, so a
                                      * file is validated identically to an NVS blob. */

static const char *PCFG_FS_TAG = "profiles_cfg_fs";

static profiles_cfg_fs_write_fn_t s_write_fn = cfg_fs_write_atomic;
static profiles_cfg_fs_delete_fn_t s_delete_fn = cfg_fs_delete;

void profiles_cfg_fs_set_write_fn(profiles_cfg_fs_write_fn_t fn)
{
    s_write_fn = fn ? fn : cfg_fs_write_atomic;
}

void profiles_cfg_fs_reset_write_fn_for_test(void)
{
    s_write_fn = cfg_fs_write_atomic;
}

void profiles_cfg_fs_set_delete_fn(profiles_cfg_fs_delete_fn_t fn)
{
    s_delete_fn = fn ? fn : cfg_fs_delete;
}

void profiles_cfg_fs_reset_delete_fn_for_test(void)
{
    s_delete_fn = cfg_fs_delete;
}

void profiles_cfg_fs_path(uint8_t id, char *out, size_t out_cap)
{
    snprintf(out, out_cap, "profiles/prof%u.json", (unsigned)id);
}

/* rev(4 bytes LE) + the on-flash versioned profile blob. PROFILE_BLOB_MAX_SIZE
 * (profiles_http_internal.h) is sized generously above the current-version
 * persisted struct, same headroom convention as zones_config_cfg_fs.c's
 * ZCFG_FILE_BUF_MAX. */
#define PCFG_FILE_BUF_MAX (4 + PROFILE_BLOB_MAX_SIZE)

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

void profiles_cfg_fs_load_raw(uint8_t id, profile_t *out_profile, uint32_t *out_rev, bool *out_valid)
{
    if (out_profile) {
        memset(out_profile, 0, sizeof(*out_profile));
    }
    if (out_rev) {
        *out_rev = 0;
    }
    if (out_valid) {
        *out_valid = false;
    }
    if (!out_profile || !out_rev || !out_valid) {
        return;
    }
    if (!cfg_fs_is_available()) {
        return;
    }

    char path[40];
    profiles_cfg_fs_path(id, path, sizeof(path));

    uint8_t raw[PCFG_FILE_BUF_MAX];
    size_t len = 0;
    esp_err_t err = cfg_fs_read(path, raw, sizeof(raw), &len);
    if (err != ESP_OK) {
        /* Absent (never migrated/never saved), oversized, or unreadable --
         * none of these are "found but bad" on their own; the caller's
         * resolve() decides whether that's worth a divergence warning. */
        return;
    }
    if (len < 5) { /* rev prefix + at least a 1-byte version */
        ESP_LOGW(PCFG_FS_TAG, "prof%u file is %u bytes, too short to hold a rev + blob -- ignoring", id,
                 (unsigned)len);
        return;
    }

    uint32_t rev = get_u32_le(raw);
    const char *reason = "";
    profile_t cand;
    profile_decode_result_t result = profile_decode_blob(raw + 4, len - 4, &cand, &reason);
    if (result != PROFILE_DECODE_OK) {
        ESP_LOGW(PCFG_FS_TAG, "prof%u file (rev %lu) REJECTED: %s -- ignoring file, NVS candidate decides", id,
                 (unsigned long)rev, reason);
        return;
    }

    *out_profile = cand;
    *out_rev = rev;
    *out_valid = true;
}

esp_err_t profiles_cfg_fs_save(uint8_t id, const profile_t *profile, uint32_t rev)
{
    if (!profile) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!cfg_fs_is_available()) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t raw[PCFG_FILE_BUF_MAX];
    size_t blob_len = profile_encode_current_blob(profile, raw + 4, sizeof(raw) - 4);
    if (blob_len == 0) {
        ESP_LOGE(PCFG_FS_TAG, "prof%u encode failed -- buffer too small, refusing to write", id);
        return ESP_ERR_INVALID_SIZE;
    }
    put_u32_le(raw, rev);

    char path[40];
    profiles_cfg_fs_path(id, path, sizeof(path));
    esp_err_t err = s_write_fn(path, raw, 4 + blob_len);
    if (err != ESP_OK) {
        ESP_LOGW(PCFG_FS_TAG, "prof%u file write (rev %lu) failed: %s", id, (unsigned long)rev,
                 esp_err_to_name(err));
    }
    return err;
}

esp_err_t profiles_cfg_fs_delete(uint8_t id)
{
    if (!cfg_fs_is_available()) {
        return ESP_ERR_INVALID_STATE;
    }
    char path[40];
    profiles_cfg_fs_path(id, path, sizeof(path));
    esp_err_t err = s_delete_fn(path);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(PCFG_FS_TAG, "prof%u file delete failed: %s", id, esp_err_to_name(err));
    }
    return (err == ESP_ERR_NOT_FOUND) ? ESP_OK : err;
}

bool profiles_cfg_fs_resolve(uint8_t id, const profile_t *nvs_profile, bool nvs_valid, uint32_t nvs_rev,
                              profile_t *out_profile, uint32_t *out_rev, bool *out_used_file)
{
    if (out_profile) {
        memset(out_profile, 0, sizeof(*out_profile));
    }
    if (out_rev) {
        *out_rev = 0;
    }
    if (out_used_file) {
        *out_used_file = false;
    }
    if (!nvs_profile || !out_profile || !out_rev || !out_used_file) {
        return false;
    }

    profile_t file_profile;
    uint32_t file_rev = 0;
    bool file_valid = false;
    profiles_cfg_fs_load_raw(id, &file_profile, &file_rev, &file_valid);

    if (!file_valid) {
        if (!nvs_valid) {
            return false; /* genuinely unused on both sides */
        }
        /* File missing/corrupt, NVS has a real profile -- adopt NVS and
         * lazily migrate a fresh file, same as zones_config_cfg_fs.c. */
        *out_profile = *nvs_profile;
        *out_rev = nvs_rev;
        *out_used_file = false;
        esp_err_t werr = profiles_cfg_fs_save(id, nvs_profile, nvs_rev);
        if (werr != ESP_OK && werr != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(PCFG_FS_TAG, "could not migrate prof%u to file: %s", id, esp_err_to_name(werr));
        }
        return true;
    }

    if (!nvs_valid) {
        /* File decodes fine but NVS has nothing for this slot. Two possible
         * histories: (a) a save wrote the file but its NVS half failed
         * (file is ahead), or (b) this slot was legitimately DELETED after
         * the file was written (file is a stale leftover). nvs_rev here is
         * profiles_http.c's persisted per-slot rev counter, which is bumped
         * on delete too -- so it still tells the two apart. */
        if (file_rev > nvs_rev) {
            ESP_LOGW(PCFG_FS_TAG,
                     "prof%u file/NVS DIVERGED (file rev %lu valid, NVS unused at rev %lu) -- adopting FILE "
                     "(higher rev, looks like a failed NVS write)",
                     id, (unsigned long)file_rev, (unsigned long)nvs_rev);
            *out_profile = file_profile;
            *out_rev = file_rev;
            *out_used_file = true;
            return true;
        }
        ESP_LOGW(PCFG_FS_TAG,
                 "prof%u file/NVS DIVERGED (file rev %lu valid, NVS unused at rev %lu) -- adopting NVS "
                 "(deleted), removing stale file",
                 id, (unsigned long)file_rev, (unsigned long)nvs_rev);
        esp_err_t derr = profiles_cfg_fs_delete(id);
        if (derr != ESP_OK) {
            ESP_LOGW(PCFG_FS_TAG, "could not remove stale prof%u file: %s", id, esp_err_to_name(derr));
        }
        return false;
    }

    /* Both sides decoded to something valid -- compare content, not just
     * rev, so two independently-identical profiles never log a spurious
     * divergence. */
    bool differs = memcmp(&file_profile, nvs_profile, sizeof(file_profile)) != 0;
    if (!differs) {
        *out_profile = file_profile;
        *out_rev = file_rev > nvs_rev ? file_rev : nvs_rev;
        *out_used_file = true;
        return true;
    }

    if (file_rev > nvs_rev) {
        /* STRICTLY higher, not >=: an EQUAL rev with differing bytes is only
         * reachable when an NVS writer that predates the rev counter wrote
         * the blob (firmware rolled back past the dual-write, or a crash
         * between the blob write and the rev write) -- in both cases NVS is
         * the newer copy. See check_cfg_fs_tie_break.ps1 and
         * docs/audits/filesystem_migration_review_2026-09-07.md, which found
         * this exact `>=` defect in zones_config_cfg_fs.c (since fixed
         * there too). */
        ESP_LOGW(PCFG_FS_TAG, "prof%u file/NVS DIVERGED (file rev %lu, NVS rev %lu) -- adopting FILE (strictly higher rev)",
                 id, (unsigned long)file_rev, (unsigned long)nvs_rev);
        *out_profile = file_profile;
        *out_rev = file_rev;
        *out_used_file = true;
        /* NVS resync happens on the next explicit save through
         * profiles_http.c, same convention zones_config_cfg_fs.c documents --
         * this function only decides and writes the FILE side. */
    } else {
        ESP_LOGW(PCFG_FS_TAG,
                 "prof%u file/NVS DIVERGED (file rev %lu, NVS rev %lu) -- adopting NVS (higher-or-equal rev), "
                 "resyncing file",
                 id, (unsigned long)file_rev, (unsigned long)nvs_rev);
        *out_profile = *nvs_profile;
        *out_rev = nvs_rev;
        *out_used_file = false;
        esp_err_t werr = profiles_cfg_fs_save(id, nvs_profile, nvs_rev);
        if (werr != ESP_OK && werr != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(PCFG_FS_TAG, "could not resync prof%u file from NVS: %s", id, esp_err_to_name(werr));
        }
    }
    return true;
}
