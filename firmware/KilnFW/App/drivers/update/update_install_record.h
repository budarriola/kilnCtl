// update_install_record.h -- one-shot "install the staged update" request and
// its result, shared by the application (writes the request, reads the result)
// and the recovery image (consumes the request, writes the result).
//
// Storage: kiln_nvs namespace UPD_INST_NS, blob keys UPD_INST_KEY_REQ and
// UPD_INST_KEY_RES. The application erases the old result before it writes a new
// request. The result is never explicitly acknowledged: it persists until the
// next install request replaces it, so GET /api/update/status can always show
// how the last unattended install ended.
//
// Result detail (blob byte 7, zero in older records so an older decoder is unaffected): the low 7 bits
// are a UPD_INST_DETAIL_* code, bit 7 says the app partition had been modified when the install failed.
// The reason stays APPLY_FAILED for every apply failure; the detail carries which one, so an unattended
// client can tell "old app intact, nothing to redo" from "app erased, re-stage".
//
// Recovery-side contract (update_install_boot_run): the request stays present
// until the apply succeeds, so a power cut mid-copy (app erased, otadata at
// factory) is re-applied on the next recovery boot. Each boot with a request
// raises its attempt counter, read back, BEFORE any erase; after
// UPD_INST_MAX_ATTEMPTS the request is cleared and the failure recorded, and
// recovery stays up. The staged image's size and sha256 must equal the
// request's before an apply is started. The success path clears the request
// (read back) before the boot target is set (the pre-boot hook). Every
// failure is recorded as a result and the image stays in recovery. Nothing
// here uses the Pico link. All IO is injected so the logic is host-testable.
#ifndef KILNCTL_UPDATE_INSTALL_RECORD_H
#define KILNCTL_UPDATE_INSTALL_RECORD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UPD_INST_NS "upd_inst"
#define UPD_INST_KEY_REQ "req"
#define UPD_INST_KEY_RES "res"

#define UPD_INST_SHA_LEN 32u
#define UPD_INST_SEMVER_LEN 32u
#define UPD_INST_REQ_SIZE 80u
#define UPD_INST_MAX_ATTEMPTS 3u
#define UPD_INST_RES_SIZE 48u
// Identity of the install a result belongs to: the first UPD_INST_ID_LEN bytes of the staged
// image's sha256 (copied from the request). All zero = unknown/legacy record. It lives in the
// previously unused bytes 40..43 of the result blob, so the blob size and CRC span are unchanged.
#define UPD_INST_ID_LEN 4u

typedef struct {
    uint8_t attempts; // apply attempts already started for this request
    uint32_t image_length;
    uint8_t sha256[UPD_INST_SHA_LEN];
    char semver[UPD_INST_SEMVER_LEN]; // NUL terminated
} update_install_req_t;

typedef enum {
    UPD_INST_RES_NONE = 0,
    UPD_INST_RES_OK = 1,
    UPD_INST_RES_FAIL = 2,
} update_install_res_code_t;

typedef enum {
    UPD_INST_REASON_NONE = 0,
    UPD_INST_REASON_RECORD_BAD,
    UPD_INST_REASON_CLEAR_FAILED,
    UPD_INST_REASON_STAGE_NOT_READY,
    UPD_INST_REASON_SIZE_MISMATCH,
    UPD_INST_REASON_SHA_MISMATCH,
    UPD_INST_REASON_APPLY_FAILED,
    UPD_INST_REASON_NVS_UNAVAILABLE,
    UPD_INST_REASON_RESULT_WRITE_FAILED,
    UPD_INST_REASON_ATTEMPTS_EXCEEDED,
    UPD_INST_REASON_COUNTER_FAILED,
    UPD_INST_REASON_OUTCOME_UNKNOWN, // poll task could not start; the copy may still run: do not power-cycle
    UPD_INST_REASON_ROLLED_BACK,     // result record version 2 only: the new app was never confirmed and the bootloader rolled it back
    UPD_INST_REASON_COUNT
} update_install_reason_t;

#define UPD_INST_DETAIL_MODIFIED_BIT 0x80u
#define UPD_INST_DETAIL_CODE_MASK 0x7Fu
typedef enum {
    UPD_INST_DETAIL_NONE = 0,
    // 1..15 mirror recovery_apply_result_t (recovery_apply.h); recovery_install.c static-asserts the mirror.
    UPD_INST_DETAIL_APPLY_STAGE_HASH = 6,
    UPD_INST_DETAIL_APPLY_SET_BOOT = 14,
    UPD_INST_DETAIL_APPLY_LAST = 15,
    UPD_INST_DETAIL_START_NO_MEM = 32,    // apply_start refused: internal heap floor / task create
    UPD_INST_DETAIL_START_NO_APP = 33,    // apply_start refused: no app partition
    UPD_INST_DETAIL_START_BUSY = 34,      // apply_start refused: an apply is already running
    UPD_INST_DETAIL_START_FAILED = 35,    // apply_start refused for any other reason
    UPD_INST_DETAIL_COUNT
} update_install_detail_t;

