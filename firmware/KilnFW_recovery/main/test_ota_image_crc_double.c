// Host-test double of ota_image_crc32() for test_recovery_apply.c: the shared
// stage_header.c calls it; on target it is the esp_rom wrapper. This is a one-line
// delegation to ric_crc32(), not a CRC implementation. Kept in its own file so the
// link-isolation check allowlists only this file, not the whole test.
#include <stddef.h>
#include <stdint.h>

#include "recovery_image_check.h"

uint32_t ota_image_crc32(const uint8_t *buf, size_t len)
{
    return ric_crc32(buf, len);
}
