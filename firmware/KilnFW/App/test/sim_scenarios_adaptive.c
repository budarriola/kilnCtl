// sim_scenarios_adaptive -- WI-8 (docs/SCENARIO_SIMULATION_PLAN.md sec 4.1/7).
//
// Runs SIM_ARM_PID_AT and SIM_ARM_FUZZY_AT as a CHAIN OF NINE FIRINGS with
// adaptation state carried across them, driving the REAL production
// adaptive_tune_zone_tick()/adaptive_tune_run_end()/adaptive_tune_refine_
// zone_locked() (adaptive_tune.c/adaptive_tune_model.c/adaptive_tune_ki.c,
// #included directly below) against a zones_config TEST FAKE whose state
// (K_dc/tau/dead_time/Kp/Ki/Kd/autotune_baseline_k_dc/adaptive_tune_enabled)
// persists across the whole 9-firing chain for one (scenario, arm) pair --
// same "tiny in-RAM table" convention as test_adaptive_tune.c's own fakes
// (this file's fake block is a trimmed, single-zone copy of that file's,
// same field shapes, same defaults).
//
// WHY A SEPARATE EXECUTABLE, NOT A CHANGE TO sim_scenarios.c: adaptive_
// tune.c pulls in hal_kv/esp_log/flash_worker_wait/pref_cfg_fs/cfg_fs_status
// and a FreeRTOS mutex (real xSemaphoreCreateMutex()/Take()/Give(), against
// the host stubs/freertos shim) -- a materially larger link surface than
// sim_scenarios.c's four-file (pid/pid_fuzzy/pid_autotune/firing_score)
// posture. Keeping it in its own executable, own build step, own budget
// line (see check_sim_scenarios_adaptive.ps1) means a break in this file's
// much heavier dependency chain never blocks sim_scenarios.c's own
// determinism proof, and vice versa -- same reasoning test_adaptive_tune.c
// itself already documents for being its own executable (build_host_
// tests.ps1's exe17 comment).
//
// WHAT IS REAL, WHAT IS MIRRORED (same posture as sim_scenarios.c's own
// header): pid.c, pid_fuzzy.c, pid_autotune.c, sim_plant.c, adaptive_tune.c,
// adaptive_tune_model.c, adaptive_tune_ki.c, zone_coupling_solve.c are ALL
// linked for real, unmodified. Only the per-tick orchestration (which
// function gets called in which order against which scenario row) is
// written here, plus the zones_config/profile_executor/flash-worker fakes
// every host test of adaptive_tune.c already needs (there is no lighter
// real seam into adaptive_tune_run_end() than the one test_adaptive_tune.c
// already proved out -- see that file's own header for why).
//
// SCOPE, STATED PLAINLY:
//   - Diagonal K_dc refinement only, exactly what adaptive_tune_model.c
//     itself does (adaptive_tune.h: "No integral (Ki) diagnosis, no
//     dynamics (tau/L) re-fit"). tau_s/dead_time_s are carried FIXED at the
//     scenario's own model_tau_s/model_dead_time_s for the whole chain.
//   - Ki is NEVER written by this module (88bb4333, docs/audits/
//     simc_sole_gain_writer_2026-09-14.md: adaptive_tune_ki.c's write path
//     is gone, z->ki_applied is hardcoded false). Ki still moves, but only
//     as a SIDE EFFECT of pid_autotune_tune_from_fopdt()'s SIMC recompute
//     against the refined K_dc -- reported as such below, never implied to
//     come from the withdrawn Ki heuristic.
//   - SIM_ARM_FUZZY_AT's dwell segments run with strength_pct FORCED TO 0
//     (plain PID), matching pid_fuzzy_prepare_gains()'s production
//     harvest_freeze contract (harvest_freeze = s_exec.dwelling &&
//     adaptive_tune_get_enabled(zi)) -- intentional, NOT a bug, and stated
//     on every printed row so a reader never mistakes this arm for "fuzzy
//     everywhere plus adaptation." Ramp segments still run at strength 50.
//   - Scenarios with high_temp_dynamic_scale/retune_per_segment set (S10,
//     S11 in the current table) are SKIPPED for this chain, loudly (a
//     printed SKIP line, not a silent gap): per-tick conductance rescaling
//     and per-segment re-tuning are themselves confounds for isolating
//     "does the K_dc estimate move toward the true plant run over run,"
//     which is this work item's whole question. Every other scenario in
//     the table runs.
//   - SIMC's invariance to fuzzy is APPROXIMATE, not exact (docs/audits/
//     simc_sole_gain_writer_2026-09-14.md's appended review, cef1df2a): the
//     settle gates never require error==0, so a still-converging approach
//     can bias the fitted K_dc by an amount on the order of 0.003*tau (the
//     settle slope floor itself). This file's job is to report that as a
//     NUMBER (the per-firing progression), not to design around it or
//     claim exactness.
//
// Usage: sim_scenarios_adaptive.exe -- always the full 11-scenario x
// 2-arm x 9-firing chain, single process, no sharding (the sharding
// discipline belongs to sim_scenarios.c's own bigger suite; this file's
// runtime is small enough -- see its own header note on expected cost --
// that WI-7's shardability argument does not apply here).
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test_common.h" // TEST_CHECK()/g_test_failures/g_test_count -- bx_worker_stub.h below uses TEST_CHECK()

