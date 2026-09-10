// test_s8_rate_guard_estimate.c -- host tests for s8_rate_guard_estimate.c.
// docs/audits/s8_auto_calc_design_2026-09-09.md is the design this pins;
// the 2026-09-10 review (five confirmed defects) reworked both the
// implementation and this file -- see s8_rate_guard_estimate.h for the full
// rationale behind each change below.
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

/* Mirrors the sentinel s8_rate_guard_estimate.c itself duplicates from
 * zones_config_accessors.h -- see that file's own comment for why this test
 * cannot #include the real header either. */
#define FIT_TEMP_UNKNOWN (-273.15f)

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

static void test_fit_temp_unknown_sentinel_rejected(void)
{
    TEST_SECTION("defect A: ZONE_MODEL_FIT_TEMP_UNKNOWN is finite and must NOT win the "
                 "coldest-zone selection, nor be used as a candidate on its own");

    // A single zone carrying the sentinel (exactly what every zone on the
    // bench reads back today, per docs/audits/... 2026-09-10 review) must
    // produce NO_DATA, not a candidate built from that zone's k_dc/tau_s.
    s8_rate_guard_zone_input_t sentinel_only[1] = {
        { .valid = true, .k_dc = 31.96f, .tau_s = 166.9f, .fit_temp_c = FIT_TEMP_UNKNOWN,
          .coupling_gain_sum_c_per_duty = 0.0f },
    };
    float out = -1.0f;
    s8_rate_guard_estimate_reason_t r = s8_rate_guard_estimate(sentinel_only, 1, &out);
    TEST_CHECK(r == S8_RATE_GUARD_ESTIMATE_NO_DATA,
               "a lone zone carrying the FIT_TEMP_UNKNOWN sentinel must report NO_DATA, "
               "not derive a candidate from it");
    TEST_CHECK(out == -1.0f, "out_c_per_min untouched when the only candidate was the sentinel");

    // Two zones: one real cold fit at 40C, one carrying the sentinel. Before
    // the 2026-09-10 fix, isfinite()-only validation let the sentinel
    // (-273.15, the lowest representable value) unconditionally win the
    // "lowest fit_temp_c" comparison, silently substituting its k_dc/tau_s
    // for the real identification. The real zone must win instead.
    s8_rate_guard_zone_input_t mixed[2] = {
        { .valid = true, .k_dc = 999.0f, .tau_s = 1.0f, .fit_temp_c = FIT_TEMP_UNKNOWN,
          .coupling_gain_sum_c_per_duty = 0.0f }, // would dominate catastrophically if it won
        { .valid = true, .k_dc = 10.0f, .tau_s = 60.0f, .fit_temp_c = 40.0f,
          .coupling_gain_sum_c_per_duty = 0.0f }, // -> 10.0 C/min unclamped * 1.3 margin = 13.0 -> floor
    };
    out = -1.0f;
    r = s8_rate_guard_estimate(mixed, 2, &out);
    TEST_CHECK(r == S8_RATE_GUARD_ESTIMATE_OK_CLAMPED_FLOOR,
               "the sentinel zone is skipped; the real zone's own candidate is used and clamped "
               "up to the floor, not the sentinel zone's absurd (k_dc=999,tau=1) candidate");
    TEST_CHECK(fabsf(out - S8_RATE_GUARD_ESTIMATE_FLOOR_C_PER_MIN) < 1e-4f,
               "candidate is the floor, proving the sentinel zone never won selection "
               "(it would have produced a candidate far above the ceiling instead)");

    // Order must not matter -- the sentinel must lose regardless of array
    // position.
    s8_rate_guard_zone_input_t mixed_swapped[2] = { mixed[1], mixed[0] };
    out = -1.0f;
    r = s8_rate_guard_estimate(mixed_swapped, 2, &out);
    TEST_CHECK(r == S8_RATE_GUARD_ESTIMATE_OK_CLAMPED_FLOOR,
               "swapped order: the sentinel zone is still skipped, regardless of array position");
    TEST_CHECK(fabsf(out - S8_RATE_GUARD_ESTIMATE_FLOOR_C_PER_MIN) < 1e-4f,
               "swapped order: same floor-clamped result as unswapped");
}

