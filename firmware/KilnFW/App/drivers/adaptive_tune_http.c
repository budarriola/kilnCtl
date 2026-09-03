#include "adaptive_tune_http.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_http_server.h"

#include "adaptive_tune.h"
#include "http_form.h"
#include "profile_executor.h" // MAX31856_CHANNEL_COUNT
#include "wifi_provision_http.h"

static const char *TAG = "adaptive_tune_http";

/* Memory budget: ONE status-response buffer, allocated from PSRAM
 * (MALLOC_CAP_SPIRAM), never the request handler's own internal-DRAM stack
 * -- same discipline log_http.c documents, following the internal-DRAM-
 * exhaustion incident that truncated /app.js (see project memory "ESP
 * internal DRAM exhaustion" and the documented httpd stack near-overflow,
 * 64 bytes free under real load).
 *
 * The prior "320 bytes/zone, generously" comment here was never measured
 * against the actual snprintf() below and was wrong: at MAX31856_CHANNEL_
 * COUNT==3 the real per-zone object is exactly 778 bytes at its worst case
 * (measured by this file's host test, render_worst_case_adaptive_tune_
 * json() in test_adaptive_tune_http.c, which builds the widest value every
 * %-spec below can produce and asserts this constant against it) --
 * against a 320-byte/zone budget that clamped and served 1023 bytes of
 * truncated JSON to /api/adaptive_tune, invalid JSON that hung the zones
 * page's Continuous Tuning panel on "Loading...". The 778-byte figure comes
 * from: three char[96] reason strings (last_refusal_reason/coupled_
 * refusal_reason/ki_refusal_reason, adaptive_tune.h) each at their full
 * 95-char capacity, every %.4f/%.2f float at a 6-digit-plus-sign-plus-
 * decimals worst case, every %u at its type's max (uint32_t observation
 * counts print up to 10 digits), plus the ~424 literal JSON bytes
 * (field names/punctuation) the format string itself contributes.
 *
 * Budgeted at 900 bytes/zone (headroom: 900*3+64=2764 vs the measured
 * 10+3*778+2=2346 needed for the full array at 3 zones -- 418 bytes,
 * ~15%, well above the 50-byte minimum margin test_dashboard_json.c's
 * sibling test enforces for the analogous /api/status buffer) plus a
 * 64-byte fixed allowance for the "{"zones":[" / "]}" wrapper. Still PSRAM
 * (MALLOC_CAP_SPIRAM), not internal DRAM -- 2764 bytes is trivialy inside
 * this board's PSRAM budget and does not touch the scarce internal-DRAM
 * pool at all, so the internal-DRAM-exhaustion risk this comment opened
 * with is unaffected by this file's growth. The enable-POST body buffer
 * below stays a small fixed stack array (<=65 bytes) -- consistent with the
 * tiny form-body buffers already used elsewhere in this codebase (e.g.
 * adaptive_tune.c's own prior version of this handler, settings_http.c) and
 * far below anything that would threaten the documented near-overflow
 * margin. */
#define ADAPTIVE_TUNE_STATUS_BUF_BYTES (900 * MAX31856_CHANNEL_COUNT + 64)

static esp_err_t status_get_handler(httpd_req_t *req)
{
    char *buf = (char *)heap_caps_malloc(ADAPTIVE_TUNE_STATUS_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }

    size_t off = 0;
    int n = snprintf(buf + off, ADAPTIVE_TUNE_STATUS_BUF_BYTES - off, "{\"zones\":[");
    if (n < 0 || (size_t)n >= ADAPTIVE_TUNE_STATUS_BUF_BYTES - off) {
        // Can't happen with the fixed literal above and any sane buffer size, but
        // treat it exactly like the per-zone check below: fail loudly, never
        // serve a truncated body -- see the loop's own comment on why.
        ESP_LOGE(TAG, "adaptive_tune status buffer too small for even the fixed preamble (%d)", n);
        free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "response too large");
        return ESP_OK;
    }
    off += (size_t)n;
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        adaptive_tune_zone_status_t st;
        adaptive_tune_get_status(zi, &st);
        n = snprintf(
            buf + off, ADAPTIVE_TUNE_STATUS_BUF_BYTES - off,
            "%s{\"zone\":%u,\"enabled\":%s,\"observation_count\":%u,\"observations_lifetime\":%u,"
            "\"has_applied\":%s,\"prior_k_dc\":%.4f,\"applied_k_dc\":%.4f,\"delta_pct\":%.2f,"
            "\"last_profile_id\":%u,\"last_applied_unix_s\":%u,\"refusal\":\"%s\","
            // Full coupled identification (PID_EXPANSION_PLAN.md 3.3, layer 2) --
            // joint_observations is module-wide (same number on every zone), the rest
            // is per-zone (this zone's row of coupling_coeff[]).
            "\"joint_observations\":%u,\"coupled_attempted\":%s,\"coupled_applied\":%s,"
            "\"coupled_cells_changed\":%u,\"coupled_refusal\":\"%s\","
            // Integral (Ki) diagnosis from dwells (same section, layer 2).
            "\"ki_verdict\":%u,\"ki_correction_pct\":%.2f,\"ki_applied\":%s,\"ki_refusal\":\"%s\","
            "\"revert_available\":%s}",
            zi == 0 ? "" : ",", (unsigned)zi, st.enabled ? "true" : "false", (unsigned)st.ring_count,
            (unsigned)st.observations_lifetime, st.has_applied ? "true" : "false", (double)st.prior_k_dc,
            (double)st.applied_k_dc, (double)st.last_delta_pct, (unsigned)st.last_applied_profile_id,
            (unsigned)st.last_applied_unix_s, st.last_refusal_reason,
            (unsigned)st.joint_observations, st.coupled_attempted ? "true" : "false",
            st.coupled_applied ? "true" : "false", (unsigned)st.coupled_cells_changed, st.coupled_refusal_reason,
            (unsigned)st.ki_verdict, (double)st.ki_correction_pct, st.ki_applied ? "true" : "false",
            st.ki_refusal_reason, st.revert_available ? "true" : "false");
        // Never silently truncate: a client cannot distinguish a truncated body
        // from a corrupt one, and this exact defect (the 320-byte/zone budget
        // clamping mid-key, on the wire as 1023 bytes of invalid JSON) hid
        // behind /api/adaptive_tune looking like a permanently-loading fetch
        // rather than a visible failure. Fail loudly instead: free the buffer
        // and answer 500, no body written.
        if (n < 0 || (size_t)n >= ADAPTIVE_TUNE_STATUS_BUF_BYTES - off) {
            ESP_LOGE(TAG, "adaptive_tune status buffer too small at zone %u (need %d, have %u left)", (unsigned)zi,
                      n, (unsigned)(ADAPTIVE_TUNE_STATUS_BUF_BYTES - off));
            free(buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "response too large");
            return ESP_OK;
        }
        off += (size_t)n;
    }
    n = snprintf(buf + off, ADAPTIVE_TUNE_STATUS_BUF_BYTES - off, "]}");
    if (n < 0 || (size_t)n >= ADAPTIVE_TUNE_STATUS_BUF_BYTES - off) {
        ESP_LOGE(TAG, "adaptive_tune status buffer too small for closing brace (%d)", n);
        free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "response too large");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, buf);
    free(buf);
    return err;
}