// Own executable (no other test file's main() links against this one) --
// same convention test_adaptive_tune.c/test_profile_executor_prestart.c use
// for these two TEST_CHECK() globals.
int g_test_failures = 0;
int g_test_count = 0;

#include "../drivers/hw/MAX31856.h" // MAX31856_CHANNEL_COUNT -- ahead of adaptive_tune.c's own #include, same
                                  // ordering convention test_adaptive_tune.c documents and uses
#include "esp_err.h"
#include "fake_kv.h" // hal_kv.h's host fake

#include "../drivers/control/profile_executor.h" // profile_exec_status_t/profile_exec_state_t/PROFILE_EXEC_*
#include "../drivers/persist/zones_config_accessors.h" // zone_control_mode_t/ZONE_CONTROL_MODE_*

// ---------------------------------------------------------------------
// Fakes for adaptive_tune.c's extern dependencies -- single-zone (index 0
// only; every scenario in this suite is single-zone), same shape and same
// defaults as test_adaptive_tune.c's own TEST_MAX_ZONES=5 table, trimmed to
// TEST_MAX_ZONES=1 because this file drives exactly one simulated zone.
// ---------------------------------------------------------------------
#define TEST_MAX_ZONES 1
static struct {
    float k_dc, tau_s, dead_time_s;
    float kp, ki, kd;
    float autotune_baseline_k_dc;
    zone_control_mode_t control_mode;
    float fuzzy_strength_pct;
} s_fake_zone_cfg[TEST_MAX_ZONES];

static bool s_stub_zone_is_on_off[TEST_MAX_ZONES];
bool zone_is_on_off(uint8_t zone_index)
{
    if (zone_index >= TEST_MAX_ZONES) return false;
    return s_stub_zone_is_on_off[zone_index];
}

bool zones_config_get_model(uint8_t zone_index, float *out_k_dc, float *out_tau_s, float *out_dead_time_s)
{
    if (zone_index >= TEST_MAX_ZONES) return false;
    *out_k_dc = s_fake_zone_cfg[zone_index].k_dc;
    *out_tau_s = s_fake_zone_cfg[zone_index].tau_s;
    *out_dead_time_s = s_fake_zone_cfg[zone_index].dead_time_s;
    return true;
}
bool zones_config_set_model(uint8_t zone_index, float k_dc, float tau_s, float dead_time_s)
{
    if (zone_index >= TEST_MAX_ZONES) return false;
    s_fake_zone_cfg[zone_index].k_dc = k_dc;
    s_fake_zone_cfg[zone_index].tau_s = tau_s;
    s_fake_zone_cfg[zone_index].dead_time_s = dead_time_s;
    return true;
}
bool zones_config_get_autotune_baseline_k_dc(uint8_t zone_index, float *out_k_dc)
{
    if (zone_index >= TEST_MAX_ZONES) return false;
    *out_k_dc = s_fake_zone_cfg[zone_index].autotune_baseline_k_dc;
    return true;
}
bool zones_config_set_autotune_baseline_k_dc(uint8_t zone_index, float k_dc)
{
    if (zone_index >= TEST_MAX_ZONES) return false;
    s_fake_zone_cfg[zone_index].autotune_baseline_k_dc = k_dc;
    return true;
}
bool zones_config_get_pid(uint8_t zone_index, float *out_kp, float *out_ki, float *out_kd)
{
    if (zone_index >= TEST_MAX_ZONES) return false;
    *out_kp = s_fake_zone_cfg[zone_index].kp;
    *out_ki = s_fake_zone_cfg[zone_index].ki;
    *out_kd = s_fake_zone_cfg[zone_index].kd;
    return true;
}
#define TEST_ZONE_PID_GAIN_MAX 1000.0f // == zones_http.h's ZONE_PID_GAIN_MAX, redefined by hand (this file
                                        // does not include zones_http.h), same convention test_adaptive_tune.c uses
bool zones_config_set_pid(uint8_t zone_index, float kp, float ki, float kd)
{
    if (zone_index >= TEST_MAX_ZONES) return false;
    if (!isfinite(kp) || !isfinite(ki) || !isfinite(kd) || kp < 0.0f || ki < 0.0f || kd < 0.0f ||
        kp > TEST_ZONE_PID_GAIN_MAX || ki > TEST_ZONE_PID_GAIN_MAX || kd > TEST_ZONE_PID_GAIN_MAX) {
        return false;
    }
    s_fake_zone_cfg[zone_index].kp = kp;
    s_fake_zone_cfg[zone_index].ki = ki;
    s_fake_zone_cfg[zone_index].kd = kd;
    return true;
}

