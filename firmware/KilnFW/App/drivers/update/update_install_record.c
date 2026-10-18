// update_install_record.c -- see update_install_record.h.
#include "update_install_record.h"

#include <string.h>

#include "ota_image_crc.h"

#define REQ_MAGIC 0x31524955u // "UIR1"
#define RES_MAGIC 0x31535255u // "URS1"
#define REC_VERSION 1u

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool semver_terminated(const char *s)
{
    return memchr(s, '\0', UPD_INST_SEMVER_LEN) != NULL;
}

size_t update_install_req_encode(const update_install_req_t *r, uint8_t out[UPD_INST_REQ_SIZE])
{
    if (!r || !out || !semver_terminated(r->semver) || r->image_length == 0) {
        return 0;
    }
    memset(out, 0, UPD_INST_REQ_SIZE);
    put_u32(out, REQ_MAGIC);
    out[4] = (uint8_t)REC_VERSION;
    out[5] = r->attempts;
    put_u32(out + 8, r->image_length);
    memcpy(out + 12, r->sha256, UPD_INST_SHA_LEN);
    memcpy(out + 44, r->semver, UPD_INST_SEMVER_LEN);
    put_u32(out + UPD_INST_REQ_SIZE - 4, ota_image_crc32(out, UPD_INST_REQ_SIZE - 4));
    return UPD_INST_REQ_SIZE;
}

bool update_install_req_decode(const uint8_t *buf, size_t len, update_install_req_t *out)
{
    if (!buf || !out || len != UPD_INST_REQ_SIZE) {
        return false;
    }
    if (get_u32(buf) != REQ_MAGIC || buf[4] != REC_VERSION) {
        return false;
    }
    if (get_u32(buf + UPD_INST_REQ_SIZE - 4) != ota_image_crc32(buf, UPD_INST_REQ_SIZE - 4)) {
        return false;
    }
    update_install_req_t r;
    memset(&r, 0, sizeof(r));
    r.attempts = buf[5];
    r.image_length = get_u32(buf + 8);
    memcpy(r.sha256, buf + 12, UPD_INST_SHA_LEN);
    memcpy(r.semver, buf + 44, UPD_INST_SEMVER_LEN);
    if (r.image_length == 0 || !semver_terminated(r.semver)) {
        return false;
    }
    *out = r;
    return true;
}

size_t update_install_res_encode(const update_install_res_t *r, uint8_t out[UPD_INST_RES_SIZE])
{
    if (!r || !out || !semver_terminated(r->semver)) {
        return 0;
    }
    memset(out, 0, UPD_INST_RES_SIZE);
    put_u32(out, RES_MAGIC);
    out[4] = (uint8_t)(r->reason >= UPD_INST_REASON_ROLLED_BACK ? UPD_INST_RES_VERSION_2 : UPD_INST_RES_VERSION_1);
    out[5] = r->code;
    out[6] = r->reason;
    out[7] = (uint8_t)((r->detail & UPD_INST_DETAIL_CODE_MASK) | (r->app_modified ? UPD_INST_DETAIL_MODIFIED_BIT : 0u));
    memcpy(out + 8, r->semver, UPD_INST_SEMVER_LEN);
    memcpy(out + 8 + UPD_INST_SEMVER_LEN, r->id, UPD_INST_ID_LEN);
    put_u32(out + UPD_INST_RES_SIZE - 4, ota_image_crc32(out, UPD_INST_RES_SIZE - 4));
    return UPD_INST_RES_SIZE;
}

