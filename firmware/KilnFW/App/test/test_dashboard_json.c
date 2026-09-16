// Host tests for App/drivers/http/dashboard_json.c -- json_escape()/
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
#include "../drivers/persist/relay_cycles.h" /* RELAY_CYCLES_COUNT -- relay_life mirror below */

int g_test_failures = 0;
int g_test_count = 0;

#include "../drivers/http/dashboard_json.c"
#include "../drivers/safety/safety_trip_words.h"

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
    /* ROADMAP.md M15 B4: zone_duty_breakdown_t worst case -- every field at
     * the same -1234.xxxx magnitude used throughout this helper, matching
     * dashboard_json.h's DASHBOARD_JSON_CONTROL_BUF_SIZE sizing comment. */
    z->duty_breakdown.ff_hold = -1234.5678f;
    z->duty_breakdown.ff_climb = -1234.5678f;
    z->duty_breakdown.coupling_correction = -1234.5678f;
    z->duty_breakdown.ff_rate_pretaper_c_per_s = -1234.56789f;
    z->duty_breakdown.ff_rate_posttaper_c_per_s = -1234.56789f;
    z->duty_breakdown.kp_effective = -1234.56789f;
    z->duty_breakdown.ki_effective = -1234.56789f;
    z->duty_breakdown.kd_effective = -1234.56789f;
    z->duty_breakdown.post_clamp_total = -1234.5678f;
    z->duty_breakdown.load_cap_boost = -1234.5678f;
    z->duty_breakdown.final_commanded = -1234.5678f;
    /* PID_EXPANSION_PLAN.md sec 7.1/7.4: sustained-lag reporting worst case. */
    z->ramp_lag_sustained = true;
    z->ramp_lag_held_s = -1234.56f;
    z->ramp_lag_commanded_rate_c_per_hr = -1234.56f;
    z->ramp_lag_achieved_rate_c_per_hr = -1234.56f;
    /* PID_EXPANSION_PLAN.md sec 7.3: dwell credit worst case. */
    z->ramp_dwell_credit_s = -1234.56f;
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
    /* docs/audits/iter_tune_decision_2026-09-07.md prep: start_temp_c is
     * live-only (profile_exec_zone_status_t, NOT profile_exec_firing_
     * stats_t -- see that field's own comment for why), same worst-case
     * magnitude convention as the rest of this helper. */
    z->start_temp_c = -1234.56f;
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
    TEST_CHECK(strstr(json, "\"bd_ff_hold\"") != NULL, "must contain the M15 B4 duty breakdown's bd_ff_hold");
    TEST_CHECK(strstr(json, "\"bd_coupling_correction\"") != NULL, "must contain bd_coupling_correction");
    TEST_CHECK(strstr(json, "\"bd_pre_clamp_total\"") != NULL, "must contain the derived bd_pre_clamp_total");
    TEST_CHECK(strstr(json, "\"bd_load_cap_boost\"") != NULL, "must contain bd_load_cap_boost -- previously "
              "invisible off-board entirely");
    TEST_CHECK(strstr(json, "\"bd_final_commanded\"") != NULL, "must contain bd_final_commanded");
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

    /* docs/audits/iter_tune_decision_2026-09-07.md prep: start_temp_c is
     * live-only, present in every zone object, same "not just once at the
     * end" discipline as firing_stats above. */
    int start_temp_objects = 0;
    for (const char *p = json; (p = strstr(p, "\"start_temp_c\":")) != NULL; p += 15) start_temp_objects++;
    TEST_CHECK(start_temp_objects == 3, "all 3 zones must carry start_temp_c");

    /* PID_EXPANSION_PLAN.md sec 7.1/7.4: sustained-lag fields must survive
     * the same worst-case 3-zone render, present in every zone object. */
    int lag_objects = 0;
    for (const char *p = json; (p = strstr(p, "\"ramp_lag_sustained\":")) != NULL; p += 22) lag_objects++;
    TEST_CHECK(lag_objects == 3, "all 3 zones must carry ramp_lag_sustained");
    TEST_CHECK(strstr(json, "\"ramp_lag_held_s\"") != NULL, "must contain ramp_lag_held_s");
    TEST_CHECK(strstr(json, "\"ramp_lag_commanded_rate_c_per_hr\"") != NULL,
              "must contain ramp_lag_commanded_rate_c_per_hr");
    TEST_CHECK(strstr(json, "\"ramp_lag_achieved_rate_c_per_hr\"") != NULL,
              "must contain ramp_lag_achieved_rate_c_per_hr");

    /* PID_EXPANSION_PLAN.md sec 7.3: dwell credit must survive the same
     * worst-case render, present in every zone object. */
    int dwell_credit_objects = 0;
    for (const char *p = json; (p = strstr(p, "\"ramp_dwell_credit_s\":")) != NULL; p += 22) dwell_credit_objects++;
    TEST_CHECK(dwell_credit_objects == 3, "all 3 zones must carry ramp_dwell_credit_s");
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

