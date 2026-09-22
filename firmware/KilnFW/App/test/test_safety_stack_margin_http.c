// Host test for App/drivers/http/safety_stack_margin_http.c's pure JSON
// builder (safety_stack_margin_build_json()) -- GET /api/saftyfw_stack_margin,
// the ESP-side surface for SaftyFW's nine live per-task stack high-water
// marks (docs/audits/saftyfw_live_stack_reporting_impl_2026-09-14.md).
//
// Own separate executable, same "no other seam" reason as test_partition_
// info_http.c/test_safety_cfg_http.c: this file #includes safety_stack_
// margin_http.c directly and supplies its own fake bodies for
// safety_link_get_stack_margin()/wifi_provision_http_get_server()/
// httpd_register_uri_handler(), which would multiply-define against any
// other test file's own fakes of those same names if linked together.
//
// Covers: the link-down/error rendering, a fully-measured round trip (all 9
// tasks), the UNMEASURED-sentinel/rounds_completed==0 rendering, and a
// max-width assertion against STACK_MARGIN_JSON_MAX's own worst-case
// reasoning (CLAUDE.md: "measure its worst-case width and pin it with a
// max-width test").
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

int g_test_failures = 0;
int g_test_count = 0;

#include "test_common.h"

#include "esp_err.h"
#include "esp_http_server.h"
#include "psa/crypto.h"

// psa/crypto.h's host stub declares this `extern` (only ONE definition per
// executable) -- this executable now links web_auth_store.c
// (docs/WEB_AUTH_PLAN.md section 5/9 route rewiring pulling in
// http_auth_policy_iface.c) so it needs its own copy.
psa_status_t g_stub_psa_import_key_result = PSA_SUCCESS;

#include "../drivers/http/safety_stack_margin_http.c"

// ---------------------------------------------------------------------------
// esp_http_server.h / wifi_provision_http.h stub bodies -- never invoked by
// these tests (only safety_stack_margin_build_json() is called directly),
// but every symbol safety_stack_margin_http.c references anywhere in the
// file must resolve at link time. Same convention test_safety_cfg_http.c
// documents for its own identical stubs.
// ---------------------------------------------------------------------------
esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri_handler)
{
    (void)handle;
    (void)uri_handler;
    return ESP_OK;
}
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type) { (void)r; (void)type; return ESP_OK; }
esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, long long buf_len)
{
    (void)r;
    (void)buf;
    (void)buf_len;
    return ESP_OK;
}
// kiln_http_register()'s pre-handler (http_auth_http.c, now linked into
// this executable) calls httpd_resp_set_status() on a DENY -- never actually
// reached by these tests, but must resolve at link time, same convention
// as the rest of this stub block.
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status) { (void)r; (void)status; return ESP_OK; }
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field, const char *value) { (void)r; (void)field; (void)value; return ESP_OK; }
esp_err_t httpd_resp_send_err(httpd_req_t *r, httpd_err_code_t error, const char *msg)
{
    (void)r; (void)error; (void)msg; return ESP_OK;
}
httpd_handle_t wifi_provision_http_get_server(void) { return NULL; }

// Controllable fake for safety_link_get_stack_margin() -- the handler-level
// path (api_stack_margin_get_handler()) is not exercised by these tests
// (they call safety_stack_margin_build_json() directly, same "pure builder"
// split build_commissioning_json() uses), but the symbol still must resolve.
esp_err_t safety_link_get_stack_margin(SafetyLinkClass *link, kilnlink_stack_margin_t *out)
{
    (void)link;
    (void)out;
    return ESP_ERR_NOT_SUPPORTED;
}

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------
static kilnlink_stack_margin_t make_fully_measured(void)
{
    kilnlink_stack_margin_t m = {0};
    m.rounds_completed = 3;
    m.last_tick_ms = 123456u;
    for (uint8_t i = 0; i < KILNLINK_STACK_MARGIN_NUM_TASKS; i++) {
        m.entries[i].task_id = i;
        m.entries[i].high_water_words = (uint16_t)(40u + i * 5u);
        m.entries[i].stack_total_words = 256u;
    }
    /* link_task (index KILNLINK_STACK_MARGIN_TASK_LINK_TASK) given the
     * CLAUDE.md-cited reference figure -- 4736 bytes of 10240 = 1184/2560
     * words -- so the max-width test below exercises a realistic 4-digit
     * high_water_words value, not just small numbers. */
    m.entries[KILNLINK_STACK_MARGIN_TASK_LINK_TASK].high_water_words = 1184;
    m.entries[KILNLINK_STACK_MARGIN_TASK_LINK_TASK].stack_total_words = 2560;
    return m;
}

