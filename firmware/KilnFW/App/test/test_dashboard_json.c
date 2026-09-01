// Host tests for App/drivers/dashboard_json.c -- json_escape()/
// append_zone_status_json(), split out of dashboard_http.c specifically so
// they could be host-tested at all (that file #includes lvgl_port.h at file
// scope, which pulls in the LCD/touch driver stack and does not compile on
// MSVC -- see dashboard_json.h's own doc comment). This file #includes
// dashboard_json.c directly, same convention as test_zones_http.c/
// test_profiles_http.c/etc: the whole point is to exercise the REAL
// production function, not a reimplementation of it.
//
// Opus review, round 3, blocker 1: dashboard_http.c's /api/control handler
// sized its JSON buffer at 128 + MAX31856_CHANNEL_COUNT*224 bytes -- already
// too small for the pre-existing (259B) worst-case per-zone object, and
// this fix's own two new fields (ff_hold_used_matrix/ff_hold_infeasible)
// pushed it to 313B, landing 3 zones at 1054B against an 800B budget.
// append_zone_status_json()'s own truncation guard
// (`if (n<0 || n>=cap-o) return o;`) bails silently, and the caller (the
// real handler, reproduced here) still appends the closing brace on top of
// the truncated buffer -- an unterminated `"zones":[{...},{...` + `}`,
// invalid JSON, with no error anywhere. Fixed by raising the per-zone
// allowance to 384/256 fixed (dashboard_http.c:1548-1568's own comment has
// the sizing math); this test renders the actual worst case at 3 zones and
// checks the result is complete, not just "didn't crash".
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "test_common.h"

int g_test_failures = 0;
int g_test_count = 0;

#include "../drivers/dashboard_json.c"

// ---------------------------------------------------------------------------
// Small helpers: fill a profile_exec_status_t with the WORST-CASE field
// widths every %-format spec in append_zone_status_json() can produce, at
// MAX31856_CHANNEL_COUNT (3 in this repo -- see uart_task_ids.h's
// THERMO_CHANNEL_COUNT) active zones -- the exact scenario that was
// truncating.

static void fill_worst_case_zone(profile_exec_zone_status_t *z, uint8_t zi)
{
    z->active = true;
    z->actual_c = -1234.56f;         /* "%.2f" worst case this test exercises */
    z->actual_valid = true;
    z->relay_commanded_on = false;
    z->duty = -1.234f;               /* "%.3f" */
    z->control_mode = 255;           /* "%u" on a uint8_t */
    z->faulted = true;
    /* Fault reason at its full 95-char capacity (96-byte buffer, see
     * profile_executor.h), every character a '"' -- json_escape()'s worst
     * case, doubling every byte. */
    memset(z->fault_reason, '"', sizeof(z->fault_reason) - 1);
    z->fault_reason[sizeof(z->fault_reason) - 1] = '\0';
    z->fault_guard = 255;
    z->pid_p = -12.3456f; z->pid_i = -12.3456f; z->pid_d = -12.3456f; z->pid_ff = -12.3456f; /* "%.4f" */
    z->cooling_limited = true;
    z->heat_blocked = true;
    z->heat_blocked_sources = 0xFFFFFFFFu; /* "%lu" worst case */
    z->ff_hold_used_matrix = true;
    z->ff_hold_infeasible = true;
    z->ff_membership_change_count = 0xFFFFFFFFu; /* "%lu" worst case, same as heat_blocked_sources */
    /* firing_stats worst case (PID_EXPANSION_PLAN.md Phase 7a dashboard
     * wiring, dashboard_json.h's DASHBOARD_JSON_PROFILE_EXEC_BUF_SIZE sizing
     * note): every %.2f/%.4f field at its widest plausible negative value,
     * every %lu/%u field at its type's max. */
    z->firing_stats.mean_error_c = -1234.56f;
    z->firing_stats.max_overshoot_c = 1234.56f;
    z->firing_stats.max_overshoot_elapsed_s = 0xFFFFFFFFu;
    z->firing_stats.max_overshoot_segment = 255;
    z->firing_stats.max_undershoot_c = 1234.56f;
    z->firing_stats.max_undershoot_elapsed_s = 0xFFFFFFFFu;
    z->firing_stats.max_undershoot_segment = 255;
    z->firing_stats.iae_raw_c_s = -123456.78f;
    z->firing_stats.iae_normalized = -1234.5678f;
    z->firing_stats.ramp_err_mean_c = -1234.56f;
    z->firing_stats.ramp_err_max_c = -1234.56f;
    z->firing_stats.dwell_err_mean_c = -1234.56f;
    z->firing_stats.dwell_err_max_c = -1234.56f;
    z->firing_stats.sample_count = 0xFFFFFFFFu;
    z->firing_stats.excluded_sample_count = 0xFFFFFFFFu;
    z->firing_stats.duration_s = 0xFFFFFFFFu;
    (void)zi;
}

