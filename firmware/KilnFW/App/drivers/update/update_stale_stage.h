// update_stale_stage.h -- the application's boot-time "stale stage" cleanup
// (docs/GITHUB_RELEASE_UPDATE_PLAN.md section 4, OT-G06).
//
// Problem: recovery's apply copies stage -> app, calls set_boot, and only THEN
// erases the stage header. A power cut between the two leaves a VERIFIED stage
// of the very image that is now running, and recovery cannot tell (it does no
// version compare, WP5 decision 5). Left alone, the UI would keep offering an
// already-installed update. After the running image has confirmed itself valid
// the application therefore compares the stage with its own image and, only on
// a proven match, clears the stage header through the normal clear path.
//
// Image identity, cheapest reliable first:
//   1. Prefilter, one 256 B read: the stage image's esp_app_desc_t (at image
//      offset 32) must carry the running image's app_elf_sha256. The ELF hash
//      is the build identity the IDF itself uses; reading it costs one small
//      flash read, so the common "nothing staged / stage differs" boot pays no
//      hash. The commit is NOT used: the header commit is optional (all zero
//      for a hand upload), so it cannot prove anything when absent.
//   2. Proof, only after the prefilter matches: SHA-256 of the first
//      image_length bytes of the RUNNING app partition must equal the header's
//      sha256 (the same bytes recovery copied and re-hashed). Only this
//      authorises the erase; the elf hash alone never does.
//   3. The proof runs WITHOUT the update claim (a ~2.6 MB read-only hash must
//      not block heat or an upload). Only afterwards is the claim taken
//      (io->claim_begin), the header re-read and required to be byte-identical
//      to the one that was hashed against, and only then cleared.
//      esp_partition_get_sha256() is deliberately not used: it hashes the
//      image per the app's own metadata length, which is not guaranteed to
//      equal the uploaded .bin length, and the bootloader does not cache it.
//
// Pure C: flash, hash and clear are injected (update_stale_io_t), so the whole
// decision is host-tested (App/test/test_update_stale_stage.c). The ESP glue
// lives in update_http.c (update_http_stale_stage_check()).
#ifndef KILNCTL_UPDATE_STALE_STAGE_H
#define KILNCTL_UPDATE_STALE_STAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "stage_header.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UPDATE_STALE_APP_DESC_SIZE 256u           // sizeof(esp_app_desc_t)
#define UPDATE_STALE_APP_DESC_MAGIC 0xABCD5432u   // ESP_APP_DESC_MAGIC_WORD
#define UPDATE_STALE_APP_DESC_ELF_SHA_OFFSET 144u // within esp_app_desc_t
#define UPDATE_STALE_APP_DESC_IMAGE_OFFSET 32u    // image header (24) + first segment header (8)
#define UPDATE_STALE_SCRATCH_MIN 256u

typedef struct {
    void *ctx;
    uint32_t stage_size; // bytes in the `stage` partition
    uint32_t app_size;   // bytes in the RUNNING app partition
    uint8_t running_elf_sha256[STAGE_SHA256_LEN];
    bool running_elf_valid; // false: identity unknown, never clear
    // All return 0 on success. Offsets are partition-relative.
    int (*stage_read)(void *ctx, uint32_t offset, void *data, size_t len);
    int (*app_read)(void *ctx, uint32_t offset, void *data, size_t len);
    int (*sha_start)(void *ctx);
    int (*sha_update)(void *ctx, const void *data, size_t len);
    int (*sha_finish)(void *ctx, uint8_t out[STAGE_SHA256_LEN]);
    void (*sha_abort)(void *ctx); // may be NULL
    // Erase the stage header via the normal clear path. 0 cleared, >0 busy or
    // refused (try again later), <0 failed.
    int (*stage_clear)(void *ctx);
    // Take the update claim and every refusal that guards a stage write (mode
    // gate, OTA interlock). 0 granted, nonzero refused/busy (may block and
    // retry inside). Called only AFTER the hash, immediately before the header
    // re-read and clear; claim_end is called exactly once per granted claim.
    int (*claim_begin)(void *ctx);
    void (*claim_end)(void *ctx);
    void (*yield)(void *ctx); // optional; called every ~64 KB of hashing
} update_stale_io_t;

typedef enum {
    UPDATE_STALE_NOT_RUN = 0,
    UPDATE_STALE_CLEARED,            // the only result that touched flash
    UPDATE_STALE_KEEP_NOT_CONFIRMED, // running image not (yet) confirmed valid; nothing read
    UPDATE_STALE_KEEP_NO_STAGE,      // header blank
    UPDATE_STALE_KEEP_BAD_HEADER,    // header present but invalid (decode refused it)
    UPDATE_STALE_KEEP_NOT_VERIFIED,  // header valid, state is not VERIFIED
    UPDATE_STALE_KEEP_NO_IDENTITY,   // running elf hash unavailable
    UPDATE_STALE_KEEP_DIFFERENT,     // stage is a different build (prefilter or size)
    UPDATE_STALE_KEEP_HASH_MISMATCH, // same elf hash but image bytes differ from the header sha256
    UPDATE_STALE_KEEP_BUSY,          // claim/gate refused, or clear refused (stage busy); retry later
    UPDATE_STALE_KEEP_CHANGED,       // header differs from the one hashed against (upload/clear raced); kept
    UPDATE_STALE_ERR_IO,             // a read or the hash failed; nothing cleared
    UPDATE_STALE_ERR_CLEAR,          // proven stale, but the clear itself failed
} update_stale_result_t;

// scratch >= UPDATE_STALE_SCRATCH_MIN bytes (larger is faster).
// app_confirmed_valid must be true only once the running image has been
// marked valid (rollback cancelled); false returns immediately with no flash
// access of any kind.
update_stale_result_t update_stale_stage_run(const update_stale_io_t *io, bool app_confirmed_valid,
                                             uint8_t *scratch, size_t scratch_len);

// True for the one result that cleared the stage.
bool update_stale_result_cleared(update_stale_result_t r);
// Static, lowercase, JSON-safe.
const char *update_stale_result_name(update_stale_result_t r);

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_UPDATE_STALE_STAGE_H
