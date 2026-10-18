// RX-11: host tests for App/drivers/update/stage_image_verify.c (the staged-image verify that
// replaced esp_image_verify over the 4 KB-offset stage). Synthetic ESP32-S3 images; the hash is a
// host stand-in (the walker is hash-agnostic), and the test checks exactly which bytes it covers.
#include <string.h>

#include "test_common.h"

#include "../drivers/update/stage_image_verify.h"

#define IMG_CAP 1024u

typedef struct {
    uint8_t img[IMG_CAP];
    uint32_t hashed;  // bytes fed to the hash
    uint8_t acc[32];  // stand-in digest state
    int begins, finishes;
    int fail_read_at; // -1 = never
    int chip_calls;
} fx_t;

static void h_mix(uint8_t acc[32], uint32_t pos, uint8_t b)
{
    acc[pos % 32] = (uint8_t)((acc[pos % 32] * 31u) ^ b ^ (uint8_t)(pos >> 5));
}
static int fx_read(void *c, uint32_t off, void *buf, uint32_t len)
{
    fx_t *f = c;
    if (off + len > IMG_CAP || (f->fail_read_at >= 0 && off <= (uint32_t)f->fail_read_at && (uint32_t)f->fail_read_at < off + len)) {
        return -1;
    }
    memcpy(buf, f->img + off, len);
    return 0;
}
static int fx_begin(void *c)
{
    fx_t *f = c;
    f->begins++;
    f->hashed = 0;
    memset(f->acc, 0x5A, 32);
    return 0;
}
static int fx_update(void *c, const void *d, uint32_t n)
{
    fx_t *f = c;
    for (uint32_t i = 0; i < n; i++) {
        h_mix(f->acc, f->hashed++, ((const uint8_t *)d)[i]);
    }
    return 0;
}
static int fx_finish(void *c, uint8_t out[32])
{
    fx_t *f = c;
    f->finishes++;
    memcpy(out, f->acc, 32);
    return 0;
}
static int fx_chip(void *c, const uint8_t h[24])
{
    fx_t *f = c;
    f->chip_calls++;
    // Mirrors bootloader_common_check_chip_validity: this chip is full rev 1; header
    // min_chip_rev_full at 15..16, max_chip_rev_full at 17..18 (0xFFFF = no limit).
    uint32_t mn = (uint32_t)h[15] | ((uint32_t)h[16] << 8);
    uint32_t mx = (uint32_t)h[17] | ((uint32_t)h[18] << 8);
    return (1u >= mn && 1u <= mx) ? 0 : -1;
}
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

// Header(24) | seg0 DROM 64 B @0x20 | seg1 DRAM 100 B | seg2 IROM 40 B @0xD4 | pad+checksum | SHA.
// Returns the full image length. irom_load lets a test misalign segment 2.
static uint32_t build(fx_t *f, uint32_t irom_load)
{
    memset(f, 0, sizeof(*f));
    f->fail_read_at = -1;
    uint8_t *p = f->img;
    memset(p, 0xFF, IMG_CAP);
    memset(p, 0, 24);
    p[0] = 0xE9;
    p[1] = 3;
    put32(p + 4, 0x40375000u);
    p[12] = 9; // ESP32-S3
    p[17] = 0xFF; // max_chip_rev_full = 0xFFFF (no limit)
    p[18] = 0xFF;
    p[23] = 1; // hash appended
    uint32_t off = 24;
    const uint32_t loads[3] = {0x3C000020u, 0x3FC88000u, irom_load};
    const uint32_t lens[3] = {64, 100, 40};
    uint8_t x = 0xEF;
    for (int s = 0; s < 3; s++) {
        put32(p + off, loads[s]);
        put32(p + off + 4, lens[s]);
        off += 8;
        for (uint32_t i = 0; i < lens[s]; i++) {
            p[off + i] = (uint8_t)(i * 13u + (uint32_t)s * 7u + 1u);
            x ^= p[off + i];
        }
        off += lens[s];
    }
    uint32_t end = (off + 1u + 15u) & ~15u;
    memset(p + off, 0, end - off);
    p[end - 1] = x;
    uint8_t acc[32];
    memset(acc, 0x5A, 32);
    for (uint32_t i = 0; i < end; i++) {
        h_mix(acc, i, p[i]);
    }
    memcpy(p + end, acc, 32);
    return end + 32;
}

