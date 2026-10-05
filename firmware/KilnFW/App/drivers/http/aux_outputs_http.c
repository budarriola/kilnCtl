// aux_outputs_http -- see the header. Thin: decisions are in aux_outputs_http_core.c.
#include "aux_outputs_http.h"
#include "http_auth_http.h" // kiln_http_register()

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "MAX31856.h"
#include "aux_outputs_cfg.h"
#include "aux_outputs_http_core.h"
#include "dashboard_http.h" // dashboard_set_relay()
#include "relay_authority.h"
#include "safety_cfg_writer_guard.h"
#include "system_mode_gate.h"
#include "wifi_provision_http.h"
#include "zones_config_accessors.h"
#include "zones_config_json.h"

static const char *TAG = "aux_outputs_http";

#define AUX_BODY_MAX 128

static bool op_mode_blocked(aux_http_action_t action, char *reason, size_t cap)
{
    sys_mode_snapshot_t snap = { 0 };
    relay_authority_heat_run_active(&snap.profile_running, &snap.autotune_running);
    return system_mode_gate_check(action == AUX_HTTP_ACTION_CONFIG ? SYS_ACTION_WRITE_ZONES_CONFIG
                                                                   : SYS_ACTION_RAW_RELAY_DEBUG_WRITE,
                                  &snap, reason, cap);
}

static uint8_t op_zones_union(void)
{
    uint8_t u = 0;
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        uint8_t m = 0;
        if (zones_config_get_relay_mask(zi, &m)) {
            u |= m;
        }
    }
    return u;
}

static aux_relay_result_t op_set_relay(uint8_t relay, bool on)
{
    uint32_t sources = 0;
    switch (dashboard_set_relay(relay, on, &sources)) {
    case DASHBOARD_RELAY_OK: return AUX_RELAY_OK;
    case DASHBOARD_RELAY_ERR_NO_BOARD: return AUX_RELAY_ERR_NO_BOARD;
    case DASHBOARD_RELAY_ERR_RANGE: return AUX_RELAY_ERR_RANGE;
    case DASHBOARD_RELAY_ERR_RUNNING: return AUX_RELAY_ERR_RUNNING;
    case DASHBOARD_RELAY_ERR_OWNED: return AUX_RELAY_ERR_OWNED;
    case DASHBOARD_RELAY_ERR_SAFETY: return AUX_RELAY_ERR_SAFETY;
    case DASHBOARD_RELAY_ERR_UPDATING:
    case DASHBOARD_RELAY_ERR_CRASH_UNACK: return AUX_RELAY_ERR_BLOCKED;
    default: return AUX_RELAY_ERR_IO_FAIL;
    }
}

static const aux_http_ops_t s_ops = {
    .mode_blocked = op_mode_blocked,
    .zones_union = op_zones_union,
    .get = aux_outputs_cfg_get,
    .set = aux_outputs_cfg_set,
    .enabled_mask = aux_outputs_cfg_enabled_mask,
    .conflict_mask = aux_outputs_cfg_conflict_mask,
    .quarantined = aux_outputs_cfg_quarantined,
    .set_relay = op_set_relay,
};

static esp_err_t send_reply(httpd_req_t *req, const aux_http_reply_t *r)
{
    if (r->status == 200) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, r->msg);
    }
    const char *status = "500 Internal Server Error";
    switch (r->status) {
    case 400: status = "400 Bad Request"; break;
    case 403: status = "403 Forbidden"; break;
    case 409: status = "409 Conflict"; break;
    case 503: status = "503 Service Unavailable"; break;
    default: break;
    }
    httpd_resp_set_status(req, status);
    return httpd_resp_sendstr(req, r->msg);
}

static bool read_body(httpd_req_t *req, char *body, size_t cap)
{
    if (req->content_len <= 0 || req->content_len >= (int)cap) {
        return false;
    }
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            return false;
        }
        received += (size_t)ret;
    }
    body[received] = '\0';
    return true;
}

static esp_err_t aux_get_handler(httpd_req_t *req)
{
    char buf[200];
    httpd_resp_set_type(req, "application/json");
    size_t n = aux_http_core_format_head(&s_ops, buf, sizeof(buf));
    if (httpd_resp_send_chunk(req, buf, (ssize_t)n) != ESP_OK) {
        return ESP_FAIL;
    }
    for (uint8_t r = 1; r <= AUX_OUTPUTS_COUNT; r++) {
        n = 0;
        if (r > 1) {
            buf[n++] = ',';
        }
        n += aux_http_core_format_entry(&s_ops, r, buf + n, sizeof(buf) - n);
        if (httpd_resp_send_chunk(req, buf, (ssize_t)n) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    if (httpd_resp_send_chunk(req, "]}", 2) != ESP_OK) {
        return ESP_FAIL;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t aux_post_handler(httpd_req_t *req)
{
    char body[AUX_BODY_MAX];
    if (!read_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }
    /* Serialize against POST /api/zones (same single-flight guard around its commit): the
     * aux set checks the zones union and the zones POST checks the aux mask, and without a
     * shared claim two concurrent writes could each pass and leave both sides owning a relay. */
    if (!safety_cfg_writer_try_claim(SAFETY_CFG_WRITER_HTTP_SYNC)) {
        aux_http_reply_t busy = { 409, "another commissioning operation is running" };
        return send_reply(req, &busy);
    }
    aux_http_reply_t reply;
    aux_http_core_set(&s_ops, body, &reply);
    (void)safety_cfg_writer_release(SAFETY_CFG_WRITER_HTTP_SYNC);
    if (reply.status != 200) {
        ESP_LOGW(TAG, "POST /api/aux_outputs refused (%d): %s", reply.status, reply.msg);
    }
    return send_reply(req, &reply);
}

static esp_err_t aux_manual_post_handler(httpd_req_t *req)
{
    char body[AUX_BODY_MAX];
    if (!read_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }
    aux_http_reply_t reply;
    aux_http_core_manual(&s_ops, body, &reply);
    if (reply.status != 200) {
        ESP_LOGW(TAG, "POST /api/aux_outputs/manual refused (%d): %s", reply.status, reply.msg);
    }
    return send_reply(req, &reply);
}

esp_err_t aux_outputs_http_start(void)
{
    /* Store + provider FIRST and unconditionally -- see the header on boot order. */
    (void)aux_outputs_cfg_start(op_zones_union());
    zones_config_json_set_aux_enabled_provider(aux_outputs_cfg_enabled_mask);

    httpd_handle_t server = wifi_provision_http_get_server();
    if (!server) {
        ESP_LOGE(TAG, "no HTTP server -- wifi_provision_http_start() must run first");
        return ESP_ERR_INVALID_STATE;
    }
    static const httpd_uri_t get_uri = {
        .uri = "/api/aux_outputs", .method = HTTP_GET, .handler = aux_get_handler,
    };
    static const httpd_uri_t post_uri = {
        .uri = "/api/aux_outputs", .method = HTTP_POST, .handler = aux_post_handler,
    };
    static const httpd_uri_t manual_uri = {
        .uri = "/api/aux_outputs/manual", .method = HTTP_POST, .handler = aux_manual_post_handler,
    };
    esp_err_t err = kiln_http_register(server, &get_uri);
    if (err == ESP_OK) {
        err = kiln_http_register(server, &post_uri);
    }
    if (err == ESP_OK) {
        err = kiln_http_register(server, &manual_uri);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "aux route registration failed: %s", esp_err_to_name(err));
    }
    return err;
}