bool update_install_res_decode(const uint8_t *buf, size_t len, update_install_res_t *out)
{
    if (!buf || !out || len != UPD_INST_RES_SIZE) {
        return false;
    }
    if (get_u32(buf) != RES_MAGIC || (buf[4] != UPD_INST_RES_VERSION_1 && buf[4] != UPD_INST_RES_VERSION_2)) {
        return false;
    }
    if (get_u32(buf + UPD_INST_RES_SIZE - 4) != ota_image_crc32(buf, UPD_INST_RES_SIZE - 4)) {
        return false;
    }
    update_install_res_t r;
    memset(&r, 0, sizeof(r));
    r.code = buf[5];
    r.reason = buf[6];
    r.detail = buf[7] & UPD_INST_DETAIL_CODE_MASK;
    r.app_modified = (buf[7] & UPD_INST_DETAIL_MODIFIED_BIT) != 0;
    memcpy(r.semver, buf + 8, UPD_INST_SEMVER_LEN);
    memcpy(r.id, buf + 8 + UPD_INST_SEMVER_LEN, UPD_INST_ID_LEN);
    if (r.code > UPD_INST_RES_FAIL || r.reason >= UPD_INST_REASON_COUNT || !semver_terminated(r.semver)) {
        return false;
    }
    if (buf[4] == UPD_INST_RES_VERSION_1 && r.reason >= UPD_INST_REASON_ROLLED_BACK) {
        return false; // a v1 writer cannot have produced a v2 reason
    }
    *out = r;
    return true;
}

bool update_install_res_apply_rollback(update_install_res_t *res, bool app_rolled_back)
{
    if (!res || !app_rolled_back || res->code != UPD_INST_RES_OK) {
        return false;
    }
    res->code = UPD_INST_RES_FAIL;
    res->reason = UPD_INST_REASON_ROLLED_BACK;
    return true;
}

void update_install_id_hex(const uint8_t id[UPD_INST_ID_LEN], char out[UPD_INST_ID_HEX_LEN])
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < UPD_INST_ID_LEN; i++) {
        out[2 * i] = hex[id[i] >> 4];
        out[2 * i + 1] = hex[id[i] & 0x0f];
    }
    out[UPD_INST_ID_LEN * 2u] = '\0';
}

const char *update_install_reason_name(uint8_t reason)
{
    static const char *const names[UPD_INST_REASON_COUNT] = {
        "none",         "record_bad",   "clear_failed",    "stage_not_ready",   "size_mismatch",
        "sha_mismatch", "apply_failed", "nvs_unavailable", "result_write_failed",
        "attempts_exceeded", "counter_failed", "outcome_unknown_do_not_power_cycle", "rolled_back",
    };
    return reason < UPD_INST_REASON_COUNT ? names[reason] : "unknown";
}

const char *update_install_reason_lcd(uint8_t reason)
{
    if (reason == UPD_INST_REASON_OUTCOME_UNKNOWN) {
        return "unknown, keep power on";
    }
    return update_install_reason_name(reason);
}

const char *update_install_detail_name(uint8_t detail)
{
    // 1..15 mirror recovery_apply_result_name().
    static const char *const apply[UPD_INST_DETAIL_APPLY_LAST + 1] = {
        "none",           "bad_arg",        "not_staged",         "bad_stage_header",    "stage_not_verified",
        "stage_read_failed", "stage_hash_mismatch", "not_a_kilnfw_image", "image_too_big", "app_erase_failed",
        "app_write_failed", "app_read_failed", "app_readback_mismatch", "app_image_verify_failed",
        "set_boot_failed", "sha_failed",
    };
    if (detail <= UPD_INST_DETAIL_APPLY_LAST) {
        return apply[detail];
    }
    switch (detail) {
    case UPD_INST_DETAIL_START_NO_MEM: return "start_low_heap";
    case UPD_INST_DETAIL_START_NO_APP: return "start_no_app";
    case UPD_INST_DETAIL_START_BUSY: return "start_busy";
    case UPD_INST_DETAIL_START_FAILED: return "start_failed";
    default: return "unknown";
    }
}

const char *update_install_code_name(uint8_t code)
{
    return code == UPD_INST_RES_OK ? "ok" : code == UPD_INST_RES_FAIL ? "fail" : "none";
}

