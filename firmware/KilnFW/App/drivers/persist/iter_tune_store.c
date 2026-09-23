#include "iter_tune_store.h"

#include <string.h>

#include "esp_log.h"
#include "hal_kv.h"
#include "nvs_key_check.h"

#include "cfg_fs.h"

static const char *TAG = "iter_tune_store";

// Own partition: same "kiln_nvs" data partition every other non-safety-
// critical store in this tree uses (see kiln_cfg_store.c's header comment
// for why -- it survives an ordinary reflash untouched). NEW, INDEPENDENT
// namespace, deliberately never "adap_tune" (the old module's namespace).
#define ITER_TUNE_NVS_PARTITION "kiln_nvs"
NVS_KEY_LEN_CHECK(ITER_TUNE_NVS_PARTITION);

#define ITER_TUNE_NVS_NAMESPACE "iter_tune"
#define ITER_TUNE_NVS_KEY_BLOB "ittblob"
#define ITER_TUNE_NVS_KEY_REV "ittrev"
NVS_KEY_LEN_CHECK(ITER_TUNE_NVS_NAMESPACE);
NVS_KEY_LEN_CHECK(ITER_TUNE_NVS_KEY_BLOB);
NVS_KEY_LEN_CHECK(ITER_TUNE_NVS_KEY_REV);

// cfg LittleFS dual-write file, same "<4-byte LE rev><raw blob>" shape as
// zones_config_cfg_fs.c/kiln_cfg_store_cfg_fs.c.
#define ITER_TUNE_CFG_FILE_PATH "iter_tune.bin"
#define ITER_TUNE_FILE_BUF_MAX (4 + sizeof(iter_tune_store_blob_t))

static iter_tune_store_blob_t s_blob;
static bool s_loaded;      // true once iter_tune_store_start() ran (even if it found nothing)
static uint32_t s_rev;

static void put_u32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t get_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool iter_tune_store_blob_validate(const void *bytes, size_t len) {
    if (bytes == NULL || len != sizeof(iter_tune_store_blob_t)) {
        return false;
    }
    iter_tune_store_blob_t tmp;
    memcpy(&tmp, bytes, sizeof(tmp));
    if (tmp.version != ITER_TUNE_STORE_VERSION) {
        return false;
    }
    if (tmp.zone_count > ITER_TUNE_STORE_MAX_ZONES) {
        return false;
    }
    for (uint8_t i = 0; i < tmp.zone_count; i++) {
        const iter_tune_store_zone_t *z = &tmp.zone[i];
        if (z->enabled > 1 || z->has_anchor > 1 || z->has_baseline > 1) {
            return false;
        }
    }
    return true;
}

static bool nvs_load_raw(iter_tune_store_blob_t *out, uint32_t *out_rev) {
    hal_kv_handle_t h;
    if (hal_kv_open(&h, ITER_TUNE_NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, ITER_TUNE_NVS_PARTITION) != HAL_OK) {
        return false;
    }
    size_t len = sizeof(*out);
    hal_status_t rc = hal_kv_get_blob(&h, ITER_TUNE_NVS_KEY_BLOB, out, &len);
    uint32_t rev = 0;
    hal_kv_get_u32(&h, ITER_TUNE_NVS_KEY_REV, &rev);
    hal_kv_close(&h);
    if (rc != HAL_OK || !iter_tune_store_blob_validate(out, len)) {
        return false;
    }
    *out_rev = rev;
    return true;
}

static bool nvs_save_raw(const iter_tune_store_blob_t *in, uint32_t rev) {
    hal_kv_handle_t h;
    if (hal_kv_open(&h, ITER_TUNE_NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, ITER_TUNE_NVS_PARTITION) != HAL_OK) {
        ESP_LOGE(TAG, "nvs open failed");
        return false;
    }
    bool ok = hal_kv_set_blob(&h, ITER_TUNE_NVS_KEY_BLOB, in, sizeof(*in)) == HAL_OK;
    ok = ok && hal_kv_set_u32(&h, ITER_TUNE_NVS_KEY_REV, rev) == HAL_OK;
    ok = ok && hal_kv_commit(&h) == HAL_OK;
    hal_kv_close(&h);
    if (!ok) {
        ESP_LOGE(TAG, "nvs write failed");
    }
    return ok;
}