// ---------------------------------------------------------------------------
// GET /api/status worst-case render -- opus review, "the ~100B headroom
// figure everyone has been reasoning from was a one-off manual measurement,
// not a check". status_get_handler() itself (dashboard_http.c) cannot be
// host-tested directly (that file #includes lvgl_port.h at file scope --
// see this file's own top comment / dashboard_json.h's DASHBOARD_JSON_
// STATUS_BUF_SIZE comment), so this mirrors its APPEND sequence field by
// field, using the SAME literal format strings dashboard_http.c's
// status_get_handler() uses (copy them back in sync by hand if that
// handler's field list ever changes -- there is no other seam) and, where
// the handler calls a real shared helper (json_escape(), safety_trip_
// words_short/_cause/_remedy/_cause_numbered(), safety_fault_source_
// words()), THIS test calls that same real, production function rather
// than reimplementing its output -- only the plain %-format fields (counter
// widths, bool literals) are hand-picked worst-case literals, matching this
// project's own documented sizing method (dashboard_http.c's
// status_get_handler() doc comment: "a standalone harness mirroring this
// file's own APPEND macro against every field above at its documented
// worst width").
//
// worst_of_all_reasons() finds the actual longest output any of these
// functions can produce over every reason code, rather than hand-counting
// characters in a table entry that can silently grow past whatever was
// counted by hand.
static uint8_t worst_of_all_reasons(const char *(*fn)(uint8_t), size_t *out_len)
{
    uint8_t best = 0;
    size_t best_len = 0;
    for (int r = 0; r <= 255; r++) {
        size_t l = strlen(fn((uint8_t)r));
        if (l > best_len) { best_len = l; best = (uint8_t)r; }
    }
    if (out_len) *out_len = best_len;
    return best;
}

#define STATUS_APPEND(...)                                                                          \
    do {                                                                                             \
        int n_ = snprintf(json + o, cap - o, __VA_ARGS__);                                           \
        if (n_ < 0 || (size_t)n_ >= cap - o) { return false; }                                        \
        o += (size_t)n_;                                                                              \
    } while (0)

/* Renders the worst-case /api/status body into json[cap]. Returns false
 * (mirroring status_get_handler()'s own `goto truncated`) the instant any
 * one field would not fit -- never writes a partial/invalid document past
 * that point, same discipline the real handler has. `extra_field` appends
 * one more plausible field (same shape as the flush_last_us/flush_max_us/
 * flush_count trio DISPLAY_ST7796_PLAN.md 9.1 actually added) AFTER every
 * other field, to prove the field-creep failure mode: a real field added
 * without re-running this sizing check. */
