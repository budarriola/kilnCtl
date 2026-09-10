// test_s8_rate_guard_estimate.c -- host tests for s8_rate_guard_estimate.c.
// docs/audits/s8_auto_calc_design_2026-09-09.md is the design this pins.
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "s8_rate_guard_estimate.h"

static int g_checks = 0;
static int g_failures = 0;

#define TEST_SECTION(msg) printf("-- %s --\n", (msg))
#define TEST_CHECK(cond, msg) do { \
        g_checks++; \
        if (!(cond)) { \
            g_failures++; \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
        } \
    } while (0)

static void test_no_data_when_nothing_valid(void)
{
    TEST_SECTION("no valid zone -> S8_RATE_GUARD_ESTIMATE_NO_DATA, out untouched");

    s8_rate_guard_zone_input_t zones[3];
    memset(zones, 0, sizeof(zones)); // valid == false for all three

    float out = -1.0f;
    s8_rate_guard_estimate_reason_t r = s8_rate_guard_estimate(zones, 3, &out);
    TEST_CHECK(r == S8_RATE_GUARD_ESTIMATE_NO_DATA, "no valid zone reports NO_DATA");
    TEST_CHECK(out == -1.0f, "out_c_per_min is left untouched on NO_DATA");

    TEST_CHECK(s8_rate_guard_estimate(NULL, 3, &out) == S8_RATE_GUARD_ESTIMATE_NO_DATA,
               "NULL zones -> NO_DATA, not a crash");
    TEST_CHECK(s8_rate_guard_estimate(zones, 3, NULL) == S8_RATE_GUARD_ESTIMATE_NO_DATA,
               "NULL out_c_per_min -> NO_DATA, not a crash");
    TEST_CHECK(s8_rate_guard_estimate(zones, 0, &out) == S8_RATE_GUARD_ESTIMATE_NO_DATA,
               "zone_count == 0 -> NO_DATA");
}

static void test_basic_derivation_and_margin(void)
{
    TEST_SECTION("one valid zone: candidate = 2x (k_dc/tau_s * 60), clamped to [floor, ceiling]");

    // k_dc=38 C/duty, tau_s=1200s (20 min) -- plausible bench-scale FOPDT
    // numbers (project_coupling_matrix_resolved: K_diag ~38 C/duty).
    // slope = 38/1200 * 60 = 1.9 C/min; margin 2x = 3.8 C/min -- well below
    // the floor, so the floor must win.
    s8_rate_guard_zone_input_t zones[1] = {
        { .valid = true, .k_dc = 38.0f, .tau_s = 1200.0f, .fit_temp_c = 40.0f },
    };
    float out = 0.0f;
    s8_rate_guard_estimate_reason_t r = s8_rate_guard_estimate(zones, 1, &out);
    TEST_CHECK(r == S8_RATE_GUARD_ESTIMATE_OK, "a single valid zone produces OK");
    TEST_CHECK(fabsf(out - S8_RATE_GUARD_ESTIMATE_FLOOR_C_PER_MIN) < 1e-4f,
               "a slow plant's raw candidate is clamped UP to the floor, not left tiny "
               "(a guard tighter than the floor would nuisance-trip on ordinary operation)");

    // A fast, small-tau zone: k_dc=38, tau_s=60s -> slope = 38 C/min, margin
    // 2x = 76 C/min -- above the ceiling, so the ceiling must win.
    zones[0].tau_s = 60.0f;
    r = s8_rate_guard_estimate(zones, 1, &out);
    TEST_CHECK(r == S8_RATE_GUARD_ESTIMATE_OK, "a fast zone still produces OK");
    TEST_CHECK(fabsf(out - S8_RATE_GUARD_ESTIMATE_CEILING_C_PER_MIN) < 1e-4f,
               "an aggressive candidate is clamped DOWN to the ceiling, never left absurd");

    // A moderate zone landing squarely inside [floor, ceiling]: k_dc=20,
    // tau_s=40s -> slope = 30 C/min; margin 2x = 60... let's pick numbers
    // that land mid-range instead: k_dc=10, tau_s=60s -> slope=10 C/min,
    // margin 2x=20 C/min -- inside [15,60].
    zones[0].k_dc = 10.0f;
    zones[0].tau_s = 60.0f;
    r = s8_rate_guard_estimate(zones, 1, &out);
    TEST_CHECK(r == S8_RATE_GUARD_ESTIMATE_OK, "a mid-range zone produces OK");
    TEST_CHECK(fabsf(out - 20.0f) < 1e-3f,
               "an unclamped candidate is exactly 2x the identified plant's own full-duty "
               "slope, not silently rounded to the floor or ceiling");
}

