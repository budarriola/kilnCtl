#include "kiln_cfg_http.h"
#include "http_auth_http.h" // kiln_http_register() -- WEB_AUTH_PLAN.md section 5

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "config_divergence.h" /* CONFIG_DIVERGENCE_REASON_MAX */
#include "http_async_job.h" /* http_async_job_busy() -- refuse an apply while ct_auto_zero's
                              * async job is mid-commit, 2026-09-25 fix-then-push re-review */
#include "http_form.h"
#include "kiln_cfg_store.h"
#include "kiln_cfg_swap_worker.h" /* item 5 -- the apply runs on its own task, not this one */
#include "ota_http.h"
#include "relay_authority.h" /* relay_authority_heat_run_active() -- see the system_mode_gate check below */
#include "system_mode_gate.h" /* SYS_ACTION_WRITE_ZONES_CONFIG -- owner decision Q2, 2026-09-25 */
#include "system_mode_gate_http.h" /* system_mode_gate_http_send_refusal() -- 409, shared sender */
#include "safety_cfg_store.h" /* safety_cfg_store_has_data() -- GET /api/kiln_configs */
#include "safety_ceiling_sync.h" /* 2026-09-15 review (review_divergence_check_561efa3b_2026-09-15.md,
                                   * LOW) -- warn on an explicit save while diverged */
#include "web_encoding.h" /* GET /settings/kiln_configs page shell -- same gzip-serving
                             * helper as settings_http.c/backup_export.c */
#include "wifi_provision_http.h"

static const char *TAG = "kiln_cfg_http";

/* GET /settings/kiln_configs -- the kiln-config selector + management page
 * (kiln_configs_page.html), split out of main_page.html's own
 * #kilnConfigPicker/#kcManage disclosure 2026-09-18 so it gets a real URL of
 * its own instead of living inside the dashboard. Same embed-gzip-at-build
 * convention as every other *_page.html in this component (see
 * web_encoding.h's header comment); the dashboard keeps only a compact
 * read-only "active config" indicator plus a link here. The API this page
 * calls (/api/kiln_configs and friends, this same file) is unchanged. */
extern const uint8_t kiln_configs_page_html_gz_start[] asm("_binary_kiln_configs_page_html_gz_start");
extern const uint8_t kiln_configs_page_html_gz_end[] asm("_binary_kiln_configs_page_html_gz_end");

static esp_err_t kiln_configs_page_get_handler(httpd_req_t *req)
{
    if (!web_client_accepts_gzip(req)) {
        return web_send_gzip_not_acceptable(req, TAG, "kiln_configs_page.html");
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    web_set_asset_cache_headers(req);
    return httpd_resp_send(req, (const char *)kiln_configs_page_html_gz_start,
                           (size_t)(kiln_configs_page_html_gz_end - kiln_configs_page_html_gz_start));
}

/* Section 5.3 table row 4 (2026-09-16): same per-request header-ack shape as
 * ota_http.c's OTA_ACK_NO_SAFETY_HEADER/ota_http_req_ack_no_safety() --
 * deliberately not a new shape, per that function's own precedent, and not
 * covered by the request HMAC for the same reason ota_http.c's header
 * documents theirs isn't: this only relaxes a LOCAL policy check
 * (kiln_cfg_store_apply()'s own hardware-shape comparison), never a
 * safety-link/authentication decision. */
#define KILN_CFG_ACK_HW_DIFFERS_HEADER "X-Kiln-Ack-Hardware-Differs"

static bool req_ack_hardware_differs(httpd_req_t *req)
{
    char val[8];
    if (httpd_req_get_hdr_value_str(req, KILN_CFG_ACK_HW_DIFFERS_HEADER, val, sizeof(val)) != ESP_OK) {
        return false;
    }
    return val[0] == '1';
}

/* Small bodies only -- id=<int>[&name=<str up to KILN_CFG_NAME_MAX_LEN>].
 * Generous headroom over what a legitimate request needs, same discipline
 * as every other small-form handler in this codebase (profiles_http.c's
 * profile_delete_post_handler() etc.), checked against Content-Length before
 * a single byte is read. */
#define KILN_CFG_BODY_MAX 128

static bool read_small_body(httpd_req_t *req, char *buf, size_t cap)
{
    if (req->content_len <= 0 || (size_t)req->content_len >= cap) {
        return false;
    }
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int ret = httpd_req_recv(req, buf + received, req->content_len - received);
        if (ret <= 0) {
            return false;
        }
        received += (size_t)ret;
    }
    buf[received] = '\0';
    return true;
}

/* Same escaping convention as zones_http.c's/wifi_provision_http.c's
 * json_escape -- a config name came from a POST body at some point. */
