// update_http.c -- see update_http.h. ESP wiring of update_stage.c: the `stage`
// partition as the injected flash, PSA SHA-256 as the injected hash, and the
// three ADMIN routes.
#include "update_http.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "psa/crypto.h"

#include "http_auth_http.h" /* kiln_http_register() */
#include "ota_http.h"
#include "ota_http_internal.h"
#include "relay_authority.h" /* relay_authority_heat_run_active() */
#include "system_mode_gate.h"
#include "system_mode_gate_http.h"
#include "update_fetch.h"
#include "update_http_internal.h"
#include "update_policy.h"
#include "update_stage.h"
#include "update_stale_stage.h"
#include "wifi_provision_http.h"

static const char *TAG = "update_http";

// Chunk/scratch buffer: ota_http_esp.c's static internal-SRAM 4 KB chunk buffer
// (no new RAM). Internal, not PSRAM: esp_partition_write from PSRAM would bounce
// through a 32 B stack temp, and esp_partition_read into PSRAM allocates a hidden
// internal temp per read. Shared safely because httpd is a single task. Never a
// stack buffer (the httpd task stack is 8 KB).

static const esp_partition_t *s_part;
static update_stage_t s_stage;
static SemaphoreHandle_t s_lock;
static psa_hash_operation_t s_sha;
static bool s_sha_active;
// Result of the one boot-time stale-stage check (update_http_stale_stage_check()). BOOT-scoped: set once per
// boot, never reset by a later upload or clear, so GET reports it as boot_auto_clear. One enum, a few bytes of .bss.
static update_stale_result_t s_last_auto_clear;

