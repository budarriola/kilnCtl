#include "backup_http.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "MAX31856.h"
#include "ota_http.h" /* ota_http_check_interlocks() -- see this file's header comment */
#include "profiles_http.h"
#include "web_encoding.h"
#include "wifi_provision_http.h"
#include "zones_http.h"

static const char *TAG = "backup_http";

/* Bump if the exported JSON shape ever changes in a way older firmware
 * cannot read back -- see backup_import_apply()'s version check.
 *
 * 1 -> 2 (2026-08-21): the four fields zones_http.h had a get+set pair for
 * (PID gains, plant model, tc_type, safety_tc_type) were the whole story at
 * version 1; this pass added setters for everything else a zone stores
 * (name, relay/thermo wiring, cal_offset_c, max_ramp/sanity_rate, control_mode,
 * temp limits, heater timing, the 8 guard-threshold overrides, and
 * cross_zone_max_delta_c -- see each new field's own comment in the "zone
 * tuning entry" pass-1 loop below), so the export/import now covers all of
 * it. Unlike zones_http.c's ZONES_CFG_VERSION there is still no migration
 * chain: a version 2 body is a strict SUPERSET of what a version 1 body
 * could contain (every new key is OPTIONAL per zone entry, exactly like
 * model_k_dc/tc_type already were at version 1), so a genuine version 1
 * export -- which simply never has any of the new keys -- imports cleanly
 * under version 2's reader with no format-specific branch needed; only the
 * NEW fields are left unset (not written) on such an import, which is
 * correct: a version 1 export never claimed to carry them, so restoring one
 * must not silently zero what the target board already has for those
 * fields. A version 3+ body (this firmware does not understand it) is still
 * refused outright, matching the "reject rather than guess" brief. */
#define BACKUP_FORMAT_VERSION 2
#define BACKUP_FORMAT_VERSION_MIN 1

/* Generous headroom over a legitimate full backup (8 profiles x up to 12
 * segments, plus MAX31856_CHANNEL_COUNT zones' worth of PID/model/tc_type
 * plus, as of version 2, every other per-zone field -- name/relay_mask/
 * thermo_mask/cal_offset_c/max_ramp/sanity_rate/control_mode/temp limits/
 * heater timing/8 guard thresholds/cross_zone_max_delta_c, roughly another
 * ~20 keys per zone) -- checked against Content-Length before a single byte
 * is read, same discipline as every other untrusted-body handler in this
 * codebase. 12288 -> 16384 (2026-08-21) for the version-2 field growth.
 * Heap-allocated, not a stack array: this board's internal DRAM was measured
 * at ~4167 bytes free after LVGL start (see this file's header comment / the
 * task brief), and a 12 KB local array would be exactly the kind of stack
 * overflow KilnFW's 2026-08-21 boot-loop fix (commit aee6171) had to dig out
 * of a different file. */
#define BACKUP_BODY_MAX 16384

/* Embedded via EMBED_TXTFILES, pre-gzipped at configure time by
 * App/drivers/CMakeLists.txt -- same convention as every other *_page.html
 * in this component. */
extern const uint8_t backup_page_html_gz_start[] asm("_binary_backup_page_html_gz_start");
extern const uint8_t backup_page_html_gz_end[] asm("_binary_backup_page_html_gz_end");

static esp_err_t backup_page_get_handler(httpd_req_t *req)
{
    if (!web_client_accepts_gzip(req)) {
        return web_send_gzip_not_acceptable(req, TAG, "backup_page.html");
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

static esp_err_t backup_export_get_handler(httpd_req_t *req)
{
    char *buf = malloc(BACKUP_STREAM_BUF);
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
                                "\"cross_zone_max_delta_c\":%.1f",
                                (double)sensor_fault_debounce_ticks, (double)frozen_window_s,
                                (double)cross_zone_max_delta_c);
        }
        /* cross_zone_max_delta_c above is the last key of this object and is
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

/* ---- Minimal JSON reader, import side -------------------------------------
 *
 * There is no cJSON (or any other JSON library) anywhere in this codebase --
 * every existing GET handler hand-builds JSON with snprintf, and every
 * existing POST handler parses application/x-www-form-urlencoded via
 * http_form.h. This import is the one place a POST body is JSON rather than
 * form-encoded (the export above is JSON so an operator can inspect/diff it
 * in a text editor, and round-tripping the same shape back in is simpler and
 * less error-prone than inventing a second, form-encoded backup format).
 * Rather than take on a general-purpose JSON library dependency for one
 * upload endpoint, this is a small, purpose-built reader for EXACTLY this
 * file's own fixed schema -- it does not aim to parse arbitrary JSON
 * correctly (no unicode escapes, no scientific-notation edge cases beyond
 * what strtod already handles, no duplicate-key-wins-last semantics beyond
 * "first match found wins"). Deliberately iterative, not recursive, when
 * skipping nested {..}/[..] (json_skip_value() below uses a depth counter,
 * not a call stack) -- a malicious deeply-nested body costs more loop
 * iterations, never more stack, which matters on a board that just fixed a
 * real stack-overflow boot loop (commit aee6171). */

static const char *json_skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    return p;
}

/* Advances past one JSON value (string/number/object/array/true/false/null)
 * starting at *p (leading whitespace tolerated), returning a pointer just
 * past it. Malformed input still advances (at least one byte) so a caller
 * scanning for the next field/element cannot spin forever on garbage. */
static const char *json_skip_value(const char *p)
{
    p = json_skip_ws(p);
    if (*p == '\0') {
        return p;
    }
    if (*p == '"') {
        p++;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) {
                p++;
            }
            p++;
        }
        if (*p == '"') {
            p++;
        }
        return p;
    }
    if (*p == '{' || *p == '[') {
        char open = *p, close = (open == '{') ? '}' : ']';
        int depth = 1;
        p++;
        while (*p && depth > 0) {
            if (*p == '"') {
                p++;
                while (*p && *p != '"') {
                    if (*p == '\\' && p[1]) {
                        p++;
                    }
                    p++;
                }
                if (*p == '"') {
                    p++;
                }
                continue;
            }
            if (*p == open) {
                depth++;
            } else if (*p == close) {
                depth--;
            }
            p++;
        }
        return p;
    }
    /* number / true / false / null -- scan to the next structural delimiter */
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
        p++;
    }
    return p;
}

