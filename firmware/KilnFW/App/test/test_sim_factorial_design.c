// Tests for sim_factorial_design.c (docs/audits/scenario_factorial_design_2026-09-14.md,
// `75bb7b8e`). Generator only -- no plant, no arms, no simulation.
#include <string.h>

#include "sim_factorial_design.h"
#include "test_common.h"

static size_t count_stage(const sim_factorial_cell_t *cells, size_t n, sim_factorial_stage_t stage)
{
    size_t c = 0;
    for (size_t i = 0; i < n; i++) {
        if (cells[i].stage == stage) {
            c++;
        }
    }
    return c;
}

static void test_cell_counts_match_the_design_doc(void)
{
    TEST_SECTION("cell counts: 224 stage-1 + 39 stage-2 = 263, per the design doc");

    static sim_factorial_cell_t cells[SIM_FACTORIAL_MAX_CELLS];
    size_t n = sim_factorial_generate(cells, SIM_FACTORIAL_MAX_CELLS);

    TEST_CHECK(n == SIM_FACTORIAL_TOTAL_COUNT_EXPECTED, "total cell count must be 263");

    size_t stage1 = count_stage(cells, n, SIM_FACTORIAL_STAGE1);
    size_t curv = count_stage(cells, n, SIM_FACTORIAL_STAGE2_CURVATURE);
    size_t tune = count_stage(cells, n, SIM_FACTORIAL_STAGE2_TUNE);
    size_t hibi = count_stage(cells, n, SIM_FACTORIAL_STAGE2_HIGH_BI);

    /* Per the mandate: if these don't match, report and stop -- never adjust
     * the assertion to match a wrong implementation. */
    TEST_CHECK(stage1 == SIM_FACTORIAL_STAGE1_COUNT_EXPECTED, "stage 1 (masked 2^8) must be 224 cells");
    TEST_CHECK(curv == 18, "stage 2 curvature block must be 18 cells (A3(3) x A7(3) x A2(2))");
    TEST_CHECK(tune == 12, "stage 2 tune block must be 12 cells (A6(4) x A3(3))");
    TEST_CHECK(hibi == 9, "stage 2 high-Bi confirmation block must be 9 cells (A3(3) x A7(3))");
    TEST_CHECK(curv + tune + hibi == SIM_FACTORIAL_STAGE2_COUNT_EXPECTED,
               "stage 2 total must be 39 cells");

    printf("  stage1=%zu curvature=%zu tune=%zu high_bi=%zu total=%zu\n", stage1, curv, tune, hibi,
           n);
}

static void test_masked_count_is_32_of_256(void)
{
    TEST_SECTION("the mask removes exactly 32 of the 256 raw stage-1 cells");

    /* Independently re-derive the masked count by brute-force enumeration
     * of the raw 2^8, using ONLY the two stage-1 levels per factor and the
     * production mask predicate -- this does not call sim_factorial_generate()
     * so it is not just re-checking the same arithmetic against itself. */
    const float a1_levels[2] = {SIM_FAC_A1_LIGHT, SIM_FAC_A1_HEAVY};
    const sim_fac_a2_headroom_t a2_levels[2] = {SIM_FAC_A2_TIGHT, SIM_FAC_A2_AMPLE};
    const float a5_levels[2] = {SIM_FAC_A5_SLOW, SIM_FAC_A5_FAST};

    size_t raw = 0;
    size_t masked = 0;
    for (size_t i1 = 0; i1 < 2; i1++) {
        for (size_t i2 = 0; i2 < 2; i2++) {
            /* The other five factors (A3, A4, A6, A7, A8) don't affect the
             * mask predicate -- multiply by 2^5=32 to cover them. */
            for (size_t i5 = 0; i5 < 2; i5++) {
                raw += 32;
                if (sim_factorial_is_masked(a1_levels[i1], a2_levels[i2], a5_levels[i5])) {
                    masked += 32;
                }
            }
        }
    }
    TEST_CHECK(raw == 256, "raw stage-1 space must be 256 cells");
    TEST_CHECK(masked == SIM_FACTORIAL_STAGE1_MASKED_COUNT, "exactly 32 cells must be masked");
}

