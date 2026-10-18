// RX-11: structural verify of an ESP32-S3 app image sitting in the update stage.
//
// Why not esp_image_verify(): the staged image starts at stage + STAGE_IMAGE_OFFSET (4 KB), so its
// flash address (e.g. 0x611000) is not 64 KB aligned. IDF v6.0.2 esp_image_format.c
// verify_segment_header() refuses every flash-mapped (IROM/DROM) segment whose ABSOLUTE flash offset
// differs from its load address modulo the MMU page size (64 KB), so a good image fails there
// (ESP_ERR_IMAGE_INVALID -> not_an_esp32s3_app_image). The image is valid for where it will RUN
// (the 64 KB aligned app partition), just not where it is staged.
//
// This walker does what esp_image_verify does for a non-secure-boot app, against the image as it
// will run: header magic/chip id/segment count, each segment's length and its mapping alignment
// computed from the IMAGE-RELATIVE offset (app partitions are 64 KB aligned, so relative ==
// absolute mod 64 KB once copied), the XOR checksum byte, and the appended SHA-256 (required).
// Pure C over callbacks so the app (update_http.c) and recovery (recovery_apply_esp.c) share it
// and host tests drive it. Offsets passed to read() are relative to the image start.
#ifndef KILNCTL_STAGE_IMAGE_VERIFY_H
#define KILNCTL_STAGE_IMAGE_VERIFY_H

#include <stdint.h>

#define STAGE_IMG_MMU_PAGE 0x10000u
#define STAGE_IMG_CHIP_ID_ESP32S3 9u
#define STAGE_IMG_MAX_SEGMENTS 16u

typedef struct {
    void *ctx;
    int (*read)(void *ctx, uint32_t off, void *buf, uint32_t len); // 0 = ok
    int (*sha_begin)(void *ctx);                                    // 0 = ok
    int (*sha_update)(void *ctx, const void *data, uint32_t len);   // 0 = ok
    int (*sha_finish)(void *ctx, uint8_t out[32]);                  // 0 = ok; must release on any call order
    // RY-01: chip validity of the 24-byte header (IDF bootloader_common_check_chip_validity: chip id and
    // min/max chip revision vs this chip). 0 = ok. Required; runs before any hashing.
    int (*chip_ok)(void *ctx, const uint8_t hdr[24]);
} stage_image_io_t;

typedef enum {
    STAGE_IMG_OK = 0,
    STAGE_IMG_IO,
    STAGE_IMG_BAD_HEADER,
    STAGE_IMG_BAD_SEGMENT,
    STAGE_IMG_MISALIGNED,
    STAGE_IMG_TOO_SHORT,    // image ends before its own segments/padding/digest (truncated)
    STAGE_IMG_BAD_CHIP_REV, // RY-01: chip revision range excludes this chip
    STAGE_IMG_BAD_CHECKSUM,
    STAGE_IMG_NO_HASH,
    STAGE_IMG_BAD_HASH,
} stage_image_result_t;

// Verifies the image in [0, avail). On STAGE_IMG_OK *out_len (if non-NULL) is the full image length
// including checksum padding and digest (always <= avail).
stage_image_result_t stage_image_verify(const stage_image_io_t *io, uint32_t avail, uint32_t *out_len);

#endif
