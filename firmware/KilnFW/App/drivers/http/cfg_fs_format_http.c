#include "cfg_fs_format_http.h"
#include "http_auth_http.h" // kiln_http_register() -- WEB_AUTH_PLAN.md section 5

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "cfg_fs_mount.h"
#include "ota_http.h" /* challenge/response auth -- see this file's header comment for
                        * why POST reuses OTA_HTTP_CONTEXT_FACTORY_RESET */
#include "wifi_provision_http.h"

static const char *TAG = "cfg_fs_format_http";

static esp_err_t format_pending_get_handler(httpd_req_t *req)
{
    bool pending = cfg_fs_mount_format_confirmation_pending();
    const char *reason = cfg_fs_mount_format_pending_reason();

    /* Reasons are all authored by cfg_fs_format_gate_describe()/this
     * module's own callers -- plain ASCII, no quotes or control characters
     * -- so a bare snprintf is safe the same way dashboard_json.c's other
     * fixed-vocabulary string fields are. */
    char json[256];
    int n = snprintf(json, sizeof(json), "{\"pending\":%s,\"reason\":\"%s\"}", pending ? "true" : "false",
                      pending ? reason : "");
    if (n < 0 || (size_t)n >= sizeof(json)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "reason too long");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

static esp_err_t format_confirm_post_handler(httpd_req_t *req)
{
    /* Same authenticate-before-anything-else ordering as
     * factory_reset.c's reset_post_handler() -- an unauthenticated caller
     * must not learn whether formatting is even pending from this
     * endpoint's own refusal. */
    char ip[46];
    if (!ota_http_authenticate_request(req, OTA_HTTP_CONTEXT_FACTORY_RESET, ip)) {
        return ESP_OK;
    }

    ESP_LOGW(TAG, "cfg_fs format_confirm from %s: authenticated, formatting cfg partition now", ip);
    esp_err_t err = cfg_fs_confirm_format_device();
    if (err != ESP_OK) {
        char msg[96];
        snprintf(msg, sizeof(msg), "format failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, msg);
        return ESP_OK;
    }

    httpd_resp_sendstr(req, "ok -- cfg partition formatted and mounted");
    return ESP_OK;
}

esp_err_t cfg_fs_format_http_start(void)
{
    httpd_handle_t server = wifi_provision_http_get_server();
    if (!server) {
        ESP_LOGE(TAG, "no HTTP server -- wifi_provision_http_start() must run first");
        return ESP_ERR_INVALID_STATE;
    }

    static const httpd_uri_t pending_uri = {
        .uri = "/api/cfgfs/format_pending", .method = HTTP_GET, .handler = format_pending_get_handler,
    };
    esp_err_t err = kiln_http_register(server, &pending_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(GET /api/cfgfs/format_pending) failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t confirm_uri = {
        .uri = "/api/cfgfs/format_confirm", .method = HTTP_POST, .handler = format_confirm_post_handler,
    };
    err = kiln_http_register(server, &confirm_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(POST /api/cfgfs/format_confirm) failed: %s",
                 esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}
