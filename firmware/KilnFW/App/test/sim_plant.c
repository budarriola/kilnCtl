#include "sim_plant.h"

#include <math.h>
#include <string.h>

void sim_plant_reset(sim_plant_state_t *state, const sim_plant_cfg_t *cfg)
{
    memset(state, 0, sizeof(*state));
    state->element_c = cfg->ambient_c;
    state->sensor_c = cfg->ambient_c;
    for (int i = 0; i < SIM_PLANT_DELAY_MAX_STEPS; i++) {
        state->delay_ring[i] = cfg->ambient_c;
    }
}

/* Sensor half of the model: transport delay then first-order lag, driven by
 * whatever temperature the thermocouple is actually seeing. Split out from
 * sim_plant_step() so sim_kiln_step() can reuse it with a *different* source
 * temperature per zone -- a detached thermocouple sees chamber air, not the
 * element it is nominally measuring. */
static void sensor_pipeline_step(sim_plant_state_t *state, const sim_plant_cfg_t *cfg, float source_c, float dt_s)
{
    /* Transport delay: push source_c into a ring buffer, read back
     * sensor_delay_s ago. Capacity is fixed (SIM_PLANT_DELAY_MAX_STEPS), so
     * a delay longer than delay_len*dt_s just clamps to the oldest sample
     * available -- acceptable for this module's purpose (falsifying control
     * logic, not modeling transport delay with precision). */
    int delay_steps = (dt_s > 0.0f) ? (int)(cfg->sensor_delay_s / dt_s + 0.5f) : 0;
    if (delay_steps < 0) delay_steps = 0;
    if (delay_steps >= SIM_PLANT_DELAY_MAX_STEPS) delay_steps = SIM_PLANT_DELAY_MAX_STEPS - 1;

    state->delay_ring[state->delay_head] = source_c;
    if (state->delay_len < SIM_PLANT_DELAY_MAX_STEPS) {
        state->delay_len++;
    }
    int read_index = state->delay_head - delay_steps;
    while (read_index < 0) {
        read_index += SIM_PLANT_DELAY_MAX_STEPS;
    }
    float delayed_c = (delay_steps < state->delay_len) ? state->delay_ring[read_index] : cfg->ambient_c;
    state->delay_head = (state->delay_head + 1) % SIM_PLANT_DELAY_MAX_STEPS;

    /* First-order lag on top of the delay -- the thermocouple's own thermal
     * mass smooths a step change rather than reporting it instantly. */
    if (cfg->sensor_lag_tau_s > 0.0f) {
        float alpha = dt_s / (cfg->sensor_lag_tau_s + dt_s);
        state->sensor_c += alpha * (delayed_c - state->sensor_c);
    } else {
        state->sensor_c = delayed_c;
    }
}

void sim_plant_step(sim_plant_state_t *state, const sim_plant_cfg_t *cfg, float duty, float dt_s)
{
    if (duty < 0.0f) duty = 0.0f;
    if (duty > 1.0f) duty = 1.0f;

    /* Element thermal balance: energy in from the heater minus energy lost
     * to ambient, divided by thermal mass -> dT/dt. Forward Euler -- fine at
     * the small dt_s (<=10s) this is meant to be driven at; not intended for
     * dt_s large enough to make this model itself go unstable. */
    float power_in_w = duty * cfg->heater_power_w;
    float power_loss_w = cfg->loss_coeff_w_per_c * (state->element_c - cfg->ambient_c);
    float d_temp_c = (power_in_w - power_loss_w) / cfg->thermal_mass_j_per_c * dt_s;
    state->element_c += d_temp_c;

    sensor_pipeline_step(state, cfg, state->element_c, dt_s);
}

/* ---------------------------- sim_kiln ---------------------------------- */

/* What a thermocouple that fell out of the kiln body but is still wired up
 * actually sees: mostly chamber air, weakly warmed by the element it used to
 * be clamped to. Not zero coupling -- a detached TC dangling in a hot
 * chamber does drift up -- but far too little to track a firing, which is
 * exactly the signature guard 1 exists to catch. */
#define DETACHED_TC_COUPLING 0.05f

