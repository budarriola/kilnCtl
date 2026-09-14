// sim_mistune -- TEST FIXTURE: never shipped; bench values never ship.
//
// WI-3 (docs/SCENARIO_SIMULATION_PLAN.md sec 2.4): gives "a PID tune that
// was never good" a reproducible, interpretable, directional meaning by
// running the project's OWN tuning rule (pid_autotune_tune_from_fopdt(),
// real production code -- LINKED here, never reimplemented) against a
// deliberately wrong {K, tau, L} model, with the wrongness stated as three
// named factors on the TRUE plant's own FOPDT parameters.
//
// The plant itself is never touched -- only the model handed to the tuning
// rule is wrong, exactly as a real bad autotune (run on an unrepresentative
// trace, or hand-guessed) would be.
#ifndef SIM_MISTUNE_H
#define SIM_MISTUNE_H

#include "pid_autotune.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SIM_MISTUNE_MATCHED = 0,   /* 1.0 / 1.0 / 1.0 -- reference, no mismatch */
    SIM_MISTUNE_HOT,           /* 0.5 / 2.0 / 0.5 -- believed sluggish/low-gain -> over-aggressive */
    SIM_MISTUNE_COLD,          /* 2.0 / 0.5 / 2.0 -- believed fast/high-gain -> heavily detuned */
    SIM_MISTUNE_SLOW_INTEGRAL, /* 1.0 / 3.0 / 1.0 -- Ti tracks a 3x-long tau -> integral far too slow */
} sim_mistune_level_t;

typedef struct {
    float mismatch_k;
    float mismatch_tau;
    float mismatch_l;
} sim_mistune_factors_t;

/* The three named factors for a level (sec 2.4's table). Factors are exact
 * powers of 2 and 3 so the resulting gain ratios are exact and reviewable by
 * hand from the SIMC formula -- TEST FIXTURE values, not measured. */
sim_mistune_factors_t sim_mistune_factors(sim_mistune_level_t level);

/* Builds model' = {true_k*mismatch_k, true_tau*mismatch_tau,
 * true_l*mismatch_l, valid=true} and calls
 * pid_autotune_tune_from_fopdt(&model', AUTOTUNE_RULE_SIMC, 0.0f) --
 * REAL production code, not a reimplementation. At SIM_MISTUNE_MATCHED this
 * is exactly a direct call against the true model (all three factors are
 * 1.0), by construction -- not a special case in this function. */
autotune_gains_t sim_mistune_tune(float true_k_c_per_duty, float true_tau_s, float true_l_s,
                                   sim_mistune_level_t level);

#ifdef __cplusplus
}
#endif

#endif // SIM_MISTUNE_H
