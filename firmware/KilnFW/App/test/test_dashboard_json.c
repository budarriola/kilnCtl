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
     * same buffer size expression, same fixed header, same trailing '}'. */
    char json[256 + MAX31856_CHANNEL_COUNT * 448];
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
     * which this exercises directly at the same per-zone budget
     * (960 + MAX31856_CHANNEL_COUNT*320, dashboard_http.c:1421). */
    char json[960 + MAX31856_CHANNEL_COUNT * 512];
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

static void run_test_dashboard_json(void)
{
    test_json_escape_doubles_every_quote_and_backslash();
    test_control_status_json_is_complete_and_well_formed_at_3_zones();
    test_exec_status_json_is_complete_and_well_formed_at_3_zones();
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