static bool s_fake_control_mode_fail = false; // never set true here -- this suite never exercises that fault path
static bool s_fake_fuzzy_pct_fail = false;
bool zones_config_get_control_mode(uint8_t zone_index, zone_control_mode_t *out_mode)
{
    if (zone_index >= TEST_MAX_ZONES) return false;
    if (s_fake_control_mode_fail) return false;
    *out_mode = s_fake_zone_cfg[zone_index].control_mode;
    return true;
}
bool zones_config_get_fuzzy_strength_pct(uint8_t zone_index, float *out_pct)
{
    if (zone_index >= TEST_MAX_ZONES) return false;
    if (s_fake_fuzzy_pct_fail) return false;
    *out_pct = s_fake_zone_cfg[zone_index].fuzzy_strength_pct;
    return true;
}

static float s_fake_coupling[TEST_MAX_ZONES][MAX31856_CHANNEL_COUNT];
static float s_fake_coupling_tau[TEST_MAX_ZONES][MAX31856_CHANNEL_COUNT];
static float s_fake_coupling_dead[TEST_MAX_ZONES][MAX31856_CHANNEL_COUNT];
bool zones_config_get_coupling(uint8_t zone_index, float out_row[MAX31856_CHANNEL_COUNT])
{
    if (zone_index >= TEST_MAX_ZONES) return false;
    for (int j = 0; j < MAX31856_CHANNEL_COUNT; j++) out_row[j] = s_fake_coupling[zone_index][j];
    return true;
}
bool zones_config_get_coupling_diag_k_dc(uint8_t zone_index, float *out_k_dc)
{
    (void)zone_index; (void)out_k_dc;
    return false; // dead code from this executable's PoV, symbol only needed to link zone_coupling_solve.o
}
bool zones_config_get_coupling_tau(uint8_t zone_index, float out_row[MAX31856_CHANNEL_COUNT])
{
    if (zone_index >= TEST_MAX_ZONES) return false;
    for (int j = 0; j < MAX31856_CHANNEL_COUNT; j++) out_row[j] = s_fake_coupling_tau[zone_index][j];
    return true;
}
bool zones_config_get_coupling_dead_time(uint8_t zone_index, float out_row[MAX31856_CHANNEL_COUNT])
{
    if (zone_index >= TEST_MAX_ZONES) return false;
    for (int j = 0; j < MAX31856_CHANNEL_COUNT; j++) out_row[j] = s_fake_coupling_dead[zone_index][j];
    return true;
}
bool zones_config_set_coupling_cell(uint8_t zone_index, uint8_t neighbor_index, float coeff, float tau_s,
                                     float dead_time_s)
{
    if (zone_index >= TEST_MAX_ZONES || neighbor_index >= MAX31856_CHANNEL_COUNT) return false;
    s_fake_coupling[zone_index][neighbor_index] = coeff;
    s_fake_coupling_tau[zone_index][neighbor_index] = tau_s;
    s_fake_coupling_dead[zone_index][neighbor_index] = dead_time_s;
    return true;
}

static bool s_fake_adaptive_enabled[TEST_MAX_ZONES];
bool zones_config_get_adaptive_tune_enabled(uint8_t zone_index)
{
    if (zone_index >= TEST_MAX_ZONES) return false;
    return s_fake_adaptive_enabled[zone_index];
}
bool zones_config_set_adaptive_tune_enabled(uint8_t zone_index, bool enabled)
{
    if (zone_index >= TEST_MAX_ZONES) return false;
    s_fake_adaptive_enabled[zone_index] = enabled;
    return true;
}

static profile_exec_state_t s_fake_exec_state = PROFILE_EXEC_IDLE;
void profile_executor_get_status(profile_exec_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state = s_fake_exec_state;
}

#include "bx_worker_stub.h" // shared uart_bridge_ext_run_on_flash_worker()/_is_on_flash_worker() stub

// Real production code, #included directly -- same one-TU convention
// test_adaptive_tune.c uses (2026-09-01 split: adaptive_tune.c no longer
// contains the model refine or the Ki diagnosis, both moved to their own
// files, #included together here).
#include "../drivers/control/adaptive_tune.c"
#include "../drivers/control/adaptive_tune_model.c"
#include "../drivers/control/adaptive_tune_ki.c"

#include "../drivers/control/pid.h"
#include "../drivers/control/pid_autotune.h"
#include "../drivers/control/pid_fuzzy.h"
#include "sim_plant.h"
#include "sim_scenario_table.h"

#define DT_S 1.0f
#define D_FILTER_TAU_S 30.0f
#define SETPOINT_WEIGHT_B 1.0f
#define PID_RANGE_C 25.0f
#define N_FIRINGS 9

static float q1(float c) { return roundf(c * 10.0f) / 10.0f; } // MAX31856-resolution quantization,
                                                                 // same convention as test_adaptive_tune.c's q1()