void sim_kiln_reset(sim_kiln_state_t *state, sim_kiln_cfg_t *cfg)
{
    memset(state, 0, sizeof(*state));
    state->rng = 0x1234567u;

    if (cfg->zone_count < 0) cfg->zone_count = 0;
    if (cfg->zone_count > SIM_KILN_MAX_ZONES) cfg->zone_count = SIM_KILN_MAX_ZONES;

    /* An all-zero sensor_map is the "caller didn't set one" case, not "every
     * zone reads channel 0" -- fill the identity map. A caller that really
     * wants every zone on one sensor sets it explicitly after reset. */
    bool map_all_zero = true;
    for (int i = 0; i < cfg->zone_count; i++) {
        if (cfg->sensor_map[i] != 0) { map_all_zero = false; break; }
    }
    if (map_all_zero) {
        for (int i = 0; i < SIM_KILN_MAX_ZONES; i++) cfg->sensor_map[i] = i;
    }

    for (int i = 0; i < cfg->zone_count; i++) {
        sim_plant_reset(&state->zone[i], &cfg->zone[i].plant);
    }
}

void sim_kiln_inject_fault(sim_kiln_state_t *state, const sim_kiln_cfg_t *cfg, int zone, sim_zone_fault_t fault)
{
    if (zone < 0 || zone >= cfg->zone_count) return;
    state->fault[zone] = fault;
    if (fault == SIM_ZONE_FAULT_TC_FROZEN) {
        state->frozen_c[zone] = state->zone[zone].sensor_c;
    }
}

void sim_kiln_step(sim_kiln_state_t *state, const sim_kiln_cfg_t *cfg, const float *duty, float dt_s)
{
    float d_temp_c[SIM_KILN_MAX_ZONES] = {0};

    /* Pass 1: every zone's dT computed from the *same* starting temperatures,
     * so the coupling term is symmetric in effect and not order-dependent. */
    for (int i = 0; i < cfg->zone_count; i++) {
        const sim_plant_cfg_t *p = &cfg->zone[i].plant;

        float u = duty ? duty[i] : 0.0f;
        if (u < 0.0f) u = 0.0f;
        if (u > 1.0f) u = 1.0f;
        if (state->fault[i] == SIM_ZONE_FAULT_ELEMENT_DEAD) u = 0.0f;
        if (state->fault[i] == SIM_ZONE_FAULT_RELAY_WELDED) u = 1.0f;

        float t_i = state->zone[i].element_c;
        float power_in_w = u * p->heater_power_w;
        float power_loss_w = p->loss_coeff_w_per_c * (t_i - p->ambient_c);

        /* Radiative loss: the reason a kiln's plant gain collapses at cone
         * temperature and a tuning fitted at 200C is wrong at 1200C
         * (TODO.md 6A.4's gain-scheduling rationale). Stefan-Boltzmann in
         * absolute temperature, folded into one coefficient. */
        if (cfg->zone[i].radiative_coeff_w_per_k4 > 0.0f) {
            double t_k = (double)t_i + 273.15;
            double amb_k = (double)p->ambient_c + 273.15;
            power_loss_w += (float)((double)cfg->zone[i].radiative_coeff_w_per_k4 *
                                    (t_k * t_k * t_k * t_k - amb_k * amb_k * amb_k * amb_k));
        }

        float power_couple_w = 0.0f;
        for (int j = 0; j < cfg->zone_count; j++) {
            if (j == i) continue;
            power_couple_w += cfg->coupling_w_per_c[i][j] * (state->zone[j].element_c - t_i);
        }

        d_temp_c[i] = (power_in_w - power_loss_w + power_couple_w) / p->thermal_mass_j_per_c * dt_s;
    }

    /* Pass 2: apply, then run each zone's sensor pipeline against whatever
     * that zone's thermocouple is actually in contact with. */
    for (int i = 0; i < cfg->zone_count; i++) {
        state->zone[i].element_c += d_temp_c[i];

        float source_c = state->zone[i].element_c;
        if (state->fault[i] == SIM_ZONE_FAULT_TC_DETACHED) {
            float ambient_c = cfg->zone[i].plant.ambient_c;
            source_c = ambient_c + DETACHED_TC_COUPLING * (state->zone[i].element_c - ambient_c);
        }
        sensor_pipeline_step(&state->zone[i], &cfg->zone[i].plant, source_c, dt_s);
    }

    /* One LCG advance per tick -- see sim_kiln_state_t.rng's comment. */
    state->rng = state->rng * 1664525u + 1013904223u;
}

