// Host-test stub -- see stubs/esp_err.h for why these exist. Added
// 2026-08-27 for ota_http.c's host tests (test_ota_http.c). Only used by the
// Pico-image staging path (ota_pico_do_stage()), never reached by these
// tests -- a fixed, non-cryptographic stand-in is enough for the translation
// unit to link.
#ifndef TEST_STUB_ESP_ROM_CRC_H
#define TEST_STUB_ESP_ROM_CRC_H

#include <stddef.h>
#include <stdint.h>

static inline uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *buf, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return crc;
}

#endif // TEST_STUB_ESP_ROM_CRC_H