static void fill_worst_case_status(profile_exec_status_t *st)
{
    memset(st, 0, sizeof(*st));
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        fill_worst_case_zone(&st->zones[zi], zi);
    }
}

/* True iff `json` is a syntactically complete, balanced JSON object: starts
 * with '{', ends with '}', and every '[' has a matching ']' before the
 * final '}' -- cheap enough for a host test, and exactly the property a
 * silent truncation breaks (an unterminated array followed by a bare '}'
 * fails this: brace balance goes wrong, or the string doesn't end on '}'
 * with balance restored). Not a full JSON parser -- doesn't need to be, the
 * defect class here is truncation, not malformed values. */
static bool json_looks_complete(const char *s)
{
    size_t len = strlen(s);
    if (len == 0 || s[0] != '{' || s[len - 1] != '}') return false;
    int depth = 0;
    bool in_string = false;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (in_string) {
            if (c == '\\') { i++; continue; } /* skip the escaped char */
            if (c == '"') in_string = false;
            continue;
        }
        if (c == '"') { in_string = true; continue; }
        if (c == '{' || c == '[') depth++;
        else if (c == '}' || c == ']') {
            depth--;
            if (depth < 0) return false;
        }
    }
    return !in_string && depth == 0;
}

static void test_control_status_json_is_complete_and_well_formed_at_3_zones(void)
{
    TEST_SECTION("append_zone_status_json(control_fields=true) -- the /api/control shape at 3 "
                 "worst-case-width active zones must produce a COMPLETE, balanced JSON object "
                 "containing both ff_hold_used_matrix and ff_hold_infeasible -- not a silently "
                 "truncated one");

    profile_exec_status_t st;
    fill_worst_case_status(&st);

    /* Reproduces control_status_get_handler() (dashboard_http.c) exactly:
     * same buffer size MACRO (item 3, firmware cleanup pass -- this used to be
     * a hardcoded duplicate of the handler's size expression, so a revert of
     * the handler's buffer would leave this test green; now both the handler
     * and this test reference DASHBOARD_JSON_CONTROL_BUF_SIZE, defined once in
     * dashboard_json.h), same fixed header, same trailing '}'. */
    char json[DASHBOARD_JSON_CONTROL_BUF_SIZE];
    int n = snprintf(json, sizeof(json),
        "{\"state\":\"%s\",\"zone_mask\":%u,\"target_c\":%.2f,\"ramp_lock_held\":%s,"
        "\"ramp_lock_lagging_mask\":%u,",
        "running", 255, -1234.56, "false", 255);
    size_t o = (n < 0 || (size_t)n >= sizeof(json)) ? sizeof(json) - 1 : (size_t)n;
    o = append_zone_status_json(json, sizeof(json), o, &st, true);
    if (o + 1 < sizeof(json)) json[o++] = '}';
    json[o < sizeof(json) ? o : sizeof(json) - 1] = '\0';

    TEST_CHECK(json_looks_complete(json), "the rendered /api/control JSON must be a complete, "
              "brace/bracket-balanced object -- a truncated 'zones' array followed by the appended "
              "'}' fails this check");
    TEST_CHECK(strstr(json, "\"ff_hold_used_matrix\"") != NULL, "must contain ff_hold_used_matrix "
              "somewhere -- the whole point of wiring it through");
    TEST_CHECK(strstr(json, "\"ff_hold_infeasible\"") != NULL, "must contain ff_hold_infeasible");
    TEST_CHECK(strstr(json, "\"ff_membership_change_count\"") != NULL, "must contain ff_membership_change_count");
    /* All 3 zones must actually be present, not just zone 0 before a bail. */
    int zone_objects = 0;
    for (const char *p = json; (p = strstr(p, "\"zone\":")) != NULL; p += 7) zone_objects++;
    TEST_CHECK(zone_objects == 3, "all 3 active zones must be present in the array, not truncated "
              "partway through");
}

