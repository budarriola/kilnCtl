#include "profiles_http_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "profile_feasibility.h"
#include "profiles_builtin.h"
#include "web_encoding.h"

/* TODO.md 10.6a: embedded pre-gzipped (CMakeLists.txt gzips it at configure
 * time before idf_component_register runs), hence the "_gz" in both the
 * filename and the symbol it generates. */
extern const uint8_t profiles_page_html_gz_start[] asm("_binary_profiles_page_html_gz_start");
extern const uint8_t profiles_page_html_gz_end[] asm("_binary_profiles_page_html_gz_end");

/* ---- HTML page ------------------------------------------------------------ */

/* TODO.md 10.6a: content negotiation lives in web_encoding.h's shared
 * web_client_accepts_gzip() -- absent Accept-Encoding is legal and served
 * gzip (RFC 9110 s12.5.3); a header that explicitly excludes gzip gets an
 * uncompressed 406 rather than a body it cannot decode. */
esp_err_t profiles_page_get_handler(httpd_req_t *req)
{
    if (!web_client_accepts_gzip(req)) {
        return web_send_gzip_not_acceptable(req, PROFILES_TAG, "profiles_page.html");
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    web_set_asset_cache_headers(req);
    return httpd_resp_send(req, (const char *)profiles_page_html_gz_start,
                           (size_t)(profiles_page_html_gz_end - profiles_page_html_gz_start));
}

/* ---- JSON ------------------------------------------------------------------ */

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

/* ---- Builtin catalogue JSON ------------------------------------------------
 *
 * RESPONSE-SIZE FINDING, which is why the catalogue gets its own endpoint
 * rather than being appended to GET /api/profile's detail object:
 *   - The UART CONTROL bridge caps a reply at BRIDGE_REPLY_MAX, which is
 *     UART_PROTO_MAX_PAYLOAD (uart_bridge.c) -- a few hundred bytes. Nothing
 *     resembling a catalogue fits through it, so the catalogue is HTTP-only
 *     and the UART profile commands are left alone entirely.
 *   - Every existing JSON handler in this file builds into ONE stack buffer
 *     and snprintf-truncates on overflow (see the APPEND macros). The
 *     catalogue is 28 entries x up to 12 segments plus a title and slug --
 *     roughly 25 KB. That is far past any sane stack buffer on this target,
 *     so this endpoint is the one place in the file that streams with
 *     httpd_resp_send_chunk() and reuses a single ~1 KB per-entry buffer.
 *     Building the whole thing in one buffer would have silently truncated
 *     the tail of the catalogue, which is the failure mode most likely to go
 *     unnoticed until a schedule is missing on the page.
 *
 * GET /api/profiles keeps its existing single-buffer shape but is likewise
 * chunked now, because it lists the catalogue's summaries alongside the user
 * slots.
 */

/* Escapes into a caller buffer and returns it, for use inline in a printf
 * argument list. */
static const char *esc(const char *src, char *buf, size_t cap)
{
    json_escape(src, buf, cap);
    return buf;
}

/* Sends one snprintf'd chunk, honouring the one thing snprintf's return value
 * is easy to get wrong: on truncation it reports the length it WOULD have
 * written, which is larger than the buffer. Passing that straight to
 * httpd_resp_send_chunk() reads past the end of the buffer. The worst-case
 * field widths in the builtin JSON below (fixed text + escaped code + a
 * 127-char title + two copies of the slug) add up to more than the 384-byte
 * chunk buffer, so this is reachable the day someone adds a longer title --
 * and the table those titles live in is generated, so that is a plausible
 * edit rather than a theoretical one.
 *
 * Truncation also means the JSON is malformed, which a clamp alone would hide,
 * so it is logged rather than silently shortened. */
static esp_err_t send_chunk_checked(httpd_req_t *req, const char *buf, int n, size_t cap, const char *what)
{
    if (n < 0) {
        return ESP_OK; /* encoding error -- skip this fragment, keep the response alive */
    }
    if ((size_t)n >= cap) {
        ESP_LOGW(PROFILES_TAG, "%s JSON truncated at %u bytes -- response will be malformed", what,
                 (unsigned)cap);
        n = (int)(cap - 1);
    }
    return httpd_resp_send_chunk(req, buf, (size_t)n);
}

/* Appends one builtin entry's summary (no segments) to a chunked response. */
static esp_err_t send_builtin_summary(httpd_req_t *req, uint8_t id, const builtin_profile_t *b, bool first)
{
    profile_t p;
    profile_seg_verdict_t rollup = PROFILE_SEG_UNKNOWN;
    if (profiles_builtin_get(id, &p)) {
        rollup = profile_feasibility_profile_mask(0, &p, NULL, 0);
    }

    char code_e[PROFILE_NAME_MAX_LEN * 2 + 1];
    char title_e[128];
    char slug_e[64];
    char chunk[384];
    int n = snprintf(chunk, sizeof(chunk),
                     "%s{\"id\":%u,\"builtin\":true,\"name\":\"%s\",\"code\":\"%s\",\"title\":\"%s\","
                     "\"slug\":\"%s\",\"url\":\"https://digitalfire.com/schedule/%s\",\"hidden\":%s,"
                     "\"zone_mask\":0,\"segment_count\":%u,\"feasibility\":\"%s\"}",
                     first ? "" : ",", id, esc(b->code, code_e, sizeof(code_e)),
                     esc(b->code, code_e, sizeof(code_e)), esc(b->title, title_e, sizeof(title_e)),
                     esc(b->slug, slug_e, sizeof(slug_e)), b->slug,
                     profiles_builtin_is_hidden(id) ? "true" : "false", b->segment_count,
                     profile_feasibility_verdict_str(rollup));
    return send_chunk_checked(req, chunk, n, sizeof(chunk), "builtin summary");
}

/* Full builtin entry: summary fields + every segment with its own verdict. */
static esp_err_t send_builtin_full(httpd_req_t *req, uint8_t id, const builtin_profile_t *b, bool first)
{
    profile_t p;
    profile_seg_verdict_t per_seg[PROFILE_MAX_SEGMENTS];
    profile_seg_verdict_t rollup = PROFILE_SEG_UNKNOWN;
    for (size_t i = 0; i < PROFILE_MAX_SEGMENTS; i++) {
        per_seg[i] = PROFILE_SEG_UNKNOWN;
    }
    if (profiles_builtin_get(id, &p)) {
        rollup = profile_feasibility_profile_mask(0, &p, per_seg, PROFILE_MAX_SEGMENTS);
    }

    char code_e[PROFILE_NAME_MAX_LEN * 2 + 1];
    char title_e[128];
    char slug_e[64];
    char chunk[384];
    int n = snprintf(chunk, sizeof(chunk),
                     "%s{\"id\":%u,\"builtin\":true,\"read_only\":true,\"name\":\"%s\",\"code\":\"%s\","
                     "\"title\":\"%s\",\"slug\":\"%s\",\"url\":\"https://digitalfire.com/schedule/%s\","
                     "\"hidden\":%s,\"zone_mask\":0,\"segment_count\":%u,\"feasibility\":\"%s\","
                     "\"segments\":[",
                     first ? "" : ",", id, esc(b->code, code_e, sizeof(code_e)),
                     esc(b->code, code_e, sizeof(code_e)), esc(b->title, title_e, sizeof(title_e)),
                     esc(b->slug, slug_e, sizeof(slug_e)), b->slug,
                     profiles_builtin_is_hidden(id) ? "true" : "false", b->segment_count,
                     profile_feasibility_verdict_str(rollup));
    esp_err_t err = send_chunk_checked(req, chunk, n, sizeof(chunk), "builtin header");
    if (err != ESP_OK) {
        return err;
    }

    for (uint8_t i = 0; i < b->segment_count && i < PROFILE_MAX_SEGMENTS; i++) {
        n = snprintf(chunk, sizeof(chunk),
                     "%s{\"target_c\":%.2f,\"ramp_c_per_hr\":%.2f,\"dwell_min\":%lu,\"feasibility\":\"%s\"}",
                     i == 0 ? "" : ",", (double)b->segments[i].target_c,
                     (double)b->segments[i].ramp_c_per_hr, (unsigned long)b->segments[i].dwell_min,
                     profile_feasibility_verdict_str(per_seg[i]));
        err = send_chunk_checked(req, chunk, n, sizeof(chunk), "builtin segment");
        if (err != ESP_OK) {
            return err;
        }
    }
    return httpd_resp_send_chunk(req, "]}", 2);
}

/* GET /api/profiles/builtin -- the whole catalogue, segments and verdicts
 * included. ?all=1 includes hidden entries (the "restore" UI needs to show
 * what it would restore); the default omits them. */
esp_err_t builtin_list_get_handler(httpd_req_t *req)
{
    bool include_hidden = false;
    char query[48];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[8];
        if (httpd_query_key_value(query, "all", val, sizeof(val)) == ESP_OK && val[0] == '1') {
            include_hidden = true;
        }
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send_chunk(req, "[", 1);
    bool first = true;
    for (size_t i = 0; i < g_builtin_profile_count && err == ESP_OK; i++) {
        uint8_t id = (uint8_t)(PROFILE_BUILTIN_ID_BASE + i);
        if (!include_hidden && profiles_builtin_is_hidden(id)) {
            continue;
        }
        err = send_builtin_full(req, id, &g_builtin_profiles[i], first);
        first = false;
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, "]", 1);
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0); /* terminate the chunked response */
    }
    return err;
}

