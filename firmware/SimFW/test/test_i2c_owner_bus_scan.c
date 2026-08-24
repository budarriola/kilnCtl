// Host tests pinning i2c_owner.c's perform_bus_scan() bit-index/bitmap
// encoding logic, and -- the whole reason this pass exists -- proving that a
// genuine 1-byte I2C probe write behaves differently from a 0-byte one.
//
// THE CONFIRMED BUG this command exists to have never shipped: pico-sdk's
// i2c_write_blocking_internal() guards its `len` parameter with
// `invalid_params_if(I2C, len == 0)`, a hard_assert()-family macro that
// compiles to *nothing* in a release (NDEBUG) build. With len == 0 the
// transfer loop inside that function never executes a single iteration, so
// the address phase is never actually driven onto the bus -- and the
// function still returns 0 (its normal "success" value) unconditionally.
// A bus scan written with a 0-byte probe would therefore report EVERY
// address in the swept range as "present," regardless of what is actually
// attached, and would still compile and run without complaint in a release
// build -- exactly the kind of bug a debugger only catches by noticing the
// real hardware doesn't match. This is precisely the shape of incident that
// motivated this whole command: two MCP23017s silently at the wrong
// addresses, discoverable only via an hour-long SWD session, because
// nothing on the PC side could ask the firmware "what's actually on your
// bus."
//
// Why this file mirrors rather than calls the real code: i2c_owner.c
// includes pico-sdk (hardware/i2c.h) and FreeRTOS headers and drives real
// I2C0 -- it is not part of this host-test harness's source list
// (build_host_tests.ps1 compiles only src/sim/'s pure modules), the same
// pure/task boundary test_i2c_owner_io_read.c's and test_i2c_owner_io_set.c's
// own header comments already document. mock_i2c_write_blocking() below
// stands in for the real pico-sdk call, deliberately modeling BOTH its
// correct len==1 behavior (ACK only at the bench's real addresses, 0x25/
// 0x26) and its buggy-if-misused len==0 behavior (always reports success) --
// mirror_perform_bus_scan() is a byte-for-byte copy of i2c_owner.c's
// perform_bus_scan() bit-index math, parameterized on the probe length so a
// test can exercise both paths explicitly. Keep the two in sync by hand if
// either changes.
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "test_common.h"

// --- mirror of i2c_owner.h's I2C_OWNER_BUS_SCAN_ADDR_MIN/MAX/COUNT/BITMAP_BYTES ---
#define MIRROR_ADDR_MIN 0x08u
#define MIRROR_ADDR_MAX 0x77u
#define MIRROR_ADDR_COUNT (MIRROR_ADDR_MAX - MIRROR_ADDR_MIN + 1u) // 112
#define MIRROR_BITMAP_BYTES ((MIRROR_ADDR_COUNT + 7u) / 8u)        // 14

typedef struct {
    uint8_t found_bitmap[MIRROR_BITMAP_BYTES];
    uint8_t configured_addr1;
    uint8_t configured_addr2;
} mirror_bus_scan_result_t;

// Mirrors real hardware on THIS bench (i2c_owner.c's MCP23017_ADDR_1/_2
// comment: "confirmed 2026-08-24 ... only 0x25 and 0x26 ACKed") when called
// with a genuine len==1 probe -- and mirrors pico-sdk's documented,
// confirmed release-build no-op when called with len==0 (see this file's
// header comment). Return convention matches i2c_write_blocking(): >= 0 on
// ACK/success, < 0 on NAK/error.
static int mock_i2c_write_blocking(uint8_t addr, uint8_t len)
{
    if (len == 0u) {
        // THE CONFIRMED BUG, reproduced deliberately: pico-sdk's
        // invalid_params_if(I2C, len == 0) compiles to nothing in a release
        // build, the address phase is never driven, and the call returns 0
        // (success) no matter what. Every address looks "present."
        return 0;
    }
    return (addr == 0x25u || addr == 0x26u) ? 0 : -1;
}

// Mirror of i2c_owner.c's perform_bus_scan(): identical bit-index math
// (bit_index = addr - MIRROR_ADDR_MIN; byte = bit_index/8, bit = bit_index%8)
// and configured-address passthrough, parameterized on `probe_len` (the real
// firmware always passes 1 -- see i2c_owner.c's own CRITICAL comment on that
// call site) so tests can exercise the len==0 regression explicitly.
static void mirror_perform_bus_scan(mirror_bus_scan_result_t *result, uint8_t configured_addr1,
                                     uint8_t configured_addr2, uint8_t probe_len)
{
    memset(result, 0, sizeof(*result));
    result->configured_addr1 = configured_addr1;
    result->configured_addr2 = configured_addr2;

    for (uint16_t addr = MIRROR_ADDR_MIN; addr <= MIRROR_ADDR_MAX; addr++) {
        int ret = mock_i2c_write_blocking((uint8_t)addr, probe_len);
        if (ret >= 0) {
            uint8_t bit_index = (uint8_t)(addr - MIRROR_ADDR_MIN);
            result->found_bitmap[bit_index / 8u] |= (uint8_t)(1u << (bit_index % 8u));
        }
    }
}

static bool bitmap_bit_set(const uint8_t *bitmap, uint8_t addr)
{
    uint8_t bit_index = (uint8_t)(addr - MIRROR_ADDR_MIN);
    return (bitmap[bit_index / 8u] & (uint8_t)(1u << (bit_index % 8u))) != 0;
}

static uint32_t count_set_bits(const uint8_t *bitmap, size_t n_bytes)
{
    uint32_t count = 0;
    for (size_t i = 0; i < n_bytes; i++) {
        uint8_t b = bitmap[i];
        while (b) {
            count += (uint32_t)(b & 1u);
            b = (uint8_t)(b >> 1);
        }
    }
    return count;
}