static bool render_worst_case_status_json(char *json, size_t cap, size_t channel_count,
                                          bool extra_field, size_t *out_len)
{
    size_t o = 0;

    STATUS_APPEND("{\"io_ready\":%s", "true");

    /* relays: KILN_IO_RELAY_COUNT objects, "false" (5 chars) is wider than
     * "true" (4) for the worst case. */
    STATUS_APPEND(",\"relays\":[");
    for (uint8_t relay = 1; relay <= KILN_IO_RELAY_COUNT; relay++) {
        STATUS_APPEND("%s{\"relay\":%u,\"on\":%s}", relay == 1 ? "" : ",", relay, "false");
    }
    STATUS_APPEND("]");
    STATUS_APPEND(",\"io_read_failed\":true");

    STATUS_APPEND(",\"relay_cycles\":[");
    for (uint8_t r = 0; r < KILN_IO_RELAY_COUNT; r++) {
        STATUS_APPEND("%s%lu", r == 0 ? "" : ",", (unsigned long)0xFFFFFFFFu);
    }
    STATUS_APPEND("]");

    /* RELAY_LIFE_BUDGET.md -- opus review finding: this mirror
     * had never been updated for relay_life[]/relay_life_tier, so the
     * headroom assertion below was vacuous. Widths match dashboard_json.h's
     * DASHBOARD_JSON_STATUS_BUF_SIZE comment's own worst-case entry: type
     * "contactor" (9 chars, widest of contactor/mercury/ssr), cycles/rated
     * at uint32 widest, percent at the widest %.2f this codebase's own
     * relay_cycles_budget() can currently produce through relay_cycles_
     * set_type()'s only live caller (rated_override always 0, so rated comes
     * from the type table -- RELAY_RATED_LIFE_CONTACTOR=100000 -- giving
     * cycles=4294967295 a percent of ~4294967.30; rated is still rendered at
     * its full uint32 width below since relay_cycles_set_type()'s signature
     * allows a nonzero override even though nothing calls it with one today,
     * so this stays a real bound rather than one tied to today's callers),
     * tier "error" (5 chars, widest of none/warn/error). */
    STATUS_APPEND(",\"relay_life\":[");
    for (uint8_t r = 0; r < RELAY_CYCLES_COUNT; r++) {
        STATUS_APPEND(
            "%s{\"relay\":%u,\"type\":\"%s\",\"cycles\":%lu,\"rated\":%s,\"percent\":%s,\"tier\":\"%s\"}",
            r == 0 ? "" : ",", 4u, "contactor", (unsigned long)0xFFFFFFFFu, "4294967295",
            "4294967.30", "error");
    }
    STATUS_APPEND("]");
    STATUS_APPEND(",\"relay_life_tier\":\"%s\"", "error");

    STATUS_APPEND(",\"thermo_ready\":%s", "true");
    STATUS_APPEND(",\"thermo_spi_wedged\":%s", "true");
    STATUS_APPEND(",\"flush_last_us\":%u,\"flush_max_us\":%u,\"flush_count\":%u",
                  0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu);

    STATUS_APPEND(",\"channels\":[");
    for (size_t i = 0; i < channel_count; i++) {
        STATUS_APPEND(
            "%s{\"channel\":%u,\"temp_c\":%s,\"cj_c\":%s,\"valid\":%s,\"fault_status\":%u,"
            "\"spi_failed\":%s,\"stale\":%s,\"age_ms\":%s}",
            i == 0 ? "" : ",", 255u, "-1234.56", "-1234.56", "false", 255u, "false", "false",
            "4294967295");
    }
    STATUS_APPEND("]");

    STATUS_APPEND(",\"safety_ready\":%s", "false");
    STATUS_APPEND(",\"zones_config_valid\":%s", "false");

    STATUS_APPEND(",\"safety_temp_c\":%.2f", -1234.56);
    /* Emitted UNCONDITIONALLY by dashboard_status_http.c and missing from this
     * mirror entirely until the 2026-09-16 review -- "false" is the wider of
     * the two boolean renderings, so it is the worst case. */
    STATUS_APPEND(",\"safety_tc_is_separate_sensor\":%s", "false");
    STATUS_APPEND(",\"enclosure_temp_c\":%.2f", -1234.56);
    STATUS_APPEND(",\"power_w\":%.1f", -1234.5);

    STATUS_APPEND(",\"ct_current_a\":[");
    for (unsigned ci = 0; ci < 3; ci++) {
        STATUS_APPEND("%s%.3f", ci == 0 ? "" : ",", -1234.567);
    }
    STATUS_APPEND("]");

    /* LINK_PROTOCOL.md Frame E, 2026-09-06 -- raw ADC counts, missing from
     * this mirror entirely until the opus review caught it. uint16_t widest
     * is 65535 (5 digits), 3 channels comma-joined. */
    STATUS_APPEND(",\"ct_counts\":%s", "[");
    for (unsigned ci = 0; ci < 3; ci++) {
        STATUS_APPEND("%s%u", ci == 0 ? "" : ",", 65535u);
    }
    STATUS_APPEND("]");

    /* CT_COMMISSIONING_PLAN.md step 4 -- real-amps display. "per_zone" (8
     * chars) is the worst case of the two topology strings, wider than
     * "summed" (6). */
    STATUS_APPEND(",\"ct_topology\":\"%s\"", "per_zone");
    STATUS_APPEND(",\"ct_fitted\":[");
    for (unsigned ci = 0; ci < 3; ci++) {
        STATUS_APPEND("%s%s", ci == 0 ? "" : ",", "false");
    }
    STATUS_APPEND("]");
    /* null is wider than any real zone index (0..4, one digit). */
    STATUS_APPEND(",\"ct_summed_attrib_zone\":%s", "null");

    STATUS_APPEND(",\"safety_relay_energized\":%s", "false");
    STATUS_APPEND(",\"safety_heating_enabled\":%s", "false");
    STATUS_APPEND(",\"heat_block_sources\":%lu", (unsigned long)0xFFFFFFFFu);
    {
        char hb_words[160];
        STATUS_APPEND(",\"heat_block_sources_words\":\"%s\"",
                      safety_fault_source_words(0x3Fu, hb_words, sizeof(hb_words)));
    }
    STATUS_APPEND(",\"zone_blocked_mask\":%u", 255u);

    STATUS_APPEND(",\"self_protocol_version\":%u", 65535u);
    STATUS_APPEND(",\"link_version_known\":%s", "true");
    STATUS_APPEND(",\"link_version_compatible\":%s", "false");
    STATUS_APPEND(",\"peer_protocol_version\":%u", 65535u);

    STATUS_APPEND(",\"diag_ever_received\":%s", "true");
    {
        size_t wlen;
        uint8_t reason = worst_of_all_reasons(safety_trip_words_short, &wlen);
        STATUS_APPEND(",\"diag_trip_reason\":%u", 255u);
        STATUS_APPEND(",\"diag_trip_reason_words\":\"%s\"", safety_trip_words_short(reason));
        uint8_t reason_cause = worst_of_all_reasons(safety_trip_words_cause, &wlen);
        STATUS_APPEND(",\"diag_trip_reason_cause\":\"%s\"", safety_trip_words_cause(reason_cause));
        uint8_t reason_remedy = worst_of_all_reasons(safety_trip_words_remedy, &wlen);
        STATUS_APPEND(",\"diag_trip_reason_remedy\":\"%s\"", safety_trip_words_remedy(reason_remedy));
        STATUS_APPEND(",\"diag_warn_mask\":%u", 65535u);
        STATUS_APPEND(",\"diag_trip_mask\":%u", 65535u);
        STATUS_APPEND(",\"diag_state\":%u", 255u);
        /* 2026-09-16: this mirror models the DEFAULT /api/status document.
         * diag_boot_reason and the three link frame counters
         * (diag_context_frames_ok/_bad, diag_tx_frames_dropped) are no longer
         * part of it -- they moved to the small ?diag=1 document -- and
         * diag_boot_stack_overflow/_malloc_failed/_assert_failed were removed
         * from the firmware outright. That is what bounds this document: the
         * honest render of the old field list was 5353 bytes against a
         * 5248-byte buffer. If any of them is ever served from the default
         * document again, it MUST be added back here in the same breath, or
         * this file goes back to reporting headroom the firmware does not
         * have. */
        STATUS_APPEND(",\"diag_age_ms\":%u", 65535u);
        STATUS_APPEND(",\"diag_context_age_100ms\":%u", 255u);
    }

    STATUS_APPEND(",\"trip_event_ever_received\":%s", "true");
    {
        size_t wlen;
        uint8_t reason = worst_of_all_reasons(safety_trip_words_short, &wlen);
        STATUS_APPEND(",\"trip_reason\":%u", 255u);
        STATUS_APPEND(",\"trip_reason_words\":\"%s\"", safety_trip_words_short(reason));
        {
            /* Real safety_trip_words_cause_numbered(), pathological float
             * magnitude on every numbered field it can carry (this header's
             * own comment: "%.2f of 1e38 is ~45 chars") -- brute-forced
             * over every reason code for the actual longest sentence this
             * function can produce, same worst_of_all_reasons() discipline
             * as the plain tables above. */
            char cause_buf[320];
            size_t best_len = 0;
            uint8_t best_reason = 0;
            char best_buf[320];
            for (int r = 0; r <= 255; r++) {
                const float huge3[3] = { -1e38f, -1e38f, -1e38f };
                safety_trip_words_cause_numbered((uint8_t)r, -1e38f, -1e38f, huge3, 254u,
                                                 cause_buf, sizeof(cause_buf));
                size_t l = strlen(cause_buf);
                if (l > best_len) { best_len = l; best_reason = (uint8_t)r; memcpy(best_buf, cause_buf, l + 1); }
            }
            (void)best_reason;
            STATUS_APPEND(",\"trip_reason_cause\":\"%s\"", best_buf);
        }
        uint8_t reason_remedy = worst_of_all_reasons(safety_trip_words_remedy, &wlen);
        STATUS_APPEND(",\"trip_reason_remedy\":\"%s\"", safety_trip_words_remedy(reason_remedy));
        STATUS_APPEND(",\"trip_event_age_ms\":%lu", (unsigned long)0xFFFFFFFFu);
        STATUS_APPEND(",\"trip_safety_tc_c\":%.1f", -1234.5);
        STATUS_APPEND(",\"trip_deciding_threshold\":%.1f", -1234.5);
        STATUS_APPEND(",\"trip_fault_sources\":%lu", (unsigned long)0xFFFFFFFFu);
        STATUS_APPEND(",\"trip_fault_sources_valid\":%s", "true");
        {
            char tf_words[160];
            STATUS_APPEND(",\"trip_fault_sources_words\":\"%s\"",
                          safety_fault_source_words(0x3Fu, tf_words, sizeof(tf_words)));
        }
    }

    STATUS_APPEND(",\"safety_build_known\":%s", "true");
    {
        char commit_raw[65], commit_esc[65 * 2 + 1];
        char dt_raw[33], dt_esc[33 * 2 + 1];
        memset(commit_raw, '"', sizeof(commit_raw) - 1); commit_raw[sizeof(commit_raw) - 1] = '\0';
        memset(dt_raw, '"', sizeof(dt_raw) - 1); dt_raw[sizeof(dt_raw) - 1] = '\0';
        json_escape(commit_raw, commit_esc, sizeof(commit_esc));
        json_escape(dt_raw, dt_esc, sizeof(dt_esc));
        STATUS_APPEND(",\"safety_build_dirty\":%s", "false");
        STATUS_APPEND(",\"safety_build_commit\":\"%s\"", commit_esc);
        STATUS_APPEND(",\"safety_build_datetime\":\"%s\"", dt_esc);
        STATUS_APPEND(",\"safety_config_version\":%u", 255u);
        STATUS_APPEND(",\"safety_config_crc\":%u", 65535u);
    }

    /* 2026-09-15 review follow-up (item E): MEDIUM 6 added a divergence
     * boolean AND an escaped reason string to dashboard_status_http.c
     * without adding either here, so this file'"'"'s headroom assertion had
     * stopped covering the real document -- exactly the field-creep failure
     * test_status_json_mutation_field_creep_goes_red() exists to catch,
     * missed because the mirror was not updated.
     *
     * Mirroring them revealed the document no longer fits: 5400 bytes
     * against DASHBOARD_JSON_STATUS_BUF_SIZE=5248, i.e. -152 bytes of
     * headroom, which in production is an HTTP 500 for the whole status
     * document. The handler now emits only the bare flag, under a shorter
     * key, and no reason string -- see its own comment for why the buffer
     * was not enlarged instead. "false" is the wider of the two boolean
     * renderings, so it is the worst case. */
    STATUS_APPEND(",\"safety_diverged\":%s", "false");

    STATUS_APPEND(",\"nvs_sections\":[");
    for (size_t i = 0; i < 4; i++) {
        STATUS_APPEND("%s{\"name\":\"%s\",\"present\":%s,\"mounted\":%s}", i == 0 ? "" : ",",
                      "profiles_nvs", "false", "false");
    }
    STATUS_APPEND("]");

    {
        char fwv_raw[32], fwv_esc[32 * 2 + 1];
        char fwb_raw[40], fwb_esc[40 * 2 + 1];
        memset(fwv_raw, '"', sizeof(fwv_raw) - 1); fwv_raw[sizeof(fwv_raw) - 1] = '\0';
        memset(fwb_raw, '"', sizeof(fwb_raw) - 1); fwb_raw[sizeof(fwb_raw) - 1] = '\0';
        json_escape(fwv_raw, fwv_esc, sizeof(fwv_esc));
        json_escape(fwb_raw, fwb_esc, sizeof(fwb_esc));
        STATUS_APPEND(",\"fw_version_known\":%s", "true");
        STATUS_APPEND(",\"fw_version\":\"%s\"", fwv_esc);
        STATUS_APPEND(",\"fw_build\":\"%s\"", fwb_esc);
    }
    STATUS_APPEND(",\"uptime_s\":%lu", (unsigned long)0xFFFFFFFFu);
    /* Longest of reset_reason_name()'s fixed set (dashboard_http.c) --
     * "software (esp_restart)", not escaped in the real handler either
     * (fixed internal strings, never operator/peer-supplied). */
    STATUS_APPEND(",\"reset_reason\":\"%s\"", "software (esp_restart)");
    STATUS_APPEND(",\"heap_internal\":{\"free\":%lu,\"largest_free_block\":%lu,\"min_free\":%lu,\"total\":%lu}",
                  (unsigned long)0xFFFFFFFFu, (unsigned long)0xFFFFFFFFu, (unsigned long)0xFFFFFFFFu,
                  (unsigned long)0xFFFFFFFFu);
    STATUS_APPEND(",\"heap_spiram\":{\"free\":%lu,\"largest_free_block\":%lu,\"min_free\":%lu,\"total\":%lu}",
                  (unsigned long)0xFFFFFFFFu, (unsigned long)0xFFFFFFFFu, (unsigned long)0xFFFFFFFFu,
                  (unsigned long)0xFFFFFFFFu);
    STATUS_APPEND(",\"heap_dma\":{\"free\":%lu,\"largest_free_block\":%lu,\"min_free\":%lu,\"total\":%lu}",
                  (unsigned long)0xFFFFFFFFu, (unsigned long)0xFFFFFFFFu, (unsigned long)0xFFFFFFFFu,
                  (unsigned long)0xFFFFFFFFu);

    STATUS_APPEND(",\"flash_size\":%lu", (unsigned long)0xFFFFFFFFu);
    STATUS_APPEND(",\"flash_partition_size\":%lu", (unsigned long)0xFFFFFFFFu);
    STATUS_APPEND(",\"flash_used\":%lu", (unsigned long)0xFFFFFFFFu);

    STATUS_APPEND(",\"temp_unit\":\"%s\"", "F");

    /* 2026-09-02, forthcoming "ramp assist" feature: worst case is "false" (5
     * bytes) not "true" (4) -- mirrors dashboard_http.c's own APPEND exactly. */
    STATUS_APPEND(",\"ramp_assist_enabled\":%s", "false");

    STATUS_APPEND(",\"time_synced\":%s", "true");
    STATUS_APPEND(",\"time_now_epoch\":%lld", (long long)9999999999LL);
    STATUS_APPEND(",\"time_last_sync_epoch\":%lld", (long long)9999999999LL);
    {
        char tz[64];
        memset(tz, 'Z', sizeof(tz) - 1);
        tz[sizeof(tz) - 1] = '\0';
        STATUS_APPEND(",\"time_tz\":\"%s\"", tz);
    }

    STATUS_APPEND(",\"watchdog_panic_disabled\":%s", "true");
    STATUS_APPEND(",\"boot_button_bypass_active\":%s", "true");
    STATUS_APPEND(",\"ota_auth_disabled\":%s", "true");
    STATUS_APPEND(",\"boot_button_bypass_remaining_s\":%lu", (unsigned long)0xFFFFFFFFu);
    STATUS_APPEND(",\"touch_calibrated\":%s", "false");

    if (extra_field) {
        /* A plausible future field of the same shape/width as the real
         * flush_last_us/flush_max_us/flush_count trio this endpoint already
         * grew once (DISPLAY_ST7796_PLAN.md 9.1) -- exercises the exact
         * failure mode the opus review is worried about: a field added
         * without re-running the sizing check. */
        STATUS_APPEND(",\"mock_new_stat_us\":%u,\"mock_new_stat_max_us\":%u", 0xFFFFFFFFu, 0xFFFFFFFFu);
    }

    STATUS_APPEND("}");

    if (out_len) *out_len = o;
    return true;
}
#undef STATUS_APPEND