float sim_kiln_reading_c(const sim_kiln_state_t *state, const sim_kiln_cfg_t *cfg, int zone)
{
    if (zone < 0 || zone >= cfg->zone_count) return NAN;

    int physical = cfg->sensor_map[zone];
    if (physical < 0 || physical >= cfg->zone_count) return NAN;

    if (state->fault[physical] == SIM_ZONE_FAULT_TC_OPEN) return NAN;

    float value_c = (state->fault[physical] == SIM_ZONE_FAULT_TC_FROZEN)
                        ? state->frozen_c[physical]
                        : state->zone[physical].sensor_c;

    /* A frozen sensor must read bit-identical for guard 7 to see it, so
     * noise is deliberately not applied to that case. */
    if (cfg->sensor_noise_c > 0.0f && state->fault[physical] != SIM_ZONE_FAULT_TC_FROZEN) {
        uint32_t h = state->rng ^ (0x9e3779b9u * (uint32_t)(physical + 1));
        h ^= h >> 15;
        h *= 0x2c1b3c6du;
        h ^= h >> 12;
        float unit = (float)(h & 0xffffu) / 65535.0f; /* [0,1] */
        value_c += (unit - 0.5f) * cfg->sensor_noise_c;
    }
    return value_c;
}

float sim_kiln_element_c(const sim_kiln_state_t *state, int zone)
{
    if (zone < 0 || zone >= SIM_KILN_MAX_ZONES) return NAN;
    return state->zone[zone].element_c;
}

/* --------------------------------- G1 ------------------------------------ */

bool sim_plant_from_zone_cfg(const zone_cfg_t *zcfg, float ambient_c, sim_plant_cfg_t *out)
{
    if (!zcfg || !out) return false;
    float k_dc = zcfg->model_k_dc;
    float tau_s = zcfg->model_tau_s;
    float dead_time_s = zcfg->model_dead_time_s;

    if (!isfinite(k_dc) || !isfinite(tau_s) || !isfinite(dead_time_s) ||
        k_dc <= 0.0f || tau_s <= 0.0f || dead_time_s < 0.0f) {
        return false;
    }

    out->ambient_c = ambient_c;
    out->heater_power_w = k_dc;
    out->thermal_mass_j_per_c = tau_s;
    out->loss_coeff_w_per_c = 1.0f; /* the free scale h -- see header comment */
    out->sensor_delay_s = dead_time_s;
    out->sensor_lag_tau_s = 0.0f; /* dead_time_s already lumps sensor lag */
    return true;
}

void sim_kiln_coupling_from_cross_gain(int zone_count,
                                        const float coupling_coeff[SIM_KILN_MAX_ZONES][SIM_KILN_MAX_ZONES],
                                        const float k_dc[SIM_KILN_MAX_ZONES],
                                        float out_coupling_w_per_c[SIM_KILN_MAX_ZONES][SIM_KILN_MAX_ZONES])
{
    if (zone_count < 0) zone_count = 0;
    if (zone_count > SIM_KILN_MAX_ZONES) zone_count = SIM_KILN_MAX_ZONES;

    for (int i = 0; i < SIM_KILN_MAX_ZONES; i++) {
        for (int j = 0; j < SIM_KILN_MAX_ZONES; j++) {
            out_coupling_w_per_c[i][j] = 0.0f;
        }
    }
    for (int i = 0; i < zone_count; i++) {
        for (int j = 0; j < zone_count; j++) {
            if (i == j) continue;
            if (k_dc[j] == 0.0f) continue; /* undefined; leave at 0 rather than divide */
            /* h_i == 1.0 (sim_plant_from_zone_cfg()'s own free-scale choice) */
            out_coupling_w_per_c[i][j] = coupling_coeff[i][j] / k_dc[j];
        }
    }
}

/* Runs one synthetic step test in a fresh sim_kiln (zone driven_j at
 * duty=1.0 from rest, everyone else at duty=0.0) using coupling_w_per_c as
 * given, for settle_s of sim time, and returns every zone's element rise
 * above ambient at the end -- the simulated counterpart of a single column
 * of coupling_coeff[][]. No radiative term (the bench coupling capture that
 * produced coupling_coeff[][] was a small-signal low-temperature test, same
 * posture as sim_plant_from_zone_cfg()'s own FOPDT mapping). */
