// thermal_model.c -- see thermal_model.h for the physics and the
// substep/timescale contract (PLAN.md section 4.3).
#include "thermal_model.h"

#include <string.h>

/* PLAN.md 4.3: "the 'fast unit-test kiln' preset ... uses 4 Euler substeps
 * per tick". This is the base substep count at timescale==1; timescale
 * multiplies it. */
#define THERMAL_MODEL_BASE_SUBSTEPS 4u

void thermal_model_load_preset(thermal_preset_id_t preset, thermal_model_params_t *out_params)
{
    memset(out_params, 0, sizeof(*out_params));

    switch (preset) {
    case THERMAL_PRESET_FAST_TEST:
        /* Seconds-scale time constants so a full "firing" completes in
         * about 2 minutes of wall time (PLAN.md 4.3) -- small thermal mass,
         * short TC lag, 3 zones (matches the board's 3 main-side channels). */
        out_params->zone_count = 3;
        out_params->V_mains = 240.0f;
        out_params->T_ambient = 25.0f;
        for (uint8_t i = 0; i < 3; i++) {
            thermal_zone_params_t *z = &out_params->zones[i];
            z->C = 2000.0f;
            z->k_loss = 8.0f;
            z->R_element = 15.0f;
            z->element_health = 1.0f;
            z->tc_lag_s = 3.0f;
            z->T0 = 25.0f;
        }
        /* Light coupling between adjacent zones, symmetric matrix. */
        out_params->zones[0].k_couple[1] = 2.0f;
        out_params->zones[1].k_couple[0] = 2.0f;
        out_params->zones[1].k_couple[2] = 2.0f;
        out_params->zones[2].k_couple[1] = 2.0f;
        break;

    case THERMAL_PRESET_SMALL_KILN:
        /* Single-zone dominant, ~1h scale behavior (PLAN.md 4.3). */
        out_params->zone_count = 1;
        out_params->V_mains = 240.0f;
        out_params->T_ambient = 25.0f;
        out_params->zones[0].C = 400000.0f;
        out_params->zones[0].k_loss = 15.0f;
        out_params->zones[0].R_element = 20.0f;
        out_params->zones[0].element_health = 1.0f;
        out_params->zones[0].tc_lag_s = 20.0f;
        out_params->zones[0].T0 = 25.0f;
        break;

    case THERMAL_PRESET_THREE_ZONE:
        /* Realistic 3-zone with top/middle/bottom coupling and the classic
         * bottom-zone-lags asymmetry (PLAN.md 4.3): bottom has more mass and
         * a longer TC lag than top. */
        out_params->zone_count = 3;
        out_params->V_mains = 240.0f;
        out_params->T_ambient = 25.0f;
        {
            /* zone 0 = top, zone 1 = middle, zone 2 = bottom */
            thermal_zone_params_t *top = &out_params->zones[0];
            thermal_zone_params_t *mid = &out_params->zones[1];
            thermal_zone_params_t *bot = &out_params->zones[2];

            top->C = 250000.0f; top->k_loss = 20.0f; top->R_element = 12.0f;
            top->element_health = 1.0f; top->tc_lag_s = 10.0f; top->T0 = 25.0f;

            mid->C = 300000.0f; mid->k_loss = 15.0f; mid->R_element = 12.0f;
            mid->element_health = 1.0f; mid->tc_lag_s = 15.0f; mid->T0 = 25.0f;

            bot->C = 350000.0f; bot->k_loss = 12.0f; bot->R_element = 12.0f;
            bot->element_health = 1.0f; bot->tc_lag_s = 30.0f; bot->T0 = 25.0f;

            top->k_couple[1] = 5.0f; mid->k_couple[0] = 5.0f;
            mid->k_couple[2] = 5.0f; bot->k_couple[1] = 5.0f;
            top->k_couple[2] = 1.0f; bot->k_couple[0] = 1.0f;
        }
        break;

    case THERMAL_PRESET_STRESS:
    default:
        /* Deliberately awkward: huge lag, weak coupling, low mass -- a
         * preset PID tuning hates, for robustness work not regression
         * (PLAN.md 4.3). */
        out_params->zone_count = 3;
        out_params->V_mains = 240.0f;
        out_params->T_ambient = 25.0f;
        for (uint8_t i = 0; i < 3; i++) {
            thermal_zone_params_t *z = &out_params->zones[i];
            z->C = 50000.0f;
            z->k_loss = 10.0f;
            z->R_element = 15.0f;
            z->element_health = 1.0f;
            z->tc_lag_s = 120.0f;
            z->T0 = 25.0f;
        }
        out_params->zones[0].k_couple[1] = 0.5f;
        out_params->zones[1].k_couple[0] = 0.5f;
        out_params->zones[1].k_couple[2] = 0.5f;
        out_params->zones[2].k_couple[1] = 0.5f;
        break;
    }
}

void thermal_model_init(thermal_model_state_t *state, const thermal_model_params_t *params)
{
    memset(state, 0, sizeof(*state));
    for (uint8_t i = 0; i < params->zone_count && i < THERMAL_MODEL_MAX_ZONES; i++) {
        state->T_zone[i] = params->zones[i].T0;
        /* A sensor sitting at a constant temperature forever reports the
         * truth -- no artificial transient at t=0. */
        state->T_tc[i] = params->zones[i].T0;
    }
}

void thermal_model_tick(thermal_model_state_t *state,
                         const thermal_model_params_t *params,
                         const float *duty,
                         float dt_s,
                         uint32_t timescale)
{
    if (timescale == 0u) {
        timescale = 1u;
    }
    uint32_t substeps = THERMAL_MODEL_BASE_SUBSTEPS * timescale;
    float h = dt_s / (float)substeps;

    uint8_t n = params->zone_count;
    if (n > THERMAL_MODEL_MAX_ZONES) {
        n = THERMAL_MODEL_MAX_ZONES;
    }

    for (uint32_t s = 0; s < substeps; s++) {
        float T_next[THERMAL_MODEL_MAX_ZONES];
        float Ttc_next[THERMAL_MODEL_MAX_ZONES];

        for (uint8_t i = 0; i < n; i++) {
            const thermal_zone_params_t *zp = &params->zones[i];
            float T_i = state->T_zone[i];

            float P_heater = duty[i] * params->V_mains * params->V_mains
                              / zp->R_element * zp->element_health;

            float loss = zp->k_loss * (T_i - params->T_ambient);

            float coupling = 0.0f;
            for (uint8_t j = 0; j < n; j++) {
                if (j == i) {
                    continue;
                }
                coupling += zp->k_couple[j] * (T_i - state->T_zone[j]);
            }

            float dT_dt = (P_heater - loss - coupling) / zp->C;
            T_next[i] = T_i + dT_dt * h;

            /* First-order TC lag: dT_tc/dt = (T_zone - T_tc) / tc_lag_s.
             * tc_lag_s <= 0 means "no lag" -- the reported value tracks the
             * zone instantaneously rather than dividing by zero. */
            if (zp->tc_lag_s > 0.0f) {
                float dTtc_dt = (T_i - state->T_tc[i]) / zp->tc_lag_s;
                Ttc_next[i] = state->T_tc[i] + dTtc_dt * h;
            } else {
                Ttc_next[i] = T_next[i];
            }
        }

        for (uint8_t i = 0; i < n; i++) {
            state->T_zone[i] = T_next[i];
            state->T_tc[i] = Ttc_next[i];
        }
    }
}
