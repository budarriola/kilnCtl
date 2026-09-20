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

static esp_err_t send_conflict(httpd_req_t *req, const char *msg)
{
    httpd_resp_set_status(req, "409 Conflict");
    return httpd_resp_sendstr(req, msg);
}

static esp_err_t send_forbidden(httpd_req_t *req, const char *msg)
{
    httpd_resp_set_status(req, "403 Forbidden");
    return httpd_resp_sendstr(req, msg);
}

static esp_err_t send_bad_request(httpd_req_t *req, const char *msg)
{
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_sendstr(req, msg);
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

/* ---- GET /api/profile/live --------------------------------------------- */

static esp_err_t api_profile_live_get_handler(httpd_req_t *req)
{
    profile_executor_live_status_t st;
    profile_executor_get_live_status(&st);

    live_edit_record_t rec;
    bool have_rec = live_profile_load_record(&rec);

    char json[512];
    int n;
    if (!st.active) {
        n = snprintf(json, sizeof(json),
                     "{\"active\":false,\"origin_id\":0,\"origin_is_builtin\":false,"
                     "\"working_id\":0,\"editable_from_segment\":0,"
                     "\"pending_decision\":%s,\"last_refusal\":%s}",
                     (have_rec && rec.pending) ? "true" : "false",
                     st.has_refusal ? "true" : "false");
    } else {
        bool pending_decision = have_rec && rec.pending && live_edit_should_prompt(&rec, st.active);
        if (st.has_refusal) {
            char refbuf[256];
            snprintf(refbuf, sizeof(refbuf), "{\"generation\":%u,\"result\":%d,\"message\":\"%s\"}",
                     (unsigned)st.refusal_generation, st.refusal_result, st.refusal_err_msg);
            n = snprintf(json, sizeof(json),
                         "{\"active\":true,\"origin_id\":%u,\"origin_is_builtin\":%s,"
                         "\"working_id\":%u,\"editable_from_segment\":%u,"
                         "\"pending_decision\":%s,\"last_refusal\":%s}",
                         (unsigned)st.profile_id, (have_rec && rec.origin_is_builtin) ? "true" : "false",
                         (unsigned)LIVE_EDIT_WORKING_SLOT_ID, (unsigned)st.segment_index,
                         pending_decision ? "true" : "false", refbuf);
        } else {
            n = snprintf(json, sizeof(json),
                         "{\"active\":true,\"origin_id\":%u,\"origin_is_builtin\":%s,"
                         "\"working_id\":%u,\"editable_from_segment\":%u,"
                         "\"pending_decision\":%s,\"last_refusal\":null}",
                         (unsigned)st.profile_id, (have_rec && rec.origin_is_builtin) ? "true" : "false",
                         (unsigned)LIVE_EDIT_WORKING_SLOT_ID, (unsigned)st.segment_index,
                         pending_decision ? "true" : "false");
        }
    }
    if (n < 0 || (size_t)n >= sizeof(json)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
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
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    }
    if (!live_http_get_origin_reference(st.profile_id, origin_is_builtin, origin)) {
        heap_caps_free(origin);
        return send_conflict(req, "origin profile not readable");
    }

    profile_t *working = heap_caps_malloc(sizeof(profile_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!working) {
        heap_caps_free(origin);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
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
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
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
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    }
    char err[160] = {0};
    bool parsed = profiles_parse_profile_fields(body, candidate, err, sizeof(err));
    heap_caps_free(body);
    if (!parsed) {
        heap_caps_free(candidate);
        return send_bad_request(req, err);
    }

    char warn_json[256] = {0};
    if (!profiles_validate_candidate(candidate, PROFILE_VALIDATE_HARD, warn_json, sizeof(warn_json), err,
                                      sizeof(err))) {
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
        heap_caps_free(candidate);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    }
    if (live_http_get_origin_reference(st.profile_id, origin_is_builtin, running)) {
        if (live_edit_check_window(running, candidate, st.segment_index, err, sizeof(err))) {
            heap_caps_free(running);
            heap_caps_free(candidate);
            return send_conflict(req, err);
        }
    }
    heap_caps_free(running);

    bool saved = live_profile_save_working(candidate, err, sizeof(err));
    heap_caps_free(candidate);
    if (!saved) {
        return httpd_resp_sendstr(req, err); /* 500 default status */
    }

    char json[320];
    snprintf(json, sizeof(json), "{\"ok\":true,\"warnings\":%s}", warn_json[0] ? warn_json : "[]");
    return send_json(req, json);
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
        return httpd_resp_sendstr(req, "{\"ok\":true}");
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
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
        }
        if (!live_profile_load_working(working)) {
            heap_caps_free(working);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
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
        return httpd_resp_sendstr(req, json);
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
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
        }
        if (!live_profile_load_working(working)) {
            heap_caps_free(working);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
        }
        uint8_t out_id = 0;
        uint8_t warn_count = 0;
        bool saved = profiles_http_save(rec.origin_id, working, &out_id, &warn_count, err, sizeof(err));
        heap_caps_free(working);
        if (!saved) {
            return send_bad_request(req, err);
        }
        live_profile_clear(err, sizeof(err));
        return httpd_resp_sendstr(req, "{\"ok\":true}");
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
