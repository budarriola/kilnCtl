#include "iter_tune_http.h"
#include "http_auth_http.h" // kiln_http_register()

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "autotune_engine.h" // autotune_engine_is_active_on_zone()
#include "iter_tune.h"
#include "iter_tune_store.h"
#include "zones_config_accessors.h" // zones_config_set_pid(), MAX31856_CHANNEL_COUNT
#include "wifi_provision_http.h"    // wifi_provision_http_get_server()

static const char *TAG = "iter_tune_http";

_Static_assert(MAX31856_CHANNEL_COUNT <= ITER_TUNE_STORE_MAX_ZONES,
               "iter_tune_store's fixed zone array must cover every real zone");

// GET /api/iter_tune/status -- one line per zone, small fixed stack buffer
// per this file's own "httpd stack blob class" (CLAUDE.md), never a
// heap-sized struct. Streamed in fixed chunks so an unusually large
// zone_count never risks a single oversized snprintf on the 8 KB httpd
// stack.
static esp_err_t iter_tune_status_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    uint8_t refused_version = 0;
    if (iter_tune_store_schema_refused(&refused_version)) {
        char preamble[80];
        int pn = snprintf(preamble, sizeof(preamble), "{\"schema_refused_version\":%u,\"zones\":[",
                           (unsigned)refused_version);
        httpd_resp_sendstr_chunk(req, (pn > 0 && (size_t)pn < sizeof(preamble)) ? preamble : "{\"zones\":[");
    } else {
        httpd_resp_sendstr_chunk(req, "{\"zones\":[");
    }
    for (uint8_t z = 0; z < MAX31856_CHANNEL_COUNT; z++) {
        iter_tune_store_zone_t st;
        bool present = iter_tune_store_get_zone(z, &st);
        char chunk[192];
        int n;
        if (present) {
            n = snprintf(chunk, sizeof(chunk),
                         "%s{\"zone\":%u,\"enabled\":%s,\"has_anchor\":%s,\"has_baseline\":%s,"
                         "\"status\":%u,\"stop_reason\":%u}",
                         (z == 0) ? "" : ",", (unsigned)z,
                         st.enabled ? "true" : "false",
                         st.has_anchor ? "true" : "false",
                         st.has_baseline ? "true" : "false",
                         (unsigned)st.status, (unsigned)st.stop_reason);
        } else {
            n = snprintf(chunk, sizeof(chunk), "%s{\"zone\":%u,\"enabled\":false,\"persisted\":false}",
                         (z == 0) ? "" : ",", (unsigned)z);
        }
        if (n < 0 || (size_t)n >= sizeof(chunk)) {
            ESP_LOGE(TAG, "status row overflowed for zone %u", (unsigned)z);
            continue;
        }
        httpd_resp_sendstr_chunk(req, chunk);
    }
    httpd_resp_sendstr_chunk(req, "]}");
    return httpd_resp_sendstr_chunk(req, NULL);
}

