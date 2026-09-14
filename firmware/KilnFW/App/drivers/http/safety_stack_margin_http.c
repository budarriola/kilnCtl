/* GET /api/saftyfw_stack_margin -- see safety_stack_margin_http.h and
 * safety_link_get_stack_margin()'s own doc comment (safety_link.h) for the
 * wire contract this surfaces. This file owns only the HTTP/JSON framing;
 * it never measures anything itself and never caches anything longer than
 * one request (each GET is a fresh, live, blocking round trip to the Pico,
 * per that function's own "expected to be polled SLOWLY" contract -- do
 * not poll this endpoint from anything resembling the ~1Hz DIAG loop).
 *
 * UNITS, repeated here deliberately (kilnlink_stack_margin.h's own "read
 * this before touching either side" section): every word count below is
 * WORDS, not bytes -- the OPPOSITE convention from this same board's own
 * ESP-side get_stack_margin() (bytes). Never mix the two without
 * converting one of them first.
 *
 * FLOOR, NOT WORST CASE: every value is the tightest margin observed since
 * the Pico's last boot on whatever code paths actually ran -- never a
 * ceiling. The JSON below carries this as an explicit field
 * (floor_not_worst_case:true) rather than only in a comment, so a
 * caller/renderer cannot miss it. */
#include "safety_stack_margin_http.h"

#include <stdio.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "kilnlink/kilnlink_stack_margin.h"
#include "wifi_provision_http.h"

static const char *TAG = "safety_stack_margin_http";

static SafetyLinkClass *s_link = NULL;

/* Stable names for kilnlink_stack_margin_task_id_t -- see that enum's own
 * "order is arbitrary but fixed" comment; this table must track it 1:1 by
 * index, never reordered independently. */
static const char *task_name(uint8_t task_id)
{
    switch ((kilnlink_stack_margin_task_id_t)task_id) {
    case KILNLINK_STACK_MARGIN_TASK_RELAY_OWNER:   return "relay_owner";
    case KILNLINK_STACK_MARGIN_TASK_SAFETY_CORE:   return "safety_core";
    case KILNLINK_STACK_MARGIN_TASK_DISCRETE_TASK: return "discrete_task";
    case KILNLINK_STACK_MARGIN_TASK_THERMO_TASK:   return "thermo_task";
    case KILNLINK_STACK_MARGIN_TASK_CURRENT_TASK:  return "current_task";
    case KILNLINK_STACK_MARGIN_TASK_LINK_TASK:     return "link_task";
    case KILNLINK_STACK_MARGIN_TASK_LOG_TASK:      return "log_task";
    case KILNLINK_STACK_MARGIN_TASK_UPDATE_TASK:   return "update_task";
    case KILNLINK_STACK_MARGIN_TASK_WATCHDOG_TASK: return "watchdog_task";
    default:                                       return "unknown";
    }
}

/* Fixed upper bound for build_stack_margin_json() below. Worst-case per
 * entry: `{"id":8,"name":"watchdog_task","high_water_words":65535,`
 * `"stack_total_words":65535,"measured":true},` -- name is the longest
 * fixed string here ("watchdog_task", "discrete_task" tie at 13 chars);
 * measured generously at 110 bytes/entry. KILNLINK_STACK_MARGIN_NUM_TASKS
 * is fixed at 9 (never variable-length, unlike partition_info_http.c's
 * table), so a single stack buffer sized with real headroom -- not a
 * chunked response -- is appropriate here, same reasoning
 * build_commissioning_json() uses for its own fixed-shape body. Header/
 * trailer generously budgeted at 160 bytes. Real worst case is
 * 160 + 9*110 = 1150; this is comfortably over that. */
#define STACK_MARGIN_JSON_MAX 1536u

/* Pure/host-testable: takes only what it needs, no httpd_req_t/SafetyLinkClass,
 * same "extract the pure builder" split safety_cfg_http.c's own
 * build_commissioning_json() uses. Returns bytes written (< out_cap), or 0
 * if the buffer was too small -- never emits truncated (= invalid) JSON. */
size_t safety_stack_margin_build_json(bool link_up, esp_err_t link_result,
                                       const kilnlink_stack_margin_t *m, char *out, size_t out_cap)
{
    size_t o = 0;
    int written;

#define APPEND(...)                                                                    \
    do {                                                                               \
        written = snprintf(out + o, out_cap - o, __VA_ARGS__);                        \
        if (written < 0 || (size_t)written >= out_cap - o) {                          \
            return 0;                                                                  \
        }                                                                              \
        o += (size_t)written;                                                          \
    } while (0)

    APPEND("{\"link_up\":%s", link_up ? "true" : "false");
    APPEND(",\"floor_not_worst_case\":true");
    APPEND(",\"units\":\"words\"");
    APPEND(",\"note\":\"every value is a FLOOR (tightest margin observed since the Pico's "
           "last boot on whatever code paths actually ran), never a worst case\"");

    if (!link_up || !m) {
        APPEND(",\"error\":\"%s\"", esp_err_to_name(link_result));
        APPEND("}");
        return o;
    }

    APPEND(",\"rounds_completed\":%u", (unsigned)m->rounds_completed);
    APPEND(",\"all_measured\":%s", m->rounds_completed > 0 ? "true" : "false");
    APPEND(",\"tasks\":[");
    for (size_t i = 0; i < KILNLINK_STACK_MARGIN_NUM_TASKS; i++) {
        const kilnlink_stack_margin_entry_t *e = &m->entries[i];
        bool measured = e->high_water_words != KILNLINK_STACK_MARGIN_UNMEASURED;
        APPEND("%s{\"id\":%u,\"name\":\"%s\",\"measured\":%s", i == 0 ? "" : ",",
               (unsigned)e->task_id, task_name(e->task_id), measured ? "true" : "false");
        if (measured) {
            APPEND(",\"high_water_words\":%u,\"stack_total_words\":%u",
                   (unsigned)e->high_water_words, (unsigned)e->stack_total_words);
        }
        APPEND("}");
    }
    APPEND("]}");

#undef APPEND
    return o;
}

static esp_err_t api_stack_margin_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    bool link_up = false;
    esp_err_t link_result = ESP_ERR_INVALID_STATE;
    kilnlink_stack_margin_t margin = {0};

    if (s_link) {
        link_result = safety_link_get_stack_margin(s_link, &margin);
        link_up = (link_result == ESP_OK);
    }

    static char json[STACK_MARGIN_JSON_MAX];
    size_t len = safety_stack_margin_build_json(link_up, link_result, link_up ? &margin : NULL,
                                                 json, sizeof(json));
    if (len == 0) {
        ESP_LOGE(TAG, "JSON build failed (buffer too small -- should not happen, fixed 9-task shape)");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "stack margin JSON build failed");
        return ESP_OK;
    }
    return httpd_resp_send(req, json, len);
}

esp_err_t safety_stack_margin_http_start(SafetyLinkClass *link_or_null)
{
    s_link = link_or_null;

    httpd_handle_t server = wifi_provision_http_get_server();
    if (!server) {
        ESP_LOGE(TAG, "no HTTP server -- wifi_provision_http_start() must run first");
        return ESP_ERR_INVALID_STATE;
    }

    static const httpd_uri_t api_uri = {
        .uri = "/api/saftyfw_stack_margin", .method = HTTP_GET, .handler = api_stack_margin_get_handler,
    };
    esp_err_t err = httpd_register_uri_handler(server, &api_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/saftyfw_stack_margin) failed: %s",
                 esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "SaftyFW stack margin API up (link_or_null=%p)", (void *)link_or_null);
    return ESP_OK;
}