typedef struct {
    uint8_t code;   // update_install_res_code_t
    uint8_t reason; // update_install_reason_t
    uint8_t detail; // update_install_detail_t (low 7 bits of byte 7); unknown values are kept, named "unknown"
    bool app_modified; // bit 7 of byte 7: the app partition had been erased/written
    char semver[UPD_INST_SEMVER_LEN];
    uint8_t id[UPD_INST_ID_LEN]; // sha256 prefix of the request this result answers; zeros = none
} update_install_res_t;

// Codecs. Encode returns the blob length or 0 on bad arguments. Decode returns
// true only for a blob of exactly the right length, magic, version and CRC.
size_t update_install_req_encode(const update_install_req_t *r, uint8_t out[UPD_INST_REQ_SIZE]);
bool update_install_req_decode(const uint8_t *buf, size_t len, update_install_req_t *out);
size_t update_install_res_encode(const update_install_res_t *r, uint8_t out[UPD_INST_RES_SIZE]);
bool update_install_res_decode(const uint8_t *buf, size_t len, update_install_res_t *out);

// Result record versions. v1 is the original layout. v2 is byte-identical except that it may carry reasons
// >= UPD_INST_REASON_ROLLED_BACK. Encode writes v2 only for such a reason and v1 otherwise, so an older
// image still decodes every ordinary record; it refuses a v2 record by version (reported as unreadable,
// never misread). Decode accepts both, and refuses a v1 record whose reason is a v2 one.
#define UPD_INST_RES_VERSION_1 1u
#define UPD_INST_RES_VERSION_2 2u

// MED-3 (AUDIT_UPDATE_FAULTS): recovery found the app image was aborted by the bootloader (never
// confirmed on its first boot). Rewrites a last OK result to FAIL/rolled_back; returns true when it changed.
bool update_install_res_apply_rollback(update_install_res_t *res, bool app_rolled_back);

// Writes the identity as UPD_INST_ID_LEN*2 lowercase hex chars plus NUL into out (needs
// UPD_INST_ID_HEX_LEN bytes). All-zero (no identity) prints as "00000000".
#define UPD_INST_ID_HEX_LEN (UPD_INST_ID_LEN * 2u + 1u)
void update_install_id_hex(const uint8_t id[UPD_INST_ID_LEN], char out[UPD_INST_ID_HEX_LEN]);

const char *update_install_reason_name(uint8_t reason);
// Short form for the recovery LCD (<= UPD_INST_REASON_LCD_MAX chars, so "INSTALL FAILED: <x>" fits the
// 40-column warning line); equals the plain name except for the long outcome_unknown reason.
#define UPD_INST_REASON_LCD_MAX 24
const char *update_install_reason_lcd(uint8_t reason);
const char *update_install_detail_name(uint8_t detail);
const char *update_install_code_name(uint8_t code);

// Injected recovery-side IO. Every callback returns 0 on success unless noted.
typedef struct {
    void *ctx;
    // 0 = request read into buf/len; 1 = no request present; <0 = NVS unavailable.
    int (*req_read)(void *ctx, uint8_t *buf, size_t cap, size_t *len);
    // Erases the request AND reads back that it is gone.
    int (*req_clear)(void *ctx);
    // Rewrites the request blob AND reads it back equal (attempt counter bump).
    int (*req_write)(void *ctx, const uint8_t *buf, size_t len);
    // Writes the result blob AND reads it back equal.
    int (*res_write)(void *ctx, const uint8_t *buf, size_t len);
    // Staged image as the stage header describes it. Returns true only when the
    // header decodes and its state is VERIFIED.
    bool (*stage_verified)(void *ctx, uint32_t *image_length, uint8_t sha256[UPD_INST_SHA_LEN]);
    // Starts the (asynchronous) apply; 0 = started, else the refusal as a UPD_INST_DETAIL_START_* code
    // (any other nonzero value is recorded as START_FAILED). The app is untouched on a refusal.
    int (*apply_start)(void *ctx);
} update_install_ops_t;

typedef enum {
    UPD_INST_BOOT_NO_REQUEST = 0, // nothing to do
    UPD_INST_BOOT_STARTED,        // apply task started; post-boot hook clears the request
    UPD_INST_BOOT_REFUSED,        // failure recorded, stay in recovery
} update_install_boot_outcome_t;

// Run once per recovery boot, after NVS is up. On REFUSED *res_out holds the
// failure that was recorded (the write is attempted; it may itself have failed).
update_install_boot_outcome_t update_install_boot_run(const update_install_ops_t *ops,
                                                      update_install_res_t *res_out);

