// sim_factorial_design -- TEST FIXTURE: never shipped, never written into
// zones_config, a preset, or a firmware default. Every constant in this
// file's .c counterpart is a design-of-experiments fixture from
// docs/audits/scenario_factorial_design_2026-09-14.md (`75bb7b8e`).
//
// SCOPE: this is the DESIGN GENERATOR ONLY -- it enumerates the factorial
// cell list, applies the one feasibility mask the design doc derives, and
// asserts the resulting counts (224 / 39 / 263). It runs no simulation,
// touches no plant, and drives no arm. The cell-driver (actually running
// each cell through sim_plant.c/pid.c/pid_fuzzy.c) and the effects analysis
// (main-effect/interaction extraction, §5 of the design doc) are separate,
// later pieces of work and are deliberately not built here.
//
// The design doc identifies eight factors (§1.8):
//   A1 load_mass_mult   (bulk time constant tau_L)
//   A2 headroom         (element power headroom, TIGHT/AMPLE)
//   A3 sensor_bias_p    (sensor placement p)
//   A4 loss_scale_span  (R_s = s(Tend)/s(Tstart))
//   A5 ramp_rate        (deg C/hr)
//   A6 tune             (categorical mistune, 2 informative dof)
//   A7 phi              (loss split G_ea/(G_ea+G_la))
//   A8 Bi               ((G_ea+G_la)/G_el)
//
// Stage 1 is a full 2^8 = 256 factorial with A1/A3/A7 at their two EXTREME
// levels (their third, centre level exists only for stage 2 and for the
// plan's named anchors to sit on-grid), masked by the one feasibility
// predicate in §4.2 (A1=heavy AND A2=TIGHT AND A5=fast, 32 cells) to leave
// 224. Stage 2 adds 39 cells across three blocks (§4.3): an 18-cell
// curvature block (A3 x A7 x A2, all three A3/A7 levels), a 12-cell tune
// block (A6 all four categories x A3 three levels), and a 9-cell high-Bi
// confirmation replicate of the curvature block's A3 x A7 grid. Total 263.
#ifndef SIM_FACTORIAL_DESIGN_H
#define SIM_FACTORIAL_DESIGN_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Factor A2: element power headroom (design doc §2).
typedef enum {
    SIM_FAC_A2_TIGHT = 0,
    SIM_FAC_A2_AMPLE,
} sim_fac_a2_headroom_t;

// Factor A6: PID tune mismatch. Categorical, 2 informative degrees of
// freedom per §1.6 (loop-gain error, integral-time error) even though it is
// carried here as 5 named categories.
//
// 2026-09-16 section-8 rebuild (docs/audits/adaptive_fuzzy_section8_cell_mix_rebuild_2026-09-16.md):
// stage 1's screening pair used to be {MATCHED, HOT}, which put NO gain
// error at all into 54.6% of the 260-cell campaign (every MATCHED cell) and
// an error so large in most of the rest (HOT's 2x/0.5x) that the dwell
// residual never settled below adaptive_tune's slope floor. MATCHED is kept
// (used as the reference level for the curvature/high-Bi blocks, which
// intentionally hold tune fixed while varying other factors) but is no
// longer one of stage 1's two varied levels. MILD_HOT replaces it there: a
// real, material gain error (comfortably above
// ADAPTIVE_TUNE_MIN_MATERIAL_MOVE_FRAC = 0.5%) sized to actually settle
// inside the 28*tau adaptive-arm dwell. HOT itself was also softened for the
// same reason -- see tune_mismatch_for() in sim_factorial_driver.c, which is
// the actual source of truth for the multipliers; COLD/SLOW_INTEGRAL remain
// stage-2-only diagnostic cells (§1.6, §4.3's "Tune block").
typedef enum {
    SIM_FAC_A6_MATCHED = 0,
    SIM_FAC_A6_HOT,
    SIM_FAC_A6_COLD,
    SIM_FAC_A6_SLOW_INTEGRAL,
    SIM_FAC_A6_MILD_HOT,
    SIM_FAC_A6_COUNT,
} sim_fac_a6_tune_t;

