// Host-test stub -- see esp_err.h's own header comment for why these exist.
// Added 2026-08-22 for crash_report.c's host tests (test_crash_report.c),
// the first module in this codebase to use esp_crc32_le().
//
// This is a REAL CRC-32 implementation (standard reflected poly 0xEDB88320,
// same polynomial ESP-IDF's esp_rom_crc32_le() uses), not a fake -- the
// tests that matter here (CRC round-trip, corruption detection) need actual
// integrity-checking behavior, not a stub that always returns a fixed value.
// It does not need to numerically match esp_rom_crc32_le()'s bit-for-bit
// output (nothing compares a host-computed CRC against an on-target one),
// only to be a real, deterministic, corruption-sensitive CRC32 -- which this
// is.
#ifndef TEST_STUB_ESP_CRC_H
#define TEST_STUB_ESP_CRC_H

#include <stdint.h>

static inline uint32_t esp_crc32_le(uint32_t crc, uint8_t const *buf, uint32_t len)
{
    crc = ~crc;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1u) + 1u));
        }
    }
    return ~crc;
}

#endif // TEST_STUB_ESP_CRC_H
