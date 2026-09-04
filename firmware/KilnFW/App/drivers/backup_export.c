// Export side of backup_http.c's split (2026-09-04, ROADMAP.md M15's
// 1500-line item) -- see backup_http_internal.h for the full split map.
// MOVE-ONLY: GET /settings/backup and GET /api/backup/export, unchanged
// apart from widening backup_page_get_handler()/backup_export_get_handler()
// from `static` to file-scope-internal linkage (declared in
// backup_http_internal.h) so backup_http.c's route table can name them.
// json_escape() stays `static` here -- it is only used within this file
// (the import side has no need to escape anything it writes back out) --
// and is DELIBERATELY NOT widened/shared even though a copy of the same
// logic lives in dashboard_json.c: that copy is a non-static
// `void json_escape(...)`, so widening this one would be an immediate link
// error, exactly the collision class backup_http_internal.h's own comment
// calls out. Keeping this copy static, under its original name, is what the
// original single file already did (see diagnostics_http.c and
// kiln_cfg_http.c for two other files carrying their own static copy of the
// same helper for the same reason).

#include "backup_http.h"
#include "backup_http_internal.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "MAX31856.h"
#include "profiles_http.h"
#include "web_encoding.h"
#include "wifi_provision_http.h"
#include "zones_http.h"

/* Embedded via EMBED_TXTFILES, pre-gzipped at configure time by
 * App/drivers/CMakeLists.txt -- same convention as every other *_page.html
 * in this component. */
extern const uint8_t backup_page_html_gz_start[] asm("_binary_backup_page_html_gz_start");
extern const uint8_t backup_page_html_gz_end[] asm("_binary_backup_page_html_gz_end");