static void json_escape(const char *src, char *out, size_t out_cap)
{
    size_t o = 0;
    for (const char *p = src; *p && o + 2 < out_cap; p++) {
        if (*p == '"' || *p == '\\') {
            if (o + 3 >= out_cap) {
                break;
            }
            out[o++] = '\\';
        }
        out[o++] = *p;
    }
    out[o] = '\0';
}

/* Parses a required "id=<int>" form field, 0..INT32_MAX. Returns false (body
 * untouched by the caller) if missing or malformed. */
static bool parse_required_id(const char *body, const char *key, int32_t *out_id)
{
    char val[16];
    int len = http_form_find_field(body, key, val, sizeof(val));
    if (len <= 0) {
        return false;
    }
    char *end = NULL;
    long v = strtol(val, &end, 10);
    if (end == val || *end != '\0' || v < 0 || v > INT32_MAX) {
        return false;
    }
    *out_id = (int32_t)v;
    return true;
}

/* Parses a required "name=<str>" form field into out (out_cap >=
 * KILN_CFG_NAME_MAX_LEN + 1). Returns false if missing or too long to decode
 * (http_form_find_field()'s -2). Does NOT itself enforce KILN_CFG_NAME_MAX_LEN
 * against the DECODED length -- kiln_cfg_store.c's name_is_valid() is the one
 * place that bound is checked, so there is exactly one definition of "too
 * long" rather than two that could drift. */
static bool parse_required_name(const char *body, char *out, size_t out_cap)
{
    int len = http_form_find_field(body, "name", out, out_cap);
    return len > 0;
}

/* ---- GET /api/kiln_configs -------------------------------------------------
 * {"active_id":<int|null>,"configs":[{"id":N,"name":"...","is_active":bool},
 * ...],"max_count":N,"pico_half_recapture_pending":bool,"has_data":bool}
 * pico_half_recapture_pending: the active slot's Pico half is owed a recapture
 * (deferred by a divergence or an unfetched safety cache); has_data: the
 * safety_cfg_store cache holds a real fetch (false blocks the recapture). */
static esp_err_t list_get_handler(httpd_req_t *req)
{
    kiln_cfg_summary_t rows[KILN_CFG_MAX_COUNT];
    uint8_t n = kiln_cfg_store_list(rows, KILN_CFG_MAX_COUNT);
    int32_t active_id = kiln_cfg_store_get_active_id();

    /* KILN_CFG_MAX_COUNT rows * (~60 bytes/row headroom for id+escaped name)
     * plus a small fixed header -- generous over what 8 rows of a <=23-char
     * name could ever need. */
    char json[KILN_CFG_MAX_COUNT * 96 + 96];
    size_t o = 0;
    int written;

#define APPEND(...)                                                                              \
    do {                                                                                          \
        written = snprintf(json + o, sizeof(json) - o, __VA_ARGS__);                             \
        if (written < 0 || (size_t)written >= sizeof(json) - o) {                                \
            goto overflow;                                                                        \
        }                                                                                          \
        o += (size_t)written;                                                                      \
    } while (0)

    if (active_id == KILN_CFG_NO_ACTIVE_ID) {
        APPEND("{\"active_id\":null,\"configs\":[");
    } else {
        APPEND("{\"active_id\":%ld,\"configs\":[", (long)active_id);
    }
    for (uint8_t i = 0; i < n; i++) {
        char name_escaped[KILN_CFG_NAME_MAX_LEN * 2 + 1];
        json_escape(rows[i].name, name_escaped, sizeof(name_escaped));
        APPEND("%s{\"id\":%ld,\"name\":\"%s\",\"is_active\":%s}", i == 0 ? "" : ",", (long)rows[i].id,
               name_escaped, rows[i].is_active ? "true" : "false");
    }
    APPEND("],\"max_count\":%u,\"pico_half_recapture_pending\":%s,\"has_data\":%s}",
           (unsigned)kiln_cfg_store_max_count(), kiln_cfg_store_pico_half_recapture_pending() ? "true" : "false",
           safety_cfg_store_has_data() ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, o);

overflow:
    /* Never send the truncated, malformed partial buffer as a 200 -- that
     * reads as success to a caller that only checks the status code. Log
     * once and fail loud with a 500 instead (same pattern as
     * diagnostics_http.c's crash_report/thermo_faults handlers). json[] is
     * sized generously already; do not enlarge it to "fix" this (house
     * rule: never enlarge httpd stack buffers). */
    ESP_LOGE(TAG, "kiln_configs list JSON overflowed %u-byte buffer at o=%u",
             (unsigned)sizeof(json), (unsigned)o);
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":false,\"error\":\"kiln configs list too large to encode\"}", HTTPD_RESP_USE_STRLEN);
#undef APPEND
}

