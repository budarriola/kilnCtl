// docs/LIVE_PROFILE_EDIT_PLAN.md pass 2 -- section 10's five-route HTTP surface
// over pass 1's live_profile.c/profile_executor_live_pickup.c backend.
//
// Window-check caveat: live_edit_check_window() needs a "running" profile_t
// to diff the candidate against. The executor's own pickup path (pass 1,
// authoritative -- re-checks under s_exec.lock at swap time) has the real
// in-RAM running profile; this HTTP layer does not, and profile_executor.h
// deliberately exposes only the small profile_executor_live_status_t slice
// (identity + segment_index), not the full segment/rule set, to keep the
// locked accessor surface small per this pass's own instructions. So this
// accept handler diffs the candidate against the ORIGIN SLOT'S CURRENTLY
// STORED content (profiles_http_get()/profiles_builtin_get()) as a
// best-effort pre-check -- accurate for the common case (no prior live edit
// adopted yet this run), potentially stale after a prior edit already got
// picked up mid-run (stored slot content does not change, only s_exec's RAM
// copy does). This is a soft, early 409; the executor's own pickup re-check
// remains the authoritative gate regardless of what this handler decides.

#include "profiles_live_http.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "dashboard_json.h" /* json_escape() */
#include "http_auth_http.h"
#include "http_form.h"
#include "live_profile.h"
#include "profile_executor.h"
#include "profiles_builtin.h"
#include "profiles_http_internal.h"
#include "web_encoding.h"
#include "wifi_provision_http.h"

static const char *TAG = "profiles_live_http";

/* ---- shared helpers -------------------------------------------------- */

static const char *live_http_name_at(void *ctx, uint8_t id)
{
    (void)ctx;
    if (id < PROFILES_MAX_COUNT) {
        if (!profiles_slot_used(id)) {
            return NULL;
        }
        return s_profiles.profiles[id].name;
    }
    if (profiles_builtin_id_valid(id)) {
        const builtin_profile_t *bp = profiles_builtin_entry(id);
        return bp ? bp->code : NULL;
    }
    return NULL;
}

/* Best-effort "running" reference for the window check -- see file header. */
static bool live_http_get_origin_reference(uint8_t origin_id, bool origin_is_builtin, profile_t *out)
{
    if (origin_is_builtin) {
        return profiles_builtin_get(origin_id, out);
    }
    return profiles_http_get(origin_id, out);
}

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send_chunk(req, json, strlen(json));
    if (err != ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0);
        return err;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* Review fix (landing pass, 2026-09-19): every refusal on this surface is a
 * {"ok":false,"error":"..."} JSON body with the right status, NOT bare text.
 * That is the house shape POST /api/profile already uses
 * (profiles_edit_http.c), and live_profile_page.html -- like every other
 * page -- does `r.json().then(...)` on the error path and reads `.error`.
 * A plain-text body made `r.json()` throw, so the page fell into its
 * network-failure catch and showed "could not reach the board" INSTEAD of
 * the server's message naming the offending segment/value/limit, which is
 * exactly what plan section 10 requires the operator to see. */
static esp_err_t send_err_json(httpd_req_t *req, const char *status, const char *msg)
{
    char escaped[320];
    json_escape(msg ? msg : "", escaped, sizeof(escaped));
    char json[400];
    int n = snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}", escaped);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, n < 0 ? 0 : (size_t)n);
}

static esp_err_t send_conflict(httpd_req_t *req, const char *msg)
{
    return send_err_json(req, "409 Conflict", msg);
}

static esp_err_t send_forbidden(httpd_req_t *req, const char *msg)
{
    return send_err_json(req, "403 Forbidden", msg);
}

static esp_err_t send_bad_request(httpd_req_t *req, const char *msg)
{
    return send_err_json(req, "400 Bad Request", msg);
}

static esp_err_t send_server_error(httpd_req_t *req, const char *msg)
{
    return send_err_json(req, "500 Internal Server Error", msg);
}