#define ADAPTIVE_TUNE_ENABLE_BODY_MAX 64

static esp_err_t enable_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > ADAPTIVE_TUNE_ENABLE_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }
    char body[ADAPTIVE_TUNE_ENABLE_BODY_MAX + 1];
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

    char zone_val[8], en_val[8];
    int zone_len = http_form_find_field(body, "zone", zone_val, sizeof(zone_val));
    int en_len = http_form_find_field(body, "enabled", en_val, sizeof(en_val));
    if (zone_len <= 0 || en_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "zone and enabled fields required");
        return ESP_OK;
    }
    int zone = atoi(zone_val);
    bool enabled = (atoi(en_val) != 0);
    if (zone < 0 || zone >= MAX31856_CHANNEL_COUNT) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "zone out of range");
        return ESP_OK;
    }

    bool saved = adaptive_tune_set_enabled((uint8_t)zone, enabled);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, saved ? "{\"ok\":true}" : "{\"ok\":true,\"warning\":\"applied live, save failed\"}");
}

// U1: one-click revert -- POST /api/adaptive_tune/revert, body "zone=N"
// (same tiny form-body convention as enable_post_handler() above). Runs on
// httpd_worker, same task/flash-safety posture as every other handler in
// this file -- see adaptive_tune_revert()'s own comment for why it is safe
// to call its zones_config_set_*() writes directly from here.
#define ADAPTIVE_TUNE_REVERT_BODY_MAX 32

static esp_err_t revert_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > ADAPTIVE_TUNE_REVERT_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }
    char body[ADAPTIVE_TUNE_REVERT_BODY_MAX + 1];
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

    char zone_val[8];
    int zone_len = http_form_find_field(body, "zone", zone_val, sizeof(zone_val));
    if (zone_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "zone field required");
        return ESP_OK;
    }
    int zone = atoi(zone_val);
    if (zone < 0 || zone >= MAX31856_CHANNEL_COUNT) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "zone out of range");
        return ESP_OK;
    }

    char reason[96];
    adaptive_tune_revert_result_t r = adaptive_tune_revert((uint8_t)zone, reason, sizeof(reason));
    httpd_resp_set_type(req, "application/json");
    if (r == ADAPTIVE_TUNE_REVERT_OK) {
        return httpd_resp_sendstr(req, "{\"ok\":true}");
    }
    char resp[192];
    snprintf(resp, sizeof(resp), "{\"ok\":false,\"reason\":\"%s\"}", reason);
    return httpd_resp_sendstr(req, resp);
}

esp_err_t adaptive_tune_http_start(void)
{
    httpd_handle_t server = wifi_provision_http_get_server();
    if (!server) {
        ESP_LOGE(TAG, "no HTTP server -- wifi_provision_http_start() must run first");
        return ESP_ERR_INVALID_STATE;
    }

    static const httpd_uri_t status_uri = {
        .uri = "/api/adaptive_tune", .method = HTTP_GET, .handler = status_get_handler,
    };
    static const httpd_uri_t enable_uri = {
        .uri = "/api/adaptive_tune/enable", .method = HTTP_POST, .handler = enable_post_handler,
    };
    static const httpd_uri_t revert_uri = {
        .uri = "/api/adaptive_tune/revert", .method = HTTP_POST, .handler = revert_post_handler,
    };
    esp_err_t err = httpd_register_uri_handler(server, &status_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/adaptive_tune) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &enable_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/adaptive_tune/enable) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &revert_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/adaptive_tune/revert) failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "adaptive tune status/enable/revert API up (/api/adaptive_tune, /api/adaptive_tune/enable, "
                  "/api/adaptive_tune/revert)");
    return ESP_OK;
}