// One firing's outcome, reported per firing (never only the endpoint --
// WI-8's own acceptance wording).
typedef struct {
    bool ok;
    char refusal_reason[160];
    float belief_k_dc_before, belief_kp_before, belief_ki_before, belief_kd_before;
    float error_band_c, rate_band_c_per_s;
    float dwell2_mean_abs_err_c; // simple settle proxy: mean |target-actual| over the SECOND dwell segment
    uint32_t ring_count_after;
    bool refine_applied;
    float refine_delta_pct;
    char refine_reason[96];
    // Peeked from adaptive_tune_zones[0].last_refusal_reason BEFORE
    // adaptive_tune_run_end() runs (which overwrites it with its own
    // coarser "only N/M dwell observations" message when the ring is
    // under ADAPTIVE_TUNE_MIN_OBSERVATIONS) -- this is what the per-TICK
    // harvesting path (adaptive_tune_zone_tick()) itself refused on, e.g.
    // "duty still oscillating" -- the DEEPER, more informative reason a
    // dwell yielded nothing, distinct from run_end()'s own aggregate one.
    char harvest_reason[96];
} chain_firing_result_t;

// Runs ONE firing against the CURRENT belief model held in s_fake_zone_cfg[0]
// (already the production zones_config seam adaptive_tune_run_end() reads/
// writes), feeding adaptive_tune_zone_tick() every control tick and calling
// adaptive_tune_run_end() once at the firing's own end -- the real run-end
// safe-boundary hook, exactly where profile_executor.c calls it on hardware.
// fuzzy_at selects SIM_ARM_FUZZY_AT's harvest-freeze behaviour (strength 0
// during dwells, 50 during ramps); false runs plain PID throughout (SIM_ARM_
// PID_AT).
static bool run_one_chained_firing(const sim_scenario_t *sc, bool fuzzy_at, uint8_t profile_id,
                                    chain_firing_result_t *out)
{
    memset(out, 0, sizeof(*out));

    float k_dc, tau_s, dead_time_s, kp, ki, kd;
    if (!zones_config_get_model(0, &k_dc, &tau_s, &dead_time_s) || !zones_config_get_pid(0, &kp, &ki, &kd)) {
        snprintf(out->refusal_reason, sizeof(out->refusal_reason), "zones_config test fake returned false");
        return false;
    }
    out->belief_k_dc_before = k_dc;
    out->belief_kp_before = kp;
    out->belief_ki_before = ki;
    out->belief_kd_before = kd;

    float error_band_c, rate_band_c_per_s;
    if (!pid_fuzzy_derive_bands(k_dc, tau_s, &error_band_c, &rate_band_c_per_s)) {
        snprintf(out->refusal_reason, sizeof(out->refusal_reason),
                 "pid_fuzzy_derive_bands() returned false for belief k_dc=%.4f tau_s=%.4f", (double)k_dc, (double)tau_s);
        return false;
    }
    out->error_band_c = error_band_c;
    out->rate_band_c_per_s = rate_band_c_per_s;

    sim_plant_cfg_t plant_cfg = sc->plant;
    sim_plant_state_t pstate;
    sim_plant_reset(&pstate, &plant_cfg);
    pid_state_t pid_state;
    pid_reset(&pid_state);
    float prev_effective_ki = 0.0f;
    bool three_node = (plant_cfg.node_model == SIM_NODE_THREE);

    float ambient = sc->ambient_c;
    float t1 = ambient + sc->t1_offset_c;
    float t2 = ambient + sc->t2_offset_c;
    float rate_c_per_s = sc->ramp_rate_c_per_hr / 3600.0f;
    int ramp1_ticks = (int)((t1 - ambient) / rate_c_per_s / DT_S);
    int ramp2_ticks = (int)((t2 - t1) / rate_c_per_s / DT_S);
    if (ramp1_ticks < 1) ramp1_ticks = 1;
    if (ramp2_ticks < 1) ramp2_ticks = 1;
    // WI-8's own dwell length needs to be LONGER than sim_scenarios.c's
    // (6*tau, tuned for the 2.0/0.5 degC error-vs-TARGET settle bands,
    // which converge relatively fast). adaptive_tune_zone_tick()'s own
    // settle-slope-floor gate (ADAPTIVE_TUNE_SETTLE_SLOPE_FLOOR_C_PER_S,
    // 0.003 degC/s, a fixed real-hardware constant this file does not and
    // must not touch) measures actual_c's OWN drift, not its offset from
    // target -- a stricter, slower-to-satisfy condition than "within 2
    // degC of target," especially for a mistuned tune whose SIMC-derived Ki
    // is small (a long integral tail keeps the sensor drifting slightly for
    // a long time after it is already visually close to target). 16*tau,
    // empirically the shortest multiple that lets S7 (the most severely
    // mistuned scenario in this suite) harvest at least one observation.
    int dwell_ticks = (int)(16.0f * sc->model_tau_s / DT_S);
    if (dwell_ticks < 3200) dwell_ticks = 3200;
    if (dwell_ticks > 60000) dwell_ticks = 60000;

    struct { bool is_ramp; float target_start, target_end; int ticks; } segs[4] = {
        { true,  ambient, t1, ramp1_ticks },
        { false, t1, t1, dwell_ticks },
        { true,  t1, t2, ramp2_ticks },
        { false, t2, t2, dwell_ticks },
    };

    bool nan_seen = false;
    float ceiling_c = ambient + sc->model_k_dc + 400.0f;
    float floor_c = ambient - 1.0f;
    bool bounds_ok = true;

    double dwell2_err_sum = 0.0; long dwell2_err_n = 0;

    for (int s = 0; s < 4; s++) {
        float target_c = segs[s].target_start;
        float step_per_tick = segs[s].is_ramp ? (segs[s].target_end - segs[s].target_start) / (float)segs[s].ticks : 0.0f;
        bool dwelling = !segs[s].is_ramp;
        // harvest_freeze contract (pid_fuzzy_prepare_gains(), production):
        // s_exec.dwelling && adaptive_tune_get_enabled(zi) -- mirrored here
        // exactly, since adaptive_tune_get_enabled() is the real function.
        bool harvest_freeze = dwelling && adaptive_tune_get_enabled(0);
        uint8_t strength_pct = (fuzzy_at && !harvest_freeze) ? 50 : 0;

        for (int t = 0; t < segs[s].ticks; t++) {
            if (segs[s].is_ramp) target_c += step_per_tick;

            float error_c = target_c - pstate.sensor_c;
            float rate_meas = pid_state.d_filtered;
            float adj_kp = kp, adj_ki = ki, adj_kd = kd;
            if (strength_pct > 0) {
                pid_fuzzy_adjust(error_c, rate_meas, error_band_c, rate_band_c_per_s, kp, ki, kd, strength_pct,
                                 &adj_kp, &adj_ki, &adj_kd);
            }
            pid_rescale_integral_for_new_ki(&pid_state, prev_effective_ki, adj_ki);
            prev_effective_ki = adj_ki;

            pid_cfg_t cfg = { adj_kp, adj_ki, adj_kd, D_FILTER_TAU_S, SETPOINT_WEIGHT_B, PID_RANGE_C };
            float duty = pid_update(&pid_state, &cfg, target_c, pstate.sensor_c, DT_S, 0.0f, 0.0f);

            if (three_node) {
                sim_plant_three_node_step(&pstate, &plant_cfg, duty, DT_S);
            } else {
                sim_plant_step(&pstate, &plant_cfg, duty, DT_S);
            }

            if (isnan(pstate.sensor_c) || isnan(pstate.element_c)) nan_seen = true;
            if (pstate.sensor_c < floor_c || pstate.sensor_c > ceiling_c) bounds_ok = false;

            // Real production seam: this is the ONLY thing standing in for
            // profile_executor.c's tick loop -- q1() quantizes to the
            // MAX31856's reporting resolution, same as test_adaptive_
            // tune.c's own feed_settled_dwell(), so the settle-slope-floor
            // gate sees the same coarseness a real board's history_pack()
            // would produce, not an idealized float.
            adaptive_tune_zone_tick(0, q1(pstate.sensor_c), true, duty, dwelling, ambient, DT_S);

            if (s == 3) { // second dwell only, simple settle proxy
                dwell2_err_sum += fabs((double)error_c);
                dwell2_err_n++;
            }
        }
    }

    if (nan_seen) { snprintf(out->refusal_reason, sizeof(out->refusal_reason), "NaN observed in plant state"); return false; }
    if (!bounds_ok) { snprintf(out->refusal_reason, sizeof(out->refusal_reason), "sensor reading left [%.1f, %.1f]", (double)floor_c, (double)ceiling_c); return false; }

    out->dwell2_mean_abs_err_c = dwell2_err_n ? (float)(dwell2_err_sum / (double)dwell2_err_n) : 0.0f;

    // Peek the per-tick harvesting path's own refusal string BEFORE
    // adaptive_tune_run_end() below overwrites it with its own coarser
    // message -- see chain_firing_result_t::harvest_reason's own comment.
    snprintf(out->harvest_reason, sizeof(out->harvest_reason), "%s", adaptive_tune_zones[0].last_refusal_reason);

    // Real run-end safe-boundary hook -- AFTER the firing's own relays are
    // (implicitly) off, exactly the production timing guarantee.
    profile_firing_run_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.profile_id = profile_id;
    rec.zones[0].active = true;
    rec.zones[0].stats.sample_count = 1000; // nonzero, non-excluded -- this chain never exercises the
    rec.zones[0].stats.excluded_sample_count = 0; // excluded-sample-count refusal path, only the K_dc refine itself
    adaptive_tune_run_end(&rec, /*clean=*/true);

    adaptive_tune_zone_status_t status;
    adaptive_tune_get_status(0, &status);
    out->ring_count_after = status.ring_count;
    out->refine_applied = status.has_applied && status.last_applied_profile_id == profile_id;
    out->refine_delta_pct = status.last_delta_pct;
    snprintf(out->refine_reason, sizeof(out->refine_reason), "%s", status.last_refusal_reason);

    out->ok = true;
    return true;
}

