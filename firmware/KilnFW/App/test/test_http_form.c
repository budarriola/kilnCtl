// Host tests for drivers/common/http_form.h's strict parse helpers (e7c98209). These are the
// shared mechanism behind the strict checks in dashboard_exec_http (start id), diagnostics_http
// (danger relay/on, danger_enable on), dashboard_autotune_http (zone/step_duty/...), iter_tune_http
// (?zone=), settings_http (display_power brightness/timeout) and security_http (set_policy
// web_enabled/lcd_enabled + parse_int_field). Those handlers have no handler-level host harness
// (iter_tune_http does, see test_iter_tune_http.c), so the contract each relies on is pinned here,
// calling find_field + the parse helper exactly as the handlers do.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test_common.h"

int g_test_failures = 0;
int g_test_count = 0;

#include "../drivers/common/http_form.h"

static bool handler_long(const char *body, const char *key, long lo, long hi, long *out)
{
    char val[8];
    int n = http_form_find_field(body, key, val, sizeof(val));
    return n > 0 && http_form_parse_long(val, n, lo, hi, out);
}

static bool handler_bool(const char *body, const char *key)
{
    char val[4];
    int n = http_form_find_field(body, key, val, sizeof(val));
    return http_form_is_bool01(val, n);
}

static bool handler_float(const char *body, const char *key, float *out)
{
    char val[16];
    int n = http_form_find_field(body, key, val, sizeof(val));
    return n > 0 && http_form_parse_float(val, n, out);
}

static void test_long(void)
{
    TEST_SECTION("http_form_parse_long -- id/zone/relay style fields");
    long v = -99;
    TEST_CHECK(handler_long("id=7", "id", 0, 255, &v) && v == 7, "valid id accepted");
    TEST_CHECK(handler_long("id=0", "id", 0, 255, &v) && v == 0, "lower bound accepted");
    TEST_CHECK(handler_long("id=255", "id", 0, 255, &v) && v == 255, "upper bound accepted");
    TEST_CHECK(!handler_long("id=abc", "id", 0, 255, &v), "abc refused");
    TEST_CHECK(!handler_long("id=1abc", "id", 0, 255, &v), "trailing garbage 1abc refused");
    TEST_CHECK(!handler_long("id=%201", "id", 0, 255, &v), "leading space refused");
    TEST_CHECK(!handler_long("id=256", "id", 0, 255, &v), "above range refused");
    TEST_CHECK(!handler_long("id=-1", "id", 0, 255, &v), "below range refused");
    TEST_CHECK(!handler_long("id=", "id", 0, 255, &v), "empty refused");
    TEST_CHECK(!handler_long("id=99999999999", "id", 0, 255, &v), "too long for the buffer refused");
    TEST_CHECK(!handler_long("id=9999999", "id", 0, 255, &v), "fits buffer but out of range refused");
    TEST_CHECK(!http_form_parse_long("99999999999999999999", 20, 0, 0x7fffffffL, &v), "strtol overflow (ERANGE) refused");
    TEST_CHECK(!handler_long("id=1%002", "id", 0, 255, &v), "%00 refused");
    TEST_CHECK(!handler_long("other=1", "id", 0, 255, &v), "missing key refused");
    TEST_CHECK(!handler_long("relay=0", "relay", 1, 8, &v), "relay below 1 refused");
    TEST_CHECK(handler_long("relay=8", "relay", 1, 8, &v) && v == 8, "relay in range accepted");
    TEST_CHECK(!http_form_parse_long("12", 3, 0, 255, &v), "len/strlen mismatch refused");
}