/* ---- POST /api/kiln_configs/save -- name=<str> [id=<int>] ----------------- */
static esp_err_t save_post_handler(httpd_req_t *req)
{
    char body[KILN_CFG_BODY_MAX];
    if (!read_small_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing, too large, or read failed");
        return ESP_OK;
    }

    char name[KILN_CFG_NAME_MAX_LEN + 1];
    if (!parse_required_name(body, name, sizeof(name))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "name missing or too long");
        return ESP_OK;
    }
    int32_t id_or_negative = -1;
    {
        char id_val[16];
        int id_len = http_form_find_field(body, "id", id_val, sizeof(id_val));
        if (id_len > 0) {
            char *end = NULL;
            long v = strtol(id_val, &end, 10);
            if (end == id_val || *end != '\0' || v < 0 || v > INT32_MAX) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "id out of range");
                return ESP_OK;
            }
            id_or_negative = (int32_t)v;
        }
    }

    int32_t new_id = -1;
    char reason[96];
    reason[0] = '\0';
    if (!kiln_cfg_store_save_current(name, id_or_negative, &new_id, reason, sizeof(reason))) {
        httpd_resp_send_err(req,
                            kiln_cfg_store_reason_is_persist_failure(reason) ? HTTPD_500_INTERNAL_SERVER_ERROR
                                                                              : HTTPD_400_BAD_REQUEST,
                            reason[0] ? reason : "save failed");
        return ESP_OK;
    }

    /* 2026-09-15 review (review_divergence_check_561efa3b_2026-09-15.md,
     * LOW -- gate/warn on explicit save while diverged): an operator-
     * initiated "save over slot" here is NOT blocked by a config divergence
     * -- unlike the autosave path, this is a deliberate action the operator
     * chose to take, and refusing it would just make the divergence harder
     * to clear. But the operator should know the slot they just saved was
     * taken while the Pico's live safety config disagreed with what the ESP
     * expects, since that snapshot may not be what the Pico is actually
     * enforcing right now. ESP_LOGW so it is visible in the boot log even
     * if the web client never reads the response body's warning field. */
    /* 2026-09-15 review (review_divergence_fixes_b2e7017f_2026-09-15.md,
     * MEDIUM 6): `static` -- off this httpd worker's stack (esp_http_server's
     * HTTPD_DEFAULT_CONFIG() runs one worker task, so this handler is never
     * reentered, same convention as other single-caller statics in this
     * file/dashboard_status_http.c) -- shrinks this handler's already-LOW-
     * headroom stack frame instead of growing it, and lets divergence_reason
     * hold the FULL CONFIG_DIVERGENCE_REASON_MAX (160 B) reason instead of
     * the previous 96-byte buffer that silently truncated it. */
    static char divergence_reason[CONFIG_DIVERGENCE_REASON_MAX];
    bool diverged = safety_ceiling_sync_is_diverged(divergence_reason, sizeof(divergence_reason)) ||
                     safety_ceiling_sync_is_standing_diverged(divergence_reason, sizeof(divergence_reason));
    if (diverged) {
        ESP_LOGW(TAG,
                 "save_post_handler: kiln config slot %ld saved while the Pico's safety config is "
                 "diverged: %s",
                 (long)new_id, divergence_reason);
    }

    static char resp[CONFIG_DIVERGENCE_REASON_MAX * 2 + 96];
    int len;
    if (diverged) {
        static char reason_escaped[CONFIG_DIVERGENCE_REASON_MAX * 2 + 1];
        json_escape(divergence_reason, reason_escaped, sizeof(reason_escaped));
        len = snprintf(resp, sizeof(resp),
                       "{\"id\":%ld,\"warning\":\"saved while the safety processor's config is diverged: %s\"}",
                       (long)new_id, reason_escaped);
    } else {
        len = snprintf(resp, sizeof(resp), "{\"id\":%ld}", (long)new_id);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, len > 0 && (size_t)len < sizeof(resp) ? (size_t)len : strlen(resp));
}

/* ---- POST /api/kiln_configs/clone -- id=<int>, name=<str> ----------------- */
static esp_err_t clone_post_handler(httpd_req_t *req)
{
    char body[KILN_CFG_BODY_MAX];
    if (!read_small_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing, too large, or read failed");
        return ESP_OK;
    }

    int32_t src_id;
    if (!parse_required_id(body, "id", &src_id)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "id missing or out of range");
        return ESP_OK;
    }
    char name[KILN_CFG_NAME_MAX_LEN + 1];
    if (!parse_required_name(body, name, sizeof(name))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "name missing or too long");
        return ESP_OK;
    }

    int32_t new_id = -1;
    char reason[96];
    reason[0] = '\0';
    if (!kiln_cfg_store_clone(src_id, name, &new_id, reason, sizeof(reason))) {
        httpd_resp_send_err(req,
                            kiln_cfg_store_reason_is_persist_failure(reason) ? HTTPD_500_INTERNAL_SERVER_ERROR
                                                                              : HTTPD_400_BAD_REQUEST,
                            reason[0] ? reason : "clone failed");
        return ESP_OK;
    }

    char resp[64];
    int len = snprintf(resp, sizeof(resp), "{\"id\":%ld}", (long)new_id);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, len > 0 && (size_t)len < sizeof(resp) ? (size_t)len : strlen(resp));
}