/* Worst-case size of one user-slot entry's JSON, sized against a name that
 * FULLY escapes -- the TODO.md bug this replaces: the old 96-byte-per-slot
 * budget was sized off PROFILE_NAME_MAX_LEN's raw 15 chars, but
 * json_escape() can double every one of them (a `"` or `\` costs two output
 * bytes), and the fixed text around the name is not free either. Counted
 * literally: `,{"id":255,"builtin":false,"name":"` (36) + up to
 * PROFILE_NAME_MAX_LEN*2 (30) escaped name bytes + `","zone_mask":255,`
 * `"segment_count":12}` (37) = 103; rounded up with slack for the format
 * rather than re-deriving the exact count if a field ever widens. */
#define PROFILE_LIST_ENTRY_MAX 190 /* +30 (2026-09-02) for the ",\"exceeds_ceiling\":false" marker */

/* Bytes reserved at the tail of `json` that no per-slot APPEND is ever
 * allowed to write into -- so the fallback "listing truncated" notice below
 * always has guaranteed room to land, and the array's own close (sent as a
 * separate chunk, never through this buffer) is never the thing at risk.
 * Same discipline as readiness_http.c's append_item() reserve. */
#define PROFILE_LIST_CLOSE_RESERVE 96

esp_err_t profiles_list_get_handler(httpd_req_t *req)
{
    char json[PROFILES_MAX_COUNT * PROFILE_LIST_ENTRY_MAX + PROFILE_LIST_CLOSE_RESERVE + 16];
    size_t o = 0;
    int n;
    bool dropped = false; /* an item didn't fit even the enlarged budget -- report it, don't hide it */

    /* Never writes past sizeof(json) - PROFILE_LIST_CLOSE_RESERVE -- `avail`
     * is clamped to 0 once `o` reaches that line, so a would-be write past it
     * is treated exactly like any other overflow (dropped, not truncated
     * into the reserve). */
#define APPEND(...)                                                                              \
    do {                                                                                          \
        size_t avail = (o + PROFILE_LIST_CLOSE_RESERVE < sizeof(json))                             \
                           ? sizeof(json) - PROFILE_LIST_CLOSE_RESERVE - o                          \
                           : 0;                                                                     \
        n = snprintf(json + o, avail, __VA_ARGS__);                                               \
        if (n < 0 || (size_t)n >= avail) {                                                         \
            dropped = true;                                                                        \
            goto list_done;                                                                        \
        }                                                                                          \
        o += (size_t)n;                                                                            \
    } while (0)

    json[o++] = '[';
    bool first = true;
    for (uint8_t id = 0; id < PROFILES_MAX_COUNT; id++) {
        if (!(s_profiles.used_bitmap & (1u << id))) {
            continue;
        }
        const profile_t *p = &s_profiles.profiles[id];
        char name_escaped[PROFILE_NAME_MAX_LEN * 2 + 1];
        json_escape(p->name, name_escaped, sizeof(name_escaped));
        /* exceeds_ceiling (2026-09-02 owner correction): computed live against
         * each zone's CURRENT max_temp_c, not stored -- a profile that was
         * fine to save can start exceeding the ceiling later if the zone's
         * limit is lowered, and vice versa, so this must always reflect the
         * present configuration, not a snapshot from save time. Advisory
         * only; see profile_exceeds_zone_ceiling()'s own comment for why
         * this never blocks the save/list, only the actual run start. */
        bool exceeds = profile_exceeds_zone_ceiling(p, NULL, 0);
        APPEND("%s{\"id\":%u,\"builtin\":false,\"name\":\"%s\",\"zone_mask\":%u,\"segment_count\":%u,"
               "\"exceeds_ceiling\":%s}",
               first ? "" : ",", id, name_escaped, p->zone_mask, p->segment_count,
               exceeds ? "true" : "false");
        first = false;
    }

#undef APPEND

list_done:
    if (dropped) {
        /* Guaranteed to fit: PROFILE_LIST_CLOSE_RESERVE bytes at json+o were
         * never touched by any APPEND above. Reported AS an item -- a
         * silently shortened list looks exactly like a pass, which is the
         * failure mode this exists to prevent (same rule readiness_http.c's
         * append_item() dropped-item notice follows). */
        int n2 = snprintf(json + o, sizeof(json) - o,
                          "%s{\"id\":null,\"builtin\":false,\"error\":\"one or more profiles omitted -- "
                          "listing too large\"}",
                          first ? "" : ",");
        if (n2 > 0 && (size_t)n2 < sizeof(json) - o) {
            o += (size_t)n2;
            first = false;
        } else {
            ESP_LOGE(PROFILES_TAG, "profiles listing: dropped-item notice itself didn't fit -- "
                         "PROFILE_LIST_CLOSE_RESERVE is too small");
        }
    }

    /* Chunked, because the visible builtin summaries appended after the user
     * slots would not fit alongside them in one stack buffer -- see the
     * response-size note above builtin_list_get_handler(). Segments are
     * deliberately NOT included here; a listing does not need 136 of them,
     * and GET /api/profile?id=<builtin> / GET /api/profiles/builtin serve
     * them when something actually does. */
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send_chunk(req, json, o);
    for (size_t i = 0; i < g_builtin_profile_count && err == ESP_OK; i++) {
        uint8_t bid = (uint8_t)(PROFILE_BUILTIN_ID_BASE + i);
        if (profiles_builtin_is_hidden(bid)) {
            continue; /* "removed by the user" -- see /api/profiles/builtin?all=1 */
        }
        err = send_builtin_summary(req, bid, &g_builtin_profiles[i], first);
        first = false;
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, "]", 1);
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }
    return err;
}

