// kiln_crc32.h -- the one shared CRC-32 (reflected, poly 0xEDB88320, same
// convention as ESP-IDF's esp_crc32_le(): seed 0 for a fresh CRC, chainable).
// On the ESP target this is a thin wrapper over esp_crc32_le(); on the host
// (no ESP_PLATFORM) a portable bitwise implementation gives identical values.
// Used by NVS ring blobs (boot_history, trip_history) that checksum a struct
// with its own crc32 field treated as zero.
#ifndef KILN_CRC32_H
#define KILN_CRC32_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t kiln_crc32_le(uint32_t crc, const uint8_t *buf, size_t len);

/* CRC over buf[0..len) with bytes [skip_off, skip_off+skip_len) read as zero
 * (the struct's own crc32 field). Requires skip_off + skip_len <= len. */
uint32_t kiln_crc32_le_zeroed(const uint8_t *buf, size_t len, size_t skip_off, size_t skip_len);

#ifdef __cplusplus
}
#endif

#endif