static void test_status_json_worst_case_render_fits_documented_buffer(void)
{
    TEST_SECTION("render_worst_case_status_json() -- GET /api/status's actual worst-case render "
                 "(opus review: the '~100B headroom' figure was folklore, never defended by a "
                 "check) must fit DASHBOARD_JSON_STATUS_BUF_SIZE, with real, measured headroom "
                 "reported here rather than assumed");

    char big[8192];
    size_t worst_len = 0;
    bool ok = render_worst_case_status_json(big, sizeof(big), MAX31856_CHANNEL_COUNT, false, &worst_len);
    TEST_CHECK(ok, "the worst-case render must fit comfortably in an 8192-byte scratch buffer, or "
              "this test's own measurement buffer is too small (raise `big`, not a real bug)");
    TEST_CHECK(worst_len == strlen(big), "the returned length must match the rendered string");

    printf("  /api/status worst-case render: %zu bytes (strlen), against "
          "DASHBOARD_JSON_STATUS_BUF_SIZE=%d -- measured headroom = %ld bytes\n",
          worst_len, (int)DASHBOARD_JSON_STATUS_BUF_SIZE,
          (long)DASHBOARD_JSON_STATUS_BUF_SIZE - (long)worst_len);

    TEST_CHECK(worst_len < DASHBOARD_JSON_STATUS_BUF_SIZE, "the real worst-case render must fit "
              "DASHBOARD_JSON_STATUS_BUF_SIZE with room for the NUL terminator -- a failure here "
              "means the shipped buffer is genuinely too small, not a test artifact");
    /* A minimum real margin, not just ">0": catches the buffer being sized
     * down to a hair's width of the worst case, same "too tight by this
     * file's own rule of thumb" standard dashboard_http.c's own comment
     * applies to itself (it grew 4096->4224 rather than leave ~19B). */
    TEST_CHECK((long)DASHBOARD_JSON_STATUS_BUF_SIZE - (long)worst_len >= 50,
              "headroom has shrunk below this file's own 50-byte minimum margin -- raise "
              "DASHBOARD_JSON_STATUS_BUF_SIZE in dashboard_json.h");

    /* Sanity: this really did render the full, complete document, not a
     * truncated one that happened to return true. */
    TEST_CHECK(json_looks_complete(big), "the worst-case render must itself be complete, balanced "
              "JSON -- a bug in this mirror, not the production handler, would show up here");
}