// Which part of the design a cell belongs to. Not a quality distinction --
// stage 2 cells are just as real as stage 1 -- purely a provenance tag so
// the later effects analysis (§5.3) knows which cells form the balanced
// 2^8 block and which are the targeted curvature/tune/high-Bi additions.
typedef enum {
    SIM_FACTORIAL_STAGE1 = 1,        // full 2^8 factorial, masked: 224 cells
    SIM_FACTORIAL_STAGE2_CURVATURE,  // A3(3) x A7(3) x A2(2) at reference: 18 cells
    SIM_FACTORIAL_STAGE2_TUNE,       // A6(4) x A3(3) at reference: 12 cells
    SIM_FACTORIAL_STAGE2_HIGH_BI,    // A8=1.5 replicate of the 9 A3xA7 cells: 9 cells
} sim_factorial_stage_t;

typedef struct {
    char cell_id[24];
    sim_factorial_stage_t stage;

    // Eight design factors (§1.8/§3). Every field is the literal physical
    // quantity the factor is DEFINED as (a multiplier, a dimensionless
    // ratio, a split fraction) -- never a bare level index -- so a later
    // effects analysis never has to re-derive which physical value a cell
    // used from its position in the design.
    float a1_load_mass_mult;         // tau_L via load_mass_mult: 0.5 / 1.0(anchor) / 3.0
    sim_fac_a2_headroom_t a2_headroom;
    float a3_sensor_bias_p;          // p: 0.0 / 0.5 / 0.8333
    float a4_loss_scale_span_r_s;    // R_s = s(Tend)/s(Tstart): 1.02 / 20.7
    float a5_ramp_rate_c_per_hr;     // 150 / 300 (deg C/hr); 40 is the S0 null, outside this design
    sim_fac_a6_tune_t a6_tune;
    float a7_phi;                    // loss split G_ea/(G_ea+G_la): 0.25 / 0.5(anchor) / 0.75
    float a8_bi;                     // (G_ea+G_la)/G_el: 0.3 / 1.5
} sim_factorial_cell_t;

#define SIM_FACTORIAL_STAGE1_MASKED_COUNT   32
#define SIM_FACTORIAL_STAGE1_COUNT_EXPECTED 224
#define SIM_FACTORIAL_STAGE2_COUNT_EXPECTED 39
#define SIM_FACTORIAL_TOTAL_COUNT_EXPECTED  263
#define SIM_FACTORIAL_MAX_CELLS             SIM_FACTORIAL_TOTAL_COUNT_EXPECTED

// The design doc's own numbers for the level sets, exposed so a test (or a
// later driver) never has to re-paste a literal that lives here.
#define SIM_FAC_A1_LIGHT   0.5f
#define SIM_FAC_A1_ANCHOR  1.0f
#define SIM_FAC_A1_HEAVY   3.0f

#define SIM_FAC_A3_LOAD_CENTRIC  0.0f
#define SIM_FAC_A3_HALFWAY       0.5f
#define SIM_FAC_A3_NEAR_ELEMENT  0.8333f

#define SIM_FAC_A4_BENCH_SPAN  1.02f
#define SIM_FAC_A4_KILN_SPAN   20.7f

#define SIM_FAC_A5_SLOW  150.0f
#define SIM_FAC_A5_FAST  300.0f

#define SIM_FAC_A7_LOAD_LEAKS     0.25f
#define SIM_FAC_A7_ANCHOR         0.5f
#define SIM_FAC_A7_ELEMENT_LEAKS  0.75f

#define SIM_FAC_A8_ISOTHERMAL  0.3f
#define SIM_FAC_A8_GRADIENT    1.5f

// The one feasibility mask (design doc §4.2). The doc's own u_req
// arithmetic for the chosen A2 levels (§2) works out, by hand, to exactly
// this corner regardless of the other five factors -- this predicate is
// that derivation, not a re-guess of it. Returns true iff the cell is
// infeasible (u_req > 0.98 at the top of the ramp) and must be EXCLUDED
// from the stage-1 list, never produced with clamped values.
bool sim_factorial_is_masked(float a1_load_mass_mult, sim_fac_a2_headroom_t a2_headroom,
                              float a5_ramp_rate_c_per_hr);

// Generates the full, ordered cell list -- stage 1's masked 224 followed by
// the three stage-2 blocks (18 + 12 + 9 = 39) -- into out[0 .. return value).
// `out` must have room for at least SIM_FACTORIAL_MAX_CELLS entries.
//
// Deterministic and order-stable: every level is enumerated from fixed
// const arrays in a single fixed nesting order, with no data-, pointer- or
// time-dependent ordering anywhere in the implementation, so two calls in
// the same process (or two separate runs) produce byte-identical cell_id
// sequences and factor levels in the same order every time.
size_t sim_factorial_generate(sim_factorial_cell_t *out, size_t out_capacity);

#ifdef __cplusplus
}
#endif

#endif
