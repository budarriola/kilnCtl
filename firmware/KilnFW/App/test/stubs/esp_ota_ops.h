// Host-test stub -- see stubs/esp_err.h for why these exist. Added
// 2026-08-27 for ota_http.c's host tests (test_ota_http.c). Every function
// here is declared only, defined per-executable, same convention
// stubs/esp_partition.h next to this file uses -- none is ever invoked by
// test_ota_http.c's tests (only ota_http_authenticate_request() is called
// directly), but the whole translation unit must still link.
#ifndef TEST_STUB_ESP_OTA_OPS_H
#define TEST_STUB_ESP_OTA_OPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_partition.h"

typedef uint32_t esp_ota_handle_t;

const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *start_from);
const esp_partition_t *esp_ota_get_running_partition(void);
esp_err_t esp_ota_begin(const esp_partition_t *partition, size_t image_size, esp_ota_handle_t *out_handle);
esp_err_t esp_ota_write(esp_ota_handle_t handle, const void *data, size_t size);
esp_err_t esp_ota_end(esp_ota_handle_t handle);
esp_err_t esp_ota_abort(esp_ota_handle_t handle);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *partition);
esp_err_t esp_ota_get_partition_description(const esp_partition_t *partition, esp_app_desc_t *out);
bool esp_ota_check_rollback_is_possible(void);
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot(void);

#endif // TEST_STUB_ESP_OTA_OPS_H