static int io_erase(void *ctx, uint32_t off, uint32_t len)
{
    (void)ctx;
    return esp_partition_erase_range(s_part, off, len) == ESP_OK ? 0 : -1;
}
static int io_write(void *ctx, uint32_t off, const void *data, size_t len)
{
    (void)ctx;
    return esp_partition_write(s_part, off, data, len) == ESP_OK ? 0 : -1;
}
static int io_read(void *ctx, uint32_t off, void *data, size_t len)
{
    (void)ctx;
    return esp_partition_read(s_part, off, data, len) == ESP_OK ? 0 : -1;
}
static int io_sha_start(void *ctx)
{
    (void)ctx;
    if (s_sha_active) {
        psa_hash_abort(&s_sha);
        s_sha_active = false;
    }
    s_sha = psa_hash_operation_init();
    if (psa_hash_setup(&s_sha, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        return -1;
    }
    s_sha_active = true;
    return 0;
}
static int io_sha_update(void *ctx, const void *data, size_t len)
{
    (void)ctx;
    return (s_sha_active && psa_hash_update(&s_sha, data, len) == PSA_SUCCESS) ? 0 : -1;
}
static int io_sha_finish(void *ctx, uint8_t out[STAGE_SHA256_LEN])
{
    (void)ctx;
    size_t n = 0;
    if (!s_sha_active) {
        return -1;
    }
    psa_status_t st = psa_hash_finish(&s_sha, out, STAGE_SHA256_LEN, &n);
    s_sha_active = false;
    return (st == PSA_SUCCESS && n == STAGE_SHA256_LEN) ? 0 : -1;
}
static void io_sha_abort(void *ctx)
{
    (void)ctx;
    if (s_sha_active) {
        psa_hash_abort(&s_sha);
        s_sha_active = false;
    }
}
static void io_lock(void *ctx)
{
    (void)ctx;
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}
static void io_unlock(void *ctx)
{
    (void)ctx;
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}
static void io_yield(void *ctx)
{
    (void)ctx;
    vTaskDelay(1); // let IDLE0 feed the watchdog during long erase/hash loops
}

static uint8_t *buf_get(size_t *cap)
{
    return ota_http_esp_chunk_buf(cap);
}

static const char *http_status_for(update_stage_err_t e)
{
    switch (e) {
    case UPDATE_STAGE_OK: return "200 OK";
    case UPDATE_STAGE_ERR_BUSY:
    case UPDATE_STAGE_ERR_VERSION_MISMATCH:
    case UPDATE_STAGE_ERR_POLICY: return "409 Conflict";
    case UPDATE_STAGE_ERR_OVERSIZE: return "413 Payload Too Large";
    case UPDATE_STAGE_ERR_FLASH:
    case UPDATE_STAGE_ERR_HASH:
    case UPDATE_STAGE_ERR_READBACK:
    case UPDATE_STAGE_ERR_HEADER: return "500 Internal Server Error";
    default: return "400 Bad Request";
    }
}

// err_name strings are fixed ASCII with no quote or backslash.
static esp_err_t send_error_json(httpd_req_t *req, const char *status, const char *name)
{
    char json[96];
    snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}", name);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

// Returns true (response sent) when the mode gate refuses.
static bool mode_gate_refuses(httpd_req_t *req, const char *what, const char *ip)
{
    sys_mode_snapshot_t snap = { 0 };
    relay_authority_heat_run_active(&snap.profile_running, &snap.autotune_running);
    char reason[SYSTEM_MODE_GATE_REASON_MAX];
    reason[0] = '\0';
    if (system_mode_gate_check(SYS_ACTION_STAGE_WRITE, &snap, reason, sizeof(reason))) {
        ESP_LOGW(TAG, "%s from %s: refused by system mode gate: %s", what, ip, reason);
        (void)system_mode_gate_http_send_refusal(req, reason);
        return true;
    }
    return false;
}

// Interlock check; true (response sent) when refused.
static bool interlock_refuses(httpd_req_t *req, const char *what, const char *ip)
{
    char reason[OTA_INTERLOCK_REASON_MAX];
    ota_interlock_result_t gate = ota_http_check_interlocks(ota_http_req_ack_no_safety(req), reason, sizeof(reason));
    if (gate != OTA_INTERLOCK_OK) {
        ESP_LOGW(TAG, "%s from %s: refused by interlock: %s", what, ip, reason);
        (void)ota_http_send_interlock_refusal(req, gate, reason);
        return true;
    }
    return false;
}

static bool claim_refuses(httpd_req_t *req, const char *what, const char *ip)
{
    if (!ota_http_update_try_begin(OTA_HTTP_CONTEXT_ESP)) {
        ESP_LOGW(TAG, "%s from %s: refused, an update is already in progress", what, ip);
        (void)send_error_json(req, "409 Conflict", "update_in_progress");
        return true;
    }
    // Claim-first ordering: heat starts test ota_http_heat_blocked_by_update(), which reads the claim, so
    // once it is held no run can start. A run that started between the pre-claim mode-gate check and the
    // claim is caught here by re-checking the mode gate under the claim; the claim is released on refusal.
    // (The OTA interlock cannot be re-run here: it refuses while ANY claim, including ours, is held.)
    if (mode_gate_refuses(req, what, ip)) {
        ota_http_update_end();
        return true;
    }
    return false;
}

// Optional request header as text; false when absent, true (and dst NUL-terminated) when present and fits.
static bool hdr_text(httpd_req_t *req, const char *name, char *dst, size_t cap)
{
    dst[0] = '\0';
    size_t n = httpd_req_get_hdr_value_len(req, name);
    if (n == 0 || n >= cap) {
        return false;
    }
    return httpd_req_get_hdr_value_str(req, name, dst, cap) == ESP_OK;
}

static bool hdr_flag(httpd_req_t *req, const char *name)
{
    char v[8];
    return hdr_text(req, name, v, sizeof(v)) && (strcmp(v, "1") == 0 || strcmp(v, "true") == 0);
}

// Optional unsigned header: 0 = absent, 1 = parsed into *out, -1 = present but malformed (not a plain
// decimal, trailing junk, zero or out of range). Never fails open to "absent".
static int hdr_u32(httpd_req_t *req, const char *name, uint32_t *out)
{
    char v[12];
    *out = 0;
    if (httpd_req_get_hdr_value_len(req, name) == 0) {
        return 0;
    }
    return hdr_text(req, name, v, sizeof(v)) && update_policy_parse_hdr_u32(v, out) ? 1 : -1;
}

// Downgrade gate (docs/GITHUB_RELEASE_UPDATE_PLAN.md section 6) for the hand upload, applied once
// the image head is in (so a version taken from the image's own app descriptor is covered too), before any
// image byte is written. On refusal the 409 (400 for a malformed header) is sent here and
// UPDATE_STAGE_ERR_POLICY returned. The schema versions come from the identity record embedded in the
// image (update_image_id_t); X-Stage-Zones-Cfg / X-Stage-Kilnlink / X-Stage-Uart are advisory and must agree
// with it. Overrides are X-Stage-Force, X-Stage-Allow-Downgrade and X-Stage-Confirm (typed: equal to the
// version). The project-identity check is not part of this.
typedef struct {
    httpd_req_t *req;
    const char *ip;
} policy_gate_ctx_t;

// Gate scratch lives here, not on the httpd stack (the handler sits at its stack ceiling). Safe because
// ota_http_update_try_begin() admits one upload at a time.
static struct {
    char confirm[STAGE_SEMVER_FIELD_LEN + 1];
    update_upload_request_t ur;
    update_identity_t run;
    char cand[STAGE_SEMVER_FIELD_LEN + 1];
    char json[512];
} s_gate;

static update_stage_err_t policy_gate(void *vctx, const char *semver, const char *commit, const update_image_id_t *id)
{
    const policy_gate_ctx_t *gc = vctx;
    httpd_req_t *req = gc->req;
    const char *ip = gc->ip;
    (void)hdr_text(req, "X-Stage-Confirm", s_gate.confirm, sizeof(s_gate.confirm));
    memset(&s_gate.ur, 0, sizeof(s_gate.ur));
    s_gate.ur.version = semver;
    s_gate.ur.commit = commit;
    s_gate.ur.have_image_id = id != NULL;
    if (id != NULL) {
        s_gate.ur.image_id = *id;
    }
    if (hdr_u32(req, "X-Stage-Zones-Cfg", &s_gate.ur.zones_cfg_version) < 0 ||
        hdr_u32(req, "X-Stage-Kilnlink", &s_gate.ur.kilnlink_version) < 0 ||
        hdr_u32(req, "X-Stage-Uart", &s_gate.ur.uart_version) < 0) {
        ESP_LOGW(TAG, "stage upload from %s: malformed X-Stage schema header", ip);
        (void)send_error_json(req, "400 Bad Request", "bad_schema_header");
        return UPDATE_STAGE_ERR_POLICY;
    }
    s_gate.ur.force = hdr_flag(req, "X-Stage-Force");
    s_gate.ur.allow_downgrade = hdr_flag(req, "X-Stage-Allow-Downgrade");
    s_gate.ur.confirm = s_gate.confirm;
    update_fetch_running_identity(&s_gate.run, commit);
    update_decision_t d = update_policy_decide_upload(&s_gate.run, &s_gate.ur);
    ESP_LOGW(TAG, "stage upload from %s: policy %s (%s)", ip, update_verdict_name(d.verdict), d.reason ? d.reason : "");
    if (d.allowed) {
        return UPDATE_STAGE_OK;
    }
    const char *name = d.verdict == UPDATE_VERDICT_REFUSE_DOWNGRADE ? "downgrade_refused"
                       : (d.verdict == UPDATE_VERDICT_REFUSE_NEEDS_FORCE || d.verdict == UPDATE_VERDICT_UP_TO_DATE)
                           ? "needs_force"
                           : update_verdict_name(d.verdict);
    // reason strings are static and quote-free; the candidate version may come from the image, so filter it.
    size_t ci = 0;
    for (; semver[ci] != '\0' && ci < sizeof(s_gate.cand) - 1; ci++) {
        char ch = semver[ci];
        bool ok = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '.' || ch == '-' || ch == '+';
        s_gate.cand[ci] = ok ? ch : '?';
    }
    s_gate.cand[ci] = '\0';
    snprintf(s_gate.json, sizeof(s_gate.json),
             "{\"ok\":false,\"error\":\"%s\",\"verdict\":\"%s\",\"reason\":\"%s\","
             "\"needs_typed_confirm\":%s,\"zones_cfg_lower\":%s,"
             "\"candidate_version\":\"%s\",\"running_version\":\"%s\"}",
             name, update_verdict_name(d.verdict), d.reason ? d.reason : "", d.needs_typed_confirm ? "true" : "false",
             d.zones_cfg_lower ? "true" : "false", s_gate.cand, s_gate.run.version);
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_set_type(req, "application/json");
    (void)httpd_resp_sendstr(req, s_gate.json);
    return UPDATE_STAGE_ERR_POLICY;
}

static esp_err_t stage_upload_post_handler(httpd_req_t *req)
{
    char ip[46];
    ota_http_get_client_ip(req, ip, sizeof(ip)); /* logging only -- ADMIN tier is the gate */

    size_t buf_cap = 0;
    uint8_t *buf = buf_get(&buf_cap);

    bool refused = false;
    bool claimed = false;
    bool failed_mid_body = false;

    if (mode_gate_refuses(req, "stage upload", ip) || interlock_refuses(req, "stage upload", ip)) {
        refused = true;
    } else if (claim_refuses(req, "stage upload", ip)) {
        refused = true;
    } else {
        claimed = true;
    }

    if (claimed) {
        char semver[STAGE_SEMVER_FIELD_LEN + 1];
        char commit[STAGE_COMMIT_HEX_LEN + 1];
        semver[0] = '\0';
        commit[0] = '\0';
        size_t vl = httpd_req_get_hdr_value_len(req, "X-Stage-Version");
        if (vl > 0 && vl <= STAGE_SEMVER_FIELD_LEN) {
            (void)httpd_req_get_hdr_value_str(req, "X-Stage-Version", semver, sizeof(semver));
        } else if (vl > STAGE_SEMVER_FIELD_LEN) {
            (void)send_error_json(req, "400 Bad Request", update_stage_err_name(UPDATE_STAGE_ERR_BAD_VERSION));
            failed_mid_body = true;
        }
        size_t cl = httpd_req_get_hdr_value_len(req, "X-Stage-Commit");
        if (!failed_mid_body && cl > 0 && cl <= STAGE_COMMIT_HEX_LEN) {
            (void)httpd_req_get_hdr_value_str(req, "X-Stage-Commit", commit, sizeof(commit));
        } else if (!failed_mid_body && cl > STAGE_COMMIT_HEX_LEN) {
            (void)send_error_json(req, "400 Bad Request", update_stage_err_name(UPDATE_STAGE_ERR_BAD_COMMIT));
            failed_mid_body = true;
        }

        update_stage_err_t e = UPDATE_STAGE_OK;
        if (!failed_mid_body) {
            e = update_stage_upload_begin(&s_stage, buf, buf_cap, (uint32_t)req->content_len, semver,
                                          commit, STAGE_SOURCE_UPLOAD);
            if (e != UPDATE_STAGE_OK) {
                ESP_LOGW(TAG, "stage upload from %s: refused: %s (%u bytes)", ip, update_stage_err_name(e),
                         (unsigned)req->content_len);
                (void)send_error_json(req, http_status_for(e), update_stage_err_name(e));
                failed_mid_body = true;
            }
        }

        policy_gate_ctx_t gate_ctx = { .req = req, .ip = ip };
        bool response_sent = false;
        if (!failed_mid_body) {
            update_stage_set_gate(&s_stage, policy_gate, &gate_ctx);
        }

        size_t remaining = failed_mid_body ? 0 : req->content_len;
        while (!failed_mid_body && remaining > 0) {
            size_t want = remaining < buf_cap ? remaining : buf_cap;
            int r = httpd_req_recv(req, (char *)buf, want);
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                r = httpd_req_recv(req, (char *)buf, want); // one retry, as the OTA path does not need more
            }
            if (r <= 0) {
                ESP_LOGW(TAG, "stage upload from %s: receive failed at %u/%u (%d)", ip,
                         (unsigned)(req->content_len - remaining), (unsigned)req->content_len, r);
                update_stage_upload_abort(&s_stage);
                // Socket is dead; nothing to send and nothing to drain.
                ota_http_update_end();
                return ESP_FAIL;
            }
            e = update_stage_upload_write(&s_stage, buf, (size_t)r);
            if (e != UPDATE_STAGE_OK) {
                ESP_LOGW(TAG, "stage upload from %s: write failed: %s", ip, update_stage_err_name(e));
                response_sent = (e == UPDATE_STAGE_ERR_POLICY); // policy_gate() already sent the 409
                if (!response_sent) {
                    (void)send_error_json(req, http_status_for(e), update_stage_err_name(e));
                }
                failed_mid_body = true;
                break;
            }
            remaining -= (size_t)r;
        }

        if (!failed_mid_body) {
            e = update_stage_upload_finish(&s_stage);
            if (e != UPDATE_STAGE_OK) {
                ESP_LOGW(TAG, "stage upload from %s: finish failed: %s", ip, update_stage_err_name(e));
                (void)send_error_json(req, http_status_for(e), update_stage_err_name(e));
                failed_mid_body = true;
            } else {
                ESP_LOGW(TAG, "stage upload from %s: %u bytes staged and verified", ip,
                         (unsigned)req->content_len);
                httpd_resp_set_type(req, "application/json");
                (void)httpd_resp_sendstr(req, "{\"ok\":true,\"staged\":true}");
            }
        } else {
            update_stage_upload_abort(&s_stage); // idempotent
        }
        update_stage_set_gate(&s_stage, NULL, NULL); // gate_ctx is on this stack frame
        ota_http_update_end();
    }

    esp_err_t ret = ESP_OK;
    if (refused || failed_mid_body) {
        ret = ota_http_refusal_drain(req, buf, buf_cap);
    }
    return ret;
}

