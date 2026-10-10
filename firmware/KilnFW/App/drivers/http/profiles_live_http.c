// docs/LIVE_PROFILE_EDIT.md pass 2 -- section 10's five-route HTTP surface
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
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "cfg_fs_refusal_http.h" /* cfg_fs_http_refuse_if_unmounted() */
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

/* M1 (REVIEW_D7GUARD): optimistic concurrency for the working copy. The
 * client echoes the "generation" it last read (GET /api/profile/live, or the
 * POST edit / fork response) as query parameter gen=N on POST edit and POST
 * decide. A mismatch means the working copy changed (another tab, the LCD, a
 * discard + re-fork) since that read: 409, nothing written. An ABSENT gen is
 * accepted (compatible with clients that predate this field): the PcTools
 * `generation` parameter is optional and the GET ?content=1 carries none, so
 * a caller that omits gen is ungated. POST edit does the check, the verified
 * save and the bump atomically (live_profile_save_working_if_gen); decide
 * still pre-checks only. The generation is RAM-only and restarts at 0 on
 * reboot. */
/* Parses gen=N. Returns false when absent (*present=false) or valid; true when malformed. */
static bool live_gen_parse(httpd_req_t *req, bool *present, uint32_t *out)
{
    *present = false;
    *out = 0;
    char query[48];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    char val[16];
    if (httpd_query_key_value(query, "gen", val, sizeof(val)) != ESP_OK) {
        return false;
    }
    char *end = NULL;
    unsigned long g = strtoul(val, &end, 10);
    if (end == val || *end != '\0') {
        return true; /* malformed gen is never silently ignored */
    }
    *present = true;
    *out = (uint32_t)g;
    return false;
}

static bool live_gen_stale(httpd_req_t *req)
{
    bool present;
    uint32_t g;
    if (live_gen_parse(req, &present, &g)) {
        return true;
    }
    return present && g != live_profile_generation();
}

#define LIVE_GEN_STALE_MSG "working copy changed elsewhere -- reload and re-apply your edit"

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
    char refbuf[320]; /* fixed JSON text (~60 B) + msg_escaped's up-to-240 B, plus margin */
    if (st.has_refusal) {
        /* profiles_http_json_escape(), not the narrower json_escape()
         * (Opus review nit N4): json_escape() only doubles '"'/'\\' and
         * leaves a raw control byte (e.g. a literal newline) unescaped,
         * breaking this JSON the same way profile_post_handler()'s
         * collision message once did (Opus review of 5dd23944, finding 3).
         * refusal_err_msg is copied out of profile_executor's own live-edit
         * refusal record (profile_executor_status.c), not operator input,
         * but it is still free-form text.
         *
         * Truncated to 40 source chars BEFORE escaping (msg_trunc), not
         * escaped at its full sizeof(st.refusal_err_msg)==128: a full-width
         * *6+1 escape buffer (769 B) would also force refbuf/json below to
         * grow to match, cascading a rare diagnostic string's worst case
         * through every buffer downstream -- for a status line the operator
         * reads on screen, 40 chars is already generous, and
         * profiles_http_json_escape()'s own `o + 1 < out_cap` bound makes a
         * shorter buffer a safe, non-corrupting truncation either way. */
        char msg_trunc[41];
        /* Precision "%.40s", not a bare "%s" -- GCC's -Werror=format-truncation
         * cannot prove a bare "%s" fits msg_trunc from st.refusal_err_msg's
         * declared size alone, even though msg_trunc's own sizeof is the
         * snprintf limit; a compile-time precision makes the 40-char cap
         * visible to the compiler too. */
        snprintf(msg_trunc, sizeof(msg_trunc), "%.40s", st.refusal_err_msg);
        char msg_escaped[sizeof(msg_trunc) * 6];
        profiles_http_json_escape(msg_trunc, msg_escaped, sizeof(msg_escaped));
        snprintf(refbuf, sizeof(refbuf), "{\"generation\":%u,\"result\":%d,\"message\":\"%s\"}",
                 (unsigned)st.refusal_generation, st.refusal_result, msg_escaped);
    } else {
        snprintf(refbuf, sizeof(refbuf), "null");
    }

    bool pending_decision = pending && live_edit_should_prompt(&rec, st.active);

    char json[640];
    int n = snprintf(json, sizeof(json),
                     "{\"active\":%s,\"origin_id\":%u,\"origin_is_builtin\":%s,"
                     "\"working_id\":%d,\"generation\":%u,\"editable_from_segment\":%u,"
                     "\"pending_decision\":%s,\"last_refusal\":%s}",
                     st.active ? "true" : "false", (unsigned)(st.active ? st.profile_id : 0),
                     (have_rec && rec.origin_is_builtin) ? "true" : "false", working_id,
                     (unsigned)live_profile_generation(), (unsigned)(st.active ? st.segment_index : 0), pending_decision ? "true" : "false",
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
    /* The working copy and its record live in cfg only (2026-10-07): refuse
     * up front with the 503 naming the cause rather than fork and fail. */
    if (cfg_fs_http_refuse_if_unmounted(req)) {
        return ESP_OK;
    }
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
    snprintf(json, sizeof(json), "{\"ok\":true,\"origin_id\":%u,\"working_id\":%u,\"generation\":%u}",
             (unsigned)st.profile_id, (unsigned)rec.working_id, (unsigned)live_profile_generation());
    return send_json(req, json);
}