int update_install_record_result_ex(const update_install_ops_t *ops, uint8_t code, uint8_t reason, uint8_t detail,
                                    bool app_modified, const char *semver, const uint8_t *id)
{
    if (!ops || !ops->res_write) {
        return -1;
    }
    update_install_res_t r;
    memset(&r, 0, sizeof(r));
    r.code = code;
    r.reason = reason;
    r.detail = detail & UPD_INST_DETAIL_CODE_MASK;
    r.app_modified = app_modified;
    if (semver) {
        strncpy(r.semver, semver, UPD_INST_SEMVER_LEN - 1);
    }
    if (id) {
        memcpy(r.id, id, UPD_INST_ID_LEN);
    }
    uint8_t blob[UPD_INST_RES_SIZE];
    size_t n = update_install_res_encode(&r, blob);
    if (n == 0) {
        return -1;
    }
    return ops->res_write(ops->ctx, blob, n);
}

int update_install_record_result(const update_install_ops_t *ops, uint8_t code, uint8_t reason,
                                 const char *semver, const uint8_t *id)
{
    return update_install_record_result_ex(ops, code, reason, UPD_INST_DETAIL_NONE, false, semver, id);
}

update_install_after_t update_install_after_fail(uint8_t reason, bool app_modified, bool app_verifies,
                                                 bool attempts_left)
{
    switch (reason) {
    case UPD_INST_REASON_NVS_UNAVAILABLE:
    case UPD_INST_REASON_COUNTER_FAILED:     // the attempt counter did not advance: a restart could loop
    case UPD_INST_REASON_RESULT_WRITE_FAILED:
    case UPD_INST_REASON_CLEAR_FAILED:
    case UPD_INST_REASON_OUTCOME_UNKNOWN:    // the copy may still be running
        return UPD_INST_AFTER_STAY;
    default:
        break;
    }
    if (app_verifies && (!app_modified || !attempts_left)) {
        return UPD_INST_AFTER_EXIT_APP;
    }
    if (attempts_left) {
        return UPD_INST_AFTER_RESTART;
    }
    return UPD_INST_AFTER_STAY;
}

int update_install_pre_boot(const update_install_ops_t *ops, int (*boot_guard_clear)(void))
{
    if (!ops || !boot_guard_clear) {
        return -1;
    }
    // Only the boot_guard clear: the request stays until the boot target switch succeeded.
    return boot_guard_clear() == 0 ? 0 : -1;
}

int update_install_post_boot(const update_install_ops_t *ops, const char *semver, const uint8_t *id,
                             bool *result_ok)
{
    if (result_ok) {
        *result_ok = false;
    }
    if (!ops || !ops->req_clear) {
        return -1;
    }
    if (ops->req_clear(ops->ctx) != 0) {
        return -1;
    }
    bool ok = update_install_record_result(ops, UPD_INST_RES_OK, UPD_INST_REASON_NONE, semver, id) == 0;
    if (result_ok) {
        *result_ok = ok;
    }
    return 0;
}

int update_install_drop_stale(bool claimed, int (*req_erase)(void))
{
    if (!claimed) {
        return 1;
    }
    if (!req_erase) {
        return -1;
    }
    return req_erase() == 0 ? 0 : -1;
}

int update_install_drop_stale_recorded(bool claimed, const update_install_ops_t *ops,
                                       bool (*running_matches)(void *ctx, const update_install_req_t *req),
                                       void *match_ctx)
{
    if (!claimed) {
        return 1;
    }
    if (!ops || !ops->req_read || !ops->req_clear) {
        return -1;
    }
    uint8_t blob[UPD_INST_REQ_SIZE];
    size_t len = sizeof(blob);
    int rr = ops->req_read(ops->ctx, blob, sizeof(blob), &len);
    if (rr == 1) {
        return 0; // no request: nothing to drop
    }
    if (rr == 0) {
        update_install_req_t req;
        if (update_install_req_decode(blob, len, &req)) {
            // OF-01: recovery already left a specific FAIL result for this image (e.g. BAD_CHIP_REV, app
            // untouched): keep it, only clear the request.
            bool keep = false;
            if (ops->res_read) {
                uint8_t rb[UPD_INST_RES_SIZE];
                size_t rl = sizeof(rb);
                update_install_res_t old;
                keep = ops->res_read(ops->ctx, rb, sizeof(rb), &rl) == 0 && update_install_res_decode(rb, rl, &old) &&
                       old.code == UPD_INST_RES_FAIL && memcmp(old.id, req.sha256, UPD_INST_ID_LEN) == 0;
            }
            bool match = !keep && running_matches != NULL && running_matches(match_ctx, &req);
            if (!keep)
            (void)update_install_record_result(ops, match ? UPD_INST_RES_OK : UPD_INST_RES_FAIL,
                                               match ? UPD_INST_REASON_NONE : UPD_INST_REASON_OUTCOME_UNKNOWN,
                                               req.semver, req.sha256);
        }
    }
    return ops->req_clear(ops->ctx) == 0 ? 0 : -1;
}

