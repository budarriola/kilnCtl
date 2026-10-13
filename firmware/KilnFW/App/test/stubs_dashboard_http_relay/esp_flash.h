// Private shim for test_dashboard_http_relay.c (see strings.h here).
#ifndef DASHBOARD_HTTP_RELAY_ESP_FLASH_H
#define DASHBOARD_HTTP_RELAY_ESP_FLASH_H
#include <stdint.h>
#include "esp_err.h"
typedef struct esp_flash_t esp_flash_t;
esp_err_t esp_flash_get_size(esp_flash_t *chip, uint32_t *out_size);
#endif