static void test_basic_derivation_and_margin(void)
{
    TEST_SECTION("one valid zone, no coupling: candidate = 1.3x (k_dc/tau_s * 60), "
                 "clamped to [floor, ceiling], reason reflects whether clamping occurred");

    // k_dc=38 C/duty, tau_s=1200s (20 min) -- plausible bench-scale FOPDT
    // numbers (project_coupling_matrix_resolved: K_diag ~38 C/duty).
    // slope = 38/1200 * 60 = 1.9 C/min; margin 1.3x = 2.47 C/min -- well
    // below the floor, so the floor must win.
    s8_rate_guard_zone_input_t zones[1] = {
        { .valid = true, .k_dc = 38.0f, .tau_s = 1200.0f, .fit_temp_c = 40.0f,
          .coupling_gain_sum_c_per_duty = 0.0f },
    };
    float out = 0.0f;
    s8_rate_guard_estimate_reason_t r = s8_rate_guard_estimate(zones, 1, &out);
    TEST_CHECK(r == S8_RATE_GUARD_ESTIMATE_OK_CLAMPED_FLOOR,
               "a slow, uncoupled plant's raw candidate is clamped UP to the floor, and the "
               "reason says so rather than reporting plain OK");
    TEST_CHECK(fabsf(out - S8_RATE_GUARD_ESTIMATE_FLOOR_C_PER_MIN) < 1e-4f,
               "a slow plant's raw candidate is clamped UP to the floor, not left tiny "
               "(a guard tighter than the floor would nuisance-trip on ordinary operation)");

    // A fast, small-tau zone: k_dc=38, tau_s=60s -> slope = 38 C/min, margin
    // 1.3x = 49.4 C/min -- inside [15,60], NOT clamped.
    zones[0].tau_s = 60.0f;
    r = s8_rate_guard_estimate(zones, 1, &out);
    TEST_CHECK(r == S8_RATE_GUARD_ESTIMATE_OK, "an unclamped candidate reports plain OK");
    TEST_CHECK(fabsf(out - 49.4f) < 1e-2f,
               "candidate is exactly 1.3x the identified plant's own full-duty slope");

    // An aggressive zone that DOES clamp to the ceiling: k_dc=100, tau_s=10
    // -> slope = 600 C/min, margin 1.3x = 780 -- above the ceiling.
    zones[0].k_dc = 100.0f;
    zones[0].tau_s = 10.0f;
    r = s8_rate_guard_estimate(zones, 1, &out);
    TEST_CHECK(r == S8_RATE_GUARD_ESTIMATE_OK_CLAMPED_CEILING,
               "an aggressive candidate is clamped DOWN to the ceiling, and the reason says so");
    TEST_CHECK(fabsf(out - S8_RATE_GUARD_ESTIMATE_CEILING_C_PER_MIN) < 1e-4f,
               "candidate is capped at the ceiling, never left absurd");

    // A moderate zone landing squarely inside [floor, ceiling]: k_dc=10,
    // tau_s=60s -> slope=10 C/min, margin 1.3x=13.0 -- below the floor,
    // clamped. Pick numbers that land unclamped instead: k_dc=20, tau_s=60s
    // -> slope=20 C/min, margin 1.3x=26.0 -- inside [15,60], unclamped.
    zones[0].k_dc = 20.0f;
    zones[0].tau_s = 60.0f;
    r = s8_rate_guard_estimate(zones, 1, &out);
    TEST_CHECK(r == S8_RATE_GUARD_ESTIMATE_OK, "a mid-range zone produces plain OK, unclamped");
    TEST_CHECK(fabsf(out - 26.0f) < 1e-3f,
               "an unclamped candidate is exactly 1.3x the identified plant's own full-duty "
               "slope, not silently rounded to the floor or ceiling");
}

