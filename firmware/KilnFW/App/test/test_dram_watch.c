// Host tests for App/drivers/common/dram_watch.h's pure low-water tracker
// (SK-04 instrumentation). The esp_timer/heap_caps sampler in dram_watch.c is
// target-only; the decision logic it feeds is header-only and tested here.
#include <stdlib.h>
#include <string.h>

#include "test_common.h"

#include "../drivers/common/dram_watch.h"

static void test_first_sample_is_a_new_low(void)
{
    dram_watch_t w;
    dram_watch_init(&w);
    dram_watch_event_t ev = dram_watch_update(&w, 9728, 10);
    TEST_CHECK(ev.new_low && w.valid, "first sample records a low");
    TEST_CHECK(w.low_largest == 9728 && w.low_at_s == 10, "value and uptime stored");
    TEST_CHECK(!ev.alarm_first, "9728 is above the alarm");
}

static void test_equal_or_higher_keeps_first_timestamp(void)
{
    dram_watch_t w;
    dram_watch_init(&w);
    (void)dram_watch_update(&w, 9000, 5);
    dram_watch_event_t ev = dram_watch_update(&w, 9000, 99);
    TEST_CHECK(!ev.new_low, "equal value is not a new low");
    ev = dram_watch_update(&w, 9728, 120);
    TEST_CHECK(!ev.new_low, "recovery is not a new low");
    TEST_CHECK(w.low_largest == 9000 && w.low_at_s == 5, "low-water mark and its first-seen time kept");
}

static void test_lower_updates_and_alarm_fires_once(void)
{
    dram_watch_t w;
    dram_watch_init(&w);
    (void)dram_watch_update(&w, KILN_DRAM_LARGEST_ALARM_BYTES, 1);
    dram_watch_event_t ev = dram_watch_update(&w, KILN_DRAM_LARGEST_ALARM_BYTES - 1, 2);
    TEST_CHECK(ev.new_low && ev.alarm_first, "one byte under the alarm: new low and first alarm");
    ev = dram_watch_update(&w, 8192, 3);
    TEST_CHECK(ev.new_low && !ev.alarm_first, "further drop: new low, alarm already reported");
    ev = dram_watch_update(&w, 9728, 4);
    ev = dram_watch_update(&w, 100, 5);
    TEST_CHECK(!ev.alarm_first, "alarm dump stays one-shot across recovery and re-drop");
    TEST_CHECK(w.low_largest == 100 && w.low_at_s == 5, "lowest value wins");
}

static void test_first_sample_already_below_alarm_fires(void)
{
    dram_watch_t w;
    dram_watch_init(&w);
    dram_watch_event_t ev = dram_watch_update(&w, 4096, 0);
    TEST_CHECK(ev.new_low && ev.alarm_first, "a first sample under the alarm fires immediately");
}

static void test_pending_is_taken_once_and_coalesces(void)
{
    dram_watch_t w;
    dram_watch_init(&w);
    dram_watch_pending_t p = dram_watch_take_pending(&w);
    TEST_CHECK(!p.new_low && !p.alarm, "nothing owed before the first sample");
    (void)dram_watch_update(&w, 9728, 10);
    (void)dram_watch_update(&w, 9600, 12);
    (void)dram_watch_update(&w, 9500, 14);
    p = dram_watch_take_pending(&w);
    TEST_CHECK(p.new_low && p.low_largest == 9500 && p.low_at_s == 14,
               "three new lows between takes coalesce into one line carrying the current low");
    TEST_CHECK(!p.alarm, "no alarm above the floor");
    p = dram_watch_take_pending(&w);
    TEST_CHECK(!p.new_low && !p.alarm, "a second take with no new sample owes nothing");
    (void)dram_watch_update(&w, 9728, 16);
    p = dram_watch_take_pending(&w);
    TEST_CHECK(!p.new_low, "a recovery owes no line");
}

static void test_alarm_pending_once_with_its_own_sample(void)
{
    dram_watch_t w;
    dram_watch_init(&w);
    (void)dram_watch_update(&w, 8192, 57000);
    (void)dram_watch_update(&w, 4096, 57002);
    dram_watch_pending_t p = dram_watch_take_pending(&w);
    TEST_CHECK(p.alarm, "alarm owed after the first sample under the floor");
    TEST_CHECK(p.alarm_largest == 8192 && p.alarm_at_s == 57000,
               "the alarm reports the sample that crossed, not a later one");
    TEST_CHECK(p.new_low && p.low_largest == 4096, "the coalesced low is the current one");
    (void)dram_watch_update(&w, 9728, 57004);
    (void)dram_watch_update(&w, 100, 57006);
    p = dram_watch_take_pending(&w);
    TEST_CHECK(!p.alarm, "the alarm dump is owed at most once per boot");
}

