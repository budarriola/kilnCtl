#include "iter_tune_http.h"
#include "http_form.h"
#include "http_auth_http.h" // kiln_http_register()

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "autotune_engine.h" // autotune_engine_reserve_zone_for_external_write()
#include "cfg_fs_refusal_http.h" // cfg_fs_http_refuse_if_unmounted(), cfg_fs_http_persist_failed()
#include "firing_shadow.h"   // ITER_TUNE_REDESIGN.md step 8 -- read-only status only
#include "iter_tune.h"
#include "iter_tune_store.h"
#include "relay_authority.h"        // relay_authority_heat_run_active() -- system_mode_gate snapshot
#include "system_mode_gate.h"       // SYS_ACTION_WRITE_ZONES_CONFIG -- owner decision Q2, 2026-09-25
#include "system_mode_gate_http.h"  // system_mode_gate_http_send_refusal()
#include "zones_config_accessors.h" // zones_config_set_pid(), MAX31856_CHANNEL_COUNT
#include "wifi_provision_http.h"    // wifi_provision_http_get_server()

static const char *TAG = "iter_tune_http";

_Static_assert(MAX31856_CHANNEL_COUNT <= ITER_TUNE_STORE_MAX_ZONES,
               "iter_tune_store's fixed zone array must cover every real zone");

// GET /api/iter_tune/status -- one line per zone, small fixed stack buffer
// per this file's own "httpd stack blob class" (CLAUDE.md), never a
// heap-sized struct. Streamed in fixed chunks so an unusually large
// zone_count never risks a single oversized snprintf on the 8 KB httpd
// stack.
static esp_err_t iter_tune_status_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    // Shadow mode (ITER_TUNE_REDESIGN.md step 8): one whole-firing-set
    // summary, not per-zone data, so it is its own top-level object rather
    // than forced into the per-zone loop below. READ-ONLY -- firing_shadow
    // never writes a gain, this handler never calls anything that would.
    // Small fixed stack buffer, same "httpd stack blob class" convention as
    // the rest of this file.
    {
        firing_shadow_status_t shadow;
        // Emitted as two writes so the buffer stays at 176 B (httpd stack
        // blob class: never grow an httpd stack buffer). Worst case, every
        // counter at 10 digits: first half 91 chars, second half 101 chars.
#define SHADOW_FMT_A "{\"shadow\":{\"firings_scored\":%lu,\"accept_count\":%lu,\"reject_count\":%lu,"
#define SHADOW_FMT_B "\"insufficient_count\":%lu,\"no_matched_pairs_count\":%lu,\"alloc_failed_count\":%lu},"
        char chunk[176];
        // Each "%lu" (3 chars) expands to at most 10 digits for a uint32_t.
        _Static_assert(sizeof(SHADOW_FMT_A) + 3 * (10 - 3) <= sizeof(chunk), "shadow chunk A overflows");
        _Static_assert(sizeof(SHADOW_FMT_B) + 3 * (10 - 3) <= sizeof(chunk), "shadow chunk B overflows");
        int n = -1;
        if (firing_shadow_get_status(&shadow)) {
            n = snprintf(chunk, sizeof(chunk), SHADOW_FMT_A, (unsigned long)shadow.firings_scored,
                         (unsigned long)shadow.accept_count, (unsigned long)shadow.reject_count);
        }
        if (n > 0 && (size_t)n < sizeof(chunk)) {
            httpd_resp_sendstr_chunk(req, chunk);
            n = snprintf(chunk, sizeof(chunk), SHADOW_FMT_B, (unsigned long)shadow.insufficient_count,
                         (unsigned long)shadow.no_matched_pairs_count, (unsigned long)shadow.alloc_failed_count);
            // The first half is already on the wire, so a "shadow":null
            // fallback is no longer possible; close the object instead so
            // the JSON stays valid (unreachable per the asserts above).
            httpd_resp_sendstr_chunk(req, (n > 0 && (size_t)n < sizeof(chunk)) ? chunk : "\"truncated\":true},");
        } else {
            httpd_resp_sendstr_chunk(req, "{\"shadow\":null,");
        }
#undef SHADOW_FMT_A
#undef SHADOW_FMT_B
    }

    uint8_t refused_version = 0;
    if (iter_tune_store_schema_refused(&refused_version)) {
        char preamble[80];
        int pn = snprintf(preamble, sizeof(preamble), "\"schema_refused_version\":%u,\"zones\":[",
                           (unsigned)refused_version);
        httpd_resp_sendstr_chunk(req, (pn > 0 && (size_t)pn < sizeof(preamble)) ? preamble : "\"zones\":[");
    } else {
        httpd_resp_sendstr_chunk(req, "\"zones\":[");
    }
    for (uint8_t z = 0; z < MAX31856_CHANNEL_COUNT; z++) {
        iter_tune_store_zone_t st;
        bool present = iter_tune_store_get_zone(z, &st);
        char chunk[192];
        int n;
        if (present) {
            n = snprintf(chunk, sizeof(chunk),
                         "%s{\"zone\":%u,\"enabled\":%s,\"has_anchor\":%s,\"has_baseline\":%s,"
                         "\"status\":%u,\"stop_reason\":%u}",
                         (z == 0) ? "" : ",", (unsigned)z,
                         st.enabled ? "true" : "false",
                         st.has_anchor ? "true" : "false",
                         st.has_baseline ? "true" : "false",
                         (unsigned)st.status, (unsigned)st.stop_reason);
        } else {
            n = snprintf(chunk, sizeof(chunk), "%s{\"zone\":%u,\"enabled\":false,\"persisted\":false}",
                         (z == 0) ? "" : ",", (unsigned)z);
        }
        if (n < 0 || (size_t)n >= sizeof(chunk)) {
            ESP_LOGE(TAG, "status row overflowed for zone %u", (unsigned)z);
            continue;
        }
        httpd_resp_sendstr_chunk(req, chunk);
    }
    httpd_resp_sendstr_chunk(req, "]}");
    return httpd_resp_sendstr_chunk(req, NULL);
}

