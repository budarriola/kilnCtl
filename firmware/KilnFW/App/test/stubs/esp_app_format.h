// Host-test stub -- see stubs/esp_err.h for why these exist. Added
// 2026-08-27 for ota_http.c's host tests (test_ota_http.c). Only the two
// fields ota_esp_do_transfer() actually reads (magic, chip_id) are modeled --
// never exercised by test_ota_http.c's tests (they call
// ota_http_authenticate_request() directly, never the transfer handlers),
// but the whole translation unit must still link.
#ifndef TEST_STUB_ESP_APP_FORMAT_H
#define TEST_STUB_ESP_APP_FORMAT_H

#include <stdint.h>

typedef struct {
    uint8_t magic;
    uint8_t segment_count;
    uint8_t spi_mode;
    uint8_t spi_speed_size;
    uint32_t entry_addr;
    uint8_t wp_pin;
    uint8_t spi_pin_drv[3];
    uint16_t chip_id;
    uint8_t min_chip_rev;
    uint16_t min_chip_rev_full;
    uint16_t max_chip_rev_full;
    uint8_t reserved[4];
    uint8_t hash_appended;
} esp_image_header_t;

#define ESP_IMAGE_HEADER_MAGIC 0xE9
#define ESP_CHIP_ID_ESP32S3 9

#endif // TEST_STUB_ESP_APP_FORMAT_H
