// profiles_export_http -- see profiles_export_http.h for scope/rationale.

#include "profiles_export_http.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_http_server.h"

#include "profiles_http.h"
#include "backup_json.h" // shared hand-rolled JSON reader (see that header's own
                          // comment on why this codebase has no cJSON dependency)
#include "wifi_provision_http.h"

static const char *TAG = "profiles_export_http";

/* Request body cap for the JSON import -- generous vs. profile_post_handler's
 * PROFILE_BODY_MAX (2048, form-encoded and compact): JSON with field names
 * spelled out on every one of up to 12 segments is verbose. Sized the same
 * way profiles_catalog_http.c sizes its export buffer (192B/segment,
 * rounded up) plus headroom for the top-level name/zone_mask keys. */
#define PROFILE_IMPORT_BODY_MAX 4096

/* ---- shared JSON escaping, same convention as every other *_http.c ------- */
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

/* ---- GET /api/profile/export?id=N ---------------------------------------- */

static esp_err_t export_get_handler(httpd_req_t *req)
{
    char query[32];
    char id_str[8];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "id", id_str, sizeof(id_str)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "id missing");
        return ESP_OK;
    }
    char *end = NULL;
    long id = strtol(id_str, &end, 10);
    if (end == id_str || id < 0 || id >= PROFILES_MAX_COUNT) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such profile");
        return ESP_OK;
    }

    profile_t p;
    if (!profiles_http_get((uint8_t)id, &p)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such profile");
        return ESP_OK;
    }

    /* HEAP (PSRAM), not stack -- same reasoning as every other sizable
     * *_http.c response buffer in this driver set (httpd worker stack is a
     * shared, previously-exhausted resource; see profile_post_handler's own
     * comment on the 2026-08-31 stack-overflow audit). 256B fixed part +
     * 192B/segment matches profiles_catalog_http.c's own per-segment budget
     * for the richer (seg_kind/io_*) field set below. */
    const size_t cap = 256 + (size_t)PROFILE_MAX_SEGMENTS * 192;
    char *json = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!json) {
        ESP_LOGE(TAG, "GET /api/profile/export: malloc(%u) failed", (unsigned)cap);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }

    char name_escaped[PROFILE_NAME_MAX_LEN * 2 + 1];
    json_escape(p.name, name_escaped, sizeof(name_escaped));

    size_t o = 0;
    int n;
#define APPEND(...)                                                                              \
    do {                                                                                          \
        n = snprintf(json + o, cap - o, __VA_ARGS__);                                            \
        if (n < 0 || (size_t)n >= cap - o) {                                                      \
            free(json);                                                                          \
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "profile too large to export");  \
            return ESP_OK;                                                                        \
        }                                                                                          \
        o += (size_t)n;                                                                            \
    } while (0)

    APPEND("{\"kind\":\"kilnctl_profile\",\"version\":1,\"name\":\"%s\",\"zone_mask\":%u,\"segments\":[",
           name_escaped, p.zone_mask);
    for (uint8_t i = 0; i < p.segment_count; i++) {
        const profile_segment_t *s = &p.segments[i];
        APPEND("%s{\"seg_kind\":%u,\"target_c\":%.2f,\"ramp_c_per_hr\":%.2f,\"dwell_min\":%lu,"
               "\"io_target\":%u,\"io_state\":%u,\"io_blocking\":%u,\"io_leave_on_at_end\":%u}",
               i == 0 ? "" : ",", s->seg_kind, (double)s->target_c, (double)s->ramp_c_per_hr,
               (unsigned long)s->dwell_min, s->io_target, s->io_state, s->io_blocking,
               s->io_leave_on_at_end);
    }
    APPEND("]}");
#undef APPEND

    /* Filename: the profile's own name where it is a legal bare filename
     * fragment, "profile" otherwise -- never echo characters that could be
     * read as a path or a header-injection attempt into a response header. */
    char fname[PROFILE_NAME_MAX_LEN + 1];
    size_t fo = 0;
    for (const char *pch = p.name; *pch && fo < sizeof(fname) - 1; pch++) {
        char c = *pch;
        bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '_' || c == '-';
        fname[fo++] = safe ? c : '_';
    }
    fname[fo] = '\0';
    if (fo == 0) {
        strcpy(fname, "profile");
    }
    char disp[PROFILE_NAME_MAX_LEN + 32];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s.json\"", fname);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    esp_err_t ret = httpd_resp_send(req, json, o);
    free(json);
    return ret;
}