static void test_real_probe_finds_only_bench_addresses(void)
{
    TEST_SECTION("perform_bus_scan (len=1, the real code path) -- finds exactly 0x25/0x26");

    mirror_bus_scan_result_t result;
    mirror_perform_bus_scan(&result, 0x25u, 0x26u, /*probe_len=*/1u);

    TEST_CHECK(result.configured_addr1 == 0x25u, "configured_addr1 passes through unchanged");
    TEST_CHECK(result.configured_addr2 == 0x26u, "configured_addr2 passes through unchanged");
    TEST_CHECK(bitmap_bit_set(result.found_bitmap, 0x25u), "0x25 (a real bench address) is reported found");
    TEST_CHECK(bitmap_bit_set(result.found_bitmap, 0x26u), "0x26 (a real bench address) is reported found");
    TEST_CHECK(!bitmap_bit_set(result.found_bitmap, 0x20u), "0x20 (the POR default, NOT on this bench) is NOT found");
    TEST_CHECK(!bitmap_bit_set(result.found_bitmap, 0x08u), "0x08 (range start, nothing attached) is NOT found");
    TEST_CHECK(!bitmap_bit_set(result.found_bitmap, 0x77u), "0x77 (range end, nothing attached) is NOT found");
    TEST_CHECK(count_set_bits(result.found_bitmap, MIRROR_BITMAP_BYTES) == 2u,
               "exactly 2 addresses found with a genuine len=1 probe -- not all 112");
}

static void test_zero_length_probe_reproduces_the_confirmed_bug(void)
{
    TEST_SECTION("perform_bus_scan (len=0, THE CONFIRMED BUG) -- every address falsely reports present");

    mirror_bus_scan_result_t result;
    mirror_perform_bus_scan(&result, 0x25u, 0x26u, /*probe_len=*/0u);

    // pico-sdk's invalid_params_if(I2C, len == 0) is a release-build no-op:
    // the address phase is never driven, i2c_write_blocking() returns 0
    // unconditionally, so a scan using this call shape would report every
    // single address in range as ACKing -- indistinguishable from "every
    // possible I2C device is attached," which is never true and would have
    // made this command actively misleading rather than merely unimplemented.
    TEST_CHECK(count_set_bits(result.found_bitmap, MIRROR_BITMAP_BYTES) == MIRROR_ADDR_COUNT,
               "a len=0 probe falsely reports all 112 addresses present -- this is exactly why "
               "i2c_owner.c's real perform_bus_scan() hardcodes a 1-byte write, never 0");
    TEST_CHECK(bitmap_bit_set(result.found_bitmap, 0x20u),
               "with the bug, even the POR default (never actually attached) falsely reports found");
}

static void test_bit_index_boundary_packing(void)
{
    TEST_SECTION("perform_bus_scan -- bit-index/byte-packing math at both range boundaries");

    // A scan where every possible address ACKs (probe_len=0 conveniently
    // gives us this without needing 112 individual mock cases) lets this
    // test focus purely on where each address's bit lands, independent of
    // test_real_probe_finds_only_bench_addresses()'s ACK/NAK content check.
    mirror_bus_scan_result_t result;
    mirror_perform_bus_scan(&result, 0u, 0u, /*probe_len=*/0u);

    // MIRROR_ADDR_MIN (0x08) is bit index 0 -> byte 0, bit 0 -> 0x01.
    TEST_CHECK((result.found_bitmap[0] & 0x01u) != 0u, "addr 0x08 (range min) sets byte0 bit0");
    // MIRROR_ADDR_MAX (0x77) is bit index 111 -> byte 111/8=13, bit 111%8=7 -> 0x80.
    TEST_CHECK((result.found_bitmap[13] & 0x80u) != 0u, "addr 0x77 (range max) sets byte13 bit7 (last bit, last byte)");
    // 112 addresses / 8 bits per byte = 14 bytes exactly -- no partial byte,
    // no bit past byte13/bit7 should ever be touched by this loop.
    TEST_CHECK(sizeof(result.found_bitmap) == 14u, "found_bitmap is exactly 14 bytes for 112 addresses, no padding");
}

static void test_configured_addresses_independent_of_found_bitmap(void)
{
    TEST_SECTION("perform_bus_scan -- configured_addr1/2 are a pure passthrough, unrelated to what was found");

    // The exact bug-incident shape: firmware configured for the POR default
    // (0x20/0x21) while the real bus only answers at 0x25/0x26.
    mirror_bus_scan_result_t result;
    mirror_perform_bus_scan(&result, 0x20u, 0x21u, /*probe_len=*/1u);

    TEST_CHECK(result.configured_addr1 == 0x20u, "configured_addr1 reflects what the caller passed in, not what was found");
    TEST_CHECK(result.configured_addr2 == 0x21u, "configured_addr2 reflects what the caller passed in, not what was found");
    TEST_CHECK(!bitmap_bit_set(result.found_bitmap, 0x20u), "0x20 was NOT found (mismatch case)");
    TEST_CHECK(!bitmap_bit_set(result.found_bitmap, 0x21u), "0x21 was NOT found (mismatch case)");
    TEST_CHECK(bitmap_bit_set(result.found_bitmap, 0x25u), "0x25 WAS found even though it isn't 'configured' here");
    TEST_CHECK(bitmap_bit_set(result.found_bitmap, 0x26u), "0x26 WAS found even though it isn't 'configured' here");
}

void run_test_i2c_owner_bus_scan(void)
{
    test_real_probe_finds_only_bench_addresses();
    test_zero_length_probe_reproduces_the_confirmed_bug();
    test_bit_index_boundary_packing();
    test_configured_addresses_independent_of_found_bitmap();
}
