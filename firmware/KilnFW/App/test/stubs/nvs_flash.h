// Host-test stub -- see stubs/esp_err.h for why these exist. Added 2026-08-21
// for wifi_prov.c's host tests.
#ifndef TEST_STUB_NVS_FLASH_H
#define TEST_STUB_NVS_FLASH_H

#include "esp_err.h"

#define NVS_DEFAULT_PART_NAME "nvs"

static inline esp_err_t nvs_flash_init_partition(const char *partition)
{
    (void)partition;
    return ESP_OK;
}

static inline esp_err_t nvs_flash_erase_partition(const char *partition)
{
    (void)partition;
    return ESP_OK;
}

#endif // TEST_STUB_NVS_FLASH_H