esp_err_t profile_detail_get_handler(httpd_req_t *req)
{
    char query[32];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "id missing");
        return ESP_OK;
    }
    char id_str[8];
    if (httpd_query_key_value(query, "id", id_str, sizeof(id_str)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "id missing");
        return ESP_OK;
    }
    char *end = NULL;
    long id = strtol(id_str, &end, 10);
    if (end == id_str || id < 0 || id > 255) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such profile");
        return ESP_OK;
    }

    /* Builtin catalogue entry: served read-only, hidden or not (hiding is a
     * listing preference, so a direct reference must still resolve). */
    if (profiles_builtin_id_valid((uint8_t)id)) {
        const builtin_profile_t *b = profiles_builtin_entry((uint8_t)id);
        httpd_resp_set_type(req, "application/json");
        esp_err_t berr = send_builtin_full(req, (uint8_t)id, b, true);
        if (berr == ESP_OK) {
            berr = httpd_resp_send_chunk(req, NULL, 0);
        }
        return berr;
    }

    if (id >= PROFILES_MAX_COUNT || !(s_profiles.used_bitmap & (1u << id))) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such profile");
        return ESP_OK;
    }

    const profile_t *p = &s_profiles.profiles[id];
    /* Sized for the per-segment "feasibility":"unreachable" field plus the
     * seg_kind/io_target/io_state/io_blocking/io_leave_on_at_end fields added
     * below (relay/IO segment support) -- worst case measured at 170 bytes
     * per segment, rounded up. 224 -> 624 (2026-09-02) for the new
     * exceeds_ceiling/ceiling_note fields -- ceiling_note_escaped is up to
     * sizeof(ceiling_note)*2 = 512 bytes worst case (every byte escaped;
     * ceiling_note itself widened 160 -> 256 to satisfy -Werror=format-
     * truncation's conservative worst-case-float-width analysis). +
     * PROFILE_MAX_ON_OFF_RULES * 128 (2026-09-08, plan step 5) -- each rule
     * object measured well under 110 bytes worst case, rounded up. */
    /* Heap-allocated (2026-09-08, httpd_worker stack-budget pass) -- this was
     * a single ~4.1 KB stack-local array, the dominant frame in
     * check_httpd_task_stack_budget's worst reachable httpd path. PSRAM,
     * same convention as backup_export_get_handler's stream buffer just
     * above: pure JSON construction, no flash writes anywhere in this
     * function, so there is no PSRAM/flash-worker re-entrancy hazard to
     * avoid (contrast backup_import.c's candidate arrays, which also DO
     * reach flash but are PSRAM-preferred -- backup_import.c's `_locked()`
     * split copies each field into the live NVS-backed store before the
     * single flash write, so nothing PSRAM-backed is live across that write;
     * see backup_import.c:1180-1193 and
     * docs/audits/unreviewed_changes_review_2026-09-08.md finding D6, which
     * this comment previously described backwards). Freed on the one `send:` exit
     * every path below funnels through; an allocation failure degrades to a
     * clean 500 rather than a stack overflow. */
