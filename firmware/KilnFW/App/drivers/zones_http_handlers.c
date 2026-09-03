#include "zones_http_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "MAX31856.h"
#include "http_form.h"
#include "ota_http.h" /* ota_http_check_interlocks() -- the shared "not while firing" gate */
#include "ota_interlock.h"
#include "safety_trip_words.h"
#include "relay_authority.h"
#include "thermo_combine.h"
#include "thermo_owner.h"
#include "web_encoding.h"
#include "wifi_provision_http.h"
#include "zone_settings_source_chain.h"

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
        return web_send_gzip_not_acceptable(req, TAG, "zones_page.html");
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
        return web_send_gzip_not_acceptable(req, TAG, "safety_config_page.html");
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
        ESP_LOGE(TAG, "GET /api/zones: malloc(%u) failed for the response buffer", (unsigned)json_cap);
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
           "\"tc_fault\":%u,\"relay_energized\":%s},"
           "\"ct_warn_mask\":%u,"
           "\"relay_names\":[",
           s_zones.cfg.thermo_count, s_zones.cfg.relay_count, s_zones.cfg.max_simultaneous_relays,
           s_zones.cfg.continue_on_zone_trip ? "true" : "false", s_zones.cfg.safety_tc_type,
           (double)s_zones.cfg.pc_link_abort_silence_ms, zone_owned_relay_mask(&s_zones.cfg),
           safety_wiring.link_up ? "true" : "false", safety_wiring.tc_temp_valid ? "true" : "false",
           (double)safety_wiring.tc_temp_c, safety_wiring.tc_fault, safety_wiring.relay_energized ? "true" : "false",
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
            (double)z->fuzzy_strength_pct);
        /* ZONES_CFG_VERSION 14->15 (PID_EXPANSION_PLAN.md 3.2 follow-up): the
         * coupling identification's own diagonal cell -- see zone_cfg_t::
         * coupling_diag_k_dc's own doc comment. Always emitted, same always-
         * emit/read-back-and-repost reasoning as fuzzy_strength_pct/
         * coupling_c%u above. %.4f matches model_k_dc's own precision -- same
         * unit, same small-gain-zone concern. */
        APPEND("\"coupling_diag_k_dc\":%.4f,", (double)z->coupling_diag_k_dc);
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
    ESP_LOGE(TAG, "GET /api/zones did not fit in %u bytes -- raise the buffer", (unsigned)json_cap);
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

/* ---- POST /api/zones ------------------------------------------------------
 * Whole-page submit; every field validated into a scratch struct before
 * anything is written to the in-RAM copy or NVS -- reject cleanly, never
 * partially apply, same discipline as every other untrusted-input boundary
 * in this codebase. */

/* Parses and validates zone index i's 7 fields from body into *z. thermo_count
 * is the just-parsed candidate count (not yet committed) -- zones at or past
 * it are still parsed (so a round-trip GET/POST of an unused zone block
 * doesn't need special-casing on the page) but not checked against
 * relay_count, since a shrunk relay_count would otherwise reject fields the
 * page never showed for a zone the submission isn't even claiming to use. */