static void test_coupling_gain_is_included_in_basis(void)
{
    TEST_SECTION("defect D: S8 watches ONE TC shared by every zone, so the basis must include "
                 "the OTHER zones' coupled contribution, not just this zone's own k_dc");

    // Bench-measured z0 row (docs/audits/high_temperature_transfer_analysis_
    // 2026-09-08.md): own k_dc=31.96, tau=166.9s, coupling from z1+z2 onto
    // z0 = 27.32 + 21.72 = 49.04. Own-only slope = 31.96/166.9*60 = 11.49
    // C/min (this is what the pre-2026-09-10 version would have used and
    // clamped up to the floor of 15 -- structurally unreachable). With
    // coupling included: (31.96+49.04)/166.9*60 = 29.13 C/min raw, *1.3
    // margin = 37.87 C/min -- comfortably inside [15,60], unclamped, and a
    // real number derived from the plant's actual worst-case (all zones at
    // full duty) rather than an artifact of ignoring coupling.
    s8_rate_guard_zone_input_t zones[1] = {
        { .valid = true, .k_dc = 31.96f, .tau_s = 166.9f, .fit_temp_c = 40.0f,
          .coupling_gain_sum_c_per_duty = 49.04f },
    };
    float out = 0.0f;
    s8_rate_guard_estimate_reason_t r = s8_rate_guard_estimate(zones, 1, &out);
    TEST_CHECK(r == S8_RATE_GUARD_ESTIMATE_OK, "coupling-inclusive candidate lands unclamped");
    TEST_CHECK(fabsf(out - 37.87f) < 0.05f,
               "candidate reflects (k_dc + coupling_gain_sum)/tau_s * 60 * margin, not k_dc alone "
               "-- omitting coupling would have produced 14.9 (clamped to the 15 floor) instead");

    // Zero coupling (a genuinely single-zone board) must reproduce the
    // own-zone-only basis exactly -- coupling is additive, not a multiplier
    // that changes behaviour when it legitimately has nothing to add.
    zones[0].coupling_gain_sum_c_per_duty = 0.0f;
    r = s8_rate_guard_estimate(zones, 1, &out);
    TEST_CHECK(r == S8_RATE_GUARD_ESTIMATE_OK_CLAMPED_FLOOR,
               "zero coupling reproduces the pre-coupling own-zone-only result: 11.49*1.3=14.9, "
               "below the floor, clamped");
    TEST_CHECK(fabsf(out - S8_RATE_GUARD_ESTIMATE_FLOOR_C_PER_MIN) < 1e-4f,
               "own-zone-only-equivalent case still lands at the floor as before");

    // Negative coupling_gain_sum_c_per_duty must be rejected (it would
    // UNDERSTATE the true worst-case basis), same disqualification class as
    // a non-finite k_dc/tau_s/fit_temp_c.
    s8_rate_guard_zone_input_t negative_coupling[1] = {
        { .valid = true, .k_dc = 20.0f, .tau_s = 60.0f, .fit_temp_c = 40.0f,
          .coupling_gain_sum_c_per_duty = -5.0f },
    };
    out = -1.0f;
    r = s8_rate_guard_estimate(negative_coupling, 1, &out);
    TEST_CHECK(r == S8_RATE_GUARD_ESTIMATE_NO_DATA,
               "a negative coupling_gain_sum_c_per_duty is rejected outright, not clamped to zero "
               "and silently used anyway");

    // NaN coupling_gain_sum_c_per_duty must also be rejected.
    s8_rate_guard_zone_input_t nan_coupling[1] = {
        { .valid = true, .k_dc = 20.0f, .tau_s = 60.0f, .fit_temp_c = 40.0f,
          .coupling_gain_sum_c_per_duty = NAN },
    };
    out = -1.0f;
    r = s8_rate_guard_estimate(nan_coupling, 1, &out);
    TEST_CHECK(r == S8_RATE_GUARD_ESTIMATE_NO_DATA,
               "a NaN coupling_gain_sum_c_per_duty is rejected outright");
}

