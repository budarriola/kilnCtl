// Host test for the little-endian read/put helpers in
// drivers/safety/safety_link_frame.c (HOST_TEST_COVERAGE_GAPS round 2, R2-H).
// safety_link_frame.c is #include'd directly. Exact wire bytes are asserted so
// a swapped byte order or a dropped mask is caught.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test_common.h"

int g_test_failures = 0;
int g_test_count = 0;
#define CHECK(c) TEST_CHECK((c), #c)

#include "../drivers/safety/safety_link_frame.c"

static void test_u16(void)
{
    const uint8_t a[2] = {0x34, 0x12};
    CHECK(safety_read_u16_le(a) == 0x1234u);
    const uint8_t b[2] = {0xFF, 0xFF};
    CHECK(safety_read_u16_le(b) == 0xFFFFu);
    const uint8_t c[2] = {0x00, 0x80};
    CHECK(safety_read_u16_le(c) == 0x8000u);
    uint8_t out[3] = {0xAA, 0xAA, 0xAA};
    safety_put_u16_le(out, 0xABCDu);
    CHECK(out[0] == 0xCD && out[1] == 0xAB);
    CHECK(out[2] == 0xAA); /* no overrun */
    safety_put_u16_le(out, 0x0100u);
    CHECK(out[0] == 0x00 && out[1] == 0x01);
}

static void test_u32(void)
{
    const uint8_t a[4] = {0x78, 0x56, 0x34, 0x12};
    CHECK(safety_read_u32_le(a) == 0x12345678u);
    const uint8_t b[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    CHECK(safety_read_u32_le(b) == 0xFFFFFFFFu);
    const uint8_t c[4] = {0x00, 0x00, 0x00, 0x80};
    CHECK(safety_read_u32_le(c) == 0x80000000u); /* high byte not sign-extended */
    uint8_t out[5] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
    safety_put_u32_le(out, 0xDEADBEEFu);
    CHECK(out[0] == 0xEF && out[1] == 0xBE && out[2] == 0xAD && out[3] == 0xDE);
    CHECK(out[4] == 0xAA);
    safety_put_u32_le(out, 0u);
    CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 0);
}

static void test_f32(void)
{
    uint8_t out[5] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
    safety_put_f32_le(out, 1.0f); /* IEEE-754 0x3F800000 */
    CHECK(out[0] == 0x00 && out[1] == 0x00 && out[2] == 0x80 && out[3] == 0x3F);
    CHECK(out[4] == 0xAA);
    CHECK(safety_read_f32_le(out) == 1.0f);
    safety_put_f32_le(out, -2.5f); /* 0xC0200000 */
    CHECK(out[0] == 0x00 && out[1] == 0x00 && out[2] == 0x20 && out[3] == 0xC0);
    CHECK(safety_read_f32_le(out) == -2.5f);
    safety_put_f32_le(out, 1234.5f); /* 0x449A5000 */
    CHECK(out[0] == 0x00 && out[1] == 0x50 && out[2] == 0x9A && out[3] == 0x44);
    CHECK(safety_read_f32_le(out) == 1234.5f);
    const uint8_t nan_bytes[4] = {0x00, 0x00, 0xC0, 0x7F};
    CHECK(isnan(safety_read_f32_le(nan_bytes)));
}

static void test_roundtrip_unaligned(void)
{
    uint8_t buf[16];
    memset(buf, 0, sizeof(buf));
    safety_put_u32_le(buf + 1, 0xCAFEBABEu);
    safety_put_u16_le(buf + 5, 0x7788u);
    safety_put_f32_le(buf + 7, 98.25f);
    CHECK(safety_read_u32_le(buf + 1) == 0xCAFEBABEu);
    CHECK(safety_read_u16_le(buf + 5) == 0x7788u);
    CHECK(safety_read_f32_le(buf + 7) == 98.25f);
    CHECK(buf[0] == 0 && buf[11] == 0);
}

int main(void)
{
    test_u16();
    test_u32();
    test_f32();
    test_roundtrip_unaligned();
    printf("safety_link_endian: %d checks, %d failures\n", g_test_count, g_test_failures);
    return g_test_failures ? 1 : 0;
}
