// See ota_esp_image_header.h.
#include "ota_esp_image_header.h"

ota_esp_image_header_result_t ota_esp_image_header_check(const uint8_t *buf, size_t len,
                                                         uint8_t expected_magic,
                                                         uint16_t expected_chip_id)
{
    if (buf == NULL || len < OTA_ESP_IMAGE_HEADER_LEN) {
        return OTA_ESP_IMAGE_HEADER_TOO_SHORT;
    }
    if (buf[0] != expected_magic) {
        return OTA_ESP_IMAGE_HEADER_BAD_MAGIC;
    }
    uint16_t chip_id = (uint16_t)((uint16_t)buf[OTA_ESP_IMAGE_HEADER_CHIP_ID_OFFSET] |
                                  ((uint16_t)buf[OTA_ESP_IMAGE_HEADER_CHIP_ID_OFFSET + 1u] << 8));
    if (chip_id != expected_chip_id) {
        return OTA_ESP_IMAGE_HEADER_WRONG_CHIP;
    }
    return OTA_ESP_IMAGE_HEADER_OK;
}
