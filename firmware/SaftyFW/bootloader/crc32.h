// crc32.h -- standard CRC-32 (IEEE 802.3 / zlib), poly 0xEDB88320 reflected,
// init 0xFFFFFFFF, final XOR 0xFFFFFFFF. Distinct from kilnlink's
// CRC16/CCITT-FALSE (CommonFW/docs/LINK_PROTOCOL.md section 3, used for the
// wire) -- this one is for the flash metadata record and, later, whole-slot
// image verification (docs/BOOTLOADER.md's "crc32" fields), which is a
// separate concern from anything that crosses the isolated link.
//
// Pure, no RTOS/SDK dependency -- host-testable, same discipline as
// src/tasks/link_frame.c and src/safety_guards.c.
#ifndef SAFTYFW_BOOTLOADER_CRC32_H
#define SAFTYFW_BOOTLOADER_CRC32_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Computes the CRC-32 of `data[0..len)`. Table-driven (256-entry table,
// built lazily on first call) rather than a bit-at-a-time loop -- this
// function is called on every boot over an up-to-832K application slot
// (docs/BOOTLOADER.md section 3 step 4, "CRC the active slot... every
// boot"), so the O(1)-per-byte table lookup matters here in a way it would
// not for a one-off small buffer.
uint32_t bootloader_crc32(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_BOOTLOADER_CRC32_H
