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
    /* HP-02 (2026-09-27): relay starvation reporting, same worst-case
     * convention -- the widest "%.1f" dashboard_json.h's sizing note budgets
     * (10B, "-1234567.0") and the uint8_t max for "%u". */
    z->relay_starved_s = -1234567.0f;
    z->relay_denied_reason = 255;
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
    /* docs/audits/profile_executor_panic_2026-09-24.md: worst-case both new
     * run-level fields too, same discipline as every other field here. */
    st->mode_state_fault_latched = true;
    st->mode_state_violation_count = 0xFFFFFFFFu; /* "%lu" worst case */
    /* Spare-relay WP-3: every aux claimed, widest rule_reason. */
    for (uint8_t ai = 0; ai < AUX_OUTPUTS_COUNT; ai++) {
        st->aux[ai].claimed = true;
        st->aux[ai].commanded_on = false; /* "false" is the widest bool */
        st->aux[ai].actuated_on = false;
        st->aux[ai].rule_reason = 255;
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
    TEST_CHECK(strstr(json, "\"relay_starved_s\":-1234567.0") != NULL,
              "must contain relay_starved_s at its worst-case width (HP-02 starvation reporting)");
    TEST_CHECK(strstr(json, "\"relay_denied_reason\":255") != NULL, "must contain relay_denied_reason");
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
    TEST_CHECK(strstr(json, "\"relay_starved_s\":-1234567.0") != NULL,
              "must contain relay_starved_s at its worst-case width (HP-02 starvation reporting)");
    TEST_CHECK(strstr(json, "\"relay_denied_reason\":255") != NULL, "must contain relay_denied_reason");
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
/* Spare-relay WP-3: the trailing "aux" array of /api/profile_exec. */
static void test_exec_status_json_carries_aux_array(void)
{
    TEST_SECTION("append_zone_status_json(control_fields=false) -- the aux array lists exactly the "
                 "claimed aux outputs, fits the buffer beside 3 worst-case zones, and is absent "
                 "from the /api/control shape");

    char *json = malloc(DASHBOARD_JSON_PROFILE_EXEC_BUF_SIZE);
    TEST_CHECK(json != NULL, "malloc must succeed");
    if (json == NULL) return;

    /* Worst case: all 4 aux claimed next to 3 worst-case zones. */
    profile_exec_status_t st;
    fill_worst_case_status(&st);
    size_t o = (size_t)snprintf(json, DASHBOARD_JSON_PROFILE_EXEC_BUF_SIZE, "{");
    o = append_zone_status_json(json, DASHBOARD_JSON_PROFILE_EXEC_BUF_SIZE, o, &st, false);
    if (o + 1 < DASHBOARD_JSON_PROFILE_EXEC_BUF_SIZE) json[o++] = '}';
    json[o < DASHBOARD_JSON_PROFILE_EXEC_BUF_SIZE ? o : DASHBOARD_JSON_PROFILE_EXEC_BUF_SIZE - 1] = '\0';
    TEST_CHECK(json_looks_complete(json), "3 worst-case zones + 4 aux must render complete JSON");
    int aux_objects = 0;
    for (const char *p = json; (p = strstr(p, "\"relay\":")) != NULL; p += 8) aux_objects++;
    TEST_CHECK(aux_objects == 4, "all 4 claimed aux must be present");
    TEST_CHECK(strstr(json, "\"relay\":4,\"commanded_on\":false,\"actuated_on\":false,\"rule_reason\":255}]") != NULL,
              "the last aux object must be intact and close the array");

    /* Mixed: only aux 2 (index 1) claimed, ON, reason NONE. */
    memset(&st, 0, sizeof(st));
    st.aux[1].claimed = true;
    st.aux[1].commanded_on = true;
    st.aux[1].actuated_on = true;
    o = (size_t)snprintf(json, DASHBOARD_JSON_PROFILE_EXEC_BUF_SIZE, "{");
    o = append_zone_status_json(json, DASHBOARD_JSON_PROFILE_EXEC_BUF_SIZE, o, &st, false);
    json[o++] = '}';
    json[o] = '\0';
    TEST_CHECK(json_looks_complete(json), "single-aux render must be complete JSON");
    TEST_CHECK(strstr(json, "\"aux\":[{\"relay\":2,\"commanded_on\":true,\"actuated_on\":true,"
                            "\"rule_reason\":0}]") != NULL,
              "only the claimed aux is listed, 1-based relay number, true values");

    /* Nothing claimed: empty array. */
    memset(&st, 0, sizeof(st));
    o = (size_t)snprintf(json, DASHBOARD_JSON_PROFILE_EXEC_BUF_SIZE, "{");
    o = append_zone_status_json(json, DASHBOARD_JSON_PROFILE_EXEC_BUF_SIZE, o, &st, false);
    json[o++] = '}';
    json[o] = '\0';
    TEST_CHECK(strstr(json, "\"aux\":[]") != NULL, "no claimed aux -> empty array");

    /* /api/control shape does not carry it. */
    fill_worst_case_status(&st);
    o = (size_t)snprintf(json, DASHBOARD_JSON_PROFILE_EXEC_BUF_SIZE, "{");
    o = append_zone_status_json(json, DASHBOARD_JSON_PROFILE_EXEC_BUF_SIZE, o, &st, true);
    json[o++] = '}';
    json[o] = '\0';
    TEST_CHECK(strstr(json, "\"aux\":") == NULL, "control_fields=true must not render aux");
    free(json);
}

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

    /* Spare-relay WP-6: the REAL dashboard_json_append_aux() (not a copy), worst
     * case = all 4 enabled, unclaimed and ON ("manual" is the widest source
     * and needs ON, "true"+"manual" beats "false"+"rule"/"off"). */
    if (!dashboard_json_append_aux(json, cap, &o, 0x0Fu, 0x00u, 0x0Fu)) { return false; }

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

    /* Pre-existing gap found 2026-09-22 while adding the two S1/S8 fields
     * below: safety_tc_reconfig_gave_up (2026-09-16) is emitted UNCONDITIONALLY
     * on the main status endpoint (dashboard_status_http.c) right here but was
     * never added to this mirror -- backfilled now rather than left short next
     * to the two new fields immediately below it. */
    STATUS_APPEND(",\"safety_tc_reconfig_gave_up\":%s", "false");
    /* 2026-09-22: S1/S8 ship disabled-by-zero, surfaced on the main status
     * endpoint too (dashboard_status_http.c, same site as
     * safety_tc_reconfig_gave_up just above). "true" is the same length as
     * "false" minus one char either way is fine here -- these are plain
     * booleans, worst case is "false" (5 bytes, one longer than "true"). */
    STATUS_APPEND(",\"safety_s1_abs_max_disabled\":%s", "false");
    STATUS_APPEND(",\"safety_s8_rate_guard_disabled\":%s", "false");
    /* 2026-09-23: same "main status endpoint too" convention as the two
     * fields just above, for SaftyFW's config_store_write_volatile()
     * RAM-only-install tracking (dashboard_status_http.c). "false" is again
     * the wider boolean rendering. */
    STATUS_APPEND(",\"config_volatile_dirty\":%s", "false");

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
    STATUS_APPEND(",\"ct_leak\":%s", "false");

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
    STATUS_APPEND(",\"touch_calibrated\":%s", "false");
    /* Mirrors dashboard_status_http.c's touch_cal_supported. Worst case is
     * the LONGEST value touch_cal_support_name() can return -- "unknown" and
     * "no_touch" are shorter, so "self_calibrating" is the one that must be
     * budgeted for here. */
    STATUS_APPEND(",\"touch_cal_supported\":\"%s\"", "self_calibrating");
    /* Mirrors the cfg_fs ask-first refusal branch: both fields, with the
     * longest reason cfg_fs_mount.c can hold (s_format_pending_reason[96] ->
     * 95 chars). Worst case is an admin/auth-off caller, who sees the reason. */
    {
        char why[96];
        memset(why, 'r', sizeof(why) - 1);
        why[sizeof(why) - 1] = '\0';
        STATUS_APPEND(",\"cfg_fs_format_pending\":true");
        STATUS_APPEND(",\"cfg_fs_format_reason\":\"%s\"", why);
    }

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

/* ---------------------------------------------------------------------------
 * Mirror discipline for the standalone `?diag=1` document.
 *
 * status_get_handler() answers GET /api/status?diag=1 with a separate, much
 * smaller document built into the SAME json[DASHBOARD_JSON_STATUS_BUF_SIZE]
 * allocation and through the same APPEND() macro, so the same `goto truncated`
 * turns any overflow into an HTTP 500 for the whole document. Nothing else in
 * this repo can render that shape on the host -- dashboard_http.c pulls in
 * lvgl_port.h -- so, exactly as for the default document, this hand-mirror is
 * the only thing policing its size and its field set.
 *
 * Keep render_diag_json() byte-for-byte in step with the `if (want_diag_detail)`
 * block of status_get_handler(): same keys, same order, same format specifiers.
 * A field added there and not here makes the size assertion below vacuous --
 * which is precisely the defect class that let the default document overflow.
 * ------------------------------------------------------------------------ */

#define DIAG_APPEND(...)                                                       \
    do {                                                                       \
        int n_ = snprintf(json + o, cap - o, __VA_ARGS__);                     \
        if (n_ < 0 || (size_t)n_ >= cap - o) { return false; }                 \
        o += (size_t)n_;                                                       \
    } while (0)

/* Renders the `?diag=1` body into json[cap]. Returns false -- mirroring the
 * handler's own `goto truncated` -- the instant any append would not fit. */
static bool render_diag_json(char *json, size_t cap, bool ever_received,
                             unsigned boot_reason, unsigned long frames_ok,
                             unsigned long frames_bad, unsigned long tx_dropped,
                             unsigned long log_dropped, bool tc_reconfig_gave_up,
                             bool s1_abs_max_disabled, bool s8_rate_guard_disabled,
                             bool extra_field, size_t *out_len)
{
    size_t o = 0;

    DIAG_APPEND("{\"ok\":true,\"diag_ever_received\":%s",
                ever_received ? "true" : "false");
    if (ever_received) {
        DIAG_APPEND(",\"diag_boot_reason\":%u", boot_reason);
        DIAG_APPEND(",\"diag_context_frames_ok\":%lu", frames_ok);
        DIAG_APPEND(",\"diag_context_frames_bad\":%lu", frames_bad);
        DIAG_APPEND(",\"diag_tx_frames_dropped\":%lu", tx_dropped);
        /* 2026-09-20: this mirror had drifted from the handler by TWO fields
         * before it was caught by the log_frames_dropped review --
         * safety_tc_reconfig_gave_up (added 2026-09-16) was never mirrored
         * either, so the "worst case" this file measured had been short by
         * ~38 bytes for four days. Exactly the vacuity this block's own
         * header comment warns about. */
        DIAG_APPEND(",\"diag_log_frames_dropped\":%lu", log_dropped);
        DIAG_APPEND(",\"safety_tc_reconfig_gave_up\":%s",
                    tc_reconfig_gave_up ? "true" : "false");
        /* 2026-09-22: same drift risk as above -- keep step with
         * dashboard_status_http.c's ?diag=1 block's own two new appends
         * for S1/S8 shipping disabled-by-zero. */
        DIAG_APPEND(",\"safety_s1_abs_max_disabled\":%s",
                    s1_abs_max_disabled ? "true" : "false");
        DIAG_APPEND(",\"safety_s8_rate_guard_disabled\":%s",
                    s8_rate_guard_disabled ? "true" : "false");
    }
    /* Not emitted by the handler -- the mutation test's stand-in for a field
     * added to the diag document without checking that it still fits. */
    if (extra_field) {
        DIAG_APPEND(",\"diag_future_counter\":%lu", 4294967295UL);
    }
    DIAG_APPEND("}");

    if (out_len) { *out_len = o; }
    return true;
}

#undef DIAG_APPEND

/* Widest the diag document can ever be: every numeric field at the maximum
 * width its C type admits (diag_boot_reason is uint8_t, the three counters are
 * uint32_t -- dashboard_http.h:293,306-308), and the gate true. The gate-false
 * literal is one byte longer but omits six fields, so gate-true is the worst
 * case; the size test below asserts that relationship rather than assuming it. */
static bool render_worst_case_diag_json(char *json, size_t cap, bool extra_field,
                                        size_t *out_len)
{
    return render_diag_json(json, cap, /*ever_received=*/true, 255u,
                            4294967295UL, 4294967295UL, 4294967295UL, 4294967295UL,
                            /*tc_reconfig_gave_up=*/false,
                            /*s1_abs_max_disabled=*/false, /*s8_rate_guard_disabled=*/false,
                            extra_field, out_len);
}

static void test_diag_json_worst_case_render_fits_documented_buffer(void)
{
    char json[DASHBOARD_JSON_STATUS_BUF_SIZE];
    size_t worst_len = 0;
    bool ok = render_worst_case_diag_json(json, sizeof(json), /*extra_field=*/false,
                                          &worst_len);
    TEST_CHECK(ok, "the worst-case ?diag=1 render must not truncate inside "
              "DASHBOARD_JSON_STATUS_BUF_SIZE -- the handler shares that one buffer "
              "with the default document");

    /* The gate-false shape must also fit, and must be the shorter of the two.
     * If it ever grows past the gate-true shape the worst case has moved and
     * the measurement below stops being a worst case. */
    size_t gate_false_len = 0;
    bool ok_false = render_diag_json(json, sizeof(json), /*ever_received=*/false,
                                     255u, 4294967295UL, 4294967295UL, 4294967295UL,
                                     4294967295UL, /*tc_reconfig_gave_up=*/false,
                                     /*s1_abs_max_disabled=*/false,
                                     /*s8_rate_guard_disabled=*/false,
                                     /*extra_field=*/false, &gate_false_len);
    TEST_CHECK(ok_false, "the diag_ever_received=false ?diag=1 render must also fit");
    TEST_CHECK(gate_false_len < worst_len,
              "the diag_ever_received=false shape must stay shorter than the true "
              "shape, or this file measures the wrong branch as the worst case");

    printf("  [diag] worst-case ?diag=1 render: %zu bytes, buffer %d, headroom %ld "
           "(gate-false shape: %zu bytes)\n",
           worst_len, (int)DASHBOARD_JSON_STATUS_BUF_SIZE,
           (long)DASHBOARD_JSON_STATUS_BUF_SIZE - (long)worst_len, gate_false_len);

    TEST_CHECK(worst_len < DASHBOARD_JSON_STATUS_BUF_SIZE,
              "the real worst-case ?diag=1 render must fit the documented buffer");
    TEST_CHECK((long)DASHBOARD_JSON_STATUS_BUF_SIZE - (long)worst_len >= 50,
              "?diag=1 headroom has shrunk below this file's own 50-byte minimum "
              "margin -- raise DASHBOARD_JSON_STATUS_BUF_SIZE in dashboard_json.h");
}

static void test_diag_json_content_is_complete_and_correctly_valued(void)
{
    char json[DASHBOARD_JSON_STATUS_BUF_SIZE];
    size_t len = 0;

    /* Distinctive, non-round values: a mis-wired field would have to coincide
     * with one of these to pass by accident. */
    bool ok = render_diag_json(json, sizeof(json), /*ever_received=*/true,
                               /*boot_reason=*/37u,
                               /*frames_ok=*/123456789UL,
                               /*frames_bad=*/7UL,
                               /*tx_dropped=*/4294967295UL,
                               /*log_dropped=*/6543UL,
                               /*tc_reconfig_gave_up=*/true,
                               /*s1_abs_max_disabled=*/true,
                               /*s8_rate_guard_disabled=*/true,
                               /*extra_field=*/false, &len);
    TEST_CHECK(ok, "the populated ?diag=1 document must render");
    TEST_CHECK(len == strlen(json), "the reported length must match the rendered string");
    TEST_CHECK(len > 0 && json[0] == '{' && json[len - 1] == '}',
              "the ?diag=1 document must be a single brace-delimited object");

    /* Every field the diag handler emits, present AND correctly valued. */
    TEST_CHECK(strstr(json, "\"ok\":true") != NULL,
              "?diag=1 must carry the ok envelope flag the dashboard checks");
    TEST_CHECK(strstr(json, "\"diag_ever_received\":true") != NULL,
              "diag_ever_received must be present and true here");
    TEST_CHECK(strstr(json, "\"diag_boot_reason\":37") != NULL,
              "diag_boot_reason must be present and carry its own value -- it is the "
              "only surface left for the Pico's last-boot reason now that the three "
              "decoded booleans are gone");
    TEST_CHECK(strstr(json, "\"diag_context_frames_ok\":123456789") != NULL,
              "diag_context_frames_ok must be present and correctly valued");
    TEST_CHECK(strstr(json, "\"diag_context_frames_bad\":7") != NULL,
              "diag_context_frames_bad must be present and correctly valued");
    TEST_CHECK(strstr(json, "\"diag_tx_frames_dropped\":4294967295") != NULL,
              "diag_tx_frames_dropped must be present and correctly valued at the "
              "full uint32_t width");
    TEST_CHECK(strstr(json, "\"diag_log_frames_dropped\":6543") != NULL,
              "diag_log_frames_dropped must be present and correctly valued -- the "
              "Pico's log_task.c drop counter, KILNLINK_PROTOCOL_VERSION 15 -> 16");
    TEST_CHECK(strstr(json, "\"safety_tc_reconfig_gave_up\":true") != NULL,
              "safety_tc_reconfig_gave_up must be present and correctly valued");
    TEST_CHECK(strstr(json, "\"safety_s1_abs_max_disabled\":true") != NULL,
              "safety_s1_abs_max_disabled must be present and correctly valued");
    TEST_CHECK(strstr(json, "\"safety_s8_rate_guard_disabled\":true") != NULL,
              "safety_s8_rate_guard_disabled must be present and correctly valued");

    /* The removed decodes must not silently come back as re-added bytes. */
    TEST_CHECK(strstr(json, "diag_boot_stack_overflow") == NULL,
              "diag_boot_stack_overflow was removed as a pure bit-decode of "
              "diag_boot_reason -- it must not reappear");
    TEST_CHECK(strstr(json, "diag_boot_malloc_failed") == NULL,
              "diag_boot_malloc_failed was removed as a pure bit-decode -- it must "
              "not reappear");
    TEST_CHECK(strstr(json, "diag_boot_assert_failed") == NULL,
              "diag_boot_assert_failed was removed as a pure bit-decode -- it must "
              "not reappear");

    /* The gate: with no DIAG frame ever received the six detail fields are
     * absent entirely, not zero-valued -- safety_page.html must be able to tell
     * "never received" from "received, counters are zero". */
    size_t gated_len = 0;
    bool gated_ok = render_diag_json(json, sizeof(json), /*ever_received=*/false,
                                     37u, 123456789UL, 7UL, 4294967295UL, 6543UL,
                                     /*tc_reconfig_gave_up=*/true,
                                     /*s1_abs_max_disabled=*/true,
                                     /*s8_rate_guard_disabled=*/true,
                                     /*extra_field=*/false, &gated_len);
    TEST_CHECK(gated_ok, "the gated ?diag=1 document must render");
    TEST_CHECK(strstr(json, "\"diag_ever_received\":false") != NULL,
              "the gated document must still report diag_ever_received:false");
    TEST_CHECK(strstr(json, "diag_boot_reason") == NULL,
              "diag_boot_reason must be absent when no DIAG frame has been received");
    TEST_CHECK(strstr(json, "diag_context_frames_ok") == NULL,
              "diag_context_frames_ok must be absent when the gate is false");
    TEST_CHECK(strstr(json, "diag_context_frames_bad") == NULL,
              "diag_context_frames_bad must be absent when the gate is false");
    TEST_CHECK(strstr(json, "diag_tx_frames_dropped") == NULL,
              "diag_tx_frames_dropped must be absent when the gate is false");
    TEST_CHECK(strstr(json, "diag_log_frames_dropped") == NULL,
              "diag_log_frames_dropped must be absent when the gate is false");
    TEST_CHECK(strstr(json, "safety_tc_reconfig_gave_up") == NULL,
              "safety_tc_reconfig_gave_up must be absent when the gate is false");
    TEST_CHECK(strstr(json, "safety_s1_abs_max_disabled") == NULL,
              "safety_s1_abs_max_disabled must be absent when the gate is false");
    TEST_CHECK(strstr(json, "safety_s8_rate_guard_disabled") == NULL,
              "safety_s8_rate_guard_disabled must be absent when the gate is false");
}

/* Proves the size assertion above is load-bearing: a field added to the diag
 * document with the buffer shrunk to the current worst case must fail to
 * render, exactly as the handler's APPEND() would `goto truncated`. */
static void test_diag_json_mutation_field_creep_goes_red(void)
{
    char probe[DASHBOARD_JSON_STATUS_BUF_SIZE];
    size_t worst_len_before = 0;
    bool ok0 = render_worst_case_diag_json(probe, sizeof(probe), /*extra_field=*/false,
                                           &worst_len_before);
    TEST_CHECK(ok0, "baseline worst-case ?diag=1 render must succeed");

    /* Budget exactly the current worst case (+1 for the NUL) and add a field. */
    char tight[DASHBOARD_JSON_STATUS_BUF_SIZE];
    size_t got = 0;
    bool ok1 = render_worst_case_diag_json(tight, worst_len_before + 1,
                                           /*extra_field=*/true, &got);
    printf("  [diag] RED (expected): adding one plausible new field (~%d bytes) with "
           "no headroom past the %zu-byte worst case returned %s\n",
           (int)strlen(",\"diag_future_counter\":4294967295"), worst_len_before,
           ok1 ? "true (BUG -- diag field creep would go undetected)" : "false");
    TEST_CHECK(!ok1, "a field added to the ?diag=1 document with no headroom left "
              "must be caught, not silently truncated into an HTTP 500");

    /* Positive control: the same field must genuinely add bytes once the buffer
     * budgets for it, or the mutation above proves nothing. */
    size_t got2 = 0;
    bool ok2 = render_worst_case_diag_json(tight, sizeof(tight), /*extra_field=*/true,
                                           &got2);
    TEST_CHECK(ok2, "the new diag field must render once the buffer budgets for it");
    TEST_CHECK(got2 > worst_len_before,
              "the new diag field must actually add bytes, or this mutation test is "
              "vacuous");
}

/* Spare-relay WP-6: /api/status "aux" block. */
static void test_status_aux_block_content_and_source(void)
{
    TEST_SECTION("dashboard_json_append_aux -- one object per ENABLED aux only, source = rule when "
                 "claimed, manual when unclaimed+ON, off when unclaimed+OFF");
    char buf[256];
    size_t o = 0;
    buf[0] = '\0';
    TEST_CHECK(dashboard_json_append_aux(buf, sizeof(buf), &o, 0x00u, 0x00u, 0x0Fu), "no aux enabled renders");
    TEST_CHECK(strcmp(buf, ",\"aux\":[]") == 0, "no aux enabled: empty array, a relay that is ON is not listed");

    o = 0;
    /* aux 1 enabled+off, aux 2 enabled+on unclaimed, aux 3 enabled+on claimed, aux 4 disabled+on */
    TEST_CHECK(dashboard_json_append_aux(buf, sizeof(buf), &o, 0x07u, 0x04u, 0x0Eu), "mixed renders");
    TEST_CHECK(strcmp(buf, ",\"aux\":[{\"relay\":1,\"on\":false,\"source\":\"off\"},"
                           "{\"relay\":2,\"on\":true,\"source\":\"manual\"},"
                           "{\"relay\":3,\"on\":true,\"source\":\"rule\"}]") == 0,
              "mixed: exact per-aux relay/on/source, disabled relay 4 omitted");
    TEST_CHECK(o == strlen(buf), "returned offset matches the rendered length");

    o = 0;
    TEST_CHECK(dashboard_json_append_aux(buf, sizeof(buf), &o, 0x01u, 0x01u, 0x00u), "claimed-off renders");
    TEST_CHECK(strstr(buf, "{\"relay\":1,\"on\":false,\"source\":\"rule\"}") != NULL,
              "claimed but OFF is still source rule (the evaluator owns it)");

    /* too small: refuses, restores the terminator at the entry offset */
    char tiny[24];
    strcpy(tiny, "{x");
    o = 2;
    TEST_CHECK(!dashboard_json_append_aux(tiny, sizeof(tiny), &o, 0x0Fu, 0x00u, 0x0Fu),
              "a block that does not fit reports failure");
    TEST_CHECK(o == 2 && strcmp(tiny, "{x") == 0, "failure leaves the prior document intact");
}

static void run_test_dashboard_json(void)
{
    test_json_escape_doubles_every_quote_and_backslash();
    test_control_status_json_is_complete_and_well_formed_at_3_zones();
    test_exec_status_json_is_complete_and_well_formed_at_3_zones();
    test_exec_status_json_carries_aux_array();
    test_status_aux_block_content_and_source();
    test_truncation_is_logged_and_still_produces_valid_json();
    test_json_append_clamped_never_walks_past_cap();
    test_heap_allocated_worst_case_render_matches_stack_sizing();
    test_firing_history_json_is_complete_and_well_formed_at_full_depth();
    test_autotune_status_json_reports_sub_phase_distinctly();
    test_status_json_worst_case_render_fits_documented_buffer();
    test_status_json_mutation_shrink_buffer_goes_red();
    test_status_json_mutation_field_creep_goes_red();
    test_diag_json_worst_case_render_fits_documented_buffer();
    test_diag_json_content_is_complete_and_correctly_valued();
    test_diag_json_mutation_field_creep_goes_red();
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