// POST /api/iter_tune/restore_commissioned?zone=N -- the ONLY sanctioned
// write path from this HTTP surface into live PID gains: build a transient,
// mostly-zeroed iter_tune_zone_state_t from the persisted anchor/baseline,
// call iter_tune_restore_commissioned() (never propose/process -- forbidden
// for this file, see iter_tune_write_surface_check.py), apply the returned
// gains via zones_config_set_pid() (iter_tune.c's own documented INTEGRATION
// POINT setter), then persist the resulting OFF/never-enabled state.
static esp_err_t iter_tune_restore_post_handler(httpd_req_t *req)
{
    char query[32];
    long zone = -1;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[8];
        if (httpd_query_key_value(query, "zone", val, sizeof(val)) == ESP_OK) {
            zone = strtol(val, NULL, 10);
        }
    }
    char json[160];
    int n;
    if (zone < 0 || zone >= MAX31856_CHANNEL_COUNT) {
        n = snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"missing or out-of-range zone\"}");
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, json, n < 0 ? 0 : (size_t)n);
    }

    iter_tune_store_zone_t stored;
    bool present = iter_tune_store_get_zone((uint8_t)zone, &stored);
    if (!present || (!stored.has_anchor && !stored.has_baseline)) {
        n = snprintf(json, sizeof(json),
                     "{\"ok\":false,\"error\":\"no commissioned/anchored gains persisted for this zone\"}");
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, json, n < 0 ? 0 : (size_t)n);
    }

    // A1 (step 7 review, 2026-09-23): refuse while autotune owns this zone --
    // autotune_engine_guard.c's accept path would silently overwrite
    // whatever gains we are about to restore, and iter_tune has no way to
    // find out its work was clobbered.
    if (autotune_engine_is_active_on_zone((uint8_t)zone)) {
        n = snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"autotune is running on this zone\"}");
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, json, n < 0 ? 0 : (size_t)n);
    }

    iter_tune_zone_state_t transient = {0};
    transient.has_anchor = stored.has_anchor;
    if (stored.has_anchor) {
        transient.anchor.kp = stored.anchor_kp;
        transient.anchor.ki = stored.anchor_ki;
        transient.anchor.kd = stored.anchor_kd;
    }
    transient.has_baseline = stored.has_baseline;
    if (stored.has_baseline) {
        transient.baseline.kp = stored.baseline_kp;
        transient.baseline.ki = stored.baseline_ki;
        transient.baseline.kd = stored.baseline_kd;
    }

    iter_tune_gains_t restored = iter_tune_restore_commissioned(&transient);
    bool applied = zones_config_set_pid((uint8_t)zone, restored.kp, restored.ki, restored.kd);

    if (!applied) {
        // step 7 review, 2026-09-23, finding 2: a refused zones_config_set_pid
        // must not be recorded as if the restore happened -- the persisted
        // record is left completely untouched (not even the OFF/status
        // fields), so a retried restore still sees the original anchor.
        n = snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"zones_config_set_pid refused\"}");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, json, n < 0 ? 0 : (size_t)n);
    }

    // step 7 review, 2026-09-23, finding 2 (reset-one-side class):
    // iter_tune_restore_commissioned() sets state->baseline = the restored
    // gains (iter_tune.c:139-147) -- that is now what step 8's
    // iter_tune_active_gains() would return for this zone, so the persisted
    // record's baseline_* must be updated to match, not left holding the
    // discarded pre-restore baseline.
    stored.has_baseline = 1;
    stored.baseline_kp = transient.baseline.kp;
    stored.baseline_ki = transient.baseline.ki;
    stored.baseline_kd = transient.baseline.kd;
    stored.enabled = 0;
    stored.status = transient.status;       // OFF, per iter_tune_restore_commissioned()
    stored.stop_reason = transient.stop_reason; // NONE
    esp_err_t persist_err = iter_tune_store_set_zone((uint8_t)zone, &stored);

    if (persist_err != ESP_OK) {
        n = snprintf(json, sizeof(json),
                     "{\"ok\":true,\"warning\":\"gains applied but persisting the OFF state failed\"}");
    } else {
        n = snprintf(json, sizeof(json), "{\"ok\":true,\"kp\":%.6f,\"ki\":%.6f,\"kd\":%.6f}",
                     (double)restored.kp, (double)restored.ki, (double)restored.kd);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, (n > 0 && (size_t)n < sizeof(json)) ? (size_t)n : 0);
}

esp_err_t iter_tune_http_start(void)
{
    iter_tune_store_start();

    httpd_handle_t server = wifi_provision_http_get_server();
    if (server == NULL) {
        ESP_LOGE(TAG, "no httpd server -- cannot register iter_tune routes");
        return ESP_FAIL;
    }

    static const httpd_uri_t status_uri = {
        .uri = "/api/iter_tune/status", .method = HTTP_GET, .handler = iter_tune_status_get_handler,
    };
    static const httpd_uri_t restore_uri = {
        .uri = "/api/iter_tune/restore_commissioned", .method = HTTP_POST,
        .handler = iter_tune_restore_post_handler,
    };

    esp_err_t err = kiln_http_register(server, &status_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(GET /api/iter_tune/status) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = kiln_http_register(server, &restore_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(POST /api/iter_tune/restore_commissioned) failed: %s",
                 esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}