// File wins ONLY on a strictly higher rev than NVS -- same tie-break
// convention as kiln_cfg_store_cfg_fs_resolve()/zones_config_cfg_fs.c.
static bool cfg_fs_load_raw(iter_tune_store_blob_t *out, uint32_t *out_rev) {
    if (!cfg_fs_is_available()) {
        return false;
    }
    uint8_t buf[ITER_TUNE_FILE_BUF_MAX];
    size_t out_len = 0;
    if (cfg_fs_read(ITER_TUNE_CFG_FILE_PATH, buf, sizeof(buf), &out_len) != ESP_OK) {
        return false;
    }
    if (out_len != 4 + sizeof(iter_tune_store_blob_t)) {
        return false;
    }
    uint32_t rev = get_u32_le(buf);
    if (!iter_tune_store_blob_validate(buf + 4, out_len - 4)) {
        return false;
    }
    memcpy(out, buf + 4, sizeof(*out));
    *out_rev = rev;
    return true;
}

static void cfg_fs_save_raw(const iter_tune_store_blob_t *in, uint32_t rev) {
    if (!cfg_fs_is_available()) {
        return;
    }
    uint8_t buf[ITER_TUNE_FILE_BUF_MAX];
    put_u32_le(buf, rev);
    memcpy(buf + 4, in, sizeof(*in));
    esp_err_t err = cfg_fs_write_atomic(ITER_TUNE_CFG_FILE_PATH, buf, sizeof(buf));
    if (err != ESP_OK) {
        // Non-fatal: NVS is authoritative and already holds the write.
        ESP_LOGE(TAG, "cfg_fs dual-write failed: %d", (int)err);
    }
}

esp_err_t iter_tune_store_start(void) {
    iter_tune_store_blob_t nvs_blob;
    uint32_t nvs_rev = 0;
    bool nvs_ok = nvs_load_raw(&nvs_blob, &nvs_rev);

    iter_tune_store_blob_t file_blob;
    uint32_t file_rev = 0;
    bool file_ok = cfg_fs_load_raw(&file_blob, &file_rev);

    // Log a rev disagreement the same way zones_config_cfg_fs.c/
    // kiln_cfg_store_cfg_fs.c do (Opus review of 5f2acb7f, advisory A5) --
    // both sides being present but not agreeing is worth a boot-time
    // breadcrumb even though the tie-break below resolves it safely either
    // way.
    if (file_ok && nvs_ok && file_rev != nvs_rev) {
        ESP_LOGW(TAG, "iter_tune file/NVS DIVERGED (file rev %lu, NVS rev %lu) -- adopting %s (strictly higher rev wins)",
                 (unsigned long)file_rev, (unsigned long)nvs_rev, (file_rev > nvs_rev) ? "FILE" : "NVS");
    }

    if (file_ok && (!nvs_ok || file_rev > nvs_rev)) {
        s_blob = file_blob;
        s_rev = file_rev;
        // File was ahead of NVS -- catch NVS up so both sides agree going
        // forward (same repair-on-read convention kiln_cfg_store_cfg_fs
        // uses).
        nvs_save_raw(&s_blob, s_rev);
    } else if (nvs_ok) {
        s_blob = nvs_blob;
        s_rev = nvs_rev;
    } else {
        memset(&s_blob, 0, sizeof(s_blob));
        s_blob.version = ITER_TUNE_STORE_VERSION;
        s_blob.zone_count = 0;
        s_rev = 0;
    }
    s_loaded = true;
    return ESP_OK;
}

bool iter_tune_store_get_zone(uint8_t zone_index, iter_tune_store_zone_t *out) {
    if (!s_loaded || zone_index >= ITER_TUNE_STORE_MAX_ZONES || zone_index >= s_blob.zone_count) {
        return false;
    }
    if (out != NULL) {
        *out = s_blob.zone[zone_index];
    }
    return true;
}

esp_err_t iter_tune_store_set_zone(uint8_t zone_index, const iter_tune_store_zone_t *in) {
    if (zone_index >= ITER_TUNE_STORE_MAX_ZONES || in == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_loaded) {
        s_blob.version = ITER_TUNE_STORE_VERSION;
        s_blob.zone_count = 0;
        s_rev = 0;
        s_loaded = true;
    }
    s_blob.zone[zone_index] = *in;
    if (zone_index + 1 > s_blob.zone_count) {
        s_blob.zone_count = (uint8_t)(zone_index + 1);
    }
    s_rev++;
    bool nvs_ok = nvs_save_raw(&s_blob, s_rev);
    cfg_fs_save_raw(&s_blob, s_rev);
    return nvs_ok ? ESP_OK : ESP_FAIL;
}

void iter_tune_store_reset_for_test(void) {
    memset(&s_blob, 0, sizeof(s_blob));
    s_blob.version = ITER_TUNE_STORE_VERSION;
    s_rev = 0;
    s_loaded = false;
}