/* ---- POST /api/kiln_configs/apply -- id=<int> ------------------------------
 *
 * The one route in this file that can rewrite a running kiln's relay/
 * thermocouple/PID/guard configuration. kiln_cfg_store_apply() itself now
 * enforces ota_http_check_interlocks() internally (the backstop -- see
 * kiln_cfg_store.h's SAFETY note), so the pre-check here is redundant
 * belt-and-suspenders, kept ONLY because it lets this handler return a
 * clean 409 with the refusal reason before ever calling into the store --
 * removing it would not reopen the hole, it would just make a refused
 * request's error path one function call longer. */
static esp_err_t apply_post_handler(httpd_req_t *req)
{
    char body[KILN_CFG_BODY_MAX];
    if (!read_small_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing, too large, or read failed");
        return ESP_OK;
    }
    int32_t id;
    if (!parse_required_id(body, "id", &id)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "id missing or out of range");
        return ESP_OK;
    }

    /* Existence BEFORE the interlock. An id that does not exist is a 404 no
     * matter what the kiln is doing, and answering "safety link is down"
     * for `id=999` sent the operator to diagnose a link that was not the
     * problem -- the same misdirection the delete route already avoids by
     * checking first. kiln_cfg_store_get_name() is the cheapest existence
     * probe the store exposes (false = no such id). */
    char exists_name[KILN_CFG_NAME_MAX_LEN + 1];
    if (!kiln_cfg_store_get_name(id, exists_name, sizeof(exists_name))) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such kiln config");
        return ESP_OK;
    }

    /* Owner decision Q2 (docs/SYSTEM_MODE_GATE_PLAN.md, 2026-09-25,
     * gate-slices-2/4/5 spec): refuse ALL zone/relay/guard config writes --
     * applying a saved kiln config is exactly that -- while a firing or
     * autotune run is active, PAUSED included. Checked HERE, right after the
     * 404 existence check and BEFORE the interlock below.
     *
     * Review fix, 2026-09-25: this used to run AFTER ota_http_check_
     * interlocks(), which already refuses (409 "a profile is running"/
     * "autotune is running") for the exact same facts -- with the mode gate
     * second, the interlock always answered first and this gate's own 409
     * body was dead code. Same ordering factory_reset.c already used (its
     * own test asserts !g_probe_interlock_called). Distinct 409 from the
     * interlock's own 428/409 below -- this refusal is not answerable with
     * an ack header, only by waiting for the run to end. */
    {
        sys_mode_snapshot_t mode_snap = { 0 };
        relay_authority_heat_run_active(&mode_snap.profile_running, &mode_snap.autotune_running);
        char mode_reason[SYSTEM_MODE_GATE_REASON_MAX];
        mode_reason[0] = '\0';
        if (system_mode_gate_check(SYS_ACTION_WRITE_ZONES_CONFIG, &mode_snap, mode_reason, sizeof(mode_reason))) {
            ESP_LOGW(TAG, "kiln config apply id=%ld refused by system mode gate: %s", (long)id, mode_reason);
            return system_mode_gate_http_send_refusal(req, mode_reason);
        }
    }

    /* Applying a saved configuration writes zone/guard settings; it streams
     * nothing over the safety link. So "the safety link is down" is
     * overridable here with the operator's per-request acknowledgement (see
     * ota_interlock.h's OTA_INTERLOCK_REFUSED_NEEDS_ACK), while a running
     * firing or a hot zone still refuses outright. kiln_cfg_store_apply()'s
     * own internal backstop is passed the same answer, below. */
    char reason[OTA_INTERLOCK_REASON_MAX];
    reason[0] = '\0';
    const bool ack = ota_http_req_ack_no_safety(req);
    ota_interlock_result_t gate = ota_http_check_interlocks(ack, reason, sizeof(reason));
    if (gate != OTA_INTERLOCK_OK) {
        ESP_LOGW(TAG, "kiln config apply id=%ld refused by interlock: %s", (long)id, reason);
        return ota_http_send_interlock_refusal(req, gate, reason);
    }

    /* Refuse while an http_async_job (ct_auto_zero's 10-15s measurement) is
     * running: kiln_cfg_swap_apply() pushes K_CT 0x0308-0x030A via
     * safety_cfg_write_apply_package_and_confirm(), the same commit path the
     * job's own COMMIT_CONFIG uses, so letting both run at once risks one
     * clobbering the other's write (2026-09-25 fix-then-push re-review). Set
     * explicitly, same as the apply-in-flight 409 a few lines below -- no
     * HTTPD_409_CONFLICT enumerator in this esp_http_server. */
    if (http_async_job_busy()) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "another commissioning operation is running");
    }

    /* Section 5.3 table row 4, checked HERE rather than inside the apply.
     *
     * This gate used to be a parameter of kiln_cfg_store_apply() (the
     * ESP-only path this handler called before item 5 landed).
     * kiln_cfg_swap_apply() -- the two-processor transaction this handler
     * now dispatches -- has no ack_hardware_differs parameter, so routing
     * the apply through it without this block would silently drop the gate:
     * it would still exist in kiln_cfg_store_apply() with nothing left
     * reaching it. kiln_cfg_store_slot_hardware_differs() is the same
     * predicate that gate always used, exported rather than re-implemented
     * so the compared field list keeps exactly one definition. Refused with
     * 428 (not 409) for the same reason the no-safety-processor interlock
     * uses it: this is a refusal the operator CAN answer, by re-sending
     * with the ack header -- which is exactly what the page's
     * "hardware differs" checkbox does. */
    if (!req_ack_hardware_differs(req)) {
        char hw_msg[256];
        if (kiln_cfg_store_slot_hardware_differs(id, hw_msg, sizeof(hw_msg))) {
            ESP_LOGW(TAG, "kiln config apply id=%ld refused, hardware shape differs: %s", (long)id, hw_msg);
            httpd_resp_set_status(req, "428 Precondition Required");
            return httpd_resp_sendstr(req, hw_msg);
        }
    }

    /* Item 5: DISPATCHED, never run here. kiln_cfg_swap_apply() performs a
     * 60+ round-trip UART exchange with the safety processor plus a flash
     * write, and stack-allocates ~1.7 kB; running it on this httpd worker
     * would blow an 8192 B stack already measured at a 4832 B ceiling and
     * wedge every other page on the board for the duration. See
     * kiln_cfg_swap_worker.h. The interlock pre-check above is therefore no
     * longer redundant belt-and-braces: it is the only thing that can turn
     * an interlock refusal into a real HTTP status code, since the worker's
     * own check runs after this handler has already answered. */
    /* 96, NOT KILN_CFG_SWAP_REASON_MAX (200), and deliberately the same size
     * this handler always used. This buffer is on the shared httpd worker
     * stack, whose honest free margin is already classified LOW; the only
     * reasons that reach it are the submit-side refusals ("an apply is
     * already running"), which are far shorter than 96. The long reasons --
     * the ones actually sized for KILN_CFG_SWAP_REASON_MAX -- come from the
     * transaction itself and are read back from apply_status_get_handler(),
     * which keeps its buffers `static` for exactly this reason. */
    char apply_reason[96];
    apply_reason[0] = '\0';
    if (!kiln_cfg_swap_worker_submit(id, ack, apply_reason, sizeof(apply_reason))) {
        /* Set explicitly rather than via httpd_resp_send_err(): esp_http_server
         * has no HTTPD_409_CONFLICT enumerator. 409 is the right code here --
         * a refusal the operator cannot argue with (an apply is already in
         * flight), unlike the 428 above, which they CAN answer with the ack
         * header. */
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, apply_reason[0] ? apply_reason : "apply could not start");
    }
    /* 202, not 200: the swap has been accepted and is running, and is NOT
     * finished when this returns. A caller that reported "applied" off this
     * response alone would be claiming an outcome nothing has confirmed --
     * the exact "logging unchecked success" shape this repo already has a
     * name for. GET /api/kiln_configs/apply_status is where the real
     * outcome comes from. */
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true,\"state\":\"running\"}");
}