static stage_image_result_t run(fx_t *f, uint32_t avail, uint32_t *len)
{
    const stage_image_io_t io = {.ctx = f, .read = fx_read, .sha_begin = fx_begin, .sha_update = fx_update, .sha_finish = fx_finish, .chip_ok = fx_chip};
    return stage_image_verify(&io, avail, len);
}

static void test_good(void)
{
    TEST_SECTION("stage_image_verify -- good image passes, hash covers [0, digest)");
    fx_t f;
    uint32_t n = build(&f, 0x420000D4u);
    uint32_t len = 0;
    TEST_CHECK(run(&f, IMG_CAP, &len) == STAGE_IMG_OK, "good image verifies with room to spare");
    TEST_CHECK(len == n, "out_len is the full image length incl. digest");
    TEST_CHECK(f.hashed == n - 32, "hash covers header, segments and padding, not the digest");
    TEST_CHECK(f.begins == 1 && f.finishes == 1, "hash begun and finished once");
    TEST_CHECK(run(&f, n, &len) == STAGE_IMG_OK, "good image verifies with avail == exact length");
}

static void test_corrupt(void)
{
    TEST_SECTION("stage_image_verify -- corrupt images fail");
    fx_t f;
    uint32_t n = build(&f, 0x420000D4u);
    f.img[40] ^= 0x01;
    TEST_CHECK(run(&f, IMG_CAP, NULL) == STAGE_IMG_BAD_CHECKSUM, "flipped segment byte -> bad checksum");
    TEST_CHECK(f.begins == f.finishes, "hash released on failure");

    build(&f, 0x420000D4u);
    f.img[40] ^= 0x01;
    f.img[41] ^= 0x01; // checksum-neutral pair: only the SHA catches it
    TEST_CHECK(run(&f, IMG_CAP, NULL) == STAGE_IMG_BAD_HASH, "checksum-neutral corruption -> bad hash");
    TEST_CHECK(f.begins == 1 && f.finishes == 1, "hash finished once on bad-hash path");

    build(&f, 0x420000D4u);
    f.img[n - 1] ^= 0x80;
    TEST_CHECK(run(&f, IMG_CAP, NULL) == STAGE_IMG_BAD_HASH, "corrupt digest -> bad hash");

    build(&f, 0x420000D4u);
    f.img[5] ^= 0x10; // entry address: header bytes are hashed too
    TEST_CHECK(run(&f, IMG_CAP, NULL) == STAGE_IMG_BAD_HASH, "corrupt header field -> bad hash");

    build(&f, 0x420000D4u);
    TEST_CHECK(run(&f, n - 1, NULL) == STAGE_IMG_TOO_SHORT, "truncated by one byte -> too short");
    TEST_CHECK(f.begins == f.finishes, "hash released on truncation");
    TEST_CHECK(run(&f, 100, NULL) == STAGE_IMG_TOO_SHORT, "truncated mid-segment -> too short");

    build(&f, 0x420000D4u);
    f.img[0] = 0xEA;
    TEST_CHECK(run(&f, IMG_CAP, NULL) == STAGE_IMG_BAD_HEADER, "bad magic");
    build(&f, 0x420000D4u);
    f.img[12] = 2;
    TEST_CHECK(run(&f, IMG_CAP, NULL) == STAGE_IMG_BAD_HEADER, "wrong chip id");
    build(&f, 0x420000D4u);
    f.img[1] = 17;
    TEST_CHECK(run(&f, IMG_CAP, NULL) == STAGE_IMG_BAD_HEADER, "too many segments");
    build(&f, 0x420000D4u);
    f.img[23] = 0;
    TEST_CHECK(run(&f, IMG_CAP, NULL) == STAGE_IMG_NO_HASH, "no appended hash");
    build(&f, 0x420000D4u);
    f.img[24 + 4] = 63; // seg0 length not a multiple of 4
    TEST_CHECK(run(&f, IMG_CAP, NULL) == STAGE_IMG_BAD_SEGMENT, "segment length not word aligned");

    build(&f, 0x420000D4u);
    f.fail_read_at = 200; // inside segment 1 data
    TEST_CHECK(run(&f, IMG_CAP, NULL) == STAGE_IMG_IO, "flash read error -> io");
    TEST_CHECK(f.begins == f.finishes, "hash released on io error");
}