esp_err_t backup_page_get_handler(httpd_req_t *req)
{
    if (!web_client_accepts_gzip(req)) {
        return web_send_gzip_not_acceptable(req, BACKUP_TAG, "backup_page.html");
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    web_set_asset_cache_headers(req);
    return httpd_resp_send(req, (const char *)backup_page_html_gz_start,
                           (size_t)(backup_page_html_gz_end - backup_page_html_gz_start));
}

/* ---- Streamed JSON writer, export side ------------------------------------
 *
 * Same reason dashboard_http.c's history_csv_get_handler() streams rather
 * than building one big buffer: this pass's own instruction is explicit
 * ("do NOT build the whole export in one heap buffer"). Unlike a CSV export
 * there is no natural per-line boundary, so this is a small ring-free
 * fixed-size buffer that flushes via httpd_resp_send_chunk() whenever a
 * printf'd fragment would overflow it -- the buffer holds at most
 * BACKUP_STREAM_BUF bytes of in-flight JSON at any moment, not the whole
 * document. */
#define BACKUP_STREAM_BUF 256

typedef struct {
    httpd_req_t *req;
    char *buf;
    size_t len;
    esp_err_t err; /* first send error, if any; every later call becomes a no-op */
} backup_stream_t;

static void backup_stream_flush(backup_stream_t *s)
{
    if (s->err != ESP_OK || s->len == 0) {
        return;
    }
    s->err = httpd_resp_send_chunk(s->req, s->buf, s->len);
    s->len = 0;
}

static void backup_stream_printf(backup_stream_t *s, const char *fmt, ...)
{
    if (s->err != ESP_OK) {
        return;
    }
    char tmp[192]; /* one JSON fragment (one field, one small object) at a time */
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    size_t tn = (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1;
    size_t off = 0;
    while (off < tn) {
        size_t space = BACKUP_STREAM_BUF - s->len;
        size_t take = (tn - off) < space ? (tn - off) : space;
        memcpy(s->buf + s->len, tmp + off, take);
        s->len += take;
        off += take;
        if (s->len == BACKUP_STREAM_BUF) {
            backup_stream_flush(s);
            if (s->err != ESP_OK) {
                return;
            }
        }
    }
}

/* Same escaping convention as every other *_http.c's json_escape -- a
 * profile name came from a POST body at some point, so it's untrusted-ish. */
static void json_escape(const char *src, char *out, size_t out_cap)
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

esp_err_t backup_export_get_handler(httpd_req_t *req)
{
    /* HEAP in PSRAM, not internal DRAM: same fix, same reasoning as
     * dashboard_http.c's GET /api/status buffer -- this handler's own
     * response chunking already keeps this small (256B), but every response
     * buffer moved off internal DRAM is one less contributor to the httpd
     * worker's DRAM pressure this file's siblings document elsewhere. Freed
     * on every return path below. */
    char *buf = heap_caps_malloc(BACKUP_STREAM_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }
    backup_stream_t s = { .req = req, .buf = buf, .len = 0, .err = ESP_OK };

    httpd_resp_set_type(req, "application/json");
    /* Wi-Fi credentials are never in this document -- see backup_http.h's
     * header comment for why -- so, unlike ota_http.c's image transfers,
     * there is no secret in this download worth a stricter cache policy than
     * the default. */
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"kilnctl_backup.json\"");

    backup_stream_printf(&s, "{\"kind\":\"kilnctl_backup\",\"version\":%d,\"profiles\":[", BACKUP_FORMAT_VERSION);

    bool first_profile = true;
    for (uint8_t id = 0; id < PROFILES_MAX_COUNT; id++) {
        profile_t p;
        /* id < PROFILES_MAX_COUNT never resolves to a builtin catalogue
         * entry (those live at id >= PROFILE_BUILTIN_ID_BASE, 128) -- this
         * only ever returns a real user-saved slot, or false for an unused
         * one. Builtins are shipped-in-flash and not this board's data to
         * back up; an operator who copied one into a slot already has it
         * here as that slot's own entry. */
        if (!profiles_http_get(id, &p)) {
            continue;
        }
        char name_escaped[PROFILE_NAME_MAX_LEN * 2 + 1];
        json_escape(p.name, name_escaped, sizeof(name_escaped));
        backup_stream_printf(&s, "%s{\"id\":%u,\"name\":\"%s\",\"zone_mask\":%u,\"segments\":[",
                            first_profile ? "" : ",", id, name_escaped, p.zone_mask);
        first_profile = false;
        for (uint8_t i = 0; i < p.segment_count && i < PROFILE_MAX_SEGMENTS; i++) {
            const profile_segment_t *seg = &p.segments[i];
            backup_stream_printf(&s, "%s{\"target_c\":%.2f,\"ramp_c_per_hr\":%.2f,\"dwell_min\":%lu}",
                                i == 0 ? "" : ",", (double)seg->target_c, (double)seg->ramp_c_per_hr,
                                (unsigned long)seg->dwell_min);
        }
        backup_stream_printf(&s, "]}");
    }
    backup_stream_printf(&s, "],\"zones\":[");

    bool first_zone = true;
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        float kp, ki, kd, k_dc, tau_s, dead_time_s;
        uint8_t tc_type;
        /* All four getters share the same "false = zone_index unconfigured"
         * convention (see zones_http.h) -- if PID isn't answerable, this
         * channel has never been configured at all, so it contributes
         * nothing to the backup rather than a half-populated entry. */
        if (!zones_config_get_pid(zi, &kp, &ki, &kd)) {
            continue;
        }
        bool have_model = zones_config_get_model(zi, &k_dc, &tau_s, &dead_time_s);
        bool have_tc = zones_config_get_tc_type(zi, &tc_type);
        backup_stream_printf(&s, "%s{\"index\":%u,\"pid_kp\":%.4f,\"pid_ki\":%.4f,\"pid_kd\":%.4f,",
                            first_zone ? "" : ",", zi, (double)kp, (double)ki, (double)kd);
        first_zone = false;
        /* model_* and tc_type are emitted only when answerable -- see
         * zones_config_get_model()'s own doc comment: all-zero legitimately
         * means "no model fitted yet" for a freshly configured zone, so
         * skipping the key (not emitting 0) is what tells an eventual
         * restore not to overwrite a fitted model on a board that has one
         * with a "no model" this backup never actually measured. */
        if (have_model) {
            backup_stream_printf(&s, "\"model_k_dc\":%.4f,\"model_tau_s\":%.1f,\"model_dead_time_s\":%.1f,",
                                (double)k_dc, (double)tau_s, (double)dead_time_s);
        }
        if (have_tc) {
            backup_stream_printf(&s, "\"tc_type\":%u,", tc_type);
        }
        /* 2026-08-21 (version 2): everything else a zone stores that used to
         * be read-only -- see zones_http.h's now-added setter for each of
         * these. All of these getters share zones_config_get_pid()'s own
         * "false = zone_index unconfigured" convention, and zi already
         * passed that check above (zones_config_get_pid() succeeded), so
         * every one of these is guaranteed answerable here -- unlike
         * have_model/have_tc above there is no "not yet measured" state for
         * any of them to skip. */
        {
            char name[ZONE_NAME_MAX_LEN + 1];
            zones_config_get_name(zi, name, sizeof(name));
            char name_escaped[ZONE_NAME_MAX_LEN * 2 + 1];
            json_escape(name, name_escaped, sizeof(name_escaped));
            uint8_t relay_mask = 0, thermo_mask = 0, ct_mask = 0;
            zones_config_get_relay_mask(zi, &relay_mask);
            zones_config_get_thermo_mask(zi, &thermo_mask);
            zones_config_get_ct_mask(zi, &ct_mask);
            float cal_offset_c = 0.0f;
            zones_config_get_cal_offset(zi, &cal_offset_c);
            float max_ramp_c_per_hr = 0.0f;
            zones_config_get_max_ramp(zi, &max_ramp_c_per_hr);
            float sanity_rate_c_per_min = 0.0f;
            zones_config_get_sanity_rate(zi, &sanity_rate_c_per_min);
            zone_control_mode_t control_mode = ZONE_CONTROL_MODE_OFF;
            zones_config_get_control_mode(zi, &control_mode);
            float max_temp_c = 0.0f, min_temp_c = 0.0f;
            zones_config_get_temp_limits(zi, &max_temp_c, &min_temp_c);
            float window_ms = 0.0f, min_on_ms = 0.0f, min_off_ms = 0.0f;
            zones_config_get_heater_cfg(zi, &window_ms, &min_on_ms, &min_off_ms);
            float wrong_dir_window_s = 0.0f, wrong_dir_rate_c_per_min = 0.0f, off_settle_s = 0.0f,
                  runaway_rate_c_per_min = 0.0f, runaway_margin_c = 0.0f, drift_period_s = 0.0f,
                  sensor_fault_debounce_ticks = 0.0f, frozen_window_s = 0.0f;
            zones_config_get_guard_thresholds(zi, &wrong_dir_window_s, &wrong_dir_rate_c_per_min, &off_settle_s,
                                              &runaway_rate_c_per_min, &runaway_margin_c, &drift_period_s,
                                              &sensor_fault_debounce_ticks, &frozen_window_s);
            float cross_zone_max_delta_c = 0.0f;
            zones_config_get_cross_zone_delta(zi, &cross_zone_max_delta_c);
            /* Version 3 (2026-08-30): PID_EXPANSION_PLAN.md Phase 2/4's four
             * new fields. Same "always answerable once zi passed the pid_kp
             * check above" reasoning as every other version-2 field in this
             * block -- no "not yet measured" state to skip, unlike
             * have_model/have_tc. */
            float fuzzy_strength_pct = 0.0f;
            zones_config_get_fuzzy_strength_pct(zi, &fuzzy_strength_pct);
            float coupling_row[MAX31856_CHANNEL_COUNT] = {0};
            zones_config_get_coupling(zi, coupling_row);
            /* ZONES_CFG_VERSION 11->12 (DATA PLUMBING pass): the tau/L
             * siblings of coupling_row above -- same "always answerable"
             * reasoning, same getters' own zeroed-array contract on a zone
             * with nothing measured yet. */
            float coupling_tau_row[MAX31856_CHANNEL_COUNT] = {0};
            zones_config_get_coupling_tau(zi, coupling_tau_row);
            float coupling_dead_row[MAX31856_CHANNEL_COUNT] = {0};
            zones_config_get_coupling_dead_time(zi, coupling_dead_row);
            uint8_t settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;
            zones_config_get_settings_source(zi, &settings_source);

            /* Each fragment kept comfortably under backup_stream_printf()'s
             * own tmp[192] scratch buffer (including formatted values, not
             * just the format string) -- one big fragment covering all of
             * these keys at once measured out to well over 192 bytes once
             * values were substituted, which would have been silently
             * truncated by that function's own overflow clamp rather than
             * erroring, so this is split into several smaller calls instead
             * of one that could quietly drop the tail of a zone's entry. */
            backup_stream_printf(&s, "\"name\":\"%s\",\"relay_mask\":%u,\"thermo_mask\":%u,\"ct_mask\":%u,",
                                name_escaped, relay_mask, thermo_mask, ct_mask);
            backup_stream_printf(&s, "\"cal_offset_c\":%.3f,\"max_ramp_c_per_hr\":%.2f,"
                                "\"sanity_rate_c_per_min\":%.3f,\"control_mode\":%u,",
                                (double)cal_offset_c, (double)max_ramp_c_per_hr, (double)sanity_rate_c_per_min,
                                (unsigned)control_mode);
            backup_stream_printf(&s, "\"max_temp_c\":%.1f,\"min_temp_c\":%.1f,\"heater_window_ms\":%.0f,"
                                "\"heater_min_on_ms\":%.0f,\"heater_min_off_ms\":%.0f,",
                                (double)max_temp_c, (double)min_temp_c, (double)window_ms, (double)min_on_ms,
                                (double)min_off_ms);
            backup_stream_printf(&s, "\"guard_wrong_dir_window_s\":%.1f,\"guard_wrong_dir_rate_c_per_min\":%.3f,"
                                "\"guard_off_settle_s\":%.1f,",
                                (double)wrong_dir_window_s, (double)wrong_dir_rate_c_per_min,
                                (double)off_settle_s);
            backup_stream_printf(&s, "\"guard_runaway_rate_c_per_min\":%.3f,\"guard_runaway_margin_c\":%.1f,"
                                "\"guard_drift_period_s\":%.1f,",
                                (double)runaway_rate_c_per_min, (double)runaway_margin_c,
                                (double)drift_period_s);
            backup_stream_printf(&s, "\"guard_sensor_fault_debounce_ticks\":%.0f,\"guard_frozen_window_s\":%.1f,"
                                "\"cross_zone_max_delta_c\":%.1f,",
                                (double)sensor_fault_debounce_ticks, (double)frozen_window_s,
                                (double)cross_zone_max_delta_c);
            backup_stream_printf(&s, "\"fuzzy_strength_pct\":%.2f,", (double)fuzzy_strength_pct);
            /* Version 4 (2026-08-30, same-day follow-up): coupling_c0..
             * coupling_cN-1, one indexed key per neighbor -- see
             * BACKUP_FORMAT_VERSION's own 3->4 comment. Diagonal included
             * (always 0), same always-emit convention every other field in
             * this block already follows. */
            for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
                backup_stream_printf(&s, "\"coupling_c%u\":%.4f,", (unsigned)j, (double)coupling_row[j]);
            }
            /* ZONES_CFG_VERSION 11->12 (DATA PLUMBING pass): coupling_tau_c%u/
             * coupling_dead_time_c%u, same per-cell always-emit shape as
             * coupling_c%u just above -- no BACKUP_FORMAT_VERSION bump, see
             * backup_import_apply()'s own comment on the matching parse. */
            for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
                backup_stream_printf(&s, "\"coupling_tau_c%u\":%.1f,", (unsigned)j, (double)coupling_tau_row[j]);
            }
            for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
                backup_stream_printf(&s, "\"coupling_dead_time_c%u\":%.1f,", (unsigned)j, (double)coupling_dead_row[j]);
            }
            /* ZONES_CFG_VERSION 14->15 (PID_EXPANSION_PLAN.md 3.2 follow-up):
             * the coupling identification's own diagonal cell -- see
             * zone_cfg_t::coupling_diag_k_dc's own doc comment. No
             * BACKUP_FORMAT_VERSION bump, same reasoning as coupling_tau_c%u/
             * coupling_dead_time_c%u above -- purely an OPTIONAL additive key,
             * see backup_import_apply()'s own comment on the matching parse. */
            float coupling_diag_k_dc = 0.0f;
            zones_config_get_coupling_diag_k_dc(zi, &coupling_diag_k_dc);
            backup_stream_printf(&s, "\"coupling_diag_k_dc\":%.4f,", (double)coupling_diag_k_dc);
            backup_stream_printf(&s, "\"settings_source\":%u", (unsigned)settings_source);
        }
        /* settings_source above is the last key of this object now (it was
         * cross_zone_max_delta_c before settings_source was added) and is
         * always emitted (every entry that reaches this point already
         * emitted pid_kp, have_model/have_tc are the only optional keys and
         * both come before this block) -- no trailing-comma guard needed, so
         * this just closes the object. Previously emitted a junk "_":0
         * sentinel key here purely to dodge a trailing comma; removed
         * (2026-08-21) since every real key already has a place before the
         * close. */
        backup_stream_printf(&s, "}");
    }
    backup_stream_printf(&s, "]");

    uint8_t safety_tc_type = 0;
    zones_config_get_safety_tc_type(&safety_tc_type); /* only fails on NULL out-pointer -- never here */
    backup_stream_printf(&s, ",\"safety_tc_type\":%u}", safety_tc_type);

    backup_stream_flush(&s);
    free(buf);
    if (s.err == ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0); /* terminates the chunked response */
    }
    return ESP_OK;
}
