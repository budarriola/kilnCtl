// See kiln_cfg_store_cfg_fs.h for the full design/rationale.
#include "kiln_cfg_store_cfg_fs.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "cfg_fs.h"

static const char *KCFG_FS_TAG = "kiln_cfg_store_fs";

static kiln_cfg_store_cfg_fs_write_fn_t s_write_fn = cfg_fs_write_atomic;

void kiln_cfg_store_cfg_fs_set_write_fn(kiln_cfg_store_cfg_fs_write_fn_t fn)
{
    s_write_fn = fn ? fn : cfg_fs_write_atomic;
}

void kiln_cfg_store_cfg_fs_reset_write_fn_for_test(void)
{
    s_write_fn = cfg_fs_write_atomic;
}

kiln_cfg_store_cfg_fs_write_fn_t kiln_cfg_store_cfg_fs_get_write_fn(void)
{
    return s_write_fn;
}

/* rev(4 bytes LE) + a byte-for-byte kiln_cfg_store_blob_t. Several KB
 * (KILN_CFG_MAX_COUNT slots x a full zones-config blob each) -- every buffer
 * of this size in this module is heap-allocated, never a stack local. See
 * this file's header comment ("STACK") for why. */
#define KCFG_FILE_BUF_MAX (4 + sizeof(kiln_cfg_store_blob_t))

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

void kiln_cfg_store_cfg_fs_load_raw(kiln_cfg_store_blob_t *out_blob, uint32_t *out_rev, bool *out_valid)
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

    uint8_t *raw = malloc(KCFG_FILE_BUF_MAX);
    if (!raw) {
        ESP_LOGW(KCFG_FS_TAG, "file read buffer alloc failed (%u bytes) -- treating as file absent, NVS "
                              "candidate decides",
                 (unsigned)KCFG_FILE_BUF_MAX);
        return;
    }
    size_t len = 0;
    esp_err_t err = cfg_fs_read(KILN_CFG_STORE_FILE_PATH, raw, KCFG_FILE_BUF_MAX, &len);
    if (err != ESP_OK) {
        /* Absent (never migrated yet), oversized, or unreadable -- none of
         * these are "found but bad" on their own; the caller's resolve()
         * decides whether that's worth a divergence warning. */
        free(raw);
        return;
    }
    if (len != KCFG_FILE_BUF_MAX) {
        /* Unlike zones_config_cfg_fs.c/profiles_cfg_fs.c, this store has no
         * versioned file layout of its own to migrate from a shorter/older
         * shape (see this module's header comment) -- ANY size other than
         * exactly "rev + one current kiln_cfg_store_blob_t" is invalid,
         * full stop. */
        ESP_LOGW(KCFG_FS_TAG, "kiln config store file is %u bytes, expected %u -- ignoring, NVS candidate "
                              "decides",
                 (unsigned)len, (unsigned)KCFG_FILE_BUF_MAX);
        free(raw);
        return;
    }

    uint32_t rev = get_u32_le(raw);
    kiln_cfg_store_blob_t cand;
    memcpy(&cand, raw + 4, sizeof(cand));
    free(raw);

    if (cand.version != KILN_CFG_STORE_VERSION) {
        ESP_LOGW(KCFG_FS_TAG,
                 "kiln config store file (rev %lu) claims version %u, this firmware wants %u -- ignoring "
                 "file, NVS candidate decides",
                 (unsigned long)rev, (unsigned)cand.version, (unsigned)KILN_CFG_STORE_VERSION);
        return;
    }

    *out_blob = cand;
    *out_rev = rev;
    *out_valid = true;
}

esp_err_t kiln_cfg_store_cfg_fs_save(const kiln_cfg_store_blob_t *blob, uint32_t rev)
{
    if (!blob) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!cfg_fs_is_available()) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t *raw = malloc(KCFG_FILE_BUF_MAX);
    if (!raw) {
        ESP_LOGE(KCFG_FS_TAG, "file write buffer alloc failed (%u bytes) -- refusing to write",
                 (unsigned)KCFG_FILE_BUF_MAX);
        return ESP_ERR_NO_MEM;
    }
    put_u32_le(raw, rev);
    memcpy(raw + 4, blob, sizeof(*blob));

    esp_err_t err = s_write_fn(KILN_CFG_STORE_FILE_PATH, raw, KCFG_FILE_BUF_MAX);
    free(raw);
    if (err != ESP_OK) {
        ESP_LOGW(KCFG_FS_TAG, "kiln config store file write (rev %lu) failed: %s", (unsigned long)rev,
                 esp_err_to_name(err));
    }
    return err;
}

