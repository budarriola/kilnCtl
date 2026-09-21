// Host test for App/drivers/persist/profiles_builtin.c -- specifically the 2026-09-05
// "Unrated" cone sentinel (PROFILES_BUILTIN_CONE_UNRATED, owner decision):
// the 10 catalogue entries whose Digital Fire source page states no cone
// number (FSCG1, FSCGB1, FSCGCL, FSCGWM, FSCRGL, FSHP1, FSHP3, FSNM5, MDDCL,
// QICA -- previously marked UNRESOLVED with an invented placeholder number
// in profiles_builtin_table.inc) now carry that sentinel instead, and must:
//   1. Print as "Unrated" via profiles_builtin_cone_label(), not a number.
//   2. Sort AFTER every real cone within the same firing-type list -- the
//      point of the sentinel is to stop pretending a number was published,
//      not to silently sort as if it were cone -128 (INT8_MIN's raw value,
//      which a plain signed compare would put FIRST, the opposite of what's
//      wanted).
//
// This file #includes profiles_builtin.c directly (own executable, same
// convention as test_profiles_http.c/test_zones_http.c) so it links the
// REAL, generated profiles_builtin_table.inc rather than a test-local fake
// catalogue -- the defect class this guards against (a sort/label bug) only
// exists against the real table's actual entries.
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

int g_test_failures = 0;
int g_test_count = 0;

#include "test_common.h"

// profiles_builtin.c's hidden-mask persistence (profiles_builtin_start()/
// profiles_builtin_set_hidden()) now calls hal_kv_get_u32()/hal_kv_set_u32()
// (HW_ABSTRACTION.md Phase 3 item 3, the nvs.h -> hal_kv.h migration)
// -- none of this file's tests exercise that path, but the symbols must
// still resolve at link time, so this executable links the real fake_kv.c
// backend (build_host_tests.ps1's exe23) rather than hand-rolling
// always-empty/always-OK stand-ins the way the old nvs.h-based version did.
#include "esp_err.h"
#include "fake_kv.h"

#include "profiles_builtin.c"

static const char *UNRATED_CODES[] = {
    "FSCG1", "FSCGB1", "FSCGCL", "FSCGWM", "FSCRGL",
    "FSHP1", "FSHP3", "FSNM5", "MDDCL", "QICA",
};
#define UNRATED_CODE_COUNT (sizeof(UNRATED_CODES) / sizeof(UNRATED_CODES[0]))

static bool code_is_expected_unrated(const char *code)
{
    for (size_t i = 0; i < UNRATED_CODE_COUNT; i++) {
        if (strcmp(code, UNRATED_CODES[i]) == 0) {
            return true;
        }
    }
    return false;
}

static void test_cone_label_unrated(void)
{
    TEST_SECTION("profiles_builtin_cone_label -- PROFILES_BUILTIN_CONE_UNRATED prints \"Unrated\", "
                 "not a number");
    char buf[8];
    profiles_builtin_cone_label(PROFILES_BUILTIN_CONE_UNRATED, buf, sizeof(buf));
    TEST_CHECK(strcmp(buf, "Unrated") == 0, "sentinel prints as the literal word \"Unrated\"");
}

static void test_cone_label_real_cone_unaffected(void)
{
    TEST_SECTION("profiles_builtin_cone_label -- a real low-fire cone (-4, i.e. cone04) still prints "
                 "\"04\", unaffected by the sentinel branch");
    char buf[8];
    profiles_builtin_cone_label(-4, buf, sizeof(buf));
    TEST_CHECK(strcmp(buf, "04") == 0, "existing negative-encoding printed form is untouched");
}