// Records a failure/success for the request that update_install_boot_run
// started (apply task failed, or pre-boot success). Returns 0 when the result
// write verified.
// ``id`` is the request's sha256 prefix (UPD_INST_ID_LEN bytes) or NULL for none.
int update_install_record_result(const update_install_ops_t *ops, uint8_t code, uint8_t reason,
                                 const char *semver, const uint8_t *id);
// Same, with the detail code and the app-modified flag stored in blob byte 7.
int update_install_record_result_ex(const update_install_ops_t *ops, uint8_t code, uint8_t reason, uint8_t detail,
                                    bool app_modified, const char *semver, const uint8_t *id);

// What a recovery entry caused by an install request does after the install failed or was refused.
typedef enum {
    UPD_INST_AFTER_STAY = 0,  // remain in recovery (nothing safe to boot, or storage unreliable)
    UPD_INST_AFTER_EXIT_APP,  // boot the verified app now (the FAIL result stays for the app to read)
    UPD_INST_AFTER_RESTART,   // reboot recovery so the bounded retry advances
} update_install_after_t;
// reason: the recorded failure reason. app_modified: the apply had erased/written app (pass true when
// unknown). app_verifies: app passes the full image verify. attempts_left: the request is still present
// with attempts < UPD_INST_MAX_ATTEMPTS (the counter for the failed attempt was already raised and read
// back, so a restart is bounded). Never returns EXIT_APP for an app that does not verify; never returns
// RESTART without attempts left; always STAY when storage itself failed (a restart or exit could loop).
update_install_after_t update_install_after_fail(uint8_t reason, bool app_modified, bool app_verifies,
                                                 bool attempts_left);

// Recovery pre-boot hook body (after the copy verified, before the boot target
// switch): boot_guard_clear only (nonzero aborts the switch, request kept for the
// bounded retry). Returns 0 to let the switch run, -1 to abort. The request is NOT
// cleared and no result is written here: a failing esp_ota_set_boot_partition must
// leave the request for a retry and no OK record behind.
int update_install_pre_boot(const update_install_ops_t *ops, int (*boot_guard_clear)(void));

// Recovery post-boot hook body: call only AFTER the boot target switch returned OK.
// req_clear (nonzero returns -1; the image is already selected), then the OK result
// write (failure only reported via *result_ok: the install is good).
int update_install_post_boot(const update_install_ops_t *ops, const char *semver, const uint8_t *id,
                             bool *result_ok);

// Application, healthy confirmed boot (REVIEW_OTA_BOOT_FIXES LOW-4, AUDIT_UPDATE_FAULTS LOW-3): drop a
// leftover install request so it cannot replay at a later recovery entry. `claimed` is whether the
// update claim was taken (an install POST in flight holds it; then nothing is erased). Result:
// 0 = erased (or nothing to do), 1 = skipped, claim busy, -1 = erase failed.
int update_install_drop_stale(bool claimed, int (*req_erase)(void));

// OA-03: same drop, but first records the outcome the lost recovery post_boot would have written. The
// request is read through ops->req_read; when it decodes and `running_matches` says the running app is
// that staged image (length and sha256 of the request), an OK result is written, else FAIL
// outcome_unknown. The result is written BEFORE the request is cleared (a cut in between just repeats).
// 0 = done (or no request), 1 = claim busy, -1 = request clear failed. A result write failure is not
// an error here (nothing is lost that the old code kept).
int update_install_drop_stale_recorded(bool claimed, const update_install_ops_t *ops,
                                       bool (*running_matches)(void *ctx, const update_install_req_t *req),
                                       void *match_ctx);

// ---- Application side: write the request before the reboot into recovery ----
// Injected IO for update_install_prepare (the install route's prepare hook).
typedef struct {
    void *ctx;
    bool (*kv_write_safe)(void *ctx); // NVS may be written from this task
    // Stage as the stage header describes it. Returns true only for a stage that
    // reads back VERIFIED (sha re-hashed from flash); *busy is true while an
    // upload or fetch owns the stage.
    bool (*stage_info)(void *ctx, bool *busy, uint32_t *image_length, uint8_t sha256[UPD_INST_SHA_LEN],
                       char semver[UPD_INST_SEMVER_LEN]);
    int (*res_erase)(void *ctx);                             // erase old result, read back gone
    int (*req_write)(void *ctx, const uint8_t *buf, size_t len); // write request, read back equal
} update_install_app_ops_t;

// Returns true when a fresh request (attempts 0) is stored and read back. On false
// msg holds the refusal text and *http_status is 409 (state refuses) or 500
// (storage failed; the partially written request has been left for the undo hook).
bool update_install_prepare(const update_install_app_ops_t *ops, char *msg, size_t cap, int *http_status);

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_UPDATE_INSTALL_RECORD_H
