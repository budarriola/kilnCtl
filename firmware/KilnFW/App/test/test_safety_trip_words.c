// Host test for App/drivers/safety_trip_words.h's safety_trip_words_cause_
// numbered() -- 2026-08-28 (ROADMAP.md M13, owner's scope change: "all
// faults... should come with... what was detected wrong", numbers not a
// generic sentence). Header-only (static inline), no seam to #include a .c
// for, so this file just includes the header directly and exercises it.
//
// Own, TWELFTH separate executable, same reasoning as test_board_temps.c
// above it: nothing else in this build needs safety_trip_words.h's
// dependencies (math.h/stdio.h/string.h only), so there is no collision risk
// to avoid, but keeping every *_prestart/board_temps-style single-purpose
// test its own exe is this test dir's established convention.
#include <math.h>
#include <stdio.h>
#include <string.h>

int g_test_failures = 0;
int g_test_count = 0;

#include "test_common.h"

#include "../drivers/safety_trip_words.h"

static void test_s1_overtemp_has_numbers(void)
{
    char buf[200];
    const float cur[3] = { NAN, NAN, NAN };
    const char *s = safety_trip_words_cause_numbered(1, 850.3f, 800.0f, cur, 255u, buf, sizeof(buf));
    TEST_CHECK(strstr(s, "850.3") != NULL, "S1 cause names the reached temperature");
    TEST_CHECK(strstr(s, "800.0") != NULL, "S1 cause names the ceiling it exceeded");
    // NEGATIVE CONTROL for the two checks above: prove they can fail. If the
    // numbers are swapped/dropped, strstr must NOT find them -- run inline
    // below with an obviously-wrong wanted string so a future accidental
    // "always true" check (e.g. strstr(s, "") which is never NULL) would be
    // caught by review, not silently pass.
    TEST_CHECK(strstr(s, "999.9") == NULL, "S1 cause does NOT invent an unrelated number");
}

static void test_s1_overtemp_falls_back_without_numbers(void)
{
    // NaN inputs (never-received TRIP_EVENT convention, safety_link.h) must
    // fall back to the honest generic sentence, not print "nan" or a
    // half-formatted string.
    char buf[200];
    const float cur[3] = { NAN, NAN, NAN };
    const char *s = safety_trip_words_cause_numbered(1, NAN, NAN, cur, 255u, buf, sizeof(buf));
    TEST_CHECK(strcmp(s, safety_trip_words_cause(1)) == 0,
               "S1 with NaN inputs falls back to the plain (unnumbered) cause verbatim");
    TEST_CHECK(strstr(s, "nan") == NULL && strstr(s, "NAN") == NULL,
               "fallback text never contains a literal nan/NAN");
}

static void test_s3_load_stuck_on_reports_current(void)
{
    char buf[200];
    const float cur[3] = { 12.34f, 0.01f, 0.02f };
    const char *s = safety_trip_words_cause_numbered(3, NAN, 2.0f, cur, 255u, buf, sizeof(buf));
    TEST_CHECK(strstr(s, "12.34") != NULL, "S3 cause names the offending channel's current");
    TEST_CHECK(strstr(s, "2.00") != NULL, "S3 cause names the present-current threshold");
}

static void test_s3_load_stuck_on_falls_back_on_nan_current(void)
{
    // P4-A (opus review, 2026-08-28): trip_current_a is a struct array
    // member at every real call site and can never be NULL, so the old
    // `trip_current_a != NULL` test was not a real gate -- an uncommissioned
    // current-sense channel (NAN, same sentinel convention as the other
    // fields) fell through to the numbered branch anyway and rendered
    // "nanA/nanA/nanA". This must now fall back to the plain cause instead.
    char buf[200];
    const float cur[3] = { NAN, 0.01f, 0.02f }; // ONE nan channel is enough
    const char *s = safety_trip_words_cause_numbered(3, NAN, 2.0f, cur, 255u, buf, sizeof(buf));
    TEST_CHECK(strcmp(s, safety_trip_words_cause(3)) == 0,
               "S3 with a NaN current channel falls back to the plain cause, not a nan sentence");
    TEST_CHECK(strstr(s, "nan") == NULL && strstr(s, "NAN") == NULL,
               "S3 fallback text never contains a literal nan/NAN");
}

static void test_s9_contactor_welded_reports_current(void)
{
    char buf[200];
    const float cur[3] = { 0.03f, 9.87f, 0.04f };
    const char *s = safety_trip_words_cause_numbered(10, NAN, NAN, cur, 255u, buf, sizeof(buf));
    TEST_CHECK(strstr(s, "9.87") != NULL, "S9 cause names the offending channel's current");
}

