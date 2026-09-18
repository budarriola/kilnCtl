#include "pico_image_manifest.h"

#include <stddef.h>
#include <string.h>

#include "esp_log.h"
#include "hal_kv.h"
#include "nvs_key_check.h"

static const char *TAG = "pico_img_manifest";

/* Same partition/namespace as boot_guard.c's and pico_update_attempts.c's
 * records -- see boot_guard.c's NVS_KEY_REC comment for why "kiln_cfg" is
 * the one namespace on this board proven to actually persist writes across a
 * reboot on this bench unit's flash. */
#define KILN_NVS_PARTITION "kiln_nvs"
#define NVS_NAMESPACE "kiln_cfg"
/* 10 chars -- NVS_KEY_LEN_CHECK() below enforces the 15-char cap. */
#define NVS_KEY_REC "picoimgman"
NVS_KEY_LEN_CHECK(KILN_NVS_PARTITION);
NVS_KEY_LEN_CHECK(NVS_NAMESPACE);
NVS_KEY_LEN_CHECK(NVS_KEY_REC);

/* Bumped whenever the record layout changes -- same "discard rather than
 * migrate" convention as boot_guard.c / pico_update_attempts.c. A lost
 * manifest costs one skipped auto-update, never a mis-parse. */
#define PIM_RECORD_VERSION 1

typedef struct {
    uint8_t  version;
    uint8_t  reserved[3];
    uint32_t image_length;
    uint32_t image_crc32;
    uint32_t crc32; /* over every preceding byte */
} pico_image_manifest_record_t;

typedef char pim_record_size_check[(sizeof(pico_image_manifest_record_t) == 16) ? 1 : -1];

/* Table-less CRC32 (IEEE 802.3/zlib polynomial) -- same routine as
 * boot_guard.c's and pico_update_attempts.c's, copied rather than shared for
 * the same reason they are: each of these persist modules builds standalone
 * in the host tests. This is the record's own integrity check and is
 * unrelated to the IMAGE's CRC (ota_image_crc.h), which is merely a payload
 * field here. */
static uint32_t crc32_compute(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

static uint32_t record_crc(const pico_image_manifest_record_t *rec)
{
    return crc32_compute(rec, offsetof(pico_image_manifest_record_t, crc32));
}

static bool record_is_valid(const pico_image_manifest_record_t *rec)
{
    if (rec->version != PIM_RECORD_VERSION) {
        return false;
    }
    if (rec->crc32 != record_crc(rec)) {
        return false;
    }
    /* A zero-length image is not a describable image. Catching it here keeps
     * every consumer from having to special-case it. */
    return rec->image_length > 0u;
}

static hal_status_t persist_record(uint32_t image_length, uint32_t image_crc32)
{
    pico_image_manifest_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.version = PIM_RECORD_VERSION;
    rec.image_length = image_length;
    rec.image_crc32 = image_crc32;
    rec.crc32 = record_crc(&rec);

    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        return err;
    }
    err = hal_kv_set_blob(&h, NVS_KEY_REC, &rec, sizeof(rec));
    if (err == HAL_OK) {
        err = hal_kv_commit(&h);
    }
    hal_kv_close(&h);
    return err;
}

/* Missing, wrong size, wrong version and bad CRC all collapse to the same
 * "nothing valid to load" answer -- same tolerant default as boot_guard.c's
 * load_count(). */
static bool load_record(pico_image_manifest_record_t *out)
{
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        return false;
    }
    pico_image_manifest_record_t rec;
    size_t len = sizeof(rec);
    err = hal_kv_get_blob(&h, NVS_KEY_REC, &rec, &len);
    hal_kv_close(&h);
    if (err != HAL_OK || len != sizeof(rec)) {
        return false;
    }
    if (!record_is_valid(&rec)) {
        ESP_LOGW(TAG, "staged-image manifest failed its version/CRC check -- treating as no staged "
                      "image");
        return false;
    }
    *out = rec;
    return true;
}

/* Strict read-back: "could not read it back" must NOT collapse to "confirmed
 * absent", same distinction pico_update_attempts.c's verify_record() draws. */
static bool verify_record(uint32_t image_length, uint32_t image_crc32)
{
    pico_image_manifest_record_t rec;
    if (!load_record(&rec)) {
        return false;
    }
    return rec.image_length == image_length && rec.image_crc32 == image_crc32;
}

static hal_status_t erase_then_persist(uint32_t image_length, uint32_t image_crc32)
{
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        return err;
    }
    hal_status_t erase_err = hal_kv_erase_key(&h, NVS_KEY_REC);
    if (erase_err != HAL_OK && erase_err != HAL_NOT_FOUND) {
        ESP_LOGW(TAG, "erase_then_persist: hal_kv_erase_key failed: %s -- writing anyway",
                 hal_status_to_name(erase_err));
    }
    hal_kv_close(&h);
    return persist_record(image_length, image_crc32);
}

bool pico_image_manifest_store(uint32_t image_length, uint32_t image_crc32)
{
    if (image_length == 0u) {
        return false;
    }
    hal_status_t err = persist_record(image_length, image_crc32);
    bool verified = (err == HAL_OK) && verify_record(image_length, image_crc32);
    if (!verified) {
        ESP_LOGW(TAG, "staged-image manifest write did not verify on the first attempt -- retrying "
                      "once with an explicit erase-then-write");
        err = erase_then_persist(image_length, image_crc32);
        verified = (err == HAL_OK) && verify_record(image_length, image_crc32);
    }
    if (!verified) {
        ESP_LOGE(TAG, "staged-image manifest (len=%lu crc=0x%08lx) did not verify after retry -- a "
                      "later boot will not be able to re-use this staged image",
                 (unsigned long)image_length, (unsigned long)image_crc32);
    }
    return verified;
}

bool pico_image_manifest_load(uint32_t *out_image_length, uint32_t *out_image_crc32)
{
    pico_image_manifest_record_t rec;
    if (!load_record(&rec)) {
        return false;
    }
    if (out_image_length != NULL) {
        *out_image_length = rec.image_length;
    }
    if (out_image_crc32 != NULL) {
        *out_image_crc32 = rec.image_crc32;
    }
    return true;
}

bool pico_image_manifest_clear(void)
{
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        return false;
    }
    hal_status_t erase_err = hal_kv_erase_key(&h, NVS_KEY_REC);
    hal_status_t commit_err = HAL_OK;
    if (erase_err == HAL_OK) {
        commit_err = hal_kv_commit(&h);
    }
    hal_kv_close(&h);
    if (erase_err != HAL_OK && erase_err != HAL_NOT_FOUND) {
        ESP_LOGW(TAG, "pico_image_manifest_clear: hal_kv_erase_key failed: %s",
                 hal_status_to_name(erase_err));
        return false;
    }
    if (commit_err != HAL_OK) {
        return false;
    }
    pico_image_manifest_record_t rec;
    if (load_record(&rec)) {
        ESP_LOGE(TAG, "pico_image_manifest_clear: record still readable after erase+commit -- not "
                      "confirmed cleared");
        return false;
    }
    return true;
}
