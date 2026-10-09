// update_stale_stage.c -- see update_stale_stage.h.
#include "update_stale_stage.h"

#include <string.h>

#define STALE_YIELD_EVERY 65536u

static bool io_complete(const update_stale_io_t *io)
{
    return io && io->stage_read && io->app_read && io->sha_start && io->sha_update && io->sha_finish &&
           io->stage_clear && io->claim_begin && io->claim_end && io->stage_size > STAGE_IMAGE_OFFSET && io->app_size > 0;
}

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void sha_abort_if(const update_stale_io_t *io)
{
    if (io->sha_abort) {
        io->sha_abort(io->ctx);
    }
}

// SHA-256 of the first `len` bytes of the running app partition into out.
static bool hash_running_app(const update_stale_io_t *io, uint32_t len, uint8_t *scratch, size_t scratch_len,
                             uint8_t out[STAGE_SHA256_LEN])
{
    if (io->sha_start(io->ctx) != 0) {
        return false;
    }
    uint32_t done = 0;
    uint32_t since_yield = 0;
    while (done < len) {
        size_t want = scratch_len;
        if ((uint32_t)want > len - done) {
            want = (size_t)(len - done);
        }
        if (io->app_read(io->ctx, done, scratch, want) != 0 || io->sha_update(io->ctx, scratch, want) != 0) {
            sha_abort_if(io);
            return false;
        }
        done += (uint32_t)want;
        since_yield += (uint32_t)want;
        if (io->yield && since_yield >= STALE_YIELD_EVERY) {
            io->yield(io->ctx);
            since_yield = 0;
        }
    }
    if (io->sha_finish(io->ctx, out) != 0) {
        sha_abort_if(io);
        return false;
    }
    return true;
}

update_stale_result_t update_stale_stage_run(const update_stale_io_t *io, bool app_confirmed_valid,
                                             uint8_t *scratch, size_t scratch_len)
{
    // Rollback is never disturbed: before the image has confirmed itself, do
    // nothing and touch nothing.
    if (!app_confirmed_valid) {
        return UPDATE_STALE_KEEP_NOT_CONFIRMED;
    }
    if (!io_complete(io) || scratch == NULL || scratch_len < UPDATE_STALE_SCRATCH_MIN) {
        return UPDATE_STALE_ERR_IO;
    }
    if (!io->running_elf_valid) {
        return UPDATE_STALE_KEEP_NO_IDENTITY;
    }

    const uint32_t capacity = io->stage_size - STAGE_IMAGE_OFFSET;
    uint8_t hdr0[STAGE_HEADER_SIZE]; // the header every later step is proven against
    if (io->stage_read(io->ctx, 0, hdr0, STAGE_HEADER_SIZE) != 0) {
        return UPDATE_STALE_ERR_IO;
    }
    stage_header_t h;
    stage_hdr_status_t hs = stage_header_decode(hdr0, STAGE_HEADER_SIZE, capacity, &h);
    if (hs == STAGE_HDR_BLANK) {
        return UPDATE_STALE_KEEP_NO_STAGE;
    }
    if (hs != STAGE_HDR_OK) {
        return UPDATE_STALE_KEEP_BAD_HEADER;
    }
    if (!stage_header_is_installable(&h)) {
        return UPDATE_STALE_KEEP_NOT_VERIFIED;
    }
    // The image must at least hold an app descriptor and fit the running slot.
    if (h.image_length < UPDATE_STALE_APP_DESC_IMAGE_OFFSET + UPDATE_STALE_APP_DESC_SIZE ||
        h.image_length > io->app_size) {
        return UPDATE_STALE_KEEP_DIFFERENT;
    }

    // 1. Prefilter: the stage image's own app descriptor.
    if (io->stage_read(io->ctx, STAGE_IMAGE_OFFSET + UPDATE_STALE_APP_DESC_IMAGE_OFFSET, scratch,
                       UPDATE_STALE_APP_DESC_SIZE) != 0) {
        return UPDATE_STALE_ERR_IO;
    }
    if (rd_le32(scratch) != UPDATE_STALE_APP_DESC_MAGIC ||
        memcmp(scratch + UPDATE_STALE_APP_DESC_ELF_SHA_OFFSET, io->running_elf_sha256, STAGE_SHA256_LEN) != 0) {
        return UPDATE_STALE_KEEP_DIFFERENT;
    }

    // 2. Proof: the running partition's bytes hash to the header's sha256.
    uint8_t digest[STAGE_SHA256_LEN];
    if (!hash_running_app(io, h.image_length, scratch, scratch_len, digest)) {
        return UPDATE_STALE_ERR_IO;
    }
    if (!stage_header_sha256_matches(&h, digest)) {
        return UPDATE_STALE_KEEP_HASH_MISMATCH;
    }

    // 3. Claim, then prove the header is still the one we hashed against.
    if (io->claim_begin(io->ctx) != 0) {
        return UPDATE_STALE_KEEP_BUSY;
    }
    update_stale_result_t res;
    if (io->stage_read(io->ctx, 0, scratch, STAGE_HEADER_SIZE) != 0) {
        res = UPDATE_STALE_ERR_IO;
    } else if (memcmp(scratch, hdr0, STAGE_HEADER_SIZE) != 0) {
        res = UPDATE_STALE_KEEP_CHANGED;
    } else {
        int c = io->stage_clear(io->ctx);
        res = c == 0 ? UPDATE_STALE_CLEARED : (c > 0 ? UPDATE_STALE_KEEP_BUSY : UPDATE_STALE_ERR_CLEAR);
    }
    io->claim_end(io->ctx);
    return res;
}

bool update_stale_result_cleared(update_stale_result_t r)
{
    return r == UPDATE_STALE_CLEARED;
}

const char *update_stale_result_name(update_stale_result_t r)
{
    switch (r) {
    case UPDATE_STALE_NOT_RUN: return "not_run";
    case UPDATE_STALE_CLEARED: return "cleared_stale_stage";
    case UPDATE_STALE_KEEP_NOT_CONFIRMED: return "kept_app_not_confirmed";
    case UPDATE_STALE_KEEP_NO_STAGE: return "kept_no_stage";
    case UPDATE_STALE_KEEP_BAD_HEADER: return "kept_bad_header";
    case UPDATE_STALE_KEEP_NOT_VERIFIED: return "kept_not_verified";
    case UPDATE_STALE_KEEP_NO_IDENTITY: return "kept_no_running_identity";
    case UPDATE_STALE_KEEP_DIFFERENT: return "kept_different_image";
    case UPDATE_STALE_KEEP_HASH_MISMATCH: return "kept_hash_mismatch";
    case UPDATE_STALE_KEEP_BUSY: return "kept_busy";
    case UPDATE_STALE_KEEP_CHANGED: return "kept_header_changed";
    case UPDATE_STALE_ERR_IO: return "io_error";
    case UPDATE_STALE_ERR_CLEAR: return "clear_failed";
    }
    return "unknown";
}
