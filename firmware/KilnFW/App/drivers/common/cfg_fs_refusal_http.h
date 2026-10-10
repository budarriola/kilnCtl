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
static inline esp_err_t cfg_fs_http_persist_failed(httpd_req_t *req);

/* Same as cfg_fs_http_persist_failed(), for a writer that can report whether the new value was
 * nonetheless ADOPTED into RAM (file read back with it, live state matches) even though the
 * save as a whole failed. Mounted case adds "adopted":true|false to the 500 JSON; the
 * unmounted 503 is unchanged (adopted cannot be told apart there). */
static inline esp_err_t cfg_fs_http_persist_failed_adopted(httpd_req_t *req, bool adopted)
{
    if (!cfg_fs_is_available()) {
        return cfg_fs_http_persist_failed(req);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req, adopted ? "{\"ok\":false,\"adopted\":true,\"error\":\"could not be saved to flash\"}"
                                    : "{\"ok\":false,\"adopted\":false,\"error\":\"could not be saved to flash\"}");
    return ESP_OK;
}

static inline esp_err_t cfg_fs_http_persist_failed(httpd_req_t *req)
{
    if (!cfg_fs_is_available()) {
        cfg_fs_http_send_not_mounted(req);
    } else if (cfg_fs_degraded_count() > 0) {
        /* persfx MED-1: a store the boot could not read refuses every save until reboot (ESP_ERR_INVALID_STATE
         * from pref_cfg_fs_save()/zones nvs_save()). That is a conflict with device state, not a flash failure. */
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"store_unreadable_at_boot\",\"reason\":\"a stored setting could not be read at boot, so saves to it are refused to avoid overwriting it; reboot the controller to retry (see GET /api/cfgfs degraded)\"}");
    } else {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"could not be saved to flash\"}");
    }
    return ESP_OK;
}