static void test_picks_lowest_fit_temp_across_zones(void)
{
    TEST_SECTION("multiple valid zones -- the LOWEST fit_temp_c zone's own basis wins, "
                 "even when a hotter zone's own candidate would be different");

    // Zone 0 identified hot (900C) with a candidate that would clamp to the
    // ceiling if used; zone 1 identified cold (40C) with a candidate that
    // lands mid-range (10.0 C/duty own-only, no coupling here -- this test
    // is about selection, not the coupling basis, which has its own test
    // above). Coldest -- zone 1 -- must be the one used.
    s8_rate_guard_zone_input_t zones[2] = {
        { .valid = true, .k_dc = 100.0f, .tau_s = 10.0f, .fit_temp_c = 900.0f,
          .coupling_gain_sum_c_per_duty = 0.0f }, // would clamp to ceiling
        { .valid = true, .k_dc = 20.0f, .tau_s = 60.0f, .fit_temp_c = 40.0f,
          .coupling_gain_sum_c_per_duty = 0.0f }, // -> 26.0 C/min unclamped
    };
    float out = 0.0f;
    s8_rate_guard_estimate_reason_t r = s8_rate_guard_estimate(zones, 2, &out);
    TEST_CHECK(r == S8_RATE_GUARD_ESTIMATE_OK, "two valid zones still produce OK (unclamped here)");
    TEST_CHECK(fabsf(out - 26.0f) < 1e-3f,
               "the COLDEST valid zone's own candidate is used (26.0), not the hottest "
               "zone's (which would have clamped to the ceiling) and not an average of the two");

    // Order must not matter -- swap the array and confirm the same zone
    // (by fit_temp_c, not by index) is still picked.
    s8_rate_guard_zone_input_t zones_swapped[2] = { zones[1], zones[0] };
    out = 0.0f;
    r = s8_rate_guard_estimate(zones_swapped, 2, &out);
    TEST_CHECK(r == S8_RATE_GUARD_ESTIMATE_OK, "swapped order still produces OK");
    TEST_CHECK(fabsf(out - 26.0f) < 1e-3f, "the lowest-fit_temp_c zone wins regardless of array order");
}

static void test_invalid_entries_are_skipped(void)
{
    TEST_SECTION("invalid/non-finite/non-positive entries are skipped, not treated as candidates");

    // Only MAX31856_CHANNEL_COUNT (3) zones exist on real hardware -- this
    // test stays within that bound rather than a fourth entry an out-of-
    // range zone_count would silently drop (see the "invalid zone_count"
    // coverage below for that separate case).
    s8_rate_guard_zone_input_t zones[3] = {
        { .valid = false, .k_dc = 10.0f, .tau_s = 60.0f, .fit_temp_c = 20.0f,
          .coupling_gain_sum_c_per_duty = 0.0f },  // skipped: not valid
        { .valid = true, .k_dc = 0.0f, .tau_s = 60.0f, .fit_temp_c = 20.0f,
          .coupling_gain_sum_c_per_duty = 0.0f },  // skipped: k_dc <= 0
        { .valid = true, .k_dc = 20.0f, .tau_s = 60.0f, .fit_temp_c = 500.0f,
          .coupling_gain_sum_c_per_duty = 0.0f },  // the only real candidate -> 26.0 C/min
    };
    float out = 0.0f;
    s8_rate_guard_estimate_reason_t r = s8_rate_guard_estimate(zones, 3, &out);
    TEST_CHECK(r == S8_RATE_GUARD_ESTIMATE_OK, "one genuinely valid entry among two bad ones still works");
    TEST_CHECK(fabsf(out - 26.0f) < 1e-3f, "the bad entries never influence the result");

    // zone_count above MAX31856_CHANNEL_COUNT is silently clamped down to
    // it (documented, defensive) -- confirm a candidate placed AFTER that
    // bound is correctly never seen.
    s8_rate_guard_zone_input_t zones_oob[4] = {
        { .valid = false, .k_dc = 0.0f, .tau_s = 0.0f, .fit_temp_c = 0.0f, .coupling_gain_sum_c_per_duty = 0.0f },
        { .valid = false, .k_dc = 0.0f, .tau_s = 0.0f, .fit_temp_c = 0.0f, .coupling_gain_sum_c_per_duty = 0.0f },
        { .valid = false, .k_dc = 0.0f, .tau_s = 0.0f, .fit_temp_c = 0.0f, .coupling_gain_sum_c_per_duty = 0.0f },
        { .valid = true, .k_dc = 20.0f, .tau_s = 60.0f, .fit_temp_c = 500.0f,
          .coupling_gain_sum_c_per_duty = 0.0f }, // beyond the clamp
    };
    out = -1.0f;
    r = s8_rate_guard_estimate(zones_oob, 4, &out);
    TEST_CHECK(r == S8_RATE_GUARD_ESTIMATE_NO_DATA,
               "a zone_count above MAX31856_CHANNEL_COUNT is clamped down, so a valid entry "
               "placed past that bound is never considered");

    // NaN k_dc / tau_s / fit_temp_c must each independently be skipped, same
    // as the <=0 cases above -- proves this isn't just a sign check.
    s8_rate_guard_zone_input_t nan_zones[3] = {
        { .valid = true, .k_dc = NAN, .tau_s = 60.0f, .fit_temp_c = 20.0f, .coupling_gain_sum_c_per_duty = 0.0f },
        { .valid = true, .k_dc = 10.0f, .tau_s = NAN, .fit_temp_c = 20.0f, .coupling_gain_sum_c_per_duty = 0.0f },
        { .valid = true, .k_dc = 10.0f, .tau_s = 60.0f, .fit_temp_c = NAN, .coupling_gain_sum_c_per_duty = 0.0f },
    };
    out = -1.0f;
    r = s8_rate_guard_estimate(nan_zones, 3, &out);
    TEST_CHECK(r == S8_RATE_GUARD_ESTIMATE_NO_DATA, "NaN in any of k_dc/tau_s/fit_temp_c is skipped "
               "for that zone; with no other candidate, the overall result is NO_DATA");
    TEST_CHECK(out == -1.0f, "out_c_per_min untouched when every candidate was disqualified");
}