static void test_bool(void)
{
    TEST_SECTION("http_form_is_bool01 -- exactly 0 or 1");
    TEST_CHECK(handler_bool("on=0", "on"), "0 accepted");
    TEST_CHECK(handler_bool("on=1", "on"), "1 accepted");
    TEST_CHECK(!handler_bool("on=2", "on"), "2 refused");
    TEST_CHECK(!handler_bool("on=01", "on"), "01 refused");
    TEST_CHECK(!handler_bool("on=true", "on"), "true refused");
    TEST_CHECK(!handler_bool("on=1x", "on"), "1x refused");
    TEST_CHECK(!handler_bool("on=", "on"), "empty refused");
    TEST_CHECK(!handler_bool("x=1", "on"), "missing refused");
    TEST_CHECK(!handler_bool("on=%00", "on"), "%00 refused");
    TEST_CHECK(!handler_bool("on=12345", "on"), "over-long refused");
}

static void test_float(void)
{
    TEST_SECTION("http_form_parse_float -- step_duty/setpoint style fields");
    float f = 0.0f;
    TEST_CHECK(handler_float("d=0.5", "d", &f) && fabsf(f - 0.5f) < 1e-6f, "valid float accepted");
    TEST_CHECK(handler_float("d=-3", "d", &f) && f == -3.0f, "negative accepted");
    TEST_CHECK(!handler_float("d=abc", "d", &f), "abc refused");
    TEST_CHECK(!handler_float("d=0.5x", "d", &f), "trailing garbage refused");
    TEST_CHECK(!handler_float("d=", "d", &f), "empty refused");
    TEST_CHECK(!handler_float("d=nan", "d", &f), "nan refused");
    TEST_CHECK(!handler_float("d=inf", "d", &f), "inf refused");
    TEST_CHECK(!handler_float("d=1e999", "d", &f), "overflow refused");
    TEST_CHECK(!handler_float("d=0.%005", "d", &f), "%00 refused");
    TEST_CHECK(!handler_float("d=%200.5", "d", &f), "leading space refused");
}

static void test_decode(void)
{
    TEST_SECTION("http_form_url_decode / find_field -- %00 and overlong");
    char out[8];
    TEST_CHECK(http_form_find_field("a=b%00c", "a", out, sizeof(out)) == -2, "%00 makes find_field return -2");
    TEST_CHECK(http_form_find_field("a=%00", "a", out, sizeof(out)) == -2, "bare %00 refused");
    TEST_CHECK(http_form_find_field("a=ok", "a", out, sizeof(out)) == 2 && strcmp(out, "ok") == 0, "plain value decodes");
    TEST_CHECK(http_form_find_field("a=%41", "a", out, sizeof(out)) == 1 && out[0] == 'A', "%41 decodes to A");
    TEST_CHECK(http_form_find_field("a=12345678", "a", out, sizeof(out)) == -2, "too long returns -2");
    TEST_CHECK(http_form_find_field("a=", "a", out, sizeof(out)) == -1 || http_form_find_field("a=", "a", out, sizeof(out)) == 0,
               "empty value is reported as absent or zero-length, never a positive length");
    TEST_CHECK(http_form_find_field("b=1", "a", out, sizeof(out)) == -1, "absent returns -1");

    /* A -2 must leave out as the empty string, never an unterminated or truncated
     * prefix: callers that test != -1 then strtol()/strcmp() the buffer (zones
     * POST max_simultaneous_relays/continue_on_zone_trip) would otherwise read
     * stack bytes past the written prefix, or take "1%00" as "1". */
    memset(out, '7', sizeof(out));
    TEST_CHECK(http_form_find_field("a=00000000", "a", out, sizeof(out)) == -2 && out[0] == '\0',
               "over-long value leaves out empty, not an unterminated \"0000000\" prefix");
    memset(out, '7', sizeof(out));
    TEST_CHECK(http_form_find_field("a=1%00", "a", out, sizeof(out)) == -2 && out[0] == '\0',
               "%00 leaves out empty, so \"1%00\" can never read back as \"1\"");
    char tiny[1] = { '7' };
    TEST_CHECK(http_form_find_field("a=x", "a", tiny, sizeof(tiny)) == -2 && tiny[0] == '\0',
               "out_cap 1: over-long still terminates out[0]");
}

int main(void)
{
    test_long();
    test_bool();
    test_float();
    test_decode();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures > 0 ? 1 : 0;
}