static void test_exec_status_json_is_complete_and_well_formed_at_3_zones(void)
{
    TEST_SECTION("append_zone_status_json(control_fields=false) -- the /api/profile_exec shape at "
                 "3 worst-case-width active zones must also be complete and contain both new keys "
                 "(reviewer's own note: this one was already sized correctly, ~1753B against 1920B "
                 "-- checked here in the same test so a future field addition is caught for BOTH "
                 "shapes, not just the one that broke this time)");

    profile_exec_status_t st;
    fill_worst_case_status(&st);

    /* Reproduces profile_exec_status_get_handler()'s zones-array portion --
     * that handler's full header includes run-level/last_run fields this
     * test does not need to reproduce; the truncation risk this fix cares
     * about is entirely inside append_zone_status_json() and its buffer,
     * which this exercises directly at the same per-zone budget, now shared
     * via DASHBOARD_JSON_PROFILE_EXEC_BUF_SIZE (dashboard_json.h) rather than
     * a hardcoded duplicate of dashboard_http.c's size expression (item 3,
     * firmware cleanup pass). */
    char json[DASHBOARD_JSON_PROFILE_EXEC_BUF_SIZE];
    int n = snprintf(json, sizeof(json), "{");
    size_t o = (size_t)n;
    o = append_zone_status_json(json, sizeof(json), o, &st, false);
    if (o + 1 < sizeof(json)) json[o++] = '}';
    json[o < sizeof(json) ? o : sizeof(json) - 1] = '\0';

    TEST_CHECK(json_looks_complete(json), "the rendered /api/profile_exec JSON must be complete");
    TEST_CHECK(strstr(json, "\"ff_hold_used_matrix\"") != NULL, "must contain ff_hold_used_matrix");
    TEST_CHECK(strstr(json, "\"ff_hold_infeasible\"") != NULL, "must contain ff_hold_infeasible");
    TEST_CHECK(strstr(json, "\"ff_membership_change_count\"") != NULL, "must contain ff_membership_change_count");
    int zone_objects = 0;
    for (const char *p = json; (p = strstr(p, "\"zone\":")) != NULL; p += 7) zone_objects++;
    TEST_CHECK(zone_objects == 3, "all 3 active zones must be present");

    /* PID_EXPANSION_PLAN.md Phase 7a dashboard wiring: firing_stats must be
     * nested inside EVERY zone object, not just appended once at the end --
     * a naive implementation that only formats the last zone's stats (or
     * drops them on truncation) would still pass the "somewhere in the
     * string" check strstr() alone gives. */
    int stats_objects = 0;
    for (const char *p = json; (p = strstr(p, "\"firing_stats\":{")) != NULL; p += 16) stats_objects++;
    TEST_CHECK(stats_objects == 3, "all 3 zones must carry their own nested firing_stats object");
    TEST_CHECK(strstr(json, "\"excluded_sample_count\"") != NULL,
              "excluded_sample_count must be present -- the untrustworthy-run signal an operator "
              "needs to see, not just an internal accumulator");
    TEST_CHECK(strstr(json, "\"iae_normalized\"") != NULL, "must contain iae_normalized");
    TEST_CHECK(strstr(json, "\"mean_error_c\"") != NULL, "must contain mean_error_c");
}

