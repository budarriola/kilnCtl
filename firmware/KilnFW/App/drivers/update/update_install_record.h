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
// Recovery-side contract (update_install_boot_run): the request is cleared
// FIRST and the clear is read back, so a failing apply can never loop; the
// staged image's size and sha256 must equal the request's before an apply is
// started; every failure is recorded as a result and the image stays in
// recovery. All IO is injected so the logic is host-testable.
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
#define UPD_INST_RES_SIZE 48u

typedef struct {
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
    UPD_INST_REASON_COUNT
} update_install_reason_t;

typedef struct {
    uint8_t code;   // update_install_res_code_t
    uint8_t reason; // update_install_reason_t
    char semver[UPD_INST_SEMVER_LEN];
} update_install_res_t;

// Codecs. Encode returns the blob length or 0 on bad arguments. Decode returns
// true only for a blob of exactly the right length, magic, version and CRC.
size_t update_install_req_encode(const update_install_req_t *r, uint8_t out[UPD_INST_REQ_SIZE]);
bool update_install_req_decode(const uint8_t *buf, size_t len, update_install_req_t *out);
size_t update_install_res_encode(const update_install_res_t *r, uint8_t out[UPD_INST_RES_SIZE]);
bool update_install_res_decode(const uint8_t *buf, size_t len, update_install_res_t *out);

const char *update_install_reason_name(uint8_t reason);
const char *update_install_code_name(uint8_t code);

// Injected recovery-side IO. Every callback returns 0 on success unless noted.
typedef struct {
    void *ctx;
    // 0 = request read into buf/len; 1 = no request present; <0 = NVS unavailable.
    int (*req_read)(void *ctx, uint8_t *buf, size_t cap, size_t *len);
    // Erases the request AND reads back that it is gone.
    int (*req_clear)(void *ctx);
    // Writes the result blob AND reads it back equal.
    int (*res_write)(void *ctx, const uint8_t *buf, size_t len);
    // Staged image as the stage header describes it. Returns true only when the
    // header decodes and its state is VERIFIED.
    bool (*stage_verified)(void *ctx, uint32_t *image_length, uint8_t sha256[UPD_INST_SHA_LEN]);
    // Starts the (asynchronous) apply; 0 = started.
    int (*apply_start)(void *ctx);
} update_install_ops_t;

typedef enum {
    UPD_INST_BOOT_NO_REQUEST = 0, // nothing to do
    UPD_INST_BOOT_STARTED,        // apply task started; result is written later
    UPD_INST_BOOT_REFUSED,        // failure recorded, stay in recovery
} update_install_boot_outcome_t;

// Run once per recovery boot, after NVS is up. On REFUSED *res_out holds the
// failure that was recorded (the write is attempted; it may itself have failed).
update_install_boot_outcome_t update_install_boot_run(const update_install_ops_t *ops,
                                                      update_install_res_t *res_out);

// Records a failure/success for the request that update_install_boot_run
// started (apply task failed, or pre-boot success). Returns 0 when the result
// write verified.
int update_install_record_result(const update_install_ops_t *ops, uint8_t code, uint8_t reason,
                                 const char *semver);

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_UPDATE_INSTALL_RECORD_H
