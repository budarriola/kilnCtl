// See ota_image_crc.h's header comment for the parameterization this module
// exists to pin, and for why calling esp_rom_crc32_le() directly is easy to
// get wrong.
#include "ota_image_crc.h"

#include "esp_rom_crc.h" /* esp_rom_crc32_le() -- complements the seed on entry and the
                          * result on exit ITSELF; do not add either inversion here. */

uint32_t ota_image_crc32_update(uint32_t crc, const uint8_t *buf, size_t len)
{
    if (!buf || len == 0) {
        return crc;
    }
    /* Seed in, value out, nothing else. The running value IS the finished
     * CRC after the last chunk -- no final XOR. */
    return esp_rom_crc32_le(crc, buf, (uint32_t)len);
}

uint32_t ota_image_crc32(const uint8_t *buf, size_t len)
{
    return ota_image_crc32_update(OTA_IMAGE_CRC32_INIT, buf, len);
}
