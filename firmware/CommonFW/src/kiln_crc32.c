#include "kiln/kiln_crc32.h"

#ifdef ESP_PLATFORM
#include "esp_crc.h"

uint32_t kiln_crc32_le(uint32_t crc, const uint8_t *buf, size_t len)
{
    return esp_crc32_le(crc, buf, (uint32_t)len);
}
#else
uint32_t kiln_crc32_le(uint32_t crc, const uint8_t *buf, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1u) + 1u));
        }
    }
    return ~crc;
}
#endif

uint32_t kiln_crc32_le_zeroed(const uint8_t *buf, size_t len, size_t skip_off, size_t skip_len)
{
    static const uint8_t zeros[8] = {0};
    uint32_t crc = kiln_crc32_le(0, buf, skip_off);
    for (size_t i = 0; i < skip_len; i += sizeof(zeros)) {
        size_t n = skip_len - i < sizeof(zeros) ? skip_len - i : sizeof(zeros);
        crc = kiln_crc32_le(crc, zeros, n);
    }
    return kiln_crc32_le(crc, buf + skip_off + skip_len, len - skip_off - skip_len);
}