/* ---- POST /api/profile/import[?id=N] -------------------------------------
 *
 * Body is the JSON object export_get_handler() above produces (or any
 * hand-authored equivalent -- "kind"/"version" are informational only and
 * not enforced here, unlike backup_import.c's whole-board format, since
 * this is a single, much smaller document with no cross-version migration
 * concerns yet). All the real range/feasibility/zone-mask validation is
 * profiles_http_save()'s -- this handler's only job is decoding the JSON
 * into a profile_t candidate, exactly the shape parse_profile_fields()
 * decodes from a form body for the interactive Save path. */
static esp_err_t import_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > PROFILE_IMPORT_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }

    /* HEAP (PSRAM) -- same reasoning as profile_post_handler's own `body`
     * buffer; freed before the (much smaller) response buffer is built. */
    char *body = heap_caps_malloc((size_t)req->content_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        ESP_LOGE(TAG, "POST /api/profile/import: malloc(%u) failed for the request body buffer",
                 (unsigned)(req->content_len + 1));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"out of memory reading the request body\"}");
    }
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int ret = httpd_req_recv(req, body + received, (size_t)req->content_len - received);
        if (ret <= 0) {
            ESP_LOGW(TAG, "profile import body read failed/short: %d", ret);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body read failed");
            free(body);
            return ESP_OK;
        }
        received += (size_t)ret;
    }
    body[received] = '\0';

#define FAIL(msg)                                                                                 \
    do {                                                                                          \
        free(body);                                                                               \
        char errjson[192];                                                                        \
        int en = snprintf(errjson, sizeof(errjson), "{\"ok\":false,\"error\":\"%s\"}", (msg));   \
        httpd_resp_set_status(req, "400 Bad Request");                                            \
        httpd_resp_set_type(req, "application/json");                                             \
        return httpd_resp_send(req, errjson, en < 0 ? 0 : (size_t)en);                            \
    } while (0)

    profile_t candidate;
    memset(&candidate, 0, sizeof(candidate));

    char name[PROFILE_NAME_MAX_LEN + 2]; /* +2: see backup_import.c's identical buffer for why
                                            * (lets an overlong name still be detected, not
                                            * silently pre-truncated to fit). */
    if (!backup_json_field_str(body, "name", name, sizeof(name))) {
        FAIL("missing \\\"name\\\"");
    }
    if (strlen(name) > PROFILE_NAME_MAX_LEN) {
        FAIL("name too long");
    }
    strncpy(candidate.name, name, PROFILE_NAME_MAX_LEN);
    candidate.name[PROFILE_NAME_MAX_LEN] = '\0';

    double dmask;
    if (!backup_json_field_num(body, "zone_mask", &dmask) || dmask < 0 || dmask > 255) {
        FAIL("missing or out-of-range \\\"zone_mask\\\"");
    }
    candidate.zone_mask = (uint8_t)dmask;

    const char *segs_arr = backup_json_obj_find(body, "segments");
    uint8_t seg_i = 0;
    for (const char *se = backup_json_arr_first(segs_arr); se; se = backup_json_arr_next(se)) {
        if (seg_i >= PROFILE_MAX_SEGMENTS) {
            FAIL("too many segments");
        }
        profile_segment_t *seg = &candidate.segments[seg_i];

        /* seg_kind defaults to 0 (ZONE_RAMP) when absent, so an export
         * produced before this field existed (or a hand-authored minimal
         * JSON) still imports as a plain ramp/dwell profile. */
        double dkind = 0.0;
        bool has_kind = false;
        if (!backup_json_field_opt_num(se, "seg_kind", 0, 1, &dkind, &has_kind, "seg_kind", NULL, 0, seg_i)) {
            FAIL("segment: seg_kind out of range (0-1)");
        }
        seg->seg_kind = has_kind ? (uint8_t)dkind : PROFILE_SEG_KIND_ZONE_RAMP;

        double dt = 0.0, dr = 0.0, dd = 0.0;
        /* target_c/ramp_c_per_hr are only meaningful for a ZONE_RAMP
         * segment (see profile_segment_t's own doc comment) -- absent for a
         * RELAY_IO one is legal, not an error, same as profiles_http_save()
         * itself only range-checks them for that kind. */
        bool have_t = backup_json_field_num(se, "target_c", &dt);
        bool have_r = backup_json_field_num(se, "ramp_c_per_hr", &dr);
        if (seg->seg_kind == PROFILE_SEG_KIND_ZONE_RAMP && (!have_t || !have_r)) {
            FAIL("segment missing target_c/ramp_c_per_hr");
        }
        seg->target_c = (float)dt;
        seg->ramp_c_per_hr = (float)dr;

        if (!backup_json_field_num(se, "dwell_min", &dd) || dd < 0) {
            FAIL("segment missing or invalid dwell_min");
        }
        seg->dwell_min = (uint32_t)dd;

        /* backup_json_field_opt_num() returns false for a PRESENT-but-
         * invalid field (absent is fine, false with *out_has left
         * untouched -- see its own header comment) -- each call below must
         * therefore check the return value itself, not just *out_has,
         * or a present-but-out-of-range field would silently fall back to
         * 0 instead of failing the import. err_msg/err_cap are NULL/0
         * (snprintf(NULL, 0, ...) is well-defined, writes nothing) because
         * this handler reports its own field-specific FAIL() message
         * instead of that helper's generic one. */
        double dio_num = 0.0;
        bool has_v = false;
        seg->io_target = 0;
        if (!backup_json_field_opt_num(se, "io_target", 0, 255, &dio_num, &has_v, "io_target", NULL, 0, seg_i)) {
            FAIL("segment: io_target out of range");
        }
        if (has_v) seg->io_target = (uint8_t)dio_num;
        has_v = false;
        seg->io_state = 0;
        if (!backup_json_field_opt_num(se, "io_state", 0, 1, &dio_num, &has_v, "io_state", NULL, 0, seg_i)) {
            FAIL("segment: io_state out of range");
        }
        if (has_v) seg->io_state = (uint8_t)dio_num;
        has_v = false;
        seg->io_blocking = 0;
        if (!backup_json_field_opt_num(se, "io_blocking", 0, 1, &dio_num, &has_v, "io_blocking", NULL, 0, seg_i)) {
            FAIL("segment: io_blocking out of range");
        }
        if (has_v) seg->io_blocking = (uint8_t)dio_num;
        has_v = false;
        seg->io_leave_on_at_end = 0;
        if (!backup_json_field_opt_num(se, "io_leave_on_at_end", 0, 1, &dio_num, &has_v, "io_leave_on_at_end", NULL,
                                        0, seg_i)) {
            FAIL("segment: io_leave_on_at_end out of range");
        }
        if (has_v) seg->io_leave_on_at_end = (uint8_t)dio_num;

        seg_i++;
    }
    if (seg_i == 0) {
        FAIL("no segments");
    }
    candidate.segment_count = seg_i;
    free(body);
    body = NULL;
