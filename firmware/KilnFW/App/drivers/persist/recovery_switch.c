#include "recovery_switch.h"

#include <stdio.h>

#include "boot_guard.h"
#include "esp_app_format.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "hal_wdt.h"

static const char *TAG = "recovery_switch";

static void set_msg(char *msg, size_t cap, const char *text)
{
    if (msg && cap > 0) {
        snprintf(msg, cap, "%s", text);
    }
}

static const esp_partition_t *find_recovery(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
}

static bool running_is_factory(const esp_partition_t *recovery)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    return !running || running == recovery || running->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY;
}

static bool recovery_image_verifies(const esp_partition_t *recovery)
{
    esp_image_metadata_t data;
    const esp_partition_pos_t pos = {
        .offset = recovery->address,
        .size = recovery->size,
    };
    return esp_image_verify(ESP_IMAGE_VERIFY, &pos, &data) == ESP_OK;
}

recovery_switch_result_t recovery_switch_select_boot(char *msg, size_t cap)
{
    const esp_partition_t *recovery = find_recovery();
    if (!recovery) {
        set_msg(msg, cap, "no recovery (factory) partition in this partition table");
        return RECOVERY_SWITCH_NOT_PRESENT;
    }
    if (running_is_factory(recovery)) {
        set_msg(msg, cap, "the running image is the factory partition -- the recovery layout is not installed here");
        return RECOVERY_SWITCH_RUNNING_FACTORY;
    }
    if (!recovery_image_verifies(recovery)) {
        set_msg(msg, cap, "the recovery partition does not hold a valid image -- not switching");
        return RECOVERY_SWITCH_INVALID;
    }
    esp_err_t err = esp_ota_set_boot_partition(recovery);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition(recovery) failed: %s", esp_err_to_name(err));
        set_msg(msg, cap, "recovery image verified but could not be selected as the boot target");
        return RECOVERY_SWITCH_SET_FAILED;
    }
    set_msg(msg, cap, "recovery selected as the next boot target");
    return RECOVERY_SWITCH_OK;
}

bool recovery_switch_restore_running(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) {
        return false;
    }
    esp_err_t err = esp_ota_set_boot_partition(running);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "restoring boot target to the running partition failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

void recovery_switch_at_boot_threshold(void)
{
    const bool threshold = boot_guard_is_recovery_mode();
    if (!threshold) {
        return;
    }
    const esp_partition_t *recovery = find_recovery();
    const bool is_factory = (recovery == NULL) || running_is_factory(recovery);
    const bool valid = (recovery != NULL) && !is_factory && recovery_image_verifies(recovery);

    switch (boot_guard_decide_recovery_route(threshold, is_factory, valid)) {
    case BOOT_RECOVERY_ROUTE_SWITCH_PARTITION: {
        char msg[96];
        if (recovery_switch_select_boot(msg, sizeof(msg)) == RECOVERY_SWITCH_OK) {
            ESP_LOGE(TAG, "boot threshold reached and the recovery image verifies -- rebooting into it");
            hal_wdt_reboot();
            return; /* not reached on a real backend */
        }
        ESP_LOGE(TAG, "boot threshold reached but selecting recovery failed (%s) -- staying in degraded in-app "
                      "recovery mode", msg);
        break;
    }
    case BOOT_RECOVERY_ROUTE_DEGRADED:
        ESP_LOGE(TAG, "boot threshold reached but the recovery partition is %s -- staying in degraded in-app "
                      "recovery mode (no partition switch)",
                 is_factory ? "absent or is the running image (old layout)" : "NOT a valid image");
        break;
    case BOOT_RECOVERY_ROUTE_NORMAL:
    default:
        break;
    }
}