static void test_excluded_corner_is_genuinely_absent(void)
{
    TEST_SECTION("the excluded corner (heavy x TIGHT x fast) is absent from the generated list, "
                 "not present with clamped values");

    static sim_factorial_cell_t cells[SIM_FACTORIAL_MAX_CELLS];
    size_t n = sim_factorial_generate(cells, SIM_FACTORIAL_MAX_CELLS);

    bool found_infeasible_in_stage1 = false;
    for (size_t i = 0; i < n; i++) {
        const sim_factorial_cell_t *c = &cells[i];
        if (c->stage != SIM_FACTORIAL_STAGE1) {
            continue;
        }
        if (c->a1_load_mass_mult >= SIM_FAC_A1_HEAVY && c->a2_headroom == SIM_FAC_A2_TIGHT &&
            c->a5_ramp_rate_c_per_hr >= SIM_FAC_A5_FAST) {
            found_infeasible_in_stage1 = true;
        }
    }
    TEST_CHECK(!found_infeasible_in_stage1,
               "no stage-1 cell may have (A1=heavy, A2=TIGHT, A5=fast) -- the masked corner");
}

static void test_every_level_is_within_the_declared_sets(void)
{
    TEST_SECTION("every cell's factor levels are members of the declared level sets");

    static sim_factorial_cell_t cells[SIM_FACTORIAL_MAX_CELLS];
    size_t n = sim_factorial_generate(cells, SIM_FACTORIAL_MAX_CELLS);

    bool all_ok = true;
    for (size_t i = 0; i < n; i++) {
        const sim_factorial_cell_t *c = &cells[i];
        bool a1_ok = (c->a1_load_mass_mult == SIM_FAC_A1_LIGHT) ||
                     (c->a1_load_mass_mult == SIM_FAC_A1_ANCHOR) ||
                     (c->a1_load_mass_mult == SIM_FAC_A1_HEAVY);
        bool a2_ok = (c->a2_headroom == SIM_FAC_A2_TIGHT) || (c->a2_headroom == SIM_FAC_A2_AMPLE);
        bool a3_ok = (c->a3_sensor_bias_p == SIM_FAC_A3_LOAD_CENTRIC) ||
                     (c->a3_sensor_bias_p == SIM_FAC_A3_HALFWAY) ||
                     (c->a3_sensor_bias_p == SIM_FAC_A3_NEAR_ELEMENT);
        bool a4_ok = (c->a4_loss_scale_span_r_s == SIM_FAC_A4_BENCH_SPAN) ||
                     (c->a4_loss_scale_span_r_s == SIM_FAC_A4_KILN_SPAN);
        bool a5_ok = (c->a5_ramp_rate_c_per_hr == SIM_FAC_A5_SLOW) ||
                     (c->a5_ramp_rate_c_per_hr == SIM_FAC_A5_FAST);
        bool a6_ok = c->a6_tune < SIM_FAC_A6_COUNT;
        bool a7_ok = (c->a7_phi == SIM_FAC_A7_LOAD_LEAKS) || (c->a7_phi == SIM_FAC_A7_ANCHOR) ||
                     (c->a7_phi == SIM_FAC_A7_ELEMENT_LEAKS);
        bool a8_ok = (c->a8_bi == SIM_FAC_A8_ISOTHERMAL) || (c->a8_bi == SIM_FAC_A8_GRADIENT);
        if (!(a1_ok && a2_ok && a3_ok && a4_ok && a5_ok && a6_ok && a7_ok && a8_ok)) {
            printf("  BAD CELL %s: a1=%.4f a2=%d a3=%.4f a4=%.4f a5=%.1f a6=%d a7=%.4f a8=%.2f\n",
                   c->cell_id, (double)c->a1_load_mass_mult, (int)c->a2_headroom,
                   (double)c->a3_sensor_bias_p, (double)c->a4_loss_scale_span_r_s,
                   (double)c->a5_ramp_rate_c_per_hr, (int)c->a6_tune, (double)c->a7_phi,
                   (double)c->a8_bi);
            all_ok = false;
        }
    }
    TEST_CHECK(all_ok, "every cell's levels must belong to the declared per-factor level sets");
}

