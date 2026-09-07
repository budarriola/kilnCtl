#include "dualwrite_window_http.h"

#include <stdio.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "dualwrite_window.h"
#include "wifi_provision_http.h"

static const char *TAG = "dualwrite_window_http";

#define BODY_BUF_SIZE 256

static esp_err_t api_dualwrite_window_get_handler(httpd_req_t *req)
{
    dualwrite_window_status_t st;
    dualwrite_window_get_status(&st); /* never returns false for a non-NULL out */

    char buf[BODY_BUF_SIZE];
    int n = snprintf(buf, sizeof(buf),
                      "{\"consecutive_clean_boots\":%u,\"clean_boots_target\":%u,"
                      "\"firing_complete\":%s,\"restore_verified\":%s,\"window_may_close\":%s}",
                      (unsigned)st.consecutive_clean_boots, (unsigned)st.clean_boots_target,
                      st.firing_complete ? "true" : "false", st.restore_verified ? "true" : "false",
                      st.window_may_close ? "true" : "false");
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        ESP_LOGE(TAG, "status JSON did not fit BODY_BUF_SIZE=%d", BODY_BUF_SIZE);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "internal error");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, (size_t)n);
}

/* POST /api/dualwrite_window/restore_verified -- see this file's header
 * comment. No request body is read; the POST itself is the attestation. */
static esp_err_t api_dualwrite_window_restore_verified_post_handler(httpd_req_t *req)
{
    dualwrite_window_note_restore_verified();

    dualwrite_window_status_t st;
    dualwrite_window_get_status(&st);
    char buf[BODY_BUF_SIZE];
    int n = snprintf(buf, sizeof(buf), "{\"ok\":true,\"restore_verified\":%s}",
                      st.restore_verified ? "true" : "false");
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "internal error");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, (size_t)n);
}

esp_err_t dualwrite_window_http_start(void)
{
    /* Runs the once-per-boot clean/unclean check as soon as this endpoint
     * comes up -- see dualwrite_window_boot_check()'s own doc comment for
     * why HTTP bring-up time (after cfg_fs's mount attempt and
     * crash_report_init() have both already run) is the deliberate call
     * site today, pending a boot_guard/main_boot_early.c owner picking this
     * up earlier in boot instead. */
    dualwrite_window_boot_check();

    httpd_handle_t server = wifi_provision_http_get_server();
    if (!server) {
        ESP_LOGE(TAG, "no HTTP server -- wifi_provision_http_start() must run first");
        return ESP_ERR_INVALID_STATE;
    }

    static const httpd_uri_t get_uri = {
        .uri = "/api/dualwrite_window", .method = HTTP_GET, .handler = api_dualwrite_window_get_handler,
    };
    esp_err_t err = httpd_register_uri_handler(server, &get_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(GET /api/dualwrite_window) failed: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t post_uri = {
        .uri = "/api/dualwrite_window/restore_verified",
        .method = HTTP_POST,
        .handler = api_dualwrite_window_restore_verified_post_handler,
    };
    err = httpd_register_uri_handler(server, &post_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(POST /api/dualwrite_window/restore_verified) failed: %s",
                 esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}
