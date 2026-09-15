// sim_factorial_driver -- CELL DRIVER ONLY (docs/audits/scenario_factorial_driver_2026-09-14.md).
//
// SCOPE: drives the 263 cells sim_factorial_design.c generates through five
// arms and emits one TSV row per (cell, arm) -- three SINGLE-FIRING arms
// (A_PID, A_FUZZY50, A_STATIC_MATCHED) plus, since
// docs/ADAPTIVE_FUZZY_EVALUATION_PLAN.md sec 5, two ADAPTIVE arms
// (A_PID_AT, A_FUZZY_AT) each run as a CHAIN OF NINE sequential firings
// with adaptation state carried across them (one row per firing, arm column
// "A_PID_AT_F1".."A_FUZZY_AT_F9"), plus a per-cell/arm fuzzy rule-cell
// occupancy histogram. This
// is glue over three already-validated pieces (the design generator, the
// three-node decomposition helper, and the real production control code) --
// it builds NO new control logic and computes NO effects analysis (no P8/P11
// materiality or interaction inference). That is a separate, later dispatch;
// this file reports numbers only, never verdicts.
//
// WHAT THIS LINKS, REAL PRODUCTION CODE, NO MIRROR: pid.c, pid_fuzzy.c,
// pid_autotune.c, firing_score.c, sim_plant.c (real
// sim_plant_decompose_three_node()), and -- for the two adaptive arms --
// adaptive_tune.c/adaptive_tune_model.c/adaptive_tune_ki.c and
// pid_fuzzy_confidence.c, all unmodified, against a single-zone
// zones_config TEST FAKE whose state persists across one chain (the same
// fake-shape convention sim_scenarios_adaptive.c and test_adaptive_tune.c
// already use).
//
// ONE TICK LOOP, NOT TWO: plan sec 5.1 explicitly forbids a fourth copy of
// the tick loop. run_cell_firing() below therefore serves all five arms --
// the adaptive arms pass a non-NULL cell_adaptive_ctx_t and every new line
// of per-tick work is inside an `if (ad)` guard, so the three existing
// arms execute byte-for-byte the same arithmetic they did before (proven
// against a clean-HEAD worktree build, see the sec 5 implementation report
// in docs/audits/adaptive_fuzzy_evaluation_progress_2026-09-14.md).
//
// The per-tick wiring below is a
// deliberate near-duplicate of sim_scenarios.c's run_firing() -- sim_scenarios.c
// is WI-4/5/6/7's own file (not owned by this dispatch, and it has its own
// main()), so this driver keeps its own copy of the tick loop rather than
// reaching into that file. Any wiring bug fixed in one must be checked
// against the other; see the audit doc for the exact ways they were kept in
// sync at the time this file was written.
//
// EVERY CONSTANT BELOW ("bench base fit", sensor-node fixture values, A2's
// headroom multiplier, A4's bench/kiln absolute temperature spans) IS A TEST
// FIXTURE. Never shipped, never written into zones_config, a preset, or a
// firmware default.
//
// Usage: sim_factorial_driver.exe [--shard I --of M]
// Sharding is cell-index modulo, same style as sim_scenarios.c's WI-7: each
// shard prints only its own rows, in ascending cell order, no reordering, no
// RNG, no shared state across cells -- run_sim_factorial.ps1 proves --of 1
// and --of 4 produce byte-identical data rows.
#include "../drivers/control/pid.h"
#include "../drivers/control/pid_autotune.h"
#include "../drivers/control/pid_fuzzy.h"
#include "../drivers/control/firing_score.h"
#include "sim_factorial_design.h"
#include "sim_high_temp.h"
#include "sim_plant.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// =====================================================================
// ADAPTIVE-ARM SUPPORT (docs/ADAPTIVE_FUZZY_EVALUATION_PLAN.md sec 5)
//
// Everything between here and the END marker exists only for A_PID_AT /
// A_FUZZY_AT. It is a trimmed, single-zone copy of sim_scenarios_adaptive.c's
// own zones_config fake block (same field shapes, same defaults, same
// reasoning) -- lifted rather than re-derived, exactly as plan sec 5.1
// directs. None of it is reachable from the three pre-existing single-firing
// arms.
// =====================================================================
#include "test_common.h" // TEST_CHECK() -- bx_worker_stub.h below uses it

// Own executable; same convention sim_scenarios_adaptive.c/test_adaptive_tune.c use.
int g_test_failures = 0;
int g_test_count = 0;

#include "../drivers/hw/MAX31856.h" // MAX31856_CHANNEL_COUNT, ahead of adaptive_tune.c's own include
#include "esp_err.h"
#include "fake_kv.h" // hal_kv.h's host fake

#include "../drivers/control/profile_executor.h"        // profile_exec_status_t/profile_exec_state_t
#include "../drivers/persist/zones_config_accessors.h"  // zone_control_mode_t

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
#define TEST_ZONE_PID_GAIN_MAX 1000.0f // == zones_http.h's ZONE_PID_GAIN_MAX, redeclared by hand exactly as
                                        // sim_scenarios_adaptive.c/test_adaptive_tune.c already do
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

