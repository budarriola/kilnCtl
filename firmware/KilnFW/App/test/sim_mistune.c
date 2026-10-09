// sim_mistune -- TEST FIXTURE: never shipped; bench values never ship.
// See sim_mistune.h for what this is and why.
#include "sim_mistune.h"

sim_mistune_factors_t sim_mistune_factors(sim_mistune_level_t level)
{
    switch (level) {
        case SIM_MISTUNE_HOT:
            return (sim_mistune_factors_t){.mismatch_k = 0.5f, .mismatch_tau = 2.0f, .mismatch_l = 0.5f};
        case SIM_MISTUNE_COLD:
            return (sim_mistune_factors_t){.mismatch_k = 2.0f, .mismatch_tau = 0.5f, .mismatch_l = 2.0f};
        case SIM_MISTUNE_SLOW_INTEGRAL:
            return (sim_mistune_factors_t){.mismatch_k = 1.0f, .mismatch_tau = 3.0f, .mismatch_l = 1.0f};
        case SIM_MISTUNE_MATCHED:
        default:
            return (sim_mistune_factors_t){.mismatch_k = 1.0f, .mismatch_tau = 1.0f, .mismatch_l = 1.0f};
    }
}

autotune_gains_t sim_mistune_tune(float true_k_c_per_duty, float true_tau_s, float true_l_s,
                                   sim_mistune_level_t level)
{
    sim_mistune_factors_t f = sim_mistune_factors(level);

    fopdt_model_t model = {0};
    model.k_gain_c_per_duty = true_k_c_per_duty * f.mismatch_k;
    model.tau_s = true_tau_s * f.mismatch_tau;
    model.dead_time_s = true_l_s * f.mismatch_l;
    model.valid = true;

    return pid_autotune_tune_from_fopdt(&model, AUTOTUNE_RULE_SIMC, 0.0f);
}