/* obj must point at (or before, with only whitespace between) a '{'. Returns
 * a pointer to the start of key's value if found at this object's top level
 * (does not descend into nested objects/arrays looking for the same key
 * elsewhere), or NULL if the key is absent or obj is not a well-formed
 * object. */
static const char *json_obj_find(const char *obj, const char *key)
{
    const char *p = json_skip_ws(obj);
    if (*p != '{') {
        return NULL;
    }
    p++;
    size_t key_len = strlen(key);
    for (;;) {
        p = json_skip_ws(p);
        if (*p == '}' || *p == '\0') {
            return NULL;
        }
        if (*p != '"') {
            return NULL; /* malformed -- not a "key": pair here */
        }
        const char *kstart = p + 1;
        const char *kend = kstart;
        while (*kend && *kend != '"') {
            if (*kend == '\\' && kend[1]) {
                kend++;
            }
            kend++;
        }
        size_t klen = (size_t)(kend - kstart);
        p = (*kend == '"') ? kend + 1 : kend;
        p = json_skip_ws(p);
        if (*p != ':') {
            return NULL;
        }
        p++;
        p = json_skip_ws(p);
        const char *vstart = p;
        bool match = (klen == key_len && strncmp(kstart, key, klen) == 0);
        p = json_skip_value(p);
        if (match) {
            return vstart;
        }
        p = json_skip_ws(p);
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == '}') {
            return NULL;
        }
        return NULL; /* malformed */
    }
}

/* arr must point at (or before, with only whitespace between) a '['. Returns
 * a pointer to the first element's value, or NULL if the array is empty,
 * absent, or malformed. */
static const char *json_arr_first(const char *arr)
{
    if (!arr) {
        return NULL;
    }
    const char *p = json_skip_ws(arr);
    if (*p != '[') {
        return NULL;
    }
    p++;
    p = json_skip_ws(p);
    if (*p == ']') {
        return NULL;
    }
    return p;
}

/* elem points at one array element's value (as returned by json_arr_first()
 * or a previous call to this function). Returns a pointer to the next
 * element, or NULL once the array's closing ']' is reached. */
static const char *json_arr_next(const char *elem)
{
    const char *p = json_skip_value(elem);
    p = json_skip_ws(p);
    if (*p == ',') {
        p++;
        return json_skip_ws(p);
    }
    return NULL;
}

static bool json_field_num(const char *obj, const char *key, double *out)
{
    const char *v = json_obj_find(obj, key);
    if (!v) {
        return false;
    }
    char *end = NULL;
    double d = strtod(v, &end);
    if (end == v || !isfinite(d)) {
        return false;
    }
    *out = d;
    return true;
}

/* Parses an OPTIONAL bounded numeric field: absent is not an error (*out_has
 * is set false, *out untouched), present-but-out-of-range or malformed IS an
 * error. Used by backup_import_apply()'s zone-tuning pass 1b for every
 * version-2 field, all of which are optional the same way model_k_dc/tc_type
 * already were at version 1 -- a version-1 export simply never has these
 * keys, and this is what lets it still import cleanly under the version-2
 * reader (see BACKUP_FORMAT_VERSION's comment). */
static bool json_field_opt_num(const char *obj, const char *key, double min, double max, double *out,
                               bool *out_has, const char *field_desc, char *err_msg, size_t err_cap,
                               unsigned entry_idx)
{
    if (!json_obj_find(obj, key)) {
        *out_has = false;
        return true;
    }
    if (!json_field_num(obj, key, out) || *out < min || *out > max) {
        snprintf(err_msg, err_cap, "zone tuning entry %u: %s missing or out of range", entry_idx, field_desc);
        return false;
    }
    *out_has = true;
    return true;
}

static bool json_field_str(const char *obj, const char *key, char *out, size_t cap)
{
    const char *v = json_obj_find(obj, key);
    if (!v || *v != '"' || cap == 0) {
        return false;
    }
    v++;
    size_t o = 0;
    while (*v && *v != '"' && o + 1 < cap) {
        if (*v == '\\' && v[1]) {
            v++;
            char c = *v;
            out[o++] = (c == 'n') ? '\n' : (c == 't') ? '\t' : c;
            v++;
        } else {
            out[o++] = *v++;
        }
    }
    out[o] = '\0';
    return true;
}

/* ---- Import: validate everything, THEN apply ------------------------------
 *
 * Two full passes over `body`. Pass 1 (this function's first half) parses
 * every profile and every zone entry into local candidate arrays and
 * validates every one of them -- range bounds via profiles_http_get_bounds(),
 * the same zone_mask-must-select-a-configured-zone and
 * ramp-vs-zone-ceiling-feasibility rules profiles_http_save() itself enforces
 * (duplicated here deliberately, not called speculatively, so a failure on
 * profile 6 of 8 is caught before profile 0 is ever written) -- and refuses
 * the WHOLE import on the first problem found, writing nothing. Pass 2 (this
 * function's second half) runs only if pass 1 fully succeeded, and commits
 * every candidate via the same profiles_http_save()/zones_config_set_*()
 * calls the UI's own pages use.
 *
 * model_k_dc/model_tau_s/model_dead_time_s are validated in THIS pass against
 * the exact same ZONE_MODEL_K_MAX/ZONE_MODEL_TIME_MAX_S ceilings
 * zones_config_set_model() enforces at commit time (both now come from
 * zones_http.h, moved there 2026-08-21 for exactly this reason -- see that
 * header's comment) -- so an out-of-range plant model is refused here, before
 * any write happens, the same as every other field in this function.
 *
 * Version 2 (2026-08-21) added every other zone_cfg_t field zones_http.h
 * gained a setter for this same pass -- name/relay_mask/thermo_mask/
 * cal_offset_c/max_ramp_c_per_hr/sanity_rate_c_per_min/control_mode/temp
 * limits/heater timing/the 8 guard-threshold overrides/cross_zone_max_delta_c.
 * Every one is OPTIONAL per zone entry (same convention model_k_dc/tc_type
 * already used), validated in THIS pass against the exact bound the new
 * zones_config_set_*() setter enforces (json_field_opt_num()'s doc comment
 * has the shared shape; the three bundled groups -- temp limits, heater
 * timing, and the 8 guard thresholds -- are each all-or-nothing per entry,
 * matching their bundled setter). */
