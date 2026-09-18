// pico_image_manifest.h -- the persisted description of whatever SaftyFW
// image is currently sitting in the `pico_img` staging partition.
//
// WHY THIS EXISTS, AND WHY IT REPLACED THE PLAN'S "EMBED THE IMAGE" SHAPE
// (docs/PICO_AUTO_UPDATE_PLAN.md G1). The plan proposed embedding a SaftyFW
// binary in the ESP application so a boot-time updater always has something
// to push. That is not implementable in this tree, for three independent
// reasons, none of which is the flash budget the plan worried about:
//
//   1. The ESP-IDF build cannot produce a SaftyFW binary. SaftyFW is built by
//      a different toolchain (pico-sdk + arm-none-eabi-gcc) that the ESP-IDF
//      build has no way to invoke as part of itself.
//   2. There is no SaftyFW .bin to embed even by hand.
//      firmware/SaftyFW/CMakeLists.txt's `pico_add_extra_outputs(SaftyFW)` is
//      deliberately DISABLED (see the comment block at that call) because
//      picotool has no prebuilt binary for this pico-sdk version and building
//      it needs a host compiler this machine does not have configured. The
//      build emits a .elf and nothing else.
//   3. Nothing of the sort could be committed anyway:
//      firmware/SaftyFW/.gitignore ignores *.bin, *.elf, *.hex and *.uf2. An
//      embedded copy would have to be a committed, gitignored blob that goes
//      stale silently the moment SaftyFW changes -- re-creating exactly the
//      "MANUAL SYNC HAZARD" that file already warns about.
//
// (The plan's sec 10.5 flash concern is separately moot: `app` in
// firmware/KilnFW/partitions.csv is 0x800000 -- 8 MB -- against a ~1.94 MB
// image. ~95 KB was never the obstacle.)
//
// So the image source is the one that already exists: the `pico_img` staging
// partition (0xE0000, 896 KB), written by a POST to /api/ota/pico. What was
// missing is not an image, it is MEMORY of one: ota_pico_do_stage() wrote the
// bytes, handed them straight to the relay, and kept no record, so after a
// reboot the partition held a perfectly good image that nothing could
// describe or trust. This manifest is that record -- length and CRC-32,
// persisted -- which is what turns a staged image into a re-usable boot-time
// update source.
//
// The CRC is stored, not recomputed-and-believed: re-running
// ota_image_crc32() over the partition at boot and comparing against this
// value is what distinguishes "the image that was staged, intact" from "896
// KB of whatever was in flash", which an erased-but-not-rewritten partition
// or a half-finished upload would otherwise look like.
//
// READ-BACK VERIFIED, like every NVS write in this neighbourhood. A HAL_OK
// from a write is not evidence here (2026-09-08 boot_guard audit: a write
// reported success while the value never reached flash). Mechanics are
// copied from persist/pico_update_attempts.c, which copied them from
// boot_guard.c, deliberately rather than re-derived.
#ifndef PICO_IMAGE_MANIFEST_H
#define PICO_IMAGE_MANIFEST_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Records that `pico_img` now holds an image of `image_length` bytes whose
 * CRC-32 (ota_image_crc.h's parameterization -- seed 0, no final XOR; NOT
 * esp_rom_crc32_le's, see commit fabd270f) is `image_crc32`.
 *
 * Called by ota_http_pico.c immediately after a successful stage, BEFORE the
 * relay is started: if the relay attempt that follows fails, the staged image
 * is still perfectly good and the next boot should be able to retry with it.
 *
 * Returns true only once the record has been read back and confirmed, with
 * one bounded erase-then-retry. A false return means the caller must not
 * assume a future boot will find this image -- the stage itself is unaffected
 * and the relay may still proceed. */
bool pico_image_manifest_store(uint32_t image_length, uint32_t image_crc32);

/* Loads the manifest. Returns false (outputs untouched) if there is none, or
 * if the stored record fails its version/CRC check -- corruption collapses to
 * "no staged image", the safe direction: it makes the updater do nothing,
 * never makes it push something unverified. Outputs may be NULL. */
bool pico_image_manifest_load(uint32_t *out_image_length, uint32_t *out_image_crc32);

/* Forgets the staged image. Returns true iff confirmed gone (or already
 * absent). Not called on a failed update -- a failure is remembered by
 * pico_update_attempts_record_failure(), and the image stays described so an
 * operator can see what was attempted. */
bool pico_image_manifest_clear(void);

#ifdef __cplusplus
}
#endif

#endif // PICO_IMAGE_MANIFEST_H