/* ---- POST /api/profile/live --------------------------------------------
 * Accept an edit into the working slot: parse -> validate (HARD) -> window
 * check -> save. 400 on a bound violation (profiles_validate_candidate's
 * err_msg already names segment/value/limit); 409 on a window violation. */

static esp_err_t api_profile_live_post_handler(httpd_req_t *req)
{
    if (cfg_fs_http_refuse_if_unmounted(req)) {
        return ESP_OK;
    }
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

    bool gen_present;
    uint32_t gen_val;
    if (live_gen_parse(req, &gen_present, &gen_val)) {
        heap_caps_free(warn_json);
        heap_caps_free(candidate);
        return send_conflict(req, LIVE_GEN_STALE_MSG);
    }
    uint32_t saved_gen = 0;
    live_save_result_t sr = live_profile_save_working_if_gen(candidate, gen_present, gen_val, &saved_gen, err, sizeof(err));
    heap_caps_free(candidate);
    if (sr == LIVE_SAVE_STALE) {
        heap_caps_free(warn_json);
        return send_conflict(req, LIVE_GEN_STALE_MSG);
    }
    if (sr != LIVE_SAVE_OK) {
        heap_caps_free(warn_json);
        /* Review fix: this used to be a bare httpd_resp_sendstr(), whose
         * default status is 200 OK -- a failed working-slot write reported
         * SUCCESS to the page. */
        return send_server_error(req, err);
    }

    size_t resp_cap = warn_json_cap + 80;
    char *json = heap_caps_malloc(resp_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!json) {
        heap_caps_free(warn_json);
        return send_server_error(req, "out of memory");
    }
    int rn = snprintf(json, resp_cap, "{\"ok\":true,\"generation\":%u,\"warnings\":%s}",
                      (unsigned)saved_gen, warn_json[0] ? warn_json : "[]");
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

/* ---- decide core (shared with the LCD) ---------------------------------- */

void profiles_live_decide_status(profiles_live_decide_status_t *out)
{
    memset(out, 0, sizeof(*out));
    live_edit_record_t rec;
    if (!live_profile_load_record(&rec) || !rec.pending) {
        return;
    }
    out->record_pending = true;
    out->origin_is_builtin = rec.origin_is_builtin != 0;
    strncpy(out->origin_name, rec.origin_name, sizeof(out->origin_name) - 1);
    profile_executor_live_status_t st;
    profile_executor_get_live_status(&st);
    out->pending_decision = live_edit_should_prompt(&rec, st.active);
}

bool profiles_live_decide_default_name(const profiles_live_decide_status_t *st, char *out, size_t cap)
{
    if (!st || !out || cap < PROFILE_NAME_MAX_LEN + 1) {
        return false;
    }
    const char *base = st->origin_name[0] ? st->origin_name : "Edited";
    for (unsigned n = 1; n <= 9; n++) {
        char suffix[4];
        if (n == 1) {
            snprintf(suffix, sizeof(suffix), "-E");
        } else {
            snprintf(suffix, sizeof(suffix), "-E%u", n);
        }
        size_t keep = PROFILE_NAME_MAX_LEN - strlen(suffix);
        size_t blen = strlen(base);
        if (blen > keep) {
            blen = keep;
            /* Back off to a UTF-8 character boundary: never leave a lead byte
             * or a partial continuation sequence at the cut. */
            while (blen > 0 && ((unsigned char)base[blen] & 0xC0) == 0x80) {
                blen--;
            }
        }
        memcpy(out, base, blen);
        strcpy(out + blen, suffix);
        if (!live_edit_name_collides_ex(out, live_http_name_at, NULL, 0xFF, false, NULL, 0)) {
            return true;
        }
    }
    return false;
}

/* Serialises whole decisions. profiles_live_decide_apply() runs on both the
 * httpd task (web route) and the LVGL task (LCD page); without this a web
 * Overwrite/Save-as and an LCD Discard could interleave their load-record /
 * save / clear steps. Statically allocated (no heap, ~80 B .bss), created once
 * from profiles_live_http_start() before either caller can reach it. A NULL
 * handle (host tests that never call the init) means "no lock, single-threaded
 * caller", the same default safety_ceiling_sync.c uses.
 *
 * LEAF lock: taken only at the top of profiles_live_decide_apply() by a caller
 * that holds no other module lock, and never taken anywhere else, so no other
 * lock can be waiting on it -- it cannot take part in a cycle. Under it the
 * apply path calls only the live_profile NVS accessors, profiles_http_save()
 * (zones_config getters, flash/NVS) and the name lookups; none of those touch
 * s_exec.lock/s_at.lock or re-enter this mutex.
 *
 * Timeout (DECIDE_LOCK_WAIT_MS): returns LIVE_DECIDE_SERVER_ERROR ("another
 * decision is in progress"), i.e. HTTP 500 on the web and a "Refused:" status
 * line on the LCD -- the same bucket the other transient storage failures use. */
#define DECIDE_LOCK_WAIT_MS 1000
static StaticSemaphore_t s_decide_lock_storage;
static SemaphoreHandle_t s_decide_lock;

static void decide_lock_init(void)
{
    if (!s_decide_lock) {
        s_decide_lock = xSemaphoreCreateMutexStatic(&s_decide_lock_storage);
    }
}

static profiles_live_decide_result_t decide_apply_locked(live_edit_decision_kind_t kind, const char *name, bool confirm,
                                                          uint8_t *out_id, char *err, size_t err_cap)
{
    if (err && err_cap) {
        err[0] = '\0';
    }
    live_edit_record_t rec;
    if (!live_profile_load_record(&rec) || !rec.pending) {
        snprintf(err, err_cap, "nothing pending");
        return LIVE_DECIDE_NOTHING_PENDING;
    }

    if (kind == LIVE_EDIT_DECISION_DISCARD) {
        live_edit_decide(LIVE_EDIT_DECISION_DISCARD, &rec, NULL, false, live_http_name_at, NULL, err, err_cap);
        if (!live_profile_clear(err, err_cap)) {
            return LIVE_DECIDE_SERVER_ERROR;
        }
        return LIVE_DECIDE_OK;
    }

    if (kind == LIVE_EDIT_DECISION_SAVE_AS) {
        if (!name || !name[0]) {
            snprintf(err, err_cap, "missing name");
            return LIVE_DECIDE_BAD_REQUEST;
        }
        if (!live_edit_decide(LIVE_EDIT_DECISION_SAVE_AS, &rec, name, false, live_http_name_at, NULL, err, err_cap)) {
            return LIVE_DECIDE_BAD_REQUEST;
        }
        profile_t *working = heap_caps_malloc(sizeof(profile_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!working) {
            snprintf(err, err_cap, "out of memory");
            return LIVE_DECIDE_SERVER_ERROR;
        }
        if (!live_profile_load_working(working)) {
            heap_caps_free(working);
            snprintf(err, err_cap, "out of memory");
            return LIVE_DECIDE_SERVER_ERROR;
        }
        strncpy(working->name, name, sizeof(working->name) - 1);
        working->name[sizeof(working->name) - 1] = '\0';
        uint8_t id = 0;
        uint8_t warn_count = 0;
        bool persisted = true;
        bool saved = profiles_http_save_ex(PROFILES_MAX_COUNT /* first free */, working, &id, &warn_count,
                                           &persisted, err, err_cap);
        heap_caps_free(working);
        if (!saved) {
            return strncmp(err, "busy:", 5) == 0 ? LIVE_DECIDE_BUSY : LIVE_DECIDE_BAD_REQUEST;
        }
        if (!persisted) {
            /* S5: the slot is live in RAM but not in storage; keep the working copy (do NOT clear it) so the
             * edit is not lost, and do not report success. */
            snprintf(err, err_cap, "storage save failed; the edit is kept, retry");
            return LIVE_DECIDE_SERVER_ERROR;
        }
        char clear_err[64];
        live_profile_clear(clear_err, sizeof(clear_err));
        if (out_id) {
            *out_id = id;
        }
        return LIVE_DECIDE_OK;
    }

    if (kind == LIVE_EDIT_DECISION_OVERWRITE) {
        if (!live_edit_can_overwrite(&rec, err, err_cap)) {
            return LIVE_DECIDE_FORBIDDEN;
        }
        if (!confirm) {
            snprintf(err, err_cap, "confirm=1 required to overwrite");
            return LIVE_DECIDE_BAD_REQUEST;
        }
        if (!live_edit_decide(LIVE_EDIT_DECISION_OVERWRITE, &rec, NULL, true, live_http_name_at, NULL, err, err_cap)) {
            return LIVE_DECIDE_BAD_REQUEST;
        }
        profile_t *working = heap_caps_malloc(sizeof(profile_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!working) {
            snprintf(err, err_cap, "out of memory");
            return LIVE_DECIDE_SERVER_ERROR;
        }
        if (!live_profile_load_working(working)) {
            heap_caps_free(working);
            snprintf(err, err_cap, "out of memory");
            return LIVE_DECIDE_SERVER_ERROR;
        }
        uint8_t id = 0;
        uint8_t warn_count = 0;
        bool persisted = true;
        bool saved = profiles_http_save_ex(rec.origin_id, working, &id, &warn_count, &persisted, err, err_cap);
        heap_caps_free(working);
        if (!saved) {
            return strncmp(err, "busy:", 5) == 0 ? LIVE_DECIDE_BUSY : LIVE_DECIDE_BAD_REQUEST;
        }
        if (!persisted) {
            snprintf(err, err_cap, "storage save failed; the edit is kept, retry");
            return LIVE_DECIDE_SERVER_ERROR;
        }
        char clear_err[64];
        live_profile_clear(clear_err, sizeof(clear_err));
        if (out_id) {
            *out_id = id;
        }
        return LIVE_DECIDE_OK;
    }

    snprintf(err, err_cap, "unknown action");
    return LIVE_DECIDE_BAD_REQUEST;
}

profiles_live_decide_result_t profiles_live_decide_apply(live_edit_decision_kind_t kind, const char *name, bool confirm,
                                                          uint8_t *out_id, char *err, size_t err_cap)
{
    if (s_decide_lock && xSemaphoreTake(s_decide_lock, pdMS_TO_TICKS(DECIDE_LOCK_WAIT_MS)) != pdTRUE) {
        if (err && err_cap) {
            snprintf(err, err_cap, "another decision is in progress");
        }
        return LIVE_DECIDE_SERVER_ERROR;
    }
    profiles_live_decide_result_t r = decide_apply_locked(kind, name, confirm, out_id, err, err_cap);
    if (s_decide_lock) {
        xSemaphoreGive(s_decide_lock);
    }
    return r;
}

static esp_err_t send_decide_failure(httpd_req_t *req, profiles_live_decide_result_t r, const char *err)
{
    switch (r) {
    case LIVE_DECIDE_NOTHING_PENDING:
    case LIVE_DECIDE_BUSY:
        return send_conflict(req, err);
    case LIVE_DECIDE_FORBIDDEN:
        return send_forbidden(req, err);
    case LIVE_DECIDE_SERVER_ERROR:
        return send_server_error(req, err);
    default:
        return send_bad_request(req, err);
    }
}

/* ---- POST /api/profile/live/decide -------------------------------------
 * action=save_as&name=... | action=overwrite&confirm=1 | action=discard.
 * 403 overwrite of a builtin origin; 400 overwrite without confirm=1.
 * The decision itself is profiles_live_decide_apply() above -- this handler
 * only parses the form and maps the result to a status. */

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

    char err[160] = {0};
    live_edit_decision_kind_t kind;
    if (strcmp(action, "discard") == 0) {
        kind = LIVE_EDIT_DECISION_DISCARD;
    } else if (strcmp(action, "save_as") == 0) {
        kind = LIVE_EDIT_DECISION_SAVE_AS;
    } else if (strcmp(action, "overwrite") == 0) {
        kind = LIVE_EDIT_DECISION_OVERWRITE;
    } else {
        /* Unknown action: nothing pending still wins (409), as before. */
        profiles_live_decide_status_t st;
        profiles_live_decide_status(&st);
        return st.record_pending ? send_bad_request(req, "unknown action") : send_conflict(req, "nothing pending");
    }

    char name[PROFILE_NAME_MAX_LEN + 1] = {0};
    int name_len = http_form_find_field(body, "name", name, sizeof(name));
    if (name_len > 0 && http_form_value_has_ctl(name, name_len)) {
        return send_bad_request(req, "name contains a control character");
    }
    bool have_name = name_len > 0;
    char confirm[4];
    bool confirmed = http_form_find_field(body, "confirm", confirm, sizeof(confirm)) > 0 && strcmp(confirm, "1") == 0;

    if (live_gen_stale(req)) {
        profiles_live_decide_status_t gst;
        profiles_live_decide_status(&gst);
        if (gst.record_pending) {
            return send_conflict(req, LIVE_GEN_STALE_MSG);
        }
    }
    uint8_t out_id = 0;
    profiles_live_decide_result_t r =
        profiles_live_decide_apply(kind, have_name ? name : NULL, confirmed, &out_id, err, sizeof(err));
    if (r != LIVE_DECIDE_OK) {
        return send_decide_failure(req, r, err);
    }
    if (kind == LIVE_EDIT_DECISION_SAVE_AS) {
        char json[128];
        snprintf(json, sizeof(json), "{\"ok\":true,\"id\":%u}", (unsigned)out_id);
        return send_json(req, json);
    }
    return send_json(req, "{\"ok\":true}");
}

/* ---- registration -------------------------------------------------------- */

esp_err_t profiles_live_http_start(void)
{
    decide_lock_init();
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