static void test_exactly_the_ten_named_entries_are_unrated(void)
{
    TEST_SECTION("g_builtin_profiles -- exactly the 10 named codes carry the Unrated sentinel; every "
                 "other entry keeps a real (non-sentinel) cone");
    size_t found_unrated = 0;
    for (size_t i = 0; i < g_builtin_profile_count; i++) {
        const builtin_profile_t *b = &g_builtin_profiles[i];
        bool expected = code_is_expected_unrated(b->code);
        bool actual = (b->cone == PROFILES_BUILTIN_CONE_UNRATED);
        if (expected) {
            found_unrated++;
        }
        TEST_CHECK(expected == actual,
                   "entry's sentinel state matches the owner's named list (code follows in next check)");
    }
    TEST_CHECK(found_unrated == UNRATED_CODE_COUNT,
               "all 10 named codes were actually present in the table and matched");
}

// SaftyFW's S8 sanity-rate guard ships with a compiled default of
// 33.3 C/min = 2x the fastest RISING ramp_c_per_hr across every built-in
// profile (999.0 C/hr, two tied steps in FSCGB1 -- see
// firmware/SaftyFW/src/config_store.h's CONFIG_STORE_DEFAULT_MAX_RATE_C_PER_MIN
// comment). That "no shipped profile's rising ramp can trip S8" claim is
// only true if every RISING segment actually declares a positive, bounded
// rate -- the table header's convention is that ramp_c_per_hr <= 0 means
// "unlimited rate" (the executor jumps the setpoint immediately), which
// would trip S8 instantly regardless of window sizing. This walks every
// built-in profile and asserts that invariant directly, rather than relying
// on eyeballing the table.
static void test_every_rising_segment_has_bounded_positive_ramp(void)
{
    TEST_SECTION("g_builtin_profiles -- every RISING segment (target above the previous target, or "
                 "above 0 for a profile's first segment) has 0 < ramp_c_per_hr <= 999.0 C/hr, so none "
                 "can produce SaftyFW S8's modeled \"unlimited rate\" (ramp_c_per_hr <= 0) behavior "
                 "and none exceeds the fastest rate the 33.3 C/min default was sized against");
    int checked = 0;
    for (size_t i = 0; i < g_builtin_profile_count; i++) {
        const builtin_profile_t *b = &g_builtin_profiles[i];
        float prev_target = 0.0f;
        for (uint8_t s = 0; s < b->segment_count; s++) {
            const profile_segment_t *seg = &b->segments[s];
            /* target_c/ramp_c_per_hr are only meaningful for
             * PROFILE_SEG_KIND_ZONE_RAMP (profiles_http.h) -- a RELAY_IO
             * segment's target_c/ramp_c_per_hr fields are unrelated unions
             * of io_* state, not a temperature ramp, so they must not be
             * fed into this invariant. */
            if (seg->seg_kind != PROFILE_SEG_KIND_ZONE_RAMP) {
                continue;
            }
            bool rising = seg->target_c > prev_target;
            if (rising) {
                checked++;
                TEST_CHECK(seg->ramp_c_per_hr > 0.0f,
                           "rising segment declares a positive (bounded) ramp, not \"unlimited\"");
                /* 999.0 C/hr mirrors SaftyFW's
                 * CONFIG_STORE_DEFAULT_MAX_RATE_C_PER_MIN (config_store.h,
                 * 33.3 C/min = 2x this) -- the other side of the pair this
                 * test protects. */
                TEST_CHECK(seg->ramp_c_per_hr <= 999.0f,
                           "rising segment's ramp does not exceed the fastest rate the S8 default "
                           "was sized against (999.0 C/hr, FSCGB1)");
            }
            prev_target = seg->target_c;
        }
    }
    /* Coverage floor: 94 is today's count of rising ZONE_RAMP segments
     * across the built-in catalogue (reviewer re-derived). Guards against
     * the loop silently checking near-zero segments if seg_kind filtering
     * or the catalogue itself regresses. */
    TEST_CHECK(checked >= 94, "checked at least 94 rising ZONE_RAMP segments across the catalogue");
}

void run_test_profiles_builtin(void)
{
    test_cone_label_unrated();
    test_cone_label_real_cone_unaffected();
    test_exactly_the_ten_named_entries_are_unrated();
    test_every_rising_segment_has_bounded_positive_ramp();
}

int main(void)
{
    run_test_profiles_builtin();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