bool zones_config_get_control_mode(uint8_t zone_index, zone_control_mode_t *out_mode)
{
    if (zone_index >= TEST_MAX_ZONES) return false;
    *out_mode = s_fake_zone_cfg[zone_index].control_mode;
    return true;
}
bool zones_config_get_fuzzy_strength_pct(uint8_t zone_index, float *out_pct)
{
    if (zone_index >= TEST_MAX_ZONES) return false;
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
    return false; // symbol only needed to link zone_coupling_solve.o, same as sim_scenarios_adaptive.c
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

#include "bx_worker_stub.h" // shared uart_bridge_ext_run_on_flash_worker() stub

// Real production code, #included directly -- same one-TU convention
// sim_scenarios_adaptive.c and test_adaptive_tune.c use.
#include "../drivers/control/adaptive_tune.c"
#include "../drivers/control/adaptive_tune_model.c"
#include "../drivers/control/adaptive_tune_ki.c"

// The sec 3 confidence gate. Linked, NOT mirrored: this driver calls
// pid_fuzzy_confidence_cap_l()/_strength_pct()/_oscillation_*() and
// adaptive_tune_get_fuzzy_confidence_c()/_fuzzy_confidence_floor_now()
// exactly as production pid_fuzzy_prepare_gains() does. If that gate's
// interface changes, this file must change with it -- it deliberately does
// NOT keep a fallback copy of the schedule (a local mirror is how this
// project has previously shipped arms that silently stopped tracking
// production).
#include "../drivers/control/pid_fuzzy_confidence.h"

#define N_FIRINGS 9

// MAX31856-resolution quantization, same convention as
// sim_scenarios_adaptive.c/test_adaptive_tune.c's own q1().
static float q1(float c) { return roundf(c * 10.0f) / 10.0f; }
// ===================== END ADAPTIVE-ARM SUPPORT ======================

#define DT_S 1.0f
#define D_FILTER_TAU_S 30.0f
#define SETPOINT_WEIGHT_B 1.0f
#define PID_RANGE_C 25.0f

// ---- Bench base fit (design doc sec 3): measured z0 FOPDT, the anchor
// every bench cell's three-node decomposition is built to reproduce in
// aggregate (K, tau). ----
#define BENCH_K0_C_PER_DUTY 42.731f
#define BENCH_TAU0_S 255.6f
#define BENCH_L0_S 40.3f

// ---- Kiln-scale base "fit" for A4=KILN_SPAN cells (200->1250C). The bench
// fit above cannot reach kiln temperatures at any headroom multiplier (its
// full-duty asymptote is ambient+K ~= 67C) -- sim_high_temp_kiln_scale_cfg()
// (sim_high_temp.c) already anchors a physically-sized kiln plant
// (heater_power_w=2500W, c_l tuned for a ~488s tau at the 200C tune point,
// full-duty asymptote ~1288C) for exactly this reason; this driver reuses
// its two headline numbers (K, tau) as the kiln-span decomposition target
// rather than inventing new ones, and still runs every cell through the
// SAME decompose-then-vary-Bi/phi/p pipeline as the bench cells. Sensor-node
// dead time and tau are now KILN-SCALED for kiln-span cells (plan sec 6:
// KILN_L0_S/KILN_SENSOR_TAU_S below, scaled 488/255.6 off the bench values)
// -- bench-span cells are unchanged. */
#define KILN_K0_C_PER_DUTY 2500.0f
#define KILN_TAU0_S 488.0f

// ---- Sec 6: kiln-scaled sensor-node dead time and tau, applied only to
// kiln-span (A4=KILN_SPAN) cells -- bench-span cells keep BENCH_L0_S /
// FIXTURE_SENSOR_TAU_S unchanged (mandatory bit-identical regression, sec
// 6.1). Scale factor is KILN_TAU0_S/BENCH_TAU0_S = 488/255.6; c_s_j_per_c is
// left at the fixture bench value everywhere -- doc sec 6 shows it is
// provably inert (only c_s/sensor_tau_s enters dS, and it cancels).
#define KILN_L0_S 76.9f
#define KILN_SENSOR_TAU_S 28.6f

// ---- [ASSUMED] fixture constants not pinned by the design doc's §3/§9,
// resolved once here so no reader has to guess (see the audit doc's
// "assumptions" section for the reasoning): ----
// A2 headroom: TIGHT keeps the bare measured heater_power_w; AMPLE scales it
// up, i.e. the true plant has more actuator authority per unit duty than
// TIGHT does for the same commanded ramp. 1.5x is a round, documented guess.
#define A2_AMPLE_HEADROOM_MULT 1.5f
// Sensor-node three-node parameters (c_s, its own tau) are OUT OF the
// decomposition helper's scope (sim_plant.h says so explicitly). c_s_j_per_c
// is held fixed across every cell (provably inert per plan sec 6). Bench
// sensor tau/dead-time (below, FIXTURE_SENSOR_TAU_S / BENCH_L0_S) are the
// bench-span values; kiln-span cells use KILN_SENSOR_TAU_S / KILN_L0_S
// instead (see build_cell_plant()). Only sensor_bias_p (A3) is a design factor.
#define FIXTURE_SENSOR_C_S_J_PER_C 500.0f
#define FIXTURE_SENSOR_TAU_S 15.0f
// A4 bench/kiln absolute spans (design doc sec 3's "bench 24->60" / "kiln
// 200->1250"), anchored the same way sim_high_temp.h's T_REF (55C) already
// is -- the driver applies sim_high_temp_scale_conductances() every tick
// using these as the ambient/segment targets, same mechanism S10/S11 use.
#define BENCH_AMBIENT_C 24.0f
#define BENCH_T1_OFFSET_C 16.0f  // ambient+16 = 40
#define BENCH_T2_OFFSET_C 36.0f  // ambient+36 = 60
#define KILN_AMBIENT_C 25.0f
#define KILN_T1_OFFSET_C 175.0f  // ambient+175 = 200
#define KILN_T2_OFFSET_C 1225.0f // ambient+1225 = 1250

// A6 tune mismatch triples (m_k, m_tau, m_L), SCENARIO_SIMULATION_PLAN.md
// sec 2.4 -- link the SIMC formula via pid_autotune_tune_from_fopdt(),
// never reimplement it; these are just the model-vs-plant multipliers.
typedef struct { float m_k, m_tau, m_l; } tune_mismatch_t;
static tune_mismatch_t tune_mismatch_for(sim_fac_a6_tune_t a6)
{
    switch (a6) {
        case SIM_FAC_A6_MATCHED:       return (tune_mismatch_t){1.0f, 1.0f, 1.0f};
        case SIM_FAC_A6_HOT:           return (tune_mismatch_t){0.5f, 2.0f, 0.5f};
        case SIM_FAC_A6_COLD:          return (tune_mismatch_t){2.0f, 0.5f, 2.0f};
        case SIM_FAC_A6_SLOW_INTEGRAL: return (tune_mismatch_t){1.0f, 3.0f, 1.0f};
        default:                       return (tune_mismatch_t){1.0f, 1.0f, 1.0f};
    }
}

// Three SINGLE-FIRING arms (the original dispatch): plain PID, fuzzy-50, and
// the matched-effective-gain static arm (measured per-cell, never the
// centre-cell max). Then plan sec 5's two ADAPTIVE arms, which are NOT
// single-firing -- each is a chain of N_FIRINGS firings and emits one row per
// firing. CELL_ARM_SINGLE_COUNT deliberately bounds the original arm loop so
// adding adaptive arms cannot change how the first three are driven.
typedef enum {
    CELL_ARM_PID = 0,
    CELL_ARM_FUZZY50,
    CELL_ARM_STATIC_MATCHED,
    CELL_ARM_SINGLE_COUNT,
    CELL_ARM_PID_AT = CELL_ARM_SINGLE_COUNT,  // adaptive_tune ON, fuzzy OFF
    CELL_ARM_FUZZY_AT,                        // adaptive_tune ON, confidence-gated fuzzy
    CELL_ARM_COUNT
} cell_arm_t;
static const char *const CELL_ARM_NAMES[CELL_ARM_COUNT] = {
    "A_PID", "A_FUZZY50", "A_STATIC_MATCHED", "A_PID_AT", "A_FUZZY_AT"
};

// Rule-cell occupancy: 3x3 dominant-membership bucket, same axis order as
// pid_fuzzy.c's own RULE_TABLE (0=NEG/FALLING,1=ZERO/STEADY,2=POS/RISING).
// This driver does not call into pid_fuzzy.c's internals (they are static);
// it re-derives the DOMINANT bucket from the same triangular membership
// shape pid_fuzzy.c documents in its own header (symmetric triangles
// centered at -band/0/+band) purely for histogram purposes -- it never
// feeds this back into a gain and is not what pid_fuzzy_adjust() itself
// uses internally (that function blends all three degrees continuously;
// this histogram answers "which single cell would a hard classifier land
// in," the same question the 2026-09-04 field capture (this file's own
// comment block references) asked of the real bench data).
static int dominant_bucket(float x, float band)
{
    if (band <= 0.0f || !isfinite(band)) return 1;
    if (x <= -band) return 0;
    if (x >= band) return 2;
    if (x <= 0.0f) return (-x) > (band + x) ? 0 : 1; /* nearer -band than 0 -> NEG else ZERO, by construction always ZERO- or NEG-leaning below 0 */
    return x > (band - x) ? 2 : 1;
}

typedef struct {
    bool ok;
    char refusal_reason[200];

    bool have_lag;         float lag_s;
    bool have_lag_signed;  float lag_signed_s;
    bool have_settle;      float settle_s_2c;
    bool have_steady;      float steady_rms_c;
    bool have_entry_peak;  float entry_peak_c;
    bool have_entry_under; float entry_undershoot_c;

    float applied_kp_mult_mean, applied_ki_mult_mean, applied_kd_mult_mean;
    float sat_frac;

    /* Reported on the ADAPTIVE_DIAG line only (plan sec 7's per-row
     * instrumentation list) -- NOT on the 23-column data row, which must
     * stay byte-identical for the three pre-existing arms. */
    float fuzzy_error_band_c, fuzzy_rate_band_c_per_s;

    long rule_hist[9];
    long rule_hist_total;
} cell_firing_result_t;

// Per-firing adaptive context (plan sec 5 + sec 7). A non-NULL pointer to one
// of these is what turns run_cell_firing() into an adaptive-arm firing; NULL
// leaves every pre-existing arm on exactly the code path it had before.
//
// The confidence gate is NOT reimplemented here. cap_l is computed once per
// firing from THIS zone's current belief model by the real
// pid_fuzzy_confidence_cap_l(); the per-tick strength comes from the real
// pid_fuzzy_confidence_strength_pct() fed by the real
// adaptive_tune_get_fuzzy_confidence_c(); the oscillation backstop is the
// real pid_fuzzy_oscillation_tick()/_reset(). Production's
// pid_fuzzy_prepare_gains() does the same four calls in the same order --
// see profile_executor_pid_tick.c.
typedef struct {
    bool  fuzzy;            // false = A_PID_AT, true = A_FUZZY_AT
    float cap_l;            // computed per firing from the belief model, not per tick
    float bounds_k_dc;      // FIXED across the chain (the cell's initial model gain) so the
                            // plausibility ceiling cannot drift as the belief moves
    pid_fuzzy_oscillation_state_t osc;

    // ---- sec 7 activity/limit-cycle instrumentation, reported per row ----
    double   strength_sum;
    long     strength_ticks;
    uint8_t  strength_max;
    long     ticks_with_strength_gt_0;
    uint8_t  confidence_c_at_start;
    bool     oscillation_tripped;
    long     dwell_zero_crossings;
    bool     have_prev_dwell_error;
    float    prev_dwell_error_c;
} cell_adaptive_ctx_t;

// Builds a sim_plant_cfg_t + model (k_dc/tau_s/dead_time_s) + ambient/target
// temperatures for one factorial cell. Returns false (refuses) if the
// decomposition helper itself refuses -- never silently clamped.
static bool build_cell_plant(const sim_factorial_cell_t *cell, sim_plant_cfg_t *out_plant,
                              float *out_model_k_dc, float *out_model_tau_s, float *out_model_dead_time_s,
                              float *out_ambient_c, float *out_t1_offset_c, float *out_t2_offset_c,
                              float *out_true_k_dc, char *refusal, size_t refusal_n)
{
    bool kiln_span = (cell->a4_loss_scale_span_r_s >= (SIM_FAC_A4_KILN_SPAN * 0.5f));
    float headroom_mult = (cell->a2_headroom == SIM_FAC_A2_AMPLE) ? A2_AMPLE_HEADROOM_MULT : 1.0f;
    float true_k = (kiln_span ? KILN_K0_C_PER_DUTY : BENCH_K0_C_PER_DUTY) * headroom_mult;
    float true_tau = kiln_span ? KILN_TAU0_S : BENCH_TAU0_S;

    sim_plant_decompose_req_t req = { .k = true_k, .tau_s = true_tau, .bi = cell->a8_bi, .phi = cell->a7_phi };
    sim_plant_cfg_t plant;
    memset(&plant, 0, sizeof(plant));
    if (!sim_plant_decompose_three_node(&req, &plant)) {
        snprintf(refusal, refusal_n,
                 "sim_plant_decompose_three_node() refused for k=%.4f tau=%.4f bi=%.4f phi=%.4f",
                 (double)true_k, (double)true_tau, (double)cell->a8_bi, (double)cell->a7_phi);
        return false;
    }

    float sensor_delay_s = kiln_span ? KILN_L0_S : BENCH_L0_S;

    plant.node_model = SIM_NODE_THREE;
    plant.c_s_j_per_c = FIXTURE_SENSOR_C_S_J_PER_C;
    plant.sensor_tau_s = kiln_span ? KILN_SENSOR_TAU_S : FIXTURE_SENSOR_TAU_S;
    plant.sensor_bias_p = cell->a3_sensor_bias_p;
    plant.sensor_delay_s = sensor_delay_s;
    plant.sensor_lag_tau_s = 0.0f;
    plant.load_mass_mult = cell->a1_load_mass_mult;

    plant.ambient_c = kiln_span ? KILN_AMBIENT_C : BENCH_AMBIENT_C;

    tune_mismatch_t tm = tune_mismatch_for(cell->a6_tune);

    *out_plant = plant;
    *out_model_k_dc = true_k * tm.m_k;
    *out_model_tau_s = true_tau * tm.m_tau;
    *out_model_dead_time_s = sensor_delay_s * tm.m_l;
    *out_ambient_c = plant.ambient_c;
    *out_t1_offset_c = kiln_span ? KILN_T1_OFFSET_C : BENCH_T1_OFFSET_C;
    *out_t2_offset_c = kiln_span ? KILN_T2_OFFSET_C : BENCH_T2_OFFSET_C;
    *out_true_k_dc = true_k; /* sec 7: the SIMULATION knows ground truth; log it */
    return true;
}

// Near-duplicate of sim_scenarios.c's run_firing() -- see this file's own
// header comment for why this is a copy, not a shared call, and the audit
// doc for how the two were kept in sync. `static_mult` non-NULL selects the
// A_STATIC_MATCHED arm (fixed multipliers, fuzzy math bypassed entirely).
static bool run_cell_firing(const sim_plant_cfg_t *plant_cfg_in, float model_k_dc, float model_tau_s,
                             float model_dead_time_s, float ambient_c, float t1_offset_c, float t2_offset_c,
                             float ramp_rate_c_per_hr, cell_arm_t arm, const float *static_mult,
                             cell_adaptive_ctx_t *ad, cell_firing_result_t *out)
{
    memset(out, 0, sizeof(*out));

    if (!(model_k_dc > 0.0f) || !isfinite(model_k_dc) || !(model_tau_s > 0.0f) || !isfinite(model_tau_s) ||
        !(model_dead_time_s >= 0.0f) || !isfinite(model_dead_time_s)) {
        snprintf(out->refusal_reason, sizeof(out->refusal_reason),
                 "no valid model installed (k_dc=%.4f tau_s=%.4f dead_time_s=%.4f)",
                 (double)model_k_dc, (double)model_tau_s, (double)model_dead_time_s);
        return false;
    }

    float error_band_c, rate_band_c_per_s;
    if (!pid_fuzzy_derive_bands(model_k_dc, model_tau_s, &error_band_c, &rate_band_c_per_s)) {
        snprintf(out->refusal_reason, sizeof(out->refusal_reason),
                 "pid_fuzzy_derive_bands() refused for model_k_dc=%.4f model_tau_s=%.4f",
                 (double)model_k_dc, (double)model_tau_s);
        return false;
    }

    fopdt_model_t model;
    memset(&model, 0, sizeof(model));
    model.k_gain_c_per_duty = model_k_dc;
    model.tau_s = model_tau_s;
    model.dead_time_s = model_dead_time_s;
    model.valid = true;
    model.settled = true;
    model.tau_consistent_with_gain = true;
    model.extrapolation_converged = true;

    autotune_gains_t gains = pid_autotune_tune_from_fopdt(&model, AUTOTUNE_RULE_SIMC, 0.0f);
    if (gains.refusal != AUTOTUNE_REFUSAL_OK) {
        snprintf(out->refusal_reason, sizeof(out->refusal_reason),
                 "pid_autotune_tune_from_fopdt() refused: %s", gains.refusal_reason);
        return false;
    }
    float base_kp = gains.kp, base_ki = gains.ki, base_kd = gains.kd;

    if (ad) {
        /* The adaptive arms run the gains adaptive_tune actually WROTE into
         * the zones_config fake, not a re-derivation of them: on firing 1
         * those are byte-identical to the SIMC tune just computed above
         * (the chain seeds them from it), and from firing 2 on they are
         * whatever adaptive_tune_refine_zone_locked() persisted. Re-deriving
         * would silently discard any clamp/blend the real module applied. */
        float fkp, fki, fkd;
        if (!zones_config_get_pid(0, &fkp, &fki, &fkd)) {
            snprintf(out->refusal_reason, sizeof(out->refusal_reason),
                     "zones_config test fake returned false reading chain gains");
            return false;
        }
        base_kp = fkp; base_ki = fki; base_kd = fkd;
    }

    bool static_matched = (arm == CELL_ARM_STATIC_MATCHED);
    uint8_t strength_pct = (arm == CELL_ARM_FUZZY50) ? 50 : 0; /* per-tick for the adaptive arms, see below */
    float static_kp_mult = static_mult ? static_mult[0] : 1.0f;
    float static_ki_mult = static_mult ? static_mult[1] : 1.0f;
    float static_kd_mult = static_mult ? static_mult[2] : 1.0f;

    sim_plant_cfg_t plant_cfg = *plant_cfg_in;
    sim_plant_state_t pstate;
    sim_plant_reset(&pstate, &plant_cfg);
    pid_state_t pid_state;
    pid_reset(&pid_state);
    float prev_effective_ki = 0.0f;

    bool zone_captured = false;
    firing_score_cfg_t fscfg;
    memset(&fscfg, 0, sizeof(fscfg));
    fscfg.band_c = 5.0f;

    float t1 = ambient_c + t1_offset_c;
    float t2 = ambient_c + t2_offset_c;
    float rate_c_per_s = ramp_rate_c_per_hr / 3600.0f;

    int ramp1_ticks = (int)((t1 - ambient_c) / rate_c_per_s / DT_S);
    int ramp2_ticks = (int)((t2 - t1) / rate_c_per_s / DT_S);
    if (ramp1_ticks < 1) ramp1_ticks = 1;
    if (ramp2_ticks < 1) ramp2_ticks = 1;
    int dwell_ticks = (int)(6.0f * model_tau_s / DT_S);
    if (dwell_ticks < 1200) dwell_ticks = 1200;
    if (dwell_ticks > 20000) dwell_ticks = 20000;

    double kp_mult_sum = 0.0, ki_mult_sum = 0.0, kd_mult_sum = 0.0;
    long ramp_tick_count = 0;
    long sat_tick_count = 0, total_tick_count = 0;
    bool nan_seen = false;
    float ceiling_c = ambient_c + (ad ? ad->bounds_k_dc : model_k_dc) + 400.0f;
    float floor_c = ambient_c - 1.0f;
    bool bounds_ok = true;

    firing_segment_score_t seg_scores[4];
    int seg_count = 0;

    struct { bool is_ramp; float rate_c_per_hr_signed; float target_start, target_end; int ticks; } segs[4] = {
        { true,  ramp_rate_c_per_hr, ambient_c, t1, ramp1_ticks },
        { false, 0.0f,               t1, t1, dwell_ticks },
        { true,  ramp_rate_c_per_hr, t1, t2, ramp2_ticks },
        { false, 0.0f,               t2, t2, dwell_ticks },
    };

    for (int s = 0; s < 4; s++) {
        firing_score_seg_t seg;
        float mean_target_c = segs[s].is_ramp ? (segs[s].target_start + segs[s].target_end) / 2.0f
                                               : segs[s].target_start;
        firing_score_seg_begin(&seg, &fscfg, 0, segs[s].rate_c_per_hr_signed, mean_target_c,
                               model_dead_time_s, model_tau_s);

        float target_c = segs[s].target_start;
        float step_per_tick = segs[s].is_ramp ? (segs[s].target_end - segs[s].target_start) / (float)segs[s].ticks
                                               : 0.0f;

        bool dwelling = !segs[s].is_ramp;

        for (int t = 0; t < segs[s].ticks; t++) {
            if (segs[s].is_ramp) target_c += step_per_tick;

            float error_c = target_c - pstate.sensor_c;
            float rate_meas = pid_state.d_filtered;

            if (ad) {
                /* Mirror of production pid_fuzzy_prepare_gains() (profile_
                 * executor_pid_tick.c), same call order, same functions:
                 *   1. harvest_freeze forces 0 while adaptive_tune is
                 *      harvesting a dwell (the shipped "Option B" contract);
                 *   2. the sec 3 gate's strength = f(c, cap_L);
                 *   3. the N3 oscillation backstop, ticked on EVERY tick of
                 *      BOTH adaptive arms (not only the fuzzy one) so the two
                 *      arms' adaptation state stays identical while strength
                 *      is 0 -- that identity is what makes firing 1 of
                 *      A_FUZZY_AT bit-for-bit A_PID_AT (plan sec 7 gate 2). */
                bool harvest_freeze = dwelling && adaptive_tune_get_enabled(0);
                uint8_t gated = 0;
                if (ad->fuzzy && !harvest_freeze) {
                    gated = pid_fuzzy_confidence_strength_pct(adaptive_tune_get_fuzzy_confidence_c(0), ad->cap_l);
                }
                if (pid_fuzzy_oscillation_tick(&ad->osc, error_c, DT_S)) {
                    adaptive_tune_fuzzy_confidence_floor_now(0);
                }
                if (ad->osc.tripped_this_firing) { gated = 0; ad->oscillation_tripped = true; }
                strength_pct = gated;

                ad->strength_sum += (double)gated;
                ad->strength_ticks++;
                if (gated > ad->strength_max) ad->strength_max = gated;
                if (gated > 0) ad->ticks_with_strength_gt_0++;

                /* sec 7 gate 3's raw material: dwell-phase error zero
                 * crossings, the same quantity the limit-cycle finding was
                 * measured in (29 in the oscillating arm, 0 in both stable
                 * ones). Counted here, never adjudicated here. */
                if (dwelling) {
                    if (ad->have_prev_dwell_error &&
                        ((ad->prev_dwell_error_c < 0.0f && error_c > 0.0f) ||
                         (ad->prev_dwell_error_c > 0.0f && error_c < 0.0f))) {
                        ad->dwell_zero_crossings++;
                    }
                    ad->prev_dwell_error_c = error_c;
                    ad->have_prev_dwell_error = true;
                } else {
                    ad->have_prev_dwell_error = false;
                }
            }

            float adj_kp = base_kp, adj_ki = base_ki, adj_kd = base_kd;
            if (static_matched) {
                adj_kp = base_kp * static_kp_mult;
                adj_ki = base_ki * static_ki_mult;
                adj_kd = base_kd * static_kd_mult;
            } else {
                pid_fuzzy_adjust(error_c, rate_meas, error_band_c, rate_band_c_per_s,
                                 base_kp, base_ki, base_kd, strength_pct, &adj_kp, &adj_ki, &adj_kd);
            }

            pid_rescale_integral_for_new_ki(&pid_state, prev_effective_ki, adj_ki);
            prev_effective_ki = adj_ki;

            pid_cfg_t cfg = { adj_kp, adj_ki, adj_kd, D_FILTER_TAU_S, SETPOINT_WEIGHT_B, PID_RANGE_C };
            float duty = pid_update(&pid_state, &cfg, target_c, pstate.sensor_c, DT_S, 0.0f, 0.0f);

            sim_high_temp_scale_conductances(&plant_cfg, plant_cfg_in->g_ea_w_per_c, plant_cfg_in->g_la_w_per_c,
                                              pstate.load_c);
            sim_plant_three_node_step(&pstate, &plant_cfg, duty, DT_S);

            if (isnan(pstate.sensor_c) || isnan(pstate.element_c)) nan_seen = true;
            if (pstate.sensor_c < floor_c || pstate.sensor_c > ceiling_c) bounds_ok = false;

            if (ad) {
                /* The real production harvesting seam. q1() quantizes to the
                 * MAX31856's reporting resolution so the settle-slope-floor
                 * gate sees the same coarseness a real board would, exactly
                 * as sim_scenarios_adaptive.c/test_adaptive_tune.c do. */
                adaptive_tune_zone_tick(0, q1(pstate.sensor_c), true, duty, dwelling, ambient_c, DT_S);
            }

            bool saturated_high = duty >= 0.98f;
            total_tick_count++;
            if (saturated_high) sat_tick_count++;
            bool was_captured = zone_captured;
            firing_score_seg_tick(&seg, &zone_captured, target_c, pstate.sensor_c, saturated_high, DT_S);

            if (segs[s].is_ramp && (was_captured || zone_captured) && !(saturated_high && error_c > 0.0f)) {
                kp_mult_sum += (base_kp != 0.0f) ? (adj_kp / base_kp) : 1.0;
                ki_mult_sum += (base_ki != 0.0f) ? (adj_ki / base_ki) : 1.0;
                kd_mult_sum += (base_kd != 0.0f) ? (adj_kd / base_kd) : 1.0;
                ramp_tick_count++;
            }

            /* Rule-cell occupancy: every tick, whole firing, regardless of
             * arm or capture -- this answers "where does this cell's own
             * (error, rate) trajectory sit relative to the fuzzy bands," a
             * question about the operating point, not about whether fuzzy
             * happened to be engaged this arm. */
            int eb = dominant_bucket(error_c, error_band_c);
            int rb = dominant_bucket(rate_meas, rate_band_c_per_s);
            out->rule_hist[eb * 3 + rb]++;
            out->rule_hist_total++;
        }

        firing_segment_score_t score;
        if (firing_score_seg_finish(&seg, &score) && seg_count < 4) {
            seg_scores[seg_count++] = score;
        }
    }

    if (pstate.delay_truncated) {
        /* sec 6.1: the sensor delay ring ran out of capacity and silently
         * shortened the effective dead time -- the exact DT_S-trap mechanism
         * that once made a 76.9s cell look byte-identical to an unrelated
         * 64.0s one. Refuse loudly rather than measure a wrong dead time. */
        snprintf(out->refusal_reason, sizeof(out->refusal_reason),
                 "sensor delay ring truncated (sensor_delay_s=%.2f exceeds SIM_PLANT_DELAY_MAX_STEPS*dt=%.2f)",
                 (double)plant_cfg_in->sensor_delay_s, (double)(SIM_PLANT_DELAY_MAX_STEPS * DT_S));
        return false;
    }
    if (nan_seen) {
        snprintf(out->refusal_reason, sizeof(out->refusal_reason), "NaN observed in plant state");
        return false;
    }
    if (!bounds_ok) {
        snprintf(out->refusal_reason, sizeof(out->refusal_reason), "sensor reading left [%.1f, %.1f]",
                 (double)floor_c, (double)ceiling_c);
        return false;
    }

    double lag_sum = 0.0; int lag_n = 0;
    double lag_signed_sum = 0.0; int lag_signed_n = 0;
    double settle_sum = 0.0; int settle_n = 0;
    double steady_sum = 0.0; int steady_n = 0;
    double peak_sum = 0.0; int peak_n = 0;
    double under_sum = 0.0; int under_n = 0;
    for (int i = 0; i < seg_count; i++) {
        const firing_segment_score_t *sc2 = &seg_scores[i];
        if (sc2->has[FIRING_SUBSCORE_LAG_S]) { lag_sum += sc2->value[FIRING_SUBSCORE_LAG_S]; lag_n++; }
        if (sc2->has[FIRING_SUBSCORE_LAG_SIGNED_S]) { lag_signed_sum += sc2->value[FIRING_SUBSCORE_LAG_SIGNED_S]; lag_signed_n++; }
        if (sc2->has[FIRING_SUBSCORE_SETTLE_S]) { settle_sum += sc2->value[FIRING_SUBSCORE_SETTLE_S]; settle_n++; }
        if (sc2->has[FIRING_SUBSCORE_STEADY_RMS_C]) { steady_sum += sc2->value[FIRING_SUBSCORE_STEADY_RMS_C]; steady_n++; }
        if (sc2->has[FIRING_SUBSCORE_ENTRY_PEAK_C]) { peak_sum += sc2->value[FIRING_SUBSCORE_ENTRY_PEAK_C]; peak_n++; }
        if (sc2->has[FIRING_SUBSCORE_ENTRY_UNDERSHOOT_C]) { under_sum += sc2->value[FIRING_SUBSCORE_ENTRY_UNDERSHOOT_C]; under_n++; }
    }
    if (lag_n) { out->have_lag = true; out->lag_s = (float)(lag_sum / lag_n); }
    if (lag_signed_n) { out->have_lag_signed = true; out->lag_signed_s = (float)(lag_signed_sum / lag_signed_n); }
    if (settle_n) { out->have_settle = true; out->settle_s_2c = (float)(settle_sum / settle_n); }
    if (steady_n) { out->have_steady = true; out->steady_rms_c = (float)(steady_sum / steady_n); }
    if (peak_n) { out->have_entry_peak = true; out->entry_peak_c = (float)(peak_sum / peak_n); }
    if (under_n) { out->have_entry_under = true; out->entry_undershoot_c = (float)(under_sum / under_n); }

    out->fuzzy_error_band_c = error_band_c;
    out->fuzzy_rate_band_c_per_s = rate_band_c_per_s;
    out->sat_frac = total_tick_count ? (float)((double)sat_tick_count / (double)total_tick_count) : 0.0f;
    out->applied_kp_mult_mean = ramp_tick_count ? (float)(kp_mult_sum / ramp_tick_count) : 1.0f;
    out->applied_ki_mult_mean = ramp_tick_count ? (float)(ki_mult_sum / ramp_tick_count) : 1.0f;
    out->applied_kd_mult_mean = ramp_tick_count ? (float)(kd_mult_sum / ramp_tick_count) : 1.0f;

    /* A cell whose PID/base arm is stuck saturated for essentially the
     * whole firing is not exercising the control law at all -- u_req
     * perpetually saturated. This is the "infeasible, refuse loudly"
     * requirement for a cell the mask did not catch (the mask only
     * excludes the one analytically-derived corner; this is the runtime
     * backstop). 0.97 leaves room for the legitimate brief saturation a
     * fast ramp's entry transient produces. */
    if (out->sat_frac >= 0.97f) {
        snprintf(out->refusal_reason, sizeof(out->refusal_reason),
                 "duty saturated (>=0.98) for %.1f%% of the firing -- u_req perpetually saturated, infeasible cell",
                 (double)(out->sat_frac * 100.0f));
        return false;
    }

    out->ok = true;
    return true;
}

static const char *fmt_val(char *buf, size_t n, bool have, float v)
{
    if (!have) { snprintf(buf, n, "NA"); return buf; }
    snprintf(buf, n, "%.4f", (double)v);
    return buf;
}

/* Row-emission bookkeeping shared by the single-firing arms and the adaptive
 * chains -- file-static so ONE printer serves both and the 23-column row
 * shape cannot drift between them. */
static long s_rows_emitted = 0;
static float s_occ_min = 2.0f, s_occ_max = -1.0f;

/* Emits one TSV data row. arm_label is CELL_ARM_NAMES[] for a single-firing
 * arm and "<arm>_F<n>" for one firing of an adaptive chain -- the column
 * count and every existing column's meaning are UNCHANGED, which is what
 * keeps the three original arms byte-identical and what keeps per-CELL
 * counting (plan sec 8) working: group rows by cell_id, exactly as before. */
static void print_cell_row(const sim_factorial_cell_t *cell, const char *arm_label,
                            const cell_firing_result_t *r)
{
    char buf1[32], buf2[32], buf3[32], buf4[32], buf5[32], buf6[32];
    float center_frac = r->rule_hist_total ? (float)((double)r->rule_hist[4] / (double)r->rule_hist_total) : 0.0f;
    if (center_frac < s_occ_min) s_occ_min = center_frac;
    if (center_frac > s_occ_max) s_occ_max = center_frac;

    printf("%s\t%d\t%.4f\t%s\t%.4f\t%.4f\t%.1f\t%s\t%.4f\t%.4f\t%s\t"
           "%s\t%s\t%s\t%s\t%s\t%s\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t",
           cell->cell_id, (int)cell->stage,
           (double)cell->a1_load_mass_mult, cell->a2_headroom == SIM_FAC_A2_TIGHT ? "TIGHT" : "AMPLE",
           (double)cell->a3_sensor_bias_p, (double)cell->a4_loss_scale_span_r_s,
           (double)cell->a5_ramp_rate_c_per_hr,
           cell->a6_tune == SIM_FAC_A6_MATCHED ? "MATCHED" : cell->a6_tune == SIM_FAC_A6_HOT ? "HOT" :
               cell->a6_tune == SIM_FAC_A6_COLD ? "COLD" : "SLOW_INTEGRAL",
           (double)cell->a7_phi, (double)cell->a8_bi, arm_label,
           fmt_val(buf1, sizeof(buf1), r->have_lag, r->lag_s),
           fmt_val(buf2, sizeof(buf2), r->have_lag_signed, r->lag_signed_s),
           fmt_val(buf3, sizeof(buf3), r->have_settle, r->settle_s_2c),
           fmt_val(buf4, sizeof(buf4), r->have_steady, r->steady_rms_c),
           fmt_val(buf5, sizeof(buf5), r->have_entry_peak, r->entry_peak_c),
           fmt_val(buf6, sizeof(buf6), r->have_entry_under, r->entry_undershoot_c),
           (double)r->sat_frac, (double)r->applied_kp_mult_mean, (double)r->applied_ki_mult_mean,
           (double)r->applied_kd_mult_mean, (double)center_frac);
    for (int h = 0; h < 9; h++) {
        printf("%.4f%s", r->rule_hist_total ? (double)((double)r->rule_hist[h] / (double)r->rule_hist_total) : 0.0,
               h < 8 ? ":" : "");
    }
    printf("\n");
    s_rows_emitted++;
}

/* ------------------------------------------------------------------
 * Plan sec 5: one adaptive chain = N_FIRINGS sequential firings for ONE
 * (cell, arm) pair, with adaptation state carried across them.
 *
 * REFUSALS ARE NEVER SILENT (3b3d631's lesson, restated in the task): a
 * chain that cannot start prints CELL_REFUSED for all N firings, and a
 * chain that dies at firing k prints CELL_REFUSED for firings k..N. A cell
 * that silently loses an arm biases exactly the per-cell tally this whole
 * campaign exists to produce, so every arm-instance that does not emit a
 * data row emits a refusal line instead -- one or the other, always.
 *
 * out_firing1 (optional) receives firing 1's result, which main() uses for
 * the sec 7 gate-2 floor-identity comparison between the two arms.
 * ------------------------------------------------------------------ */
static bool run_cell_chain(const sim_factorial_cell_t *cell, const sim_plant_cfg_t *plant,
                            float model_k_dc, float model_tau_s, float model_dead_time_s,
                            float ambient_c, float t1_off, float t2_off, float true_k_dc,
                            cell_arm_t arm, cell_firing_result_t *out_firing1)
{
    const char *arm_name = CELL_ARM_NAMES[arm];
    bool fuzzy = (arm == CELL_ARM_FUZZY_AT);

    /* Fresh module state for this (cell, arm) chain -- nothing from the
     * previous chain may leak in (same reset block sim_scenarios_adaptive.c
     * uses, kept in step with it). */
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

    s_fake_zone_cfg[0].k_dc = model_k_dc;
    s_fake_zone_cfg[0].tau_s = model_tau_s;
    s_fake_zone_cfg[0].dead_time_s = model_dead_time_s;
    s_fake_zone_cfg[0].autotune_baseline_k_dc = 0.0f; /* not recorded yet -- bootstraps from the live model */

    fopdt_model_t m0 = {
        .k_gain_c_per_duty = model_k_dc, .tau_s = model_tau_s, .dead_time_s = model_dead_time_s,
        .valid = true, .settled = true, .tau_consistent_with_gain = true, .extrapolation_converged = true,
    };
    autotune_gains_t g0 = pid_autotune_tune_from_fopdt(&m0, AUTOTUNE_RULE_SIMC, 0.0f);
    if (g0.refusal != AUTOTUNE_REFUSAL_OK) {
        for (int fi = 1; fi <= N_FIRINGS; fi++) {
            printf("CELL_REFUSED %s %s_F%d: initial SIMC tune refused: %s\n",
                   cell->cell_id, arm_name, fi, g0.refusal_reason);
        }
        return false;
    }
    s_fake_zone_cfg[0].kp = g0.kp;
    s_fake_zone_cfg[0].ki = g0.ki;
    s_fake_zone_cfg[0].kd = g0.kd;
    s_fake_adaptive_enabled[0] = true;
    adaptive_tune_zones[0].enabled = true;

    for (int fi = 1; fi <= N_FIRINGS; fi++) {
        float belief_k, belief_tau, belief_dead;
        if (!zones_config_get_model(0, &belief_k, &belief_tau, &belief_dead)) {
            for (int j = fi; j <= N_FIRINGS; j++) {
                printf("CELL_REFUSED %s %s_F%d: zones_config test fake returned false\n",
                       cell->cell_id, arm_name, j);
            }
            return false;
        }

        cell_adaptive_ctx_t ad;
        memset(&ad, 0, sizeof(ad));
        ad.fuzzy = fuzzy;
        ad.bounds_k_dc = model_k_dc; /* fixed for the chain, see the field's own comment */
        /* The real sec 3 cap, on the CURRENT belief model's L/tau. */
        ad.cap_l = pid_fuzzy_confidence_cap_l(belief_dead, belief_tau);
        ad.confidence_c_at_start = adaptive_tune_get_fuzzy_confidence_c(0);
        pid_fuzzy_oscillation_reset(&ad.osc);

        cell_firing_result_t r;
        bool ok = run_cell_firing(plant, belief_k, belief_tau, belief_dead, ambient_c, t1_off, t2_off,
                                   cell->a5_ramp_rate_c_per_hr, arm, NULL, &ad, &r);
        if (!ok) {
            for (int j = fi; j <= N_FIRINGS; j++) {
                printf("CELL_REFUSED %s %s_F%d: %s%s\n", cell->cell_id, arm_name, j, r.refusal_reason,
                       (j > fi) ? " (chain cannot continue past the refused firing)" : "");
            }
            return false;
        }

        /* The real run-end safe-boundary hook, where profile_executor.c calls
         * it on hardware: AFTER the firing, never mid-firing. */
        profile_firing_run_record_t rec;
        memset(&rec, 0, sizeof(rec));
        rec.profile_id = (uint8_t)fi;
        rec.zones[0].active = true;
        rec.zones[0].stats.sample_count = 1000;
        rec.zones[0].stats.excluded_sample_count = 0;
        adaptive_tune_run_end(&rec, /*clean=*/true);

        adaptive_tune_zone_status_t st;
        adaptive_tune_get_status(0, &st);

        char arm_label[48];
        snprintf(arm_label, sizeof(arm_label), "%s_F%d", arm_name, fi);
        print_cell_row(cell, arm_label, &r);

        /* sec 7's per-(cell, arm, firing) activity instrumentation. A
         * SEPARATE line, not extra columns on the data row: adding columns
         * would have changed every pre-existing row and destroyed the
         * inertness proof for the three original arms. run_sim_factorial.ps1
         * includes these lines in its --of 1 vs --of 4 comparison. */
        printf("ADAPTIVE_DIAG\t%s\t%s\tfiring=%d/%d\tstrength_mean=%.4f\tstrength_max=%u\t"
               "ticks_strength_gt0=%ld\tticks_total=%ld\tconfidence_c_at_start=%u\tconfidence_c_after=%u\t"
               "cap_l=%.4f\terror_band_c=%.4f\trate_band_c_per_s=%.6f\tbelief_k_dc=%.4f\ttrue_k_dc=%.4f\t"
               "belief_tau_s=%.4f\tbelief_dead_time_s=%.4f\toscillation_tripped=%s\t"
               "dwell_zero_crossings=%ld\tring_count=%u\trefine_applied=%s\trefine_delta_pct=%.3f\n",
               cell->cell_id, arm_label, fi, N_FIRINGS,
               ad.strength_ticks ? ad.strength_sum / (double)ad.strength_ticks : 0.0,
               (unsigned)ad.strength_max, ad.ticks_with_strength_gt_0, ad.strength_ticks,
               (unsigned)ad.confidence_c_at_start, (unsigned)adaptive_tune_get_fuzzy_confidence_c(0),
               (double)ad.cap_l, (double)r.fuzzy_error_band_c, (double)r.fuzzy_rate_band_c_per_s,
               (double)belief_k, (double)true_k_dc, (double)belief_tau, (double)belief_dead,
               ad.oscillation_tripped ? "yes" : "no", ad.dwell_zero_crossings,
               (unsigned)st.ring_count,
               (st.has_applied && st.last_applied_profile_id == (uint8_t)fi) ? "yes" : "no",
               (double)st.last_delta_pct);

        if (fi == 1 && out_firing1) *out_firing1 = r;
    }
    return true;
}

int main(int argc, char **argv)
{
    SIM_PLANT_ASSERT_ABI_FRESH();

    int shard = 0, of = 1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--shard") == 0 && i + 1 < argc) shard = atoi(argv[++i]);
        else if (strcmp(argv[i], "--of") == 0 && i + 1 < argc) of = atoi(argv[++i]);
    }
    if (of < 1 || shard < 0 || shard >= of) {
        printf("sim_factorial_driver: invalid --shard/--of (need 0 <= shard < of, of >= 1); got shard=%d of=%d\n",
               shard, of);
        return 1;
    }

    static sim_factorial_cell_t cells[SIM_FACTORIAL_MAX_CELLS];
    size_t n = sim_factorial_generate(cells, SIM_FACTORIAL_MAX_CELLS);
    if (n != SIM_FACTORIAL_TOTAL_COUNT_EXPECTED) {
        printf("sim_factorial_driver: FATAL sim_factorial_generate() returned %zu cells, expected %d\n",
               n, SIM_FACTORIAL_TOTAL_COUNT_EXPECTED);
        return 1;
    }

    printf("=== sim_factorial_driver: %zu cells x %d arms (A_PID, A_FUZZY50, A_STATIC_MATCHED, "
           "A_PID_AT x%d firings, A_FUZZY_AT x%d firings) ===\n", n, CELL_ARM_COUNT, N_FIRINGS, N_FIRINGS);
    printf("shard=%d of=%d. Report numbers, not verdicts -- effects analysis (P8/P11) is a separate dispatch.\n\n", shard, of);

    printf("cell_id\tstage\ta1_load_mass_mult\ta2_headroom\ta3_sensor_bias_p\ta4_loss_scale_span_r_s\t"
           "a5_ramp_rate_c_per_hr\ta6_tune\ta7_phi\ta8_bi\tarm\tlag_s\tlag_signed_s\tsettle_s_2c\t"
           "steady_rms_c\tentry_peak_c\tentry_undershoot_c\tsat_frac\tapplied_kp_mult_mean\t"
           "applied_ki_mult_mean\tapplied_kd_mult_mean\trulecell_center_frac\trulecell_hist9\n");

    long cells_refused = 0;
    long floor_identity_checked = 0, floor_identity_failures = 0;

    for (size_t ci = 0; ci < n; ci++) {
        if ((ci % (size_t)of) != (size_t)shard) continue;
        const sim_factorial_cell_t *cell = &cells[ci];

        sim_plant_cfg_t plant;
        float model_k_dc, model_tau_s, model_dead_time_s, ambient_c, t1_off, t2_off, true_k_dc;
        char build_refusal[200];
        if (!build_cell_plant(cell, &plant, &model_k_dc, &model_tau_s, &model_dead_time_s,
                               &ambient_c, &t1_off, &t2_off, &true_k_dc, build_refusal, sizeof(build_refusal))) {
            printf("CELL_REFUSED %s (all arms): %s\n", cell->cell_id, build_refusal);
            cells_refused++;
            continue;
        }

        float measured_mult[3] = {1.0f, 1.0f, 1.0f};
        bool cell_ok = true;

        cell_firing_result_t r_fuzzy50;
        bool have_fuzzy50 = run_cell_firing(&plant, model_k_dc, model_tau_s, model_dead_time_s, ambient_c,
                                             t1_off, t2_off, cell->a5_ramp_rate_c_per_hr, CELL_ARM_FUZZY50, NULL,
                                             NULL, &r_fuzzy50);
        if (have_fuzzy50) {
            measured_mult[0] = r_fuzzy50.applied_kp_mult_mean;
            measured_mult[1] = r_fuzzy50.applied_ki_mult_mean;
            measured_mult[2] = r_fuzzy50.applied_kd_mult_mean;
        } else {
            printf("CELL_REFUSED %s A_FUZZY50: %s\n", cell->cell_id, r_fuzzy50.refusal_reason);
            cell_ok = false;
        }

        for (int ai = 0; ai < CELL_ARM_SINGLE_COUNT; ai++) {
            cell_arm_t arm = (cell_arm_t)ai;
            if (arm == CELL_ARM_FUZZY50) {
                if (!have_fuzzy50) continue;
            }
            cell_firing_result_t r;
            bool ok;
            if (arm == CELL_ARM_FUZZY50) {
                r = r_fuzzy50;
                ok = true;
            } else {
                const float *mult_arg = (arm == CELL_ARM_STATIC_MATCHED) ? measured_mult : NULL;
                if (arm == CELL_ARM_STATIC_MATCHED && !have_fuzzy50) {
                    /* A_STATIC_MATCHED's whole purpose is to replay A_FUZZY50's measured
                     * effective gain multipliers with fuzzy math bypassed -- it has no
                     * independent gain definition of its own, so when A_FUZZY50 refuses
                     * there is nothing for it to match and it cannot be run at all (not
                     * "run with defaults", which would silently compare against a
                     * different, unintended control arm). Print a refusal line for the
                     * dependent arm too rather than dropping it with no trace, so the
                     * per-cell arm tally the fuzzy keep/remove decision depends on never
                     * loses a row silently. */
                    printf("CELL_REFUSED %s A_STATIC_MATCHED: dependent on A_FUZZY50, which refused: %s\n",
                           cell->cell_id, r_fuzzy50.refusal_reason);
                    cell_ok = false;
                    continue;
                }
                ok = run_cell_firing(&plant, model_k_dc, model_tau_s, model_dead_time_s, ambient_c, t1_off, t2_off,
                                      cell->a5_ramp_rate_c_per_hr, arm, mult_arg, NULL, &r);
            }
            if (!ok) {
                printf("CELL_REFUSED %s %s: %s\n", cell->cell_id, CELL_ARM_NAMES[ai], r.refusal_reason);
                cell_ok = false;
                continue;
            }

            print_cell_row(cell, CELL_ARM_NAMES[ai], &r);
        }

        /* ---- plan sec 5's two adaptive arms: two independent N-firing
         * chains over this same cell. They depend on NOTHING from the three
         * single-firing arms above (unlike A_STATIC_MATCHED, which replays
         * A_FUZZY50's multipliers) and so run even when those refused. ---- */
        cell_firing_result_t pid_at_f1, fuzzy_at_f1;
        memset(&pid_at_f1, 0, sizeof(pid_at_f1));
        memset(&fuzzy_at_f1, 0, sizeof(fuzzy_at_f1));
        bool pid_at_ok = run_cell_chain(cell, &plant, model_k_dc, model_tau_s, model_dead_time_s,
                                         ambient_c, t1_off, t2_off, true_k_dc, CELL_ARM_PID_AT, &pid_at_f1);
        bool fuzzy_at_ok = run_cell_chain(cell, &plant, model_k_dc, model_tau_s, model_dead_time_s,
                                           ambient_c, t1_off, t2_off, true_k_dc, CELL_ARM_FUZZY_AT, &fuzzy_at_f1);
        if (!pid_at_ok || !fuzzy_at_ok) cell_ok = false;

        /* Plan sec 7 gate 2, the FLOOR-IDENTITY gate: with the confidence
         * counter starting at 0, firing 1 of A_FUZZY_AT must be bit-for-bit
         * firing 1 of A_PID_AT -- low confidence means strength_pct == 0,
         * which means plain PID exactly, not "nearly". memcmp over the whole
         * result struct (zeroed at entry, so padding compares clean) rather
         * than a tolerance on selected fields: the claim registered in the
         * plan is bit-identity, so the check must be bit-identity. */
        if (pid_at_ok && fuzzy_at_ok) {
            if (memcmp(&pid_at_f1, &fuzzy_at_f1, sizeof(pid_at_f1)) != 0) {
                printf("FLOOR_IDENTITY_FAIL %s: A_FUZZY_AT_F1 differs from A_PID_AT_F1 -- fuzzy authority was "
                       "non-zero before any confidence was earned (plan sec 7 gate 2)\n", cell->cell_id);
                floor_identity_failures++;
            } else {
                floor_identity_checked++;
            }
        }

        if (!cell_ok) cells_refused++;
    }

    printf("\n=== sim_factorial_driver: shard %d/%d done. rows_emitted=%ld cells_with_a_refusal=%ld "
           "rulecell_center_frac_range=[%.4f,%.4f] ===\n",
           shard, of, s_rows_emitted, cells_refused,
           (double)(s_occ_max < 0.0f ? 0.0f : s_occ_min), (double)(s_occ_max < 0.0f ? 0.0f : s_occ_max));
    printf("=== floor-identity (plan sec 7 gate 2): %ld cells checked, %ld FAILED ===\n",
           floor_identity_checked, floor_identity_failures);
    if (floor_identity_failures > 0) {
        printf("FAIL: plan sec 7 gate 2 violated -- see the FLOOR_IDENTITY_FAIL lines above. This is a "
               "non-zero process exit, never a printed-only verdict.\n");
        return 1;
    }
    return 0;
}
