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

/* Memory budget: ONE status-response buffer, sized generously per zone
 * (96 bytes/zone covers every field printed below with room to spare) and
 * allocated from PSRAM (MALLOC_CAP_SPIRAM), never the request handler's own
 * internal-DRAM stack -- same discipline log_http.c documents, following the
 * internal-DRAM-exhaustion incident that truncated /app.js (see project
 * memory "ESP internal DRAM exhaustion" and the documented httpd stack
 * near-overflow, 64 bytes free under real load). At MAX31856_CHANNEL_COUNT
 * (<=5) zones this is comfortably under 1.5KB, once, freed before the
 * handler returns -- the per-zone budget below was widened from 96 to 320
 * bytes when the coupled-solve/Ki-diagnosis fields (PID_EXPANSION_PLAN.md
 * 3.3) were added, to cover two more ~96-byte refusal-reason strings per
 * zone plus their surrounding numeric fields. The enable-POST body buffer
 * below stays a small fixed stack array (<=65 bytes) -- consistent with the
 * tiny form-body buffers already used elsewhere in this codebase (e.g.
 * adaptive_tune.c's own prior version of this handler, settings_http.c) and
 * far below anything that would threaten the documented near-overflow
 * margin. */
#define ADAPTIVE_TUNE_STATUS_BUF_BYTES (320 * MAX31856_CHANNEL_COUNT + 64)

static esp_err_t status_get_handler(httpd_req_t *req)
{
    char *buf = (char *)heap_caps_malloc(ADAPTIVE_TUNE_STATUS_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }

    size_t off = 0;
    off += (size_t)snprintf(buf + off, ADAPTIVE_TUNE_STATUS_BUF_BYTES - off, "{\"zones\":[");
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        adaptive_tune_zone_status_t st;
        adaptive_tune_get_status(zi, &st);
        off += (size_t)snprintf(
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
            "\"ki_verdict\":%u,\"ki_correction_pct\":%.2f,\"ki_applied\":%s,\"ki_refusal\":\"%s\"}",
            zi == 0 ? "" : ",", (unsigned)zi, st.enabled ? "true" : "false", (unsigned)st.ring_count,
            (unsigned)st.observations_lifetime, st.has_applied ? "true" : "false", (double)st.prior_k_dc,
            (double)st.applied_k_dc, (double)st.last_delta_pct, (unsigned)st.last_applied_profile_id,
            (unsigned)st.last_applied_unix_s, st.last_refusal_reason,
            (unsigned)st.joint_observations, st.coupled_attempted ? "true" : "false",
            st.coupled_applied ? "true" : "false", (unsigned)st.coupled_cells_changed, st.coupled_refusal_reason,
            (unsigned)st.ki_verdict, (double)st.ki_correction_pct, st.ki_applied ? "true" : "false",
            st.ki_refusal_reason);
        if (off >= ADAPTIVE_TUNE_STATUS_BUF_BYTES) {
            off = ADAPTIVE_TUNE_STATUS_BUF_BYTES - 1; // truncated -- MAX31856_CHANNEL_COUNT is small (<=5) and
                                                        // the buffer sized generously, so this should not trigger
            break;
        }
    }
    snprintf(buf + off, ADAPTIVE_TUNE_STATUS_BUF_BYTES - off, "]}");

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

    ESP_LOGI(TAG, "adaptive tune status/enable API up (/api/adaptive_tune, /api/adaptive_tune/enable)");
    return ESP_OK;
}
