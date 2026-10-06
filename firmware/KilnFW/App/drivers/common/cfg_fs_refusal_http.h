#pragma once
/* Shared HTTP refusal for save routes now that cfg (LittleFS) is the only
 * persistence target. Header-only on purpose: no stack buffers, only string
 * literals, so it is safe on the httpd task's tight stack. */

#include "cfg_fs.h"
#include "esp_http_server.h"

/* Pre-check, call at the top of a save handler before any RAM mutation.
 * Returns true if the response was sent (503, body names the cause and the
 * format-confirm remedy) and the handler must return ESP_OK. */
static inline bool cfg_fs_http_refuse_if_unmounted(httpd_req_t *req)
{
    if (cfg_fs_is_available()) {
        return false;
    }
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"" CFG_FS_NOT_MOUNTED_TEXT "\"}");
    return true;
}

/* Post-failure response for a save that failed. 503 with the cfg text when
 * the partition is unmounted, else 500 naming a flash write failure. Never a
 * fake success. Returns ESP_OK so a handler can `return` it directly. */
static inline esp_err_t cfg_fs_http_persist_failed(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    if (!cfg_fs_is_available()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"" CFG_FS_NOT_MOUNTED_TEXT "\"}");
    } else {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"could not be saved to flash\"}");
    }
    return ESP_OK;
}