static update_install_boot_outcome_t refuse_ex(const update_install_ops_t *ops, uint8_t reason, uint8_t detail,
                                               const char *semver, const uint8_t *id, update_install_res_t *res_out)
{
    if (res_out) {
        memset(res_out, 0, sizeof(*res_out));
        res_out->code = UPD_INST_RES_FAIL;
        res_out->reason = reason;
        res_out->detail = detail & UPD_INST_DETAIL_CODE_MASK;
        if (semver) {
            strncpy(res_out->semver, semver, UPD_INST_SEMVER_LEN - 1);
        }
        if (id) {
            memcpy(res_out->id, id, UPD_INST_ID_LEN);
        }
    }
    // A refusal happens before any erase: the app partition is untouched.
    (void)update_install_record_result_ex(ops, UPD_INST_RES_FAIL, reason, detail, false, semver, id);
    return UPD_INST_BOOT_REFUSED;
}

static update_install_boot_outcome_t refuse(const update_install_ops_t *ops, uint8_t reason,
                                            const char *semver, const uint8_t *id, update_install_res_t *res_out)
{
    return refuse_ex(ops, reason, UPD_INST_DETAIL_NONE, semver, id, res_out);
}

// A refusal that drops the request: if the clear itself fails the request would replay at the next entry,
// so say CLEAR_FAILED (STAY) instead of the specific reason (REVIEW_OTA_BOOT_FIXES recfx leftovers).
static update_install_boot_outcome_t refuse_dropping(const update_install_ops_t *ops, uint8_t reason,
                                                     const char *semver, const uint8_t *id,
                                                     update_install_res_t *res_out)
{
    if (ops->req_clear(ops->ctx) != 0) {
        return refuse(ops, UPD_INST_REASON_CLEAR_FAILED, semver, id, res_out);
    }
    return refuse(ops, reason, semver, id, res_out);
}

