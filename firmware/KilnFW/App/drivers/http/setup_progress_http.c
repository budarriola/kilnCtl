#include "setup_progress_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "http_auth_http.h" // kiln_http_register() -- WEB_AUTH_PLAN.md section 5
#include "esp_log.h"

#include "cfg_fs_refusal_http.h" // cfg_fs_http_refuse_if_unmounted(), cfg_fs_http_persist_failed()
#include "http_form.h"
#include "setup_wizard_progress.h"
#include "wifi_provision_http.h"

static const char *TAG = "setup_progress_http";

/* API-level version number in the GET response body -- deliberately its own
 * constant, separate from setup_wizard_progress.c's internal NVS blob
 * schema version (SETUP_WIZARD_PROGRESS_VERSION). The two are allowed to
 * diverge: the on-disk layout can gain a migration without the wire shape
 * this endpoint promises to callers changing at all. Bump this only if the
 * JSON shape itself changes. */
#define SETUP_WIZARD_PROGRESS_API_VERSION 1u

/* Escapes '"'/'\\' only -- notes are operator-typed free text (a skip
 * reason), same minimal escaping readiness_http.c's json_escape() applies to
 * its own free-text detail strings. */
static void json_escape(const char *src, char *out, size_t out_cap)
{
    size_t o = 0;
    for (const char *p = src; *p && o + 2 < out_cap; p++) {
        if (*p == '"' || *p == '\\') {
            if (o + 3 >= out_cap) break;
            out[o++] = '\\';
        }
        out[o++] = *p;
    }
    out[o] = '\0';
}

/* json_cap sizing (recomputed 2026-09-18 when SETUP_WIZARD_STEP_COUNT grew
 * 13 -> 14, and again 2026-09-19 when it went back to 13 -- see
 * setup_wizard_progress.h's header comment on the "Coupling matrix
 * (optional)" step's removal): each row's worst case is `,"12":{"state":
 * "skipped","ts":4294967295,"note":"<62-char escaped note>"}` = 113 bytes
 * (index up to 2 digits, longest state name "skipped", ts at UINT32_MAX,
 * note escaped to its full 2*(SETUP_WIZARD_NOTE_MAX-1)=62-byte worst case),
 * so 13 such rows plus the 22-byte `{"version":1,"steps":{` prefix and
 * 2-byte `}}` suffix, minus one byte since the first row has no leading
 * comma, comes to 1492 bytes -- comfortably under SETUP_PROGRESS_JSON_CAP=
 * 2048 with 556 bytes of margin (more margin than before the removal, since
 * fewer rows are now emitted; the buffer size itself did not change). 2048
 * must NOT be grown to make a future step count fit; if a future
 * SETUP_WIZARD_STEP_COUNT no longer fits with margin, that is a sign the
 * wizard has grown too large for this endpoint's shape, not a reason to
 * enlarge this buffer.
 *
 * Shape: {"version":N,"steps":{"0":{"state":...,"ts":...,"note":...},...}}
 * -- steps keyed by string index (object, not array) to match the
 * setup-page shell's already-landed contract (setup_wizard_page.html's
 * defaultProgress()/mergeAllSteps(), docs/SETUP_WIZARD.md section 5
 * item 2's own "{version, per-step {state, ts, note}}" wording). */
#define SETUP_PROGRESS_JSON_CAP 2048

/* 2026-09-08 hardware verification (httpd_worker measured 632 B free of
 * 8192 B, down from a 1728 B pre-flash baseline): this handler's locals used
 * to live directly on the httpd task stack -- a SETUP_WIZARD_STEP_COUNT-entry
 * setup_wizard_step_t array plus the full 2048-byte json[] buffer, ~2.7 KB in
 * this frame alone, on the same shared task every other handler in this
 * codebase runs on (see CLAUDE.md's "httpd stack" note). Moved to the heap,
 * same request-scoped-malloc-then-free-on-every-return-path convention as
 * diagnostics_http.c's cfgfs_status_get_handler. malloc() failure is reported
 * as 500, never a truncated/garbage response. */
typedef struct {
    setup_wizard_step_t steps[SETUP_WIZARD_STEP_COUNT];
    char json[SETUP_PROGRESS_JSON_CAP];
} setup_progress_get_scratch_t;