static void test_status_json_mutation_shrink_buffer_goes_red(void)
{
    TEST_SECTION("MUTATION 1/2 -- shrinking the buffer to just under the measured worst case must "
                 "make the worst-case render fail (prove the check can actually fail)");

    char big[8192];
    size_t worst_len = 0;
    bool ok0 = render_worst_case_status_json(big, sizeof(big), MAX31856_CHANNEL_COUNT, false, &worst_len);
    TEST_CHECK(ok0, "setup: the unshrunk render must succeed, or this mutation test proves nothing");

    /* Exactly the measured worst length -- one byte short of what
     * STATUS_APPEND's `n_ >= cap - o` check needs to leave room for the
     * final NUL. Must go red. */
    char *tight = malloc(worst_len);
    TEST_CHECK(tight != NULL, "malloc must succeed on a host with plenty of heap");
    if (tight != NULL) {
        size_t got_len = 0;
        bool ok = render_worst_case_status_json(tight, worst_len, MAX31856_CHANNEL_COUNT, false, &got_len);
        printf("  RED (expected): render_worst_case_status_json() into a %zu-byte buffer (worst "
              "case is %zu bytes) returned %s\n", worst_len, worst_len, ok ? "true (BUG)" : "false");
        TEST_CHECK(!ok, "a buffer exactly at (not over) the worst-case length must be reported as "
                  "too small -- this is the exact class of bug a stale/reverted "
                  "DASHBOARD_JSON_STATUS_BUF_SIZE would reintroduce");
        free(tight);
    }
}

