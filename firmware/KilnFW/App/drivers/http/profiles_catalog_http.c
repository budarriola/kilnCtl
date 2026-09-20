#include "profiles_http_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "profile_feasibility.h"
#include "profiles_builtin.h"
#include "profiles_favorites.h"
#include "web_encoding.h"
#include "zones_config_query.h"  /* zones_config_get_thermo_count() -- builtin_effective_zone_mask() */

/* PROFILE_SLOTS_100_PLAN.md section 4 item 3 / section 7 task 8: the web
 * "recently fired" group needs a per-profile last-run timestamp, and the
 * plan is explicit that this must cost one extra field in the EXISTING
 * /api/profiles listing rather than a new route (the URI handler cap has one
 * spare slot, check_uri_handler_cap.ps1). Declared here rather than by
 * #including profile_executor.h: that header drags in MAX31856.h/kiln_io.h/
 * pid.h/safety_link.h/thermal_guard.h, none of which this file otherwise
 * needs, and test_profiles_http.c #includes this file directly (it does not
 * link profile_executor.c -- see that test file's own header comment on why
 * it fakes profile_executor-adjacent symbols instead) -- pulling that whole
 * header chain in here would drag it into that host-test build too. A bare
 * prototype for one small, stable-signature accessor keeps the dependency to
 * exactly what's used; profile_executor_status.c (which already has every
 * header this needs) implements it next to profile_executor_get_firing_history(),
 * and test_profiles_http.c supplies its own fake, same convention as every
 * other faked symbol in that file. */
uint32_t profile_executor_last_run_started_unix_s(uint8_t profile_id);

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

/* profiles_http_json_escape() now lives in profiles_http_internal.h (Opus review of
 * 5dd23944, finding 3) -- see that header's doc comment. */

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
    profiles_http_json_escape(src, buf, cap);
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
        /* Opus review, docs/PROFILE_SLOTS_100_PLAN.md sec 7 task 11 item 4:
         * this used to clamp and still send ESP_OK, shipping a truncated
         * fragment as if it were a complete, valid 200. Unreachable at
         * today's field widths (~127 B max vs the smallest cap here, 190 B)
         * but a caller checking `err == ESP_OK` in a loop has no way to
         * learn the body was cut short. Fail the call instead so the loop's
         * own err-checked exit takes over, same as any other write failure
         * these handlers already treat as fatal. */
        ESP_LOGE(PROFILES_TAG, "%s JSON truncated at %u bytes -- refusing to send a malformed fragment", what,
                 (unsigned)cap);
        return ESP_FAIL;
    }
    return httpd_resp_send_chunk(req, buf, (size_t)n);
}

/* The zone set a builtin catalogue entry will actually be RUN on.
 *
 * builtin_profile_t carries no zone assignment of its own -- the catalogue is
 * zone-agnostic. profiles_http.c's profile getter (the one profile_executor_
 * run() uses) resolves that absence to "every configured zone" before the
 * executor ever sees it, and profile_feasibility_profile_mask() resolves an
 * incoming 0 the same way, for the same stated reason. This helper is that
 * one fact written once for THIS file's responses, so what a client is told
 * a builtin targets is the set it would really run on -- not a literal 0 a
 * consumer has to guess the meaning of.
 *
 * Emitting the resolved mask (rather than 0) is what makes main_page.html's
 * computeProfileLimitWarning() work for builtins at all: it walks zone_mask's
 * bits, so a 0 meant "check no zones" and the profile-vs-ceiling warning was
 * silently inert for the entire builtin catalogue -- the exact class most
 * likely to exceed a bench board's configured ceiling (BQ1000 targets over
 * 1000C against an 80C bench ceiling and showed no icon at all). A board with
 * no zones configured still reports 0, which is honest: there is no zone to
 * judge against, and profiles_http.c's getter says the same. */
static uint8_t builtin_effective_zone_mask(void)
{
    uint8_t n = zones_config_get_thermo_count();
    return (n >= 8) ? 0xFFu : (uint8_t)((1u << n) - 1u);
}

