// GET side of zones_http_handlers.c's split (2026-09-04, ROADMAP.md M15's
// 1500-line item) -- see zones_http_internal.h for the full split
// map. MOVE-ONLY: page_get_handler()/zones_json_escape()/
// safety_config_page_get_handler()/zones_get_handler(), unchanged, still
// declared non-static (as they already were) in zones_http_internal.h so
// zones_http.c's route table and zones_http_post_parse.c's
// zones_json_escape() caller keep reaching them the same way.

#include "zones_http_internal.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "MAX31856.h"
#include "web_encoding.h"

/* Embedded via EMBED_TXTFILES, pre-gzipped at configure time by
 * App/drivers/CMakeLists.txt -- same convention as every other *_page.html
 * in this component (see web_encoding.h's header comment for the flash-
 * budget reasoning that makes gzip the only stored representation). */
extern const uint8_t zones_page_html_gz_start[] asm("_binary_zones_page_html_gz_start");
extern const uint8_t zones_page_html_gz_end[] asm("_binary_zones_page_html_gz_end");
/* The safety-timings page (/settings/safety). Served from this file rather
 * than a module of its own because it edits the same zones_cfg_t record
 * through the same /api/zones endpoint -- see the page's own header comment
 * on why the two pages have to echo each other's fields back. */
extern const uint8_t safety_config_page_html_gz_start[] asm("_binary_safety_config_page_html_gz_start");
extern const uint8_t safety_config_page_html_gz_end[] asm("_binary_safety_config_page_html_gz_end");

/* ---- HTTP handlers --------------------------------------------------------- */

/* Same content-negotiation shape as diagnostics_http.c's send_gz_page():
 * web_client_accepts_gzip() covers the "no Accept-Encoding header" (legal,
 * served gzip per RFC 9110 s12.5.3) and "header present but excludes gzip"
 * (406, since this server keeps no uncompressed copy) cases; see
 * web_encoding.h for the full rationale. */