static void measure_sim_cross_gain_column(int zone_count, int driven_j,
                                           const float k_dc[SIM_KILN_MAX_ZONES],
                                           const float tau_s[SIM_KILN_MAX_ZONES],
                                           const float dead_time_s[SIM_KILN_MAX_ZONES],
                                           const float coupling_w_per_c[SIM_KILN_MAX_ZONES][SIM_KILN_MAX_ZONES],
                                           float ambient_c, float dt_s, float settle_s,
                                           float out_rise_c[SIM_KILN_MAX_ZONES])
{
    sim_kiln_cfg_t kcfg;
    memset(&kcfg, 0, sizeof(kcfg));
    kcfg.zone_count = zone_count;
    for (int i = 0; i < zone_count; i++) {
        kcfg.zone[i].plant.ambient_c = ambient_c;
        kcfg.zone[i].plant.heater_power_w = k_dc[i];
        kcfg.zone[i].plant.thermal_mass_j_per_c = tau_s[i];
        kcfg.zone[i].plant.loss_coeff_w_per_c = 1.0f; /* h == 1, sim_plant_from_zone_cfg()'s own choice */
        kcfg.zone[i].plant.sensor_delay_s = dead_time_s[i];
        kcfg.zone[i].plant.sensor_lag_tau_s = 0.0f;
        for (int j = 0; j < zone_count; j++) {
            kcfg.coupling_w_per_c[i][j] = coupling_w_per_c[i][j];
        }
    }

    sim_kiln_state_t st;
    sim_kiln_reset(&st, &kcfg);

    float duty[SIM_KILN_MAX_ZONES] = {0};
    duty[driven_j] = 1.0f;

    int steps = (dt_s > 0.0f) ? (int)(settle_s / dt_s + 0.5f) : 0;
    for (int s = 0; s < steps; s++) {
        sim_kiln_step(&st, &kcfg, duty, dt_s);
    }

    for (int i = 0; i < zone_count; i++) {
        out_rise_c[i] = sim_kiln_element_c(&st, i) - ambient_c;
    }
}