/* Appends one builtin entry's summary (no segments) to a chunked response. */
static esp_err_t send_builtin_summary(httpd_req_t *req, uint8_t id, const builtin_profile_t *b, bool first)
{
    profile_t p;
    const uint8_t zone_mask = builtin_effective_zone_mask();
    profile_seg_verdict_t rollup = PROFILE_SEG_UNKNOWN;
    if (profiles_builtin_get(id, &p)) {
        rollup = profile_feasibility_profile_mask(zone_mask, &p, NULL, 0);
    }

    char code_e[PROFILE_NAME_MAX_LEN * 2 + 1];
    char title_e[128];
    char slug_e[64];
    char chunk[384];
    int n = snprintf(chunk, sizeof(chunk),
                     "%s{\"id\":%u,\"builtin\":true,\"name\":\"%s\",\"code\":\"%s\",\"title\":\"%s\","
                     "\"slug\":\"%s\",\"url\":\"https://digitalfire.com/schedule/%s\",\"hidden\":%s,"
                     "\"zone_mask\":%u,\"segment_count\":%u,\"feasibility\":\"%s\","
                     "\"last_run_started_unix_s\":%lu}",
                     first ? "" : ",", id, esc(b->code, code_e, sizeof(code_e)),
                     esc(b->code, code_e, sizeof(code_e)), esc(b->title, title_e, sizeof(title_e)),
                     esc(b->slug, slug_e, sizeof(slug_e)), b->slug,
                     profiles_builtin_is_hidden(id) ? "true" : "false", (unsigned)zone_mask,
                     b->segment_count, profile_feasibility_verdict_str(rollup),
                     (unsigned long)profile_executor_last_run_started_unix_s(id));
    return send_chunk_checked(req, chunk, n, sizeof(chunk), "builtin summary");
}