/* dashboard_format_firing_history_json() -- GET /api/firing_history's body.
 * Same worst-case-render-must-stay-complete discipline as the two tests
 * above, at PROFILE_EXECUTOR_FIRING_HISTORY_DEPTH records x
 * MAX31856_CHANNEL_COUNT active zones each, exercising the real production
 * function (not a reimplementation), same convention as every other test in
 * this file. */
static void fill_worst_case_run_record(profile_firing_run_record_t *rec, uint8_t index)
{
    memset(rec, 0, sizeof(*rec));
    rec->profile_id = index;
    memset(rec->profile_name, 'A', sizeof(rec->profile_name) - 1);
    rec->profile_name[sizeof(rec->profile_name) - 1] = '\0';
    rec->run_started_unix_s = 0xFFFFFFFFu;
    rec->duration_s = 0xFFFFFFFFu;
    rec->zone_mask = 0xFFu;
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        profile_firing_zone_record_t *zr = &rec->zones[zi];
        zr->active = true;
        zr->kp = -12.34567f; zr->ki = -12.34567f; zr->kd = -12.34567f; /* "%.5f" worst case */
        profile_exec_zone_status_t tmp;
        fill_worst_case_zone(&tmp, zi);
        zr->stats = tmp.firing_stats;
    }
}

static void test_firing_history_json_is_complete_and_well_formed_at_full_depth(void)
{
    TEST_SECTION("dashboard_format_firing_history_json() -- PROFILE_EXECUTOR_FIRING_HISTORY_DEPTH "
                 "worst-case-width records at 3 active zones each must produce a COMPLETE, balanced "
                 "JSON document, every record and every zone present");

    profile_firing_run_record_t records[PROFILE_EXECUTOR_FIRING_HISTORY_DEPTH];
    for (size_t i = 0; i < PROFILE_EXECUTOR_FIRING_HISTORY_DEPTH; i++) {
        fill_worst_case_run_record(&records[i], (uint8_t)i);
    }

    const size_t json_cap = 64 + PROFILE_EXECUTOR_FIRING_HISTORY_DEPTH * (160 + MAX31856_CHANNEL_COUNT * 600);
    char *json = malloc(json_cap);
    TEST_CHECK(json != NULL, "malloc must succeed on a host with plenty of heap");
    if (json == NULL) return;

    size_t o = dashboard_format_firing_history_json(json, json_cap, 7, records,
                                                     PROFILE_EXECUTOR_FIRING_HISTORY_DEPTH);
    TEST_CHECK(o == strlen(json), "the returned offset must match the actual rendered length");
    TEST_CHECK(json_looks_complete(json), "the rendered /api/firing_history JSON must be a complete, "
              "balanced object");

    int record_objects = 0;
    for (const char *p = json; (p = strstr(p, "\"profile_name\":")) != NULL; p += 15) record_objects++;
    TEST_CHECK(record_objects == PROFILE_EXECUTOR_FIRING_HISTORY_DEPTH,
              "all 5 records must be present, not truncated partway through");

    int zone_objects = 0;
    for (const char *p = json; (p = strstr(p, "\"zone\":")) != NULL; p += 7) zone_objects++;
    TEST_CHECK(zone_objects == PROFILE_EXECUTOR_FIRING_HISTORY_DEPTH * MAX31856_CHANNEL_COUNT,
              "every zone of every record must be present (5 records x 3 zones)");

    TEST_CHECK(strstr(json, "\"kp\":-12.34567") != NULL, "kp must round-trip at its worst-case width");
    TEST_CHECK(strstr(json, "\"excluded_sample_count\"") != NULL, "excluded_sample_count must be present here too");

    free(json);

    /* Sanity: an empty history (never-run profile) must still be a valid,
     * complete document with an empty records array -- not an error, not a
     * truncated one. */
    char empty_json[64];
    size_t eo = dashboard_format_firing_history_json(empty_json, sizeof(empty_json), 9, NULL, 0);
    TEST_CHECK(json_looks_complete(empty_json), "zero records must still render a complete JSON object");
    TEST_CHECK(strstr(empty_json, "\"records\":[]") != NULL, "zero records must render an empty array, "
              "not an open one");
    (void)eo;

    /* Truncation case: a buffer far too small for even one worst-case
     * record must still close every array/object it opened. */
    char tiny[80];
    size_t to = dashboard_format_firing_history_json(tiny, sizeof(tiny), 3, records, 1);
    TEST_CHECK(json_looks_complete(tiny), "a truncated firing_history render must still be complete, "
              "balanced JSON -- the same truncation discipline append_zone_status_json() already has");
    (void)to;
}