static bool backup_import_apply(const char *body, char *err_msg, size_t err_cap)
{
    double dver;
    char kind[24];
    if (!json_field_str(body, "kind", kind, sizeof(kind)) || strcmp(kind, "kilnctl_backup") != 0) {
        snprintf(err_msg, err_cap, "not a kilnCtl backup file (missing/wrong \"kind\")");
        return false;
    }
    if (!json_field_num(body, "version", &dver) || (int)dver < BACKUP_FORMAT_VERSION_MIN ||
        (int)dver > BACKUP_FORMAT_VERSION) {
        snprintf(err_msg, err_cap, "unsupported backup version (this firmware understands versions %d-%d)",
                BACKUP_FORMAT_VERSION_MIN, BACKUP_FORMAT_VERSION);
        return false;
    }

    float bound_target_min, bound_target_max, bound_ramp_min, bound_ramp_max;
    uint32_t bound_dwell_max;
    profiles_http_get_bounds(&bound_target_min, &bound_target_max, &bound_ramp_min, &bound_ramp_max,
                             &bound_dwell_max);
    uint8_t thermo_count = zones_config_get_thermo_count();
    uint8_t valid_zone_bits = thermo_count >= 8 ? 0xFFu : (uint8_t)((1u << thermo_count) - 1u);

    /* ---- Pass 1a: profiles ---- */
    typedef struct {
        bool has_id;
        uint8_t id;
        profile_t p;
    } profile_candidate_t;
    profile_candidate_t candidates[PROFILES_MAX_COUNT];
    size_t candidate_count = 0;

    const char *profiles_arr = json_obj_find(body, "profiles");
    for (const char *pe = json_arr_first(profiles_arr); pe; pe = json_arr_next(pe)) {
        if (candidate_count >= PROFILES_MAX_COUNT) {
            snprintf(err_msg, err_cap, "backup has more than %u profiles", (unsigned)PROFILES_MAX_COUNT);
            return false;
        }
        profile_candidate_t *c = &candidates[candidate_count];
        memset(c, 0, sizeof(*c));

        double did;
        c->has_id = json_field_num(pe, "id", &did);
        if (c->has_id) {
            if (did < 0 || did >= PROFILES_MAX_COUNT) {
                snprintf(err_msg, err_cap, "profile entry %u: id out of range (0-%u)",
                        (unsigned)candidate_count, (unsigned)(PROFILES_MAX_COUNT - 1));
                return false;
            }
            c->id = (uint8_t)did;
        }

        /* +2, not +1: json_field_str() silently truncates to cap-1 bytes with
         * no way to tell the caller it did so, so a buffer sized exactly
         * PROFILE_NAME_MAX_LEN+1 could never actually observe an overlong
         * name -- it would just come back pre-truncated to a fit, and any
         * "name too long" check below would be permanently unreachable (dead)
         * code. Same fix as the zone-name pass below (its buffer's comment
         * has the full walkthrough): sizing one byte larger than the real
         * limit means ANY name whose true length exceeds
         * PROFILE_NAME_MAX_LEN still results in strlen(name) ==
         * PROFILE_NAME_MAX_LEN+1 after the copy (truncated to fit this
         * buffer, but still detectably over the limit), so the length check
         * that follows can actually fire -- matching the "name too long"
         * rejection the interactive POST /api/profile path already gives
         * for the same input (parse_profile_fields(), via
         * http_form_find_field()'s -2 return). Without this, import
         * silently accepted what the interactive path refuses. */
        char name[PROFILE_NAME_MAX_LEN + 2];
        if (json_field_str(pe, "name", name, sizeof(name))) {
            if (strlen(name) > PROFILE_NAME_MAX_LEN) {
                snprintf(err_msg, err_cap, "profile entry %u: name too long", (unsigned)candidate_count);
                return false;
            }
            strncpy(c->p.name, name, PROFILE_NAME_MAX_LEN);
            c->p.name[PROFILE_NAME_MAX_LEN] = '\0';
        }

        double dmask;
        if (!json_field_num(pe, "zone_mask", &dmask) || dmask < 0 || dmask > 255) {
            snprintf(err_msg, err_cap, "profile entry %u: zone_mask missing or out of range", (unsigned)candidate_count);
            return false;
        }
        c->p.zone_mask = (uint8_t)dmask;
        if (c->p.zone_mask == 0 || (c->p.zone_mask & (uint8_t)~valid_zone_bits) != 0) {
            snprintf(err_msg, err_cap,
                    "profile entry %u: zone_mask must select at least one configured zone", (unsigned)candidate_count);
            return false;
        }

        const char *segs_arr = json_obj_find(pe, "segments");
        uint8_t seg_i = 0;
        for (const char *se = json_arr_first(segs_arr); se; se = json_arr_next(se)) {
            if (seg_i >= PROFILE_MAX_SEGMENTS) {
                snprintf(err_msg, err_cap, "profile entry %u: more than %u segments", (unsigned)candidate_count,
                        (unsigned)PROFILE_MAX_SEGMENTS);
                return false;
            }
            double dt, dr, dd;
            if (!json_field_num(se, "target_c", &dt) || dt < bound_target_min || dt > bound_target_max) {
                snprintf(err_msg, err_cap, "profile entry %u, segment %u: target_c missing or out of range",
                        (unsigned)candidate_count, (unsigned)(seg_i + 1));
                return false;
            }
            if (!json_field_num(se, "ramp_c_per_hr", &dr) || dr < bound_ramp_min || dr > bound_ramp_max) {
                snprintf(err_msg, err_cap, "profile entry %u, segment %u: ramp_c_per_hr missing or out of range",
                        (unsigned)candidate_count, (unsigned)(seg_i + 1));
                return false;
            }
            if (!json_field_num(se, "dwell_min", &dd) || dd < 0 || dd > bound_dwell_max) {
                snprintf(err_msg, err_cap, "profile entry %u, segment %u: dwell_min missing or out of range",
                        (unsigned)candidate_count, (unsigned)(seg_i + 1));
                return false;
            }
            c->p.segments[seg_i].target_c = (float)dt;
            c->p.segments[seg_i].ramp_c_per_hr = (float)dr;
            c->p.segments[seg_i].dwell_min = (uint32_t)dd;
            seg_i++;
        }
        if (seg_i == 0) {
            snprintf(err_msg, err_cap, "profile entry %u: no segments", (unsigned)candidate_count);
            return false;
        }
        c->p.segment_count = seg_i;

        /* Same feasibility rule profiles_http_save() enforces -- duplicated
         * here so it is caught in validation, before any profile in this
         * import has been written. */
        for (uint8_t i = 0; i < c->p.segment_count; i++) {
            float rate = c->p.segments[i].ramp_c_per_hr;
            if (rate <= 0.0f) {
                continue;
            }
            for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
                if (!(c->p.zone_mask & (1u << zi))) {
                    continue;
                }
                float ceiling = 0.0f;
                zones_config_get_max_ramp(zi, &ceiling);
                if (rate > ceiling) {
                    snprintf(err_msg, err_cap,
                            "profile entry %u, segment %u: ramp rate %.1f C/hr exceeds zone %u's %.1f C/hr ceiling",
                            (unsigned)candidate_count, (unsigned)(i + 1), (double)rate, zi, (double)ceiling);
                    return false;
                }
            }
        }

        candidate_count++;
    }

    /* ---- Pass 1b: zone tuning ---- */
    typedef struct {
        uint8_t index;
        float kp, ki, kd;
        bool has_model;
        float k_dc, tau_s, dead_time_s;
        bool has_tc;
        uint8_t tc_type;
        /* Version 2 (2026-08-21): everything else zones_http.h gained a
         * setter for this pass. Each has its own has_* flag, same
         * optional-per-field convention as has_model/has_tc above -- see
         * json_field_opt_num()'s comment for why "absent" must not be an
         * error. */
        bool has_name;
        /* +2, not +1: json_field_str() silently truncates to cap-1 bytes with
         * no way to tell the caller it did so, so a buffer sized exactly
         * ZONE_NAME_MAX_LEN+1 could never actually observe an overlong name
         * -- it would just come back pre-truncated to a fit, and the "name
         * too long" check below would be permanently unreachable (dead)
         * code. Sizing one byte larger than the real limit means ANY name
         * whose true length exceeds ZONE_NAME_MAX_LEN still results in
         * strlen(name) == ZONE_NAME_MAX_LEN+1 after the copy (truncated to
         * fit this buffer, but still detectably over the limit), so the
         * length check that follows can actually fire. See this pass's
         * report for the negative test that proves it does. */
        char name[ZONE_NAME_MAX_LEN + 2];
        bool has_relay_mask;
        uint8_t relay_mask;
        bool has_thermo_mask;
        uint8_t thermo_mask;
        bool has_ct_mask;
        uint8_t ct_mask;
        bool has_cal;
        float cal_offset_c;
        bool has_ramp;
        float max_ramp_c_per_hr;
        bool has_sanity;
        float sanity_rate_c_per_min;
        bool has_mode;
        uint8_t control_mode;
        /* max_temp_c/min_temp_c are a bundled pair (zones_config_set_temp_limits()
         * takes both together) -- either both are present in the import or
         * neither is, same "all-or-nothing" rule TODO already applies to
         * model_k_dc/tau_s/dead_time_s just above. */
        bool has_temp_limits;
        float max_temp_c, min_temp_c;
        /* heater_window_ms/min_on_ms/min_off_ms -- same bundled-pair rule. */
        bool has_heater_cfg;
        float heater_window_ms, heater_min_on_ms, heater_min_off_ms;
        /* The 8 guard-threshold overrides -- same bundled-pair rule, all 8
         * or none (zones_config_set_guard_thresholds() takes all 8
         * together). */
        bool has_guard;
        float guard_wrong_dir_window_s, guard_wrong_dir_rate_c_per_min, guard_off_settle_s,
            guard_runaway_rate_c_per_min, guard_runaway_margin_c, guard_drift_period_s,
            guard_sensor_fault_debounce_ticks, guard_frozen_window_s;
        bool has_cross_zone;
        float cross_zone_max_delta_c;
    } zone_candidate_t;
    zone_candidate_t zone_candidates[MAX31856_CHANNEL_COUNT];
    size_t zone_candidate_count = 0;

    const char *zones_arr = json_obj_find(body, "zones");
    for (const char *ze = json_arr_first(zones_arr); ze; ze = json_arr_next(ze)) {
        if (zone_candidate_count >= MAX31856_CHANNEL_COUNT) {
            snprintf(err_msg, err_cap, "backup has more than %u zone tuning entries",
                    (unsigned)MAX31856_CHANNEL_COUNT);
            return false;
        }
        zone_candidate_t *zc = &zone_candidates[zone_candidate_count];
        memset(zc, 0, sizeof(*zc));

        double didx;
        if (!json_field_num(ze, "index", &didx) || didx < 0 || didx >= MAX31856_CHANNEL_COUNT) {
            snprintf(err_msg, err_cap, "zone tuning entry %u: index missing or out of range",
                    (unsigned)zone_candidate_count);
            return false;
        }
        zc->index = (uint8_t)didx;
        if (zc->index >= thermo_count) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u: channel %u is not a configured zone on this board (Thermocouples & "
                    "Zones settings)",
                    (unsigned)zone_candidate_count, zc->index);
            return false;
        }

        double dkp, dki, dkd;
        if (!json_field_num(ze, "pid_kp", &dkp) || !json_field_num(ze, "pid_ki", &dki) ||
            !json_field_num(ze, "pid_kd", &dkd) || dkp < 0 || dki < 0 || dkd < 0) {
            snprintf(err_msg, err_cap, "zone tuning entry %u: pid_kp/pid_ki/pid_kd missing, negative, or malformed",
                    (unsigned)zone_candidate_count);
            return false;
        }
        zc->kp = (float)dkp;
        zc->ki = (float)dki;
        zc->kd = (float)dkd;

        double dk, dtau, ddead;
        bool has_k = json_field_num(ze, "model_k_dc", &dk);
        bool has_tau = json_field_num(ze, "model_tau_s", &dtau);
        bool has_dead = json_field_num(ze, "model_dead_time_s", &ddead);
        if (has_k || has_tau || has_dead) {
            if (!(has_k && has_tau && has_dead) || dk < 0 || dtau < 0 || ddead < 0) {
                snprintf(err_msg, err_cap,
                        "zone tuning entry %u: model_k_dc/model_tau_s/model_dead_time_s must all be present "
                        "together and non-negative",
                        (unsigned)zone_candidate_count);
                return false;
            }
            /* Same ceilings zones_config_set_model() enforces at commit time
             * (ZONE_MODEL_K_MAX/ZONE_MODEL_TIME_MAX_S, now exposed by
             * zones_http.h) -- checked here, in pass 1, so an out-of-range
             * model is rejected before any earlier candidate in this same
             * import has been written. */
            if (dk > ZONE_MODEL_K_MAX || dtau > ZONE_MODEL_TIME_MAX_S || ddead > ZONE_MODEL_TIME_MAX_S) {
                snprintf(err_msg, err_cap,
                        "zone tuning entry %u: model_k_dc/model_tau_s/model_dead_time_s exceeds this firmware's "
                        "sanity bounds (K<=%.0f, tau/dead_time<=%.0fs)",
                        (unsigned)zone_candidate_count, (double)ZONE_MODEL_K_MAX, (double)ZONE_MODEL_TIME_MAX_S);
                return false;
            }
            zc->has_model = true;
            zc->k_dc = (float)dk;
            zc->tau_s = (float)dtau;
            zc->dead_time_s = (float)ddead;
        }

        double dtc;
        if (json_field_num(ze, "tc_type", &dtc)) {
            if (dtc < 0 || dtc > 7) {
                snprintf(err_msg, err_cap, "zone tuning entry %u: tc_type out of range (0-7)",
                        (unsigned)zone_candidate_count);
                return false;
            }
            zc->has_tc = true;
            zc->tc_type = (uint8_t)dtc;
        }

        /* ---- Version 2 fields -- see zone_candidate_t's comment ---- */
        if (json_field_str(ze, "name", zc->name, sizeof(zc->name))) {
            /* Same rejection zones_config_set_name()/parse_zone_fields()'s
             * z%u_name produce for a name over ZONE_NAME_MAX_LEN chars --
             * see zc->name's own comment for why the buffer is sized one
             * byte over the limit, which is what makes this reachable. */
            if (strlen(zc->name) > ZONE_NAME_MAX_LEN) {
                snprintf(err_msg, err_cap, "zone tuning entry %u: name too long", (unsigned)zone_candidate_count);
                return false;
            }
            zc->has_name = true;
        }

        double drelay;
        if (json_field_opt_num(ze, "relay_mask", 0, 255, &drelay, &zc->has_relay_mask, "relay_mask", err_msg,
                               err_cap, (unsigned)zone_candidate_count) == false) {
            return false;
        }
        if (zc->has_relay_mask) {
            zc->relay_mask = (uint8_t)drelay;
            /* Same bound zones_config_set_relay_mask()/parse_zone_fields()'s
             * z%u_relay_mask enforce: only relays 1..relay_count on THIS
             * board may be referenced. relay_count is a board-wide setting
             * this module never writes, so it is read straight off the live
             * config via zones_config_get_relay_count() (added this pass for
             * exactly this check), same as thermo_count already was for the
             * zone_mask/thermo_mask checks elsewhere in this function. */
            uint8_t relay_count = zones_config_get_relay_count();
            uint8_t valid_relay_bits = relay_count >= 8 ? 0xFFu : (uint8_t)((1u << relay_count) - 1u);
            if ((zc->relay_mask & ~valid_relay_bits) != 0) {
                snprintf(err_msg, err_cap,
                        "zone tuning entry %u: relay_mask references an unconfigured relay",
                        (unsigned)zone_candidate_count);
                return false;
            }
        }

        double dthermo;
        if (json_field_opt_num(ze, "thermo_mask", 0, 255, &dthermo, &zc->has_thermo_mask, "thermo_mask", err_msg,
                               err_cap, (unsigned)zone_candidate_count) == false) {
            return false;
        }
        if (zc->has_thermo_mask) {
            zc->thermo_mask = (uint8_t)dthermo;
            /* Same bound zones_config_set_thermo_mask()/parse_zone_fields()'s
             * z%u_thermo_mask enforce -- valid_zone_bits was already
             * computed above from thermo_count for the profile zone_mask
             * check, and is the identical bound here (both are "which
             * MAX31856 channels are configured on this board"). */
            if ((zc->thermo_mask & ~valid_zone_bits) != 0) {
                snprintf(err_msg, err_cap,
                        "zone tuning entry %u: thermo_mask references an unconfigured thermocouple channel",
                        (unsigned)zone_candidate_count);
                return false;
            }
        }

        double dct;
        if (json_field_opt_num(ze, "ct_mask", 0, 255, &dct, &zc->has_ct_mask, "ct_mask", err_msg,
                               err_cap, (unsigned)zone_candidate_count) == false) {
            return false;
        }
        if (zc->has_ct_mask) {
            zc->ct_mask = (uint8_t)dct;
            /* Fixed hardware count, unlike thermo_mask/relay_mask above --
             * see zones_http.c's ZONE_CT_CHANNEL_COUNT. */
            uint8_t valid_ct_bits = (uint8_t)((1u << ZONE_CT_CHANNEL_COUNT) - 1u);
            if ((zc->ct_mask & ~valid_ct_bits) != 0) {
                snprintf(err_msg, err_cap,
                        "zone tuning entry %u: ct_mask references an unconfigured current-sense channel",
                        (unsigned)zone_candidate_count);
                return false;
            }
        }

        double dcal;
        if (json_field_opt_num(ze, "cal_offset_c", (double)ZONE_CAL_OFFSET_MIN_C, (double)ZONE_CAL_OFFSET_MAX_C,
                               &dcal, &zc->has_cal, "cal_offset_c", err_msg, err_cap,
                               (unsigned)zone_candidate_count) == false) {
            return false;
        }
        if (zc->has_cal) {
            zc->cal_offset_c = (float)dcal;
        }

        double dramp;
        if (json_field_opt_num(ze, "max_ramp_c_per_hr", 0, (double)ZONE_MAX_RAMP_C_PER_HR_MAX, &dramp,
                               &zc->has_ramp, "max_ramp_c_per_hr", err_msg, err_cap,
                               (unsigned)zone_candidate_count) == false) {
            return false;
        }
        if (zc->has_ramp) {
            zc->max_ramp_c_per_hr = (float)dramp;
        }

        double dsanity;
        if (json_field_opt_num(ze, "sanity_rate_c_per_min", 0, (double)ZONE_SANITY_RATE_MAX_C_PER_MIN, &dsanity,
                               &zc->has_sanity, "sanity_rate_c_per_min", err_msg, err_cap,
                               (unsigned)zone_candidate_count) == false) {
            return false;
        }
        if (zc->has_sanity) {
            zc->sanity_rate_c_per_min = (float)dsanity;
        }

        double dmode;
        if (json_field_opt_num(ze, "control_mode", 0, (double)ZONE_CONTROL_MODE_PID, &dmode, &zc->has_mode,
                               "control_mode", err_msg, err_cap, (unsigned)zone_candidate_count) == false) {
            return false;
        }
        if (zc->has_mode) {
            zc->control_mode = (uint8_t)dmode;
        }

        double dmaxt, dmint;
        bool has_maxt = json_field_num(ze, "max_temp_c", &dmaxt);
        bool has_mint = json_field_num(ze, "min_temp_c", &dmint);
        if (has_maxt || has_mint) {
            if (!(has_maxt && has_mint)) {
                snprintf(err_msg, err_cap,
                        "zone tuning entry %u: max_temp_c/min_temp_c must both be present together",
                        (unsigned)zone_candidate_count);
                return false;
            }
            if (dmaxt < 0 || dmaxt > (double)ZONE_MAX_TEMP_C_MAX || dmint < (double)ZONE_MIN_TEMP_C_MIN ||
                dmint > (double)ZONE_MIN_TEMP_C_MAX) {
                snprintf(err_msg, err_cap, "zone tuning entry %u: max_temp_c/min_temp_c out of range",
                        (unsigned)zone_candidate_count);
                return false;
            }
            zc->has_temp_limits = true;
            zc->max_temp_c = (float)dmaxt;
            zc->min_temp_c = (float)dmint;
        }

        double dwin, don, doff;
        bool has_win = json_field_num(ze, "heater_window_ms", &dwin);
        bool has_on = json_field_num(ze, "heater_min_on_ms", &don);
        bool has_off = json_field_num(ze, "heater_min_off_ms", &doff);
        if (has_win || has_on || has_off) {
            if (!(has_win && has_on && has_off)) {
                snprintf(err_msg, err_cap,
                        "zone tuning entry %u: heater_window_ms/heater_min_on_ms/heater_min_off_ms must all be "
                        "present together",
                        (unsigned)zone_candidate_count);
                return false;
            }
            if (dwin < 0 || dwin > (double)ZONE_HEATER_WINDOW_MS_MAX || don < 0 ||
                don > (double)ZONE_HEATER_MIN_ON_OFF_MS_MAX || doff < 0 ||
                doff > (double)ZONE_HEATER_MIN_ON_OFF_MS_MAX) {
                snprintf(err_msg, err_cap, "zone tuning entry %u: heater timing out of range",
                        (unsigned)zone_candidate_count);
                return false;
            }
            zc->has_heater_cfg = true;
            zc->heater_window_ms = (float)dwin;
            zc->heater_min_on_ms = (float)don;
            zc->heater_min_off_ms = (float)doff;
        }

        {
            double d1, d2, d3, d4, d5, d6, d7, d8;
            bool h1 = json_field_num(ze, "guard_wrong_dir_window_s", &d1);
            bool h2 = json_field_num(ze, "guard_wrong_dir_rate_c_per_min", &d2);
            bool h3 = json_field_num(ze, "guard_off_settle_s", &d3);
            bool h4 = json_field_num(ze, "guard_runaway_rate_c_per_min", &d4);
            bool h5 = json_field_num(ze, "guard_runaway_margin_c", &d5);
            bool h6 = json_field_num(ze, "guard_drift_period_s", &d6);
            bool h7 = json_field_num(ze, "guard_sensor_fault_debounce_ticks", &d7);
            bool h8 = json_field_num(ze, "guard_frozen_window_s", &d8);
            bool any = h1 || h2 || h3 || h4 || h5 || h6 || h7 || h8;
            if (any) {
                if (!(h1 && h2 && h3 && h4 && h5 && h6 && h7 && h8)) {
                    snprintf(err_msg, err_cap,
                            "zone tuning entry %u: all 8 guard threshold overrides must be present together",
                            (unsigned)zone_candidate_count);
                    return false;
                }
                if (d1 < 0 || d1 > (double)ZONE_GUARD_TIME_S_MAX || d2 < 0 ||
                    d2 > (double)ZONE_GUARD_RATE_C_PER_MIN_MAX || d3 < 0 || d3 > (double)ZONE_GUARD_TIME_S_MAX ||
                    d4 < 0 || d4 > (double)ZONE_GUARD_RATE_C_PER_MIN_MAX || d5 < 0 ||
                    d5 > (double)ZONE_GUARD_MARGIN_C_MAX || d6 < 0 || d6 > (double)ZONE_GUARD_TIME_S_MAX ||
                    d7 < 0 || d7 > (double)ZONE_GUARD_DEBOUNCE_TICKS_MAX || d8 < 0 ||
                    d8 > (double)ZONE_GUARD_TIME_S_MAX) {
                    snprintf(err_msg, err_cap, "zone tuning entry %u: a guard threshold override is out of range",
                            (unsigned)zone_candidate_count);
                    return false;
                }
                zc->has_guard = true;
                zc->guard_wrong_dir_window_s = (float)d1;
                zc->guard_wrong_dir_rate_c_per_min = (float)d2;
                zc->guard_off_settle_s = (float)d3;
                zc->guard_runaway_rate_c_per_min = (float)d4;
                zc->guard_runaway_margin_c = (float)d5;
                zc->guard_drift_period_s = (float)d6;
                zc->guard_sensor_fault_debounce_ticks = (float)d7;
                zc->guard_frozen_window_s = (float)d8;
            }
        }

        double dxzone;
        if (json_field_opt_num(ze, "cross_zone_max_delta_c", 0, (double)ZONE_CROSS_ZONE_DELTA_C_MAX, &dxzone,
                               &zc->has_cross_zone, "cross_zone_max_delta_c", err_msg, err_cap,
                               (unsigned)zone_candidate_count) == false) {
            return false;
        }
        if (zc->has_cross_zone) {
            zc->cross_zone_max_delta_c = (float)dxzone;
        }

        zone_candidate_count++;
    }

    double dsafety;
    bool has_safety_tc = json_field_num(body, "safety_tc_type", &dsafety);
    if (has_safety_tc && (dsafety < 0 || dsafety > 7)) {
        snprintf(err_msg, err_cap, "safety_tc_type out of range (0-7)");
        return false;
    }

    /* ---- Pass 2: everything validated -- commit ---- */
    for (size_t i = 0; i < candidate_count; i++) {
        profile_candidate_t *c = &candidates[i];
        uint8_t out_id = 0;
        char save_err[96];
        if (!profiles_http_save(c->has_id ? c->id : PROFILES_MAX_COUNT, &c->p, &out_id, NULL, save_err,
                                sizeof(save_err))) {
            /* Should not happen -- pass 1 already checked everything
             * profiles_http_save() itself checks -- but if it does (a race
             * with a concurrent change to zone config between pass 1 and
             * pass 2, say), report exactly which entry and why rather than a
             * generic failure. Any candidates before this one in the loop
             * are already committed -- see this function's header comment. */
            snprintf(err_msg, err_cap, "profile entry %u rejected at commit: %s", (unsigned)i, save_err);
            return false;
        }
    }
    for (size_t i = 0; i < zone_candidate_count; i++) {
        zone_candidate_t *zc = &zone_candidates[i];
        if (!zones_config_set_pid(zc->index, zc->kp, zc->ki, zc->kd)) {
            snprintf(err_msg, err_cap, "zone tuning entry %u (channel %u) rejected at commit setting PID gains",
                    (unsigned)i, zc->index);
            return false;
        }
        if (zc->has_model && !zones_config_set_model(zc->index, zc->k_dc, zc->tau_s, zc->dead_time_s)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting the plant model -- value "
                    "outside this firmware's sanity bounds",
                    (unsigned)i, zc->index);
            return false;
        }
        if (zc->has_tc && !zones_config_set_tc_type(zc->index, zc->tc_type)) {
            snprintf(err_msg, err_cap, "zone tuning entry %u (channel %u) rejected at commit setting tc_type",
                    (unsigned)i, zc->index);
            return false;
        }
        /* Version 2 fields -- see zone_candidate_t's comment. Every one of
         * these setters was just validated against the exact same bound in
         * pass 1 above, so a commit-time rejection here means a race with a
         * concurrent config change between the two passes (same rationale
         * as the PID/model/tc_type "should not happen" comments above), not
         * a bug in this pass's own bounds. */
        if (zc->has_name && !zones_config_set_name(zc->index, zc->name)) {
            snprintf(err_msg, err_cap, "zone tuning entry %u (channel %u) rejected at commit setting name",
                    (unsigned)i, zc->index);
            return false;
        }
        if (zc->has_relay_mask && !zones_config_set_relay_mask(zc->index, zc->relay_mask)) {
            snprintf(err_msg, err_cap, "zone tuning entry %u (channel %u) rejected at commit setting relay_mask",
                    (unsigned)i, zc->index);
            return false;
        }
        if (zc->has_thermo_mask && !zones_config_set_thermo_mask(zc->index, zc->thermo_mask)) {
            snprintf(err_msg, err_cap, "zone tuning entry %u (channel %u) rejected at commit setting thermo_mask",
                    (unsigned)i, zc->index);
            return false;
        }
        if (zc->has_ct_mask && !zones_config_set_ct_mask(zc->index, zc->ct_mask)) {
            snprintf(err_msg, err_cap, "zone tuning entry %u (channel %u) rejected at commit setting ct_mask",
                    (unsigned)i, zc->index);
            return false;
        }
        if (zc->has_cal && !zones_config_set_cal_offset(zc->index, zc->cal_offset_c)) {
            snprintf(err_msg, err_cap, "zone tuning entry %u (channel %u) rejected at commit setting cal_offset_c",
                    (unsigned)i, zc->index);
            return false;
        }
        if (zc->has_ramp && !zones_config_set_max_ramp(zc->index, zc->max_ramp_c_per_hr)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting max_ramp_c_per_hr",
                    (unsigned)i, zc->index);
            return false;
        }
        if (zc->has_sanity && !zones_config_set_sanity_rate(zc->index, zc->sanity_rate_c_per_min)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting sanity_rate_c_per_min",
                    (unsigned)i, zc->index);
            return false;
        }
        if (zc->has_mode && !zones_config_set_control_mode(zc->index, (zone_control_mode_t)zc->control_mode)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting control_mode",
                    (unsigned)i, zc->index);
            return false;
        }
        if (zc->has_temp_limits && !zones_config_set_temp_limits(zc->index, zc->max_temp_c, zc->min_temp_c)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting temp limits",
                    (unsigned)i, zc->index);
            return false;
        }
        if (zc->has_heater_cfg &&
            !zones_config_set_heater_cfg(zc->index, zc->heater_window_ms, zc->heater_min_on_ms,
                                         zc->heater_min_off_ms)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting heater timing",
                    (unsigned)i, zc->index);
            return false;
        }
        if (zc->has_guard &&
            !zones_config_set_guard_thresholds(zc->index, zc->guard_wrong_dir_window_s,
                                               zc->guard_wrong_dir_rate_c_per_min, zc->guard_off_settle_s,
                                               zc->guard_runaway_rate_c_per_min, zc->guard_runaway_margin_c,
                                               zc->guard_drift_period_s, zc->guard_sensor_fault_debounce_ticks,
                                               zc->guard_frozen_window_s)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting guard thresholds",
                    (unsigned)i, zc->index);
            return false;
        }
        if (zc->has_cross_zone && !zones_config_set_cross_zone_delta(zc->index, zc->cross_zone_max_delta_c)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting cross_zone_max_delta_c",
                    (unsigned)i, zc->index);
            return false;
        }
    }
    if (has_safety_tc) {
        zones_config_set_safety_tc_type((uint8_t)dsafety); /* only fails on out-of-range, already checked above */
    }

    return true;
}

