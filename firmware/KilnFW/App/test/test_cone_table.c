#include "test_common.h"
#include "../drivers/cone_table.h"

#include <math.h>
#include <string.h>

void run_test_cone_table(void)
{
    TEST_SECTION("cone_table");

    /* Known cone/temperature round-trips. */
    {
        float t = 0.0f;
        cone_table_status_t st = cone_table_temp_c_for_cone("6", &t);
        TEST_CHECK(st == CONE_TABLE_OK, "cone '6' lookup -> OK");
        TEST_CHECK_NEAR(t, 1222.2f, 1e-3, "cone '6' -> 1222.2 C");

        int idx = -1;
        st = cone_table_cone_for_temp_c(1222.2f, &idx);
        TEST_CHECK(st == CONE_TABLE_OK, "temp at cone 6 -> OK");
        TEST_CHECK(idx >= 0 && strcmp(cone_table_get(idx)->cone_label, "6") == 0,
                   "temp at cone 6 -> index resolves back to label '6'");

        st = cone_table_temp_c_for_cone("022", &t);
        TEST_CHECK(st == CONE_TABLE_OK, "cone '022' lookup -> OK");
        TEST_CHECK_NEAR(t, 586.1f, 1e-3, "cone '022' -> 586.1 C (lowest cone)");

        st = cone_table_temp_c_for_cone("14", &t);
        TEST_CHECK(st == CONE_TABLE_OK, "cone '14' lookup -> OK");
        TEST_CHECK_NEAR(t, 1365.0f, 1e-3, "cone '14' -> 1365.0 C (highest cone)");

        st = cone_table_temp_c_for_cone("nope", &t);
        TEST_CHECK(st == CONE_TABLE_ERR_INVALID_INPUT, "unknown cone label -> INVALID_INPUT");
    }

    /* Non-uniform band width: the low-temperature end of the table is
     * packed much tighter than the high-temperature end, so the
     * half-cone-step band under a low cone must be narrower than under a
     * high cone. This is the requirement that matters most -- prove the
     * two widths actually differ, not just that both functions return. */
    {
        /* cone 021 (600.0) sits close to cone 022 (586.1) -- 13.9 C apart. */
        float band_low_c;
        cone_table_status_t st = cone_table_band_bottom_c(600.0f, &band_low_c);
        TEST_CHECK(st == CONE_TABLE_OK, "band bottom under cone 021 -> OK");
        float width_low = 600.0f - band_low_c;

        /* cone 6 (1222.2) sits far from cone 5 (1186.1) -- 36.1 C apart. */
        float band_high_c;
        st = cone_table_band_bottom_c(1222.2f, &band_high_c);
        TEST_CHECK(st == CONE_TABLE_OK, "band bottom under cone 6 -> OK");
        float width_high = 1222.2f - band_high_c;

        TEST_CHECK(fabsf(width_low - width_high) > 5.0f,
                   "band width differs meaningfully between a tightly-spaced low cone and a widely-spaced high cone");
        TEST_CHECK(width_low < width_high,
                   "low-cone band is narrower than high-cone band (non-uniform spacing preserved)");
    }

    /* Target between two cones: band half-width is half the LOCAL spacing
     * between the bracketing pair, not half the distance down to the lower
     * cone -- that was DEFECT 2 (see cone_table.h's comment on
     * cone_table_band_bottom_c()). */
    {
        /* Between cone 06 (997.8) and cone 05 (1031.1); pick 1010.0. */
        float band_c;
        cone_table_status_t st = cone_table_band_bottom_c(1010.0f, &band_c);
        TEST_CHECK(st == CONE_TABLE_OK, "band bottom for target between cones -> OK");
        float expected = 1010.0f - (1031.1f - 997.8f) / 2.0f;
        TEST_CHECK_NEAR(band_c, expected, 1e-3, "between-cones band bottom == target minus half the LOCAL spacing between cones 06 and 05");
    }

    /* DEFECT 2 regression: band width must not collapse for a target a
     * hair above a tabulated cone. Under the broken formula (half = the
     * distance from target_c down to the lower cone), 1222.21 -- one
     * hundredth of a degree above cone 6's 1222.2 -- produced a ~0.005 C
     * band instead of the ~8.35 C the surrounding spacing (cone 6->cone 7
     * is 1222.2->1238.9) implies. Pin the corrected, non-cliff widths at
     * three targets straddling cone 6. */
    {
        float band_below_c, band_just_above_c, band_further_above_c;
        cone_table_status_t st;

        st = cone_table_band_bottom_c(1222.19f, &band_below_c);
        TEST_CHECK(st == CONE_TABLE_OK, "band bottom just below cone 6 -> OK");
        float width_below = 1222.19f - band_below_c;

        st = cone_table_band_bottom_c(1222.21f, &band_just_above_c);
        TEST_CHECK(st == CONE_TABLE_OK, "band bottom just above cone 6 -> OK");
        float width_just_above = 1222.21f - band_just_above_c;

        st = cone_table_band_bottom_c(1223.00f, &band_further_above_c);
        TEST_CHECK(st == CONE_TABLE_OK, "band bottom further above cone 6 -> OK");
        float width_further_above = 1223.00f - band_further_above_c;

        TEST_CHECK_NEAR(width_below, 18.05f, 1e-2, "band width just below cone 6 == half the 5->6 spacing (18.05 C)");
        TEST_CHECK_NEAR(width_just_above, 8.35f, 1e-2, "band width just above cone 6 == half the 6->7 spacing (8.35 C), not a ~0.005 C cliff");
        TEST_CHECK_NEAR(width_further_above, 8.35f, 1e-2, "band width further above cone 6 == half the 6->7 spacing (8.35 C)");
        TEST_CHECK(width_just_above > 1.0f, "band width just above a tabulated cone is not collapsed to near-zero (the DEFECT 2 symptom)");
    }

    /* Out of range both directions. */
    {
        float band_c;
        cone_table_status_t st = cone_table_band_bottom_c(500.0f, &band_c);
        TEST_CHECK(st == CONE_TABLE_ERR_OUT_OF_RANGE_LOW, "band bottom below lowest cone -> OUT_OF_RANGE_LOW");

        st = cone_table_band_bottom_c(1400.0f, &band_c);
        TEST_CHECK(st == CONE_TABLE_ERR_OUT_OF_RANGE_HIGH, "band bottom above highest cone -> OUT_OF_RANGE_HIGH");

        int idx;
        st = cone_table_cone_for_temp_c(500.0f, &idx);
        TEST_CHECK(st == CONE_TABLE_ERR_OUT_OF_RANGE_LOW, "cone-for-temp below lowest cone -> OUT_OF_RANGE_LOW");

        /* Above the highest cone is explicitly NOT an error for
         * cone_table_cone_for_temp_c() -- documented behaviour. */
        st = cone_table_cone_for_temp_c(1400.0f, &idx);
        TEST_CHECK(st == CONE_TABLE_OK && idx == cone_table_count() - 1,
                   "cone-for-temp above highest cone -> OK, clamped to top index (documented, not an error)");
    }

    /* Weight == 1.0 at target. */
    {
        float w = -1.0f;
        cone_table_status_t st = cone_table_heat_work_weight(1222.2f, 1222.2f, &w);
        TEST_CHECK(st == CONE_TABLE_OK, "weight at target -> OK");
        TEST_CHECK_NEAR(w, 1.0f, 1e-4, "weight == 1.0 exactly at target");

        /* current above target clamps to 1.0 too. */
        st = cone_table_heat_work_weight(1300.0f, 1222.2f, &w);
        TEST_CHECK(st == CONE_TABLE_OK && fabsf(w - 1.0f) < 1e-4, "weight clamps to 1.0 above target");
    }

    /* Weight == 0 at/below band bottom. */
    {
        float band_c;
        cone_table_status_t st = cone_table_band_bottom_c(1222.2f, &band_c);
        TEST_CHECK(st == CONE_TABLE_OK, "band bottom for weight-zero check -> OK");

        float w = -1.0f;
        st = cone_table_heat_work_weight(band_c, 1222.2f, &w);
        TEST_CHECK(st == CONE_TABLE_OK, "weight at band bottom -> OK");
        TEST_CHECK_NEAR(w, 0.0f, 1e-4, "weight == 0.0 exactly at band bottom");

        st = cone_table_heat_work_weight(band_c - 20.0f, 1222.2f, &w);
        TEST_CHECK(st == CONE_TABLE_OK && w == 0.0f, "weight == 0.0 below band bottom");

        /* Pin the shifted-Arrhenius formula itself at an interior point, not
         * just the two boundary values -- otherwise a mutation that drops
         * the "-rate_bottom" normalisation shift (e.g. weight =
         * rate_current/rate_target instead of (rate_current-rate_bottom)/
         * (rate_target-rate_bottom)) still passes both boundary checks
         * above (they short-circuit exactly at/below band_c) while being
         * physically wrong everywhere in between. Hand-computed from the
         * documented formula: Ea=300000 J/mol, R=8.314 J/(mol*K),
         * T = C + 273.15. */
        {
            const float ea = 300000.0f;
            const float r = 8.314f;
            const float abs0 = 273.15f;
            float target_k = 1222.2f + abs0;
            float bottom_k = band_c + abs0;
            float mid_c = band_c + 0.5f * (1222.2f - band_c);
            float mid_k = mid_c + abs0;
            double rate_target = exp(-(double)ea / (r * target_k));
            double rate_bottom = exp(-(double)ea / (r * bottom_k));
            double rate_mid = exp(-(double)ea / (r * mid_k));
            double expected_mid_w = (rate_mid - rate_bottom) / (rate_target - rate_bottom);

            float w_mid = -1.0f;
            st = cone_table_heat_work_weight(mid_c, 1222.2f, &w_mid);
            TEST_CHECK(st == CONE_TABLE_OK, "weight at band midpoint -> OK");
            TEST_CHECK_NEAR(w_mid, expected_mid_w, 1e-3, "weight at band midpoint matches the documented shifted-Arrhenius formula exactly");
        }
    }

    /* Weight monotonically increasing toward the target. */
    {
        float target_c = 1222.2f;
        float band_c;
        cone_table_status_t st = cone_table_band_bottom_c(target_c, &band_c);
        TEST_CHECK(st == CONE_TABLE_OK, "band bottom for monotonicity check -> OK");

        float prev = -1.0f;
        bool monotonic = true;
        int steps = 10;
        for (int i = 0; i <= steps; i++) {
            float frac = (float)i / (float)steps;
            float t = band_c + frac * (target_c - band_c);
            float w = -1.0f;
            cone_table_heat_work_weight(t, target_c, &w);
            if (i > 0 && w < prev - 1e-6f) {
                monotonic = false;
            }
            prev = w;
        }
        TEST_CHECK(monotonic, "weight is monotonically non-decreasing as current_c rises from band bottom to target");
    }

    /* NaN/Inf/zero-input guards. */
    {
        float nan_v = NAN;
        float inf_v = INFINITY;
        float out;
        int idx;

        TEST_CHECK(cone_table_cone_for_temp_c(nan_v, &idx) == CONE_TABLE_ERR_INVALID_INPUT,
                   "cone_for_temp_c(NaN) -> INVALID_INPUT");
        TEST_CHECK(cone_table_cone_for_temp_c(inf_v, &idx) == CONE_TABLE_ERR_INVALID_INPUT,
                   "cone_for_temp_c(Inf) -> INVALID_INPUT");
        TEST_CHECK(cone_table_band_bottom_c(nan_v, &out) == CONE_TABLE_ERR_INVALID_INPUT,
                   "band_bottom_c(NaN) -> INVALID_INPUT");
        TEST_CHECK(cone_table_band_bottom_c(inf_v, &out) == CONE_TABLE_ERR_INVALID_INPUT,
                   "band_bottom_c(Inf) -> INVALID_INPUT");
        TEST_CHECK(cone_table_heat_work_weight(nan_v, 1000.0f, &out) == CONE_TABLE_ERR_INVALID_INPUT,
                   "heat_work_weight(NaN current) -> INVALID_INPUT");
        TEST_CHECK(cone_table_heat_work_weight(1000.0f, inf_v, &out) == CONE_TABLE_ERR_INVALID_INPUT,
                   "heat_work_weight(Inf target) -> INVALID_INPUT");
        TEST_CHECK(cone_table_heat_work_weight(-500.0f, 1000.0f, &out) == CONE_TABLE_ERR_INVALID_INPUT,
                   "heat_work_weight(current below absolute zero) -> INVALID_INPUT");
        TEST_CHECK(cone_table_temp_c_for_cone(NULL, &out) == CONE_TABLE_ERR_INVALID_INPUT,
                   "temp_c_for_cone(NULL label) -> INVALID_INPUT");
        TEST_CHECK(cone_table_get(-1) == NULL, "cone_table_get(-1) -> NULL");
        TEST_CHECK(cone_table_get(cone_table_count()) == NULL, "cone_table_get(count) -> NULL (one past end)");
    }
}