#undef FAIL

    /* Optional ?id=N on the import URL addresses an exact slot (create-or-
     * overwrite), same convention profile_post_handler's form "id" field
     * uses; absent/invalid means "first free slot" via
     * profiles_http_save()'s own PROFILES_MAX_COUNT-means-first-free rule. */
    uint8_t requested_id = PROFILES_MAX_COUNT;
    char query[32], id_str[8];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "id", id_str, sizeof(id_str)) == ESP_OK) {
        char *end = NULL;
        long rid = strtol(id_str, &end, 10);
        if (end != id_str && rid >= 0 && rid < PROFILES_MAX_COUNT) {
            requested_id = (uint8_t)rid;
        }
    }

    uint8_t out_id = 0;
    uint8_t warn_count = 0;
    char err_msg[128];
    if (!profiles_http_save(requested_id, &candidate, &out_id, &warn_count, err_msg, sizeof(err_msg))) {
        char errjson[192];
        int en = snprintf(errjson, sizeof(errjson), "{\"ok\":false,\"error\":\"%s\"}", err_msg);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, errjson, en < 0 ? 0 : (size_t)en);
    }

    char json[128];
    int n = snprintf(json, sizeof(json), "{\"ok\":true,\"id\":%u,\"warning_count\":%u}", out_id, warn_count);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, n < 0 ? 0 : (size_t)n);
}

esp_err_t profiles_export_http_start(void)
{
    httpd_handle_t server = wifi_provision_http_get_server();
    if (!server) {
        ESP_LOGE(TAG, "no HTTP server -- wifi_provision_http_start() must run first");
        return ESP_ERR_INVALID_STATE;
    }

    static const httpd_uri_t export_uri = {
        .uri = "/api/profile/export", .method = HTTP_GET, .handler = export_get_handler,
    };
    static const httpd_uri_t import_uri = {
        .uri = "/api/profile/import", .method = HTTP_POST, .handler = import_post_handler,
    };
    esp_err_t err = httpd_register_uri_handler(server, &export_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(GET /api/profile/export) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &import_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(POST /api/profile/import) failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}
