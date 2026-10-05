// recovery_apply.h -- the recovery image's "apply staged update" core: copies
// the candidate image the application's stager left in the `stage` partition
// into `app`, then verifies, selects `app` as the boot partition and only then
// erases the stage header (docs/GITHUB_RELEASE_UPDATE_PLAN.md sections 2, 4, 5,
// WP5).
//
// Pure C, no ESP-IDF includes. Flash, SHA-256, full-image verification and the
// boot-partition switch are injected through recovery_apply_io_t so the whole
// order of operations is host-tested against an in-memory NOR model with a
// power cut at every step (test_recovery_apply.c). The ESP wiring is
// recovery_apply_esp.c.
//
// Order of operations (every step's failure leaves a bootable board):
//   1. Read + fully decode the stage header (stage_header.c, shared with the
//      application's stager). Must be state VERIFIED.
//   2. Re-hash the staged image from flash; must equal the header sha256.
//   3. Validate the image's own header/app descriptor (recovery_image_check.c,
//      the same gate the upload route uses) and that it fits `app`.
//   4. Erase `app` ahead in 64 KB blocks and copy 4 KB at a time. From the
//      first erase `app` is not a bootable image; otadata is NOT touched, so
//      the bootloader keeps booting this recovery image (factory) -- and would
//      do so anyway, since an invalid ota_0 falls back to factory.
//      The stage is NOT modified: it stays VERIFIED and installable, so a cut
//      or failure during the copy is retried by simply applying again.
//      (The header is deliberately never rewritten to APPLYING: flash can only
//      clear bits without an erase, and the only way to change the state byte
//      is to erase the header sector, which would destroy the stage's
//      installability before the copy is verified.)
//   5. Re-read `app` from flash; sha256 must equal the header's.
//   6. app_verify (esp_image_verify over the whole image, checks the image's
//      own checksum and digest).
//   7. set_boot(app). Only now does otadata change.
//   8. Erase the stage header sector (the stage reads BLANK afterwards). A
//      failure here is reported but is not an apply failure: `app` is good.
//      A power cut between 7 and 8 leaves a booting app and a still-valid
//      stage, which is harmless (the application treats it as staged).
#ifndef KILNCTL_RECOVERY_APPLY_H
#define KILNCTL_RECOVERY_APPLY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RECOVERY_APPLY_CHUNK 4096u          // copy/hash granularity == flash sector
#define RECOVERY_APPLY_ERASE_BLOCK 65536u   // erase-ahead block on `app`
#define RECOVERY_APPLY_SHA_LEN 32u

typedef struct {
    void *ctx;
    uint32_t stage_size; // bytes in the `stage` partition
    uint32_t app_size;   // bytes in the `app` partition
    // All return 0 on success. Offsets are partition-relative.
    int (*stage_read)(void *ctx, uint32_t off, void *buf, size_t len);
    int (*stage_erase)(void *ctx, uint32_t off, uint32_t len);
    int (*app_read)(void *ctx, uint32_t off, void *buf, size_t len);
    int (*app_erase)(void *ctx, uint32_t off, uint32_t len);
    int (*app_write)(void *ctx, uint32_t off, const void *buf, size_t len);
    int (*sha_start)(void *ctx);
    int (*sha_update)(void *ctx, const void *buf, size_t len);
    int (*sha_finish)(void *ctx, uint8_t out[RECOVERY_APPLY_SHA_LEN]);
    void (*sha_abort)(void *ctx); // may be NULL
    int (*app_verify)(void *ctx, uint32_t image_len); // esp_image_verify; 0 = valid
    int (*set_boot)(void *ctx);                       // esp_ota_set_boot_partition(app); 0 = ok
    void (*yield)(void *ctx);                         // optional, ~every 64 KB
} recovery_apply_io_t;

typedef enum {
    RECOVERY_APPLY_OK = 0,
    RECOVERY_APPLY_ERR_ARG,
    RECOVERY_APPLY_ERR_NOT_STAGED,    // blank header: nothing staged
    RECOVERY_APPLY_ERR_BAD_HEADER,    // header present but invalid
    RECOVERY_APPLY_ERR_NOT_VERIFIED,  // valid header, state is not VERIFIED
    RECOVERY_APPLY_ERR_STAGE_READ,
    RECOVERY_APPLY_ERR_STAGE_HASH,    // staged image does not match the header sha256
    RECOVERY_APPLY_ERR_BAD_IMAGE,     // not a KilnCtrl ESP32-S3 application image
    RECOVERY_APPLY_ERR_TOO_BIG,       // does not fit the app partition
    RECOVERY_APPLY_ERR_ERASE,
    RECOVERY_APPLY_ERR_WRITE,
    RECOVERY_APPLY_ERR_APP_READ,
    RECOVERY_APPLY_ERR_APP_HASH,      // copied image does not read back equal
    RECOVERY_APPLY_ERR_APP_VERIFY,
    RECOVERY_APPLY_ERR_SET_BOOT,
    RECOVERY_APPLY_ERR_SHA,
} recovery_apply_result_t;

typedef enum {
    RECOVERY_APPLY_IDLE = 0,
    RECOVERY_APPLY_CHECKING,   // steps 1-3
    RECOVERY_APPLY_COPYING,    // step 4
    RECOVERY_APPLY_VERIFYING,  // steps 5-6
    RECOVERY_APPLY_FINALIZING, // steps 7-8
    RECOVERY_APPLY_DONE,       // success; the caller restarts
    RECOVERY_APPLY_FAILED,
} recovery_apply_phase_t;

// Written by recovery_apply_run() from the apply task, read by the HTTP status
// route. Single writer; fields are independent word-sized values, so a torn
// snapshot only mixes two adjacent moments.
typedef struct {
    volatile int phase;           // recovery_apply_phase_t
    volatile int result;          // recovery_apply_result_t, valid once phase is DONE/FAILED
    volatile uint32_t done_bytes;
    volatile uint32_t total_bytes;
    volatile bool app_modified;   // true from the first erase of `app`
    volatile bool stage_cleared;  // header erased after a successful apply
} recovery_apply_progress_t;

// scratch must be at least RECOVERY_APPLY_CHUNK bytes. Runs steps 1-8; on
// return prog->phase is DONE or FAILED. Returns the result.
recovery_apply_result_t recovery_apply_run(const recovery_apply_io_t *io, uint8_t *scratch,
                                           size_t scratch_len, recovery_apply_progress_t *prog);

const char *recovery_apply_result_name(recovery_apply_result_t r);
const char *recovery_apply_phase_name(recovery_apply_phase_t p);

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_RECOVERY_APPLY_H