static void test_s9_contactor_welded_falls_back_on_nan_current(void)
{
    // Same NULL-can-never-fire gap as S3 above, for the S9/case-10 branch.
    char buf[200];
    const float cur[3] = { NAN, NAN, NAN };
    const char *s = safety_trip_words_cause_numbered(10, NAN, NAN, cur, 255u, buf, sizeof(buf));
    TEST_CHECK(strcmp(s, safety_trip_words_cause(10)) == 0,
               "S9 with NaN current falls back to the plain cause, not a nan sentence");
    TEST_CHECK(strstr(s, "nan") == NULL && strstr(s, "NAN") == NULL,
               "S9 fallback text never contains a literal nan/NAN");
}

static void test_s6b_link_dead_reports_elapsed_time(void)
{
    char buf[200];
    const float cur[3] = { NAN, NAN, NAN };
    // 37 * 100ms = 3.7s
    const char *s = safety_trip_words_cause_numbered(7, NAN, NAN, cur, 37u, buf, sizeof(buf));
    TEST_CHECK(strstr(s, "3.7") != NULL, "S6b cause names the elapsed silence in seconds");
}

static void test_s6b_link_dead_falls_back_on_sentinel(void)
{
    // 255 is the "never captured" sentinel (safety_link.h convention) -- it
    // must NOT be printed as "25.5s", which is exactly what a naive /10.0
    // with no sentinel check would do.
    char buf[200];
    const float cur[3] = { NAN, NAN, NAN };
    const char *s = safety_trip_words_cause_numbered(7, NAN, NAN, cur, 255u, buf, sizeof(buf));
    TEST_CHECK(strcmp(s, safety_trip_words_cause(7)) == 0,
               "S6b with the 255 sentinel falls back to the plain cause, not a fake 25.5s");
    TEST_CHECK(strstr(s, "25.5") == NULL, "S6b never prints the sentinel misread as a real duration");
}

static void test_s12_enclosure_never_claims_wrong_sensor(void)
{
    // Deliberate design point from this function's own header comment:
    // trip_safety_tc_c is the SAFETY thermocouple, not the enclosure/CJ
    // sensor S12 actually trips on, so even with a valid tc_c this guard
    // must NOT print it as if it were the enclosure reading.
    char buf[200];
    const float cur[3] = { NAN, NAN, NAN };
    const char *s = safety_trip_words_cause_numbered(13, 123.4f, 60.0f, cur, 255u, buf, sizeof(buf));
    TEST_CHECK(strstr(s, "123.4") == NULL,
               "S12 cause never prints trip_safety_tc_c as though it were the enclosure reading");
    TEST_CHECK(strstr(s, "60.0") != NULL, "S12 cause still names the threshold it DOES have");
    TEST_CHECK(strstr(s, "does not currently carry") != NULL,
               "S12 cause states the missing-reading gap explicitly rather than staying silent");
}

static void test_unknown_reason_never_crashes(void)
{
    char buf[200];
    const float cur[3] = { NAN, NAN, NAN };
    const char *s = safety_trip_words_cause_numbered(99, 1.0f, 2.0f, cur, 5u, buf, sizeof(buf));
    TEST_CHECK(s != NULL && s[0] != '\0', "an unrecognised reason code still returns a real string");
}

static void test_buffer_always_terminated(void)
{
    // Tiny buffer: snprintf must truncate safely, never overrun.
    char buf[6];
    const float cur[3] = { 12.34f, 0.01f, 0.02f };
    const char *s = safety_trip_words_cause_numbered(1, 850.3f, 800.0f, cur, 255u, buf, sizeof(buf));
    TEST_CHECK(strlen(s) < sizeof(buf), "truncated output still fits within the given buffer");
    TEST_CHECK(buf[sizeof(buf) - 1] == '\0' || strlen(buf) < sizeof(buf) - 1,
               "buffer stays NUL-terminated even when the sentence is truncated");
}

int main(void)
{
    TEST_SECTION("safety_trip_words_cause_numbered");

    test_s1_overtemp_has_numbers();
    test_s1_overtemp_falls_back_without_numbers();
    test_s3_load_stuck_on_reports_current();
    test_s3_load_stuck_on_falls_back_on_nan_current();
    test_s9_contactor_welded_reports_current();
    test_s9_contactor_welded_falls_back_on_nan_current();
    test_s6b_link_dead_reports_elapsed_time();
    test_s6b_link_dead_falls_back_on_sentinel();
    test_s12_enclosure_never_claims_wrong_sensor();
    test_unknown_reason_never_crashes();
    test_buffer_always_terminated();

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures > 0 ? 1 : 0;
}