static void test_json_escape_doubles_every_quote_and_backslash(void)
{
    TEST_SECTION("json_escape() -- sanity: every '\"'/'\\\\' is escaped, output always NUL-terminated, "
                 "truncates rather than overflows a too-small destination");
    char out[8];
    json_escape("ab\"c", out, sizeof(out));
    TEST_CHECK(strcmp(out, "ab\\\"c") == 0, "a single quote must become \\\"");
    char tiny[3];
    json_escape("\"\"\"\"\"", tiny, sizeof(tiny)); /* must not overflow a 3-byte destination */
    TEST_CHECK(strlen(tiny) < sizeof(tiny), "must truncate, never overflow, a too-small buffer");
}

/* Item 2, firmware cleanup pass: truncation used to bail silently -- the
 * array was left open, and the caller's own trailing '}' turned that into
 * unparseable JSON served with HTTP 200 and no error anywhere. Proves the
 * fix two ways: (1) the rendered document is still complete/balanced even
 * when the buffer is deliberately far too small to hold every zone, and
 * (2) the truncation is actually SIGNALLED (ESP_LOGE, counted here via
 * stubs/esp_log.h's g_esp_loge_calls spy) rather than silent. Negative-tested
 * by hand against a reverted copy of append_zone_status_json() (the "return o"
 * bail with no goto/log) before this was written into the file -- see the
 * firmware cleanup report for the quoted FAIL output; not re-run here since
 * this file always exercises the current, fixed production function. */
static void test_truncation_is_logged_and_still_produces_valid_json(void)
{
    TEST_SECTION("append_zone_status_json() -- a buffer far too small for even one zone must still "
                 "yield complete, balanced JSON (the array gets closed even on truncation) AND must "
                 "signal the truncation via ESP_LOGE, not fail silently");

    profile_exec_status_t st;
    fill_worst_case_status(&st);

    /* Deliberately tiny: room for the "{" header and "\"zones\":[" but not
     * even one full worst-case zone object. */
    char json[48];
    int n = snprintf(json, sizeof(json), "{");
    size_t o = (size_t)n;
    g_esp_loge_calls = 0;
    o = append_zone_status_json(json, sizeof(json), o, &st, false);
    if (o + 1 < sizeof(json)) json[o++] = '}';
    json[o < sizeof(json) ? o : sizeof(json) - 1] = '\0';

    TEST_CHECK(json_looks_complete(json), "a truncated render must still be complete/balanced JSON, "
              "not an unterminated 'zones' array followed by a bare '}'");
    TEST_CHECK(g_esp_loge_calls >= 1, "truncation must be logged (ESP_LOGE), not silent -- the exact "
              "defect this fix removes: HTTP 200 with invalid JSON and no error anywhere");

    /* Sanity: this buffer really did truncate (didn't just happen to fit) --
     * otherwise the checks above would be vacuous. */
    int zone_objects = 0;
    for (const char *p = json; (p = strstr(p, "\"zone\":")) != NULL; p += 7) zone_objects++;
    TEST_CHECK(zone_objects < 3, "test setup sanity: the buffer must actually be too small to hold "
              "all 3 worst-case zones, or this test cannot tell the fix from a no-op");
}

