// sim_factorial_design -- generator only. See the header for scope.
//
// REFERENCE VALUES used to hold non-varied factors fixed within a stage-2
// block (the design doc's §4.3 table says e.g. "at reference A1, A4, A5,
// A6, A8" without pinning down which side of a two-level factor "reference"
// means for A2 -- that is the one genuine ambiguity found in the doc, see
// the audit note this ships with). This file resolves it once, explicitly,
// here:
//   A1 reference = SIM_FAC_A1_ANCHOR   (1.0, the named-anchor value, §3)
//   A2 reference = SIM_FAC_A2_AMPLE    (non-saturating baseline, matching
//                                        the plan's S1/S3 named anchors)
//   A4 reference = SIM_FAC_A4_BENCH_SPAN
//   A5 reference = SIM_FAC_A5_SLOW     (150, the anchor ramp rate)
//   A6 reference = SIM_FAC_A6_MATCHED  (except the tune block, which VARIES A6)
//   A7 reference = SIM_FAC_A7_ANCHOR   (except the curvature block, which VARIES A7)
//   A8 reference = SIM_FAC_A8_ISOTHERMAL (except the high-Bi block, which sets A8=1.5)
#include "sim_factorial_design.h"

#include <stdio.h>

bool sim_factorial_is_masked(float a1_load_mass_mult, sim_fac_a2_headroom_t a2_headroom,
                              float a5_ramp_rate_c_per_hr)
{
    return a1_load_mass_mult >= SIM_FAC_A1_HEAVY && a2_headroom == SIM_FAC_A2_TIGHT &&
           a5_ramp_rate_c_per_hr >= SIM_FAC_A5_FAST;
}

static void fill_cell(sim_factorial_cell_t *c, sim_factorial_stage_t stage, const char *id,
                       float a1, sim_fac_a2_headroom_t a2, float a3, float a4, float a5,
                       sim_fac_a6_tune_t a6, float a7, float a8)
{
    c->stage = stage;
    snprintf(c->cell_id, sizeof(c->cell_id), "%s", id);
    c->a1_load_mass_mult = a1;
    c->a2_headroom = a2;
    c->a3_sensor_bias_p = a3;
    c->a4_loss_scale_span_r_s = a4;
    c->a5_ramp_rate_c_per_hr = a5;
    c->a6_tune = a6;
    c->a7_phi = a7;
    c->a8_bi = a8;
}