static void test_status_json_mutation_field_creep_goes_red(void)
{
    TEST_SECTION("MUTATION 2/2 -- a plausible new field added to /api/status without re-running "
                 "this sizing check must overflow a buffer sized for the OLD worst case (the real "
                 "failure mode: 'fields were added all night')");

    char big[8192];
    size_t worst_len_before = 0;
    bool ok0 = render_worst_case_status_json(big, sizeof(big), MAX31856_CHANNEL_COUNT, false,
                                             &worst_len_before);
    TEST_CHECK(ok0, "setup: the render without the new field must succeed");

    /* A buffer sized to exactly hold the OLD worst case (+1 for the NUL --
     * this is what "the field list grew but nobody bumped the constant"
     * looks like: the buffer is the same size it always was). */
    char *old_size_buf = malloc(worst_len_before + 1);
    TEST_CHECK(old_size_buf != NULL, "malloc must succeed on a host with plenty of heap");
    if (old_size_buf != NULL) {
        size_t got_len = 0;
        bool ok = render_worst_case_status_json(old_size_buf, worst_len_before + 1,
                                                 MAX31856_CHANNEL_COUNT, /*extra_field=*/true, &got_len);
        printf("  RED (expected): adding one plausible new field (~%d bytes) without raising the "
              "buffer from its old %zu-byte worst case returned %s\n",
              (int)strlen(",\"mock_new_stat_us\":4294967295,\"mock_new_stat_max_us\":4294967295"),
              worst_len_before, ok ? "true (BUG -- field creep would go undetected)" : "false");
        TEST_CHECK(!ok, "adding a new field without raising the buffer size must be caught, not "
                  "silently absorbed by unaccounted headroom");
        free(old_size_buf);
    }

    /* And the positive control: the SAME new field, in a buffer that DOES
     * budget for it, must succeed -- proves the field itself isn't simply
     * broken/unreachable in this mirror. */
    char with_room[8192];
    size_t got_len2 = 0;
    bool ok2 = render_worst_case_status_json(with_room, sizeof(with_room), MAX31856_CHANNEL_COUNT,
                                             /*extra_field=*/true, &got_len2);
    TEST_CHECK(ok2, "the new field must render successfully once the buffer actually budgets for it");
    TEST_CHECK(got_len2 > worst_len_before, "the new field must actually add bytes, or this whole "
              "mutation test is vacuous");
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
    test_status_json_worst_case_render_fits_documented_buffer();
    test_status_json_mutation_shrink_buffer_goes_red();
    test_status_json_mutation_field_creep_goes_red();
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