/* ---- GET /api/kiln_configs/apply_status ------------------------------------
 *
 * The outcome half of the asynchronous apply above. Reports the worker's
 * state enum, the target id, whether a failure left the board DIVERGED as
 * opposed to merely refused with nothing changed, and the board's own
 * refusal text verbatim. 2026-09-22 fix: `diverged==true` alone does NOT
 * mean heaters were disabled -- only the ceiling-latch divergence branch in
 * kiln_cfg_swap.c does that; four other DIVERGED paths (rollback failures)
 * disable nothing (docs/audits/kiln_config_self_apply_diverged_2026-09-22.md
 * sec 4). `reason` is the honest source for what actually happened; do not
 * generically claim heat-off from `diverged` here. */
static esp_err_t apply_status_get_handler(httpd_req_t *req)
{
    kiln_cfg_swap_job_state_t state = KILN_CFG_SWAP_JOB_IDLE;
    int32_t target_id = -1;
    bool diverged = false;
    char reason[KILN_CFG_SWAP_REASON_MAX];
    kiln_cfg_swap_worker_get_status(&state, &target_id, &diverged, reason, sizeof(reason));

    const char *state_str = "idle";
    switch (state) {
    case KILN_CFG_SWAP_JOB_RUNNING:     state_str = "running";     break;
    case KILN_CFG_SWAP_JOB_DONE_OK:     state_str = "done_ok";     break;
    case KILN_CFG_SWAP_JOB_DONE_FAILED: state_str = "done_failed"; break;
    case KILN_CFG_SWAP_JOB_IDLE:
    default:                            state_str = "idle";        break;
    }

    /* Escaped: `reason` can carry a slot name that came from a POST body at
     * some point, same reasoning as this file's other json_escape() uses.
     * Sized against KILN_CFG_SWAP_REASON_MAX (200) doubled for worst-case
     * escaping, plus the fixed envelope -- a `static` buffer, deliberately,
     * to keep it off the shared 8192 B httpd worker stack (the same
     * discipline save_post_handler()'s own divergence_reason buffer uses). */
    static char escaped[KILN_CFG_SWAP_REASON_MAX * 2 + 8];
    json_escape(reason, escaped, sizeof(escaped));

    static char out[KILN_CFG_SWAP_REASON_MAX * 2 + 128];
    int n = snprintf(out, sizeof(out), "{\"state\":\"%s\",\"id\":%ld,\"diverged\":%s,\"reason\":\"%s\"}",
                     state_str, (long)target_id, diverged ? "true" : "false", escaped);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, out, n > 0 && (size_t)n < sizeof(out) ? (size_t)n : strlen(out));
}

