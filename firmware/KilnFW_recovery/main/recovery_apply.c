// recovery_apply.c -- see recovery_apply.h. No ESP-IDF includes.
#include "recovery_apply.h"

#include <string.h>

#include "recovery_image_check.h"
#include "stage_header.h"

static void set_phase(recovery_apply_progress_t *p, recovery_apply_phase_t ph)
{
    p->phase = (int)ph;
}

static recovery_apply_result_t fail(recovery_apply_progress_t *p, recovery_apply_result_t r)
{
    p->result = (int)r;
    set_phase(p, RECOVERY_APPLY_FAILED);
    return r;
}

typedef int (*read_fn)(void *ctx, uint32_t off, void *buf, size_t len);

// Hashes `len` bytes starting at `base` into `out`. 0 ok, 1 read failure, 2 sha failure.
static int hash_region(const recovery_apply_io_t *io, read_fn read, uint32_t base, uint32_t len,
                       uint8_t *scratch, uint8_t out[RECOVERY_APPLY_SHA_LEN])
{
    if (io->sha_start(io->ctx) != 0) {
        return 2;
    }
    uint32_t off = 0;
    while (off < len) {
        uint32_t n = len - off;
        if (n > RECOVERY_APPLY_CHUNK) {
            n = RECOVERY_APPLY_CHUNK;
        }
        if (read(io->ctx, base + off, scratch, n) != 0) {
            if (io->sha_abort) {
                io->sha_abort(io->ctx);
            }
            return 1;
        }
        if (io->sha_update(io->ctx, scratch, n) != 0) {
            if (io->sha_abort) {
                io->sha_abort(io->ctx);
            }
            return 2;
        }
        off += n;
        if (io->yield && (off % RECOVERY_APPLY_ERASE_BLOCK) == 0) {
            io->yield(io->ctx);
        }
    }
    return io->sha_finish(io->ctx, out) == 0 ? 0 : 2;
}

recovery_apply_result_t recovery_apply_run(const recovery_apply_io_t *io, uint8_t *scratch,
                                           size_t scratch_len, recovery_apply_progress_t *prog)
{
    if (!prog) {
        return RECOVERY_APPLY_ERR_ARG;
    }
    prog->result = 0;
    prog->done_bytes = 0;
    prog->total_bytes = 0;
    prog->app_modified = false;
    prog->stage_cleared = false;
    set_phase(prog, RECOVERY_APPLY_CHECKING);
    if (!io || !scratch || scratch_len < RECOVERY_APPLY_CHUNK || !io->stage_read || !io->stage_erase ||
        !io->app_read || !io->app_erase || !io->app_write || !io->sha_start || !io->sha_update ||
        !io->sha_finish || !io->app_verify || !io->set_boot ||
        io->stage_size < STAGE_IMAGE_OFFSET + RECOVERY_APPLY_CHUNK || io->app_size == 0 ||
        (io->app_size % RECOVERY_APPLY_ERASE_BLOCK) != 0) {
        return fail(prog, RECOVERY_APPLY_ERR_ARG);
    }
    const uint32_t capacity = (io->stage_size - STAGE_IMAGE_OFFSET) & ~(RECOVERY_APPLY_CHUNK - 1u);

    // 1. header.
    if (io->stage_read(io->ctx, 0, scratch, STAGE_HEADER_SIZE) != 0) {
        return fail(prog, RECOVERY_APPLY_ERR_STAGE_READ);
    }
    stage_header_t hdr;
    stage_hdr_status_t hs = stage_header_decode(scratch, STAGE_HEADER_SIZE, capacity, &hdr);
    if (hs == STAGE_HDR_BLANK) {
        return fail(prog, RECOVERY_APPLY_ERR_NOT_STAGED);
    }
    if (hs != STAGE_HDR_OK) {
        return fail(prog, RECOVERY_APPLY_ERR_BAD_HEADER);
    }
    if (!stage_header_is_installable(&hdr)) {
        return fail(prog, RECOVERY_APPLY_ERR_NOT_VERIFIED);
    }
    const uint32_t len = hdr.image_length;
    prog->total_bytes = len;
    if (len > io->app_size) {
        return fail(prog, RECOVERY_APPLY_ERR_TOO_BIG);
    }

    // 2. the staged bytes must still be the ones the header vouches for.
    uint8_t sha[RECOVERY_APPLY_SHA_LEN];
    int hr = hash_region(io, io->stage_read, STAGE_IMAGE_OFFSET, len, scratch, sha);
    if (hr == 1) {
        return fail(prog, RECOVERY_APPLY_ERR_STAGE_READ);
    }
    if (hr != 0) {
        return fail(prog, RECOVERY_APPLY_ERR_SHA);
    }
    if (!stage_header_sha256_matches(&hdr, sha)) {
        return fail(prog, RECOVERY_APPLY_ERR_STAGE_HASH);
    }

    // 3. the image's own identity (chip, project) and size.
    size_t first = len < RECOVERY_APPLY_CHUNK ? len : RECOVERY_APPLY_CHUNK;
    if (io->stage_read(io->ctx, STAGE_IMAGE_OFFSET, scratch, first) != 0) {
        return fail(prog, RECOVERY_APPLY_ERR_STAGE_READ);
    }
    ric_result_t vr = ric_validate_first_chunk(scratch, first, len, io->app_size, RIC_EXPECTED_PROJECT);
    if (vr == RIC_OVERSIZE) {
        return fail(prog, RECOVERY_APPLY_ERR_TOO_BIG);
    }
    if (vr != RIC_OK) {
        return fail(prog, RECOVERY_APPLY_ERR_BAD_IMAGE);
    }

    // 4. copy. `app` stops being bootable at the first erase.
    set_phase(prog, RECOVERY_APPLY_COPYING);
    prog->app_modified = true;
    uint32_t erased = 0;
    for (uint32_t off = 0; off < len; off += RECOVERY_APPLY_CHUNK) {
        if (off >= erased) {
            uint32_t en = io->app_size - erased;
            if (en > RECOVERY_APPLY_ERASE_BLOCK) {
                en = RECOVERY_APPLY_ERASE_BLOCK;
            }
            if (io->app_erase(io->ctx, erased, en) != 0) {
                return fail(prog, RECOVERY_APPLY_ERR_ERASE);
            }
            erased += en;
        }
        uint32_t n = len - off;
        if (n > RECOVERY_APPLY_CHUNK) {
            n = RECOVERY_APPLY_CHUNK;
        }
        if (io->stage_read(io->ctx, STAGE_IMAGE_OFFSET + off, scratch, n) != 0) {
            return fail(prog, RECOVERY_APPLY_ERR_STAGE_READ);
        }
        if (io->app_write(io->ctx, off, scratch, n) != 0) {
            return fail(prog, RECOVERY_APPLY_ERR_WRITE);
        }
        prog->done_bytes = off + n;
        if (io->yield && ((off + n) % RECOVERY_APPLY_ERASE_BLOCK) == 0) {
            io->yield(io->ctx);
        }
    }

    // 5-6. what is in `app` must be what was staged, and a whole valid image.
    set_phase(prog, RECOVERY_APPLY_VERIFYING);
    hr = hash_region(io, io->app_read, 0, len, scratch, sha);
    if (hr == 1) {
        return fail(prog, RECOVERY_APPLY_ERR_APP_READ);
    }
    if (hr != 0) {
        return fail(prog, RECOVERY_APPLY_ERR_SHA);
    }
    if (!stage_header_sha256_matches(&hdr, sha)) {
        return fail(prog, RECOVERY_APPLY_ERR_APP_HASH);
    }
    if (io->app_verify(io->ctx, len) != 0) {
        return fail(prog, RECOVERY_APPLY_ERR_APP_VERIFY);
    }

    // 7-8. only a verified app becomes the boot partition; only then is the stage dropped.
    set_phase(prog, RECOVERY_APPLY_FINALIZING);
    if (io->set_boot(io->ctx) != 0) {
        return fail(prog, RECOVERY_APPLY_ERR_SET_BOOT);
    }
    prog->stage_cleared = io->stage_erase(io->ctx, 0, STAGE_HEADER_SECTOR) == 0;
    prog->result = RECOVERY_APPLY_OK;
    set_phase(prog, RECOVERY_APPLY_DONE);
    return RECOVERY_APPLY_OK;
}

