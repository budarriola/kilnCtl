// POST /api/zones/pid narrow handler, split off zones_http_handlers.c
// (2026-09-04, ROADMAP.md M15's 1500-line item) -- see zones_http_internal.h
// for the full split map. MOVE-ONLY: zones_pid_post_handler(), unchanged,
// still declared non-static (as it already was) in zones_http_internal.h.
// See this handler's own header comment (below) for why it is a separate
// endpoint from POST /api/zones rather than a narrowed interlock inside it.

#include "zones_http_internal.h"

#include "esp_log.h"

/* ---- POST /api/zones/pid --------------------------------------------------
 * Narrow, deliberate exception to POST /api/zones's interlock: PID gain
 * (kp/ki/kd) edits ONLY, allowed even while a firing is RUNNING/PAUSED.
 *
 * WHY A SEPARATE ENDPOINT, NOT A DIFF AGAINST THE WHOLE-PAGE SUBMIT: this
 * handler's request body can name nothing but zone/kp/ki/kd -- there is no
 * relay_mask, no control_mode, no guard threshold, no coupling cell for it
 * to carry even by accident. A "narrowed interlock" living inside
 * zones_post_handler() instead would have to compare a freshly-parsed
 * zones_cfg_t against the live one field-by-field and prove every OTHER
 * field is byte-identical before allowing the submit through -- correct
 * only if that comparison names every field that exists today AND every
 * field this file grows later (this module has added a field practically
 * every week: fuzzy_strength_pct, coupling_coeff[], settings_source, the
 * plant model, the nine v8 guard overrides now living on timing profiles
 * ...). Forgetting one there is a silent hole in a safety interlock. This
 * endpoint cannot develop that hole: it has no field to forget, and
 * everything else this module owns keeps going through
 * ota_http_check_interlocks() exactly as it does today, completely
 * unmodified by this addition.
 *
 * WHAT STAYS REFUSED WHILE RUNNING, unconditionally, by construction (not by
 * a check that could be wrong): relay_mask/zone membership, control_mode,
 * max_temp_c/min_temp_c, max_ramp_c_per_hr, cal_offset_c, every guard
 * threshold (guard_wrong_dir_window_s, guard_wrong_dir_rate_c_per_min,
 * guard_off_settle_s, guard_runaway_rate_c_per_min, guard_runaway_margin_c,
 * guard_drift_period_s, guard_sensor_fault_debounce_ticks,
 * guard_frozen_window_s, cross_zone_max_delta_c), coupling_coeff[], the plant model
 * (model_k_dc/model_tau_s/model_dead_time_s), thermo_mask/ct_mask,
 * settings_source, timing_profile, and anything safety_link.c mirrors to
 * the RP2040 (safety_tc_type, the guard thresholds it reads back via
 * zones_config_get_*) -- none of those fields has an HTTP key this handler
 * even looks for, let alone writes.
 *
 * VALIDATION: parses kp/ki/kd with zones_config_json_parse_float_field(),
 * the EXACT function and EXACT bound (ZONE_PID_GAIN_MAX, zones_http.h) that
 * parse_zone_fields() uses for the identical fields on the whole-page path
 * -- not a second, hand-rolled range check. zones_config_set_pid() (the
 * single setter both paths end in) enforces the same bound again as a
 * second line of defense, so a future caller of that setter that isn't an
 * HTTP handler at all still cannot exceed it either.
 *
 * PERSISTENCE / BUMPLESS: zones_config_set_pid() bumps s_config_generation
 * before calling nvs_save() -- profile_executor.c's reload_config_if_changed()
 * (called once per control tick) compares that counter and, finding it
 * advanced, re-reads the gains via zones_config_get_pid() and re-seeds the
 * integral with seed_bumpless_with_ff() so the commanded duty does not step.
 * That reseed is skipped -- falling back to a COLD pid_reset() instead --
 * whenever the zone's last reading was invalid (z->actual_valid false) at
 * the moment the new gains are noticed; see reload_config_if_changed()'s own
 * comment. A PID edit landing during exactly that window is NOT bumpless: a
 * sensor fault at gain-reload time can still produce a duty step from this
 * endpoint, same as it would from the LCD UI's existing zones_config_set_pid()
 * caller. Not fixed here -- it is profile_executor.c, which this task does
 * not own -- but worth the dashboard surfacing a warning next to this
 * control, not just accepting the value silently.
 *
 * TASK/FLASH SAFETY: this handler runs on the httpd_worker task, the SAME
 * task POST /api/zones already runs zones_post_handler() -- and that
 * existing handler already calls nvs_save() directly, unwrapped by
 * uart_bridge_ext.c's bx_run_on_internal_stack() worker, with no reported
 * hazard. uart_bridge_ext.c's own HAZARD comment (~line 91) names exactly
 * which tasks the PSRAM-stack/flash-cache assert was reproduced on --
 * control_task, profiles_task, autotune_task, wifi_uart_bridge -- tasks THIS
 * file creates itself with xTaskCreatePinnedToCore(..., MALLOC_CAP_SPIRAM,
 * ...); httpd_worker is not among them, is not created by this codebase at
 * all (esp_http_server owns it, sized by wifi_provision_http.c's
 * config.stack_size = 8192, an ordinary xTaskCreate stack), and the
 * existing whole-page zones_post_handler() -- unwrapped -- is the strongest
 * evidence available that this task's stack is not PSRAM. Confirmed by
 * inspection of wifi_provision_http.c's httpd_start() call site (no
 * task_caps/PSRAM stack config passed) and by dashboard_http.c's own
 * "httpd_worker measured at 64 bytes free of 8192" comment discussing the
 * SAME task/stack for other handlers that also touch NVS on this path
 * (autotune_matrix, zones_get_handler, etc.). */