esp_err_t page_get_handler(httpd_req_t *req)
{
    if (!web_client_accepts_gzip(req)) {
        return web_send_gzip_not_acceptable(req, ZONES_HTTP_TAG, "zones_page.html");
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    web_set_asset_cache_headers(req);
    return httpd_resp_send(req, (const char *)zones_page_html_gz_start,
                           (size_t)(zones_page_html_gz_end - zones_page_html_gz_start));
}

/* Same escaping convention as wifi_provision_http.c's json_escape -- a zone
 * name came from a POST body at some point, so it's untrusted-ish. Named
 * zones_json_escape (not json_escape) because dashboard_json.c has its own
 * copy with external linkage too; two same-named externally-linked symbols
 * collide at link time. */
void zones_json_escape(const char *src, char *out, size_t out_cap)
{
    size_t o = 0;
    for (const char *p = src; *p && o + 2 < out_cap; p++) {
        if (*p == '"' || *p == '\\') {
            if (o + 3 >= out_cap) {
                break;
            }
            out[o++] = '\\';
        }
        out[o++] = *p;
    }
    out[o] = '\0';
}

esp_err_t safety_config_page_get_handler(httpd_req_t *req)
{
    if (!web_client_accepts_gzip(req)) {
        return web_send_gzip_not_acceptable(req, ZONES_HTTP_TAG, "safety_config_page.html");
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    web_set_asset_cache_headers(req);
    return httpd_resp_send(req, (const char *)safety_config_page_html_gz_start,
                           (size_t)(safety_config_page_html_gz_end - safety_config_page_html_gz_start));
}

esp_err_t zones_get_handler(httpd_req_t *req)
{
    /* HEAP, not stack -- this was easily the largest single transient
     * buffer on the httpd_worker task's request path (dashboard_http.c's
     * profile_exec/control/autotune_matrix/profile_plan handlers all share
     * this same task and stack; httpd_worker was measured at 64 bytes free
     * of 8192 live). At 5760 bytes this one buffer alone was more than 70%
     * of the entire 8192-byte task stack. Freed on every return path
     * (success and truncated). */
    const size_t json_cap = 7360; /* heap buffer (heap_caps_malloc below, not
                      * stack). 6528 -> 7360 (2026-09-01, ZONES_CFG_VERSION
                      * 12->13, the tuning-quality record): 11 new keys a
                      * zone (tuning_valid/method/rule/settled/extrapolation_
                      * converged/tau_consistent/baseline_c/step_ambient_c/
                      * raw_rise_c/rise_inf_c/seq), worst case ~35 bytes each
                      * ("tuning_extrapolation_converged":false, is the
                      * longest) = ~385 bytes/zone x MAX31856_CHANNEL_COUNT
                      * (3) zones = ~1155 bytes, leaving ~677 bytes of
                      * headroom in the 832 bytes this bump grants.
                      * 5760 -> 6528 (2026-08-31, ZONES_CFG_VERSION
                      * 11->12): coupling_tau_c%u/coupling_dead_time_c%u add
                      * two more indexed keys per cell alongside coupling_c%u.
                      * Worst case per cell is both new keys at once:
                      * "coupling_tau_c%u":86400.0, (~26 bytes) plus
                      * "coupling_dead_time_c%u":86400.0, (~32 bytes) = ~58
                      * bytes/cell x MAX31856_CHANNEL_COUNT^2 (9) cells =
                      * ~522 bytes, leaving ~246 bytes of headroom in the 768
                      * bytes this bump grants -- still adequate, not
                      * changing the size, just correcting this estimate.
                      * 5632 -> 5760 (2026-08-30, same-day follow-up,
                      * ZONES_CFG_VERSION 10->11): coupling_coeff/
                      * coupling_neighbor_zone (2 keys) replaced by
                      * MAX31856_CHANNEL_COUNT indexed coupling_c%u keys (3
                      * keys, ~20 bytes each worst case) -- net +1 key/zone,
                      * ~60 bytes across 3 zones, rounded up generously.
                      * 5120 -> 5632 (2026-08-30, PID_EXPANSION_PLAN.md Phase 4):
                      * four new per-zone keys/values (fuzzy_strength_pct,
                      * coupling_coeff, coupling_neighbor_zone, settings_source
                      * -- ~130 bytes a zone at worst, MAX31856_CHANNEL_COUNT
                      * zones), comfortably inside this bump.
                      * 4608 -> 5120 (2026-08-27+2, Tasks 1/2/3): one
                      * top-level safety_wiring object (~110 bytes) plus
                      * ct_warn_mask (~20 bytes), and two new per-zone keys
                      * (normal_current_measured/normal_current_a, ~50 bytes
                      * a zone) -- comfortably inside the ~500 bytes of
                      * headroom this bumps by.
                      * 4352 -> 4608 (2026-08-27, ZONES_CFG_VERSION 8->9,
                      * timing profiles): the nine v8 override keys/values move
                      * OFF each zone object and onto a new top-level
                      * timing_profiles array, one object per profile
                      * (name + the same nine keys/values that used to be
                      * inline on every zone, up to MAX31856_CHANNEL_COUNT of
                      * them) -- roughly what the per-zone removal frees up,
                      * since worst case (every zone its own profile) is the
                      * same MAX31856_CHANNEL_COUNT repeats of the same nine
                      * keys either way. Each zone object shrinks by those nine
                      * keys/values (~250 bytes) and grows by one
                      * "timing_profile":%u (~20 bytes); the net is a modest
                      * increase, not a decrease, because the new
                      * timing_profiles array also carries a name string per
                      * profile that the old inline fields never had. Sized
                      * with headroom rather than computed exactly, same
                      * discipline as every prior bump of this buffer.
                      * 2816 -> 4352 (2026-08-27) with the nine v8 per-zone
                      * overrides plus one global: nine float keys a zone,
                      * whose names alone run ~250 bytes before any values.
                      * 2688 -> 2816 (2026-08-27) with ct_mask: one small integer
                      * key/value per zone, well under the 128 bytes added.
                      * 1024 -> 1536 with heater_window_ms/min_on_ms/min_off_ms,
                      * 1536 -> 1792 with cross_zone_max_delta_c,
                      * 1792 -> 2048 with the three plant-model fields (their
                      * key names alone are ~50 bytes a zone before values),
                      * 2048 -> 2560 with the 8 guard-threshold overrides,
                      * 2560 -> 2688 with tc_type/safety_tc_type (2026-08-21):
                      * one small integer per zone plus one top-level field.
                      * thermo_mask (TODO.md 10.8) added ~20 bytes/zone --
                      * left inside the existing 2560 headroom rather than
                      * bumped again, MAX31856_CHANNEL_COUNT zones' worth of
                      * one small integer key is nowhere near what's left. */
    char *json = heap_caps_malloc(json_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (json == NULL) {
        ESP_LOGE(ZONES_HTTP_TAG, "GET /api/zones: malloc(%u) failed for the response buffer", (unsigned)json_cap);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req,
                                  "{\"ok\":false,\"error\":\"out of memory building the response\"}");
    }
    size_t o = 0;
    int n;

#define APPEND(...)                                                                              \
    do {                                                                                          \
        n = snprintf(json + o, json_cap - o, __VA_ARGS__);                                   \
        if (n < 0 || (size_t)n >= json_cap - o) {                                             \
            goto truncated;                                                                            \
        }                                                                                          \
        o += (size_t)n;                                                                            \
    } while (0)

    /* Task 3 (2026-08-27+2): the safety thermocouple/relay's LIVE reading,
     * read-only -- computed BEFORE the APPEND below so its values are
     * ordinary args, same as every other field here. See
     * zone_safety_wiring_t's doc comment for why UNSET (tc_temp_valid
     * false) renders distinct from a real 0/false rather than being folded
     * into the same field. Task 2 (2026-08-27+2): ct_warn_mask, computed the
     * same way -- bit N-1 = zone N is currently commanded on and its live
     * current mismatches its Task 1 measured normal, a WARNING never a trip
     * (zones_ct_mapping_mismatch()'s doc comment); 0 for any zone whose
     * normal was never measured. */
    zone_safety_wiring_t safety_wiring;
    zones_get_safety_wiring(&safety_wiring);
    uint8_t ct_warn_mask = zones_ct_mapping_warn_mask();

    APPEND("{\"thermo_count\":%u,\"relay_count\":%u,\"max_simultaneous_relays\":%u,"
           "\"continue_on_zone_trip\":%s,\"safety_tc_type\":%u,"
           "\"pc_link_abort_silence_ms\":%.0f,"
           /* 2026-08-27+1 (owner request: name relays that are NOT in any
            * zone): relay_zone_owned_mask lets the page tell, without its
            * own recompute, which of relay_names[] below it should render
            * editable -- bit N-1 set iff relay N is currently claimed by
            * SOME zone (zone_owned_relay_mask(), same rule
            * rules_task.c/rules_http.c already enforce server-side). The
            * page still recomputes this live client-side too, from its own
            * in-progress checkbox state -- see zones_page.html's
            * clientOwnedRelayMask() -- since an operator can retick a relay
            * checkbox after this GET without reloading; this field is what a
            * fresh load (or a consumer that isn't this page) sees.
            * relay_names is DENSE over KILN_IO_RELAY_COUNT, index r == bit r
            * (relay r+1 on the wire), always emitted regardless of
            * relay_count or ownership -- a name persists whether or not its
            * relay is in use today, same "always emit, let the page decide
            * what to show" convention as zone_cfg_t's model_k_dc/etc above. */
           "\"relay_zone_owned_mask\":%u,"
           "\"safety_wiring\":{\"link_up\":%s,\"tc_temp_valid\":%s,\"tc_temp_c\":%.1f,"
           "\"tc_fault\":%u,\"relay_energized\":%s,\"tc_is_separate_sensor\":%s},"
           "\"ct_warn_mask\":%u,"
           "\"relay_names\":[",
           s_zones.cfg.thermo_count, s_zones.cfg.relay_count, s_zones.cfg.max_simultaneous_relays,
           s_zones.cfg.continue_on_zone_trip ? "true" : "false", s_zones.cfg.safety_tc_type,
           (double)s_zones.cfg.pc_link_abort_silence_ms,
           zone_owned_relay_mask(&s_zones.cfg),
           safety_wiring.link_up ? "true" : "false", safety_wiring.tc_temp_valid ? "true" : "false",
           (double)safety_wiring.tc_temp_c, safety_wiring.tc_fault, safety_wiring.relay_energized ? "true" : "false",
           safety_wiring.tc_is_separate_sensor ? "true" : "false",
           ct_warn_mask);

    for (uint8_t r = 0; r < KILN_IO_RELAY_COUNT; r++) {
        char rn_escaped[RELAY_NAME_MAX_LEN * 2 + 1];
        zones_json_escape(s_relay_names.cfg.names[r], rn_escaped, sizeof(rn_escaped));
        APPEND("%s\"%s\"", r == 0 ? "" : ",", rn_escaped);
    }
    APPEND("],\"timing_profiles\":[");
    /* 2026-08-27 (ZONES_CFG_VERSION 8->9): exactly timing_profile_count
     * entries, never padded out to MAX31856_CHANNEL_COUNT -- the page derives
     * "how many profiles exist" from this array's length (the same way it
     * already derives "how many zones exist" from thermo_count, not from
     * zones[]'s fixed capacity), and a POST replays that same dense
     * 0..count-1 range back as tp0_name.. (see zones_post_handler()'s own
     * comment on that loop). */
    for (uint8_t p = 0; p < s_zones.cfg.timing_profile_count; p++) {
        const zone_timing_profile_t *tp = &s_zones.cfg.timing_profiles[p];
        char tp_name_escaped[TIMING_PROFILE_NAME_MAX_LEN * 2 + 1];
        zones_json_escape(tp->name, tp_name_escaped, sizeof(tp_name_escaped));
        APPEND(
            "%s{\"index\":%u,\"name\":\"%s\","
            "\"guard_progress_duty_min\":%.3f,\"guard_progress_window_s\":%.1f,"
            "\"guard_drift_hysteresis_c\":%.1f,\"guard_frozen_eps_c\":%.3f,"
            "\"guard_cross_zone_period_s\":%.1f,\"bangbang_hysteresis_c\":%.1f,"
            "\"cooling_limited_margin_c\":%.1f,\"cooling_limited_hold_s\":%.1f,"
            "\"ramp_lock_band_c\":%.1f}",
            p == 0 ? "" : ",", p, tp_name_escaped,
            (double)tp->guard_progress_duty_min, (double)tp->guard_progress_window_s,
            (double)tp->guard_drift_hysteresis_c, (double)tp->guard_frozen_eps_c,
            (double)tp->guard_cross_zone_period_s, (double)tp->bangbang_hysteresis_c,
            (double)tp->cooling_limited_margin_c, (double)tp->cooling_limited_hold_s,
            (double)tp->ramp_lock_band_c);
    }
    APPEND("],\"zones\":[");
    for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
        const zone_cfg_t *z = &s_zones.cfg.zones[i];
        char name_escaped[ZONE_NAME_MAX_LEN * 2 + 1];
        zones_json_escape(z->name, name_escaped, sizeof(name_escaped));
        /* Task 1 (2026-08-27+2): this zone's measured normal current, if
         * any -- read-only, never round-tripped through POST (it is
         * measured data, not an operator-entered field; see
         * zones_config_get_normal_current()'s doc comment). */
        float normal_a = 0.0f;
        bool normal_measured = false;
        zones_config_get_normal_current(i, &normal_a, &normal_measured);
        APPEND(
            "%s{\"index\":%u,\"name\":\"%s\",\"relay_mask\":%u,\"thermo_mask\":%u,\"cal_offset_c\":%.3f,"
            "\"pid_kp\":%.4f,\"pid_ki\":%.4f,\"pid_kd\":%.4f,\"max_ramp_c_per_hr\":%.2f,"
            "\"sanity_rate_c_per_min\":%.3f,\"control_mode\":%u,\"max_temp_c\":%.1f,"
            "\"min_temp_c\":%.1f,\"heater_window_ms\":%.0f,\"heater_min_on_ms\":%.0f,"
            "\"heater_min_off_ms\":%.0f,"
            "\"guard_wrong_dir_window_s\":%.1f,\"guard_wrong_dir_rate_c_per_min\":%.3f,"
            "\"guard_off_settle_s\":%.1f,\"guard_runaway_rate_c_per_min\":%.3f,"
            "\"guard_runaway_margin_c\":%.1f,\"guard_drift_period_s\":%.1f,"
            "\"guard_sensor_fault_debounce_ticks\":%.0f,\"guard_frozen_window_s\":%.1f,"
            "\"cross_zone_max_delta_c\":%.1f,"
            /* Emitted for every zone whether or not a model exists -- an
             * absent key and a zero would mean the same thing to a client,
             * and always emitting keeps the page's read-back-and-repost
             * round-trip (see the POST side) from depending on which zones
             * happen to have been autotuned. %.4f on K because a small-gain
             * zone's fit can land in the fractional range and the
             * feedforward divides by it. */
            "\"model_k_dc\":%.4f,\"model_tau_s\":%.1f,\"model_dead_time_s\":%.1f,"
            /* tc_type is CONFIG, not a live reading, so it deliberately does
             * NOT go anywhere near kc-live-value on the page -- see
             * zones_page.html's rendering of this field. */
            "\"tc_type\":%u,\"ct_mask\":%u,"
            /* 2026-08-27 (ZONES_CFG_VERSION 8->9): replaces the nine v8
             * override fields that used to be inline here -- see this zone's
             * chosen profile in the top-level timing_profiles array above.
             * Always emitted (even for a zone past thermo_count), same
             * round-trip reasoning as every other always-emitted field here:
             * the page reads this back and reposts it. */
            /* PID_EXPANSION_PLAN.md Phase 4 (2026-08-30): always emitted for
             * every zone, same "read-back-and-repost round-trip" reasoning as
             * model_k_dc/etc above -- a page that reads this back and posts
             * it straight through untouched must never see an absent key
             * mean something different from a zero. */
            "\"timing_profile\":%u,\"normal_current_measured\":%s,\"normal_current_a\":%.3f,"
            /* RELAY_LIFE_BUDGET_PLAN.md step 2 (ZONES_CFG_VERSION 19->20):
             * always emitted, same read-back-and-repost round-trip reasoning
             * as every other always-emitted field above -- the page reads
             * this back and reposts it as z%u_relaytype (POST side:
             * zones_http_post_parse.c). */
            "\"relay_type\":%u,"
            "\"fuzzy_strength_pct\":%.2f,",
            i == 0 ? "" : ",", i, name_escaped, z->relay_mask, z->thermo_mask, (double)z->cal_offset_c,
            (double)z->pid_kp, (double)z->pid_ki, (double)z->pid_kd, (double)z->max_ramp_c_per_hr,
            (double)z->sanity_rate_c_per_min, z->control_mode, (double)z->max_temp_c,
            (double)z->min_temp_c, (double)z->heater_window_ms, (double)z->heater_min_on_ms,
            (double)z->heater_min_off_ms,
            (double)z->guard_wrong_dir_window_s, (double)z->guard_wrong_dir_rate_c_per_min,
            (double)z->guard_off_settle_s, (double)z->guard_runaway_rate_c_per_min,
            (double)z->guard_runaway_margin_c, (double)z->guard_drift_period_s,
            (double)z->guard_sensor_fault_debounce_ticks, (double)z->guard_frozen_window_s,
            (double)z->cross_zone_max_delta_c, (double)z->model_k_dc,
            (double)z->model_tau_s, (double)z->model_dead_time_s, z->tc_type, z->ct_mask,
            z->timing_profile, normal_measured ? "true" : "false", (double)normal_a,
            z->relay_type, (double)z->fuzzy_strength_pct);
        /* ZONES_CFG_VERSION 14->15 (PID_EXPANSION_PLAN.md 3.2 follow-up): the
         * coupling identification's own diagonal cell -- see zone_cfg_t::
         * coupling_diag_k_dc's own doc comment. Always emitted, same always-
         * emit/read-back-and-repost reasoning as fuzzy_strength_pct/
         * coupling_c%u above. %.4f matches model_k_dc's own precision -- same
         * unit, same small-gain-zone concern. */
        APPEND("\"coupling_diag_k_dc\":%.4f,", (double)z->coupling_diag_k_dc);
        /* ZONES_CFG_VERSION 16->17 (PID_EXPANSION_PLAN.md sec 3.6d): the
         * terminal ease-off taper window multiplier, now per-zone -- was a
         * single top-level "ease_off_window_mult" key applied to every zone
         * (see zone_cfg_t::ease_off_window_mult's own comment for why one
         * number could not give z0 alone a wider window). z%u_easeoffmult on
         * the POST side (parse_zone_fields()) is the matching wire name.
         * Emits the RAW stored value (including the legal 0 sentinel), not
         * the resolved-to-2.0 value zones_config_get_ease_off_window_mult()
         * would answer with -- same "the page shows what is actually stored,
         * not the default it resolves to" convention pc_link_abort_
         * silence_ms's own 0 uses above. Always emitted, same always-emit/
         * read-back-and-repost reasoning as coupling_diag_k_dc/model_k_dc
         * above. */
        APPEND("\"ease_off_window_mult\":%.3f,", (double)z->ease_off_window_mult);
        /* ZONES_CFG_VERSION 17->18 (PID_EXPANSION_PLAN.md sec 3.6d /
         * PER_ZONE_TARGET_DESIGN_STUDY.md option (b)): the per-zone
         * approach-rate cap -- z%u_approachratecap on the POST side
         * (parse_zone_fields()). Emits the RAW stored value (0 = uncapped,
         * the legal sentinel), same "the page shows what is actually
         * stored" convention ease_off_window_mult above uses -- there is no
         * resolved/default value to show instead for this field (see
         * zones_config_get_approach_rate_cap_c_per_hr()'s own comment for
         * why 0 is answered verbatim, not substituted). Always emitted, same
         * always-emit/read-back-and-repost reasoning as every field above. */
        APPEND("\"approach_rate_cap_c_per_hr\":%.3f,", (double)z->approach_rate_cap_c_per_hr);
        /* ZONES_CFG_VERSION 18->19 (PID_EXPANSION_PLAN.md sec 3.6g): the
         * fuzzy-PID membership-band widths -- z%u_errorband/z%u_rateband on
         * the POST side (parse_zone_fields()). Emits the RAW stored value
         * (including the legal 0 sentinel), same "the page shows what is
         * actually stored, not the resolved default" convention ease_off_
         * window_mult above uses (0 means "use the firmware default", see
         * zones_config_get_error_band_c()'s own comment). Always emitted,
         * same always-emit/read-back-and-repost reasoning as every field
         * above. */
        APPEND("\"error_band_c\":%.3f,\"rate_band_c_per_s\":%.4f,",
               (double)z->error_band_c, (double)z->rate_band_c_per_s);
        /* 2026-08-30 (ZONES_CFG_VERSION 10->11): the coupling row, one
         * indexed key per cell (z%u_coupling_c%u is the matching POST-side
         * wire name -- see parse_zone_fields()) rather than a JSON array, so
         * the same key-per-value convention this whole object already uses
         * for every other field extends here too, and a diff between two
         * saved configs stays a per-key diff rather than needing array-aware
         * tooling. Always emitted for every cell including the diagonal
         * (always 0) -- same always-emit, read-back-and-repost reasoning as
         * model_k_dc/timing_profile above. */
        for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
            APPEND("\"coupling_c%u\":%.4f,", j, (double)z->coupling_coeff[j]);
        }
        /* ZONES_CFG_VERSION 11->12: coupling_tau_s[]/coupling_dead_time_s[],
         * same orientation as coupling_c%u just above ([affected][stepped],
         * i.e. row i = affected zone i's row -- NOT the transpose orientation
         * /api/autotune_matrix uses for the RAM-only s_at.coupling matrix,
         * see that endpoint's own comment for why it stays untouched by this
         * pass). Key names deliberately echo model_tau_s/model_dead_time_s's
         * own JSON key style rather than coupling_c%u's "_c" suffix, since
         * these are the tau/L, not another gain. Always emitted, same
         * always-emit/round-trip reasoning as coupling_c%u. */
        for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
            APPEND("\"coupling_tau_c%u\":%.1f,\"coupling_dead_time_c%u\":%.1f,", j,
                   (double)z->coupling_tau_s[j], j, (double)z->coupling_dead_time_s[j]);
        }
        /* ZONES_CFG_VERSION 12->13: the tuning-quality record (set 1 -- see
         * zone_cfg_t::tuning_valid's own doc comment), read-only here (the
         * POST side never accepts these back -- see this endpoint's own
         * comment on normal_current_measured/normal_current_a above for the
         * identical "measured data, not an operator-entered field"
         * reasoning). Always emitted, tuning_valid included, so the page can
         * tell "measured" from "unknown" without a missing key meaning
         * something different from a present-but-invalid one -- same
         * always-emit convention as model_k_dc/etc. */
        APPEND("\"tuning_valid\":%s,\"tuning_method\":%u,\"tuning_rule\":%u,"
               "\"tuning_settled\":%s,\"tuning_extrapolation_converged\":%s,\"tuning_tau_consistent\":%s,"
               "\"tuning_baseline_c\":%.2f,\"tuning_step_ambient_c\":%.2f,"
               "\"tuning_raw_rise_c\":%.2f,\"tuning_rise_inf_c\":%.2f,\"tuning_seq\":%u,",
               z->tuning_valid ? "true" : "false", z->tuning_method, z->tuning_rule,
               z->tuning_settled ? "true" : "false", z->tuning_extrapolation_converged ? "true" : "false",
               z->tuning_tau_consistent ? "true" : "false",
               (double)z->tuning_baseline_c, (double)z->tuning_step_ambient_c,
               (double)z->tuning_raw_rise_c, (double)z->tuning_rise_inf_c, (unsigned)z->tuning_seq);
        APPEND("\"settings_source\":%u}", z->settings_source);
    }
    APPEND("]}");

#undef APPEND

    httpd_resp_set_type(req, "application/json");
    {
        esp_err_t ret = httpd_resp_send(req, json, o);
        free(json);
        return ret;
    }

    /* Reached only if `json` is too small for the config it holds. The old
     * behaviour was to send what had been written so far, which is a truncated
     * JSON document: the page's fetch throws on it, and the operator sees a
     * settings page stuck on "Loading" with no idea their zone config is fine
     * and only the response was too big. A 500 with a valid body at least says
     * what happened. Sizing `json` is the actual fix; this is the guard that
     * makes an undersized buffer visible instead of silent. */
truncated:
    ESP_LOGE(ZONES_HTTP_TAG, "GET /api/zones did not fit in %u bytes -- raise the buffer", (unsigned)json_cap);
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    {
        esp_err_t ret = httpd_resp_sendstr(req,
                                  "{\"ok\":false,\"error\":\"zone config did not fit in the response "
                                  "buffer -- this is a firmware sizing bug, not a bad configuration\"}");
        free(json);
        return ret;
    }
}
