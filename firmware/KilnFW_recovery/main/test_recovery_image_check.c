// test_recovery_image_check.c -- host test for recovery_image_check.c.
// Built and run by check_recovery_image_check.ps1 (MSVC). Prints
// "RESULT pass=N fail=N"; exit code is the fail count.
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <string.h>

#include "recovery_image_check.h"

static int g_pass, g_fail;

#define CHECK(cond, name)                                                   \
    do {                                                                    \
        if (cond) {                                                         \
            g_pass++;                                                       \
        } else {                                                            \
            g_fail++;                                                       \
            fprintf(stderr, "FAIL: %s (line %d)\n", name, __LINE__);        \
        }                                                                   \
    } while (0)

#define PART (0x1E0000u)
#define IMG_LEN 4096u

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

// Builds a valid first chunk of IMG_LEN bytes.
static void build_valid(uint8_t *b)
{
    memset(b, 0, IMG_LEN);
    b[0] = 0xE9;
    b[1] = 5; // segment count
    b[12] = 9; // chip id ESP32-S3 (u16 LE)
    put_u32(b + 24, 0x3C000000u); // load addr
    put_u32(b + 28, 0x1000u);     // seg 0 data_len
    put_u32(b + 32, 0xABCD5432u); // app desc magic
    strcpy((char *)b + 32 + 16, "v1.2.3");
    strcpy((char *)b + 32 + 48, "KilnCtrl");
}

static ric_result_t run(const uint8_t *b, size_t len, size_t content, size_t part)
{
    return ric_validate_first_chunk(b, len, content, part, RIC_EXPECTED_PROJECT);
}

int main(void)
{
    static uint8_t img[IMG_LEN];

    build_valid(img);
    CHECK(run(img, IMG_LEN, 0x100000, PART) == RIC_OK, "valid image accepted");
    CHECK(run(img, IMG_LEN, PART, PART) == RIC_OK, "image exactly partition size accepted");
    CHECK(run(img, RIC_MIN_FIRST_CHUNK, 0x100000, PART) == RIC_OK, "minimal first chunk accepted");

    // Content length gates.
    CHECK(run(img, IMG_LEN, 0, PART) == RIC_NO_LENGTH, "zero content length rejected");
    CHECK(run(img, IMG_LEN, PART + 1, PART) == RIC_OVERSIZE, "oversize rejected");
    CHECK(run(NULL, 0, PART + 1, PART) == RIC_OVERSIZE, "oversize rejected with no data read");
    CHECK(run(NULL, 0, 0, PART) == RIC_NO_LENGTH, "no length rejected with no data read");
    CHECK(ric_http_status(RIC_OVERSIZE) == 413, "oversize maps to 413");
    CHECK(ric_http_status(RIC_BAD_MAGIC) == 400, "bad magic maps to 400");
    CHECK(ric_http_status(RIC_NO_LENGTH) == 400, "no length maps to 400");

    // Truncation.
    CHECK(run(img, RIC_MIN_FIRST_CHUNK - 1, 0x100000, PART) == RIC_TRUNCATED, "short chunk rejected");
    CHECK(run(img, IMG_LEN, 100, PART) == RIC_TRUNCATED, "tiny content length rejected");
    CHECK(run(img, IMG_LEN, IMG_LEN - 1, PART) == RIC_TRUNCATED, "chunk longer than body rejected");

    // Bad magic.
    build_valid(img);
    img[0] = 0xE8;
    CHECK(run(img, IMG_LEN, 0x100000, PART) == RIC_BAD_MAGIC, "bad magic rejected");

    // Wrong chip (ESP32 = 0, ESP32-S2 = 2, ESP32-C3 = 5).
    build_valid(img);
    img[12] = 5;
    CHECK(run(img, IMG_LEN, 0x100000, PART) == RIC_WRONG_CHIP, "wrong chip rejected");
    build_valid(img);
    img[13] = 1; // high byte of chip id
    CHECK(run(img, IMG_LEN, 0x100000, PART) == RIC_WRONG_CHIP, "chip id high byte checked");

    // Segment sanity.
    build_valid(img);
    img[1] = 0;
    CHECK(run(img, IMG_LEN, 0x100000, PART) == RIC_BAD_SEGMENT, "zero segments rejected");
    build_valid(img);
    img[1] = 17;
    CHECK(run(img, IMG_LEN, 0x100000, PART) == RIC_BAD_SEGMENT, "too many segments rejected");
    build_valid(img);
    put_u32(img + 28, 0x10u);
    CHECK(run(img, IMG_LEN, 0x100000, PART) == RIC_BAD_SEGMENT, "segment too small for app desc rejected");
    build_valid(img);
    put_u32(img + 28, 0x200000u);
    CHECK(run(img, IMG_LEN, 0x100000, PART) == RIC_BAD_SEGMENT, "segment longer than body rejected");

    // App desc.
    build_valid(img);
    put_u32(img + 32, 0xDEADBEEFu);
    CHECK(run(img, IMG_LEN, 0x100000, PART) == RIC_BAD_APP_DESC, "bad app desc magic rejected");

    // Project name.
    build_valid(img);
    strcpy((char *)img + 32 + 48, "OtherApp");
    CHECK(run(img, IMG_LEN, 0x100000, PART) == RIC_WRONG_PROJECT, "wrong project rejected");
    build_valid(img);
    strcpy((char *)img + 32 + 48, "KilnCtrl2");
    CHECK(run(img, IMG_LEN, 0x100000, PART) == RIC_WRONG_PROJECT, "project prefix match rejected");
    build_valid(img);
    strcpy((char *)img + 32 + 48, "KilnCtr");
    CHECK(run(img, IMG_LEN, 0x100000, PART) == RIC_WRONG_PROJECT, "project name too short rejected");

    // Every failure has a message.
    for (int r = RIC_OK; r <= RIC_WRONG_PROJECT; r++) {
        const char *m = ric_message((ric_result_t)r);
        CHECK(m && m[0], "message present");
    }

    // CRC-32 (zlib) known vector.
    CHECK(ric_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u, "crc32 check value");

    // boot_guard record: version 1, 3 reserved, count=5 LE, crc32 over first 8
    // bytes = 0x9e562fc5 (computed with python zlib.crc32).
    {
        uint8_t rec[12] = {1, 0, 0, 0, 5, 0, 0, 0, 0xC5, 0x2F, 0x56, 0x9E};
        uint32_t count = 0;
        CHECK(ric_boot_guard_decode(rec, 12, &count) == 1 && count == 5, "boot_guard decode ok");
        rec[4] = 6;
        CHECK(ric_boot_guard_decode(rec, 12, &count) == 0, "boot_guard bad crc rejected");
        rec[4] = 5;
        rec[0] = 2; // valid CRC for version 2, so only the version check can reject
        rec[8] = 0x26; rec[9] = 0x28; rec[10] = 0xD9; rec[11] = 0x10;
        CHECK(ric_boot_guard_decode(rec, 12, &count) == 0, "boot_guard wrong version rejected");
        rec[0] = 1;
        rec[8] = 0xC5; rec[9] = 0x2F; rec[10] = 0x56; rec[11] = 0x9E;
        CHECK(ric_boot_guard_decode(rec, 11, &count) == 0, "boot_guard short rejected");
        CHECK(ric_boot_guard_decode(rec, 13, &count) == 0, "boot_guard long rejected");
    }

    printf("RESULT pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail;
}
