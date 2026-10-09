#include "benchproto/benchproto_frame.h"

/* Independent implementation of CRC16/CCITT-FALSE -- see benchproto_frame.h
 * for why this deliberately does not call kilnlink_crc16_ccitt_false()
 * even though the two compute the same variant. */
uint16_t benchproto_crc16_ccitt_false(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (int bit = 0; bit < 8; ++bit) {
            crc = (uint16_t)((crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                                              : (uint16_t)(crc << 1));
        }
    }
    return crc;
}