const char *recovery_apply_result_name(recovery_apply_result_t r)
{
    switch (r) {
    case RECOVERY_APPLY_OK: return "ok";
    case RECOVERY_APPLY_ERR_ARG: return "bad_arg";
    case RECOVERY_APPLY_ERR_NOT_STAGED: return "not_staged";
    case RECOVERY_APPLY_ERR_BAD_HEADER: return "bad_stage_header";
    case RECOVERY_APPLY_ERR_NOT_VERIFIED: return "stage_not_verified";
    case RECOVERY_APPLY_ERR_STAGE_READ: return "stage_read_failed";
    case RECOVERY_APPLY_ERR_STAGE_HASH: return "stage_hash_mismatch";
    case RECOVERY_APPLY_ERR_BAD_IMAGE: return "not_a_kilnfw_image";
    case RECOVERY_APPLY_ERR_TOO_BIG: return "image_too_big";
    case RECOVERY_APPLY_ERR_ERASE: return "app_erase_failed";
    case RECOVERY_APPLY_ERR_WRITE: return "app_write_failed";
    case RECOVERY_APPLY_ERR_APP_READ: return "app_read_failed";
    case RECOVERY_APPLY_ERR_APP_HASH: return "app_readback_mismatch";
    case RECOVERY_APPLY_ERR_APP_VERIFY: return "app_image_verify_failed";
    case RECOVERY_APPLY_ERR_SET_BOOT: return "set_boot_failed";
    case RECOVERY_APPLY_ERR_SHA: return "sha_failed";
    }
    return "unknown";
}

const char *recovery_apply_phase_name(recovery_apply_phase_t p)
{
    switch (p) {
    case RECOVERY_APPLY_IDLE: return "idle";
    case RECOVERY_APPLY_CHECKING: return "checking";
    case RECOVERY_APPLY_COPYING: return "copying";
    case RECOVERY_APPLY_VERIFYING: return "verifying";
    case RECOVERY_APPLY_FINALIZING: return "finalizing";
    case RECOVERY_APPLY_DONE: return "done";
    case RECOVERY_APPLY_FAILED: return "failed";
    }
    return "unknown";
}
