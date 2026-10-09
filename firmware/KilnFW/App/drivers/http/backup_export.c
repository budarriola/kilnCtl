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
#include "update_settings.h" /* WP9: the persisted update repo, top-level "update_repo" */
#include "wifi_provision_http.h"
#include "zones_config_accessors.h"
#include "zones_config_json.h" /* relay_type/ease_off_window_mult/approach_rate_cap/
                                 * error_band_c/rate_band_c_per_s getters+setters --
                                 * 2026-09-16 backup-round-trip-gap closure, see
                                 * docs/audits for the field-by-field enumeration */
#include "zones_http_internal.h" /* zone_normals_set()/zones_config_get_normal_current()
                                   * -- CT normals, the owner's own named example */
#include "relay_cycles.h" /* top-level "relay_cycles" wear counters */
#include "aux_outputs_cfg.h" /* top-level "aux_outputs" array (spare-relay on/off outputs) */
#include "kiln_cfg_store.h" /* KILN_PROFILES_PLAN.md item 17 follow-up: "kiln_configs"
                              * array below -- every saved kiln config slot, not just
                              * the active one, is now part of the backup document. */

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

/* Streams `len` raw bytes verbatim (no vsnprintf, no escaping) -- used only
 * for splicing an already-valid JSON fragment (kiln_cfg_store_export_package_
 * json()'s output, up to KILN_CFG_EXPORT_JSON_MAX_LEN=6144 bytes) into the
 * document as-is. backup_stream_printf()'s 192-byte tmp[] can't hold a
 * fragment this large; this bypasses that buffer and feeds
 * BACKUP_STREAM_BUF-sized pieces straight from the source. */