/* ---- POST /api/kiln_configs/delete -- id=<int> ---------------------------- */
static esp_err_t delete_post_handler(httpd_req_t *req)
{
    char body[KILN_CFG_BODY_MAX];
    if (!read_small_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing, too large, or read failed");
        return ESP_OK;
    }
    int32_t id;
    if (!parse_required_id(body, "id", &id)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "id missing or out of range");
        return ESP_OK;
    }
    /* H5 fix (docs/audits/kiln_profiles_robustness_2026-09-14.md):
     * kiln_cfg_store_delete() now carries its own backstop interlock and
     * refuses to delete the active config -- same ack-passthrough pattern
     * apply_post_handler() above already uses. A pre-check here would be
     * redundant, not load-bearing (the store's own check is the backstop),
     * but the store no longer returns a bare false for "not found" either,
     * so distinguish that case for the right HTTP status. */
    const bool ack = ota_http_req_ack_no_safety(req);
    char delete_reason[96];
    delete_reason[0] = '\0';
    if (!kiln_cfg_store_delete(id, ack, delete_reason, sizeof(delete_reason))) {
        if (strcmp(delete_reason, "no saved kiln config with that id") == 0) {
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such kiln config");
        } else {
            httpd_resp_send_err(req,
                                kiln_cfg_store_reason_is_persist_failure(delete_reason)
                                    ? HTTPD_500_INTERNAL_SERVER_ERROR
                                    : HTTPD_400_BAD_REQUEST,
                                delete_reason[0] ? delete_reason : "delete failed");
        }
        return ESP_OK;
    }
    return httpd_resp_sendstr(req, "ok");
}

/* ---- POST /api/kiln_configs/rename -- id=<int>, name=<str> ---------------- */
static esp_err_t rename_post_handler(httpd_req_t *req)
{
    char body[KILN_CFG_BODY_MAX];
    if (!read_small_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing, too large, or read failed");
        return ESP_OK;
    }
    int32_t id;
    if (!parse_required_id(body, "id", &id)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "id missing or out of range");
        return ESP_OK;
    }
    char name[KILN_CFG_NAME_MAX_LEN + 1];
    if (!parse_required_name(body, name, sizeof(name))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "name missing or too long");
        return ESP_OK;
    }
    /* Pre-check purely for a specific message -- same "pre-check for a
     * better-worded error, backed by the store's own authoritative check"
     * pattern the apply handler uses for the interlock. `id` is excluded so
     * a no-op rename (or a rename that only changes case/whitespace of the
     * name this entry already has) is not reported as a collision.
     * kiln_cfg_store_rename() below enforces the same rule itself -- this
     * cannot be bypassed by this pre-check being wrong or skipped. */
    if (kiln_cfg_store_name_would_collide(name, id)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "a saved kiln config already has that name");
        return ESP_OK;
    }
    char rename_reason[96];
    rename_reason[0] = '\0';
    if (!kiln_cfg_store_rename_ex(id, name, rename_reason, sizeof(rename_reason))) {
        if (kiln_cfg_store_reason_is_persist_failure(rename_reason)) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, rename_reason);
        } else {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no such kiln config, or name invalid");
        }
        return ESP_OK;
    }
    return httpd_resp_sendstr(req, "ok");
}

