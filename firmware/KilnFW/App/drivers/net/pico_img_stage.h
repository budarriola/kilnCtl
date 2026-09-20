// pico_img_stage.h -- the one erase/write/manifest sequence that writes an
// image into the `pico_img` staging partition, shared by BOTH writers:
// http/ota_http_pico.c's browser-upload handler (streaming, chunk-by-chunk
// off a socket) and net/pico_auto_update_boot.c's embedded-image writer
// (the whole image already sits in flash-mapped .rodata, no streaming
// needed). Factored out so the erase-then-write-then-verified-manifest
// sequence exists in exactly one place, per docs/PICO_AUTO_UPDATE_PLAN.md's
// task 3 instruction not to duplicate it.
//
// CONTRACT, mirrored from ota_http_pico.c's prior in-line version:
//   1. pico_img_stage_begin() finds the partition, checks the image fits,
//      and erases exactly the sectors the image needs (never the whole
//      partition).
//   2. pico_img_stage_write_chunk() may be called any number of times with
//      consecutive, non-overlapping byte ranges; it writes each chunk to
//      flash and folds it into the running CRC-32 (ota_image_crc.h's
//      parameterization -- the SAME routine the relay and SaftyFW's own
//      bootloader_crc32() use, so the two processors can never compute two
//      different "correct" answers for the same bytes -- see
//      docs/audits/pico_ota_staged_crc_mismatch_2026-09-18.md for why that
//      already went wrong once).
//   3. pico_img_stage_finish() stores the persisted, read-back-verified
//      manifest (pico_image_manifest_store()) so a LATER boot can re-verify
//      and re-use this exact image, exactly as ota_http_pico.c's manual
//      upload path already did before this factor.
//
// NOT reentrant and not locked internally: exactly one context may be
// staging into `pico_img` at a time, which the existing cross-processor
// update mutex (ota_http.h's ota_http_update_try_begin()) already
// guarantees for every caller in this tree -- this module trusts that,
// rather than adding a second lock.
#ifndef PICO_IMG_STAGE_H
#define PICO_IMG_STAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_partition.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const esp_partition_t *part;
    uint32_t crc;
    size_t   written;
    size_t   total_len;
} pico_img_stage_ctx_t;

/* Looks up the `pico_img` partition, refuses if content_len exceeds it, and
 * erases ceil(content_len / sector_size) sectors starting at offset 0.
 * Fills *fail_reason (may be NULL) with a short, static-format-free message
 * on failure. Returns false on any failure; *ctx is undefined in that case. */
bool pico_img_stage_begin(pico_img_stage_ctx_t *ctx, size_t content_len, char *fail_reason,
                          size_t fail_reason_len);

/* Writes `len` bytes at the current write offset (ctx->written) and folds
 * them into the running CRC. Chunks must be presented in order, as they were
 * to pico_img_stage_begin()'s content_len budget -- this function does not
 * reorder or buffer. Returns false (and leaves ctx->written at the last
 * successfully written offset) on a flash write failure. */
bool pico_img_stage_write_chunk(pico_img_stage_ctx_t *ctx, const uint8_t *data, size_t len,
                                char *fail_reason, size_t fail_reason_len);

/* Stores the persisted manifest for what was just staged (ctx->written bytes,
 * ctx->crc). Returns pico_image_manifest_store()'s own result -- false means
 * the bytes are staged and usable THIS boot, but a later boot will not find
 * them (same non-fatal-to-the-caller convention ota_http_pico.c already
 * used). out_crc (may be NULL) receives the final CRC for the caller to hand
 * to ota_pico_relay_start(). */
bool pico_img_stage_finish(pico_img_stage_ctx_t *ctx, uint32_t *out_crc);

#ifdef __cplusplus
}
#endif

#endif // PICO_IMG_STAGE_H
