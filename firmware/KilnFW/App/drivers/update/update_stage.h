// update_stage.h -- the stager core: streams a candidate application image
// into the `stage` partition, and reports whether anything installable is
// staged (docs/GITHUB_RELEASE_UPDATE_PLAN.md sections 2-4, WP4).
//
// Pure C. Flash and SHA-256 are injected through update_stage_io_t so the
// whole state machine is host-tested against an in-memory flash; the ESP
// wiring (esp_partition + PSA hash) lives in update_http.c.
//
// On-flash order, chosen so that a power cut or a failed upload can only ever
// leave "nothing staged", never a half-valid stage:
//   1. upload_begin  : checks size/args, then erases the header sector.
//                      From here the stage reads BLANK (not staged).
//   2. upload_write  : image bytes go to STAGE_IMAGE_OFFSET.., erased ahead in
//                      UPDATE_STAGE_ERASE_UNIT blocks, hashed as they arrive.
//   3. upload_finish : re-reads the whole image from flash, hashes it again
//                      and requires it to equal the streamed hash, and only
//                      then writes the header (state VERIFIED, CRC) in one
//                      256-byte write. The header is the LAST thing written.
// "Staged" is reported only when the header decodes (magic, CRC, fields),
// its state is VERIFIED, AND the sha256 of the image bytes re-read from flash
// equals the header's. The re-read is cached (keyed on sha+length) after a
// successful verify and dropped by every begin/clear/abort.
//
// Concurrency: one operation at a time. The optional io->lock/unlock guard
// only the short phase transitions, never a flash call. A second operation
// (or a status call during an upload) is answered UPDATE_STAGE_ERR_BUSY
// without touching flash. The HTTP layer additionally holds the cross-
// processor OTA claim for upload/clear.
#ifndef KILNCTL_UPDATE_STAGE_H
#define KILNCTL_UPDATE_STAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "stage_header.h"
#include "update_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UPDATE_STAGE_ERASE_UNIT 65536u   // erase-ahead granularity (multiple of the 4 KB sector)
#define UPDATE_STAGE_HEAD_LEN 320u       // image bytes buffered before the first flash write (through app_desc and the identity record after it)
#define UPDATE_STAGE_APP_DESC_SIZE 256u
#define UPDATE_STAGE_IMAGE_ID_FROM (UPDATE_STAGE_APP_DESC_OFFSET + UPDATE_STAGE_APP_DESC_SIZE) // identity record scan start
#define UPDATE_STAGE_ESP32S3_CHIP_ID 9u
#define UPDATE_STAGE_ESP_IMAGE_MAGIC 0xE9u
#define UPDATE_STAGE_APP_DESC_OFFSET 32u // image header (24) + first segment header (8)
#define UPDATE_STAGE_APP_DESC_MAGIC 0xABCD5432u
#define UPDATE_STAGE_APP_DESC_VERSION_OFFSET 48u // magic(4)+secure_version(4)+reserv1(8)+version[32]
#define UPDATE_STAGE_APP_DESC_PROJECT_OFFSET 80u // image offset of esp_app_desc_t.project_name[32] (32 + 0x30)
#define UPDATE_STAGE_APP_DESC_PROJECT_LEN 32u
// The only project this stager accepts (CMake project(KilnCtrl)); the recovery image's own check
// uses the same name (RIC_EXPECTED_PROJECT). This is the project-identity gate for every staged
// image, uploaded or fetched from a release.
#define UPDATE_STAGE_EXPECTED_PROJECT "KilnCtrl"
#define UPDATE_STAGE_SCRATCH_MIN 256u

typedef struct {
    void *ctx;
    uint32_t partition_size; // bytes in the `stage` partition
    // All return 0 on success, non-zero on failure. Offsets are partition-relative.
    int (*erase)(void *ctx, uint32_t offset, uint32_t len);
    int (*write)(void *ctx, uint32_t offset, const void *data, size_t len);
    int (*read)(void *ctx, uint32_t offset, void *data, size_t len);
    // One running SHA-256 at a time.
    int (*sha_start)(void *ctx);
    int (*sha_update)(void *ctx, const void *data, size_t len);
    int (*sha_finish)(void *ctx, uint8_t out[STAGE_SHA256_LEN]);
    void (*sha_abort)(void *ctx); // may be NULL
    // Optional; NULL on the host.
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
    void (*yield)(void *ctx); // optional: called every ~64 KB of long loops
} update_stage_io_t;

typedef enum {
    UPDATE_STAGE_IDLE = 0,
    UPDATE_STAGE_UPLOADING,
    UPDATE_STAGE_VERIFYING,
    UPDATE_STAGE_CLEARING,
    UPDATE_STAGE_CHECKING, // a status call is re-hashing the stage
} update_stage_phase_t;

