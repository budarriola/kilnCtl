#include "sim_plant.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* See sim_plant.h's SIM_PLANT_ASSERT_ABI_FRESH() comment. sizeof() here is
 * evaluated against THIS TU's own #include of sim_plant.h -- the reference
 * every caller's own sizeof() (passed in via the macro) is checked against. */
void sim_plant_assert_abi_fresh_impl(size_t caller_state_size, size_t caller_cfg_size, const char *caller_file)
{
    size_t here_state_size = sizeof(sim_plant_state_t);
    size_t here_cfg_size = sizeof(sim_plant_cfg_t);
    if (caller_state_size != here_state_size || caller_cfg_size != here_cfg_size) {
        fprintf(stderr,
            "FATAL: sim_plant ABI mismatch between %s and sim_plant.c's own build.\n"
            "  sim_plant_state_t: caller sizeof=%zu, sim_plant.c sizeof=%zu\n"
            "  sim_plant_cfg_t:   caller sizeof=%zu, sim_plant.c sizeof=%zu\n"
            "This means at least one linked .obj was compiled against a DIFFERENT\n"
            "sim_plant.h than the others -- a stale object file, an incremental link\n"
            "against a leftover .obj, or a partial manual revert of sim_plant.c/.h\n"
            "while other sources kept the post-WI-1 header. Delete every build/object\n"
            "directory for this harness and rebuild from a fully clean state before\n"
            "trusting any output. See\n"
            "docs/audits/sim_iter_tune_stale_object_triage_2026-09-14.md.\n",
            caller_file, caller_state_size, here_state_size, caller_cfg_size, here_cfg_size);
        fflush(stderr);
        abort();
    }
}