/* ---- GET /api/kiln_configs/export?id=N -- docs/KILN_PROFILES_PLAN.md
 * items 3/9. Streams one slot as the section 5.1 JSON envelope with a
 * download filename, so the browser's own "Save As" offers a sensible name
 * instead of the route path. HEAP buffer only (KILN_CFG_EXPORT_JSON_MAX_LEN
 * -- up to ~6 KB) -- never stack, per this feature's own httpd-stack rule
 * (project_httpd_stack_blob_class). */
static esp_err_t export_get_handler(httpd_req_t *req)
{
    char query[32];
    char id_str[16];
    int32_t id;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "id", id_str, sizeof(id_str)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "id query parameter missing");
        return ESP_OK;
    }
    char *end = NULL;
    long v = strtol(id_str, &end, 10);
    if (end == id_str || *end != '\0' || v < 0 || v > INT32_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "id out of range");
        return ESP_OK;
    }
    id = (int32_t)v;

    char *json = (char *)heap_caps_malloc(KILN_CFG_EXPORT_JSON_MAX_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }
    size_t len = 0;
    char reason[160];
    reason[0] = '\0';
    if (!kiln_cfg_store_export_package_json(id, json, KILN_CFG_EXPORT_JSON_MAX_LEN, &len, reason,
                                            sizeof(reason))) {
        free(json);
        /* "no saved kiln config with that id" is the only 404-shaped
         * reason this call can produce; anything else (half-package, too
         * large) is a 400. */
        if (strstr(reason, "no saved kiln config")) {
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, reason[0] ? reason : "no such kiln config");
        } else {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, reason[0] ? reason : "export failed");
        }
        return ESP_OK;
    }

    char name[KILN_CFG_NAME_MAX_LEN + 1];
    kiln_cfg_store_get_name(id, name, sizeof(name));
    char disp[80];
    snprintf(disp, sizeof(disp), "attachment; filename=\"kiln_%ld.kilnpkg.json\"", (long)id);
    (void)name; /* the filename uses the id, not the (unescaped) operator name, to stay a safe HTTP header value */
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    esp_err_t err = httpd_resp_send(req, json, len);
    free(json);
    return err;
}

/* ---- POST /api/kiln_configs/import -- docs/KILN_PROFILES_PLAN.md items
 * 4/9/14. Body is a section 5.1 package JSON. Creates a NEW slot only, never
 * applies (section 5.3) -- HEAP body buffer (PSRAM preferred), same shape
 * as backup_import.c's own upload handler. */
#define KILN_CFG_IMPORT_BODY_MAX (KILN_CFG_EXPORT_JSON_MAX_LEN + 512u)

static esp_err_t import_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || (size_t)req->content_len > KILN_CFG_IMPORT_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }
    char *body = (char *)heap_caps_malloc((size_t)req->content_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int ret = httpd_req_recv(req, body + received, (size_t)req->content_len - received);
        if (ret <= 0) {
            free(body);
            ESP_LOGW(TAG, "kiln config import body read failed/short: %d", ret);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "upload incomplete or connection dropped");
            return ESP_OK;
        }
        received += (size_t)ret;
    }
    body[received] = '\0';

    int32_t new_id = -1;
    char reason[200];
    reason[0] = '\0';
    bool ok = kiln_cfg_store_import_package_json(body, &new_id, reason, sizeof(reason));
    free(body);

    if (!ok) {
        char resp[256];
        json_escape(reason[0] ? reason : "upload refused", resp, sizeof(resp) - 32);
        char out[288];
        int n = snprintf(out, sizeof(out), "{\"error\":\"%s\"}", resp);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, out, n > 0 && (size_t)n < sizeof(out) ? (size_t)n : strlen(out));
    }

    char resp[64];
    int len = snprintf(resp, sizeof(resp), "{\"id\":%ld}", (long)new_id);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, len > 0 && (size_t)len < sizeof(resp) ? (size_t)len : strlen(resp));
}