/* Coordinator review, 2026-08-31: autotune_matrix_get_handler()'s RGA block
 * used to do `o += snprintf(json+o, json_cap-o, ...)` with no re-clamp after
 * the cells loop's single clamp -- snprintf returns the WOULD-BE length even
 * when truncated, so a long enough chain of appends can walk `o` past
 * `json_cap`, and the next call's `json_cap - o` then wraps a size_t into a
 * huge value and writes out of bounds. Unreachable at today's
 * MAX31856_CHANNEL_COUNT (json_cap comfortably covers the RGA block's real
 * output), but that handler lives in dashboard_http.c, which cannot compile
 * on the host (see this file's own header comment) -- so the fix moved into
 * json_append_clamped() (dashboard_json.c), the ONE place both the handler
 * and this test call, and this test proves the general property directly:
 * an arbitrarily long chain of appends into a deliberately tiny buffer can
 * NEVER push the returned offset past `cap - 1`, however many more calls a
 * future channel-count/zone-count increase adds. A one-byte canary placed
 * immediately after the buffer detects the exact out-of-bounds write the
 * old unclamped pattern could produce; if this regresses to the unclamped
 * pattern, the canary gets overwritten and this test fails. */
static void test_json_append_clamped_never_walks_past_cap(void)
{
    TEST_SECTION("json_append_clamped() -- a long chain of appends into a too-small buffer must never "
                 "push the offset past cap-1, however many calls follow (the RGA-block heap-overflow "
                 "class, generalized past today's MAX31856_CHANNEL_COUNT)");

    /* +1 canary byte the real buffer must never reach, whatever `o` does. */
    char buf[17];
    buf[16] = (char)0xAA;
    const size_t cap = 16;

    size_t o = 0;
    o = json_append_clamped(buf, cap, o, "{\"zone_count\":%u,\"cells\":[", 3u);
    TEST_CHECK(o <= cap - 1, "offset must never exceed cap-1 after the very first oversized append");

    /* Simulate a MUCH larger n/zone_count than today's 3 -- e.g. an 8-zone
     * board -- by chaining far more appends than this 16-byte buffer could
     * ever hold, exactly the shape autotune_matrix_get_handler's cells/RGA
     * loops produce. */
    for (int i = 0; i < 200; i++) {
        o = json_append_clamped(buf, cap, o, "{\"i\":%u,\"j\":%u,\"valid\":true,\"k\":%.3f}", i, i,
                                (double)i);
        TEST_CHECK(o <= cap - 1, "offset must stay <= cap-1 after every single append in a long chain, "
                  "not just the first or last one");
    }
    o = json_append_clamped(buf, cap, o, "]}");
    TEST_CHECK(o <= cap - 1, "the final append must also respect the clamp");
    TEST_CHECK(buf[16] == (char)0xAA, "the canary byte immediately past the buffer must be untouched -- "
              "this is the exact byte the old unclamped `o += snprintf(...)` pattern could corrupt once "
              "`cap - o` wrapped");
}

/* dashboard_http.c's httpd_worker stack-overflow fix (64B free of 8192
 * measured live, 2026-08-31): profile_exec_status_get_handler() and
 * control_status_get_handler() now malloc() DASHBOARD_JSON_PROFILE_EXEC_
 * BUF_SIZE / DASHBOARD_JSON_CONTROL_BUF_SIZE off the HEAP instead of
 * declaring `char json[...]` on the stack -- same buffer size, same macro,
 * only WHERE the bytes live changed. This proves that move didn't
 * accidentally shrink the usable capacity (an off-by-one in a malloc(cap)
 * vs. a `char json[cap]` would show up here as the render silently no
 * longer fitting all 3 worst-case zones) and that a malloc'd buffer renders
 * byte-for-byte the same complete, balanced JSON the stack version did. */