update_install_boot_outcome_t update_install_boot_run(const update_install_ops_t *ops,
                                                      update_install_res_t *res_out)
{
    if (res_out) {
        memset(res_out, 0, sizeof(*res_out));
    }
    if (!ops || !ops->req_read || !ops->req_clear || !ops->req_write || !ops->res_write ||
        !ops->stage_verified || !ops->apply_start) {
        return UPD_INST_BOOT_NO_REQUEST;
    }
    uint8_t blob[UPD_INST_REQ_SIZE + 16];
    size_t len = 0;
    int rr = ops->req_read(ops->ctx, blob, sizeof(blob), &len);
    if (rr == 1) {
        return UPD_INST_BOOT_NO_REQUEST;
    }
    if (rr < 0) {
        // Cannot read the request: stay in recovery, apply nothing.
        return refuse(ops, UPD_INST_REASON_NVS_UNAVAILABLE, NULL, NULL, res_out);
    }
    update_install_req_t req;
    if (!update_install_req_decode(blob, len, &req)) {
        return refuse_dropping(ops, UPD_INST_REASON_RECORD_BAD, NULL, NULL, res_out);
    }
    // Refusals below leave the app untouched, so the request is dropped.
    if (req.attempts >= UPD_INST_MAX_ATTEMPTS) {
        return refuse_dropping(ops, UPD_INST_REASON_ATTEMPTS_EXCEEDED, req.semver, req.sha256, res_out);
    }
    uint32_t stage_len = 0;
    uint8_t stage_sha[UPD_INST_SHA_LEN];
    if (!ops->stage_verified(ops->ctx, &stage_len, stage_sha)) {
        return refuse_dropping(ops, UPD_INST_REASON_STAGE_NOT_READY, req.semver, req.sha256, res_out);
    }
    if (stage_len != req.image_length) {
        return refuse_dropping(ops, UPD_INST_REASON_SIZE_MISMATCH, req.semver, req.sha256, res_out);
    }
    if (memcmp(stage_sha, req.sha256, UPD_INST_SHA_LEN) != 0) {
        return refuse_dropping(ops, UPD_INST_REASON_SHA_MISMATCH, req.semver, req.sha256, res_out);
    }
    // Raise the attempt counter (read back) BEFORE any erase can happen.
    req.attempts++;
    uint8_t nb[UPD_INST_REQ_SIZE];
    if (update_install_req_encode(&req, nb) == 0 || ops->req_write(ops->ctx, nb, UPD_INST_REQ_SIZE) != 0) {
        return refuse(ops, UPD_INST_REASON_COUNTER_FAILED, req.semver, req.sha256, res_out);
    }
    int sr = ops->apply_start(ops->ctx);
    if (sr != 0) {
        uint8_t d = (sr >= (int)UPD_INST_DETAIL_START_NO_MEM && sr < (int)UPD_INST_DETAIL_COUNT)
                        ? (uint8_t)sr
                        : (uint8_t)UPD_INST_DETAIL_START_FAILED;
        return refuse_ex(ops, UPD_INST_REASON_APPLY_FAILED, d, req.semver, req.sha256, res_out);
    }
    if (res_out) {
        res_out->code = UPD_INST_RES_NONE;
        strncpy(res_out->semver, req.semver, UPD_INST_SEMVER_LEN - 1);
        memcpy(res_out->id, req.sha256, UPD_INST_ID_LEN);
    }
    return UPD_INST_BOOT_STARTED;
}

static void set_msg(char *msg, size_t cap, const char *text)
{
    if (msg != NULL && cap > 0) {
        strncpy(msg, text, cap - 1);
        msg[cap - 1] = '\0';
    }
}

bool update_install_prepare(const update_install_app_ops_t *ops, char *msg, size_t cap, int *http_status)
{
    int dummy = 0;
    if (http_status == NULL) {
        http_status = &dummy;
    }
    *http_status = 409;
    if (ops == NULL || ops->kv_write_safe == NULL || ops->stage_info == NULL || ops->res_erase == NULL ||
        ops->req_write == NULL) {
        *http_status = 500;
        set_msg(msg, cap, "install not available");
        return false;
    }
    bool busy = false;
    update_install_req_t req;
    memset(&req, 0, sizeof(req));
    bool verified = ops->stage_info(ops->ctx, &busy, &req.image_length, req.sha256, req.semver);
    if (busy) {
        set_msg(msg, cap, "stage busy (upload or fetch in progress)");
        return false;
    }
    if (!verified || req.image_length == 0 || req.semver[0] == '\0') {
        set_msg(msg, cap, "no verified staged image");
        return false;
    }
    if (!ops->kv_write_safe(ops->ctx)) {
        *http_status = 500;
        set_msg(msg, cap, "storage not writable from this task");
        return false;
    }
    req.semver[UPD_INST_SEMVER_LEN - 1] = '\0';
    uint8_t blob[UPD_INST_REQ_SIZE];
    if (update_install_req_encode(&req, blob) == 0) {
        *http_status = 500;
        set_msg(msg, cap, "install request encode failed");
        return false;
    }
    // Old result first: if the request write then fails, the stale result is gone, never mistaken for this install's.
    if (ops->res_erase(ops->ctx) != 0) {
        *http_status = 500;
        set_msg(msg, cap, "could not clear previous install result");
        return false;
    }
    if (ops->req_write(ops->ctx, blob, UPD_INST_REQ_SIZE) != 0) {
        *http_status = 500;
        set_msg(msg, cap, "could not store install request");
        return false;
    }
    return true;
}