void sim_plant_reset(sim_plant_state_t *state, const sim_plant_cfg_t *cfg)
{
    memset(state, 0, sizeof(*state));
    state->element_c = cfg->ambient_c;
    state->sensor_c = cfg->ambient_c;
    for (int i = 0; i < SIM_PLANT_DELAY_MAX_STEPS; i++) {
        state->delay_ring[i] = cfg->ambient_c;
    }
    /* WI-1: harmless under SIM_NODE_LEGACY, which never reads these. */
    state->load_c = cfg->ambient_c;
    state->sensor_node_c = cfg->ambient_c;
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
    if (delay_steps >= SIM_PLANT_DELAY_MAX_STEPS) {
        delay_steps = SIM_PLANT_DELAY_MAX_STEPS - 1;
        /* sec 6.1: make the clamp loud. Sticky for the run -- a driver
         * checks this once at run end, not every tick. */
        state->delay_truncated = true;
    }

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

/* ------------------------------ WI-1 ------------------------------------
 * Three-node model (SCENARIO_SIMULATION.md sec 2.1). Opt-in via
 * cfg->node_model == SIM_NODE_THREE; sim_plant_step() above is completely
 * untouched and remains the SIM_NODE_LEGACY path. */
/* Shared by sim_plant_three_node_step() and sim_kiln_step() (credibility-gate
 * dwell-peak work, docs/audits/credibility_gate_dwell_peak_2026-10-07.md):
 * computes the E/L/S deltas from one common snapshot, WITHOUT applying them,
 * so sim_kiln_step() can compute every zone's deltas before mutating any.
 * extra_in_w is power added to the ELEMENT balance beyond the zone's own
 * duty*heater_power_w -- neighbour-duty coupling minus any radiative loss in
 * sim_kiln_step(); 0 for the single-zone step, which keeps that path
 * bit-identical to before this helper existed (adding +0.0f is exact). */
static void three_node_deltas(const sim_plant_state_t *state, const sim_plant_cfg_t *cfg, float duty,
                              float extra_in_w, float dt_s, float *dE, float *dL, float *dS)
{
    if (duty < 0.0f) duty = 0.0f;
    if (duty > 1.0f) duty = 1.0f;

    float E = state->element_c;
    float L = state->load_c;
    float S = state->sensor_node_c;
    float Tamb = cfg->ambient_c;

    float c_l = cfg->c_l_j_per_c * ((cfg->load_mass_mult > 0.0f) ? cfg->load_mass_mult : 1.0f);

    float g_s = (cfg->sensor_tau_s > 0.0f) ? (cfg->c_s_j_per_c / cfg->sensor_tau_s) : 0.0f;
    float g_se = g_s * cfg->sensor_bias_p;
    float g_sl = g_s * (1.0f - cfg->sensor_bias_p);

    float p_in_w = duty * cfg->heater_power_w + extra_in_w;

    *dE = (p_in_w - cfg->g_el_w_per_c * (E - L) - cfg->g_ea_w_per_c * (E - Tamb)) / cfg->c_e_j_per_c * dt_s;
    *dL = (cfg->g_el_w_per_c * (E - L) - cfg->g_la_w_per_c * (L - Tamb)) / c_l * dt_s;
    *dS = (g_se * (E - S) + g_sl * (L - S)) / cfg->c_s_j_per_c * dt_s;
}

void sim_plant_three_node_step(sim_plant_state_t *state, const sim_plant_cfg_t *cfg, float duty, float dt_s)
{
    /* Computed from the common snapshot (E, L, S) -- order of the three
     * balance equations cannot matter because none reads a field already
     * mutated by another. */
    float dE, dL, dS;
    three_node_deltas(state, cfg, duty, 0.0f, dt_s, &dE, &dL, &dS);

    state->element_c += dE;
    state->load_c += dL;
    state->sensor_node_c += dS;

    /* Existing transport-delay ring + first-order sensor lag, reused
     * unchanged, applied to the sensor NODE (S) rather than to the element,
     * per sec 2.1's compatibility contract. */
    sensor_pipeline_step(state, cfg, state->sensor_node_c, dt_s);
}

/* -------------------- WI-1 decomposition helper -------------------------
 * See sim_plant.h's sim_plant_decompose_three_node() comment and
 * docs/audits/three_node_decomposition_helper_2026-09-14.md for the
 * derivation. Summary of the algebra:
 *
 * Steady state of the (E,L) pair at duty=1, Tamb=0 (WLOG, since the model is
 * linear in theta=T-Tamb): with G_a = G_ea+G_la, G_ea=phi*G_a, G_la=(1-phi)*G_a,
 * G_el = G_a/Bi:
 *
 *   K = Pmax / [ G_a * (1 + phi*(1-phi)*Bi) ]
 *
 * Fixing Pmax = k (same free-scale convention as sim_plant_from_zone_cfg())
 * pins G_a = 1 / (1 + phi*(1-phi)*Bi).
 *
 * With C_e = C_l = C, the (E,L) state matrix's characteristic equation in
 * x = C*lambda is:
 *
 *   x^2 + (2*G_el+G_a)*x + (G_el*G_a + G_ea*G_la) = 0
 *
 * whose discriminant reduces algebraically to 4*G_el^2 + (G_ea-G_la)^2 -- a
 * sum of squares, so always >= 0 (real poles, no complex/oscillatory case to
 * refuse) and strictly less than (2*G_el+G_a)^2 whenever G_el>0 (so both
 * roots are strictly negative -- stable). The larger (less negative) root
 * x_slow is the dominant pole; C = -x_slow * tau_s pins the capacity that
 * makes that pole land exactly on the target tau. */
bool sim_plant_decompose_three_node(const sim_plant_decompose_req_t *req, sim_plant_cfg_t *out)
{
    if (!req || !out) return false;
    if (!isfinite(req->k) || !isfinite(req->tau_s) || req->k <= 0.0f || req->tau_s <= 0.0f) return false;
    if (!(req->bi > 0.0f) || !isfinite(req->bi)) return false; /* Bi<=0 -> g_el infinite; refuse, don't clamp */
    if (!isfinite(req->phi) || req->phi < 0.0f || req->phi > 1.0f) return false;

    double bi = (double)req->bi;
    double phi = (double)req->phi;
    double k = (double)req->k;
    double tau = (double)req->tau_s;

    double g_a = 1.0 / (1.0 + phi * (1.0 - phi) * bi);
    double g_el = g_a / bi;
    double g_ea = phi * g_a;
    double g_la = (1.0 - phi) * g_a;

    double b = 2.0 * g_el + g_a;
    double disc = 4.0 * g_el * g_el + (g_ea - g_la) * (g_ea - g_la); /* == b^2 - 4*(g_el*g_a+g_ea*g_la) */
    if (disc < 0.0) return false; /* unreachable given the algebra above; defence in depth */
    double sqrt_disc = sqrt(disc);
    double x_slow = (-b + sqrt_disc) / 2.0;
    if (!(x_slow < 0.0)) return false; /* unreachable given g_el>0; defence in depth */

    double c = -x_slow * tau;
    if (!(c > 0.0) || !isfinite(c)) return false;

    out->heater_power_w = (float)k;
    out->c_e_j_per_c = (float)c;
    out->c_l_j_per_c = (float)c;
    out->g_el_w_per_c = (float)g_el;
    out->g_ea_w_per_c = (float)g_ea;
    out->g_la_w_per_c = (float)g_la;
    return true;
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
    float d_load_c[SIM_KILN_MAX_ZONES] = {0};
    float d_sens_c[SIM_KILN_MAX_ZONES] = {0};

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

        /* Coupling: ADDITIVE SOURCE-GAIN, matching the firmware's model
         * class (zone_coupling_solve.c's G = diag(model_k_dc) + coupling_coeff,
         * off-diagonals ADD and are driven by the NEIGHBOUR'S DUTY -- not a
         * temperature-difference exchange term). This was previously
         * cfg->coupling_w_per_c[i][j] * (T_j - T_i), which is identically
         * zero whenever the zones are at a common temperature -- exactly
         * the operating point a dwell scores at -- and is a different model
         * class from the firmware's, not merely a different fit of the same
         * one. See docs/audits/sim_credibility_gate_real_cause_2026-09-10.md.
         *
         * coupling_w_per_c[i][j] is now W delivered to zone i's element per
         * unit of zone j's commanded duty (u_j in [0,1]) -- i.e. it is
         * numerically the same quantity as coupling_coeff[i][j] (the
         * measured "zone i's rise per unit duty in zone j" cross-gain): a
         * neighbour driven at full duty forever raises this zone by
         * coupling_w_per_c[i][j] / loss_coeff_w_per_c[i], the same shape as
         * a driven zone's own ambient + heater_power_w / loss_coeff. Always
         * non-negative in a physical matrix (a duty can only ever add power,
         * never remove it) and independent of both zones' current
         * temperatures. */
        float power_couple_w = 0.0f;
        for (int j = 0; j < cfg->zone_count; j++) {
            if (j == i) continue;
            float u_j = duty ? duty[j] : 0.0f;
            if (u_j < 0.0f) u_j = 0.0f;
            if (u_j > 1.0f) u_j = 1.0f;
            if (state->fault[j] == SIM_ZONE_FAULT_ELEMENT_DEAD) u_j = 0.0f;
            if (state->fault[j] == SIM_ZONE_FAULT_RELAY_WELDED) u_j = 1.0f;
            power_couple_w += cfg->coupling_w_per_c[i][j] * u_j;
        }

        if (p->node_model == SIM_NODE_THREE) {
            /* Credibility-gate dwell-peak work (2026-10-07): a SIM_NODE_THREE
             * zone runs the E/L/S balance with the same neighbour-duty
             * coupling (and radiative loss) landing on the ELEMENT node, as
             * the legacy branch does. g_ea/g_la replace loss_coeff, so the
             * legacy power_loss_w linear term is NOT applied; only the
             * radiative part is passed through as a negative extra input. */
            float rad_w = 0.0f;
            if (cfg->zone[i].radiative_coeff_w_per_k4 > 0.0f) {
                double t_k3 = (double)t_i + 273.15;
                double amb_k3 = (double)p->ambient_c + 273.15;
                rad_w = (float)((double)cfg->zone[i].radiative_coeff_w_per_k4 *
                                (t_k3 * t_k3 * t_k3 * t_k3 - amb_k3 * amb_k3 * amb_k3 * amb_k3));
            }
            three_node_deltas(&state->zone[i], p, u, power_couple_w - rad_w, dt_s,
                              &d_temp_c[i], &d_load_c[i], &d_sens_c[i]);
        } else {
            d_temp_c[i] = (power_in_w - power_loss_w + power_couple_w) / p->thermal_mass_j_per_c * dt_s;
        }
    }

    /* Pass 2: apply, then run each zone's sensor pipeline against whatever
     * that zone's thermocouple is actually in contact with. */
    for (int i = 0; i < cfg->zone_count; i++) {
        state->zone[i].element_c += d_temp_c[i];
        bool three = (cfg->zone[i].plant.node_model == SIM_NODE_THREE);
        if (three) {
            state->zone[i].load_c += d_load_c[i];
            state->zone[i].sensor_node_c += d_sens_c[i];
        }

        /* A SIM_NODE_THREE zone's thermocouple sits on the sensor node S, not
         * the element (same contract as sim_plant_three_node_step()). */
        float source_c = three ? state->zone[i].sensor_node_c : state->zone[i].element_c;
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
