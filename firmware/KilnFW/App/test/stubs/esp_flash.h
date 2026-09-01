// Host-test stub -- see stubs/esp_err.h for why these exist. Added for
// dashboard_http.c's host tests: only esp_flash_get_size() is called there
// (build_firmware_statics()), never actually invoked by the JSON-budget
// tests (they call profile_executor_get_status()/append_zone_status_json()
// directly, not the handlers that reach this), but the whole translation
// unit must still link.
#ifndef TEST_STUB_ESP_FLASH_H
#define TEST_STUB_ESP_FLASH_H

#include <stdint.h>

#include "esp_err.h"

typedef struct esp_flash_t esp_flash_t;

esp_err_t esp_flash_get_size(esp_flash_t *chip, uint32_t *out_size);

#endif // TEST_STUB_ESP_FLASH_H
