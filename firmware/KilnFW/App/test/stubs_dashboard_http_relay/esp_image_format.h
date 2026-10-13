// Private shim for test_dashboard_http_relay.c (see strings.h here).
#ifndef DASHBOARD_HTTP_RELAY_ESP_IMAGE_FORMAT_H
#define DASHBOARD_HTTP_RELAY_ESP_IMAGE_FORMAT_H
#include <stdint.h>
#include "esp_err.h"
typedef struct { uint32_t offset; uint32_t size; } esp_partition_pos_t;
typedef struct { uint32_t start_addr; uint32_t image_len; } esp_image_metadata_t;
esp_err_t esp_image_get_metadata(const esp_partition_pos_t *part, esp_image_metadata_t *metadata);
#endif
