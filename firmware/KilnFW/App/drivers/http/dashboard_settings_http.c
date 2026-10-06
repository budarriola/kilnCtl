/* POST /api/unit_pref, POST /api/safety/log_level -- moved out of
 * dashboard_http.c 2026-09-04 (ROADMAP.md M15, the 1500-line rule). See
 * dashboard_http_internal.h for the shared s_dash/DASH_TAG seam. */

#include "dashboard_http_internal.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"

#include "http_form.h"
#include "cfg_fs_refusal_http.h"
#include "safety_trip_words.h"
#include "unit_pref.h"

/* Declared here rather than #include "../bridge/uart_log_bridge.h": that
 * header pulls in uart_protocol.h via a relative path that collides with
 * the one dashboard_http_internal.h's chain already resolved (redefinition
 * errors in the host-test build, since the two paths dedupe as different
 * files despite identical content). A single extern decl avoids the clash;
 * signature kept in sync with uart_log_bridge.h by hand. */
void uart_log_bridge_set_safety_relay_level(uint8_t level);

/* ---- Unit preference (ROADMAP.md, 2026-08-21) -----------------------------
 * POST /api/unit_pref -- the write side of the shared display-unit setting
 * (unit_pref.c). Same application/x-www-form-urlencoded, bounded-body-then-
 * validate-then-commit shape every other settings POST in this codebase uses
 * (relay_post_handler above, zones_http.c's zones_post_handler). DISPLAY-ONLY:
 * this never touches a stored/transmitted temperature anywhere else -- see
 * unit_pref.h's header comment. */
#define UNIT_PREF_BODY_MAX 16 /* "unit=fahrenheit" plus headroom -- generous over the ~14-byte worst case */

esp_err_t unit_pref_post_handler(httpd_req_t *req)
{
    if (cfg_fs_http_refuse_if_unmounted(req)) {
        return ESP_OK;
    }
    if (req->content_len <= 0 || req->content_len > UNIT_PREF_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }

    char body[UNIT_PREF_BODY_MAX + 1];
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

    char unit_val[12];
    int unit_len = http_form_find_field(body, "unit", unit_val, sizeof(unit_val));
    unit_pref_t pref;
    /* Accepts either the short suffix ("C"/"F", matching unit_pref_suffix())
     * or the full word ("celsius"/"fahrenheit", matching what a browser
     * <select> naturally submits) -- case-insensitive on the full word since
     * that one is more likely to be hand-typed by a future integration.
     * Anything else is refused rather than defaulted, same "reject
     * outright, never guess" discipline as zones_http.c's field parsers. */
    if (unit_len > 0 && (strcmp(unit_val, "F") == 0 || strcasecmp(unit_val, "fahrenheit") == 0)) {
        pref = UNIT_PREF_FAHRENHEIT;
    } else if (unit_len > 0 && (strcmp(unit_val, "C") == 0 || strcasecmp(unit_val, "celsius") == 0)) {
        pref = UNIT_PREF_CELSIUS;
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unit must be \"C\" or \"F\"");
        return ESP_OK;
    }

    if (unit_pref_set(pref) != ESP_OK) {
        /* Live value took effect (unit_pref_set() updates RAM first) but the
         * cfg save failed. With no NVS fallback that is an error, never ok. */
        ESP_LOGW(DASH_TAG, "unit preference applied but not persisted -- will not survive a reboot");
        return cfg_fs_http_persist_failed(req);
    }
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* "level=N&peer=relay" plus headroom, same "generous over the actual worst
 * case, checked against Content-Length before a single byte is read"
 * reasoning as UNIT_PREF_BODY_MAX above. */
#define SAFETY_LOG_LEVEL_BODY_MAX 32

/* POST /api/safety/log_level -- ROADMAP.md "SET_LOG_LEVEL (0x1B) has a codec
 * and a Pico consumer but no ESP caller" loose end. Deliberately API-only,
 * no LCD or web page control: this is a developer/bench knob (how chatty
 * the safety processor's own log_task is), not an operator-facing setting
 * -- there is no kiln-operation reason to ever change it during a firing,
 * unlike safety_get_status()'s clear_trip or the commissioning page's
 * config fields, which the operator or installer routinely needs. A
 * developer with `curl` or tools/PcTools has this endpoint; that is judged
 * sufficient exposure. Body is "level=N" (0-4, UART_LOG_LEVEL_* --
 * ERROR/WARN/INFO/DEBUG/VERBOSE), same query-string-in-POST-body shape as
 * unit_pref_post_handler() above.
 *
 * Optional "peer" field (2026-09-20, tools/PcTools/TODO.md's "per-peer level
 * filter" item), same body, same route -- NOT a second endpoint. Two peers
 * on this link, two independent knobs:
 *   - peer omitted or "safety" (default, unchanged behaviour): sends
 *     SAFETY_CMD_SET_LOG_LEVEL over the wire, exactly as before -- sets the
 *     RP2040's OWN log_task filter.
 *   - peer=relay: sets uart_log_bridge_set_safety_relay_level() instead --
 *     purely local to this board, no wire traffic at all -- the floor this
 *     board itself applies to a SAFETY-sourced line before ever relaying it
 *     to the PC link, independent of what the Pico is currently sending.
 * See uart_log_bridge.h's doc comment on that setter for the full
 * rationale (why this needs to be independent of the Pico's own filter). */
esp_err_t safety_log_level_post_handler(httpd_req_t *req)
{
    if (!s_dash.safety) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "safety link not wired up");
        return ESP_OK;
    }
    if (req->content_len <= 0 || req->content_len > SAFETY_LOG_LEVEL_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }

    char body[SAFETY_LOG_LEVEL_BODY_MAX + 1];
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

    char level_val[8];
    int level_len = http_form_find_field(body, "level", level_val, sizeof(level_val));
    if (level_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing \"level\" field");
        return ESP_OK;
    }
    char *endptr = NULL;
    long level = strtol(level_val, &endptr, 10);
    if (endptr == level_val || *endptr != '\0' || level < 0 || level > UART_LOG_LEVEL_VERBOSE) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "level must be 0-4 (ERROR..VERBOSE)");
        return ESP_OK;
    }

    /* Fails CLOSED on anything that is not exactly one of the two known
     * peers. http_form_find_field() returns -1 for "field absent", -2 for
     * "present but too long for out_cap" and 0 for "present but empty" --
     * only -1 may fall through to the historical default. Treating -2/0 as
     * "absent" would quietly send the level to the WRONG peer (over the wire
     * to the Pico) for a body like "peer=relayyyyyyy", which is exactly the
     * silent-wrong-default this route must not have. */
    char peer_val[8];
    int peer_len = http_form_find_field(body, "peer", peer_val, sizeof(peer_val));
    if (peer_len != -1) {
        bool is_relay  = (peer_len > 0) && (strcasecmp(peer_val, "relay") == 0);
        bool is_safety = (peer_len > 0) && (strcasecmp(peer_val, "safety") == 0);
        if (!is_relay && !is_safety) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                "peer must be \"safety\" or \"relay\"");
            return ESP_OK;
        }
        if (is_relay) {
            /* Local-only knob, no wire traffic -- always succeeds. */
            uart_log_bridge_set_safety_relay_level((uint8_t)level);
            return httpd_resp_sendstr(req, "{\"ok\":true}");
        }
    }

    esp_err_t err = safety_link_send_set_log_level(s_dash.safety, (uint8_t)level);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"send failed\"}");
    }
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