static void test_heap_allocated_worst_case_render_matches_stack_sizing(void)
{
    TEST_SECTION("malloc(DASHBOARD_JSON_PROFILE_EXEC_BUF_SIZE)/malloc(DASHBOARD_JSON_CONTROL_BUF_SIZE) "
                 "-- the heap-allocated buffers the real handlers now use must still hold the full "
                 "3-zone worst case, exactly like the stack-sized test above");

    profile_exec_status_t st;
    fill_worst_case_status(&st);

    char *pe_json = malloc(DASHBOARD_JSON_PROFILE_EXEC_BUF_SIZE);
    TEST_CHECK(pe_json != NULL, "malloc(DASHBOARD_JSON_PROFILE_EXEC_BUF_SIZE) must succeed on a host "
              "with plenty of heap -- a NULL here would be a test-environment problem, not the fix");
    if (pe_json != NULL) {
        int n = snprintf(pe_json, DASHBOARD_JSON_PROFILE_EXEC_BUF_SIZE, "{");
        size_t o = (size_t)n;
        o = append_zone_status_json(pe_json, DASHBOARD_JSON_PROFILE_EXEC_BUF_SIZE, o, &st, false);
        if (o + 1 < DASHBOARD_JSON_PROFILE_EXEC_BUF_SIZE) pe_json[o++] = '}';
        pe_json[o < DASHBOARD_JSON_PROFILE_EXEC_BUF_SIZE ? o : DASHBOARD_JSON_PROFILE_EXEC_BUF_SIZE - 1] = '\0';
        TEST_CHECK(json_looks_complete(pe_json), "the heap-rendered /api/profile_exec JSON must be complete");
        int zone_objects = 0;
        for (const char *p = pe_json; (p = strstr(p, "\"zone\":")) != NULL; p += 7) zone_objects++;
        TEST_CHECK(zone_objects == 3, "all 3 active zones must fit in the heap buffer, same as the "
                  "stack-sized version");
        free(pe_json);
    }

    char *ctl_json = malloc(DASHBOARD_JSON_CONTROL_BUF_SIZE);
    TEST_CHECK(ctl_json != NULL, "malloc(DASHBOARD_JSON_CONTROL_BUF_SIZE) must succeed on a host "
              "with plenty of heap");
    if (ctl_json != NULL) {
        int n = snprintf(ctl_json, DASHBOARD_JSON_CONTROL_BUF_SIZE,
            "{\"state\":\"%s\",\"zone_mask\":%u,\"target_c\":%.2f,\"ramp_lock_held\":%s,"
            "\"ramp_lock_lagging_mask\":%u,",
            "running", 255, -1234.56, "false", 255);
        size_t o = (n < 0 || (size_t)n >= DASHBOARD_JSON_CONTROL_BUF_SIZE)
                       ? DASHBOARD_JSON_CONTROL_BUF_SIZE - 1
                       : (size_t)n;
        o = append_zone_status_json(ctl_json, DASHBOARD_JSON_CONTROL_BUF_SIZE, o, &st, true);
        if (o + 1 < DASHBOARD_JSON_CONTROL_BUF_SIZE) ctl_json[o++] = '}';
        ctl_json[o < DASHBOARD_JSON_CONTROL_BUF_SIZE ? o : DASHBOARD_JSON_CONTROL_BUF_SIZE - 1] = '\0';
        TEST_CHECK(json_looks_complete(ctl_json), "the heap-rendered /api/control JSON must be complete");
        int zone_objects = 0;
        for (const char *p = ctl_json; (p = strstr(p, "\"zone\":")) != NULL; p += 7) zone_objects++;
        TEST_CHECK(zone_objects == 3, "all 3 active zones must fit in the heap buffer");
        free(ctl_json);
    }
}