/* Small, fixed-size POST body reader (form fields only, no profile blob) --
 * mirrors profiles_edit_http.c's read_small_body(). */
static esp_err_t read_small_body(httpd_req_t *req, char *buf, size_t cap)
{
    if (req->content_len <= 0 || (size_t)req->content_len >= cap) {
        return ESP_FAIL;
    }
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int r = httpd_req_recv(req, buf + received, (size_t)req->content_len - received);
        if (r <= 0) {
            return ESP_FAIL;
        }
        received += (size_t)r;
    }
    buf[received] = '\0';
    return ESP_OK;
}

/* ---- GET /live_profile -------------------------------------------------
 * Same embed mechanism as profiles_page_get_handler() (profiles_catalog_http.c).
 * live_profile_page.html is a placeholder as of this pass -- a sibling
 * change replaces its content; only the serving mechanism is this pass's
 * concern. */

extern const uint8_t live_profile_page_html_gz_start[] asm("_binary_live_profile_page_html_gz_start");
extern const uint8_t live_profile_page_html_gz_end[] asm("_binary_live_profile_page_html_gz_end");

static esp_err_t live_profile_page_get_handler(httpd_req_t *req)
{
    if (!web_client_accepts_gzip(req)) {
        return web_send_gzip_not_acceptable(req, TAG, "live_profile_page.html");
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    web_set_asset_cache_headers(req);
    return httpd_resp_send(req, (const char *)live_profile_page_html_gz_start,
                            (size_t)(live_profile_page_html_gz_end - live_profile_page_html_gz_start));
}

/* ---- GET /api/profile/live ---------------------------------------------
 *
 * Plain form: the plan section 10 status object.
 *
 * `?content=1`: the WORKING COPY'S profile body, in the same field shape
 * GET /api/profile returns, so live_profile_page.html can populate its
 * segment editor. Review fix (landing pass, 2026-09-19): the page
 * originally fetched `/api/profile?id=<working_id>` for this, which can
 * never work -- the working slot is deliberately NOT in profiles_http.c's
 * s_profiles array (live_profile.h's own header explains why), so that
 * endpoint 404s on it, and it must stay out of the catalogue/favorites for
 * exactly the same reason. Served on this existing route rather than a
 * sixth one so the route-tier row, the URI-handler cap and the plan's
 * five-route surface are all unchanged.
 */

static esp_err_t live_send_working_content(httpd_req_t *req)
{
    live_edit_record_t rec;
    if (!live_profile_load_record(&rec) || !rec.pending) {
        return send_conflict(req, "no working copy -- fork first");
    }
    profile_t *w = heap_caps_malloc(sizeof(profile_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!w) {
        return send_server_error(req, "out of memory");
    }
    if (!live_profile_load_working(w)) {
        heap_caps_free(w);
        return send_conflict(req, "working copy is not readable");
    }

#define LIVE_CONTENT_CAP (256 + PROFILE_MAX_SEGMENTS * 192)
    char *json = heap_caps_malloc(LIVE_CONTENT_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!json) {
        heap_caps_free(w);
        return send_server_error(req, "out of memory");
    }
    size_t o = 0;
    int n;
    bool truncated = false;
#define LIVE_APPEND(...)                                                                             \
    do {                                                                                             \
        n = snprintf(json + o, LIVE_CONTENT_CAP - o, __VA_ARGS__);                                   \
        if (n < 0 || (size_t)n >= LIVE_CONTENT_CAP - o) {                                            \
            truncated = true;                                                                        \
        } else {                                                                                     \
            o += (size_t)n;                                                                          \
        }                                                                                            \
    } while (0)

    char name_escaped[PROFILE_NAME_MAX_LEN * 2 + 2];
    json_escape(w->name, name_escaped, sizeof(name_escaped));
    LIVE_APPEND("{\"id\":%u,\"name\":\"%s\",\"zone_mask\":%u,\"segment_count\":%u,\"segments\":[",
                (unsigned)LIVE_EDIT_WORKING_SLOT_ID, name_escaped, w->zone_mask, w->segment_count);
    for (uint8_t i = 0; i < w->segment_count && !truncated; i++) {
        const profile_segment_t *sg = &w->segments[i];
        LIVE_APPEND("%s{\"seg_kind\":%u,\"target_c\":%.2f,\"ramp_c_per_hr\":%.2f,\"dwell_min\":%lu,"
                    "\"io_target\":%u,\"io_state\":%u,\"io_blocking\":%u,\"io_leave_on_at_end\":%u}",
                    i == 0 ? "" : ",", sg->seg_kind, (double)sg->target_c, (double)sg->ramp_c_per_hr,
                    (unsigned long)sg->dwell_min, sg->io_target, sg->io_state, sg->io_blocking,
                    sg->io_leave_on_at_end);
    }
    LIVE_APPEND("]}");
#undef LIVE_APPEND
    heap_caps_free(w);
    if (truncated) {
        heap_caps_free(json);
        return send_server_error(req, "working copy did not fit the response buffer");
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, json, o);
    heap_caps_free(json);
    return err;
#undef LIVE_CONTENT_CAP
}

static esp_err_t api_profile_live_get_handler(httpd_req_t *req)
{
    char query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[8];
        if (httpd_query_key_value(query, "content", val, sizeof(val)) == ESP_OK && strcmp(val, "1") == 0) {
            return live_send_working_content(req);
        }
    }

    profile_executor_live_status_t st;
    profile_executor_get_live_status(&st);

    live_edit_record_t rec;
    bool have_rec = live_profile_load_record(&rec);
    bool pending = have_rec && rec.pending;

    /* Review fix: working_id used to be reported unconditionally, so the
     * page could never tell "a firing is running but nothing has been
     * forked yet" from "a working copy exists" -- its Fork button never
     * appeared and it immediately tried to load a working copy that did not
     * exist. -1 is the "no working copy" answer; the page already tests
     * `working_id >= 0`. */
    int working_id = pending ? (int)LIVE_EDIT_WORKING_SLOT_ID : -1;

    /* last_refusal is always an object or null -- never a bare boolean.
     * (It was `true`/`false` on the inactive branch, which the page rendered
     * as the literal text "true".) */
    char refbuf[300];
    if (st.has_refusal) {
        char msg_escaped[sizeof(st.refusal_err_msg) * 2 + 2];
        json_escape(st.refusal_err_msg, msg_escaped, sizeof(msg_escaped));
        snprintf(refbuf, sizeof(refbuf), "{\"generation\":%u,\"result\":%d,\"message\":\"%s\"}",
                 (unsigned)st.refusal_generation, st.refusal_result, msg_escaped);
    } else {
        snprintf(refbuf, sizeof(refbuf), "null");
    }

    bool pending_decision = pending && live_edit_should_prompt(&rec, st.active);

    char json[640];
    int n = snprintf(json, sizeof(json),
                     "{\"active\":%s,\"origin_id\":%u,\"origin_is_builtin\":%s,"
                     "\"working_id\":%d,\"editable_from_segment\":%u,"
                     "\"pending_decision\":%s,\"last_refusal\":%s}",
                     st.active ? "true" : "false", (unsigned)(st.active ? st.profile_id : 0),
                     (have_rec && rec.origin_is_builtin) ? "true" : "false", working_id,
                     (unsigned)(st.active ? st.segment_index : 0), pending_decision ? "true" : "false",
                     refbuf);
    if (n < 0 || (size_t)n >= sizeof(json)) {
        return send_server_error(req, "status response did not fit");
    }
    return send_json(req, json);
}

/* ---- POST /api/profile/live/fork ---------------------------------------
 * Idempotent: origin is whatever is currently running. Refuses (409) if
 * nothing is running or if fork itself fails (no free slot etc). */

static esp_err_t api_profile_live_fork_post_handler(httpd_req_t *req)
{
    profile_executor_live_status_t st;
    profile_executor_get_live_status(&st);
    if (!st.active) {
        return send_conflict(req, "no active firing to fork from");
    }

    bool origin_is_builtin = (st.profile_id >= PROFILES_MAX_COUNT);
    char origin_name[PROFILE_NAME_MAX_LEN + 1] = {0};
    const char *nm = live_http_name_at(NULL, st.profile_id);
    if (nm) {
        strncpy(origin_name, nm, sizeof(origin_name) - 1);
    }

    profile_t *origin = heap_caps_malloc(sizeof(profile_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!origin) {
        return send_server_error(req, "out of memory");
    }
    if (!live_http_get_origin_reference(st.profile_id, origin_is_builtin, origin)) {
        heap_caps_free(origin);
        return send_conflict(req, "origin profile not readable");
    }

    profile_t *working = heap_caps_malloc(sizeof(profile_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!working) {
        heap_caps_free(origin);
        return send_server_error(req, "out of memory");
    }
    live_edit_record_t rec;
    char err[128] = {0};
    bool ok = live_profile_fork(st.profile_id, origin_is_builtin, origin_name, origin, working, &rec, err,
                                 sizeof(err));
    heap_caps_free(origin);
    heap_caps_free(working);
    if (!ok) {
        ESP_LOGW(TAG, "live_profile_fork failed: %s", err);
        return send_conflict(req, err[0] ? err : "fork refused");
    }

    char json[160];
    snprintf(json, sizeof(json), "{\"ok\":true,\"origin_id\":%u,\"working_id\":%u}", (unsigned)st.profile_id,
             (unsigned)rec.working_id);
    return send_json(req, json);
}

/* ---- POST /api/profile/live --------------------------------------------
 * Accept an edit into the working slot: parse -> validate (HARD) -> window
 * check -> save. 400 on a bound violation (profiles_validate_candidate's
 * err_msg already names segment/value/limit); 409 on a window violation. */

static esp_err_t api_profile_live_post_handler(httpd_req_t *req)
{
    profile_executor_live_status_t st;
    profile_executor_get_live_status(&st);
    if (!st.active) {
        return send_conflict(req, "no active firing");
    }
    if (!live_profile_has_pending_for_origin(st.profile_id)) {
        return send_conflict(req, "fork before editing");
    }

    if (req->content_len <= 0 || req->content_len >= PROFILE_BODY_MAX) {
        return send_bad_request(req, "body too large");
    }
    char *body = heap_caps_malloc((size_t)req->content_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        return send_server_error(req, "out of memory");
    }
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int r = httpd_req_recv(req, body + received, (size_t)req->content_len - received);
        if (r <= 0) {
            heap_caps_free(body);
            return ESP_FAIL;
        }
        received += (size_t)r;
    }
    body[received] = '\0';

    profile_t *candidate = heap_caps_malloc(sizeof(profile_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!candidate) {
        heap_caps_free(body);
        return send_server_error(req, "out of memory");
    }
    char err[160] = {0};
    bool parsed = profiles_parse_profile_fields(body, candidate, err, sizeof(err));
    heap_caps_free(body);
    if (!parsed) {
        heap_caps_free(candidate);
        return send_bad_request(req, err);
    }

    /* Review fix (landing pass): the warnings buffer was a 256-byte stack
     * local, far below the cap profile_post_handler() uses for the same
     * validator (PROFILE_MAX_SEGMENTS * 96 + 16). append_warning() stops
     * rather than overruns, but a stopped append also drops the closing
     * `]`, so an over-full warnings array shipped MALFORMED JSON in an
     * otherwise-200 response -- which the page then failed to parse and
     * reported as a lost connection. Heap (PSRAM), same convention as the
     * other transient buffers in this file, and off the 8 KB httpd stack. */
    const size_t warn_json_cap = PROFILE_MAX_SEGMENTS * 96 + 16;
    char *warn_json = heap_caps_malloc(warn_json_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!warn_json) {
        heap_caps_free(candidate);
        return send_server_error(req, "out of memory");
    }
    warn_json[0] = '\0';
    if (!profiles_validate_candidate(candidate, PROFILE_VALIDATE_HARD, warn_json, warn_json_cap, err,
                                      sizeof(err))) {
        heap_caps_free(warn_json);
        heap_caps_free(candidate);
        return send_bad_request(req, err);
    }

    live_edit_record_t rec;
    bool origin_is_builtin = (st.profile_id >= PROFILES_MAX_COUNT);
    if (live_profile_load_record(&rec)) {
        origin_is_builtin = rec.origin_is_builtin;
    }
    profile_t *running = heap_caps_malloc(sizeof(profile_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!running) {
        heap_caps_free(warn_json);
        heap_caps_free(candidate);
        return send_server_error(req, "out of memory");
    }
    if (live_http_get_origin_reference(st.profile_id, origin_is_builtin, running)) {
        if (live_edit_check_window(running, candidate, st.segment_index, err, sizeof(err))) {
            heap_caps_free(running);
            heap_caps_free(warn_json);
            heap_caps_free(candidate);
            return send_conflict(req, err);
        }
    }
    heap_caps_free(running);

    bool saved = live_profile_save_working(candidate, err, sizeof(err));
    heap_caps_free(candidate);
    if (!saved) {
        heap_caps_free(warn_json);
        /* Review fix: this used to be a bare httpd_resp_sendstr(), whose
         * default status is 200 OK -- a failed working-slot write reported
         * SUCCESS to the page. */
        return send_server_error(req, err);
    }

    size_t resp_cap = warn_json_cap + 48;
    char *json = heap_caps_malloc(resp_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!json) {
        heap_caps_free(warn_json);
        return send_server_error(req, "out of memory");
    }
    int rn = snprintf(json, resp_cap, "{\"ok\":true,\"warnings\":%s}", warn_json[0] ? warn_json : "[]");
    heap_caps_free(warn_json);
    if (rn < 0 || (size_t)rn >= resp_cap) {
        heap_caps_free(json);
        return send_server_error(req, "response did not fit");
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t send_err = httpd_resp_send(req, json, (size_t)rn);
    heap_caps_free(json);
    return send_err;
}

/* ---- POST /api/profile/live/decide -------------------------------------
 * action=save_as&name=... | action=overwrite&confirm=1 | action=discard.
 * 403 overwrite of a builtin origin; 400 overwrite without confirm=1. */

static esp_err_t api_profile_live_decide_post_handler(httpd_req_t *req)
{
    char body[192];
    if (read_small_body(req, body, sizeof(body)) != ESP_OK) {
        return send_bad_request(req, "bad body");
    }

    char action[16];
    if (http_form_find_field(body, "action", action, sizeof(action)) < 0) {
        return send_bad_request(req, "missing action");
    }

    live_edit_record_t rec;
    if (!live_profile_load_record(&rec) || !rec.pending) {
        return send_conflict(req, "nothing pending");
    }

    char err[160] = {0};

    if (strcmp(action, "discard") == 0) {
        live_edit_decide(LIVE_EDIT_DECISION_DISCARD, &rec, NULL, false, live_http_name_at, NULL, err, sizeof(err));
        live_profile_clear(err, sizeof(err));
        return send_json(req, "{\"ok\":true}");
    }

    if (strcmp(action, "save_as") == 0) {
        char name[PROFILE_NAME_MAX_LEN + 1];
        if (http_form_find_field(body, "name", name, sizeof(name)) <= 0) {
            return send_bad_request(req, "missing name");
        }
        if (!live_edit_decide(LIVE_EDIT_DECISION_SAVE_AS, &rec, name, false, live_http_name_at, NULL, err,
                               sizeof(err))) {
            return send_bad_request(req, err);
        }
        profile_t *working = heap_caps_malloc(sizeof(profile_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!working) {
            return send_server_error(req, "out of memory");
        }
        if (!live_profile_load_working(working)) {
            heap_caps_free(working);
            return send_server_error(req, "out of memory");
        }
        strncpy(working->name, name, sizeof(working->name) - 1);
        working->name[sizeof(working->name) - 1] = '\0';
        uint8_t out_id = 0;
        uint8_t warn_count = 0;
        bool saved = profiles_http_save(PROFILES_MAX_COUNT /* first free */, working, &out_id, &warn_count, err,
                                         sizeof(err));
        heap_caps_free(working);
        if (!saved) {
            return send_bad_request(req, err);
        }
        live_profile_clear(err, sizeof(err));
        char json[128];
        snprintf(json, sizeof(json), "{\"ok\":true,\"id\":%u}", (unsigned)out_id);
        return send_json(req, json);
    }

    if (strcmp(action, "overwrite") == 0) {
        if (!live_edit_can_overwrite(&rec, err, sizeof(err))) {
            return send_forbidden(req, err);
        }
        char confirm[4];
        bool confirmed =
            http_form_find_field(body, "confirm", confirm, sizeof(confirm)) > 0 && strcmp(confirm, "1") == 0;
        if (!confirmed) {
            return send_bad_request(req, "confirm=1 required to overwrite");
        }
        if (!live_edit_decide(LIVE_EDIT_DECISION_OVERWRITE, &rec, NULL, true, live_http_name_at, NULL, err,
                               sizeof(err))) {
            return send_bad_request(req, err);
        }
        profile_t *working = heap_caps_malloc(sizeof(profile_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!working) {
            return send_server_error(req, "out of memory");
        }
        if (!live_profile_load_working(working)) {
            heap_caps_free(working);
            return send_server_error(req, "out of memory");
        }
        uint8_t out_id = 0;
        uint8_t warn_count = 0;
        bool saved = profiles_http_save(rec.origin_id, working, &out_id, &warn_count, err, sizeof(err));
        heap_caps_free(working);
        if (!saved) {
            return send_bad_request(req, err);
        }
        live_profile_clear(err, sizeof(err));
        return send_json(req, "{\"ok\":true}");
    }

    return send_bad_request(req, "unknown action");
}

/* ---- registration -------------------------------------------------------- */

esp_err_t profiles_live_http_start(void)
{
    httpd_handle_t server = wifi_provision_http_get_server();
    if (!server) {
        ESP_LOGE(TAG, "no httpd server");
        return ESP_FAIL;
    }

    static const httpd_uri_t live_page_uri = {
        .uri = "/live_profile", .method = HTTP_GET, .handler = live_profile_page_get_handler};
    static const httpd_uri_t live_get_uri = {
        .uri = "/api/profile/live", .method = HTTP_GET, .handler = api_profile_live_get_handler};
    static const httpd_uri_t live_fork_uri = {
        .uri = "/api/profile/live/fork", .method = HTTP_POST, .handler = api_profile_live_fork_post_handler};
    static const httpd_uri_t live_post_uri = {
        .uri = "/api/profile/live", .method = HTTP_POST, .handler = api_profile_live_post_handler};
    static const httpd_uri_t live_decide_uri = {
        .uri = "/api/profile/live/decide", .method = HTTP_POST, .handler = api_profile_live_decide_post_handler};

    const httpd_uri_t *routes[] = {&live_page_uri, &live_get_uri, &live_fork_uri, &live_post_uri, &live_decide_uri};
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t err = kiln_http_register(server, routes[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register %s failed: %s", routes[i]->uri, esp_err_to_name(err));
            return err;
        }
    }

    ESP_LOGI(TAG, "live profile edit HTTP routes registered");
    return ESP_OK;
}