/* Full builtin entry: summary fields + every segment with its own verdict. */
static esp_err_t send_builtin_full(httpd_req_t *req, uint8_t id, const builtin_profile_t *b, bool first)
{
    profile_t p;
    profile_seg_verdict_t per_seg[PROFILE_MAX_SEGMENTS];
    const uint8_t zone_mask = builtin_effective_zone_mask();
    profile_seg_verdict_t rollup = PROFILE_SEG_UNKNOWN;
    for (size_t i = 0; i < PROFILE_MAX_SEGMENTS; i++) {
        per_seg[i] = PROFILE_SEG_UNKNOWN;
    }
    if (profiles_builtin_get(id, &p)) {
        rollup = profile_feasibility_profile_mask(zone_mask, &p, per_seg, PROFILE_MAX_SEGMENTS);
    }

    char code_e[PROFILE_NAME_MAX_LEN * 2 + 1];
    char title_e[128];
    char slug_e[64];
    char chunk[384];
    int n = snprintf(chunk, sizeof(chunk),
                     "%s{\"id\":%u,\"builtin\":true,\"read_only\":true,\"name\":\"%s\",\"code\":\"%s\","
                     "\"title\":\"%s\",\"slug\":\"%s\",\"url\":\"https://digitalfire.com/schedule/%s\","
                     "\"hidden\":%s,\"zone_mask\":%u,\"segment_count\":%u,\"feasibility\":\"%s\","
                     "\"segments\":[",
                     first ? "" : ",", id, esc(b->code, code_e, sizeof(code_e)),
                     esc(b->code, code_e, sizeof(code_e)), esc(b->title, title_e, sizeof(title_e)),
                     esc(b->slug, slug_e, sizeof(slug_e)), b->slug,
                     profiles_builtin_is_hidden(id) ? "true" : "false", (unsigned)zone_mask,
                     b->segment_count, profile_feasibility_verdict_str(rollup));
    esp_err_t err = send_chunk_checked(req, chunk, n, sizeof(chunk), "builtin header");
    if (err != ESP_OK) {
        return err;
    }

    for (uint8_t i = 0; i < b->segment_count && i < PROFILE_MAX_SEGMENTS; i++) {
        n = snprintf(chunk, sizeof(chunk),
                     /* seg_kind is emitted EXPLICITLY, matching the user-slot
                      * emitter below. Omitting it left main_page.html's
                      * computeProfileLimitWarning() reading `undefined !== 0`
                      * and bailing out of every builtin segment; a missing
                      * field must never be read as "kind 0" by accident, so
                      * the producer states it rather than the consumer
                      * guessing. Every builtin schedule segment IS a zone
                      * ramp -- builtin_profile_t has no relay/IO concept at
                      * all -- so the constant, not a stored field. */
                     "%s{\"seg_kind\":%u,\"target_c\":%.2f,\"ramp_c_per_hr\":%.2f,\"dwell_min\":%lu,"
                     "\"feasibility\":\"%s\"}",
                     i == 0 ? "" : ",", (unsigned)PROFILE_SEG_KIND_ZONE_RAMP,
                     (double)b->segments[i].target_c,
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
 * profiles_http_json_escape() can double every one of them (a `"` or `\` costs two output
 * bytes), and the fixed text around the name is not free either. Counted
 * literally: `,{"id":255,"builtin":false,"name":"` (36) + up to
 * PROFILE_NAME_MAX_LEN*2 (30) escaped name bytes + `","zone_mask":255,`
 * `"segment_count":12}` (37) = 103; rounded up with slack for the format
 * rather than re-deriving the exact count if a field ever widens.
 *
 * Recomputed (Opus review nit N3, 2026-09-20): profiles_http_json_escape()
 * can emit \u00XX (6 output bytes) for a control byte, not just a doubled
 * backslash for '"'/'\\' -- the *2 name-escaping term above understated the
 * true worst case. With PROFILE_NAME_MAX_LEN*6 (90) escaped name bytes, the
 * base entry is 36 + 90 + 37 = 163, plus the two markers below (+30, +37) =
 * 230. The constant is widened to keep the same ~121-byte slack margin the
 * original 224 (vs. its own 103-byte base) carried. */
#define PROFILE_LIST_ENTRY_MAX 352 /* +30 (2026-09-02) for the ",\"exceeds_ceiling\":false" marker;
                                     * +37 (PROFILE_SLOTS_100_PLAN.md task 8) for
                                     * ",\"last_run_started_unix_s\":4294967295" (10-digit uint32 max);
                                     * base recomputed to 230 for N3's *6 name-escaping term above,
                                     * +121 slack (matching the original margin) = 351, rounded to 352 */

/* Chunked (2026-09-19, 100-slot plan task 2): the old shape built the ENTIRE
 * user-slot section into one stack-local `json[PROFILES_MAX_COUNT *
 * PROFILE_LIST_ENTRY_MAX + ...]` array before ever calling
 * httpd_resp_send_chunk() -- fine at 8 slots (~1.6 KB), but
 * PROFILES_MAX_COUNT growing toward 100 would put ~19 KB on the 8 KB
 * httpd_worker stack, the exact `httpd_stack_blob` class
 * check_httpd_task_stack_budget.py exists to catch. Each entry is now built
 * into, and sent from, its own small per-entry buffer -- the same
 * send_chunk_checked() pattern send_builtin_summary()/send_builtin_full()
 * already use just above in this file -- so the stack cost is O(1) in
 * PROFILES_MAX_COUNT, not O(N). At the current 8 slots the emitted bytes are
 * unchanged: same fields, same order (user slots, then visible builtins),
 * same JSON. */
esp_err_t profiles_list_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send_chunk(req, "[", 1);
    bool first = true;
    for (uint8_t id = 0; id < PROFILES_MAX_COUNT && err == ESP_OK; id++) {
        if (!profiles_slot_used(id)) {
            continue;
        }
        const profile_t *p = &s_profiles.profiles[id];
        /* *6+1, not *2+1 (Opus review nit N3): profiles_http_json_escape()
         * can emit \u00XX (6 output bytes) for any control byte, not just a
         * doubled backslash for '"'/'\\' -- *2+1 under-sized this whenever a
         * name held a control byte, silently truncating the escaped name. */
        char name_escaped[PROFILE_NAME_MAX_LEN * 6 + 1];
        profiles_http_json_escape(p->name, name_escaped, sizeof(name_escaped));
        /* exceeds_ceiling (2026-09-02 owner correction): computed live against
         * each zone's CURRENT max_temp_c, not stored -- a profile that was
         * fine to save can start exceeding the ceiling later if the zone's
         * limit is lowered, and vice versa, so this must always reflect the
         * present configuration, not a snapshot from save time. Advisory
         * only; see profile_exceeds_zone_ceiling()'s own comment for why
         * this never blocks the save/list, only the actual run start. */
        bool exceeds = profile_exceeds_zone_ceiling(p, NULL, 0);
        uint32_t last_run = profile_executor_last_run_started_unix_s(id);
        char chunk[PROFILE_LIST_ENTRY_MAX];
        int n = snprintf(chunk, sizeof(chunk),
                         "%s{\"id\":%u,\"builtin\":false,\"name\":\"%s\",\"zone_mask\":%u,\"segment_count\":%u,"
                         "\"exceeds_ceiling\":%s,\"last_run_started_unix_s\":%lu}",
                         first ? "" : ",", id, name_escaped, p->zone_mask, p->segment_count,
                         exceeds ? "true" : "false", (unsigned long)last_run);
        err = send_chunk_checked(req, chunk, n, sizeof(chunk), "profile list entry");
        first = false;
    }

    /* Visible builtin summaries appended after the user slots -- unchanged
     * from before this pass; see the response-size note above
     * builtin_list_get_handler(). Segments are deliberately NOT included
     * here; a listing does not need 136 of them, and
     * GET /api/profile?id=<builtin> / GET /api/profiles/builtin serve them
     * when something actually does. */
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

    if (id >= PROFILES_MAX_COUNT || !profiles_slot_used(id)) {
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
     * PROFILE_MAX_ON_OFF_RULES * PROFILE_ON_OFF_RULE_JSON_MAX (2026-09-08,
     * plan step 5; that per-rule constant, profiles_http_internal.h, was
     * widened 128 -> 224 in the Opus review pass) -- a rule object with
     * temp_source measured 218 bytes worst case, not the ~110 originally
     * assumed; combined with 12 segments + a 512-byte escaped ceiling_note
     * the old 128 figure could overflow this buffer and truncate the JSON. */
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
#define PROFILE_DETAIL_JSON_CAP (816 + PROFILE_MAX_SEGMENTS * 192 + PROFILE_MAX_ON_OFF_RULES * PROFILE_ON_OFF_RULE_JSON_MAX)
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

    /* *6+1, not *2+1 -- see the identical N3 comment above in
     * profiles_list_get_handler(). */
    char name_escaped[PROFILE_NAME_MAX_LEN * 6 + 1];
    profiles_http_json_escape(p->name, name_escaped, sizeof(name_escaped));
    /* exceeds_ceiling/ceiling_note (2026-09-02 owner correction): same live
     * check the list endpoint runs -- see profile_exceeds_zone_ceiling()'s
     * own comment. ceiling_note is "" when exceeds_ceiling is false. */
    char ceiling_note[256];
    bool exceeds_ceiling = profile_exceeds_zone_ceiling(p, ceiling_note, sizeof(ceiling_note));
    char ceiling_note_escaped[sizeof(ceiling_note) * 2];
    profiles_http_json_escape(ceiling_note, ceiling_note_escaped, sizeof(ceiling_note_escaped));
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
     * discipline the segment loop above already follows. "temp_source"
     * added alongside the profiles_page.html editor pass: without it, a
     * rule loaded back into the editor could not tell whether its
     * (already-saved) temp_cmp was actually live -- profile_resolve_on_off_
     * rule() (profile_executor.c) only honors temp_cmp when temp_source == 1,
     * so this field must round-trip or the editor's own preview would lie
     * about which rules are actually armed. */
    for (uint8_t i = 0; i < p->on_off_rule_count; i++) {
        const profile_on_off_rule_t *r = &p->on_off_rules[i];
        APPEND("%s{\"zone\":%u,\"segment\":%u,\"enable\":%u,\"phase_mask\":%u,\"direction_mask\":%u,"
               "\"temp_source\":%u,\"temp_cmp\":%u,\"temp_c\":%.2f,\"time_start_s\":%u,\"time_stop_s\":%u,"
               "\"invert\":%u}",
               i == 0 ? "" : ",", r->zone_index, r->segment_index, r->enable, r->phase_mask, r->direction_mask,
               r->temp_source, r->temp_cmp, (double)r->temp_threshold_c, r->time_start_s, r->time_stop_s,
               r->invert);
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

/* ---- Favorites listing ------------------------------------------------------
 *
 * GET /api/profiles/favorites -> {"user_mask":N,"builtin_mask":N,"ids":[...]}
 *
 * Reports the marks only; it deliberately does NOT repeat the profiles
 * themselves. A favorite is a shortcut, not a move (profiles_favorites.h), so
 * every favorited profile is already present in the ordinary listings that
 * GET /api/profiles and GET /api/profiles/builtin return, and the page draws
 * its Favorites section by picking those ids out of the list it already has.
 *
 * Chunked (2026-09-19, 100-slot plan task 2): the old shape built the WHOLE
 * response into one `json[320]` stack local, guarded by
 * `_Static_assert(PROFILES_MAX_COUNT + 32 <= 60, ...)` -- true at 8 slots,
 * false by construction once PROFILES_MAX_COUNT reaches 100. Streaming one
 * id at a time removes the dependency on the total id count entirely, so
 * there is no total-capacity assert left to get wrong as that count grows;
 * the _Static_assert below instead pins the one thing that actually could
 * overflow regardless of id count -- a single id's own rendered width -- so
 * it stays correct at 100 (and at any other id count PROFILE_BUILTIN_ID_BASE
 * plus a byte range can produce). The wire format (a single
 * `{"user_mask":N,"builtin_mask":N,"ids":[...]}` object) is unchanged. */
esp_err_t favorites_list_get_handler(httpd_req_t *req)
{
    profiles_slot_bitmap_t user_mask;
    uint32_t builtin_mask = 0;
    profiles_favorites_masks(&user_mask, &builtin_mask);

    /* Widest one id can ever render as: a comma plus up to 3 digits (ids are
     * uint8_t, max 255) plus a NUL. This bound does not depend on
     * PROFILES_MAX_COUNT or the builtin catalogue count, so it holds at 8
     * slots, at 100, and beyond. */
#define FAV_ID_CHUNK_MAX 8
    _Static_assert(FAV_ID_CHUNK_MAX >= 1 + 3 + 1, "one favorited id (comma + 3 digits + NUL) must fit");

    /* "user_mask" stays a single number in the wire format -- word[0] of the
     * widened bitmap is exactly the old uint32_t's bits, and at today's 8
     * slots that is the whole mask, so this is byte-identical to before. */
    char header[64];
    int n = snprintf(header, sizeof(header), "{\"user_mask\":%lu,\"builtin_mask\":%lu,\"ids\":[",
                     (unsigned long)profiles_slot_bitmap_to_u32(&user_mask), (unsigned long)builtin_mask);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = send_chunk_checked(req, header, n, sizeof(header), "favorites header");

    bool first = true;
    for (uint8_t i = 0; i < PROFILES_MAX_COUNT && err == ESP_OK; i++) {
        if (!profiles_slot_bitmap_test(&user_mask, i)) {
            continue;
        }
        /* Review fold-in (PROFILE_SLOTS_100_PLAN.md section 7): filter by the
         * used bitmap too. An orphaned favorite bit surviving over an
         * unused/deleted slot (e.g. a power cut between clearing the
         * favorite and erasing the slot, or vice versa, pre-fix) must never
         * be emitted here as a favorited id for a profile that no longer
         * exists. */
        if (!profiles_slot_used(i)) {
            continue;
        }
        char idbuf[FAV_ID_CHUNK_MAX];
        int in = snprintf(idbuf, sizeof(idbuf), "%s%u", first ? "" : ",", (unsigned)i);
        err = send_chunk_checked(req, idbuf, in, sizeof(idbuf), "favorites user id");
        first = false;
    }
    for (size_t i = 0; i < g_builtin_profile_count && i < 32 && err == ESP_OK; i++) {
        if (!(builtin_mask & (1u << i))) {
            continue;
        }
        char idbuf[FAV_ID_CHUNK_MAX];
        int in = snprintf(idbuf, sizeof(idbuf), "%s%u", first ? "" : ",",
                          (unsigned)(PROFILE_BUILTIN_ID_BASE + i));
        err = send_chunk_checked(req, idbuf, in, sizeof(idbuf), "favorites builtin id");
        first = false;
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, "]}", 2);
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }
    return err;
}
#undef FAV_ID_CHUNK_MAX