static void test_auto_decide_policy(void)
{
    TEST_SECTION("s8_rate_guard_auto_decide: tighten-auto-apply, loosen-requires-confirm");

    // Dormant guard (never commissioned / 0.0) -- arming it is always
    // APPLY, regardless of how large the candidate is, since "no ceiling"
    // is never tighter than any finite one.
    TEST_CHECK(s8_rate_guard_auto_decide(60.0f, 0.0f, false) == S8_RATE_GUARD_AUTO_APPLY,
               "arming a dormant guard at the ceiling still auto-applies");
    TEST_CHECK(s8_rate_guard_auto_decide(15.0f, 0.0f, false) == S8_RATE_GUARD_AUTO_APPLY,
               "arming a dormant guard at the floor auto-applies");

    // Armed guard, candidate tightens (strictly less) -- APPLY.
    TEST_CHECK(s8_rate_guard_auto_decide(15.0f, 20.0f, true) == S8_RATE_GUARD_AUTO_APPLY,
               "a strictly tighter candidate auto-applies");

    // Armed guard, candidate ties exactly -- APPLY (not a loosening, no
    // reason to demand a confirm for a no-op write).
    TEST_CHECK(s8_rate_guard_auto_decide(20.0f, 20.0f, true) == S8_RATE_GUARD_AUTO_APPLY,
               "an unchanged candidate auto-applies (treated as a tie, not a loosening)");

    // Armed guard, candidate loosens (strictly greater) -- SUGGEST_ONLY,
    // the one case this whole policy exists to catch.
    TEST_CHECK(s8_rate_guard_auto_decide(25.0f, 20.0f, true) == S8_RATE_GUARD_AUTO_SUGGEST_ONLY,
               "a looser candidate against an armed guard must never auto-apply");

    // Barely looser -- proves this is a strict compare, not fuzzed with an
    // epsilon that could let a tiny loosening slip through as a "tie".
    TEST_CHECK(s8_rate_guard_auto_decide(20.001f, 20.0f, true) == S8_RATE_GUARD_AUTO_SUGGEST_ONLY,
               "even a marginally looser candidate requires confirmation");
}

int main(void)
{
    test_no_data_when_nothing_valid();
    test_fit_temp_unknown_sentinel_rejected();
    test_basic_derivation_and_margin();
    test_coupling_gain_is_included_in_basis();
    test_picks_lowest_fit_temp_across_zones();
    test_invalid_entries_are_skipped();
    test_auto_decide_policy();

    printf("\n%d/%d checks passed\n", g_checks - g_failures, g_checks);
    if (g_failures > 0) {
        printf("%d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