/* ---- POST /api/kiln_configs/quarantine_clear -- confirm=1 -----------------
 *
 * The one way out of a quarantined store (kiln_cfg_store.c's
 * set_quarantine()/kiln_cfg_store_is_quarantined()): every save/clone/
 * rename/delete is refused until this runs. Discards whatever bytes could
 * not be read and starts a fresh, empty store -- kiln_cfg_store_quarantine_
 * clear()'s own doc comment. Requires an explicit confirm=1 form field (400
 * without it, mirroring every other confirm-gated write in this codebase);
 * a store that is not actually quarantined is a 409, not a silent no-op,
 * since a caller expecting to clear something should know nothing needed
 * clearing. The quarantine check runs BEFORE the confirm gate, deliberately
 * -- a POST with no/blank confirm still distinguishes "409, not quarantined,
 * nothing to clear" from "400, quarantined, confirm=1 required", so a
 * caller (a PC-side status probe, in particular) can learn the store's
 * quarantine state with a non-mutating request instead of having to guess
 * or duplicate kiln_cfg_store_is_quarantined()'s own logic client-side. */
static esp_err_t quarantine_clear_post_handler(httpd_req_t *req)
{
    char body[KILN_CFG_BODY_MAX];
    if (!read_small_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing, too large, or read failed");
        return ESP_OK;
    }

    char quarantine_reason[128];
    quarantine_reason[0] = '\0';
    if (!kiln_cfg_store_is_quarantined(quarantine_reason, sizeof(quarantine_reason))) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "kiln config store is not quarantined -- nothing to clear");
    }

    char confirm_val[4];
    int confirm_len = http_form_find_field(body, "confirm", confirm_val, sizeof(confirm_val));
    if (confirm_len <= 0 || strcmp(confirm_val, "1") != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "confirm=1 required to discard the quarantined store");
        return ESP_OK;
    }

    char reason[128];
    reason[0] = '\0';
    bool ok = kiln_cfg_store_quarantine_clear(true, reason, sizeof(reason));
    if (!ok) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, reason[0] ? reason : "quarantine clear failed");
        return ESP_OK;
    }

    char resp[256];
    char reason_escaped[224];
    json_escape(quarantine_reason, reason_escaped, sizeof(reason_escaped));
    int len = snprintf(resp, sizeof(resp), "{\"ok\":true,\"discarded_reason\":\"%s\"}", reason_escaped);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, len > 0 && (size_t)len < sizeof(resp) ? (size_t)len : strlen(resp));
}

esp_err_t kiln_cfg_http_start(void)
{
    httpd_handle_t server = wifi_provision_http_get_server();
    if (!server) {
        ESP_LOGE(TAG, "no HTTP server -- wifi_provision_http_start() must run first");
        return ESP_ERR_INVALID_STATE;
    }

    static const httpd_uri_t page_uri = {
        .uri = "/settings/kiln_configs", .method = HTTP_GET, .handler = kiln_configs_page_get_handler,
    };
    static const httpd_uri_t list_uri = {
        .uri = "/api/kiln_configs", .method = HTTP_GET, .handler = list_get_handler,
    };
    static const httpd_uri_t save_uri = {
        .uri = "/api/kiln_configs/save", .method = HTTP_POST, .handler = save_post_handler,
    };
    static const httpd_uri_t clone_uri = {
        .uri = "/api/kiln_configs/clone", .method = HTTP_POST, .handler = clone_post_handler,
    };
    static const httpd_uri_t apply_uri = {
        .uri = "/api/kiln_configs/apply", .method = HTTP_POST, .handler = apply_post_handler,
    };
    static const httpd_uri_t apply_status_uri = {
        .uri = "/api/kiln_configs/apply_status", .method = HTTP_GET, .handler = apply_status_get_handler,
    };
    static const httpd_uri_t delete_uri = {
        .uri = "/api/kiln_configs/delete", .method = HTTP_POST, .handler = delete_post_handler,
    };
    static const httpd_uri_t rename_uri = {
        .uri = "/api/kiln_configs/rename", .method = HTTP_POST, .handler = rename_post_handler,
    };
    static const httpd_uri_t export_uri = {
        .uri = "/api/kiln_configs/export", .method = HTTP_GET, .handler = export_get_handler,
    };
    static const httpd_uri_t import_uri = {
        .uri = "/api/kiln_configs/import", .method = HTTP_POST, .handler = import_post_handler,
    };
    static const httpd_uri_t quarantine_clear_uri = {
        .uri = "/api/kiln_configs/quarantine_clear", .method = HTTP_POST, .handler = quarantine_clear_post_handler,
    };

    const httpd_uri_t *uris[] = { &page_uri,   &list_uri,   &save_uri,   &clone_uri,  &apply_uri,
                                 &apply_status_uri, &delete_uri, &rename_uri, &export_uri, &import_uri,
                                 &quarantine_clear_uri };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        esp_err_t err = kiln_http_register(server, uris[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "httpd_register_uri_handler(%s) failed: %s", uris[i]->uri, esp_err_to_name(err));
            return err;
        }
    }

    ESP_LOGI(TAG, "kiln config API up (max_count=%u)", (unsigned)kiln_cfg_store_max_count());
    return ESP_OK;
}
