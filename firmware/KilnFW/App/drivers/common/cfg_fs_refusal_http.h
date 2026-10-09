#pragma once
/* Shared HTTP refusal for save routes now that cfg (LittleFS) is the only
 * persistence target. Header-only on purpose: no stack buffers, only string
 * literals, so it is safe on the httpd task's tight stack. */

#include "cfg_fs.h"
#include "esp_http_server.h"

/* Sends the 503 body for an unmounted cfg. Two literals, no buffer: the
 * recovery-mode variant (cfg_fs_skipped_for_recovery()) must not advise a
 * format, because the partition still holds the only saved config. */
static inline void cfg_fs_http_send_not_mounted(httpd_req_t *req)
{
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "application/json");
    if (cfg_fs_skipped_for_recovery()) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"" CFG_FS_RECOVERY_SKIPPED_TEXT "\"}");
    } else {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"" CFG_FS_NOT_MOUNTED_TEXT "\"}");
    }
}

/* Pre-check, call at the top of a save handler before any RAM mutation.
 * Returns true if the response was sent (503, body names the cause and the
 * remedy: format-confirm, or leaving recovery mode) and the handler must return ESP_OK. */
static inline bool cfg_fs_http_refuse_if_unmounted(httpd_req_t *req)
{
    if (cfg_fs_is_available()) {
        return false;
    }
    cfg_fs_http_send_not_mounted(req);
    return true;
}

/* Post-failure response for a save that failed. 503 with the cfg text when
 * the partition is unmounted, else 500 naming a flash write failure. Never a
 * fake success. Returns ESP_OK so a handler can `return` it directly. */
static inline esp_err_t cfg_fs_http_persist_failed(httpd_req_t *req)
{
    if (!cfg_fs_is_available()) {
        cfg_fs_http_send_not_mounted(req);
    } else {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"could not be saved to flash\"}");
    }
    return ESP_OK;
}