static esp_err_t api_setup_progress_get_handler(httpd_req_t *req)
{
    setup_progress_get_scratch_t *s = malloc(sizeof(*s));
    if (!s) {
        ESP_LOGE(TAG, "api_setup_progress_get_handler: malloc(%u) failed", (unsigned)sizeof(*s));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }

    setup_wizard_progress_get_all(s->steps);

    size_t o = 0;
    int n = snprintf(s->json, sizeof(s->json), "{\"version\":%u,\"steps\":{",
                      (unsigned)SETUP_WIZARD_PROGRESS_API_VERSION);
    if (n < 0 || (size_t)n >= sizeof(s->json)) {
        free(s);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "internal error");
        return ESP_OK;
    }
    o = (size_t)n;

    for (uint8_t i = 0; i < SETUP_WIZARD_STEP_COUNT; i++) {
        char note_esc[2 * SETUP_WIZARD_NOTE_MAX];
        json_escape(s->steps[i].note, note_esc, sizeof(note_esc));
        n = snprintf(s->json + o, sizeof(s->json) - o,
                     "%s\"%u\":{\"state\":\"%s\",\"ts\":%u,\"note\":\"%s\"}", i == 0 ? "" : ",", (unsigned)i,
                     setup_wizard_step_state_name(s->steps[i].state), (unsigned)s->steps[i].ts, note_esc);
        if (n < 0 || (size_t)n >= sizeof(s->json) - o) {
            /* Still unreachable at SETUP_WIZARD_STEP_COUNT=13 against a
             * 2048-byte buffer (recomputed worst case 1492 bytes, see the
             * SETUP_PROGRESS_JSON_CAP comment above) -- re-verified, not
             * merely re-asserted, each time the count has changed (13->14 on
             * 2026-09-18, back to 13 on 2026-09-19). Never ship a truncated
             * JSON document silently -- same discipline as readiness_http.c's
             * own overflow guard. */
            ESP_LOGE(TAG, "setup progress JSON did not fit SETUP_PROGRESS_JSON_CAP=%d", SETUP_PROGRESS_JSON_CAP);
            free(s);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "internal error");
            return ESP_OK;
        }
        o += (size_t)n;
    }

    n = snprintf(s->json + o, sizeof(s->json) - o, "}}");
    if (n < 0 || (size_t)n >= sizeof(s->json) - o) {
        free(s);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "internal error");
        return ESP_OK;
    }
    o += (size_t)n;

    httpd_resp_set_type(req, "application/json");
    esp_err_t send_err = httpd_resp_send(req, s->json, o);
    free(s);
    return send_err;
}

/* POST /api/setup/progress -- form body "step=<0-12>&state=pending|done|skipped[&note=...]",
 * same bounded-body-then-validate-then-commit shape as every other settings
 * POST in this codebase (settings_http.c's settings_tz_post_handler). One
 * step per call, matching docs/SETUP_WIZARD.md section 5 point 6:
 * "[w]rites stay per-step ... There is no global 'commit everything at the
 * end'". */
#define SETUP_PROGRESS_BODY_MAX 192

static esp_err_t api_setup_progress_post_handler(httpd_req_t *req)
{
    /* The progress record's only persistence target is the cfg file
     * (docs/CONFIG_FILESYSTEM.md, "Dual-write window: closed"): refuse before
     * touching state when it is not mounted. */
    if (cfg_fs_http_refuse_if_unmounted(req)) {
        return ESP_OK;
    }

    if (req->content_len <= 0 || req->content_len > SETUP_PROGRESS_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }

    char body[SETUP_PROGRESS_BODY_MAX + 1];
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body read failed");
            return ESP_OK;
        }
        received += (size_t)ret;
    }
    body[received] = '\0';

    char step_val[8];
    int step_len = http_form_find_field(body, "step", step_val, sizeof(step_val));
    char state_val[16];
    int state_len = http_form_find_field(body, "state", state_val, sizeof(state_val));
    if (step_len <= 0 || state_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "step and state are both required");
        return ESP_OK;
    }

    char *endp = NULL;
    long step_num = strtol(step_val, &endp, 10);
    if (endp == step_val || *endp != '\0' || step_num < 0 || step_num >= SETUP_WIZARD_STEP_COUNT) {
        /* "unknown-step rejection": a step number outside the valid range is
         * refused here at the HTTP boundary AND, defense in depth, again by
         * setup_wizard_progress_set_step() itself below. */
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "step out of range");
        return ESP_OK;
    }

    setup_wizard_step_state_t state;
    if (!setup_wizard_step_state_from_name(state_val, &state)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "state must be pending, done, or skipped");
        return ESP_OK;
    }

    /* Contract (setup_wizard_progress.h): a too-long note is truncated, never rejected. */
    char note_val[SETUP_WIZARD_NOTE_MAX];
    if (!setup_wizard_progress_note_from_form(body, note_val, sizeof(note_val))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "note too long or contains a control character");
        return ESP_OK;
    }
    const char *note = (note_val[0] != '\0') ? note_val : NULL;

    esp_err_t err = setup_wizard_progress_set_step((uint8_t)step_num, state, note);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "setup_wizard_progress_set_step(%ld) failed: %s", step_num, esp_err_to_name(err));
        return cfg_fs_http_persist_failed(req);
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

esp_err_t setup_progress_http_start(void)
{
    esp_err_t start_err = setup_wizard_progress_start();
    if (start_err != ESP_OK) {
        ESP_LOGW(TAG, "setup_wizard_progress_start failed: %s -- progress stays at defaults this boot",
                 esp_err_to_name(start_err));
    }

    httpd_handle_t server = wifi_provision_http_get_server();
    if (!server) {
        ESP_LOGE(TAG, "no HTTP server -- wifi_provision_http_start() must run first");
        return ESP_ERR_INVALID_STATE;
    }

    static const httpd_uri_t get_uri = {
        .uri = "/api/setup/progress", .method = HTTP_GET, .handler = api_setup_progress_get_handler,
    };
    esp_err_t err = kiln_http_register(server, &get_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(GET /api/setup/progress) failed: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t post_uri = {
        .uri = "/api/setup/progress", .method = HTTP_POST, .handler = api_setup_progress_post_handler,
    };
    err = kiln_http_register(server, &post_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(POST /api/setup/progress) failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "setup wizard progress API up");
    return ESP_OK;
}
