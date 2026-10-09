// recovery_image_check.h -- pure (no ESP-IDF) validation of the FIRST chunk of
// an uploaded ESP application image, plus a decoder for boot_guard's persisted
// record. Split out of recovery_http.c so both are host-testable
// (check_recovery_image_check.ps1 builds test_recovery_image_check.c against
// this file with MSVC).
//
// Image layout checked (ESP-IDF esp_image_format.h, little-endian):
//   0x00  esp_image_header_t: magic 0xE9, segment_count, ..., chip_id @0x0C (u16)
//   0x18  first esp_image_segment_header_t: load_addr u32, data_len u32
//   0x20  first segment data begins with esp_app_desc_t (256 bytes):
//           +0x00 magic_word 0xABCD5432, +0x10 version[32], +0x30 project_name[32]
// The expected project name is what firmware/KilnFW's project(KilnCtrl) puts
// in esp_app_desc_t (verified against a built KilnCtrl.bin, 2026-10-02).
#ifndef RECOVERY_IMAGE_CHECK_H
#define RECOVERY_IMAGE_CHECK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RIC_EXPECTED_PROJECT "KilnCtrl"
#define RIC_CHIP_ID_ESP32S3 9u
#define RIC_IMAGE_MAGIC 0xE9u
#define RIC_APP_DESC_MAGIC 0xABCD5432u
#define RIC_APP_DESC_OFFSET 32u
#define RIC_APP_DESC_LEN 256u
// Smallest first chunk that can hold header + segment header + app desc.
#define RIC_MIN_FIRST_CHUNK (RIC_APP_DESC_OFFSET + RIC_APP_DESC_LEN)

typedef enum {
    RIC_OK = 0,
    RIC_NO_LENGTH,       // Content-Length absent/zero (chunked upload)
    RIC_OVERSIZE,        // larger than the target partition
    RIC_TRUNCATED,       // first chunk (or whole body) too short to hold the headers
    RIC_BAD_MAGIC,       // byte 0 is not 0xE9
    RIC_WRONG_CHIP,      // chip_id is not ESP32-S3
    RIC_BAD_SEGMENT,     // segment count/length implausible
    RIC_BAD_APP_DESC,    // esp_app_desc_t magic missing
    RIC_WRONG_PROJECT,   // project_name is not RIC_EXPECTED_PROJECT
} ric_result_t;

// Validates `buf[0..len)` (the first bytes of the body) against the declared
// `content_len` and the target `partition_size`. Pure; reads nothing past len.
ric_result_t ric_validate_first_chunk(const uint8_t *buf, size_t len, size_t content_len,
                                      size_t partition_size, const char *expected_project);

// Short human-readable reason, suitable for an HTTP error body.
const char *ric_message(ric_result_t r);

// HTTP status for a non-OK result: 413 for RIC_OVERSIZE, otherwise 400.
int ric_http_status(ric_result_t r);

// boot_guard's persisted record (firmware/KilnFW/App/drivers/persist/
// boot_guard.c boot_guard_record_t): 12 bytes = u8 version(1), 3 reserved,
// u32 boot_count, u32 crc32 over the first 8 bytes (zlib CRC-32). Returns 1
// and fills *count only when length, version and CRC all check out; 0
// otherwise (never fabricates a count).
int ric_boot_guard_decode(const uint8_t *blob, size_t len, uint32_t *count);

uint32_t ric_crc32(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif // RECOVERY_IMAGE_CHECK_H
