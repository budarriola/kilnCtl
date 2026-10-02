// recovery_image_check.c -- see recovery_image_check.h. No ESP-IDF includes.
#include "recovery_image_check.h"

#include <string.h>

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

ric_result_t ric_validate_first_chunk(const uint8_t *buf, size_t len, size_t content_len,
                                      size_t partition_size, const char *expected_project)
{
    if (content_len == 0) {
        return RIC_NO_LENGTH;
    }
    if (content_len > partition_size) {
        return RIC_OVERSIZE;
    }
    if (content_len < RIC_MIN_FIRST_CHUNK || len < RIC_MIN_FIRST_CHUNK || len > content_len) {
        return RIC_TRUNCATED;
    }
    if (buf[0] != RIC_IMAGE_MAGIC) {
        return RIC_BAD_MAGIC;
    }
    unsigned chip_id = (unsigned)buf[12] | ((unsigned)buf[13] << 8);
    if (chip_id != RIC_CHIP_ID_ESP32S3) {
        return RIC_WRONG_CHIP;
    }
    unsigned seg_count = buf[1];
    uint32_t seg0_len = rd32(buf + 24 + 4);
    if (seg_count < 1 || seg_count > 16 || seg0_len < RIC_APP_DESC_LEN ||
        (size_t)seg0_len > content_len - RIC_APP_DESC_OFFSET) {
        return RIC_BAD_SEGMENT;
    }
    const uint8_t *desc = buf + RIC_APP_DESC_OFFSET;
    if (rd32(desc) != RIC_APP_DESC_MAGIC) {
        return RIC_BAD_APP_DESC;
    }
    const char *name = (const char *)(desc + 48);
    size_t n = 0;
    while (n < 32 && name[n] != '\0') {
        n++;
    }
    if (n == 32 || strlen(expected_project) != n || memcmp(name, expected_project, n) != 0) {
        return RIC_WRONG_PROJECT;
    }
    return RIC_OK;
}

const char *ric_message(ric_result_t r)
{
    switch (r) {
    case RIC_OK: return "ok";
    case RIC_NO_LENGTH: return "Content-Length required (chunked upload not supported)";
    case RIC_OVERSIZE: return "image larger than the app partition";
    case RIC_TRUNCATED: return "image too short to contain an ESP image header and app descriptor";
    case RIC_BAD_MAGIC: return "not an ESP application image (bad magic byte)";
    case RIC_WRONG_CHIP: return "image is not built for ESP32-S3";
    case RIC_BAD_SEGMENT: return "image segment header is implausible";
    case RIC_BAD_APP_DESC: return "image has no application descriptor";
    case RIC_WRONG_PROJECT: return "image project name is not the KilnFW main application";
    }
    return "invalid image";
}

int ric_http_status(ric_result_t r)
{
    return r == RIC_OVERSIZE ? 413 : 400;
}

uint32_t ric_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

int ric_boot_guard_decode(const uint8_t *blob, size_t len, uint32_t *count)
{
    if (len != 12 || blob[0] != 1) {
        return 0;
    }
    if (rd32(blob + 8) != ric_crc32(blob, 8)) {
        return 0;
    }
    *count = rd32(blob + 4);
    return 1;
}
