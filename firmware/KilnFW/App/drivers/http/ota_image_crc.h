// CRC-32 over an OTA image, in the one parameterization the whole system
// agrees on.
//
// WHY THIS FILE EXISTS. The value produced here is compared, byte for byte,
// against a number computed on a DIFFERENT PROCESSOR by a DIFFERENT
// IMPLEMENTATION: the ESP stages a Pico image into `pico_img` and CRCs it
// (ota_http_pico.c's ota_pico_do_stage()), sends that CRC across the
// isolated link in UPDATE_BEGIN/UPDATE_END (ota_pico_relay.c), and SaftyFW
// reads the freshly written slot back out of flash and CRCs it with
// bootloader_crc32() (firmware/SaftyFW/bootloader/crc32.c) before accepting
// the update (firmware/SaftyFW/src/tasks/update_task.c). If the two sides
// disagree about the CRC PARAMETERIZATION -- not the data, just the
// arithmetic -- the comparison can never succeed for any image, and the
// whole Pico OTA path is inoperable. That is exactly what happened; see
// docs/audits/pico_ota_staged_crc_mismatch_2026-09-18.md.
//
// The agreed parameterization is standard CRC-32 (also called CRC-32/zlib,
// CRC-32/ISO-HDLC, IEEE 802.3):
//
//   polynomial 0x04C11DB7, reflected as 0xEDB88320
//   init       0xFFFFFFFF
//   final XOR  0xFFFFFFFF
//   input and output both reflected
//   check value: CRC-32 of the ASCII bytes "123456789" is 0xCBF43926
//
// THE TRAP THIS FILE EXISTS TO CLOSE. esp_rom_crc32_le() ALREADY performs
// both of those inversions itself -- it complements the seed it is given on
// entry and complements its result on exit. esp_rom_crc.h's own note that
// the algorithm "adds ~ at the beginning and the end" describes what the
// function does FOR the caller; it is not an instruction TO the caller. A
// caller that seeds 0xFFFFFFFF and then applies its own final XOR inverts
// twice at each end, and those extra inversions do NOT cancel: they shift
// the computation to init 0x00000000 with no output XOR, which is a
// different CRC-32 variant that agrees with nothing. The correct call is a
// seed of 0, chaining by feeding the previous return value straight back
// in, and no final XOR -- which is what every other CRC call site in KilnFW
// already does (kiln_package.c, kiln_cfg_swap.c, crash_report.c,
// zones_config_migrate.c, profiles_http.c) and what this module wraps so
// there is exactly one place left to get it wrong.
//
// Pinned by test_ota_image_crc.c's known-answer test against 0xCBF43926.
// That test is the point of this file being a separate, linkable module at
// all: HTTP handlers in this project are target-build-only and do not link
// into the host suite, so a test written against ota_pico_do_stage() would
// never have run.
#ifndef OTA_IMAGE_CRC_H
#define OTA_IMAGE_CRC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Seed for a running CRC. Zero, NOT 0xFFFFFFFF -- see the header comment
 * above: the initial complement is applied inside the CRC primitive, so
 * handing it 0xFFFFFFFF here would apply it a second time. */
#define OTA_IMAGE_CRC32_INIT 0u

/* Folds `len` bytes of `buf` into the running CRC `crc` and returns the new
 * running value. Start from OTA_IMAGE_CRC32_INIT, call once per chunk in
 * order, and the value returned by the LAST call is the finished CRC-32 --
 * there is no separate finish step and no final XOR to apply. Chunk
 * boundaries do not affect the result: hashing a buffer in one call and in
 * N calls yields the same value.
 *
 * A NULL `buf` or a zero `len` returns `crc` unchanged. */
uint32_t ota_image_crc32_update(uint32_t crc, const uint8_t *buf, size_t len);

/* One-shot convenience for a buffer already held whole in memory --
 * equivalent to a single _update() from OTA_IMAGE_CRC32_INIT. */
uint32_t ota_image_crc32(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif // OTA_IMAGE_CRC_H