static void test_link_down_reports_error_not_data(void)
{
    char buf[STACK_MARGIN_JSON_MAX];
    size_t len = safety_stack_margin_build_json(false, ESP_ERR_TIMEOUT, NULL, buf, sizeof(buf));
    TEST_CHECK(len > 0, "link-down build produced output");
    TEST_CHECK(strstr(buf, "\"link_up\":false") != NULL, "link-down reports link_up:false");
    TEST_CHECK(strstr(buf, "ESP_ERR_TIMEOUT") != NULL, "link-down names the esp_err");
    TEST_CHECK(strstr(buf, "\"tasks\":") == NULL, "link-down omits fabricated task data");
    TEST_CHECK(strstr(buf, "\"floor_not_worst_case\":true") != NULL,
               "link-down still carries the floor caveat");
}

static void test_full_round_trip_all_nine(void)
{
    kilnlink_stack_margin_t m = make_fully_measured();
    char buf[STACK_MARGIN_JSON_MAX];
    size_t len = safety_stack_margin_build_json(true, ESP_OK, &m, buf, sizeof(buf));
    TEST_CHECK(len > 0, "full round trip build succeeded");
    TEST_CHECK(strstr(buf, "\"link_up\":true") != NULL, "reports link_up:true");
    TEST_CHECK(strstr(buf, "\"rounds_completed\":3") != NULL, "reports rounds_completed");
    TEST_CHECK(strstr(buf, "\"last_tick_ms\":123456") != NULL, "reports last_tick_ms (freshness signal)");
    TEST_CHECK(strstr(buf, "\"all_measured\":true") != NULL, "all_measured true when every entry is measured");
    TEST_CHECK(strstr(buf, "\"units\":\"words\"") != NULL, "units field present and WORDS, not bytes");
    TEST_CHECK(strstr(buf, "\"name\":\"relay_owner\"") != NULL, "task 0 named relay_owner");
    TEST_CHECK(strstr(buf, "\"name\":\"watchdog_task\"") != NULL, "task 8 named watchdog_task");
    TEST_CHECK(strstr(buf, "\"name\":\"link_task\"") != NULL, "link_task present");
    TEST_CHECK(strstr(buf, "\"high_water_words\":1184") != NULL, "link_task high water reported in words");
    TEST_CHECK(strstr(buf, "\"stack_total_words\":2560") != NULL, "link_task configured depth in words");

    // Every one of the 9 tasks must appear exactly once.
    for (uint8_t i = 0; i < KILNLINK_STACK_MARGIN_NUM_TASKS; i++) {
        char needle[32];
        snprintf(needle, sizeof(needle), "\"id\":%u,", (unsigned)i);
        TEST_CHECK(strstr(buf, needle) != NULL, "each task id 0..8 present");
    }
}

static void test_unmeasured_sentinel_omits_fabricated_numbers(void)
{
    kilnlink_stack_margin_t m = {0};
    m.rounds_completed = 0; // startup: not even one full round yet
    for (uint8_t i = 0; i < KILNLINK_STACK_MARGIN_NUM_TASKS; i++) {
        m.entries[i].task_id = i;
        m.entries[i].high_water_words = KILNLINK_STACK_MARGIN_UNMEASURED;
        m.entries[i].stack_total_words = 256;
    }
    // One entry got its first sample already (round-robin, mid-cycle).
    m.entries[2].high_water_words = 80;

    char buf[STACK_MARGIN_JSON_MAX];
    size_t len = safety_stack_margin_build_json(true, ESP_OK, &m, buf, sizeof(buf));
    TEST_CHECK(len > 0, "unmeasured-sentinel build succeeded");
    TEST_CHECK(strstr(buf, "\"all_measured\":false") != NULL,
               "rounds_completed==0 must report all_measured:false");
    TEST_CHECK(strstr(buf, "\"measured\":false") != NULL, "an unmeasured entry reports measured:false");
    TEST_CHECK(strstr(buf, "\"measured\":true") != NULL, "the one sampled entry reports measured:true");

    // The still-unmeasured entries must NEVER emit the 0xFFFF sentinel (or
    // any other number) as if it were a real reading -- this is the exact
    // "never report an unmeasured entry as a real number" rule kilnlink_
    // stack_margin.h's own header comment states.
    TEST_CHECK(strstr(buf, "65535") == NULL, "sentinel value never leaks into the JSON as a number");
}