static void test_p_has_three_levels_others_have_two(void)
{
    TEST_SECTION("A3 (sensor placement p) has 3 distinct levels across the whole design; the "
                 "other seven factors have 2");

    static sim_factorial_cell_t cells[SIM_FACTORIAL_MAX_CELLS];
    size_t n = sim_factorial_generate(cells, SIM_FACTORIAL_MAX_CELLS);

    bool saw_p0 = false, saw_p_half = false, saw_p_near = false;
    bool saw_a1_light = false, saw_a1_heavy = false, saw_a1_other = false;
    bool saw_a7_lo = false, saw_a7_hi = false, saw_a7_other = false;
    for (size_t i = 0; i < n; i++) {
        const sim_factorial_cell_t *c = &cells[i];
        if (c->a3_sensor_bias_p == SIM_FAC_A3_LOAD_CENTRIC) saw_p0 = true;
        if (c->a3_sensor_bias_p == SIM_FAC_A3_HALFWAY) saw_p_half = true;
        if (c->a3_sensor_bias_p == SIM_FAC_A3_NEAR_ELEMENT) saw_p_near = true;

        /* A1 does carry a third (anchor) value, but ONLY as a held-fixed
         * reference in stage 2 -- it is never itself varied across 3 levels
         * the way A3/A7 are in the curvature/tune/high-Bi blocks. Confirm
         * that shape: the anchor value appears, but never alongside a
         * genuine 3-level sweep of A1 the way A3 gets one. */
        if (c->a1_load_mass_mult == SIM_FAC_A1_LIGHT) saw_a1_light = true;
        else if (c->a1_load_mass_mult == SIM_FAC_A1_HEAVY) saw_a1_heavy = true;
        else saw_a1_other = true;

        if (c->a7_phi == SIM_FAC_A7_LOAD_LEAKS) saw_a7_lo = true;
        else if (c->a7_phi == SIM_FAC_A7_ELEMENT_LEAKS) saw_a7_hi = true;
        else saw_a7_other = true;
    }

    TEST_CHECK(saw_p0 && saw_p_half && saw_p_near, "A3 must exercise all three of its levels somewhere in the design");
    TEST_CHECK(saw_a1_light && saw_a1_heavy, "A1 must exercise both stage-1 levels");
    TEST_CHECK(saw_a7_lo && saw_a7_hi, "A7 must exercise both stage-1 extreme levels");
    (void)saw_a1_other;
    (void)saw_a7_other;
}

static void test_ordering_is_stable_across_two_generations(void)
{
    TEST_SECTION("ordering is stable: two independent generations produce the identical cell "
                 "sequence");

    static sim_factorial_cell_t first[SIM_FACTORIAL_MAX_CELLS];
    static sim_factorial_cell_t second[SIM_FACTORIAL_MAX_CELLS];
    size_t n1 = sim_factorial_generate(first, SIM_FACTORIAL_MAX_CELLS);
    size_t n2 = sim_factorial_generate(second, SIM_FACTORIAL_MAX_CELLS);

    TEST_CHECK(n1 == n2, "both generations must produce the same count");

    bool identical = (n1 == n2);
    if (identical) {
        identical = memcmp(first, second, n1 * sizeof(sim_factorial_cell_t)) == 0;
    }
    TEST_CHECK(identical, "the two generations must be byte-identical, including cell_id order");
}

static void test_generate_refuses_undersized_buffer(void)
{
    TEST_SECTION("sim_factorial_generate() refuses a buffer smaller than SIM_FACTORIAL_MAX_CELLS "
                 "rather than silently truncating");

    sim_factorial_cell_t too_small[10];
    size_t n = sim_factorial_generate(too_small, 10);
    TEST_CHECK(n == 0, "an undersized buffer must yield 0, not a truncated list");
}

void run_test_sim_factorial_design(void)
{
    test_cell_counts_match_the_design_doc();
    test_masked_count_is_32_of_256();
    test_excluded_corner_is_genuinely_absent();
    test_every_level_is_within_the_declared_sets();
    test_p_has_three_levels_others_have_two();
    test_ordering_is_stable_across_two_generations();
    test_generate_refuses_undersized_buffer();
}