static esp_err_t stage_clear_post_handler(httpd_req_t *req)
{
    char ip[46];
    ota_http_get_client_ip(req, ip, sizeof(ip));
    if (mode_gate_refuses(req, "stage clear", ip) || interlock_refuses(req, "stage clear", ip) ||
        claim_refuses(req, "stage clear", ip)) {
        return ESP_OK; // no body expected
    }
    update_stage_err_t e = update_stage_clear(&s_stage);
    ota_http_update_end();
    if (e != UPDATE_STAGE_OK) {
        ESP_LOGW(TAG, "stage clear from %s: failed: %s", ip, update_stage_err_name(e));
        return send_error_json(req, http_status_for(e), update_stage_err_name(e));
    }
    ESP_LOGW(TAG, "stage clear from %s: stage header erased", ip);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true,\"staged\":false}");
}

static esp_err_t stage_status_get_handler(httpd_req_t *req)
{
    size_t buf_cap = 0;
    uint8_t *buf = buf_get(&buf_cap);
    update_stage_info_t info = { 0 };
    update_stage_err_t e = update_stage_get_status(&s_stage, buf, buf_cap, &info);
    if (e != UPDATE_STAGE_OK && e != UPDATE_STAGE_ERR_BUSY && info.reason == NULL) {
        return send_error_json(req, http_status_for(e), update_stage_err_name(e));
    }

    char sha_hex[2 * STAGE_SHA256_LEN + 1] = "";
    const bool hdr_ok = (info.hdr_status == STAGE_HDR_OK);
    if (hdr_ok) {
        ota_http_hex_encode(info.sha256, STAGE_SHA256_LEN, sha_hex);
    }
    // semver is validated charset (digits, '.', '-', '+', alnum) and commit is
    // lowercase hex, by stage_header_decode(); no escaping needed.
    char json[576];
    int n = snprintf(json, sizeof(json),
                     "{\"ok\":true,\"phase\":\"%s\",\"busy\":%s,\"bytes_done\":%u,\"bytes_total\":%u,"
                     "\"staged\":%s,\"reason\":\"%s\",\"header\":\"%s\",\"capacity\":%u,"
                     "\"image_length\":%u,\"state\":\"%s\",\"semver\":\"%s\",\"commit\":\"%s\","
                     "\"sha256\":\"%s\",\"source\":%u,\"boot_auto_clear\":\"%s\",\"boot_auto_cleared\":%s}",
                     update_stage_phase_name(info.phase), info.busy ? "true" : "false",
                     (unsigned)info.bytes_done, (unsigned)info.bytes_total, info.staged ? "true" : "false",
                     info.reason ? info.reason : "", stage_hdr_status_name(info.hdr_status),
                     (unsigned)update_stage_capacity(&s_stage), hdr_ok ? (unsigned)info.image_length : 0u,
                     hdr_ok ? stage_state_name(info.state) : "", hdr_ok ? info.semver : "",
                     hdr_ok ? info.commit : "", sha_hex, hdr_ok ? (unsigned)info.source : 0u,
                     update_stale_result_name(s_last_auto_clear),
                     update_stale_result_cleared(s_last_auto_clear) ? "true" : "false");
    return ota_http_send_json_clamped(req, json, n, sizeof(json));
}