// POST /api/iter_tune/restore_commissioned?zone=N -- the ONLY sanctioned
// write path from this HTTP surface into live PID gains: build a transient,
// mostly-zeroed iter_tune_zone_state_t from the persisted anchor/baseline,
// call iter_tune_restore_commissioned() (never propose/process -- forbidden
// for this file, see iter_tune_write_surface_check.py), apply the returned
// gains via zones_config_set_pid() (iter_tune.c's own documented INTEGRATION
// POINT setter), then persist the resulting OFF/never-enabled state.
static esp_err_t iter_tune_restore_post_handler(httpd_req_t *req)
{
    char query[32];
    long zone = -1;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[8];
        if (httpd_query_key_value(query, "zone", val, sizeof(val)) == ESP_OK) {
            if (!http_form_parse_long(val, (int)strlen(val), 0, 255, &zone)) {
                zone = -1;
            }
        }
    }
    char json[160];
    int n;
    if (zone < 0 || zone >= MAX31856_CHANNEL_COUNT) {
        n = snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"missing or out-of-range zone\"}");
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, json, n < 0 ? 0 : (size_t)n);
    }

    /* Owner decision Q2 (docs/SYSTEM_MODE_GATE.md, 2026-09-25,
     * gate-slices-2/4/5 spec): this is a live PID-gain write into
     * zones_config (zones_config_set_pid() below, this file's "ONLY
     * sanctioned write path" per the header comment above) -- refuse it
     * while a firing or autotune run is active, PAUSED included, same rule
     * and snapshot accessor as every other wired call site. Checked before
     * touching the iter_tune store or reserving the zone for external write. */
    {
        sys_mode_snapshot_t mode_snap = { 0 };
        relay_authority_heat_run_active(&mode_snap.profile_running, &mode_snap.autotune_running);
        char mode_reason[SYSTEM_MODE_GATE_REASON_MAX];
        mode_reason[0] = '\0';
        if (system_mode_gate_check(SYS_ACTION_WRITE_ZONES_CONFIG, &mode_snap, mode_reason, sizeof(mode_reason))) {
            ESP_LOGW(TAG, "POST /api/iter_tune/restore_commissioned zone=%ld refused by system mode gate: %s",
                     zone, mode_reason);
            return system_mode_gate_http_send_refusal(req, mode_reason);
        }
    }

    // cfg is the only persistence target for the iter_tune record
    // (docs/CONFIG_FILESYSTEM.md, "Dual-write window: closed"): refuse BEFORE
    // applying the restored gains, so a restore is never half done.
    if (cfg_fs_http_refuse_if_unmounted(req)) {
        return ESP_OK;
    }

    iter_tune_store_zone_t stored;
    bool present = iter_tune_store_get_zone((uint8_t)zone, &stored);
    if (!present || (!stored.has_anchor && !stored.has_baseline)) {
        n = snprintf(json, sizeof(json),
                     "{\"ok\":false,\"error\":\"no commissioned/anchored gains persisted for this zone\"}");
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, json, n < 0 ? 0 : (size_t)n);
    }

    // A1 (step 7 review, 2026-09-23) + the follow-up step-7 review race
    // (2026-09-23): refuse while autotune owns this zone, AND close the
    // window between that check and the zones_config_set_pid()/persist below
    // where autotune could start and race the restore -- autotune_engine_
    // guard.c's accept path would silently overwrite whatever gains we are
    // about to restore, and iter_tune has no way to find out its work was
    // clobbered (or vice versa). autotune_engine_reserve_zone_for_external_
    // write() combines the check with a reservation held under s_at.lock
    // that BOTH autotune_begin_run_locked() (a fresh START) AND autotune_
    // engine_accept() (a DONE run committing its gains) consult at their own
    // commit point, so either kind of concurrent write is refused rather
    // than racing this handler. MUST be paired with autotune_engine_
    // release_zone_for_external_write() on every exit path below once this
    // succeeds.
    //
    // NOT gated by this reservation (opus review 2026-09-23, second pass):
    // adaptive_tune_model.c's SIMC-refine write (a separate module with its
    // own lock domain, no access to s_at without an unwanted coupling) and
    // uart_bridge_ext_control.c's CONTROL_CMD_SET_ZONE_PID benchproto command
    // (a raw passthrough to zones_config_set_pid() with no autotune
    // awareness at all). Both remain a live, un-closed instance of this same
    // class of race against a restore; narrowing this comment rather than
    // claiming a broader interlock than actually exists.
    if (!autotune_engine_reserve_zone_for_external_write((uint8_t)zone)) {
        n = snprintf(json, sizeof(json),
                     "{\"ok\":false,\"error\":\"autotune is active on this zone, or another restore is "
                     "already in progress\"}");
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, json, n < 0 ? 0 : (size_t)n);
    }

    iter_tune_zone_state_t transient = {0};
    transient.has_anchor = stored.has_anchor;
    if (stored.has_anchor) {
        transient.anchor.kp = stored.anchor_kp;
        transient.anchor.ki = stored.anchor_ki;
        transient.anchor.kd = stored.anchor_kd;
    }
    transient.has_baseline = stored.has_baseline;
    if (stored.has_baseline) {
        transient.baseline.kp = stored.baseline_kp;
        transient.baseline.ki = stored.baseline_ki;
        transient.baseline.kd = stored.baseline_kd;
    }

    iter_tune_gains_t restored = iter_tune_restore_commissioned(&transient);
    bool applied = zones_config_set_pid((uint8_t)zone, restored.kp, restored.ki, restored.kd);

    if (!applied) {
        // step 7 review, 2026-09-23, finding 2: a refused zones_config_set_pid
        // must not be recorded as if the restore happened -- the persisted
        // record is left completely untouched (not even the OFF/status
        // fields), so a retried restore still sees the original anchor.
        autotune_engine_release_zone_for_external_write((uint8_t)zone);
        n = snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"zones_config_set_pid refused\"}");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, json, n < 0 ? 0 : (size_t)n);
    }

    // step 7 review, 2026-09-23, finding 2 (reset-one-side class):
    // iter_tune_restore_commissioned() sets state->baseline = the restored
    // gains (iter_tune.c:139-147) -- that is now what step 8's
    // iter_tune_active_gains() would return for this zone, so the persisted
    // record's baseline_* must be updated to match, not left holding the
    // discarded pre-restore baseline.
    stored.has_baseline = 1;
    stored.baseline_kp = transient.baseline.kp;
    stored.baseline_ki = transient.baseline.ki;
    stored.baseline_kd = transient.baseline.kd;
    stored.enabled = 0;
    stored.status = transient.status;       // OFF, per iter_tune_restore_commissioned()
    stored.stop_reason = transient.stop_reason; // NONE
    esp_err_t persist_err = iter_tune_store_set_zone((uint8_t)zone, &stored);

    // Reservation held across both the live-gains write above and this
    // persisted-record write -- released only once the whole restore is
    // durably done, success or not.
    autotune_engine_release_zone_for_external_write((uint8_t)zone);

    if (persist_err != ESP_OK) {
        // The live gains were applied, but the persisted OFF/baseline record
        // was not written: say so as a failure, never a success with a warning.
        ESP_LOGE(TAG, "restore_commissioned zone=%ld: gains applied but persisting the OFF state failed (%d)", zone,
                 (int)persist_err);
        return cfg_fs_http_persist_failed_for(req, ITER_TUNE_CFG_FILE_PATH, persist_err);
    } else {
        n = snprintf(json, sizeof(json), "{\"ok\":true,\"kp\":%.6f,\"ki\":%.6f,\"kd\":%.6f}",
                     (double)restored.kp, (double)restored.ki, (double)restored.kd);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, (n > 0 && (size_t)n < sizeof(json)) ? (size_t)n : 0);
}

esp_err_t iter_tune_http_start(void)
{
    iter_tune_store_start();
    // ITER_TUNE_REDESIGN.md step 8: load shadow mode's verdict-summary
    // counters once here, at boot, so the status GET handler never has to
    // load them lazily from the httpd task (see firing_shadow_get_status()).
    firing_shadow_store_start();

    httpd_handle_t server = wifi_provision_http_get_server();
    if (server == NULL) {
        ESP_LOGE(TAG, "no httpd server -- cannot register iter_tune routes");
        return ESP_FAIL;
    }

    static const httpd_uri_t status_uri = {
        .uri = "/api/iter_tune/status", .method = HTTP_GET, .handler = iter_tune_status_get_handler,
    };
    static const httpd_uri_t restore_uri = {
        .uri = "/api/iter_tune/restore_commissioned", .method = HTTP_POST,
        .handler = iter_tune_restore_post_handler,
    };

    esp_err_t err = kiln_http_register(server, &status_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(GET /api/iter_tune/status) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = kiln_http_register(server, &restore_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(POST /api/iter_tune/restore_commissioned) failed: %s",
                 esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}
