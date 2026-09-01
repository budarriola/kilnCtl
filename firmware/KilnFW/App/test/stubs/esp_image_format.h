// Host-test stub -- see stubs/esp_err.h for why these exist. Added for
// dashboard_http.c's host tests: only esp_image_get_metadata() is called
// there (build_firmware_statics()'s flash_used estimate), never actually
// invoked by the JSON-budget tests, but the whole translation unit must
// still link.
#ifndef TEST_STUB_ESP_IMAGE_FORMAT_H
#define TEST_STUB_ESP_IMAGE_FORMAT_H

#include <stdint.h>

#include "esp_err.h"
#include "esp_partition.h"

typedef struct {
    uint32_t offset;
    uint32_t size;
} esp_partition_pos_t;

typedef struct {
    uint32_t image_len;
} esp_image_metadata_t;

esp_err_t esp_image_get_metadata(const esp_partition_pos_t *part, esp_image_metadata_t *data);

#endif // TEST_STUB_ESP_IMAGE_FORMAT_H