// ---- boot-time stale-stage cleanup (OT-G06) --------------------------------
// See update_stale_stage.h for the identity rule. This is only the ESP wiring:
// its own PSA hash operation (never s_sha, which a concurrent upload or status
// GET may restart), a small stack work area (no static RAM, no heap), the same refusal
// order as POST /api/update/stage/clear (mode gate, OTA interlock, update
// claim, taken only after the hash) and the same clear (update_stage_clear()).

typedef struct {
    const esp_partition_t *app;
    psa_hash_operation_t sha;
    bool sha_active;
    uint8_t buf[1024]; // on the caller stack (ota_confirm, 5120 B): update_http.c takes no heap buffer
} stale_work_t;

static int st_stage_read(void *ctx, uint32_t off, void *data, size_t len)
{
    (void)ctx;
    return esp_partition_read(s_part, off, data, len) == ESP_OK ? 0 : -1;
}
static int st_app_read(void *ctx, uint32_t off, void *data, size_t len)
{
    stale_work_t *w = ctx;
    return esp_partition_read(w->app, off, data, len) == ESP_OK ? 0 : -1;
}
static int st_sha_start(void *ctx)
{
    stale_work_t *w = ctx;
    w->sha = psa_hash_operation_init();
    if (psa_hash_setup(&w->sha, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        return -1;
    }
    w->sha_active = true;
    return 0;
}
static int st_sha_update(void *ctx, const void *data, size_t len)
{
    stale_work_t *w = ctx;
    return (w->sha_active && psa_hash_update(&w->sha, data, len) == PSA_SUCCESS) ? 0 : -1;
}
static int st_sha_finish(void *ctx, uint8_t out[STAGE_SHA256_LEN])
{
    stale_work_t *w = ctx;
    size_t n = 0;
    if (!w->sha_active) {
        return -1;
    }
    psa_status_t st = psa_hash_finish(&w->sha, out, STAGE_SHA256_LEN, &n);
    w->sha_active = false;
    return (st == PSA_SUCCESS && n == STAGE_SHA256_LEN) ? 0 : -1;
}
static void st_sha_abort(void *ctx)
{
    stale_work_t *w = ctx;
    if (w->sha_active) {
        psa_hash_abort(&w->sha);
        w->sha_active = false;
    }
}
static int st_stage_clear(void *ctx)
{
    (void)ctx;
    update_stage_err_t e = update_stage_clear(&s_stage);
    if (e == UPDATE_STAGE_OK) {
        return 0;
    }
    return e == UPDATE_STAGE_ERR_BUSY ? 1 : -1;
}
static void st_yield(void *ctx)
{
    (void)ctx;
    vTaskDelay(1);
}

// Claim step, taken only AFTER the hash (update_stale_stage.h): the same
// refusal order as POST /api/update/stage/clear (mode gate, OTA interlock,
// update claim). A refusal is transient, so retry 6 times, 5 s apart; the hash
// is already done and is not repeated.
static int st_claim_begin(void *ctx)
{
    (void)ctx;
    for (int attempt = 0; attempt < 6; attempt++) {
        if (attempt > 0) {
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
        sys_mode_snapshot_t snap = { 0 };
        relay_authority_heat_run_active(&snap.profile_running, &snap.autotune_running);
        char reason[OTA_INTERLOCK_REASON_MAX > SYSTEM_MODE_GATE_REASON_MAX ? OTA_INTERLOCK_REASON_MAX
                                                                            : SYSTEM_MODE_GATE_REASON_MAX];
        reason[0] = '\0';
        if (system_mode_gate_check(SYS_ACTION_STAGE_WRITE, &snap, reason, sizeof(reason))) {
            ESP_LOGI(TAG, "stale-stage clear deferred by system mode gate: %s", reason);
            continue;
        }
        reason[0] = '\0';
        // ack_no_safety_processor=true: this only erases a 4 KB header sector, it
        // never writes either processor, so a missing safety processor must not
        // keep a stale stage on the page.
        if (ota_http_check_interlocks(true, reason, sizeof(reason)) != OTA_INTERLOCK_OK) {
            ESP_LOGI(TAG, "stale-stage clear deferred by OTA interlock: %s", reason);
            continue;
        }
        if (!ota_http_update_try_begin(OTA_HTTP_CONTEXT_ESP)) {
            ESP_LOGI(TAG, "stale-stage clear deferred: an update is in progress");
            continue;
        }
        return 0;
    }
    return 1;
}
static void st_claim_end(void *ctx)
{
    (void)ctx;
    ota_http_update_end();
}

// Layout assumptions update_stale_stage.c makes about the image it reads
// straight out of flash; tied to the IDF definitions so an IDF change breaks
// the build instead of silently never matching.
_Static_assert(offsetof(esp_app_desc_t, app_elf_sha256) == UPDATE_STALE_APP_DESC_ELF_SHA_OFFSET,
               "stale-stage: app_elf_sha256 offset moved");
_Static_assert(sizeof(((esp_app_desc_t *)0)->app_elf_sha256) == STAGE_SHA256_LEN, "stale-stage: elf sha size");
_Static_assert(sizeof(esp_app_desc_t) == UPDATE_STALE_APP_DESC_SIZE, "stale-stage: esp_app_desc_t size");
_Static_assert(ESP_APP_DESC_MAGIC_WORD == UPDATE_STALE_APP_DESC_MAGIC, "stale-stage: app desc magic");
_Static_assert(sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) == UPDATE_STALE_APP_DESC_IMAGE_OFFSET,
               "stale-stage: app desc offset within the image");

update_stale_result_t update_http_stale_stage_check(bool app_marked_valid)
{
    if (s_part == NULL) {
        return UPDATE_STALE_NOT_RUN; // no stage partition this boot
    }
    const esp_partition_t *app = esp_ota_get_running_partition();
    // The caller's own esp_ota_mark_app_valid_cancel_rollback() result is the
    // confirmation. esp_ota_get_state_partition() is not used: on this
    // single-slot table it reads otadata[0], not the active (higher-seq) entry.
    // A factory boot has no pending-verify slot, so it is never "confirmed".
    bool confirmed = app_marked_valid && app && app->subtype != ESP_PARTITION_SUBTYPE_APP_FACTORY;
    if (!confirmed) {
        s_last_auto_clear = UPDATE_STALE_KEEP_NOT_CONFIRMED;
        ESP_LOGI(TAG, "stale-stage check skipped: running image was not marked valid this boot");
        return s_last_auto_clear;
    }
    stale_work_t work;
    stale_work_t *w = &work;
    memset(w, 0, sizeof(*w));
    w->app = app;

    update_stale_io_t io = {
        .ctx = w,
        .stage_size = s_part->size,
        .app_size = app->size,
        .stage_read = st_stage_read,
        .app_read = st_app_read,
        .sha_start = st_sha_start,
        .sha_update = st_sha_update,
        .sha_finish = st_sha_finish,
        .sha_abort = st_sha_abort,
        .stage_clear = st_stage_clear,
        .claim_begin = st_claim_begin,
        .claim_end = st_claim_end,
        .yield = st_yield,
    };
    const esp_app_desc_t *desc = esp_app_get_description();
    if (desc != NULL) {
        memcpy(io.running_elf_sha256, desc->app_elf_sha256, STAGE_SHA256_LEN);
        for (size_t i = 0; i < STAGE_SHA256_LEN; i++) {
            if (io.running_elf_sha256[i] != 0) {
                io.running_elf_valid = true;
                break;
            }
        }
    }

    update_stale_result_t r = update_stale_stage_run(&io, true, w->buf, sizeof(w->buf));
    s_last_auto_clear = r;
    if (r == UPDATE_STALE_CLEARED) {
        ESP_LOGW(TAG, "stale stage matches the running image (interrupted recovery apply): stage header cleared");
    } else {
        ESP_LOGI(TAG, "stale-stage check: %s", update_stale_result_name(r));
    }
    return r;
}

esp_err_t update_http_start(void)
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "stage");
    if (s_part == NULL) {
        ESP_LOGW(TAG, "no `stage` partition (pre-WP2 table?) -- update staging unavailable this boot");
        return ESP_ERR_NOT_FOUND;
    }
    httpd_handle_t server = wifi_provision_http_get_server();
    if (!server) {
        ESP_LOGE(TAG, "no HTTP server -- wifi_provision_http_start() must run first");
        return ESP_ERR_INVALID_STATE;
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    update_stage_io_t io = {
        .ctx = NULL,
        .partition_size = s_part->size,
        .erase = io_erase,
        .write = io_write,
        .read = io_read,
        .sha_start = io_sha_start,
        .sha_update = io_sha_update,
        .sha_finish = io_sha_finish,
        .sha_abort = io_sha_abort,
        .lock = io_lock,
        .unlock = io_unlock,
        .yield = io_yield,
    };
    update_stage_init(&s_stage, &io);

    static const httpd_uri_t upload_uri = {
        .uri = "/api/update/stage", .method = HTTP_POST, .handler = stage_upload_post_handler,
    };
    static const httpd_uri_t clear_uri = {
        .uri = "/api/update/stage/clear", .method = HTTP_POST, .handler = stage_clear_post_handler,
    };
    static const httpd_uri_t status_uri = {
        .uri = "/api/update/stage", .method = HTTP_GET, .handler = stage_status_get_handler,
    };
    const httpd_uri_t *routes[] = { &upload_uri, &clear_uri, &status_uri };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t err = kiln_http_register(server, routes[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register %s failed: %s", routes[i]->uri, esp_err_to_name(err));
            return err;
        }
    }
    return update_fetch_start(server);
}

// ---- shared with update_fetch.c (GitHub fetch path) ----
update_stage_t *update_http_stage(void)
{
    return &s_stage;
}

bool update_http_mode_gate_refuses(httpd_req_t *req, const char *what, const char *ip)
{
    return mode_gate_refuses(req, what, ip);
}

bool update_http_gate_refuses(httpd_req_t *req, const char *what, const char *ip)
{
    return mode_gate_refuses(req, what, ip) || interlock_refuses(req, what, ip) || claim_refuses(req, what, ip);
}