static void test_picks_lowest_fit_temp_across_zones(void)
{
    TEST_SECTION("multiple valid zones -- the LOWEST fit_temp_c zone's own k_dc/tau_s wins, "
                 "even when a hotter zone's own candidate would be different");

    // Zone 0 identified hot (900C) with a candidate that would clamp to the
    // ceiling if used; zone 1 identified cold (40C) with a candidate that
    // lands mid-range. The coldest -- zone 1 -- must be the one used, per
    // this module's documented "coldest is fastest, so it is the
    // conservative single point to trust" rule.
    s8_rate_guard_zone_input_t zones[2] = {
        { .valid = true, .k_dc = 100.0f, .tau_s = 10.0f, .fit_temp_c = 900.0f }, // would clamp to ceiling
        { .valid = true, .k_dc = 10.0f,  .tau_s = 60.0f, .fit_temp_c = 40.0f },  // -> 20.0 C/min unclamped
    };
    float out = 0.0f;
    s8_rate_guard_estimate_reason_t r = s8_rate_guard_estimate(zones, 2, &out);
    TEST_CHECK(r == S8_RATE_GUARD_ESTIMATE_OK, "two valid zones still produce OK");
    TEST_CHECK(fabsf(out - 20.0f) < 1e-3f,
               "the COLDEST valid zone's own candidate is used (20.0), not the hottest "
               "zone's (which would have clamped to the ceiling) and not an average of the two");

    // Order must not matter -- swap the array and confirm the same zone
    // (by fit_temp_c, not by index) is still picked.
    s8_rate_guard_zone_input_t zones_swapped[2] = { zones[1], zones[0] };
    out = 0.0f;
    r = s8_rate_guard_estimate(zones_swapped, 2, &out);
    TEST_CHECK(r == S8_RATE_GUARD_ESTIMATE_OK, "swapped order still produces OK");
    TEST_CHECK(fabsf(out - 20.0f) < 1e-3f, "the lowest-fit_temp_c zone wins regardless of array order");
}

static void test_invalid_entries_are_skipped(void)
{
    TEST_SECTION("invalid/non-finite/non-positive entries are skipped, not treated as candidates");

    // Only MAX31856_CHANNEL_COUNT (3) zones exist on real hardware -- this
    // test stays within that bound rather than a fourth entry an out-of-
    // range zone_count would silently drop (see the "invalid zone_count"
    // coverage below for that separate case).
    s8_rate_guard_zone_input_t zones[3] = {
        { .valid = false, .k_dc = 10.0f, .tau_s = 60.0f, .fit_temp_c = 20.0f },       // skipped: not valid
        { .valid = true,  .k_dc = 0.0f,  .tau_s = 60.0f, .fit_temp_c = 20.0f },       // skipped: k_dc <= 0
        { .valid = true,  .k_dc = 10.0f, .tau_s = 60.0f, .fit_temp_c = 500.0f },      // the only real candidate
    };
    float out = 0.0f;
    s8_rate_guard_estimate_reason_t r = s8_rate_guard_estimate(zones, 3, &out);
    TEST_CHECK(r == S8_RATE_GUARD_ESTIMATE_OK, "one genuinely valid entry among two bad ones still works");
    TEST_CHECK(fabsf(out - 20.0f) < 1e-3f, "the bad entries never influence the result");

    // zone_count above MAX31856_CHANNEL_COUNT is silently clamped down to
    // it (documented, defensive) -- confirm a candidate placed AFTER that
    // bound is correctly never seen.
    s8_rate_guard_zone_input_t zones_oob[4] = {
        { .valid = false, .k_dc = 0.0f, .tau_s = 0.0f, .fit_temp_c = 0.0f },
        { .valid = false, .k_dc = 0.0f, .tau_s = 0.0f, .fit_temp_c = 0.0f },
        { .valid = false, .k_dc = 0.0f, .tau_s = 0.0f, .fit_temp_c = 0.0f },
        { .valid = true,  .k_dc = 10.0f, .tau_s = 60.0f, .fit_temp_c = 500.0f }, // beyond the clamp
    };
    out = -1.0f;
    r = s8_rate_guard_estimate(zones_oob, 4, &out);
    TEST_CHECK(r == S8_RATE_GUARD_ESTIMATE_NO_DATA,
               "a zone_count above MAX31856_CHANNEL_COUNT is clamped down, so a valid entry "
               "placed past that bound is never considered");

    // NaN k_dc / tau_s / fit_temp_c must each independently be skipped, same
    // as the <=0 cases above -- proves this isn't just a sign check.
    s8_rate_guard_zone_input_t nan_zones[3] = {
        { .valid = true, .k_dc = NAN, .tau_s = 60.0f, .fit_temp_c = 20.0f },
        { .valid = true, .k_dc = 10.0f, .tau_s = NAN, .fit_temp_c = 20.0f },
        { .valid = true, .k_dc = 10.0f, .tau_s = 60.0f, .fit_temp_c = NAN },
    };
    out = -1.0f;
    r = s8_rate_guard_estimate(nan_zones, 3, &out);
    TEST_CHECK(r == S8_RATE_GUARD_ESTIMATE_NO_DATA, "NaN in any of k_dc/tau_s/fit_temp_c is skipped "
               "for that zone; with no other candidate, the overall result is NO_DATA");
    TEST_CHECK(out == -1.0f, "out_c_per_min untouched when every candidate was disqualified");
}

int main(void)
{
    test_no_data_when_nothing_valid();
    test_basic_derivation_and_margin();
    test_picks_lowest_fit_temp_across_zones();
    test_invalid_entries_are_skipped();

    printf("\n%d/%d checks passed\n", g_checks - g_failures, g_checks);
    if (g_failures > 0) {
        printf("%d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