#define ZONES_PID_BODY_MAX 128

esp_err_t zones_pid_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > ZONES_PID_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }
    char body[ZONES_PID_BODY_MAX + 1];
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            ESP_LOGW(ZONES_HTTP_TAG, "zones/pid body read failed/short: %d", ret);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body read failed");
            return ESP_OK;
        }
        received += (size_t)ret;
    }
    body[received] = '\0';

    uint8_t zone_index;
    if (!zones_config_json_parse_u8_field(body, "zone", 0, MAX31856_CHANNEL_COUNT - 1, &zone_index)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "zone missing or out of range");
        return ESP_OK;
    }
    if (zone_index >= s_zones.cfg.thermo_count) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "zone is not configured");
        return ESP_OK;
    }
    /* Same parser, same bound (ZONE_PID_GAIN_MAX) as parse_zone_fields()'s
     * z%u_kp/z%u_ki/z%u_kd handling above -- see this handler's own header
     * comment. Ki in particular is routinely ~1e-4 on this hardware (a
     * measured autotune result, not a typo) -- zones_config_json_parse_float_field()
     * parses with strtof() into a float, so a small magnitude like that is
     * carried exactly, not rounded toward zero by this parse step. */
    float kp, ki, kd;
    if (!zones_config_json_parse_float_field(body, "kp", 0.0f, ZONE_PID_GAIN_MAX, &kp)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "kp missing or out of range");
        return ESP_OK;
    }
    if (!zones_config_json_parse_float_field(body, "ki", 0.0f, ZONE_PID_GAIN_MAX, &ki)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ki missing or out of range");
        return ESP_OK;
    }
    if (!zones_config_json_parse_float_field(body, "kd", 0.0f, ZONE_PID_GAIN_MAX, &kd)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "kd missing or out of range");
        return ESP_OK;
    }

    if (!zones_config_set_pid(zone_index, kp, ki, kd)) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"failed to apply/persist PID gains\"}");
    }
    ESP_LOGI(ZONES_HTTP_TAG, "POST /api/zones/pid: zone %u gains -> kp=%.6g ki=%.6g kd=%.6g", zone_index, (double)kp,
             (double)ki, (double)kd);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