int sim_kiln_coupling_fit_iterative(int zone_count,
                                     const float coupling_coeff[SIM_KILN_MAX_ZONES][SIM_KILN_MAX_ZONES],
                                     const float k_dc[SIM_KILN_MAX_ZONES],
                                     const float tau_s[SIM_KILN_MAX_ZONES],
                                     const float dead_time_s[SIM_KILN_MAX_ZONES],
                                     float ambient_c,
                                     float rel_tol,
                                     int max_iters,
                                     float out_coupling_w_per_c[SIM_KILN_MAX_ZONES][SIM_KILN_MAX_ZONES],
                                     bool *out_converged)
{
    if (zone_count < 0) zone_count = 0;
    if (zone_count > SIM_KILN_MAX_ZONES) zone_count = SIM_KILN_MAX_ZONES;
    if (rel_tol <= 0.0f) rel_tol = 0.10f;
    if (max_iters < 1) max_iters = 1;

    /* Seed from the algebraic first cut -- the starting point sec 6.2 itself
     * names, not an arbitrary guess. */
    sim_kiln_coupling_from_cross_gain(zone_count, coupling_coeff, k_dc, out_coupling_w_per_c);

    /* Settle time: the slowest zone's dead time plus several time constants,
     * so the driven zone and every coupled zone are within noise of their
     * true steady state, not still visibly ramping. */
    float slowest_tau = 0.0f, slowest_dead = 0.0f;
    for (int i = 0; i < zone_count; i++) {
        if (tau_s[i] > slowest_tau) slowest_tau = tau_s[i];
        if (dead_time_s[i] > slowest_dead) slowest_dead = dead_time_s[i];
    }
    float settle_s = slowest_dead + 8.0f * slowest_tau;
    if (settle_s < 60.0f) settle_s = 60.0f;
    const float dt_s = 1.0f;

    int sweep;
    bool converged = false;
    for (sweep = 0; sweep < max_iters; sweep++) {
        float max_rel_err = 0.0f;

        for (int j = 0; j < zone_count; j++) {
            float rise[SIM_KILN_MAX_ZONES];
            measure_sim_cross_gain_column(zone_count, j, k_dc, tau_s, dead_time_s,
                                           out_coupling_w_per_c, ambient_c, dt_s, settle_s, rise);

            for (int i = 0; i < zone_count; i++) {
                if (i == j) continue;
                float target = coupling_coeff[i][j];
                float sim = rise[i];

                /* Undefined target (no measured coupling) or a driven zone
                 * that develops no rise of its own (k_dc[j] <= 0, guarded by
                 * sim_kiln_coupling_from_cross_gain() leaving it at 0) --
                 * nothing to fit, leave the conductance at 0. */
                if (target == 0.0f) continue;

                /* A prior sweep's correction (possibly to a DIFFERENT pair
                 * sharing zone i or j) can in principle push this simulated
                 * step test into instability before this entry's own value
                 * has had a chance to settle. Never trust a non-finite
                 * reading: back this one entry off (not the whole matrix)
                 * and force another sweep -- do not let a non-finite rel_err
                 * silently fail to update max_rel_err (NaN compares false
                 * against everything, which would let a diverged entry slip
                 * through as "no worse than the finite ones" and get
                 * reported CONVERGED). */
                if (!isfinite(sim)) {
                    max_rel_err = INFINITY;
                    out_coupling_w_per_c[i][j] *= 0.5f;
                    continue;
                }

                float rel_err = fabsf(sim - target) / fabsf(target);
                if (rel_err > max_rel_err) max_rel_err = rel_err;
                if (rel_err <= rel_tol) continue;

                /* Damped proportional correction on the conductance: the
                 * map from conductance to simulated steady-state cross-gain
                 * is monotonic and close to linear for small corrections
                 * (the loading effect that makes the algebraic cut wrong is
                 * itself roughly proportional), so scaling the conductance
                 * by target/sim converges in a handful of sweeps without
                 * needing a real secant/Newton step. Damping (0.5) and a
                 * tight per-sweep multiplier clamp keep this from compounding
                 * into an Euler-unstable conductance across sweeps -- an
                 * earlier, looser clamp (0.1x-5x) let one pair's correction
                 * reach a magnitude that made sim_kiln_step()'s explicit
                 * Euler integration diverge to NaN within a few sweeps. */
                if (sim != 0.0f) {
                    float ratio = target / sim;
                    float damped = 1.0f + 0.5f * (ratio - 1.0f);
                    if (damped < 0.5f) damped = 0.5f;   /* never flip sign or collapse in one step */
                    if (damped > 2.0f) damped = 2.0f;   /* never more than double in one step */
                    out_coupling_w_per_c[i][j] *= damped;
                } else if (out_coupling_w_per_c[i][j] <= 0.0f) {
                    /* Zero conductance producing zero rise: nudge off zero
                     * using the algebraic estimate as a seed so subsequent
                     * sweeps have something nonzero to scale. */
                    out_coupling_w_per_c[i][j] = (k_dc[j] > 0.0f) ? (target / k_dc[j]) : 0.0f;
                }
                if (out_coupling_w_per_c[i][j] < 0.0f) out_coupling_w_per_c[i][j] = 0.0f;
                /* Hard ceiling under the pair's Euler-stability bound: zone
                 * i's own balance effectively gains a loss term of size
                 * out_coupling_w_per_c[i][j], so explicit Euler at dt_s needs
                 * dt_s*(h_i + sum_j g_ij)/tau_i < 2 to avoid diverging.
                 * With this zone set's tau_s (>=114.8s) and dt_s=1s, up to
                 * ~200 W/C total stays comfortably inside that bound even
                 * before the other off-diagonal entries in row i are
                 * counted; 200 is NOT a belief about the true conductance,
                 * only a numerical safety rail. */
                if (out_coupling_w_per_c[i][j] > 200.0f) out_coupling_w_per_c[i][j] = 200.0f;
            }
        }

        if (max_rel_err <= rel_tol) {
            converged = true;
            sweep++; /* count this sweep as run */
            break;
        }
    }

    if (out_converged) *out_converged = converged;
    return sweep;
}

/* --------------------------------- G3 ------------------------------------ */

bool sim_relay_lag_step(sim_relay_lag_t *state, bool commanded, float lag_s, float dt_s)
{
    int lag_steps = (dt_s > 0.0f) ? (int)(lag_s / dt_s + 0.5f) : 0;
    if (lag_steps < 0) lag_steps = 0;
    if (lag_steps >= SIM_RELAY_LAG_RING_MAX) lag_steps = SIM_RELAY_LAG_RING_MAX - 1;

    state->ring[state->head] = commanded;
    if (state->len < SIM_RELAY_LAG_RING_MAX) state->len++;
    int read_idx = state->head - lag_steps;
    while (read_idx < 0) read_idx += SIM_RELAY_LAG_RING_MAX;
    bool out = (lag_steps < state->len) ? state->ring[read_idx] : false;
    state->head = (state->head + 1) % SIM_RELAY_LAG_RING_MAX;
    return out;
}

/* --------------------------------- G4 ------------------------------------ */

float sim_max31856_quantize_tc(float temperature_c)
{
    if (!isfinite(temperature_c)) return temperature_c;
    return roundf(temperature_c / SIM_MAX31856_TC_RESOLUTION_C) * SIM_MAX31856_TC_RESOLUTION_C;
}
