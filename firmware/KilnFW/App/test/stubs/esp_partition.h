// Host-test stub -- see stubs/esp_err.h for why these exist. Added
// 2026-08-27 for ota_http.c's host tests (test_ota_http.c). Every function
// here is declared only, defined per-executable (same "declared once,
// defined per test file" split stubs/esp_http_server.h already uses) --
// none of these is ever invoked by test_ota_http.c's tests (only
// ota_http_verify_request()/ota_http_authenticate_request() are called
// directly), but the whole translation unit must still link.
#ifndef TEST_STUB_ESP_PARTITION_H
#define TEST_STUB_ESP_PARTITION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint32_t address;
    uint32_t size;
    char label[17];
    int type;
    int subtype;
    bool encrypted;
} esp_partition_t;

esp_err_t esp_partition_write(const esp_partition_t *partition, size_t dst_offset, const void *src, size_t size);
esp_err_t esp_partition_erase_range(const esp_partition_t *partition, size_t offset, size_t size);
uint32_t esp_partition_get_main_flash_sector_size(void);

#endif // TEST_STUB_ESP_PARTITION_H