// Task B: the probe and identify STEPPING sub-phases were previously
// indistinguishable on the wire -- both report state=="stepping" (see
// autotune_state_name() above, deliberately unchanged: PcTools and this
// page's own captured .jsonl fixtures match that string literally). The new
// "sub_phase" field is what a client uses instead. Proves all three cases:
// probing, identifying, and "no sub-phase" (a plain/relay run, or any state
// other than STEPPING) -- the last one guards against a field that reads
// "probe" by leftover memory rather than genuine absence.
static void test_autotune_status_json_reports_sub_phase_distinctly(void)
{
    TEST_SECTION("dashboard_format_autotune_status_json() -- \"sub_phase\" distinguishes target mode's "
                 "probe and identify STEPPING sub-phases, without touching the wire-compatible \"state\" "
                 "string");
    autotune_engine_status_t st;
    char json[2048];

    memset(&st, 0, sizeof(st));
    st.state = AUTOTUNE_ENGINE_STEPPING;
    st.target_mode = true;
    st.probe_phase = true;
    dashboard_format_autotune_status_json(json, sizeof(json), &st);
    TEST_CHECK(strstr(json, "\"state\":\"stepping\"") != NULL, "state must still read \"stepping\" -- "
              "wire compatibility with PcTools/main_page.html is not touched");
    TEST_CHECK(strstr(json, "\"sub_phase\":\"probe\"") != NULL, "PHASE 1 (probe) must report sub_phase=probe");

    memset(&st, 0, sizeof(st));
    st.state = AUTOTUNE_ENGINE_STEPPING;
    st.target_mode = true;
    st.probe_phase = false;
    dashboard_format_autotune_status_json(json, sizeof(json), &st);
    TEST_CHECK(strstr(json, "\"sub_phase\":\"identify\"") != NULL,
              "PHASE 2 (identify) must report sub_phase=identify, distinctly from probe");

    // Negative control: a plain (non-target-mode) STEPPING run has no
    // sub-phase at all -- must read "", not stale/garbage "probe" or
    // "identify" from probe_phase's zero-initialized value happening to
    // match one of the two branches by accident.
    memset(&st, 0, sizeof(st));
    st.state = AUTOTUNE_ENGINE_STEPPING;
    st.target_mode = false;
    st.probe_phase = false;
    dashboard_format_autotune_status_json(json, sizeof(json), &st);
    TEST_CHECK(strstr(json, "\"sub_phase\":\"\"") != NULL,
              "a plain duty-based run must report sub_phase as an empty string, not a stale probe/identify "
              "label");

    // Negative control: target_mode still true, but no longer STEPPING
    // (e.g. DONE) -- sub_phase must clear even though target_mode/probe_
    // phase fields could still carry meaningful values for other purposes.
    memset(&st, 0, sizeof(st));
    st.state = AUTOTUNE_ENGINE_DONE;
    st.target_mode = true;
    st.probe_phase = false;
    dashboard_format_autotune_status_json(json, sizeof(json), &st);
    TEST_CHECK(strstr(json, "\"sub_phase\":\"\"") != NULL,
              "sub_phase must read empty outside STEPPING, even for a target-mode run that just finished");
}

static void run_test_dashboard_json(void)
{
    test_json_escape_doubles_every_quote_and_backslash();
    test_control_status_json_is_complete_and_well_formed_at_3_zones();
    test_exec_status_json_is_complete_and_well_formed_at_3_zones();
    test_truncation_is_logged_and_still_produces_valid_json();
    test_json_append_clamped_never_walks_past_cap();
    test_heap_allocated_worst_case_render_matches_stack_sizing();
    test_firing_history_json_is_complete_and_well_formed_at_full_depth();
    test_autotune_status_json_reports_sub_phase_distinctly();
}

int main(void)
{
    run_test_dashboard_json();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
