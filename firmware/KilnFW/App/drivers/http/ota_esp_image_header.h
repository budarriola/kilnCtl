// Pure validation of the first bytes of an ESP-IDF application image, the
// check ota_http_esp.c runs BEFORE esp_ota_begin() (UPDATE_PROTOCOL.md
// section 3: "ESP-IDF images already carry a magic byte and a chip ID;
// verify them before calling esp_ota_begin()").
//
// Factored out of ota_esp_do_transfer() so a host test can pin it: HTTP
// handlers are target-build-only and never link into the host suite. No
// ESP-IDF calls and no ESP-IDF headers; the field offsets below mirror the
// packed esp_image_header_t (ota_http_esp.c static_asserts them on target).
#ifndef OTA_ESP_IMAGE_HEADER_H
#define OTA_ESP_IMAGE_HEADER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* sizeof(esp_image_header_t), packed. */
#define OTA_ESP_IMAGE_HEADER_LEN 24u
/* Byte offset of the little-endian uint16 chip_id field. */
#define OTA_ESP_IMAGE_HEADER_CHIP_ID_OFFSET 12u

typedef enum {
    OTA_ESP_IMAGE_HEADER_OK = 0,
    OTA_ESP_IMAGE_HEADER_TOO_SHORT, /* NULL buffer or fewer than OTA_ESP_IMAGE_HEADER_LEN bytes */
    OTA_ESP_IMAGE_HEADER_BAD_MAGIC, /* byte 0 is not `expected_magic` */
    OTA_ESP_IMAGE_HEADER_WRONG_CHIP /* chip_id is not `expected_chip_id` */
} ota_esp_image_header_result_t;

/* Checks the magic first, then the chip id (the order the handler always
 * used, so a header wrong in both reports BAD_MAGIC). Bytes past the first
 * OTA_ESP_IMAGE_HEADER_LEN are ignored. */
ota_esp_image_header_result_t ota_esp_image_header_check(const uint8_t *buf, size_t len,
                                                         uint8_t expected_magic,
                                                         uint16_t expected_chip_id);

#ifdef __cplusplus
}
#endif

#endif // OTA_ESP_IMAGE_HEADER_H