#define PROFILE_DETAIL_JSON_CAP (816 + PROFILE_MAX_SEGMENTS * 192 + PROFILE_MAX_ON_OFF_RULES * 128)
    char *json = heap_caps_malloc(PROFILE_DETAIL_JSON_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }
    size_t o = 0;
    int n;

#define APPEND(...)                                                                              \
    do {                                                                                          \
        n = snprintf(json + o, PROFILE_DETAIL_JSON_CAP - o, __VA_ARGS__);                        \
        if (n < 0 || (size_t)n >= PROFILE_DETAIL_JSON_CAP - o) {                                  \
            goto send;                                                                            \
        }                                                                                          \
        o += (size_t)n;                                                                            \
    } while (0)

    /* Same model-based feasibility the catalogue entries carry -- a user's own
     * profile deserves the identical answer, and the UI can then colour both
     * kinds with one rule. */
    profile_seg_verdict_t per_seg[PROFILE_MAX_SEGMENTS];
    for (size_t si = 0; si < PROFILE_MAX_SEGMENTS; si++) {
        per_seg[si] = PROFILE_SEG_UNKNOWN;
    }
    /* Review fix: a user-slot profile with zone_mask == 0 targets no zones
     * at all -- the executor refuses it outright ("targets no zones"), so
     * calling profile_feasibility_profile_mask() with that mask would get
     * an optimistic coupled verdict for a profile that can never actually
     * run. Report unknown instead of asking feasibility a question that
     * does not apply. */
    profile_seg_verdict_t rollup;
    if (p->zone_mask == 0) {
        rollup = PROFILE_SEG_UNKNOWN;
    } else {
        rollup = profile_feasibility_profile_mask(p->zone_mask, p, per_seg, PROFILE_MAX_SEGMENTS);
    }

    char name_escaped[PROFILE_NAME_MAX_LEN * 2 + 1];
    json_escape(p->name, name_escaped, sizeof(name_escaped));
    /* exceeds_ceiling/ceiling_note (2026-09-02 owner correction): same live
     * check the list endpoint runs -- see profile_exceeds_zone_ceiling()'s
     * own comment. ceiling_note is "" when exceeds_ceiling is false. */
    char ceiling_note[256];
    bool exceeds_ceiling = profile_exceeds_zone_ceiling(p, ceiling_note, sizeof(ceiling_note));
    char ceiling_note_escaped[sizeof(ceiling_note) * 2];
    json_escape(ceiling_note, ceiling_note_escaped, sizeof(ceiling_note_escaped));
    APPEND("{\"id\":%ld,\"builtin\":false,\"read_only\":false,\"name\":\"%s\",\"zone_mask\":%u,"
           "\"segment_count\":%u,\"feasibility\":\"%s\",\"exceeds_ceiling\":%s,"
           "\"ceiling_note\":\"%s\",\"segments\":[",
           id, name_escaped, p->zone_mask, p->segment_count,
           profile_feasibility_verdict_str(rollup), exceeds_ceiling ? "true" : "false",
           ceiling_note_escaped);
    for (uint8_t i = 0; i < p->segment_count; i++) {
        const profile_segment_t *s = &p->segments[i];
        /* Genuine firmware defect found while wiring the editor UI to this
         * endpoint (owner's relay/IO segment request, profiles_http.h's
         * profile_seg_kind_t comment): this response used to emit only the
         * three ZONE_RAMP fields, so GETting a profile that has a RELAY_IO
         * segment silently dropped seg_kind/io_target/io_state/io_blocking/
         * io_leave_on_at_end -- editProfile() in profiles_page.html loads a
         * profile through exactly this call and repopulates the editor from
         * it, so without these fields every "Edit" of a saved relay segment
         * would reload it as target_c 0 / ramp 0 / dwell <whatever dwell_min
         * held>, i.e. a bogus ZONE_RAMP row, discarding the relay config on
         * the very next save. Added rather than routed around client-side. */
        APPEND("%s{\"seg_kind\":%u,\"target_c\":%.2f,\"ramp_c_per_hr\":%.2f,\"dwell_min\":%lu,"
               "\"io_target\":%u,\"io_state\":%u,\"io_blocking\":%u,\"io_leave_on_at_end\":%u,"
               "\"feasibility\":\"%s\"}",
               i == 0 ? "" : ",", s->seg_kind, (double)s->target_c, (double)s->ramp_c_per_hr,
               (unsigned long)s->dwell_min, s->io_target, s->io_state, s->io_blocking,
               s->io_leave_on_at_end, profile_feasibility_verdict_str(per_seg[i]));
    }
    APPEND("],\"on_off_rules\":[");
    /* docs/ON_OFF_ZONE_PLAN.md plan step 5 API surface -- echoes exactly the
     * fields profiles_edit_http.c's rule%u_* parser accepts, same round-trip
     * discipline the segment loop above already follows. */
    for (uint8_t i = 0; i < p->on_off_rule_count; i++) {
        const profile_on_off_rule_t *r = &p->on_off_rules[i];
        APPEND("%s{\"zone\":%u,\"segment\":%u,\"enable\":%u,\"phase_mask\":%u,\"direction_mask\":%u,"
               "\"temp_cmp\":%u,\"temp_c\":%.2f,\"time_start_s\":%u,\"time_stop_s\":%u,\"invert\":%u}",
               i == 0 ? "" : ",", r->zone_index, r->segment_index, r->enable, r->phase_mask, r->direction_mask,
               r->temp_cmp, (double)r->temp_threshold_c, r->time_start_s, r->time_stop_s, r->invert);
    }
    APPEND("]}");

#undef APPEND

send:
    httpd_resp_set_type(req, "application/json");
    esp_err_t send_err = httpd_resp_send(req, json, o);
    free(json);
    return send_err;
}
#undef PROFILE_DETAIL_JSON_CAP