/* Returns the body of `sig` (through the first "\n}" after its brace), or NULL. */
static const char *dw_function_body(const char *text, const char *sig, size_t *len)
{
    const char *start = strstr(text, sig);
    if (!start) {
        return NULL;
    }
    const char *brace = strchr(start, '{');
    if (!brace) {
        return NULL;
    }
    const char *end = strstr(brace, "\n}");
    if (!end) {
        return NULL;
    }
    *len = (size_t)(end - brace);
    return brace;
}

static bool dw_body_has(const char *body, size_t len, const char *needle)
{
    size_t n = strlen(needle);
    for (size_t i = 0; i + n <= len; i++) {
        if (memcmp(body + i, needle, n) == 0) {
            return true;
        }
    }
    return false;
}

/* dram_watch.c is target-only, so this is a source-text guard: the esp_timer
 * callback shares the esp_timer task (3584 B stack) with every other timer in
 * the firmware, so it must stay sample-and-record only. Every log line and the
 * heap dump belong to dram_watch_service(), which telemetry_log_task() calls. */
static void test_timer_callback_does_no_logging(void)
{
    static const char *const candidates[] = {"../drivers/common/dram_watch.c", "drivers/common/dram_watch.c",
                                             "App/drivers/common/dram_watch.c"};
    char *text = test_read_source_anchored(__FILE__, candidates[0], candidates, 3);
    TEST_CHECK(text != NULL, "could not read drivers/common/dram_watch.c");
    if (!text) {
        return;
    }
    size_t len = 0;
    const char *body = dw_function_body(text, "static void sample_cb(", &len);
    TEST_CHECK(body != NULL, "sample_cb() not found in dram_watch.c");
    if (body) {
        TEST_CHECK(!dw_body_has(body, len, "ESP_LOG"), "sample_cb() must not log (esp_timer task stack)");
        TEST_CHECK(!dw_body_has(body, len, "print"), "sample_cb() must not print a heap dump");
        TEST_CHECK(!dw_body_has(body, len, "heap_caps_walk"), "sample_cb() must not walk the heap");
        TEST_CHECK(dw_body_has(body, len, "dram_watch_update("), "sample_cb() still records the sample");
    }
    body = dw_function_body(text, "void dram_watch_service(", &len);
    TEST_CHECK(body != NULL && dw_body_has(body, len, "dram_watch_take_pending("),
               "dram_watch_service() drains the pending flags");
    TEST_CHECK(strstr(text, "heap_caps_print_heap_info") == NULL,
               "no printf-based heap dump: it bypasses the esp_log_set_vprintf() PC log link");
    free(text);

    static const char *const tl_candidates[] = {"../drivers/persist/telemetry_log.c",
                                                "drivers/persist/telemetry_log.c",
                                                "App/drivers/persist/telemetry_log.c"};
    char *tl = test_read_source_anchored(__FILE__, tl_candidates[0], tl_candidates, 3);
    TEST_CHECK(tl != NULL, "could not read drivers/persist/telemetry_log.c");
    if (tl) {
        body = dw_function_body(tl, "static void telemetry_log_task(", &len);
        TEST_CHECK(body != NULL && dw_body_has(body, len, "dram_watch_service();"),
                   "telemetry_log_task() calls dram_watch_service(), or nothing ever logs the alarm");
        free(tl);
    }
}

static void test_host_stand_ins_are_inert(void)
{
    size_t v = 1;
    uint32_t t = 1;
    TEST_CHECK(!dram_watch_get(&v, &t), "host build has no sampler");
    TEST_CHECK(dram_watch_task_after("x", 7) == 7, "after-create passes the create result through");
}

void run_test_dram_watch(void)
{
    TEST_SECTION("dram_watch");
    test_first_sample_is_a_new_low();
    test_equal_or_higher_keeps_first_timestamp();
    test_lower_updates_and_alarm_fires_once();
    test_first_sample_already_below_alarm_fires();
    test_pending_is_taken_once_and_coalesces();
    test_alarm_pending_once_with_its_own_sample();
    test_timer_callback_does_no_logging();
    test_host_stand_ins_are_inert();
}