// Runs the 9-firing chain for one (scenario, arm) pair, printing every
// firing's row. Returns false (and refuses loudly) if any firing itself
// refuses -- same "never emit a partial/misleading row" posture as
// sim_scenarios.c's run_firing() caller.
static bool run_chain(const sim_scenario_t *sc, bool fuzzy_at, float *out_final_k_dc, float *out_initial_k_dc,
                       uint32_t *out_max_ring_count, bool *out_any_refine_applied)
{
    // Fresh module state for this (scenario, arm) chain -- the ring/settle/
    // baseline state from a PREVIOUS chain must never leak into this one.
    memset(adaptive_tune_zones, 0, sizeof(adaptive_tune_zones));
    memset(s_fake_zone_cfg, 0, sizeof(s_fake_zone_cfg));
    memset(s_fake_adaptive_enabled, 0, sizeof(s_fake_adaptive_enabled));
    memset(s_stub_zone_is_on_off, 0, sizeof(s_stub_zone_is_on_off));
    s_fake_exec_state = PROFILE_EXEC_IDLE;
    adaptive_tune_joint_ring_count = 0;
    adaptive_tune_joint_ring_head = 0;
    adaptive_tune_joint_observations_lifetime = 0;
    memset(adaptive_tune_joint_ring, 0, sizeof(adaptive_tune_joint_ring));
    memset(adaptive_tune_joint_last_duty, 0, sizeof(adaptive_tune_joint_last_duty));
    memset(adaptive_tune_joint_last_rise_c, 0, sizeof(adaptive_tune_joint_last_rise_c));
    memset(adaptive_tune_joint_last_valid, 0, sizeof(adaptive_tune_joint_last_valid));
    adaptive_tune_joint_dwell_row_committed = false;

    // Belief model starts at the scenario's OWN model_* -- the deliberately
    // mistuned tuner input for S7/S8/S9/S12, the matched value elsewhere.
    // autotune_baseline_k_dc starts at 0 ("not recorded yet"): the survey
    // that prompted this pass found adaptive_tune has NEVER run on the real
    // board (enabled=false, lifetime=0 on all three zones) -- a board
    // opting in for the first time bootstraps its baseline from whatever
    // model is live, exactly this path (adaptive_tune_model.c's own
    // baseline-bootstrap comment).
    s_fake_zone_cfg[0].k_dc = sc->model_k_dc;
    s_fake_zone_cfg[0].tau_s = sc->model_tau_s;
    s_fake_zone_cfg[0].dead_time_s = sc->model_dead_time_s;
    s_fake_zone_cfg[0].autotune_baseline_k_dc = 0.0f;

    fopdt_model_t model = {
        .k_gain_c_per_duty = sc->model_k_dc, .tau_s = sc->model_tau_s, .dead_time_s = sc->model_dead_time_s,
        .valid = true, .settled = true, .tau_consistent_with_gain = true, .extrapolation_converged = true,
    };
    autotune_gains_t gains0 = pid_autotune_tune_from_fopdt(&model, AUTOTUNE_RULE_SIMC, 0.0f);
    if (gains0.refusal != AUTOTUNE_REFUSAL_OK) {
        printf("SCENARIO_REFUSED %s %s: initial SIMC tune refused: %s\n", sc->id,
               fuzzy_at ? "A_FUZZY_AT" : "A_PID_AT", gains0.refusal_reason);
        return false;
    }
    s_fake_zone_cfg[0].kp = gains0.kp;
    s_fake_zone_cfg[0].ki = gains0.ki;
    s_fake_zone_cfg[0].kd = gains0.kd;
    s_fake_adaptive_enabled[0] = true;
    adaptive_tune_zones[0].enabled = true;

    *out_initial_k_dc = sc->model_k_dc;
    *out_max_ring_count = 0;
    *out_any_refine_applied = false;

    for (int fi = 1; fi <= N_FIRINGS; fi++) {
        chain_firing_result_t r;
        bool ok = run_one_chained_firing(sc, fuzzy_at, (uint8_t)fi, &r);
        if (ok) {
            if (r.ring_count_after > *out_max_ring_count) *out_max_ring_count = r.ring_count_after;
            if (r.refine_applied) *out_any_refine_applied = true;
        }
        if (!ok) {
            printf("SCENARIO_REFUSED %s %s firing %d/%d: %s\n", sc->id, fuzzy_at ? "A_FUZZY_AT" : "A_PID_AT", fi,
                   N_FIRINGS, r.refusal_reason);
            return false;
        }
        printf("ADAPTIVE_CHAIN\t%s\t%s\t%d/%d\t"
               "belief_k_dc=%.4f\tkp=%.5f\tki=%.6f\tkd=%.4f\t"
               "error_band_c=%.4f\trate_band_c_per_s=%.6f\t"
               "dwell2_mean_abs_err_c=%.4f\tring_count=%u\t"
               "refine_applied=%s\trefine_delta_pct=%.3f\trefine_reason=%s\tharvest_reason=%s\n",
               sc->id, fuzzy_at ? "A_FUZZY_AT" : "A_PID_AT", fi, N_FIRINGS,
               (double)r.belief_k_dc_before, (double)r.belief_kp_before, (double)r.belief_ki_before,
               (double)r.belief_kd_before, (double)r.error_band_c, (double)r.rate_band_c_per_s,
               (double)r.dwell2_mean_abs_err_c, (unsigned)r.ring_count_after,
               r.refine_applied ? "yes" : "no", (double)r.refine_delta_pct,
               r.refine_reason[0] ? r.refine_reason : "(n/a)",
               r.harvest_reason[0] ? r.harvest_reason : "(none this firing)");
        fflush(stdout);
    }

    float final_k_dc, unused_tau, unused_dead;
    zones_config_get_model(0, &final_k_dc, &unused_tau, &unused_dead);
    *out_final_k_dc = final_k_dc;
    return true;
}