static bool parse_zone_fields(const char *body, uint8_t i, uint8_t thermo_count, uint8_t relay_count,
                              uint8_t timing_profile_count, const zone_cfg_t *current_z, zone_cfg_t *z,
                              const char **err_reason)
{
    char key[24]; /* 16 -> 24 when the 8 guard-threshold override keys were
                   * added -- "z0_wrongdirwindow" is the longest at 18 chars
                   * plus terminator. */

    snprintf(key, sizeof(key), "z%u_name", i);
    char name[ZONE_NAME_MAX_LEN + 1];
    int name_len = http_form_find_field(body, key, name, sizeof(name));
    if (name_len == -2) {
        *err_reason = "zone name too long";
        return false;
    }
    if (name_len < 0) {
        /* Omitted: preserve the currently-stored name rather than blanking
         * it. In practice zones_page.html always sends z%u_name for every
         * zone it renders (i < thermo_count), so this only matters for a
         * slot the page never showed -- see the i >= thermo_count preserve
         * block below, which this keeps consistent with. */
        strncpy(z->name, current_z->name, ZONE_NAME_MAX_LEN);
        z->name[ZONE_NAME_MAX_LEN] = '\0';
    } else {
        strncpy(z->name, name, ZONE_NAME_MAX_LEN);
        z->name[ZONE_NAME_MAX_LEN] = '\0';
    }

    /* 2026-08-21, TODO.md owner-report item 1/4: this channel's MAX31856
     * thermocouple type. Parsed BEFORE the `i >= thermo_count` early return
     * below, deliberately unlike relay_mask/thermo_mask/every other zone
     * field -- this is per-CHANNEL hardware state (see zone_cfg_t::tc_type's
     * comment), not per-zone, so a channel physically present but not
     * currently claimed by any configured zone (thermo_count set lower than
     * the physical channel count) must still keep its own real type across
     * an ordinary page save rather than being silently zeroed to THERMO_TC_B
     * the moment it falls outside thermo_count's range -- which is exactly
     * what would happen if this fell after the early return, since tmp is
     * zero-initialized by the caller and 0 is a real, different, wrong type
     * here (see ZONES_CFG_VERSION's migration comment for the identical
     * reasoning).
     *
     * OPTIONAL, falling back to current_z->tc_type (the live value) rather
     * than to a fixed default when omitted -- there is no default
     * thermocouple type that could possibly be correct for a channel this
     * submission never mentioned, unlike thermo_mask's "zone i reads channel
     * i" legacy mapping just below. zones_page.html always sends this field
     * (see its JS), so the fallback matters only for a client that predates
     * thermocouple-type selection entirely (pc_tools/MCP, the test
     * harnesses).
     *
     * Present-but-out-of-range is still an error -- "in range" is
     * ZONE_TC_TYPE_MAX_REAL (0-7, the eight real thermocouple types), not
     * MAX31856_configure()'s wider 0-0x0F: see that macro's comment for why
     * this operator-facing endpoint is deliberately stricter than the raw
     * UART debug path. */
    snprintf(key, sizeof(key), "z%u_tctype", i);
    {
        char probe[8];
        if (http_form_find_field(body, key, probe, sizeof(probe)) > 0) {
            uint8_t tc_type_raw;
            if (!zones_config_json_parse_u8_field(body, key, 0, ZONE_TC_TYPE_MAX_REAL, &tc_type_raw)) {
                *err_reason = "zone thermocouple type must be a real thermocouple type (0-7: "
                              "B/E/J/K/N/R/S/T), not a voltage-input mode";
                return false;
            }
            z->tc_type = tc_type_raw;
        } else {
            z->tc_type = current_z->tc_type;
        }
    }

    if (i >= thermo_count) {
        /* DEFECT FIX (found by black-box testing against the live board):
         * this slot is past the just-submitted thermo_count, so
         * zones_page.html's whole-page submit never rendered UI for it and
         * carries no data for relay_mask/thermo_mask/cal/PID/ramp/guard
         * thresholds/model/etc. The caller's `z` starts zero-initialized
         * (zones_post_handler()'s memset(&tmp, 0, ...)), so returning here
         * unconditionally used to leave every one of those fields at 0 --
         * an accepted 200 OK POST that silently zeroed state the operator
         * never asked to change (reproduced live: thermo_count=1 zeroed
         * zone 1 and zone 2's thermo_mask on an ordinary re-save that only
         * replayed what GET had just reported).
         *
         * Fix: preserve the currently-stored zone_cfg_t for this slot
         * instead of leaving it zeroed. z->name and z->tc_type are already
         * set above (name/tc_type each have their own omit-means-preserve
         * handling), so save and restore just those two fields around a
         * bulk copy of everything else from current_z -- the live value,
         * same object z%u_tctype's fallback already reads from just above.
         * A future submission that raises thermo_count back up (or a
         * client that explicitly names this slot's fields once one exists)
         * still goes through the normal validated path below, unaffected --
         * this branch only runs for a slot outside today's thermo_count. */
        char preserved_name[ZONE_NAME_MAX_LEN + 1];
        strncpy(preserved_name, z->name, sizeof(preserved_name));
        preserved_name[ZONE_NAME_MAX_LEN] = '\0';
        uint8_t preserved_tc_type = z->tc_type;
        *z = *current_z;
        strncpy(z->name, preserved_name, ZONE_NAME_MAX_LEN);
        z->name[ZONE_NAME_MAX_LEN] = '\0';
        z->tc_type = preserved_tc_type;
        return true;
    }

    snprintf(key, sizeof(key), "z%u_relay_mask", i);
    uint8_t relay_mask_raw;
    if (!zones_config_json_parse_u8_field(body, key, 0, 0xFF, &relay_mask_raw)) {
        *err_reason = "zone relay_mask missing or invalid";
        return false;
    }
    uint8_t valid_bits = relay_count >= 8 ? 0xFF : (uint8_t)((1u << relay_count) - 1u);
    if ((relay_mask_raw & ~valid_bits) != 0) {
        *err_reason = "zone relay_mask references an unconfigured relay";
        return false;
    }
    z->relay_mask = relay_mask_raw;

    /* 2026-08-27 (ZONES_CFG_VERSION 8->9, owner's request: "assign the zones
     * to them"): which timing_profiles[] slot this zone uses. REQUIRED, unlike
     * the nine fields it replaces (which were each individually OPTIONAL) --
     * there is no legacy client to preserve backward compatibility for here,
     * since no firmware before this pass ever had a "timing profile" concept
     * to send or omit. Bounded against timing_profile_count, the number of
     * profiles THIS submission itself defines (parsed by zones_post_handler()
     * before this per-zone loop runs -- see its own comment), not against
     * some fixed ceiling: a submission that only defines two profiles must not
     * let a zone reference a third one that doesn't exist in it. */
    snprintf(key, sizeof(key), "z%u_timingprofile", i);
    if (!zones_config_json_parse_u8_field(body, key, 0, timing_profile_count - 1, &z->timing_profile)) {
        *err_reason = "zone timing_profile missing or references a profile that doesn't exist "
                      "in this submission";
        return false;
    }

    /* TODO.md 10.8. Deliberately NOT validated/defaulted the same way as
     * z%u_xzone/z%u_k above (present-but-omit-means-0/disabled): omitting
     * this field must mean "this client doesn't know about multi-thermo,
     * keep controlling off the channel this zone always used", not "no
     * thermocouple assigned". zones_page.html doesn't send this field yet
     * (TODO.md 10.8's open item -- see the getter's header comment), and if
     * an absent field defaulted to 0 here, saving that page's form today
     * would silently blind every zone on the very next ordinary settings
     * save. bit i is that legacy mapping (zone i <-> channel i), same as
     * migrate_zones_cfg_v1_to_current() falls back to for an already-saved
     * blob that predates this field entirely -- one fallback rule for both
     * an old blob and a client that just doesn't send the key.
     *
     * A client that DOES know this field and sends it explicitly -- including
     * an explicit 0, deliberately clearing a zone's thermocouple -- is
     * honoured exactly as sent; present-but-out-of-range is still an error,
     * same discipline as relay_mask above. */
    snprintf(key, sizeof(key), "z%u_thermo_mask", i);
    {
        char probe[8];
        if (http_form_find_field(body, key, probe, sizeof(probe)) > 0) {
            uint8_t thermo_mask_raw;
            if (!zones_config_json_parse_u8_field(body, key, 0, 0xFF, &thermo_mask_raw)) {
                *err_reason = "zone thermo_mask missing or invalid";
                return false;
            }
            uint8_t valid_thermo_bits =
                thermo_count >= 8 ? 0xFF : (uint8_t)((1u << thermo_count) - 1u);
            if ((thermo_mask_raw & ~valid_thermo_bits) != 0) {
                *err_reason = "zone thermo_mask references an unconfigured thermocouple channel";
                return false;
            }
            z->thermo_mask = thermo_mask_raw;
        } else {
            z->thermo_mask = (uint8_t)(1u << i);
        }
    }

    /* 2026-08-27: purely informational (see ZONES_CFG_VERSION's 5->6
     * comment), so unlike thermo_mask above there is no legacy single-
     * channel mapping to preserve -- an omitted field is simply "no CT probe
     * mapped to this zone", the same safe-zero default a brand-new zone
     * already gets. Still validated against ZONE_CT_CHANNEL_COUNT (a fixed
     * hardware count, not relay_count/thermo_count) when present. */
    snprintf(key, sizeof(key), "z%u_ct_mask", i);
    {
        char probe[8];
        if (http_form_find_field(body, key, probe, sizeof(probe)) > 0) {
            uint8_t ct_mask_raw;
            if (!zones_config_json_parse_u8_field(body, key, 0, 0xFF, &ct_mask_raw)) {
                *err_reason = "zone ct_mask missing or invalid";
                return false;
            }
            uint8_t valid_ct_bits = (uint8_t)((1u << ZONE_CT_CHANNEL_COUNT) - 1u);
            if ((ct_mask_raw & ~valid_ct_bits) != 0) {
                *err_reason = "zone ct_mask references an unconfigured current-sense channel";
                return false;
            }
            z->ct_mask = ct_mask_raw;
        } else {
            z->ct_mask = 0;
        }
    }

    /* Sane numeric bounds -- firmware sanity bounds against a malformed/
     * typo'd submission, not real kiln-safety limits (that's the feasibility
     * check in profiles_http.c, on the ramp-rate side). Kp/Ki/Kd have no
     * natural physical bound, so 0..1000 is just generous headroom over
     * anything a real PID loop on this hardware would ever be tuned to. */
    snprintf(key, sizeof(key), "z%u_cal", i);
    if (!zones_config_json_parse_float_field(body, key, ZONE_CAL_OFFSET_MIN_C, ZONE_CAL_OFFSET_MAX_C, &z->cal_offset_c)) {
        *err_reason = "zone cal_offset_c missing or out of range";
        return false;
    }
    snprintf(key, sizeof(key), "z%u_kp", i);
    if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_PID_GAIN_MAX, &z->pid_kp)) {
        *err_reason = "zone pid_kp missing or out of range";
        return false;
    }
    snprintf(key, sizeof(key), "z%u_ki", i);
    if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_PID_GAIN_MAX, &z->pid_ki)) {
        *err_reason = "zone pid_ki missing or out of range";
        return false;
    }
    snprintf(key, sizeof(key), "z%u_kd", i);
    if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_PID_GAIN_MAX, &z->pid_kd)) {
        *err_reason = "zone pid_kd missing or out of range";
        return false;
    }
    /* ZONES_CFG_VERSION 12->13: invalidate the tuning-quality record (set 1
     * -- see zone_cfg_t::tuning_valid's own doc comment) when THIS path
     * changes the gains. *z started as *current_z (this function's own
     * comment/caller, "omit-means-preserve"), so z->tuning_valid already
     * carries the OLD record forward untouched by default -- exactly the
     * reset-one-side shape this whole feature exists to avoid, since this
     * whole-page submit writes pid_kp/ki/kd directly into the scratch
     * struct and never goes through zones_config_set_pid() (the narrow
     * POST /api/zones/pid endpoint's own choke point). A gain that reads
     * back identical to what was already stored (an unrelated field on the
     * same page changed, gains untouched) leaves the record standing --
     * only an ACTUAL change invalidates it. */
    /* z has NOT been seeded from current_z on this (in-range) path -- unlike
     * the i >= thermo_count early return above, which does `*z = *current_z`
     * wholesale, this path builds z field-by-field from the submission, and
     * the caller's z started zero-initialized (zones_post_handler()'s
     * memset(&tmp, 0, ...)). The tuning_* fields have no z%u_ POST key at
     * all (read-only, see GET /api/zones' own comment on this block), so
     * without an explicit carry-through here EVERY in-range save would zero
     * tuning_valid regardless of whether the gains actually changed -- the
     * exact reset-one-side shape this whole feature exists to avoid, just
     * from the opposite direction (wiping a GOOD record instead of keeping
     * a STALE one). Carry the old record through by default, then
     * invalidate ONLY on an actual gain change.
     *
     * Tolerance, not exact equality, for the change check: GET /api/zones
     * emits pid_kp/ki/kd at %.4f (this file's own APPEND format string
     * above), so an ORDINARY read-back-and-repost round trip through the
     * page's own form fields already loses precision below the 4th decimal
     * place -- an exact `!=` here would invalidate a good record on every
     * single resave, even one that changes nothing about the gains at all.
     * 0.0001 matches that same %.4f resolution; a real operator-entered
     * change is never that close to the stored value by accident. */
    z->tuning_valid = current_z->tuning_valid;
    z->tuning_method = current_z->tuning_method;
    z->tuning_rule = current_z->tuning_rule;
    z->tuning_settled = current_z->tuning_settled;
    z->tuning_extrapolation_converged = current_z->tuning_extrapolation_converged;
    z->tuning_tau_consistent = current_z->tuning_tau_consistent;
    z->tuning_baseline_c = current_z->tuning_baseline_c;
    z->tuning_step_ambient_c = current_z->tuning_step_ambient_c;
    z->tuning_raw_rise_c = current_z->tuning_raw_rise_c;
    z->tuning_rise_inf_c = current_z->tuning_rise_inf_c;
    z->tuning_seq = current_z->tuning_seq;
    if (fabsf(z->pid_kp - current_z->pid_kp) > 0.0001f || fabsf(z->pid_ki - current_z->pid_ki) > 0.0001f ||
        fabsf(z->pid_kd - current_z->pid_kd) > 0.0001f) {
        z->tuning_valid = 0;
    }
    snprintf(key, sizeof(key), "z%u_ramp", i);
    if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_MAX_RAMP_C_PER_HR_MAX, &z->max_ramp_c_per_hr)) {
        *err_reason = "zone max_ramp_c_per_hr missing or out of range";
        return false;
    }
    /* 0 = "never configured" (profile_executor.c substitutes its own
     * default); 20 C/min is a generous ceiling -- well above anything this
     * board's bang-bang control could plausibly produce, just a sanity bound
     * against a typo. */
    snprintf(key, sizeof(key), "z%u_sanity", i);
    if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_SANITY_RATE_MAX_C_PER_MIN, &z->sanity_rate_c_per_min)) {
        *err_reason = "zone sanity_rate_c_per_min missing or out of range";
        return false;
    }
    snprintf(key, sizeof(key), "z%u_mode", i);
    uint8_t mode_raw;
    if (!zones_config_json_parse_u8_field(body, key, 0, (long)ZONE_CONTROL_MODE_PID_FUZZY, &mode_raw)) {
        *err_reason = "zone control_mode missing or out of range (0-3)";
        return false;
    }
    z->control_mode = mode_raw;
    /* 1400C ceiling matches PROFILE_TARGET_C_MAX (profiles_http.c) -- a
     * guard 5 limit tighter than what a profile could ever request would be
     * a contradiction between the two checks. */
    snprintf(key, sizeof(key), "z%u_maxtemp", i);
    if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_MAX_TEMP_C_MAX, &z->max_temp_c)) {
        *err_reason = "zone max_temp_c missing or out of range";
        return false;
    }
    snprintf(key, sizeof(key), "z%u_mintemp", i);
    if (!zones_config_json_parse_float_field(body, key, ZONE_MIN_TEMP_C_MIN, ZONE_MIN_TEMP_C_MAX, &z->min_temp_c)) {
        *err_reason = "zone min_temp_c missing or out of range";
        return false;
    }
    /* 0 = "not configured" (caller substitutes PROFILE_EXECUTOR_DEFAULT_*_MS,
     * same convention as sanity_rate_c_per_min above). 600000ms (10min) is a
     * generous upper bound on window_ms -- well past any window that would
     * still make sense against a kiln's thermal time constant; 60000ms on
     * min_on/min_off is the same generosity relative to window_ms's own
     * range. */
    snprintf(key, sizeof(key), "z%u_window", i);
    if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_HEATER_WINDOW_MS_MAX, &z->heater_window_ms)) {
        *err_reason = "zone heater_window_ms missing or out of range";
        return false;
    }
    snprintf(key, sizeof(key), "z%u_minon", i);
    if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_HEATER_MIN_ON_OFF_MS_MAX, &z->heater_min_on_ms)) {
        *err_reason = "zone heater_min_on_ms missing or out of range";
        return false;
    }
    /* The one heater field with a LOWER bound too, and the only reason it
     * cannot just be another zones_config_json_parse_float_field() range: the accepted set is
     * disjoint (0, or >= the floor), not an interval. See
     * ZONE_HEATER_MIN_ON_MS_FLOOR in zones_http.h for why this is refused
     * rather than quietly raised. */
    if (z->heater_min_on_ms > 0.0f && z->heater_min_on_ms < ZONE_HEATER_MIN_ON_MS_FLOOR) {
        *err_reason = "zone heater_min_on_ms below the 10000 ms relay-protection floor "
                      "(use 0 for the firmware default)";
        return false;
    }
    /* The window-vs-min-on RELATIONSHIP (2026-08-29). Checked here, after
     * both fields are parsed, because it is the only check in this function
     * that needs two of them at once. A window that passes its own range but
     * fails this cannot render a fractional duty at all -- see
     * ZONE_HEATER_WINDOW_MIN_MULTIPLE in zones_http.h. */
    if (z->heater_window_ms > 0.0f && z->heater_window_ms < zone_required_window_ms(z->heater_min_on_ms)) {
        *err_reason = "zone heater_window_ms too short for its heater_min_on_ms: the window must be at "
                      "least 3x the minimum on-time or no fractional duty can be rendered "
                      "(use 0 for the firmware default)";
        return false;
    }
    snprintf(key, sizeof(key), "z%u_minoff", i);
    if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_HEATER_MIN_ON_OFF_MS_MAX, &z->heater_min_off_ms)) {
        *err_reason = "zone heater_min_off_ms missing or out of range";
        return false;
    }
    /* TODO.md 6A.3's remaining named guard thresholds. OPTIONAL, same reason
     * z%u_xzone below is: a submission that omits one leaves the
     * corresponding firmware default in force (z is zero-initialized by the
     * caller, and 0 is thermal_guard.c's own "substitute the default" value
     * for every one of these -- unlike z%u_xzone, omitting one of these does
     * NOT disable its guard). Bounds are generous sanity ceilings against a
     * typo, not real per-field tuning limits: rates 0-20C/min matches
     * z%u_sanity's own ceiling, windows/periods 0-7200s (2h) covers any
     * kiln's plausible time constant, debounce ticks 0-100, margin 0-500C. */
    snprintf(key, sizeof(key), "z%u_wrongdirwindow", i);
    {
        char probe[16];
        if (http_form_find_field(body, key, probe, sizeof(probe)) > 0) {
            if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_GUARD_TIME_S_MAX, &z->guard_wrong_dir_window_s)) {
                *err_reason = "zone guard_wrong_dir_window_s out of range";
                return false;
            }
        }
    }
    snprintf(key, sizeof(key), "z%u_wrongdirrate", i);
    {
        char probe[16];
        if (http_form_find_field(body, key, probe, sizeof(probe)) > 0) {
            if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_GUARD_RATE_C_PER_MIN_MAX, &z->guard_wrong_dir_rate_c_per_min)) {
                *err_reason = "zone guard_wrong_dir_rate_c_per_min out of range";
                return false;
            }
        }
    }
    snprintf(key, sizeof(key), "z%u_offsettle", i);
    {
        char probe[16];
        if (http_form_find_field(body, key, probe, sizeof(probe)) > 0) {
            if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_GUARD_TIME_S_MAX, &z->guard_off_settle_s)) {
                *err_reason = "zone guard_off_settle_s out of range";
                return false;
            }
        }
    }
    snprintf(key, sizeof(key), "z%u_runawayrate", i);
    {
        char probe[16];
        if (http_form_find_field(body, key, probe, sizeof(probe)) > 0) {
            if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_GUARD_RATE_C_PER_MIN_MAX, &z->guard_runaway_rate_c_per_min)) {
                *err_reason = "zone guard_runaway_rate_c_per_min out of range";
                return false;
            }
        }
    }
    snprintf(key, sizeof(key), "z%u_runawaymargin", i);
    {
        char probe[16];
        if (http_form_find_field(body, key, probe, sizeof(probe)) > 0) {
            if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_GUARD_MARGIN_C_MAX, &z->guard_runaway_margin_c)) {
                *err_reason = "zone guard_runaway_margin_c out of range";
                return false;
            }
        }
    }
    snprintf(key, sizeof(key), "z%u_driftperiod", i);
    {
        char probe[16];
        if (http_form_find_field(body, key, probe, sizeof(probe)) > 0) {
            if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_GUARD_TIME_S_MAX, &z->guard_drift_period_s)) {
                *err_reason = "zone guard_drift_period_s out of range";
                return false;
            }
        }
    }
    snprintf(key, sizeof(key), "z%u_debounce", i);
    {
        char probe[16];
        if (http_form_find_field(body, key, probe, sizeof(probe)) > 0) {
            if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_GUARD_DEBOUNCE_TICKS_MAX, &z->guard_sensor_fault_debounce_ticks)) {
                *err_reason = "zone guard_sensor_fault_debounce_ticks out of range";
                return false;
            }
        }
    }
    snprintf(key, sizeof(key), "z%u_frozenwindow", i);
    {
        char probe[16];
        if (http_form_find_field(body, key, probe, sizeof(probe)) > 0) {
            if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_GUARD_TIME_S_MAX, &z->guard_frozen_window_s)) {
                *err_reason = "zone guard_frozen_window_s out of range";
                return false;
            }
        }
    }
    /* The nine v8 overrides that used to be parsed inline here now live on
     * the timing profile this zone points at -- see z%u_timingprofile above
     * and zones_config_json_parse_timing_profile_fields() (tp%u_progressduty..tp%u_ramplock),
     * parsed once per PROFILE rather than once per zone. */
    /* Guard 8. OPTIONAL, unlike every field above: a submission that omits
     * it means "leave the guard disabled" (z is zero-initialized by the
     * caller), so older clients -- the MCP/pc_tools path and the test
     * harnesses that post the original 14 fields -- keep working unchanged
     * instead of being rejected by a field they've never heard of. Such a
     * client does clear a previously saved threshold, which is the same
     * whole-page-submit semantics every other field already has. Present
     * but malformed is still an error. 1000C is a sanity bound only; a real
     * threshold comes from a measured cross-gain matrix. */
    snprintf(key, sizeof(key), "z%u_xzone", i);
    {
        char probe[16];
        if (http_form_find_field(body, key, probe, sizeof(probe)) > 0) {
            if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_CROSS_ZONE_DELTA_C_MAX, &z->cross_zone_max_delta_c)) {
                *err_reason = "zone cross_zone_max_delta_c out of range";
                return false;
            }
        }
    }
    /* The identified plant model (TODO.md 6A.4 -> 6A.2's feedforward).
     * OPTIONAL, for exactly the same reason z%u_xzone above is: a client
     * that predates these fields -- pc_tools/MCP, the test harnesses --
     * must not start getting 400s for a field it has never heard of.
     *
     * The whole-page-submit semantics have real teeth here, though. These
     * numbers are NOT typed by an operator; they are measured by a
     * multi-hour step test. A client that omits them silently deletes that
     * measurement, because z is zero-initialized by the caller and zero is
     * the "no model" encoding. That is the established behaviour of this
     * endpoint and is left as-is rather than special-cased into a
     * merge-on-omit, which would make this one field group behave unlike
     * every other one on the page -- but it is why zones_page.html reads
     * these back from GET and posts them straight through untouched, and
     * why anything else driving this endpoint must do the same.
     *
     * Bounds are ZONE_MODEL_*_MAX so this path and zones_config_set_model()
     * accept exactly the same set of models; see their definition. Present
     * but malformed is still an error. */
    snprintf(key, sizeof(key), "z%u_k", i);
    {
        char probe[24];
        if (http_form_find_field(body, key, probe, sizeof(probe)) > 0) {
            if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_MODEL_K_MAX, &z->model_k_dc)) {
                *err_reason = "zone model K out of range";
                return false;
            }
        }
    }
    snprintf(key, sizeof(key), "z%u_tau", i);
    {
        char probe[24];
        if (http_form_find_field(body, key, probe, sizeof(probe)) > 0) {
            if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_MODEL_TIME_MAX_S, &z->model_tau_s)) {
                *err_reason = "zone model tau out of range";
                return false;
            }
        }
    }
    snprintf(key, sizeof(key), "z%u_deadtime", i);
    {
        char probe[24];
        if (http_form_find_field(body, key, probe, sizeof(probe)) > 0) {
            if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_MODEL_TIME_MAX_S, &z->model_dead_time_s)) {
                *err_reason = "zone model dead time out of range";
                return false;
            }
        }
    }
    /* PID_EXPANSION_PLAN.md Phase 2/4 (2026-08-30): the fuzzy-PID and
     * cross-zone-coupling fields. OPTIONAL, same "older clients must not
     * start getting 400s for a field they've never heard of" reasoning as
     * z%u_xzone/z%u_k above -- but UNLIKE those, omitted means PRESERVE the
     * currently-stored value (current_z), the same convention z%u_tctype/
     * z%u_settings_source use, not "reset to 0". These three are measured
     * quantities (an operator-set adjustment knob, and an autotune-measured
     * coupling coefficient), and a whole-page save from a client that
     * predates this field (or simply didn't re-render every input) must not
     * silently delete a measurement/setting that took real effort to obtain
     * -- the model_k_dc/model_tau_s/model_dead_time_s "omit deletes it" case
     * above is this file's OWN documented sharp edge, not a precedent to
     * repeat for a field with no compensating "the page always posts these
     * back verbatim" guarantee behind it. Present but out of range is still
     * an error, never silently clamped (PID_EXPANSION_PLAN.md's own "prove
     * range checks refuse, not clamp" rule). */
    snprintf(key, sizeof(key), "z%u_fuzzy_strength", i);
    {
        if (zones_config_json_field_present(body, key)) {
            if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_FUZZY_STRENGTH_PCT_MAX, &z->fuzzy_strength_pct)) {
                *err_reason = "zone fuzzy_strength_pct out of range (0-100)";
                return false;
            }
        } else {
            z->fuzzy_strength_pct = current_z->fuzzy_strength_pct;
        }
    }
    /* 2026-09-02 (ZONES_CFG_VERSION 14->15, PID_EXPANSION_PLAN.md 3.2
     * follow-up): the coupling identification's own diagonal cell -- see
     * zone_cfg_t::coupling_diag_k_dc's own doc comment. Same OPTIONAL,
     * omit-PRESERVES convention as z%u_fuzzy_strength/z%u_coupling_c%u just
     * above: this is a measured quantity, and a whole-page save from a
     * client that predates this field must not silently delete it. */
    snprintf(key, sizeof(key), "z%u_coupling_diag_k_dc", i);
    {
        if (zones_config_json_field_present(body, key)) {
            if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_MODEL_K_MAX, &z->coupling_diag_k_dc)) {
                *err_reason = "zone coupling_diag_k_dc out of range";
                return false;
            }
        } else {
            z->coupling_diag_k_dc = current_z->coupling_diag_k_dc;
        }
    }
    /* 2026-08-30 (ZONES_CFG_VERSION 10->11): one indexed key per cell,
     * z%u_coupling_c%u -- e.g. z1_coupling_c0 is zone 1's measured response
     * to zone 0's heater. Same per-cell "omit preserves the currently-stored
     * value" convention z%u_fuzzy_strength above uses (these are measured
     * quantities; a whole-page save from a client that predates a cell must
     * not silently delete it), and the diagonal (j == i) is refused if a
     * client submits anything but 0 for it, matching zones_config_set_
     * coupling()'s own storage-layer rule. */
    for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
        snprintf(key, sizeof(key), "z%u_coupling_c%u", i, j);
        if (zones_config_json_field_present(body, key)) {
            float cell;
            if (!zones_config_json_parse_float_field(body, key, 0.0f, (j == i) ? 0.0f : ZONE_COUPLING_COEFF_MAX, &cell)) {
                *err_reason = "zone coupling_coeff out of range";
                return false;
            }
            z->coupling_coeff[j] = cell;
        } else {
            z->coupling_coeff[j] = current_z->coupling_coeff[j];
        }
    }
    /* 2026-08-31 (ZONES_CFG_VERSION 11->12): coupling_tau_s[]/
     * coupling_dead_time_s[] have NO POST wire fields of their own -- unlike
     * coupling_coeff[], nothing on the settings page lets an operator type a
     * cross-zone time constant, so there is nothing to parse here. What DOES
     * matter is the omit case: `z` starts fresh (not copied from current_z),
     * so without this, every ordinary whole-page save from the settings page
     * -- which never sends these two arrays at all -- would silently zero out
     * whatever autotune_engine.c's finalize_fit() had persisted for every
     * zone, the exact "reset-one-side" class this codebase has shipped
     * before. Always preserve, unconditionally -- autotune_engine.c's own
     * persist path writes s_zones.cfg directly (via a coupling-cell setter),
     * never through this POST parser, so there is no legitimate way for a
     * POST to be the one updating these two arrays. */
    memcpy(z->coupling_tau_s, current_z->coupling_tau_s, sizeof(z->coupling_tau_s));
    memcpy(z->coupling_dead_time_s, current_z->coupling_dead_time_s, sizeof(z->coupling_dead_time_s));
    /* settings_source: UNLIKE the three floats above, omitted must NOT
     * default to 0 -- 0 is a real, different value here ("copies zone 0's
     * settings"), not a safe empty default. Falls back to the CURRENT stored
     * value (current_z), same "omit preserves the live setting" convention
     * z%u_tctype uses just above, rather than to ZONE_SETTINGS_SOURCE_CUSTOM
     * unconditionally -- this is a whole-page submit, and an older client
     * that predates this field must not silently flip every zone back to
     * "custom" on an otherwise ordinary save (see this file's own
     * whole-page-submit discipline: every other optional field either
     * defaults to a safe zero or preserves the live value, never invents a
     * third behavior). */
    snprintf(key, sizeof(key), "z%u_settings_source", i);
    {
        if (zones_config_json_field_present(body, key)) {
            uint8_t src_raw;
            if (!zones_config_json_parse_u8_field(body, key, 0, 0xFF, &src_raw)) {
                *err_reason = "zone settings_source missing or invalid";
                return false;
            }
            if (src_raw != ZONE_SETTINGS_SOURCE_CUSTOM && src_raw >= MAX31856_CHANNEL_COUNT) {
                *err_reason = "zone settings_source references a zone that doesn't exist";
                return false;
            }
            /* Self-reference is the degenerate cycle ("zone 1 copies zone
             * 1"). Phase 5 owns the general cycle/disabled-zone guards, but
             * this one case is free to reject here and saves Phase 5 having
             * to unwind it. */
            if (src_raw == i) {
                *err_reason = "zone settings_source cannot point at itself";
                return false;
            }
            /* Longer cycle (2-zone, 3-zone, ...): same chain-walk
             * zones_config_set_settings_source() runs, against the live
             * s_zones.cfg for every OTHER zone -- this is a single-zone
             * write (only slot i's link is changing here), so any NEW cycle
             * must run through zone i; walking from i against everyone
             * else's live value is sufficient, matching the setter's own
             * reasoning. A whole-page POST that changes several zones'
             * links AT ONCE in a way that only cycles once every change is
             * applied is caught by the zones-POST handler's own re-walk of
             * every zone's chain across the fully-assembled tmp.zones[],
             * right after this function's call site's loop and before that
             * handler's commit point -- see the comment there. */
            {
                zone_cfg_t probe[MAX31856_CHANNEL_COUNT];
                memcpy(probe, s_zones.cfg.zones, sizeof(probe));
                probe[i].settings_source = src_raw;
                if (zones_config_json_settings_source_chain_has_cycle(probe, i, thermo_count)) {
                    *err_reason = "zone settings_source would create an inheritance cycle";
                    return false;
                }
            }
            z->settings_source = src_raw;
        } else {
            z->settings_source = current_z->settings_source;
        }
    }
    return true;
}

