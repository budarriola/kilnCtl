// sim_scenario_table -- WI-4 (docs/SCENARIO_SIMULATION.md sec 4.2/4.3).
//
// One row per scenario, DATA ONLY -- no function pointers, no per-scenario
// code anywhere in sim_scenarios.c. Adding a scenario is appending one
// `static const sim_scenario_t` entry to SIM_SCENARIO_TABLE in
// sim_scenario_table.c and bumping SIM_SCENARIO_COUNT_EXPECTED (the
// _Static_assert in the .c file makes that bump a deliberate, reviewed act,
// same posture as FIRING_COMPARE_VOTING_MASK's own _Static_assert). If a new
// scenario genuinely needs new behaviour, that behaviour becomes a new FIELD
// here with a default that leaves every existing row unchanged -- never a
// branch in the runner keyed on scenario id.
//
// WI-4 SCOPE, STATED PLAINLY: this table currently holds S0, S1, S3 only
// (plan sec 7's WI-4 acceptance criterion). S2/S4-S12 are WI-6. Every plant
// constant below is a TEST FIXTURE -- never shipped, never written into
// zones_config, a preset, or a firmware default.
#ifndef SIM_SCENARIO_TABLE_H
#define SIM_SCENARIO_TABLE_H

#include <stdbool.h>

#include "sim_plant.h"

#ifdef __cplusplus
extern "C" {
#endif

// Arm ids -- plan sec 4.1. Selected at RUNTIME from one binary (the
// strength_pct==0 bit-exact contract is what makes this safe -- re-verified
// by this suite itself, not just cited).
typedef enum {
    SIM_ARM_PID = 0,             // fuzzy off, adaptive_tune off. Baseline.
    SIM_ARM_PID_AT,              // fuzzy off, adaptive_tune ON. In sim_scenarios.c's own six-arm table this
                                  // still runs 1 firing, identical to SIM_ARM_PID (notes column says so); the
                                  // real N=9 chained-adaptation sequence (WI-8, DONE) is a separate harness,
                                  // sim_scenarios_adaptive.c/.exe -- see that file's header.
    SIM_ARM_FUZZY25,             // fuzzy strength 25, adaptive_tune off
    SIM_ARM_FUZZY50,             // fuzzy strength 50, adaptive_tune off
    SIM_ARM_FUZZY_AT,            // fuzzy strength 50, adaptive_tune ON (same 1-firing-here/9-firing-in-
                                  // sim_scenarios_adaptive.exe split as SIM_ARM_PID_AT above). There is no
                                  // Ki-withholding state any more -- 88bb4333 removed adaptive_tune_ki.c's
                                  // write path entirely (z->ki_applied is hardcoded false), not merely a
                                  // guard, so the WI-9 "remove the mutual exclusion" work item this comment
                                  // used to point at no longer has a premise and is dropped. Ki now moves, if
                                  // at all, only as a side effect of adaptive_tune_model.c's SIMC recompute
                                  // against a refined K_dc. g_sim_ki_withheld (sim_scenarios.c) is kept as the
                                  // single flag driving ki_state's label, currently always KI_ACTIVE.
    SIM_ARM_STATIC_MATCHED,      // fixed gain multipliers = A_FUZZY50's measured ramp-phase mean (WI-5).
                                  // WI-4 runs this arm with multipliers pinned at 1.0 (== SIM_ARM_PID) until
                                  // WI-5's instrumentation pass lands -- notes column says so explicitly,
                                  // this is NEVER reported as a real A_STATIC_MATCHED result.
    SIM_ARM_COUNT,
} sim_arm_t;

extern const char *const SIM_ARM_NAMES[SIM_ARM_COUNT];

typedef struct {
    const char *id;              // scenario id string, e.g. "S1_BASELINE"

    // Plant. plant.node_model selects SIM_NODE_LEGACY (sim_plant_step()) or
    // SIM_NODE_THREE (sim_plant_three_node_step()) -- the runner dispatches
    // on this field, never on the scenario id.
    sim_plant_cfg_t plant;

    // Aggregate FOPDT the TUNE and the fuzzy bands are derived from. For a
    // "matched" scenario this equals the plant's own true DC gain/tau/dead
    // time; a mismatched-tune scenario (WI-3/WI-6, S7 TUNE_HOT etc.) sets
    // these to a DELIBERATELY WRONG model while the plant keeps the true
    // one -- that mismatch is exactly what those scenarios probe. Not used
    // yet by S0/S1/S3 (all "matched"), present now so WI-6 needs no new field.
    float model_k_dc;
    float model_tau_s;
    float model_dead_time_s;

    float ambient_c;
    float ramp_rate_c_per_hr;    // rising-segment rate; profile is ramp/dwell/ramp/dwell (sec 4.2)
    float t1_offset_c;           // first dwell target, ABOVE ambient
    float t2_offset_c;           // second dwell target, ABOVE ambient

    bool sep_expected;           // plan's pinned SEP? column -- sec 5.3 #2
    const char *sep_reason;      // one-line reason, required whenever the pin is set or changed by hand

    // WI-6 (S10/S11 only): dynamic s(T) conductance scaling and per-segment
    // re-tuning. Both default false/0, which is a no-op for every S0-S9/S12
    // row -- added as fields, not a branch on scenario id, per this file's
    // own "adding a scenario needs no runner change" contract (the runner's
    // dispatch is on these fields, present for every row, not on `id`).
    bool high_temp_dynamic_scale; // apply sim_high_temp_scale_conductances() every tick using load_c
    bool retune_per_segment;      // S11 only: recompute model_k_dc/tau_s (and derived bands/gains) at
                                   // the START temperature of each of the 4 segments, via the same
                                   // s(T) scale used for the plant -- an upper bound on what a perfect
                                   // gain schedule buys. Requires high_temp_dynamic_scale.
    float base_g_ea_w_per_c;      // T_REF-anchored (scale==1.0) base conductances the dynamic scale
    float base_g_la_w_per_c;      // multiplies -- see sim_high_temp.h.
} sim_scenario_t;

// SIM_SCENARIO_COUNT_EXPECTED is pinned in the .c file next to the
// _Static_assert that enforces it -- see that file for why the constant
// lives there instead of here.
//
// sim_scenario_table() returns a pointer to SIM_SCENARIO_COUNT read-only
// rows. It is a function, not a raw extern array, only because
// sim_plant_cfg_t is too large/nested to hand-write as a portable C99
// designated initializer inline in the table without duplicating the
// legacy_bench_plant()/three_node_centre_plant() builder logic per row --
// the returned array is patched exactly once (idempotent) and is stable
// across every call after that, so every caller may treat it as `static
// const` in practice. Call this instead of touching TABLE/g_table directly.
const sim_scenario_t *sim_scenario_table(void);
extern const int SIM_SCENARIO_COUNT;

#ifdef __cplusplus
}
#endif

#endif // SIM_SCENARIO_TABLE_H