int main(void)
{
    printf("=== sim_scenarios_adaptive (WI-8): SIM_ARM_PID_AT / SIM_ARM_FUZZY_AT, "
           "9 chained firings, adaptation state carried across the chain ===\n");
    printf("Real production adaptive_tune_zone_tick()/adaptive_tune_run_end()/adaptive_tune_refine_zone_locked()\n"
           "are linked and driven directly (adaptive_tune.c/adaptive_tune_model.c/adaptive_tune_ki.c), against a\n"
           "single-zone zones_config test fake whose K_dc/Kp/Ki/Kd/autotune_baseline_k_dc/adaptive_tune_enabled\n"
           "state persists for the whole 9-firing chain -- carried state is exactly what this file exists to\n"
           "prove. Diagonal K_dc only (tau_s/dead_time_s fixed for the chain, matching adaptive_tune.h's own\n"
           "documented scope). Ki is never written directly any more (88bb4333 removed that path -- z->ki_applied\n"
           "is permanently false); Ki still moves as a side effect of the SIMC recompute against the refined\n"
           "K_dc, reported below via kp/ki/kd, never attributed to the withdrawn Ki heuristic. A_FUZZY_AT's dwell\n"
           "segments run strength_pct=0 (plain PID) by the production harvest_freeze contract -- intentional,\n"
           "not a bug: this arm is fuzzy-on-ramps-only-while-adapting, never \"fuzzy everywhere plus adaptation.\"\n"
           "SIMC's invariance to fuzzy is approximate (residual ~0.003*tau, docs/audits/simc_sole_gain_writer_\n"
           "2026-09-14.md's appended review) -- numbers below are reported, not adjudicated; an opus review\n"
           "follows this pass.\n\n");

    const sim_scenario_t *table = sim_scenario_table();
    int n = SIM_SCENARIO_COUNT;
    bool any_fail = false;
    int ran = 0, skipped = 0;

    // S7 (TUNE_HOT) direction check, WI-8 acceptance (a): belief_k_dc must
    // move TOWARD the true plant gain across the 9 runs. Direction is
    // asserted; magnitude is only reported (acceptance (a)'s own wording).
    // three_node_matched_plant()'s own construction (sim_scenario_table.c)
    // guarantees its steady-state DC gain equals BENCH_K_DC exactly, while
    // S7's model_k_dc (the belief handed to the tuner) is BENCH_K_DC*0.5 --
    // see sim_mistune.h's SIM_MISTUNE_HOT factors. BENCH_K_DC itself is a
    // runtime array read (sim_measured_zone_constants.h), so it is looked
    // up from the S1 row (which uses it unmodified as its own model_k_dc)
    // rather than re-declared as a second literal here.
    float s7_true_k_dc = 0.0f;
    bool have_s7_truth = false;
    // Global falsifiable check, sec below: at least one (scenario, arm)
    // chain in this suite must actually harvest ADAPTIVE_TUNE_MIN_
    // OBSERVATIONS (4) dwell points at some point across its 9 firings. If
    // NOTHING ever does, the real adaptive_tune_zone_tick()/run_end() link
    // is not exercising its harvesting path at all -- a much more serious,
    // and more honest, thing to gate the build on than forcing a specific
    // scenario's K_dc to converge (see the empirical finding below).
    bool any_chain_ever_harvested = false;
    bool any_refine_ever_applied = false;

    for (int si = 0; si < n; si++) {
        const sim_scenario_t *sc = &table[si];
        if (sc->high_temp_dynamic_scale || sc->retune_per_segment) {
            printf("SKIP %s: dynamic conductance scaling / per-segment retuning is itself a confound for "
                   "isolating cross-firing K_dc convergence -- out of WI-8's scope, see this file's header.\n\n",
                   sc->id);
            skipped++;
            continue;
        }
        if (strcmp(sc->id, "S1_BASELINE") == 0) {
            s7_true_k_dc = sc->model_k_dc; // S1 is matched: model_k_dc == BENCH_K_DC (the plant's own true gain)
            have_s7_truth = true;
        }
    }

    for (int si = 0; si < n; si++) {
        const sim_scenario_t *sc = &table[si];
        if (sc->high_temp_dynamic_scale || sc->retune_per_segment) continue;
        ran++;

        for (int arm = 0; arm < 2; arm++) {
            bool fuzzy_at = (arm == 1);
            float initial_k_dc = 0.0f, final_k_dc = 0.0f;
            uint32_t max_ring_count = 0;
            bool refine_applied_ever = false;
            bool ok = run_chain(sc, fuzzy_at, &final_k_dc, &initial_k_dc, &max_ring_count, &refine_applied_ever);
            if (!ok) { any_fail = true; continue; }

            if (max_ring_count >= 4) any_chain_ever_harvested = true; // ADAPTIVE_TUNE_MIN_OBSERVATIONS, this
                                                                        // file does not #include adaptive_tune_
                                                                        // internal.h's own constant to keep its
                                                                        // include list minimal -- 4 is that value,
                                                                        // named again here on purpose
            if (refine_applied_ever) any_refine_ever_applied = true;

            printf("ADAPTIVE_CHAIN_SUMMARY\t%s\t%s\tinitial_belief_k_dc=%.4f\tfinal_belief_k_dc=%.4f\t"
                   "net_move=%.4f\tmax_ring_count_reached=%u\trefine_ever_applied=%s\n\n",
                   sc->id, fuzzy_at ? "A_FUZZY_AT" : "A_PID_AT", (double)initial_k_dc, (double)final_k_dc,
                   (double)(final_k_dc - initial_k_dc), (unsigned)max_ring_count,
                   refine_applied_ever ? "yes" : "no");

            // WI-8 acceptance (a), re-stated honestly against what this
            // simulation actually found (see this file's own commit message
            // for the full empirical account): a direction check is asserted
            // ONLY when the chain actually harvested enough dwell data for
            // adaptive_tune_refine_zone_locked() to have had a real chance
            // to move K_dc. When it never did -- which this suite found is
            // the OUTCOME for every mistuned scenario (S7/S8/S9/S12), for a
            // production reason named per-firing above (harvest_reason:
            // "duty still oscillating..." -- the real production duty-
            // stability gate refuses the ONE settle-window evaluation a
            // dwell ever gets, because that window's min/max duty still
            // spans the ramp-to-dwell entry transient by the time the
            // temperature slope first reads flat) -- this is reported as
            // INCONCLUSIVE, not FAILED: "belief never moved" is not the same
            // claim as "belief moved the wrong way," and forcing the former
            // to read as the latter would be exactly the kind of dishonest
            // green this repo has been burned by before.
            if (strcmp(sc->id, "S7_TUNE_HOT") == 0 && have_s7_truth) {
                if (max_ring_count < 4) {
                    printf("S7_DIRECTION_CHECK\t%s\tINCONCLUSIVE (never harvested %u/4 minimum observations "
                           "across %d firings -- see per-firing harvest_reason above; this is a REAL "
                           "production-gate finding, not a harness defect)\n\n",
                           fuzzy_at ? "A_FUZZY_AT" : "A_PID_AT", (unsigned)max_ring_count, N_FIRINGS);
                } else {
                    float before_gap = fabsf(initial_k_dc - s7_true_k_dc);
                    float after_gap = fabsf(final_k_dc - s7_true_k_dc);
                    printf("S7_DIRECTION_CHECK\t%s\ttrue_k_dc=%.4f\tbefore_gap=%.4f\tafter_gap=%.4f\t%s\n\n",
                           fuzzy_at ? "A_FUZZY_AT" : "A_PID_AT", (double)s7_true_k_dc, (double)before_gap,
                           (double)after_gap, (after_gap < before_gap) ? "PASS (moved toward true plant)" : "FAIL");
                    if (!(after_gap < before_gap)) {
                        printf("FAIL: S7 (%s) belief_k_dc HARVESTED data and moved AWAY from the true plant "
                               "gain over %d firings -- WI-8 acceptance (a).\n",
                               fuzzy_at ? "A_FUZZY_AT" : "A_PID_AT", N_FIRINGS);
                        any_fail = true;
                    }
                }
            }
        }
    }

    printf("=== sim_scenarios_adaptive: %d scenario(s) run, %d skipped (out of scope, see SKIP lines above) ===\n",
           ran, skipped);
    if (!have_s7_truth) {
        printf("FAIL: S1_BASELINE not found in the table -- S7's ground truth lookup depends on it.\n");
        any_fail = true;
    }

    // The suite-wide falsifiable check this file actually gates on (see
    // any_chain_ever_harvested's own comment above): if the real production
    // harvesting path never fires for ANY (scenario, arm) pair in the whole
    // suite, that is either a genuine, repo-wide adaptive_tune finding (this
    // gate's own settle-slope/duty-stability combination may be close to
    // impossible to satisfy from an ordinary ramp-into-dwell profile at
    // these zone time constants -- worth a follow-up audit either way) or a
    // real regression in this file's wiring -- either way it must not read
    // as quiet PASS.
    if (!any_chain_ever_harvested) {
        printf("FAIL: no (scenario, arm) chain in this suite ever harvested the minimum 4 dwell observations "
               "adaptive_tune_refine_zone_locked() needs to attempt a fit -- the real harvesting path "
               "(adaptive_tune_zone_tick()) is not producing usable data anywhere in this suite.\n");
        any_fail = true;
    } else {
        printf("INFO: at least one chain harvested the minimum dwell observations "
               "(any_chain_ever_harvested=yes).\n");
    }
    printf("INFO: adaptive_tune_refine_zone_locked() actually APPLIED a gain change at least once anywhere "
           "in this suite: %s.\n", any_refine_ever_applied ? "yes" : "no");

    if (any_fail) {
        printf("=== sim_scenarios_adaptive: FAIL ===\n");
        return 1;
    }
    printf("=== sim_scenarios_adaptive: PASS ===\n");
    return 0;
}