esp_err_t zones_post_handler(httpd_req_t *req)
{
    /* Refuse to rewrite zone/relay/guard configuration while a firing is running, the same
     * gate kiln_cfg_http.c's apply and backup_http.c's restore already put in
     * front of the very same zones_cfg_t. Without it, changing relay_mask
     * mid-run moved the firing onto a different physical relay and left the
     * old one wherever it was last commanded, with nobody driving it off --
     * a contact that stays closed because the code that owned it stopped
     * looking at it.
     *
     * ota_http_check_interlocks() is that shared gate rather than a private
     * profile-is-RUNNING check, deliberately: it also covers a hot zone and
     * a commanded heater, and reads the kiln's ACTUAL current state rather
     * than profile_executor's own view (see its doc comment for why that
     * distinction matters). ota_http_req_ack_no_safety() carries the same
     * per-request operator acknowledgement every other caller passes, so a
     * board with no safety processor can still be configured -- saving this
     * page streams nothing over the link, exactly as backup_http's restore
     * argues for itself. */
    char interlock_reason[OTA_INTERLOCK_REASON_MAX];
    interlock_reason[0] = '\0';
    ota_interlock_result_t gate = ota_http_check_interlocks(ota_http_req_ack_no_safety(req),
                                                            interlock_reason, sizeof(interlock_reason));
    if (gate != OTA_INTERLOCK_OK) {
        ESP_LOGW(TAG, "POST /api/zones refused by interlock: %s", interlock_reason);
        return ota_http_send_interlock_refusal(req, gate, interlock_reason);
    }

    if (req->content_len <= 0 || req->content_len > ZONES_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }

    /* HEAP (PSRAM), not stack -- ZONES_BODY_MAX+1 (4097B) was the second-
     * largest transient buffer on the whole httpd_worker task's request
     * path (coordinator review, 2026-08-31), and unlike profile_post_
     * handler's `body` this one genuinely IS read throughout the entire
     * function (http_form_find_field() calls scattered across the whole
     * parse), so it cannot be freed early the way that one's could -- it is
     * freed on EVERY return path below instead, mirroring every other
     * heap-converted handler in this pass. */
    char *body = heap_caps_malloc(ZONES_BODY_MAX + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (body == NULL) {
        ESP_LOGE(TAG, "POST /api/zones: malloc(%u) failed for the request body buffer",
                 (unsigned)(ZONES_BODY_MAX + 1));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req,
                                  "{\"ok\":false,\"error\":\"out of memory reading the request body\"}");
    }
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            ESP_LOGW(TAG, "zones body read failed/short: %d", ret);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body read failed");
            free(body);
            return ESP_OK;
        }
        received += (size_t)ret;
    }
    body[received] = '\0';

    zones_cfg_t tmp;
    memset(&tmp, 0, sizeof(tmp));

    if (!zones_config_json_parse_u8_field(body, "thermo_count", 0, MAX31856_CHANNEL_COUNT, &tmp.thermo_count)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "thermo_count missing or out of range");
        free(body);
        return ESP_OK;
    }
    if (!zones_config_json_parse_u8_field(body, "relay_count", 0, KILN_IO_RELAY_COUNT, &tmp.relay_count)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "relay_count missing or out of range");
        free(body);
        return ESP_OK;
    }
    /* Optional -- unlike thermo_count/relay_count, a missing
     * max_simultaneous_relays means "not set" (0, unlimited), not a
     * rejected request, so existing/older callers that don't send it keep
     * working unchanged. Present-but-out-of-range is still rejected. */
    {
        char val[8];
        int len = http_form_find_field(body, "max_simultaneous_relays", val, sizeof(val));
        if (len > 0) {
            char *end = NULL;
            long v = strtol(val, &end, 10);
            /* *end != '\0' rejects trailing garbage after a valid numeric
             * prefix (e.g. "2X"), same gap as zones_config_json_parse_u8_field()/
             * zones_config_json_parse_float_field() above -- end == val alone lets it through. */
            if (end == val || *end != '\0' || v < 0 || v > KILN_IO_RELAY_COUNT) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "max_simultaneous_relays out of range");
                free(body);
                return ESP_OK;
            }
            tmp.max_simultaneous_relays = (uint8_t)v;
        }
    }
    /* Optional, same "missing means keep the safe default" convention as
     * max_simultaneous_relays above -- tmp is zeroed, so a caller that
     * never sends this field gets continue_on_zone_trip=0 (abort the whole
     * firing), TODO.md 6A.3's stated default. Only "0" or "1" accepted. */
    {
        char val[4];
        int len = http_form_find_field(body, "continue_on_zone_trip", val, sizeof(val));
        if (len > 0) {
            if (strcmp(val, "1") == 0) {
                tmp.continue_on_zone_trip = 1;
            } else if (strcmp(val, "0") == 0) {
                tmp.continue_on_zone_trip = 0;
            } else {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "continue_on_zone_trip must be 0 or 1");
                free(body);
                return ESP_OK;
            }
        }
    }

    /* 2026-08-27 (ZONES_CFG_VERSION 8->9, owner's request: "make it so that i
     * can have diffrent Safety timings and assign the zones to them"): the
     * named timing profiles this submission defines, parsed BEFORE the
     * per-zone loop below -- each zone's z%u_timingprofile must be bounded
     * against however many profiles THIS submission actually carries, and
     * that count is only known once this loop finishes. A profile slot is
     * "in the submission" iff tp<N>_name is present, checked contiguously
     * from N=0: GET /api/zones always emits a dense 0..count-1
     * timing_profiles array (see zones_get_handler()), so the page always
     * reposts one too, and the first missing tp<N>_name is the end of the
     * list, not a gap to skip past. At least one profile (tp0_name) is
     * required -- an empty timing_profiles[] would leave every zone's
     * z%u_timingprofile with nothing valid to reference, and
     * zones_cfg_t::timing_profile_count must never be 0 in a config this
     * handler commits (see its own comment). */
    for (uint8_t p = 0; p < MAX31856_CHANNEL_COUNT; p++) {
        char probe_key[16];
        char probe[TIMING_PROFILE_NAME_MAX_LEN + 1];
        snprintf(probe_key, sizeof(probe_key), "tp%u_name", p);
        int probe_len = http_form_find_field(body, probe_key, probe, sizeof(probe));
        if (probe_len == -2) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "timing profile name too long");
            free(body);
            return ESP_OK;
        }
        if (probe_len < 0) {
            break; /* no tp<p>_name -- this and every following slot is absent from this submission */
        }
        const char *err_reason = "invalid timing profile field";
        if (!zones_config_json_parse_timing_profile_fields(body, p, &tmp.timing_profiles[p], &err_reason)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err_reason);
            free(body);
            return ESP_OK;
        }
        tmp.timing_profile_count = (uint8_t)(p + 1);
    }
    if (tmp.timing_profile_count == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "at least one timing profile (tp0_name) is required");
        free(body);
        return ESP_OK;
    }

    for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
        const char *err_reason = "invalid zone field";
        /* &s_zones.cfg.zones[i]: the LIVE value, for z%u_tctype's
         * omit-means-preserve fallback (see parse_zone_fields()'s comment) --
         * tmp itself is zeroed, so tmp.zones[i] can't supply "what this
         * channel is already set to." */
        if (!parse_zone_fields(body, i, tmp.thermo_count, tmp.relay_count, tmp.timing_profile_count,
                               &s_zones.cfg.zones[i], &tmp.zones[i], &err_reason)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err_reason);
            free(body);
            return ESP_OK;
        }
    }
    /* parse_zone_fields()'s own per-zone chain-walk (above, inside that
     * function) only ever sees ONE zone's link changing at a time -- it
     * checks the new link for zone i against every OTHER zone's LIVE stored
     * value, which is correct for that single write but blind to a
     * whole-page POST that changes SEVERAL zones' links in the same
     * request, none of which is a cycle by itself against the old live
     * config, but which together close one (e.g. z0_settings_source=1 and
     * z1_settings_source=0 in the same POST when neither zone pointed
     * anywhere before). tmp.zones[] now holds every zone's fully-assembled
     * NEW link, so re-walk every zone's chain against THAT -- before the
     * commit point below, so a rejection here still leaves s_zones
     * untouched, same "never partially apply" discipline the rest of this
     * handler follows. Bounded by tmp.thermo_count, same "unused trailing
     * slot" discipline zones_config_json_settings_source_chain_has_cycle()'s own comment
     * explains -- a slot past this submission's own thermo_count was never
     * rendered and never posted to, so it is excluded from this walk
     * entirely rather than treated as a real link. (It does NOT read back at
     * a zero-initialized default: parse_zone_fields()'s early return for
     * such a slot does `*z = *current_z`, so tmp.zones[i] carries whatever
     * settings_source is already LIVE and stored for that zone, not 0 --
     * still bounded out of this walk on principle, since that live value was
     * not part of this submission either, but the "reads back as 0" premise
     * would be wrong if repeated as a reason.) */
    for (uint8_t i = 0; i < tmp.thermo_count && i < MAX31856_CHANNEL_COUNT; i++) {
        if (zones_config_json_settings_source_chain_has_cycle(tmp.zones, i, tmp.thermo_count)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                "zone settings_source would create an inheritance cycle");
            free(body);
            return ESP_OK;
        }
    }

    /* 2026-08-21, TODO.md owner-report item 3: the safety processor's own
     * thermocouple type (see zones_cfg_t::safety_tc_type's comment for why
     * this is a separate global setting rather than any zone's tc_type).
     * OPTIONAL, falling back to the current live value on omit -- same
     * "cannot silently relinearize a channel against the wrong type"
     * reasoning as z%u_tctype above, and for the identical reason: an older
     * client that predates this field must not zero a real, physically
     * meaningful setting just by doing an otherwise-ordinary whole-page
     * save. Bounds match ZONE_TC_TYPE_MAX_REAL -- the safety processor's own
     * MAX31856 is the same part with the same eight real thermocouple types;
     * see that macro's comment. */
    {
        char val[8];
        int len = http_form_find_field(body, "safety_tc_type", val, sizeof(val));
        if (len > 0) {
            char *end = NULL;
            long v = strtol(val, &end, 10);
            /* *end != '\0' rejects trailing garbage, same gap as the other
             * numeric parsers in this file -- see zones_config_json_parse_u8_field()'s comment. */
            if (end == val || *end != '\0' || v < 0 || v > ZONE_TC_TYPE_MAX_REAL) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                    "safety_tc_type must be a real thermocouple type (0-7: "
                                    "B/E/J/K/N/R/S/T), not a voltage-input mode");
                free(body);
                return ESP_OK;
            }
            tmp.safety_tc_type = (uint8_t)v;
        } else {
            tmp.safety_tc_type = s_zones.cfg.safety_tc_type;
        }
    }

    /* The one global v8 override. OPTIONAL, and on omit it keeps the CURRENT
     * live value rather than resetting to 0 -- the same reasoning as
     * safety_tc_type above: this is a whole-page submit, and an older client
     * that predates the field must not silently reset how long a firing
     * tolerates a dead PC link just by saving the zones page. */
    {
        char val[16];
        int len = http_form_find_field(body, "pc_link_abort_silence_ms", val, sizeof(val));
        if (len > 0) {
            if (!zones_config_json_parse_float_field(body, "pc_link_abort_silence_ms", 0.0f,
                                   ZONE_PC_LINK_SILENCE_MS_MAX, &tmp.pc_link_abort_silence_ms)) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                    "pc_link_abort_silence_ms out of range (0 = firmware default)");
                free(body);
                return ESP_OK;
            }
        } else {
            tmp.pc_link_abort_silence_ms = s_zones.cfg.pc_link_abort_silence_ms;
        }
    }

    /* 2026-08-27+1 (owner request: name relays not assigned to any zone).
     * relay<N>_name (N = 1..KILN_IO_RELAY_COUNT, matching relay_mask's wire
     * numbering) is GLOBAL, relay-indexed, not per-zone -- so it follows the
     * same "omitted means keep the current live value" convention this
     * handler already uses for safety_tc_type/pc_link_abort_silence_ms
     * above, NOT the per-zone fields' "omitted means zero" convention
     * (tmp.zones[] is zero-initialized; this isn't). That matters here even
     * more than it does for those: zones_page.html and safety_config_page.html
     * BOTH POST to this same endpoint, and only zones_page.html renders a
     * relay-name input at all (see its renderRelayNames()) -- a save
     * triggered from /settings/safety must not wipe every relay name just
     * because that page has no editable field for them. safety_config_page.html
     * still echoes them anyway (defense in depth, matching its existing
     * safety_tc_type/pc_link_abort_silence_ms echo style), but this
     * omit-preserves convention is what actually GUARANTEES neither page can
     * clobber the other's relay names, independent of whether that echo is
     * ever forgotten in a future edit to either page.
     *
     * Storage is unconditional regardless of the relay's CURRENT zone
     * ownership -- see this file's relay-names section header comment for
     * why a name is kept, not cleared, when its relay becomes zone-owned.
     * Present-but-overlong is still rejected (a real mistake, not a
     * deliberate omission), same as z%u_name's -2 handling in
     * parse_zone_fields(). Parsed into a scratch copy of the CURRENT live
     * relay names, not applied to s_relay_names.cfg directly, so a request
     * that gets rejected later in this handler (impossible past this point
     * today, but kept for the same "never partially apply" discipline every
     * other section of this handler follows) has touched nothing. */
    relay_names_cfg_t tmp_relay_names = s_relay_names.cfg;
    for (uint8_t r = 1; r <= KILN_IO_RELAY_COUNT; r++) {
        char rkey[16];
        snprintf(rkey, sizeof(rkey), "relay%u_name", r);
        char rval[RELAY_NAME_MAX_LEN + 1];
        int rlen = http_form_find_field(body, rkey, rval, sizeof(rval));
        if (rlen == -2) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "relay name too long");
            free(body);
            return ESP_OK;
        }
        if (rlen >= 0) {
            strncpy(tmp_relay_names.names[r - 1], rval, RELAY_NAME_MAX_LEN);
            tmp_relay_names.names[r - 1][RELAY_NAME_MAX_LEN] = '\0';
        }
        /* rlen < 0 (omitted): tmp_relay_names.names[r-1] already holds the
         * current live value, copied above -- left untouched. */
    }

    /* Commit point: every rejection above returned before touching s_zones,
     * so this is the first and only line at which the submission becomes the
     * live config -- and therefore the only place in this handler the
     * generation may advance. A 400'd submission changed nothing and must
     * not make a running profile re-read identical settings (TODO.md
     * 6A.7). */
    s_zones.cfg = tmp;
    s_relay_names.cfg = tmp_relay_names;
    /* A validated, freshly-submitted config is trustworthy the moment it's
     * live in RAM, regardless of whether the NVS write below succeeds --
     * same "applied now either way" convention nvs_save()'s failure handling
     * already uses below. This is the other half of s_zones_config_valid's
     * contract: true after either a real successful load OR a fresh valid
     * save. */
    s_zones_config_valid = true;
    s_config_generation++;
    esp_err_t err = nvs_save();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_save failed: %s -- config applied live but will not survive a reboot",
                 esp_err_to_name(err));
        /* Still applied above -- the operator asked for this right now,
         * whether or not it persists past a reboot, same convention as
         * wifi_prov.c's nvs_save_creds failure handling. */
    }
    esp_err_t names_err = relay_names_save();
    if (names_err != ESP_OK) {
        ESP_LOGE(TAG, "relay_names_save failed: %s -- names applied live but will not survive a reboot",
                 esp_err_to_name(names_err));
        /* Same "applied now either way" convention as nvs_save() above --
         * cosmetic data that failed to persist is not worth refusing a
         * whole-page save that DID validate and apply everything else. */
    }
    free(body);
    return httpd_resp_sendstr(req, "ok");
}

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
            ESP_LOGW(TAG, "zones/pid body read failed/short: %d", ret);
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
    ESP_LOGI(TAG, "POST /api/zones/pid: zone %u gains -> kp=%.6g ki=%.6g kd=%.6g", zone_index, (double)kp,
             (double)ki, (double)kd);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

