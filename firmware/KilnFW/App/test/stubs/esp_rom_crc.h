// Host-test stub -- see stubs/esp_err.h for why these exist. Added
// 2026-08-27 for ota_http.c's host tests (test_ota_http.c).
//
// This MUST model the real ROM routine's contract, not merely link. As of
// 2026-09-18 it is exercised numerically: ota_image_crc.c (the ESP's staged
// Pico-image CRC32, App/drivers/http/) is compiled into the host suite and
// test_ota_image_crc.c asserts the standard CRC-32 check value 0xCBF43926
// over "123456789" through it. That assertion only means anything if the
// stub underneath behaves the way esp_rom_crc32_le() actually behaves.
//
// The contract: esp_rom_crc32_le() is a reflected CRC-32 over polynomial
// 0xEDB88320 that COMPLEMENTS THE SEED ON ENTRY AND THE RESULT ON EXIT --
// f(c, d) = ~R(~c, d), where R is the bare bit loop with no inversion at
// either end. That is what esp_rom_crc.h's own "add ~ at the beginning and
// the end" note is describing: work the function does FOR the caller, not
// work the caller is supposed to repeat. A caller therefore reaches
// standard CRC-32/zlib by seeding 0, chaining the previous return value
// straight back in, and applying no final XOR.
//
// This stub previously implemented the BARE loop (no inversions), which is
// a different function from the one it stands in for. Nothing compared its
// output against anything, so the difference was invisible -- and the one
// caller in the tree had the matching mistake on its own side (seeding
// 0xFFFFFFFF and XORing the result), which made the Pico OTA CRC unusable
// on real hardware while every host test stayed green. See
// docs/audits/pico_ota_staged_crc_mismatch_2026-09-18.md. Do not "simplify"
// the two complements below back out.
#ifndef TEST_STUB_ESP_ROM_CRC_H
#define TEST_STUB_ESP_ROM_CRC_H

#include <stddef.h>
#include <stdint.h>

static inline uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *buf, uint32_t len)
{
    crc = ~crc;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

#endif // TEST_STUB_ESP_ROM_CRC_H
