#include "ota_record.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "ota_record";

/* Same namespace as run_state.c/relay_cycles.c/zones_http.c, own key -- see
 * run_state.c's header comment for why a shared blob is the wrong move (a
 * corrupt/rejected record here must never be able to take zone config down
 * with it, and vice versa). */
#define NVS_NAMESPACE "kiln_cfg"
#define NVS_KEY_OTA_RECORD "ota_record"

/* TODO.md 8.1's shared partition, split out of the default NVS partition. */
#define KILN_NVS_PARTITION "kiln_nvs"

/* Pins the on-disk layout the same way run_state.c's own _Static_assert
 * does: a silent field add/reorder would otherwise look like corruption to
 * anything that later tries to load this record, rather than fail the
 * build. There is no loader in this first pass (see ota_record.h -- only
 * the most recent record is kept and nothing currently reads it back), but
 * pinning the size now costs nothing and pays off the day something does.
 *
 * A plain _Static_assert() here builds fine under ESP-IDF's GCC, but this
 * file is ALSO compiled into App/test/build_host_tests.ps1's MSVC host-test
 * binary (added 2026-08-21 alongside the image_sha256_hex field, so
 * ota_record_fill() could get a real host test) -- and the cl.exe invocation
 * that script uses compiles .c files in a C mode old enough that
 * _Static_assert is a hard syntax error, not just unavailable. The classic
 * negative-array-size trick below is portable C89/C99/C11 alike and checks
 * exactly the same thing. */
typedef char ota_record_t_size_check[(sizeof(ota_record_t) == 216) ? 1 : -1];

/* Brings up KILN_NVS_PARTITION, erasing ONLY that partition if its contents
 * are unusable -- identical to run_state.c's/relay_cycles.c's own
 * nvs_partition_init(), duplicated rather than shared because each of those
 * modules already duplicates it independently (see run_state.c's header
 * comment: "each of which manages its own init/migration independently").
 * Cheap to call on every append: nvs_flash_init_partition() is a no-op
 * success if the partition is already initialized. */
static esp_err_t nvs_partition_init(const char *partition)
{
    esp_err_t err = nvs_flash_init_partition(partition);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition '%s' needs erase (%s) -- erasing THAT PARTITION ONLY and retrying",
                 partition, esp_err_to_name(err));
        err = nvs_flash_erase_partition(partition);
        if (err == ESP_OK) {
            err = nvs_flash_init_partition(partition);
        }
    }
    return err;
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

void ota_record_fill(ota_record_t *out, uint32_t uptime_s, const char *processor,
                      const char *version_before, const char *version_after, bool success,
                      const char *reason, const char *image_sha256_hex_or_null)
{
    memset(out, 0, sizeof(*out));
    out->version = OTA_RECORD_VERSION;
    out->uptime_s = uptime_s;
    copy_str(out->processor, sizeof(out->processor), processor);
    copy_str(out->version_before, sizeof(out->version_before), version_before);
    copy_str(out->version_after, sizeof(out->version_after), version_after);
    out->success = success ? 1u : 0u;
    copy_str(out->reason, sizeof(out->reason), reason);
    copy_str(out->image_sha256_hex, sizeof(out->image_sha256_hex), image_sha256_hex_or_null);
}

esp_err_t ota_record_append(const ota_record_t *rec)
{
    if (!rec) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t part_err = nvs_partition_init(KILN_NVS_PARTITION);
    if (part_err != ESP_OK) {
        ESP_LOGE(TAG, "NVS partition '%s' init failed: %s -- update record not saved",
                 KILN_NVS_PARTITION, esp_err_to_name(part_err));
        return part_err;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(KILN_NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open_from_partition failed: %s -- update record not saved",
                 esp_err_to_name(err));
        return err;
    }
    err = nvs_set_blob(h, NVS_KEY_OTA_RECORD, rec, sizeof(*rec));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        /* Best-effort, like run_state.c's own persist failures -- an update
         * record that failed to save is not a reason to have refused (or
         * un-refused) the update itself, but it must not be silent: this is
         * the only place the outcome would otherwise be recorded at all. */
        ESP_LOGE(TAG, "could not persist OTA update record: %s -- this update's outcome will not "
                      "survive a reboot",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "OTA update record saved: processor=%s success=%d reason=\"%s\" "
                      "version %s -> %s sha256=%s",
                 rec->processor, (int)rec->success, rec->reason, rec->version_before,
                 rec->version_after, rec->image_sha256_hex[0] ? rec->image_sha256_hex : "(none)");
    }
    return err;
}

esp_err_t ota_record_load(ota_record_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t part_err = nvs_partition_init(KILN_NVS_PARTITION);
    if (part_err != ESP_OK) {
        ESP_LOGE(TAG, "NVS partition '%s' init failed: %s -- cannot read update record",
                 KILN_NVS_PARTITION, esp_err_to_name(part_err));
        return part_err;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(KILN_NVS_PARTITION, NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        /* ESP_ERR_NVS_NOT_FOUND here means the namespace itself has never
         * been written -- same "no record yet" case as the blob-not-found
         * path below, not a real failure. */
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGE(TAG, "nvs_open_from_partition (read) failed: %s", esp_err_to_name(err));
        }
        return err;
    }

    size_t len = sizeof(*out);
    err = nvs_get_blob(h, NVS_KEY_OTA_RECORD, out, &len);
    nvs_close(h);

    if (err == ESP_OK && len != sizeof(*out)) {
        /* Same "a size-mismatched blob is not a current record" tolerance
         * ota_record.h's doc comment on this function promises -- treat it
         * as absent rather than hand back a partially-filled struct. */
        ESP_LOGW(TAG, "stored OTA record is %u bytes, expected %u -- treating as absent",
                 (unsigned)len, (unsigned)sizeof(*out));
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "nvs_get_blob(ota_record) failed: %s", esp_err_to_name(err));
    }
    return err;
}