typedef enum {
    UPDATE_STAGE_OK = 0,
    UPDATE_STAGE_ERR_ARG,
    UPDATE_STAGE_ERR_BUSY,
    UPDATE_STAGE_ERR_EMPTY,       // zero-length upload
    UPDATE_STAGE_ERR_OVERSIZE,    // larger than the stage image capacity (refused before any flash op)
    UPDATE_STAGE_ERR_BAD_IMAGE,   // not an ESP32-S3 app image (magic, chip id, app descriptor)
    UPDATE_STAGE_ERR_BAD_VERSION, // no usable semver (client's, or the image's own)
    UPDATE_STAGE_ERR_BAD_COMMIT,
    UPDATE_STAGE_ERR_OVERRUN,     // more bytes than the declared length
    UPDATE_STAGE_ERR_SHORT,       // finish before all declared bytes arrived
    UPDATE_STAGE_ERR_FLASH,       // erase/write/read failed
    UPDATE_STAGE_ERR_HASH,        // sha engine failed
    UPDATE_STAGE_ERR_READBACK,    // image re-read from flash does not match what was streamed
    UPDATE_STAGE_ERR_HEADER,      // header write/readback failed
    UPDATE_STAGE_ERR_STATE,       // call out of sequence
    UPDATE_STAGE_ERR_WRONG_PROJECT, // app descriptor project_name is not UPDATE_STAGE_EXPECTED_PROJECT
    UPDATE_STAGE_ERR_VERSION_MISMATCH, // declared version differs from the image descriptor's valid semver
    UPDATE_STAGE_ERR_POLICY,        // the install gate (update_stage_set_gate) refused the resolved version
} update_stage_err_t;

// Optional install gate, called once per upload from the first write that completes the image head,
// AFTER the project-identity check and with the final semver (the caller's, or the image's own with a
// leading v stripped) and commit. Return UPDATE_STAGE_OK to proceed or UPDATE_STAGE_ERR_POLICY to refuse
// (the upload fails like any other and the stage stays blank). NULL = no gate (the GitHub fetch path
// decides before it downloads).
// `id` is the identity record read from the held-back head, NULL when the image carries none.
typedef update_stage_err_t (*update_stage_gate_fn)(void *ctx, const char *semver, const char *commit,
                                                   const update_image_id_t *id);

typedef struct {
    update_stage_io_t io;
    update_stage_gate_fn gate;
    void *gate_ctx;
    update_stage_phase_t phase;
    uint8_t *scratch;
    size_t scratch_len;
    uint32_t total;
    uint32_t received;   // bytes accepted from the client
    uint32_t written;    // bytes put on flash
    uint32_t erased_upto; // image-relative bytes erased so far
    stage_source_t source;
    char semver[STAGE_SEMVER_FIELD_LEN];
    char commit[STAGE_COMMIT_HEX_LEN + 1];
    bool semver_given;
    uint8_t head[UPDATE_STAGE_HEAD_LEN];
    uint32_t head_len;
    bool head_flushed;
    bool cache_valid;
    uint8_t cache_sha[STAGE_SHA256_LEN];
    uint32_t cache_len;
    // Negative cache: a header whose sha256 does not match the flash image
    // (keyed on the header's sha256 + image_length); spares re-hashing every GET.
    bool bad_valid;
    uint8_t bad_sha[STAGE_SHA256_LEN];
    uint32_t bad_len;
} update_stage_t;

typedef struct {
    update_stage_phase_t phase;
    uint32_t bytes_done;
    uint32_t bytes_total;
    bool busy;
    bool staged;               // header valid AND VERIFIED AND sha256 re-read from flash matches
    stage_hdr_status_t hdr_status;
    const char *reason;        // static; why not staged ("" when staged)
    stage_state_t state;       // valid when hdr_status == STAGE_HDR_OK
    uint32_t image_length;     // valid when hdr_status == STAGE_HDR_OK
    uint8_t sha256[STAGE_SHA256_LEN];
    char semver[STAGE_SEMVER_FIELD_LEN];
    char commit[STAGE_COMMIT_HEX_LEN + 1];
    stage_source_t source;
} update_stage_info_t;

void update_stage_init(update_stage_t *st, const update_stage_io_t *io);

// Largest image the stage can hold.
uint32_t update_stage_capacity(const update_stage_t *st);

// semver may be NULL/"" (the image's own app-descriptor version is used, and
// must then parse as semver); commit may be NULL/"" (zeroed). scratch must stay
// valid until finish/abort and be >= UPDATE_STAGE_SCRATCH_MIN bytes; larger is
// faster. Refuses oversize/empty/bad args BEFORE any flash access.
update_stage_err_t update_stage_upload_begin(update_stage_t *st, uint8_t *scratch, size_t scratch_len,
                                             uint32_t total_len, const char *semver, const char *commit,
                                             stage_source_t source);
// Install a gate for the upload just begun (call after a successful update_stage_upload_begin).
void update_stage_set_gate(update_stage_t *st, update_stage_gate_fn gate, void *ctx);
update_stage_err_t update_stage_upload_write(update_stage_t *st, const uint8_t *data, size_t len);
update_stage_err_t update_stage_upload_finish(update_stage_t *st);
// Ends a failed/cancelled upload. The header was erased at begin, so the stage
// stays blank; safe to call when no upload is active.
void update_stage_upload_abort(update_stage_t *st);

// Erases the header sector (the image area is left; without a header it is
// unreachable). Idempotent.
update_stage_err_t update_stage_clear(update_stage_t *st);

// Reads (and, if needed, re-hashes) the stage. While another operation is in
// flight it reports busy/progress without touching flash. scratch as above.
update_stage_err_t update_stage_get_status(update_stage_t *st, uint8_t *scratch, size_t scratch_len,
                                           update_stage_info_t *out);

const char *update_stage_err_name(update_stage_err_t e);
const char *update_stage_phase_name(update_stage_phase_t p);

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_UPDATE_STAGE_H