size_t sim_factorial_generate(sim_factorial_cell_t *out, size_t out_capacity)
{
    if (out == NULL || out_capacity < SIM_FACTORIAL_MAX_CELLS) {
        return 0;
    }

    size_t n = 0;

    // ---- Stage 1: full 2^8 factorial, masked. ----
    // Fixed nesting order A1..A8, each over its two stage-1 levels, each
    // array traversed low-to-high in a fixed literal order below -- this is
    // the entire determinism guarantee: no set/map/hash, no traversal order
    // that could depend on anything but the source text.
    static const float A1[2] = {SIM_FAC_A1_LIGHT, SIM_FAC_A1_HEAVY};
    static const sim_fac_a2_headroom_t A2[2] = {SIM_FAC_A2_TIGHT, SIM_FAC_A2_AMPLE};
    static const float A3[2] = {SIM_FAC_A3_LOAD_CENTRIC, SIM_FAC_A3_NEAR_ELEMENT};
    static const float A4[2] = {SIM_FAC_A4_BENCH_SPAN, SIM_FAC_A4_KILN_SPAN};
    static const float A5[2] = {SIM_FAC_A5_SLOW, SIM_FAC_A5_FAST};
    static const sim_fac_a6_tune_t A6[2] = {SIM_FAC_A6_MATCHED, SIM_FAC_A6_HOT};
    static const float A7[2] = {SIM_FAC_A7_LOAD_LEAKS, SIM_FAC_A7_ELEMENT_LEAKS};
    static const float A8[2] = {SIM_FAC_A8_ISOTHERMAL, SIM_FAC_A8_GRADIENT};

    size_t raw_index = 0; /* 0..255, over the full unmasked 2^8 -- part of the stable id */
    for (size_t i1 = 0; i1 < 2; i1++) {
        for (size_t i2 = 0; i2 < 2; i2++) {
            for (size_t i3 = 0; i3 < 2; i3++) {
                for (size_t i4 = 0; i4 < 2; i4++) {
                    for (size_t i5 = 0; i5 < 2; i5++) {
                        for (size_t i6 = 0; i6 < 2; i6++) {
                            for (size_t i7 = 0; i7 < 2; i7++) {
                                for (size_t i8 = 0; i8 < 2; i8++) {
                                    float a1 = A1[i1];
                                    sim_fac_a2_headroom_t a2 = A2[i2];
                                    float a3 = A3[i3];
                                    float a4 = A4[i4];
                                    float a5 = A5[i5];
                                    sim_fac_a6_tune_t a6 = A6[i6];
                                    float a7 = A7[i7];
                                    float a8 = A8[i8];

                                    if (!sim_factorial_is_masked(a1, a2, a5)) {
                                        char id[24];
                                        snprintf(id, sizeof(id), "ST1-%03zu", raw_index);
                                        fill_cell(&out[n++], SIM_FACTORIAL_STAGE1, id, a1, a2, a3,
                                                  a4, a5, a6, a7, a8);
                                    }
                                    raw_index++;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // ---- Stage 2, block 1: curvature (18 cells). ----
    // A3(3) x A7(3) x A2(2) at reference A1/A4/A5/A6/A8.
    static const float CURV_A3[3] = {SIM_FAC_A3_LOAD_CENTRIC, SIM_FAC_A3_HALFWAY,
                                      SIM_FAC_A3_NEAR_ELEMENT};
    static const float CURV_A7[3] = {SIM_FAC_A7_LOAD_LEAKS, SIM_FAC_A7_ANCHOR,
                                      SIM_FAC_A7_ELEMENT_LEAKS};
    static const sim_fac_a2_headroom_t CURV_A2[2] = {SIM_FAC_A2_TIGHT, SIM_FAC_A2_AMPLE};
    size_t curv_index = 0;
    for (size_t i3 = 0; i3 < 3; i3++) {
        for (size_t i7 = 0; i7 < 3; i7++) {
            for (size_t i2 = 0; i2 < 2; i2++) {
                char id[24];
                snprintf(id, sizeof(id), "ST2C-%02zu", curv_index++);
                fill_cell(&out[n++], SIM_FACTORIAL_STAGE2_CURVATURE, id, SIM_FAC_A1_ANCHOR,
                          CURV_A2[i2], CURV_A3[i3], SIM_FAC_A4_BENCH_SPAN, SIM_FAC_A5_SLOW,
                          SIM_FAC_A6_MATCHED, CURV_A7[i7], SIM_FAC_A8_ISOTHERMAL);
            }
        }
    }

    // ---- Stage 2, block 2: tune (12 cells). ----
    // A6(4) x A3(3) at reference elsewhere.
    static const sim_fac_a6_tune_t TUNE_A6[4] = {SIM_FAC_A6_MATCHED, SIM_FAC_A6_HOT,
                                                  SIM_FAC_A6_COLD, SIM_FAC_A6_SLOW_INTEGRAL};
    static const float TUNE_A3[3] = {SIM_FAC_A3_LOAD_CENTRIC, SIM_FAC_A3_HALFWAY,
                                      SIM_FAC_A3_NEAR_ELEMENT};
    size_t tune_index = 0;
    for (size_t i6 = 0; i6 < 4; i6++) {
        for (size_t i3 = 0; i3 < 3; i3++) {
            char id[24];
            snprintf(id, sizeof(id), "ST2T-%02zu", tune_index++);
            fill_cell(&out[n++], SIM_FACTORIAL_STAGE2_TUNE, id, SIM_FAC_A1_ANCHOR,
                      SIM_FAC_A2_AMPLE, TUNE_A3[i3], SIM_FAC_A4_BENCH_SPAN, SIM_FAC_A5_SLOW,
                      TUNE_A6[i6], SIM_FAC_A7_ANCHOR, SIM_FAC_A8_ISOTHERMAL);
        }
    }

    // ---- Stage 2, block 3: high-Bi confirmation (9 cells). ----
    // A8=1.5 replicate of the curvature block's 9 A3xA7 cells, at reference A2.
    static const float HIBI_A3[3] = {SIM_FAC_A3_LOAD_CENTRIC, SIM_FAC_A3_HALFWAY,
                                      SIM_FAC_A3_NEAR_ELEMENT};
    static const float HIBI_A7[3] = {SIM_FAC_A7_LOAD_LEAKS, SIM_FAC_A7_ANCHOR,
                                      SIM_FAC_A7_ELEMENT_LEAKS};
    size_t hibi_index = 0;
    for (size_t i3 = 0; i3 < 3; i3++) {
        for (size_t i7 = 0; i7 < 3; i7++) {
            char id[24];
            snprintf(id, sizeof(id), "ST2B-%02zu", hibi_index++);
            fill_cell(&out[n++], SIM_FACTORIAL_STAGE2_HIGH_BI, id, SIM_FAC_A1_ANCHOR,
                      SIM_FAC_A2_AMPLE, HIBI_A3[i3], SIM_FAC_A4_BENCH_SPAN, SIM_FAC_A5_SLOW,
                      SIM_FAC_A6_MATCHED, HIBI_A7[i7], SIM_FAC_A8_GRADIENT);
        }
    }

    return n;
}
