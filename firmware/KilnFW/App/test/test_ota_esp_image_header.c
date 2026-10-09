// Host tests for App/drivers/http/ota_esp_image_header.c -- the pre-
// esp_ota_begin() magic/chip_id refusal of ota_http_esp.c, previously
// untested because the HTTP handler is target-build-only.
#include <stdio.h>
#include <string.h>

#include "test_common.h"

#include "ota_esp_image_header.h"

#define MAGIC 0xE9
#define CHIP_S3 9

static void make_hdr(uint8_t *h, uint8_t magic, uint16_t chip)
{
    memset(h, 0, OTA_ESP_IMAGE_HEADER_LEN);
    h[0] = magic;
    h[OTA_ESP_IMAGE_HEADER_CHIP_ID_OFFSET] = (uint8_t)(chip & 0xFF);
    h[OTA_ESP_IMAGE_HEADER_CHIP_ID_OFFSET + 1] = (uint8_t)(chip >> 8);
}

static ota_esp_image_header_result_t chk(const uint8_t *h, size_t n)
{
    return ota_esp_image_header_check(h, n, MAGIC, CHIP_S3);
}

static void test_valid_header_accepted(void)
{
    uint8_t h[OTA_ESP_IMAGE_HEADER_LEN];
    make_hdr(h, MAGIC, CHIP_S3);
    TEST_CHECK(chk(h, sizeof(h)) == OTA_ESP_IMAGE_HEADER_OK, "valid magic + ESP32-S3 chip id accepted");

    uint8_t big[64];
    memset(big, 0xA5, sizeof(big));
    make_hdr(big, MAGIC, CHIP_S3);
    TEST_CHECK(chk(big, sizeof(big)) == OTA_ESP_IMAGE_HEADER_OK, "bytes past the header are ignored");

    /* Other header fields are not part of the refusal. */
    h[1] = 7; h[2] = 2; h[3] = 0x20; h[4] = 0x11; h[23] = 1;
    TEST_CHECK(chk(h, sizeof(h)) == OTA_ESP_IMAGE_HEADER_OK, "unrelated header fields do not matter");
}

static void test_bad_magic_refused(void)
{
    uint8_t h[OTA_ESP_IMAGE_HEADER_LEN];
    make_hdr(h, 0x00, CHIP_S3);
    TEST_CHECK(chk(h, sizeof(h)) == OTA_ESP_IMAGE_HEADER_BAD_MAGIC, "magic 0x00 refused");
    make_hdr(h, MAGIC ^ 0x01, CHIP_S3);
    TEST_CHECK(chk(h, sizeof(h)) == OTA_ESP_IMAGE_HEADER_BAD_MAGIC, "one-bit-off magic refused");
    make_hdr(h, 0xFF, CHIP_S3);
    TEST_CHECK(chk(h, sizeof(h)) == OTA_ESP_IMAGE_HEADER_BAD_MAGIC, "erased-flash magic 0xFF refused");
    make_hdr(h, 0x00, 0x1234);
    TEST_CHECK(chk(h, sizeof(h)) == OTA_ESP_IMAGE_HEADER_BAD_MAGIC, "bad magic AND bad chip reports magic first");
}

static void test_wrong_chip_refused(void)
{
    uint8_t h[OTA_ESP_IMAGE_HEADER_LEN];
    make_hdr(h, MAGIC, 0);
    TEST_CHECK(chk(h, sizeof(h)) == OTA_ESP_IMAGE_HEADER_WRONG_CHIP, "chip id 0 (ESP32) refused");
    make_hdr(h, MAGIC, 5);
    TEST_CHECK(chk(h, sizeof(h)) == OTA_ESP_IMAGE_HEADER_WRONG_CHIP, "chip id 5 (ESP32-C3) refused");
    make_hdr(h, MAGIC, CHIP_S3 + 1);
    TEST_CHECK(chk(h, sizeof(h)) == OTA_ESP_IMAGE_HEADER_WRONG_CHIP, "chip id 10 refused");
    make_hdr(h, MAGIC, 0x0900);
    TEST_CHECK(chk(h, sizeof(h)) == OTA_ESP_IMAGE_HEADER_WRONG_CHIP, "high byte matters (0x0900 != 9)");
    make_hdr(h, MAGIC, 0xFFFF);
    TEST_CHECK(chk(h, sizeof(h)) == OTA_ESP_IMAGE_HEADER_WRONG_CHIP, "chip id 0xFFFF refused");
    make_hdr(h, MAGIC, CHIP_S3);
    h[OTA_ESP_IMAGE_HEADER_CHIP_ID_OFFSET - 1] = CHIP_S3;
    h[OTA_ESP_IMAGE_HEADER_CHIP_ID_OFFSET] = 0;
    TEST_CHECK(chk(h, sizeof(h)) == OTA_ESP_IMAGE_HEADER_WRONG_CHIP, "chip id is read at offset 12, not 11");
}

static void test_short_and_null_buffers(void)
{
    uint8_t h[OTA_ESP_IMAGE_HEADER_LEN];
    make_hdr(h, MAGIC, CHIP_S3);
    TEST_CHECK(chk(h, 0) == OTA_ESP_IMAGE_HEADER_TOO_SHORT, "length 0 is too short");
    TEST_CHECK(chk(h, 1) == OTA_ESP_IMAGE_HEADER_TOO_SHORT, "length 1 is too short");
    TEST_CHECK(chk(h, OTA_ESP_IMAGE_HEADER_CHIP_ID_OFFSET + 2) == OTA_ESP_IMAGE_HEADER_TOO_SHORT,
               "buffer ending right after chip_id is still too short");
    TEST_CHECK(chk(h, OTA_ESP_IMAGE_HEADER_LEN - 1) == OTA_ESP_IMAGE_HEADER_TOO_SHORT, "23 bytes is too short");
    TEST_CHECK(chk(h, OTA_ESP_IMAGE_HEADER_LEN) == OTA_ESP_IMAGE_HEADER_OK, "exactly 24 bytes is enough");
    TEST_CHECK(chk(NULL, OTA_ESP_IMAGE_HEADER_LEN) == OTA_ESP_IMAGE_HEADER_TOO_SHORT, "NULL buffer is too short");
    /* A short buffer is reported short even when its magic is wrong. */
    h[0] = 0;
    TEST_CHECK(chk(h, 5) == OTA_ESP_IMAGE_HEADER_TOO_SHORT, "short wins over bad magic");
}

void run_test_ota_esp_image_header(void)
{
    TEST_SECTION("ota_esp_image_header");
    test_valid_header_accepted();
    test_bad_magic_refused();
    test_wrong_chip_refused();
    test_short_and_null_buffers();
}