static void backup_stream_raw(backup_stream_t *s, const char *data, size_t len)
{
    size_t off = 0;
    while (off < len) {
        if (s->err != ESP_OK) {
            return;
        }
        size_t space = BACKUP_STREAM_BUF - s->len;
        size_t take = (len - off) < space ? (len - off) : space;
        memcpy(s->buf + s->len, data + off, take);
        s->len += take;
        off += take;
        if (s->len == BACKUP_STREAM_BUF) {
            backup_stream_flush(s);
        }
    }
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
    if ((size_t)n >= sizeof(tmp)) {
        /* A fragment that does not fit tmp[] would be emitted TRUNCATED -- a silently corrupt backup that
         * only fails (or worse, imports short) on restore. Fail the export loudly instead. */
        ESP_LOGE(BACKUP_TAG, "backup export: JSON fragment of %d bytes exceeds the %u-byte buffer -- aborting", n,
                 (unsigned)sizeof(tmp));
        s->err = ESP_ERR_INVALID_SIZE;
        return;
    }
    size_t tn = (size_t)n;
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

/* httpd-stack-budget fix (2026-09-09, check_httpd_task_stack_budget RED at
 * 4848 B / 4832 B ceiling): this handler's own frame carried a `profile_t p`
 * (~376 B: 12-entry segments[] + 8-entry on_off_rules[]) inside the profile
 * loop, plus a second cluster of locals inside the zone loop (name[16],
 * name_escaped[31], three MAX31856_CHANNEL_COUNT-wide float arrays, a
 * SRC_GROUP_COUNT settings_source_group[], and ~20 scalar floats/uint8s read
 * one accessor call at a time) that the compiler could not prove
 * non-overlapping with the profile loop's own locals since both loops are
 * siblings in the same function body. Same fix as this file's
 * `setup_progress_http.c`/`safety_cfg_http.c` precedents cited in
 * check_httpd_task_stack_budget.py's own history comment: heap-allocate the
 * two scratch blocks instead of declaring them as stack locals, freed on
 * every return path. Neither struct is touched by more than one loop
 * iteration at a time, so one instance of each, reused per iteration, is
 * enough -- no need to size either for PROFILES_MAX_COUNT/
 * MAX31856_CHANNEL_COUNT-many entries at once. */
typedef struct {
    profile_t p;
    char name_escaped[PROFILE_NAME_MAX_LEN * 2 + 1];
} backup_export_profile_scratch_t;

typedef struct {
    char name[ZONE_NAME_MAX_LEN + 1];
    char name_escaped[ZONE_NAME_MAX_LEN * 2 + 1];
    float coupling_row[MAX31856_CHANNEL_COUNT];
    float coupling_tau_row[MAX31856_CHANNEL_COUNT];
    float coupling_dead_row[MAX31856_CHANNEL_COUNT];
    uint8_t settings_source_group[SRC_GROUP_COUNT];
} backup_export_zone_scratch_t;

/* Top-level "aux_outputs" array: the spare-relay on/off outputs
 * (docs/SPARE_RELAY_ONOFF_PLAN.md), one entry per relay, in the same shape GET /api/aux_outputs
 * prints for each of its relays[] (relay 1-based, enabled bool, tc_zone -1 = none, hyst_c,
 * min_on_s, min_off_s) -- minus "conflicted", which is derived state, not configuration. The
 * values are the EFFECTIVE ones (defaults substituted), hyst_c at %.9g so a float32 round-trips
 * exactly (the live route's %.2f would not). A quarantined store (newer-firmware blob) is NOT
 * exported: its effective view is all-disabled defaults, not the stored data, and a restore must
 * not stamp those over it -- the key is omitted, which import treats as "preserve". Not part of a
 * kiln package. No BACKUP_FORMAT_VERSION bump: an absent key is a no-op on import (same optional
 * key rule as update_repo). Kept out of line so its locals never join the handler's frame. */
#if defined(_MSC_VER)
#define BACKUP_EXPORT_NOINLINE
#else
#define BACKUP_EXPORT_NOINLINE __attribute__((noinline))
#endif
static BACKUP_EXPORT_NOINLINE void backup_export_aux_outputs(backup_stream_t *s)
{
    if (aux_outputs_cfg_quarantined()) {
        return;
    }
    backup_stream_printf(s, ",\"aux_outputs\":[");
    bool first = true;
    for (uint8_t relay = 1; relay <= AUX_OUTPUTS_COUNT; relay++) {
        aux_output_t a;
        if (!aux_outputs_cfg_get(relay, &a)) {
            continue;
        }
        backup_stream_printf(s,
                             "%s{\"relay\":%u,\"enabled\":%s,\"tc_zone\":%d,\"hyst_c\":%.9g,"
                             "\"min_on_s\":%u,\"min_off_s\":%u}",
                             first ? "" : ",", (unsigned)relay, a.enabled ? "true" : "false",
                             a.tc_zone == AUX_TC_ZONE_NONE ? -1 : (int)a.tc_zone, (double)a.hyst_c,
                             (unsigned)a.min_on_s, (unsigned)a.min_off_s);
        first = false;
    }
    backup_stream_printf(s, "]");
}

/* Top-level "relay_cycles": the per-relay contact-wear counters (RELAY_CYCLES_COUNT slots: the four
 * heater relays c0..c3 plus the safety relay K4 as c4) and "hw_relays", the heater relay count of the
 * board that wrote them. Wear history the operator cannot regenerate, so it is exported (the kiln
 * factory reset erases it). Import is raise-only (relay_cycles_restore_all's monotonic guard) and
 * refuses a backup whose hw_relays differs. Optional key, no BACKUP_FORMAT_VERSION bump: absent = no-op. */
static BACKUP_EXPORT_NOINLINE void backup_export_relay_cycles(backup_stream_t *s)
{
    uint32_t c[RELAY_CYCLES_COUNT];
    relay_cycles_get_all(c);
    backup_stream_printf(s, ",\"relay_cycles\":{\"hw_relays\":%u", (unsigned)KILN_IO_RELAY_COUNT);
    for (unsigned i = 0; i < RELAY_CYCLES_COUNT; i++) {
        backup_stream_printf(s, ",\"c%u\":%lu", i, (unsigned long)c[i]);
    }
    backup_stream_printf(s, "}");
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
    /* Both scratch blocks allocated up front, before anything is sent, so an
     * OOM here is a clean 500 rather than a truncated mid-stream response --
     * same "allocate before the first byte goes out" ordering as buf above. */
    backup_export_profile_scratch_t *ps = heap_caps_malloc(sizeof(*ps), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    backup_export_zone_scratch_t *zs = heap_caps_malloc(sizeof(*zs), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    /* kiln_configs[] scratch (item 17 follow-up) -- allocated here, up front
     * with everything else, for the same "OOM is a clean 500, never a
     * truncated mid-stream response" reason. */
    char *pkg_json_scratch = heap_caps_malloc(KILN_CFG_EXPORT_JSON_MAX_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf || !ps || !zs || !pkg_json_scratch) {
        free(buf);
        free(ps);
        free(zs);
        free(pkg_json_scratch);
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
        profile_t *p = &ps->p;
        /* id < PROFILES_MAX_COUNT never resolves to a builtin catalogue
         * entry (those live at id >= PROFILE_BUILTIN_ID_BASE, 128) -- this
         * only ever returns a real user-saved slot, or false for an unused
         * one. Builtins are shipped-in-flash and not this board's data to
         * back up; an operator who copied one into a slot already has it
         * here as that slot's own entry. */
        if (!profiles_http_get(id, p)) {
            continue;
        }
        char *name_escaped = ps->name_escaped;
        json_escape(p->name, name_escaped, PROFILE_NAME_MAX_LEN * 2 + 1);
        backup_stream_printf(&s, "%s{\"id\":%u,\"name\":\"%s\",\"zone_mask\":%u,\"segments\":[",
                            first_profile ? "" : ",", id, name_escaped, p->zone_mask);
        first_profile = false;
        for (uint8_t i = 0; i < p->segment_count && i < PROFILE_MAX_SEGMENTS; i++) {
            const profile_segment_t *seg = &p->segments[i];
            /* seg_kind/io_* carried so RELAY_IO segments survive a restore (same key names as
             * profiles_export_http.c; absent on an older backup = ZONE_RAMP, io_* 0). */
            backup_stream_printf(&s,
                                "%s{\"seg_kind\":%u,\"target_c\":%.9g,\"ramp_c_per_hr\":%.9g,\"dwell_min\":%lu,"
                                "\"io_target\":%u,\"io_state\":%u,\"io_blocking\":%u,\"io_leave_on_at_end\":%u}",
                                i == 0 ? "" : ",", seg->seg_kind, (double)seg->target_c,
                                (double)seg->ramp_c_per_hr, (unsigned long)seg->dwell_min, seg->io_target,
                                seg->io_state, seg->io_blocking, seg->io_leave_on_at_end);
        }
        /* on_off_rules: zone is the target byte (0..2 zone, 8..11 aux relay 1..4). No version bump:
         * an absent key imports as zero rules, same as before. */
        backup_stream_printf(&s, "],\"on_off_rules\":[");
        for (uint8_t i = 0; i < p->on_off_rule_count && i < PROFILE_MAX_ON_OFF_RULES; i++) {
            const profile_on_off_rule_t *r = &p->on_off_rules[i];
            backup_stream_printf(&s,
                                "%s{\"zone\":%u,\"segment\":%u,\"enable\":%u,\"phase_mask\":%u,"
                                "\"direction_mask\":%u,\"temp_source\":%u,\"temp_cmp\":%u,\"temp_c\":%.9g,"
                                "\"time_start_s\":%u,\"time_stop_s\":%u,\"invert\":%u}",
                                i == 0 ? "" : ",", r->zone_index, r->segment_index, r->enable, r->phase_mask,
                                r->direction_mask, r->temp_source, r->temp_cmp, (double)r->temp_threshold_c,
                                r->time_start_s, r->time_stop_s, r->invert);
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
        backup_stream_printf(&s, "%s{\"index\":%u,\"pid_kp\":%.9g,\"pid_ki\":%.9g,\"pid_kd\":%.9g,",
                            first_zone ? "" : ",", zi, (double)kp, (double)ki, (double)kd);
        first_zone = false;
        /* model_* and tc_type are emitted only when answerable -- see
         * zones_config_get_model()'s own doc comment: all-zero legitimately
         * means "no model fitted yet" for a freshly configured zone, so
         * skipping the key (not emitting 0) is what tells an eventual
         * restore not to overwrite a fitted model on a board that has one
         * with a "no model" this backup never actually measured. */
        if (have_model) {
            backup_stream_printf(&s, "\"model_k_dc\":%.9g,\"model_tau_s\":%.9g,\"model_dead_time_s\":%.9g,",
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
            char *name = zs->name;
            zones_config_get_name(zi, name, ZONE_NAME_MAX_LEN + 1);
            char *name_escaped = zs->name_escaped;
            json_escape(name, name_escaped, ZONE_NAME_MAX_LEN * 2 + 1);
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
            /* zones_config_get_coupling_raw(), not zones_config_get_coupling():
             * the latter zeroes any row/column touching an on/off zone
             * (docs/ON_OFF_ZONE_PLAN.md sec 1's belt-and-braces control-loop
             * guard) -- reading through it here exported 0.0 for a
             * currently on/off zone's real, previously-measured coupling
             * cells, and a later import then committed that 0.0 as the new
             * stored value via zones_config_set_coupling_cell() (which has
             * no on/off awareness), permanently losing the real number.
             * Found via bench A4 (2026-09-28). The raw accessor round-trips
             * the true stored value regardless of the zone's current type. */
            float *coupling_row = zs->coupling_row;
            memset(coupling_row, 0, sizeof(zs->coupling_row));
            zones_config_get_coupling_raw(zi, coupling_row);
            /* ZONES_CFG_VERSION 11->12 (DATA PLUMBING pass): the tau/L
             * siblings of coupling_row above -- same "always answerable"
             * reasoning, same getters' own zeroed-array contract on a zone
             * with nothing measured yet. */
            float *coupling_tau_row = zs->coupling_tau_row;
            memset(coupling_tau_row, 0, sizeof(zs->coupling_tau_row));
            zones_config_get_coupling_tau(zi, coupling_tau_row);
            float *coupling_dead_row = zs->coupling_dead_row;
            memset(coupling_dead_row, 0, sizeof(zs->coupling_dead_row));
            zones_config_get_coupling_dead_time(zi, coupling_dead_row);
            /* docs/ARCHITECTURE_DECISIONS.md#zones-page-clean-up-info-disclosure-schema-v20-v21-chartjs (ZONES_CFG_VERSION 20->21) split this
             * into SRC_GROUP_COUNT independent bytes. Opus review of 5672719
             * (item 4): the backup format now carries all five --
             * "settings_source" stays the LIMITS group's value, kept for
             * older readers of a backup taken from this build, and
             * "settings_source_g%u" (0..SRC_GROUP_COUNT-1) carries every
             * group explicitly so a zone whose groups point at different
             * sources round-trips exactly instead of collapsing to LIMITS's
             * choice for all five. An import from a version-4 backup that
             * only has the scalar key falls back to applying it to every
             * group -- see backup_import_apply()'s matching parse. */
            uint8_t *settings_source_group = zs->settings_source_group;
            for (uint8_t g = 0; g < SRC_GROUP_COUNT; g++) {
                settings_source_group[g] = ZONE_SETTINGS_SOURCE_CUSTOM;
                zones_config_get_settings_source(zi, g, &settings_source_group[g]);
            }
            uint8_t settings_source = settings_source_group[SRC_GROUP_LIMITS];

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
            backup_stream_printf(&s, "\"cal_offset_c\":%.9g,\"max_ramp_c_per_hr\":%.9g,"
                                "\"sanity_rate_c_per_min\":%.9g,\"control_mode\":%u,",
                                (double)cal_offset_c, (double)max_ramp_c_per_hr, (double)sanity_rate_c_per_min,
                                (unsigned)control_mode);
            backup_stream_printf(&s, "\"max_temp_c\":%.9g,\"min_temp_c\":%.9g,\"heater_window_ms\":%.9g,"
                                "\"heater_min_on_ms\":%.9g,\"heater_min_off_ms\":%.9g,",
                                (double)max_temp_c, (double)min_temp_c, (double)window_ms, (double)min_on_ms,
                                (double)min_off_ms);
            backup_stream_printf(&s, "\"guard_wrong_dir_window_s\":%.9g,\"guard_wrong_dir_rate_c_per_min\":%.9g,"
                                "\"guard_off_settle_s\":%.9g,",
                                (double)wrong_dir_window_s, (double)wrong_dir_rate_c_per_min,
                                (double)off_settle_s);
            backup_stream_printf(&s, "\"guard_runaway_rate_c_per_min\":%.9g,\"guard_runaway_margin_c\":%.9g,"
                                "\"guard_drift_period_s\":%.9g,",
                                (double)runaway_rate_c_per_min, (double)runaway_margin_c,
                                (double)drift_period_s);
            backup_stream_printf(&s, "\"guard_sensor_fault_debounce_ticks\":%.9g,\"guard_frozen_window_s\":%.9g,"
                                "\"cross_zone_max_delta_c\":%.9g,",
                                (double)sensor_fault_debounce_ticks, (double)frozen_window_s,
                                (double)cross_zone_max_delta_c);
            backup_stream_printf(&s, "\"fuzzy_strength_pct\":%.9g,", (double)fuzzy_strength_pct);
            /* Version 4 (2026-08-30, same-day follow-up): coupling_c0..
             * coupling_cN-1, one indexed key per neighbor -- see
             * BACKUP_FORMAT_VERSION's own 3->4 comment. Diagonal included
             * (always 0), same always-emit convention every other field in
             * this block already follows. */
            for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
                backup_stream_printf(&s, "\"coupling_c%u\":%.9g,", (unsigned)j, (double)coupling_row[j]);
            }
            /* ZONES_CFG_VERSION 11->12 (DATA PLUMBING pass): coupling_tau_c%u/
             * coupling_dead_time_c%u, same per-cell always-emit shape as
             * coupling_c%u just above -- no BACKUP_FORMAT_VERSION bump, see
             * backup_import_apply()'s own comment on the matching parse. */
            for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
                backup_stream_printf(&s, "\"coupling_tau_c%u\":%.9g,", (unsigned)j, (double)coupling_tau_row[j]);
            }
            for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
                backup_stream_printf(&s, "\"coupling_dead_time_c%u\":%.9g,", (unsigned)j, (double)coupling_dead_row[j]);
            }
            /* ZONES_CFG_VERSION 14->15 (PID_EXPANSION_PLAN.md 3.2 follow-up):
             * the coupling identification's own diagonal cell -- see
             * zone_cfg_t::coupling_diag_k_dc's own doc comment. No
             * BACKUP_FORMAT_VERSION bump, same reasoning as coupling_tau_c%u/
             * coupling_dead_time_c%u above -- purely an OPTIONAL additive key,
             * see backup_import_apply()'s own comment on the matching parse. */
            float coupling_diag_k_dc = 0.0f;
            zones_config_get_coupling_diag_k_dc(zi, &coupling_diag_k_dc);
            backup_stream_printf(&s, "\"coupling_diag_k_dc\":%.9g,", (double)coupling_diag_k_dc);
            backup_stream_printf(&s, "\"settings_source\":%u,", (unsigned)settings_source);
            for (uint8_t g = 0; g < SRC_GROUP_COUNT; g++) {
                backup_stream_printf(&s, "\"settings_source_g%u\":%u,", (unsigned)g,
                                    (unsigned)settings_source_group[g]);
            }
            /* 2026-09-16 backup-round-trip-gap closure: every one of these
             * has a PUBLIC getter+setter pair already (confirmed by direct
             * read of zones_config_accessors.h/zones_config_json.h), so each
             * was a field that read back nowhere and could be restored
             * nowhere -- exactly the "worse than never offered" case
             * backup_http.h's own header comment calls out. All are
             * "always answerable once zi passed the pid_kp check", same as
             * every version-2/3/4 field above -- no has_X skip needed here,
             * only on the import side where an OLDER package simply won't
             * carry the key. */
            float ease_off_window_mult = 0.0f;
            zones_config_get_ease_off_window_mult(zi, &ease_off_window_mult);
            float approach_rate_cap_c_per_hr = 0.0f;
            zones_config_get_approach_rate_cap_c_per_hr(zi, &approach_rate_cap_c_per_hr);
            float error_band_c = 0.0f, rate_band_c_per_s = 0.0f;
            zones_config_get_error_band_c(zi, &error_band_c);
            zones_config_get_rate_band_c_per_s(zi, &rate_band_c_per_s);
            uint8_t relay_type = 0;
            zones_config_get_relay_type(zi, &relay_type);
            float progress_band_c = 0.0f;
            zones_config_get_progress_band_c(zi, &progress_band_c);
            zone_type_t zone_type = ZONE_TYPE_HEATER;
            zones_config_get_zone_type(zi, &zone_type);
            float model_fit_temp_c = 0.0f, model_fit_ambient_c = 0.0f;
            zones_config_get_model_fit_context(zi, &model_fit_temp_c, &model_fit_ambient_c);
            float coil_power_w = 0.0f;
            zones_config_get_coil_power_w(zi, &coil_power_w);
            float autotune_baseline_k_dc = 0.0f;
            zones_config_get_autotune_baseline_k_dc(zi, &autotune_baseline_k_dc);
            bool adaptive_tune_enabled = zones_config_get_adaptive_tune_enabled(zi);
            backup_stream_printf(&s, "\"ease_off_window_mult\":%.9g,\"approach_rate_cap_c_per_hr\":%.9g,",
                                (double)ease_off_window_mult, (double)approach_rate_cap_c_per_hr);
            backup_stream_printf(&s, "\"error_band_c\":%.9g,\"rate_band_c_per_s\":%.9g,\"relay_type\":%u,",
                                (double)error_band_c, (double)rate_band_c_per_s, (unsigned)relay_type);
            backup_stream_printf(&s, "\"progress_band_c\":%.9g,\"zone_type\":%u,",
                                (double)progress_band_c, (unsigned)zone_type);
            backup_stream_printf(&s, "\"model_fit_temp_c\":%.9g,\"model_fit_ambient_c\":%.9g,",
                                (double)model_fit_temp_c, (double)model_fit_ambient_c);
            /* 2026-09-16 backup-round-trip-gap closure, group 1/2/3: these
             * four also have public getter+setter pairs added this pass
             * (zones_config_accessors.h) but previously round-tripped
             * nowhere. failsafe_state/hyst_c/min_on_s/min_off_s are always
             * answerable once zi passed the pid_kp check, same as every
             * other field in this unconditional block. NOTE: min_on_s/
             * min_off_s are the on/off-zone plan's SECONDS fields
             * (zone_cfg_t::min_on_s/min_off_s) -- a completely different
             * pair from heater_min_on_ms/heater_min_off_ms (milliseconds,
             * zones_config_get_heater_cfg()) already emitted above, so
             * they get distinct key names to avoid any collision. */
            bool failsafe_state = false;
            zones_config_get_failsafe_state(zi, &failsafe_state);
            float hyst_c = 0.0f;
            zones_config_get_hyst_c(zi, &hyst_c);
            uint16_t min_on_s = 0, min_off_s = 0;
            zones_config_get_min_on_s(zi, &min_on_s);
            zones_config_get_min_off_s(zi, &min_off_s);
            uint8_t timing_profile_index = 0;
            zones_config_get_timing_profile_index(zi, &timing_profile_index);
            backup_stream_printf(&s, "\"failsafe_state\":%u,\"hyst_c\":%.9g,",
                                failsafe_state ? 1u : 0u, (double)hyst_c);
            backup_stream_printf(&s, "\"min_on_s\":%u,\"min_off_s\":%u,\"timing_profile\":%u,",
                                (unsigned)min_on_s, (unsigned)min_off_s, (unsigned)timing_profile_index);
            /* adaptive_tune_enabled is the last UNCONDITIONAL key of this
             * object -- no trailing comma here. The two blocks that follow
             * (tuning_*, normal_current_a) are each conditionally emitted,
             * so they each carry their own LEADING comma instead, keeping
             * the object valid JSON whether zero, one, or both fire. */
            /* Booleans emitted as 0/1, not JSON true/false: backup_json.h's
             * reader (backup_json_field_num/_opt_num) has no boolean
             * primitive, and adding one purely for these few fields is not
             * worth a new parser code path on this board's fixed 8KB httpd
             * stack budget -- 0/1 round-trips through the existing numeric
             * reader exactly. */
            backup_stream_printf(&s, "\"coil_power_w\":%.9g,\"autotune_baseline_k_dc\":%.9g,"
                                "\"adaptive_tune_enabled\":%u",
                                (double)coil_power_w, (double)autotune_baseline_k_dc,
                                adaptive_tune_enabled ? 1u : 0u);
            /* tuning_* provenance family (zone_tuning_quality_t) -- emitted
             * only when the underlying tuning has actually run (valid==true),
             * same "skip the key rather than emit a bogus zero" convention
             * as have_model above: this struct's own float fields (baseline_c
             * etc.) are meaningless before a run has ever completed, and
             * import must not overwrite a REAL tuning record on the target
             * board with a "never tuned" backup that never measured one.
             * tuning_seq itself is deliberately not part of this struct
             * (no accessor exposes it anywhere in the firmware) and so
             * cannot be carried here -- see the backup round-trip report. */
            zone_tuning_quality_t tq;
            memset(&tq, 0, sizeof(tq));
            bool have_tuning = zones_config_get_tuning_quality(zi, &tq) && tq.valid;
            if (have_tuning) {
                backup_stream_printf(&s, ",\"tuning_valid\":1,\"tuning_method\":%u,\"tuning_rule\":%u,",
                                    (unsigned)tq.method, (unsigned)tq.rule);
                backup_stream_printf(&s, "\"tuning_settled\":%u,\"tuning_extrapolation_converged\":%u,"
                                    "\"tuning_tau_consistent\":%u,",
                                    tq.settled ? 1u : 0u,
                                    tq.extrapolation_converged ? 1u : 0u,
                                    tq.tau_consistent ? 1u : 0u);
                backup_stream_printf(&s, "\"tuning_baseline_c\":" BACKUP_TUNING_FLOAT_FMT ",\"tuning_step_ambient_c\":" BACKUP_TUNING_FLOAT_FMT ",",
                                    (double)tq.baseline_c, (double)tq.step_ambient_c);
                backup_stream_printf(&s, "\"tuning_raw_rise_c\":" BACKUP_TUNING_FLOAT_FMT ",\"tuning_rise_inf_c\":" BACKUP_TUNING_FLOAT_FMT,
                                    (double)tq.raw_rise_c, (double)tq.rise_inf_c);
            }
            /* CT normals -- the owner's own literal example ("ct normals
             * ... backup"). This is a SEPARATE NVS store (zone_normals_cfg_t,
             * its own version/CRC, NVS key zone_norm_cfg) from zone_cfg_t,
             * indexed the same way (zi = MAX31856 channel = zone index), so
             * it is folded into this same zone object rather than a new
             * top-level array -- one JSON entry per zone either way, and
             * this avoids a second index space to keep in sync. Emitted only
             * when actually measured (has_normal_current), same "skip
             * unmeasured rather than emit a false 0.0" convention as
             * model_k_dc/tuning above -- an unmeasured CT normal is not the
             * same fact as "measured at 0 A". */
            float normal_current_a = 0.0f;
            bool normal_current_measured = false;
            if (zones_config_get_normal_current(zi, &normal_current_a, &normal_current_measured) &&
                normal_current_measured) {
                backup_stream_printf(&s, ",\"normal_current_a\":%.9g", (double)normal_current_a);
            }
            /* 2026-09-16 config-backup round-trip gap closure: the Pico's OWN
             * i_normal_a[zi] (0x031A-0x031C) -- the actual S14/S15 arming
             * baseline those two guards read on the safety processor. This is
             * a SEPARATE store from normal_current_a just above (that is the
             * ESP-side zones_config_get_normal_current() record fixed in
             * 163b5842); losing this one forces a full CT-normal recalibration
             * (a real current sweep) after every restore even though the ESP
             * half round-trips fine. Emitted only when the Pico has actually
             * reported a value for this channel (zones_get_safety_pico_i_
             * normal_a() returns false for "never fetched/measured") -- same
             * "skip unmeasured rather than emit a false 0.0" convention as
             * normal_current_a and model_k_dc above. */
            float safety_i_normal_a = 0.0f;
            if (zones_get_safety_pico_i_normal_a(zi, &safety_i_normal_a)) {
                backup_stream_printf(&s, ",\"safety_i_normal_a\":%.9g", (double)safety_i_normal_a);
            }
        }
        /* settings_source_g%u above is now the last key of this object (it
         * was settings_source before the per-group keys were added) and is
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

    /* 2026-09-16 backup-round-trip-gap closure, group 3: the named
     * timing_profiles[] bundle itself (as opposed to each zone's
     * "timing_profile" index into it, emitted above). One top-level array,
     * indexed 0..timing_profile_count-1 -- zones_config_get_timing_profile_raw()
     * is the scalar-out-param accessor added this pass (zone_timing_profile_t
     * cannot be named in zones_config_accessors.h -- circular include with
     * zones_config_json.h -- so this mirrors zones_config_get_guard_thresholds()'s
     * existing scalar-out-param shape rather than exposing the struct).
     * tuning_seq is DELIBERATELY NOT exported here or anywhere: no accessor
     * anywhere in the firmware exposes it (confirmed by grep before this
     * pass), so there is nothing to read it back from; adding one purely for
     * this backup would be new surface with no other caller, on a value
     * whose only use is de-duplicating already-applied autotune runs. */
    {
        uint8_t tp_count = zones_config_get_timing_profile_count();
        backup_stream_printf(&s, ",\"timing_profiles\":[");
        for (uint8_t p = 0; p < tp_count; p++) {
            char tp_name[TIMING_PROFILE_NAME_MAX_LEN + 1];
            float duty_min, window_s, drift_c, frozen_eps_c, cross_zone_s, bangbang_c, cool_margin_c,
                cool_hold_s, ramp_lock_c;
            if (!zones_config_get_timing_profile_raw(p, tp_name, sizeof(tp_name), &duty_min, &window_s,
                                                     &drift_c, &frozen_eps_c, &cross_zone_s, &bangbang_c,
                                                     &cool_margin_c, &cool_hold_s, &ramp_lock_c)) {
                continue;
            }
            char tp_name_escaped[TIMING_PROFILE_NAME_MAX_LEN * 2 + 1];
            json_escape(tp_name, tp_name_escaped, sizeof(tp_name_escaped));
            backup_stream_printf(&s, "%s{\"name\":\"%s\",\"progress_duty_min\":%.9g,\"progress_window_s\":%.9g,",
                                p == 0 ? "" : ",", tp_name_escaped, (double)duty_min, (double)window_s);
            backup_stream_printf(&s, "\"drift_hysteresis_c\":%.9g,\"frozen_eps_c\":%.9g,"
                                "\"cross_zone_period_s\":%.9g,",
                                (double)drift_c, (double)frozen_eps_c, (double)cross_zone_s);
            backup_stream_printf(&s, "\"bangbang_hysteresis_c\":%.9g,\"cooling_limited_margin_c\":%.9g,"
                                "\"cooling_limited_hold_s\":%.9g,\"ramp_lock_band_c\":%.9g}",
                                (double)bangbang_c, (double)cool_margin_c, (double)cool_hold_s,
                                (double)ramp_lock_c);
        }
        backup_stream_printf(&s, "]");
    }

    /* 2026-09-16 backup-round-trip-gap closure, group 4: ct_map_zone[]/
     * k_ct_v_per_a[] -- EXPORT-ONLY, deliberately not restored on import.
     * Public getters already existed before this pass
     * (zones_ct_channel_map_derived()/zones_ct_k_v_per_a_derived(), both in
     * zones_config_accessors.h) -- the "no getter" premise for this group
     * was false. What is genuinely missing is a SAFE way to restore them:
     * each is a mapping from a physical CT clamp channel to a zone, or that
     * channel's derived volts-per-amp scale, both derived by the
     * current-sweep commissioning flow against THIS board's actual CT
     * wiring. Restoring a backup taken on one board's wiring onto a
     * different board (or the same board after a CT clamp was moved to a
     * different channel) would silently mislabel which physical channel
     * feeds which zone -- the commissioning page would show it as
     * "derived"/trustworthy when it no longer matches the wiring in front
     * of the operator. kiln_cfg_store.c's foreign-package pattern
     * (source_board_id set on export, force-cleared on import when absent
     * or mismatched) is the precedent for gating this kind of restore, but
     * the JSON backup format has no board-identity field to reuse that
     * pattern with, and adding one is a larger change than this pass's
     * scope. The safe middle ground taken here: export it as read-only,
     * informational context (useful for a human diffing two backups, or
     * confirming what a board's wiring WAS at export time), and
     * backup_import_apply() never calls zone_ct_map_set()/zone_k_ct_set()
     * at all -- an operator who needs to restore CT wiring re-runs the
     * current-sweep commissioning flow on the actual hardware instead. */
    {
        uint8_t ct_map_mask = 0, k_ct_mask = 0;
        uint8_t ct_map_zone[ZONE_CT_CHANNEL_COUNT];
        float k_ct_v_per_a[ZONE_CT_CHANNEL_COUNT];
        memset(ct_map_zone, 0, sizeof(ct_map_zone));
        memset(k_ct_v_per_a, 0, sizeof(k_ct_v_per_a));
        zones_ct_channel_map_derived(&ct_map_mask, ct_map_zone);
        zones_ct_k_v_per_a_derived(&k_ct_mask, k_ct_v_per_a);
        backup_stream_printf(&s, ",\"ct_map_informational_only\":[");
        bool first_ct = true;
        for (uint8_t ch = 0; ch < ZONE_CT_CHANNEL_COUNT; ch++) {
            if (!(ct_map_mask & (1u << ch))) {
                continue;
            }
            backup_stream_printf(&s, "%s{\"ct_channel\":%u,\"zone\":%u}", first_ct ? "" : ",", ch,
                                ct_map_zone[ch]);
            first_ct = false;
        }
        backup_stream_printf(&s, "],\"k_ct_v_per_a_informational_only\":[");
        bool first_k = true;
        for (uint8_t ch = 0; ch < ZONE_CT_CHANNEL_COUNT; ch++) {
            if (!(k_ct_mask & (1u << ch))) {
                continue;
            }
            backup_stream_printf(&s, "%s{\"ct_channel\":%u,\"k_v_per_a\":%.9g}", first_k ? "" : ",", ch,
                                (double)k_ct_v_per_a[ch]);
            first_k = false;
        }
        backup_stream_printf(&s, "]");
    }

    /* 2026-09-15 (Opus adversarial re-review, F6): this used to read
     * zones_config_get_safety_tc_type() -- the ESP's own cached copy, which
     * N5 already found and fixed on the GET-page path (zones_http_get.c),
     * but this export path was missed. The commissioning page owns
     * tc_type; the Pico is authoritative. A backup taken before the first
     * Pico read-back (or after a Pico-side change the ESP hasn't yet
     * polled) would otherwise capture a stale value. Same live read-back
     * as zones_http_get.c, with the same "0 only when known" convention --
     * import never writes this field back to the Pico either way, so the
     * only consequence of getting it wrong is a misleading backup file, not
     * a wrong device, but there is no reason to keep shipping the stale
     * one when the live getter already exists. */
    uint8_t safety_tc_type = 0;
    bool safety_tc_type_known = zones_get_safety_pico_tc_type(&safety_tc_type);
    if (!safety_tc_type_known) {
        safety_tc_type = 0;
    }
    backup_stream_printf(&s, ",\"safety_tc_type\":%u", safety_tc_type);

    /* docs/KILN_PROFILES_PLAN.md item 17 follow-up: every SAVED kiln config
     * slot (not just the live/active one, which is already fully covered by
     * every field above). Each populated (pico_populated) slot embeds its
     * full section-5.1 package envelope verbatim -- kiln_cfg_store_export_
     * package_json()'s output is already valid, complete JSON, so it is
     * spliced in raw (backup_stream_raw()) rather than re-escaped/re-parsed.
     * A slot saved by pre-v3 firmware and never re-saved since (pico_
     * populated == 0) cannot be captured as a complete package -- same
     * refusal kiln_cfg_store_export_package_json() itself gives for that
     * case -- so it is listed by id/name/active only, with "omitted" naming
     * why, rather than silently dropped: a restore reading this array still
     * learns the slot existed, even though it cannot recreate its content
     * (the operator's only recourse is what it always was -- re-save that
     * slot on the original board first, which populates its Pico half). */
    {
        kiln_cfg_summary_t rows[KILN_CFG_MAX_COUNT];
        uint8_t n = kiln_cfg_store_list(rows, KILN_CFG_MAX_COUNT);
        backup_stream_printf(&s, ",\"kiln_configs\":[");
        for (uint8_t i = 0; i < n; i++) {
            char name_escaped[KILN_CFG_NAME_MAX_LEN * 2 + 1];
            json_escape(rows[i].name, name_escaped, sizeof(name_escaped));
            bool pico_populated = false;
            kiln_cfg_store_get_package_identity(rows[i].id, &pico_populated, NULL, NULL);
            backup_stream_printf(&s, "%s{\"id\":%d,\"name\":\"%s\",\"is_active\":%s", i == 0 ? "" : ",",
                                (int)rows[i].id, name_escaped, rows[i].is_active ? "true" : "false");
            if (pico_populated) {
                size_t pkg_len = 0;
                char reason[96];
                if (kiln_cfg_store_export_package_json(rows[i].id, pkg_json_scratch, KILN_CFG_EXPORT_JSON_MAX_LEN,
                                                       &pkg_len, reason, sizeof(reason))) {
                    backup_stream_printf(&s, ",\"package\":");
                    backup_stream_raw(&s, pkg_json_scratch, pkg_len);
                } else {
                    /* Should not happen (pico_populated was just true), but
                     * fail visibly in the document rather than silently
                     * emitting a package-less entry with no explanation. */
                    char reason_escaped[192];
                    json_escape(reason, reason_escaped, sizeof(reason_escaped));
                    backup_stream_printf(&s, ",\"omitted\":\"export_failed: %s\"", reason_escaped);
                }
            } else {
                backup_stream_printf(&s, ",\"omitted\":\"no_pico_half\"");
            }
            backup_stream_printf(&s, "}");
        }
        backup_stream_printf(&s, "]");
    }
    /* docs/GITHUB_RELEASE_UPDATE_PLAN.md WP9: the persisted update repo
     * ("owner/name"). Re-validated on import. "" when the compiled-in default is
     * in use, so a backup taken on a stock board never pins a restore onto
     * today's default if the default later changes, and a restore onto a stock
     * board writes nothing. No BACKUP_FORMAT_VERSION bump: an absent key is a
     * no-op on import, the same optional-key rule as safety_tc_type. */
    {
        char repo_escaped[UPDATE_SETTINGS_REPO_MAX_LEN * 2 + 1];
        repo_escaped[0] = '\0';
        if (!update_settings_repo_is_default()) {
            json_escape(update_settings_repo(), repo_escaped, sizeof(repo_escaped));
        }
        backup_stream_printf(&s, ",\"update_repo\":\"%s\"", repo_escaped);
    }
    backup_export_aux_outputs(&s);
    backup_export_relay_cycles(&s);
    backup_stream_printf(&s, "}");

    backup_stream_flush(&s);
    free(buf);
    free(ps);
    free(zs);
    free(pkg_json_scratch);
    if (s.err == ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0); /* terminates the chunked response */
    }
    return ESP_OK;
}