static esp_err_t backup_import_post_handler(httpd_req_t *req)
{
    /* Interlock FIRST, before reading the body at all -- "refused while a
     * profile is running or the heaters are on" is exactly
     * ota_http_check_interlocks()'s own precondition list (profile RUNNING/
     * PAUSED, autotune active, any zone's heater commanded on, any zone over
     * temperature, the safety link down, or another update already in
     * flight) -- see backup_http.h's header comment: this file's brief named
     * heat_interlock.c/.h, but that module answers the OPPOSITE question
     * ("may heat be commanded while an OTA update is in progress"), not "may
     * a config-changing action proceed while the kiln is hot" -- the
     * predicate this endpoint actually needs is ota_interlock.c/.h via this
     * same ota_http_check_interlocks() wrapper ota_http.c's own OTA routes
     * call before touching flash. Reusing it here (rather than writing a
     * second copy of the same profile/heater/temperature check) keeps there
     * being exactly one place this decision is made, matching the reuse this
     * pass's brief actually asked for even though the specific filename
     * named was the other half of that pair.
     *
     * 2026-08-22: the ONE precondition in that list a restore may proceed
     * past is "the safety link is down", and only when the operator has
     * answered the warning dialog for this request (the
     * X-Ota-Ack-No-Safety header). Restoring a saved configuration streams
     * nothing over that link -- the rule it was inheriting is
     * UPDATE_PROTOCOL.md's about firmware TRANSFERS -- and refusing
     * outright left a board whose safety processor is absent unable to
     * restore the very configuration that gets it commissioned. Every other
     * precondition still refuses unconditionally. */
    char reason[OTA_INTERLOCK_REASON_MAX];
    ota_interlock_result_t gate = ota_http_check_interlocks(ota_http_req_ack_no_safety(req), reason,
                                                            sizeof(reason));
    if (gate != OTA_INTERLOCK_OK) {
        return ota_http_send_interlock_refusal(req, gate, reason);
    }

    if (req->content_len <= 0 || (size_t)req->content_len > BACKUP_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }

    char *body = malloc((size_t)req->content_len + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int ret = httpd_req_recv(req, body + received, (size_t)req->content_len - received);
        if (ret <= 0) {
            /* A short/failed read means the body this handler has is
             * incomplete -- e.g. the connection dropped mid-upload. Nothing
             * has been parsed or applied yet at this point (the read loop
             * runs entirely before backup_import_apply() is ever called), so
             * a truncated upload simply gets refused with nothing changed --
             * exactly the "must not leave configuration half-applied" case,
             * satisfied here by construction rather than by a rollback. */
            free(body);
            ESP_LOGW(TAG, "backup import body read failed/short: %d", ret);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "upload incomplete or connection dropped");
            return ESP_OK;
        }
        received += (size_t)ret;
    }
    body[received] = '\0';

    char err_msg[160];
    bool ok = backup_import_apply(body, err_msg, sizeof(err_msg));
    free(body);

    if (!ok) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, err_msg, strlen(err_msg));
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    const char *ok_json = "{\"ok\":true}";
    return httpd_resp_send(req, ok_json, strlen(ok_json));
}

esp_err_t backup_http_start(void)
{
    httpd_handle_t server = wifi_provision_http_get_server();
    if (!server) {
        ESP_LOGE(TAG, "no HTTP server -- wifi_provision_http_start() must run first");
        return ESP_ERR_INVALID_STATE;
    }

    static const httpd_uri_t page_uri = {
        .uri = "/settings/backup", .method = HTTP_GET, .handler = backup_page_get_handler,
    };
    static const httpd_uri_t export_uri = {
        .uri = "/api/backup/export", .method = HTTP_GET, .handler = backup_export_get_handler,
    };
    static const httpd_uri_t import_uri = {
        .uri = "/api/backup/import", .method = HTTP_POST, .handler = backup_import_post_handler,
    };

    esp_err_t err = httpd_register_uri_handler(server, &page_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/settings/backup) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &export_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/backup/export) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &import_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/backup/import) failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "settings/backup page up");
    return ESP_OK;
}
