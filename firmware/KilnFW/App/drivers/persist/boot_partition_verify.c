#include "boot_partition_verify.h"

#include "esp_log.h"
#include "esp_ota_ops.h"

static const char *BOOT_PART_TAG = "boot_part";

bool boot_partition_matches(const esp_partition_t *requested, const esp_partition_t *actual)
{
    return requested != NULL && actual != NULL && requested->address == actual->address &&
           requested->subtype == actual->subtype;
}

esp_err_t boot_partition_set_and_verify(const esp_partition_t *requested)
{
    if (requested == NULL) {
        ESP_LOGE(BOOT_PART_TAG, "boot partition request is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = esp_ota_set_boot_partition(requested);
    if (err != ESP_OK) {
        return err;
    }
    const esp_partition_t *actual = esp_ota_get_boot_partition();
    if (!boot_partition_matches(requested, actual)) {
        ESP_LOGE(BOOT_PART_TAG,
                 "boot partition read-back mismatch: requested '%s' (addr 0x%x subtype 0x%x), actual '%s' (addr 0x%x subtype 0x%x)",
                 requested->label, (unsigned)requested->address, (unsigned)requested->subtype,
                 actual ? actual->label : "(none)", actual ? (unsigned)actual->address : 0u,
                 actual ? (unsigned)actual->subtype : 0u);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}
