// Host-test stub, private to test_recovery_switch.c (build_host_tests.ps1
// puts this directory on the include path ahead of the shared stubs/). Only
// what recovery_switch.c uses: esp_image_verify() over a partition position.
// esp_image_verify() itself is defined by the test file.
#ifndef TEST_STUB_ESP_IMAGE_FORMAT_H
#define TEST_STUB_ESP_IMAGE_FORMAT_H

#include <stdint.h>

#include "esp_err.h"

typedef enum {
    ESP_IMAGE_VERIFY = 0,
    ESP_IMAGE_VERIFY_SILENT,
} esp_image_load_mode_t;

typedef struct {
    uint32_t offset;
    uint32_t size;
} esp_partition_pos_t;

typedef struct {
    uint32_t start_addr;
    uint32_t image_len;
} esp_image_metadata_t;

esp_err_t esp_image_verify(esp_image_load_mode_t mode, const esp_partition_pos_t *part,
                           esp_image_metadata_t *data);

#endif // TEST_STUB_ESP_IMAGE_FORMAT_H
