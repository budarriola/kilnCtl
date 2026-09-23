#include "board_temps_http.h"

#include <stdio.h>

#include "esp_http_server.h"
#include "http_auth_http.h" // kiln_http_register() -- WEB_AUTH_PLAN.md section 5
#include "esp_log.h"

#include "board_temps.h"
#include "wifi_provision_http.h"

static const char *TAG = "board_temps_http";

/* {"esp32_c": <float or null>, "thermo_cj_c": [<float or null>, ...]} --
 * null (not 0) for a field that has no valid reading, unlike
 * dashboard_http.c's status_get_handler() which reports 0 alongside a
 * separate valid:false flag for its channels array. Chosen here instead
 * because this endpoint has no per-field companion validity flag in the
 * shape TODO.md 10.7 asked for -- "or null" is explicit in the spec this
 * was built against, so null is the one signal a client can check without
 * a second field to cross-reference. */
static esp_err_t api_board_temps_get_handler(httpd_req_t *req)
{
    board_temps_t bt;
    board_temps_get_live(&bt);

    char json[256];
    size_t o = 0;
    int n;

#define APPEND(...)                                                                             \
    do {                                                                                         \
        n = snprintf(json + o, sizeof(json) - o, __VA_ARGS__);                                  \
        if (n < 0 || (size_t)n >= sizeof(json) - o) {                                            \
            goto overflow;                                                                       \
        }                                                                                         \
        o += (size_t)n;                                                                           \
    } while (0)

    APPEND("{\"esp32_c\":");
    if (bt.esp32_valid) {
        APPEND("%.2f", (double)bt.esp32_c);
    } else {
        APPEND("null");
    }
    APPEND(",\"thermo_cj_c\":[");
    for (size_t i = 0; i < bt.thermo_count; i++) {
        if (bt.thermo_cj_valid[i]) {
            APPEND("%s%.2f", i == 0 ? "" : ",", (double)bt.thermo_cj_c[i]);
        } else {
            APPEND("%snull", i == 0 ? "" : ",");
        }
    }
    APPEND("]}");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, o);

overflow:
    /* Never send the truncated, malformed partial buffer as a 200 -- that
     * reads as success to a caller that only checks the status code. Log
     * once and fail loud with a 500 instead (same pattern as
     * diagnostics_http.c's crash_report/thermo_faults handlers). json[] is
     * 256 B; do not enlarge it to "fix" this (house rule: never enlarge
     * httpd stack buffers). */
    ESP_LOGE(TAG, "board_temps JSON overflowed %u-byte buffer at o=%u",
             (unsigned)sizeof(json), (unsigned)o);
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":false,\"error\":\"board temps report too large to encode\"}", HTTPD_RESP_USE_STRLEN);
#undef APPEND
}

esp_err_t board_temps_http_start(MAX31856BusClass *thermo_bus_or_null)
{
    httpd_handle_t server = wifi_provision_http_get_server();
    if (!server) {
        ESP_LOGE(TAG, "no HTTP server -- wifi_provision_http_start() must run first");
        return ESP_ERR_INVALID_STATE;
    }

    board_temps_bind_thermo_bus(thermo_bus_or_null);

    static const httpd_uri_t api_uri = {
        .uri = "/api/board_temps", .method = HTTP_GET, .handler = api_board_temps_get_handler,
    };
    esp_err_t err = kiln_http_register(server, &api_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/board_temps) failed: %s", esp_err_to_name(err));
        return err;
    }

    /* No standalone /board_temps page any more (owner request, 2026-08-27:
     * "the board health page should be folded into the diagnostics page.
     * most of the information is there anyway") -- it was already a byte-
     * for-byte duplicate of diagnostics_page.html's own "Board health" card
     * (both read this same GET /api/board_temps endpoint and render the
     * identical esp32Temp + per-channel cold-junction rows), so folding it
     * in dropped nothing operator-visible. The API stays: diagnostics_page's
     * card is its only web consumer now, plus ui_page_board_health.c (LCD)
     * via board_temps_get_live() directly, never through HTTP. */

    ESP_LOGI(TAG, "board_temps API up (/board_temps page removed, folded into /diagnostics)");
    return ESP_OK;
}