static void test_chip_rev_and_padding(void)
{
    TEST_SECTION("stage_image_verify -- chip revision (RY-01) and truncation in padding/digest (RY-03)");
    fx_t f;
    uint32_t n = build(&f, 0x420000D4u);
    TEST_CHECK(run(&f, IMG_CAP, NULL) == STAGE_IMG_OK && f.chip_calls == 1, "in-range chip rev passes, callback ran once");
    build(&f, 0x420000D4u);
    f.img[15] = 2; // min_chip_rev_full 2 > chip rev 1
    TEST_CHECK(run(&f, IMG_CAP, NULL) == STAGE_IMG_BAD_CHIP_REV, "min chip rev above this chip refused");
    TEST_CHECK(f.hashed == 0 && f.begins == f.finishes, "chip refusal hashes no body, hash released");
    build(&f, 0x420000D4u);
    f.img[17] = 0;
    f.img[18] = 0; // max_chip_rev_full 0 < chip rev 1
    TEST_CHECK(run(&f, IMG_CAP, NULL) == STAGE_IMG_BAD_CHIP_REV, "max chip rev below this chip refused");
    // Segments end at 24+3*8+204 = 252; padding 252..255 (checksum at 255), digest 256..288.
    build(&f, 0x420000D4u);
    TEST_CHECK(n == 288, "fixture layout as assumed");
    TEST_CHECK(run(&f, 254, NULL) == STAGE_IMG_TOO_SHORT, "truncated inside checksum padding -> too short");
    TEST_CHECK(run(&f, 256, NULL) == STAGE_IMG_TOO_SHORT, "truncated before digest -> too short");
    TEST_CHECK(run(&f, 270, NULL) == STAGE_IMG_TOO_SHORT, "truncated inside digest -> too short");
    TEST_CHECK(f.begins == f.finishes, "hash released on every truncation");
}

static void test_alignment(void)
{
    TEST_SECTION("stage_image_verify -- mapped segment alignment is image-relative (RX-11)");
    fx_t f;
    // The good image's IROM segment sits at image offset 0xD4 and loads at 0x...00D4: it verifies
    // although the stage puts it at an absolute flash offset 0x611000 + 0xD4 (esp_image_verify's
    // absolute check refused exactly this).
    build(&f, 0x420000D4u);
    TEST_CHECK(run(&f, IMG_CAP, NULL) == STAGE_IMG_OK, "relative-aligned IROM segment passes");
    build(&f, 0x420010D4u); // relative offset 0xD4 vs load % 64K 0x10D4
    TEST_CHECK(run(&f, IMG_CAP, NULL) == STAGE_IMG_MISALIGNED, "IROM segment misaligned for the app partition");
    build(&f, 0x420000D8u);
    TEST_CHECK(run(&f, IMG_CAP, NULL) == STAGE_IMG_MISALIGNED, "IROM segment off by 4");
}

void run_test_stage_image_verify(void)
{
    test_good();
    test_corrupt();
    test_alignment();
    test_chip_rev_and_padding();
}