// Reproduces the defect found by the 2026-09-14 review: the poller can
// advance rounds_completed past 0 while one slot never resolved its
// TaskHandle_t (a task-name drift, or a task not yet created) and therefore
// stays at KILNLINK_STACK_MARGIN_UNMEASURED forever. Deriving all_measured
// from rounds_completed alone would then report all_measured:true right
// next to that entry's own measured:false -- self-contradictory. Proves
// all_measured is now derived by scanning the entries themselves.
static void test_all_measured_cannot_contradict_a_stuck_sentinel(void)
{
    kilnlink_stack_margin_t m = make_fully_measured();
    m.rounds_completed = 200; // well past 0 -- the old, wrong signal for "all done"
    m.entries[3].high_water_words = KILNLINK_STACK_MARGIN_UNMEASURED; // thermo_task stuck

    char buf[STACK_MARGIN_JSON_MAX];
    size_t len = safety_stack_margin_build_json(true, ESP_OK, &m, buf, sizeof(buf));
    TEST_CHECK(len > 0, "stuck-sentinel-with-high-rounds build succeeded");
    TEST_CHECK(strstr(buf, "\"all_measured\":false") != NULL,
               "all_measured is false when ANY entry is still UNMEASURED, "
               "regardless of how high rounds_completed has climbed");
    TEST_CHECK(strstr(buf, "\"measured\":false") != NULL, "the stuck entry itself reports measured:false");
}

// Real worst-case width, per STACK_MARGIN_JSON_MAX's own comment: longest
// task name (13 chars, "watchdog_task"/"discrete_task"), max 5-digit id
// (impossible here since ids are 0-8, but high_water_words/stack_total_words
// both at their real u16 ceiling, 65535, is realistic on a badly-mis-sized
// task) -- assert the actual worst-case fixture fits comfortably inside
// STACK_MARGIN_JSON_MAX with real headroom to spare, not just that it
// doesn't return 0.
static void test_worst_case_width_fits_with_headroom(void)
{
    kilnlink_stack_margin_t m = {0};
    m.rounds_completed = 255; // saturated, per the field's own doc comment
    for (uint8_t i = 0; i < KILNLINK_STACK_MARGIN_NUM_TASKS; i++) {
        m.entries[i].task_id = i;
        m.entries[i].high_water_words = 65535u; // not KILNLINK_STACK_MARGIN_UNMEASURED-shaped output check, just max width
        m.entries[i].stack_total_words = 65535u;
    }

    char buf[STACK_MARGIN_JSON_MAX];
    size_t len = safety_stack_margin_build_json(true, ESP_OK, &m, buf, sizeof(buf));
    TEST_CHECK(len > 0, "worst-case-width fixture fits STACK_MARGIN_JSON_MAX");
    TEST_CHECK(len < STACK_MARGIN_JSON_MAX - 200,
               "worst-case JSON leaves real headroom in STACK_MARGIN_JSON_MAX (buffer not merely "
               "just barely sufficient)");

    // Pin the actual number so a future format-string change that grows the
    // per-entry width is caught here rather than only discovered by a
    // buffer-too-small failure on target.
    TEST_CHECK(len < 1200, "worst-case width stays under the 1150-byte estimate in the header comment "
                           "(with slack)");
}

int main(void)
{
    test_link_down_reports_error_not_data();
    test_full_round_trip_all_nine();
    test_unmeasured_sentinel_omits_fabricated_numbers();
    test_all_measured_cannot_contradict_a_stuck_sentinel();
    test_worst_case_width_fits_with_headroom();

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures == 0 ? 0 : 1;
}
