#pragma once
/* Shared HTTP refusal for save routes now that cfg (LittleFS) is the only
 * persistence target. Header-only on purpose: no stack buffers, only string
 * literals, so it is safe on the httpd task's tight stack. */

#include <stdbool.h>
#include "cfg_fs.h"
#include "esp_err.h"
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

/* Recovery text shared by every store_unreadable_at_boot 409 (persfx3 LOW-2). A reboot retries the read; if
 * the cause persists, GET /api/cfgfs names the store and these two routes clear it. */
#define CFG_FS_STORE_UNREADABLE_REASON     "a stored setting could not be read at boot, so saves to it are refused to avoid overwriting it; reboot the "     "controller to retry; if it persists after a reboot see GET /api/cfgfs (degraded), then clear it with a cfg "     "format (POST /api/cfgfs/format_confirm force_healthy=1) or a factory reset (scope kiln)"

/* Post-failure response for a save that failed. 503 with the cfg text when the partition is unmounted; 409
 * store_unreadable_at_boot ONLY when the caller says THIS store is refused (`store_refused`) and the error is the
 * refusal's ESP_ERR_INVALID_STATE (persfx3 MED-2: a flash write error or OOM on another store is a 500, and a
 * different store being degraded must not rename it); else 500 naming a flash write failure. Never a fake
 * success. `adopted` < 0 omits the field. Returns ESP_OK so a handler can `return` it directly. */
static inline esp_err_t cfg_fs_http_persist_failed_state(httpd_req_t *req, bool store_refused, esp_err_t err,
                                                         int adopted)
{
    if (!cfg_fs_is_available()) {
        cfg_fs_http_send_not_mounted(req);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    if (store_refused && err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
        if (adopted < 0) {
            httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"store_unreadable_at_boot\",\"reason\":\"" CFG_FS_STORE_UNREADABLE_REASON "\"}");
        } else if (adopted) {
            httpd_resp_sendstr(req, "{\"ok\":false,\"adopted\":true,\"error\":\"store_unreadable_at_boot\",\"reason\":\"" CFG_FS_STORE_UNREADABLE_REASON "\"}");
        } else {
            httpd_resp_sendstr(req, "{\"ok\":false,\"adopted\":false,\"error\":\"store_unreadable_at_boot\",\"reason\":\"" CFG_FS_STORE_UNREADABLE_REASON "\"}");
        }
        return ESP_OK;
    }
    httpd_resp_set_status(req, "500 Internal Server Error");
    if (adopted < 0) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"could not be saved to flash\"}");
    } else if (adopted) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"adopted\":true,\"error\":\"could not be saved to flash\"}");
    } else {
        httpd_resp_sendstr(req, "{\"ok\":false,\"adopted\":false,\"error\":\"could not be saved to flash\"}");
    }
    return ESP_OK;
}

/* Per-store form: `store` is the cfg path the handler's own store registers in the degraded table
 * (e.g. "zones.json", UNIT_PREF_FILE_PATH). */
static inline esp_err_t cfg_fs_http_persist_failed_for(httpd_req_t *req, const char *store, esp_err_t err)
{
    return cfg_fs_http_persist_failed_state(req, cfg_fs_degraded_is(store), err, -1);
}

/* Handlers with no store/error to name: unmounted -> 503, otherwise 500. Never the unreadable-store 409. */
static inline esp_err_t cfg_fs_http_persist_failed(httpd_req_t *req)
{
    return cfg_fs_http_persist_failed_state(req, false, ESP_FAIL, -1);
}

/* Same as cfg_fs_http_persist_failed_for(), for a writer that can report whether the new value was
 * nonetheless ADOPTED into RAM (file read back with it, live state matches) even though the save as a whole
 * failed. Mounted case adds "adopted":true|false; the unmounted 503 is unchanged. */
static inline esp_err_t cfg_fs_http_persist_failed_adopted_for(httpd_req_t *req, bool adopted, const char *store,
                                                               esp_err_t err)
{
    return cfg_fs_http_persist_failed_state(req, cfg_fs_degraded_is(store), err, adopted ? 1 : 0);
}

static inline esp_err_t cfg_fs_http_persist_failed_adopted(httpd_req_t *req, bool adopted)
{
    return cfg_fs_http_persist_failed_state(req, false, ESP_FAIL, adopted ? 1 : 0);
}
