// update_settings_http.c -- see update_settings_http.h.
#include "update_settings_http.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "http_auth_http.h" /* kiln_http_register() */
#include "http_form.h"
#include "cfg_fs_refusal_http.h"
#include "relay_authority.h" /* relay_authority_heat_run_active() */
#include "system_mode_gate.h"
#include "system_mode_gate_http.h"
#include "update_settings.h"
#include "wifi_provision_http.h"

static const char *TAG = "update_settings_http";

// "repo=" plus a worst-case fully percent-encoded 62-character repo is 191 bytes.
#define SETTINGS_BODY_MAX 256

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *json, size_t n)
{
    if (status != NULL) {
        httpd_resp_set_status(req, status);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, n);
}

static esp_err_t send_settings(httpd_req_t *req)
{
    // repo is validated to [A-Za-z0-9._-/] only, so no JSON escaping is needed.
    char json[192];
    char repo[UPDATE_SETTINGS_REPO_MAX_LEN + 1];
    if (!update_settings_repo_copy(repo, sizeof(repo))) {
        return httpd_resp_send_500(req);
    }
    int n = snprintf(json, sizeof(json),
                     "{\"ok\":true,\"repo\":\"%s\",\"default_repo\":\"%s\",\"is_default\":%s}", repo,
                     UPDATE_SETTINGS_DEFAULT_REPO, strcmp(repo, UPDATE_SETTINGS_DEFAULT_REPO) == 0 ? "true" : "false");
    if (n < 0 || (size_t)n >= sizeof(json)) {
        return httpd_resp_send_500(req);
    }
    return send_json(req, NULL, json, (size_t)n);
}

static esp_err_t settings_get_handler(httpd_req_t *req) { return send_settings(req); }

static esp_err_t settings_post_handler(httpd_req_t *req)
{
    if (cfg_fs_http_refuse_if_unmounted(req)) {
        return ESP_OK;
    }
    if (req->content_len <= 0 || req->content_len > SETTINGS_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }
    char body[SETTINGS_BODY_MAX + 1];
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

    // Mode gate after the body is drained: a write during a firing/autotune is refused (409).
    sys_mode_snapshot_t snap = { 0 };
    relay_authority_heat_run_active(&snap.profile_running, &snap.autotune_running);
    char reason[SYSTEM_MODE_GATE_REASON_MAX];
    reason[0] = '\0';
    if (system_mode_gate_check(SYS_ACTION_UPDATE_SETTINGS_WRITE, &snap, reason, sizeof(reason))) {
        ESP_LOGW(TAG, "update settings write refused by system mode gate: %s", reason);
        (void)system_mode_gate_http_send_refusal(req, reason);
        return ESP_OK;
    }

    // One byte over the longest valid repo, so an over-long value is refused, never truncated.
    char repo[UPDATE_SETTINGS_REPO_MAX_LEN + 2];
    int len = http_form_find_field(body, "repo", repo, sizeof(repo));
    if (len == -1) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing \"repo\" field");
        return ESP_OK;
    }
    static const char kBad[] = "{\"ok\":false,\"error\":\"invalid repo\"}";
    if (len < 0) {
        return send_json(req, "400 Bad Request", kBad, sizeof(kBad) - 1);
    }
    // A %00 in the value decodes to an embedded NUL: the string would read as a truncated, possibly
    // valid repo while the request meant something else. Refuse when the C-string length disagrees
    // with the decoded length.
    if (strlen(repo) != (size_t)len) {
        return send_json(req, "400 Bad Request", kBad, sizeof(kBad) - 1);
    }
    esp_err_t err = update_settings_set(repo);
    if (err == ESP_ERR_INVALID_ARG) {
        return send_json(req, "400 Bad Request", kBad, sizeof(kBad) - 1);
    }
    if (err != ESP_OK) {
        // Applied in RAM, not persisted: report it as a failure, never as success.
        return cfg_fs_http_persist_failed(req);
    }
    return send_settings(req);
}

esp_err_t update_settings_http_start(void)
{
    httpd_handle_t server = wifi_provision_http_get_server();
    if (!server) {
        ESP_LOGE(TAG, "no HTTP server -- wifi_provision_http_start() must run first");
        return ESP_ERR_INVALID_STATE;
    }
    static const httpd_uri_t get_uri = {
        .uri = "/api/update/settings", .method = HTTP_GET, .handler = settings_get_handler,
    };
    static const httpd_uri_t post_uri = {
        .uri = "/api/update/settings", .method = HTTP_POST, .handler = settings_post_handler,
    };
    const httpd_uri_t *routes[] = { &get_uri, &post_uri };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t err = kiln_http_register(server, routes[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register %s failed: %s", routes[i]->uri, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}