bool kiln_cfg_store_cfg_fs_resolve(const kiln_cfg_store_blob_t *nvs_blob, bool nvs_valid, uint32_t nvs_rev,
                                    kiln_cfg_store_blob_t *out_blob, uint32_t *out_rev, bool *out_used_file)
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

    kiln_cfg_store_blob_t *file_blob = malloc(sizeof(kiln_cfg_store_blob_t));
    if (!file_blob) {
        /* Cannot even attempt the file side this call -- degrade to the NVS
         * candidate exactly like "partition absent"/"file absent" would. */
        ESP_LOGW(KCFG_FS_TAG, "resolve: scratch buffer alloc failed -- falling back to NVS candidate");
        *out_blob = *nvs_blob;
        *out_rev = nvs_rev;
        *out_used_file = false;
        return nvs_valid;
    }

    uint32_t file_rev = 0;
    bool file_valid = false;
    kiln_cfg_store_cfg_fs_load_raw(file_blob, &file_rev, &file_valid);

    if (!file_valid) {
        /* No usable file. Fall back to the NVS candidate, and if it is
         * itself trustworthy, write it out -- lazy, one-shot migration, no
         * separate migration task. */
        *out_blob = *nvs_blob;
        *out_rev = nvs_rev;
        *out_used_file = false;
        if (nvs_valid) {
            esp_err_t werr = kiln_cfg_store_cfg_fs_save(nvs_blob, nvs_rev);
            if (werr != ESP_OK && werr != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(KCFG_FS_TAG, "could not migrate NVS kiln config store to file: %s", esp_err_to_name(werr));
            }
        }
        free(file_blob);
        return nvs_valid;
    }

    if (!nvs_valid) {
        /* File is good, NVS side has nothing trustworthy -- use the file
         * outright. Not logged as a divergence: nothing on the NVS side to
         * disagree WITH. */
        *out_blob = *file_blob;
        *out_rev = file_rev;
        *out_used_file = true;
        free(file_blob);
        return true;
    }

    /* Both sides decoded to something valid -- compare content, not just
     * rev, so two independently-arrived-at-identical stores never get
     * logged as a spurious divergence. */
    bool differs = memcmp(file_blob, nvs_blob, sizeof(*file_blob)) != 0;
    if (!differs) {
        *out_blob = *file_blob;
        *out_rev = file_rev > nvs_rev ? file_rev : nvs_rev;
        *out_used_file = true;
        free(file_blob);
        return true;
    }

    /* DIVERGENCE TIE-BREAK: the file wins only on a STRICTLY higher rev.
     * An EQUAL rev with differing content means NVS -- see this module's
     * header comment and check_cfg_fs_tie_break.ps1, which enforces `>` (not
     * `>=`) between file_rev and nvs_rev in every *_cfg_fs.c bridge module. */
    if (file_rev > nvs_rev) {
        ESP_LOGW(KCFG_FS_TAG,
                 "kiln config store file/NVS DIVERGED (file rev %lu, NVS rev %lu) -- adopting FILE (strictly "
                 "higher rev)",
                 (unsigned long)file_rev, (unsigned long)nvs_rev);
        *out_blob = *file_blob;
        *out_rev = file_rev;
        *out_used_file = true;
        /* NVS resync happens on the next nvs_save_store() call the caller
         * drives once it adopts this result -- this module only ever writes
         * the file side (see header comment). */
    } else {
        ESP_LOGW(KCFG_FS_TAG,
                 "kiln config store file/NVS DIVERGED (file rev %lu, NVS rev %lu) -- adopting NVS "
                 "(higher-or-equal rev), resyncing file",
                 (unsigned long)file_rev, (unsigned long)nvs_rev);
        *out_blob = *nvs_blob;
        *out_rev = nvs_rev;
        *out_used_file = false;
        esp_err_t werr = kiln_cfg_store_cfg_fs_save(nvs_blob, nvs_rev);
        if (werr != ESP_OK && werr != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(KCFG_FS_TAG, "could not resync kiln config store file from NVS: %s", esp_err_to_name(werr));
        }
    }
    free(file_blob);
    return true;
}
