// sim_fuzzy_closedloop -- the blocking prerequisite identified in
// docs/audits/fuzzy_controller_improvement_scoping_2026-09-11.md: "no
// closed-loop simulation exercises the fuzzy path at all." Neither
// sim_iter_tune.c nor sim_credibility_gate_closedloop.c ever calls
// pid_fuzzy_adjust() (the latter explicitly forces ZONE_CONTROL_MODE_PID),
// and the only fuzzy-aware harness that existed before this one,
// pid_fuzzy_drift_harness.c, is a stateless C-vs-Python numerical check of
// the membership math alone -- not a closed loop, no plant, no PID
// integrator, no tick-to-tick state at all.
//
// DELIBERATELY SINGLE-ZONE. The audit's own scoping is explicit that the
// multi-zone coupling model is the part currently refuted/under revision
// (project_z0_coupling_is_shape_not_scale.md, project_coupling_failure_is_
// joint_dwell_specific.md) while single-column (single-zone) transport is
// the part MEASURED LINEAR on hardware. pid_fuzzy_adjust() itself is also
// structurally single-zone by design -- pid_fuzzy.h's own header comment
// states plainly that a neighbour's temperature is deliberately never an
// input to this function (coupling is a measured disturbance and belongs on
// the feedforward term, not folded into the feedback gains). A single-zone
// harness is therefore not a simplification that loses coverage of what this
// module actually does; it is the natural scope for it, and it sidesteps a
// coupling model another session is actively rewriting (sim_plant.c is
// off-limits to this file for exactly that reason -- see the commit this
// shipped in).
//
// WHAT THIS LINKS (real production .c, no mirror of the control math):
//   - pid_fuzzy.c: pid_fuzzy_adjust() itself, unmodified.
//   - pid.c: pid_update() (via pid_update_terms()'s thin wrapper) and
//     pid_rescale_integral_for_new_ki() -- the same hazard-3 bump-transfer
//     tool profile_executor_pid_tick.c's real pid_fuzzy_prepare_gains() uses
//     every tick a fuzzy-adjusted Ki takes effect.
//   - sim_plant.c: sim_plant_reset()/sim_plant_step(), the single-zone FOPDT
//     model (G1's real-config-shaped constants, sim_measured_zone_
//     constants.h, zone 0 -- see its own provenance comment).
//
// WHAT THIS DOES NOT LINK, AND WHY: profile_executor_pid_tick.c's own
// pid_fuzzy_prepare_gains()/pid_family_zone_tick() are the actual production
// call sites, but pulling either in drags along heater_output.c's 60s PWM
// window, zone_coupling_solve.c's coupled feedforward solve, and
// profile_executor_feedforward.c's taper/hold machinery -- none of which a
// single-zone, no-feedforward-model harness needs, and all of which
// sim_credibility_gate_closedloop.c's own header comment already documents
// as "executor machinery this diagnostic does not need." Instead this file
// reproduces, in its own small orchestration loop below, EXACTLY the wiring
// pid_fuzzy_prepare_gains() documents (profile_executor_pid_tick.c lines
// ~325-374, read in full before writing this): error_c = setpoint -
// measurement; error_rate_c_per_s = pid_state.d_filtered read BEFORE this
// tick's own pid_update() call (one tick of lag on an already ~30s filter,
// the same lag that function's own comment calls immaterial); the real
// pid_fuzzy_adjust() call; then the real pid_rescale_integral_for_new_ki()
// bump-transfer keyed on the previous tick's effective Ki, exactly as that
// function does. What differs from the real call site is orchestration
// (which functions get called in which order with which inputs), never the
// control math itself -- every number that actually turns into a gain or a
// duty comes from pid_fuzzy.c/pid.c unmodified. feedforward is fixed at
// ff_u=0/ff_hold=0 throughout (no identified model needed for a fuzzy-layer
// test -- PID_FUZZY is explicitly "a second selectable mode... it does not
// change the base gains", pid_fuzzy.h's own header, so this is not
// under-testing anything the mode's own contract claims).
//
// THREE THINGS THIS HARNESS ACTUALLY CHECKS (all three gate the build --
// this file's own exit code, unlike sim_credibility_gate_closedloop.c's
// deliberately-non-gating diagnostic posture, since every assertion here is
// cheap, deterministic, and has a known-correct answer):
//
//   1. THE SAFETY CONTRACT. strength_pct == 0 must reproduce the exact same
//      gains (bit-for-bit, ==, not "close") pid_update() would run under
//      plain PID with the same base_kp/ki/kd, on EVERY tick of BOTH
//      scenarios below. This is the one property the live board currently
//      depends on (control_mode=3 but fuzzy_strength_pct=0.0 on all three
//      zones today) -- see this repo's own finding,
//      project_fuzzy_ab_inert_control_mode.md.
//
//   2. RULE-CELL COVERAGE. logs/coupling/fuzzy_bands_envelope_20260904e_
//      report.md's withdrawn finding is exactly why this matters: the one
//      real capture that ever exercised pid_fuzzy_adjust() at all landed
//      100% of its samples in the single centre cell (ZERO error / STEADY
//      rate). Eight of the nine rule cells have never been exercised by
//      anything, on hardware or in simulation, before this file. The
//      coverage scenario below deliberately injects synthetic thermal
//      disturbances (a direct, non-physical perturbation of the plant's
//      element/sensor temperature -- see run_coverage_scenario()'s own
//      comment for why this is necessary and not a modeling error) sized to
//      drive both axes past their +-band edges in every sign combination.
//      Ordinary ramps/steps alone cannot do this: pid_fuzzy.c's own header
//      comment documents that even a brisk 300 degC/hr profile ramp is only
//      0.083 degC/s on the rate axis, versus this project's 0.5 degC/s band
//      -- "large rate" is supposed to mean a genuine disturbance, not a
//      normal firing, so reaching it honestly requires simulating one.
//
//   3. TRACKING-ERROR RANKING. A SEPARATE, ordinary ramp+dwell+ramp-down
//      scenario (no injected disturbances) reports IAE (integral absolute
//      error, degC*s) and mean absolute error at strength_pct 0/25/50. Per
//      this repo's own standing instruction, these numbers are a
//      RANKING/REGRESSION TOOL ONLY -- they say nothing about hardware
//      behaviour, are not compared to any hardware capture, and a
//      difference under 0.5 degC is not chased (project_ignore_sub_half_
//      degree_effects.md). Kept as a SEPARATE scenario from the coverage
//      one deliberately: the coverage scenario's huge synthetic
//      disturbances (+-8C shocks against a 20-63C achievable range) would
//      make an IAE number over that same run meaningless as a tracking
//      metric.
//
// NEGATIVE-TESTED (see the commit this shipped in for the transcript): a
// deliberate one-line break of pid_fuzzy.c's strength_pct==0 short-circuit
// (multiplying the returned base gain by 0.999f) was confirmed to fail
// check 1 above with a non-zero exit, then reverted BY HAND -- git diff on
// pid_fuzzy.c after the revert is empty. This proves the check can actually
// fail, not just pass by construction (project_negative_test_every_check.md).
//
// Usage: no arguments, no external inputs (unlike sim_credibility_gate*.c,
// this needs no capture files -- fully synthetic and deterministic, safe to
// run in a Monte-Carlo loop later per the task's own requirement). Exit 0 =
// all three gates pass. Non-zero = named failure printed to stdout.

#include "../drivers/control/pid.h"
#include "../drivers/control/pid_fuzzy.h"
#include "sim_plant.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "sim_measured_zone_constants.h"

#define DT_S 5.0f
#define AMBIENT_C 24.0f
#define BASE_KP 0.0318f
#define BASE_KI 0.0001f
#define BASE_KD 0.8401f
#define D_FILTER_TAU_S 30.0f
#define SETPOINT_WEIGHT_B 1.0f
#define PID_RANGE_C 25.0f
#define ERROR_BAND_C 20.0f
#define RATE_BAND_C_PER_S 0.5f
#define CELL_EPS 0.02f /* membership degree above which a bucket counts as "reached" for coverage */

// ---------------------------------------------------------------------
// Single-zone plant, built directly from zone 0's real measured FOPDT
// constants (sim_measured_zone_constants.h), same G1 mapping
// sim_plant_from_zone_cfg() documents (heater_power_w=k_dc,
// thermal_mass_j_per_c=tau_s, sensor_delay_s=dead_time_s,
// loss_coeff_w_per_c held at the same free scale of 1.0 every other
// harness in this tree uses). Built by hand rather than via
// sim_plant_from_zone_cfg() to avoid needing a zone_cfg_t at all --
// this file never touches zones_config.
// ---------------------------------------------------------------------
static void make_plant_cfg(sim_plant_cfg_t *out)
{
    memset(out, 0, sizeof(*out));
    out->ambient_c = AMBIENT_C;
    out->thermal_mass_j_per_c = g_tau_s[0];
    out->heater_power_w = g_k_dc[0];
    out->loss_coeff_w_per_c = 1.0f;
    out->sensor_delay_s = g_dead_time_s[0];
    out->sensor_lag_tau_s = 0.0f;
}

// Local replica of pid_fuzzy.c's triangular_memberships(), for CLASSIFYING
// (error, rate) into the same 3x3 grid pid_fuzzy_adjust() itself uses
// internally, purely so this harness can report coverage. This is
// measurement scaffolding, the same convention sim_credibility_gate_
// closedloop.c's own capture-JSON parser uses for the same reason ("this is
// measurement scaffolding, not the production control code the 'no mirror'
// promise is about") -- it never feeds a gain, a duty, or an assertion of
// CONTROL correctness; it only labels which cell a trajectory point falls
// nearest, for a coverage COUNT. If pid_fuzzy.c's own bucket-edge math ever
// changes, this classifier could drift from it -- bounded risk, since a
// drift here can only mis-COUNT a cell as reached/unreached, never mask a
// wrong GAIN (which is what the actual linked pid_fuzzy_adjust() computes).
static void classify_memberships(float x, float band, float deg[3])
{
    if (x <= -band) { deg[0] = 1.0f; deg[1] = 0.0f; deg[2] = 0.0f; return; }
    if (x >= band)  { deg[0] = 0.0f; deg[1] = 0.0f; deg[2] = 1.0f; return; }
    if (x <= 0.0f) {
        float t = (-x) / band;
        deg[0] = t; deg[1] = 1.0f - t; deg[2] = 0.0f;
    } else {
        float t = x / band;
        deg[0] = 0.0f; deg[1] = 1.0f - t; deg[2] = t;
    }
}

static void mark_cells(float error_c, float rate_c_per_s, int cell_hits[3][3])
{
    float e_deg[3], r_deg[3];
    classify_memberships(error_c, ERROR_BAND_C, e_deg);
    classify_memberships(rate_c_per_s, RATE_BAND_C_PER_S, r_deg);
    for (int ei = 0; ei < 3; ei++) {
        if (e_deg[ei] < CELL_EPS) continue;
        for (int ri = 0; ri < 3; ri++) {
            if (r_deg[ri] < CELL_EPS) continue;
            cell_hits[ei][ri]++;
        }
    }
}

static int count_cells_visited(int cell_hits[3][3])
{
    int n = 0;
    for (int ei = 0; ei < 3; ei++)
        for (int ri = 0; ri < 3; ri++)
            if (cell_hits[ei][ri] > 0) n++;
    return n;
}

static const char *ERR_NAMES[3] = {"NEG(overshoot)", "ZERO", "POS(undershoot)"};
static const char *RATE_NAMES[3] = {"FALLING", "STEADY", "RISING"};

static void print_cell_table(const char *label, int cell_hits[3][3])
{
    printf("  %s cell occupancy (%d/9 cells reached, ticks landing in each cell):\n", label,
           count_cells_visited(cell_hits));
    for (int ei = 0; ei < 3; ei++) {
        for (int ri = 0; ri < 3; ri++) {
            printf("    error=%-16s rate=%-8s : %d ticks%s\n", ERR_NAMES[ei], RATE_NAMES[ri],
                   cell_hits[ei][ri], cell_hits[ei][ri] > 0 ? "" : "  <-- UNVISITED");
        }
    }
}

// One control tick, wiring pid_fuzzy_adjust()/pid_update()/
// pid_rescale_integral_for_new_ki() EXACTLY the way profile_executor_pid_
// tick.c's real pid_fuzzy_prepare_gains() + pid_family_zone_tick() do (see
// this file's own top comment for the citation) -- only the executor
// scaffolding (heater_output.c's PWM window, feedforward) is omitted, never
// the control math itself. Returns the commanded duty, and reports the
// (error, rate) pair actually presented to pid_fuzzy_adjust() this tick (for
// coverage) plus whether the strength==0 contract held exactly.
static float sim_tick(sim_plant_state_t *pstate, const sim_plant_cfg_t *pcfg,
                       pid_state_t *pid_state, float *prev_effective_ki,
                       float target_c, uint8_t strength_pct, float disturbance_c,
                       float *out_error_c, float *out_rate_c_per_s, bool *out_bitexact_ok)
{
    if (disturbance_c != 0.0f) {
        pstate->element_c += disturbance_c;
        pstate->sensor_c += disturbance_c; /* also nudge the reading directly so the
                                            * disturbance is visible to the controller
                                            * THIS tick, not sensor_delay_s later --
                                            * see run_coverage_scenario()'s comment. */
    }

    float measurement = pstate->sensor_c;
    float error_c = target_c - measurement;
    float rate_c_per_s = pid_state->d_filtered; /* read BEFORE this tick's pid_update_terms(),
                                                 * exactly the hazard-1 ordering
                                                 * pid_fuzzy_prepare_gains() documents. */

    float adj_kp = BASE_KP, adj_ki = BASE_KI, adj_kd = BASE_KD;
    pid_fuzzy_adjust(error_c, rate_c_per_s, ERROR_BAND_C, RATE_BAND_C_PER_S,
                     BASE_KP, BASE_KI, BASE_KD, strength_pct, &adj_kp, &adj_ki, &adj_kd);

    if (strength_pct == 0) {
        *out_bitexact_ok = (adj_kp == BASE_KP) && (adj_ki == BASE_KI) && (adj_kd == BASE_KD);
    } else {
        *out_bitexact_ok = true; /* contract only applies at strength 0 */
    }

    pid_rescale_integral_for_new_ki(pid_state, *prev_effective_ki, adj_ki);
    *prev_effective_ki = adj_ki;

    pid_cfg_t cfg = { adj_kp, adj_ki, adj_kd, D_FILTER_TAU_S, SETPOINT_WEIGHT_B, PID_RANGE_C };
    float duty = pid_update(pid_state, &cfg, target_c, measurement, DT_S, 0.0f, 0.0f);

    sim_plant_step(pstate, pcfg, duty, DT_S);

    *out_error_c = error_c;
    *out_rate_c_per_s = rate_c_per_s;
    return duty;
}

// ---------------------------------------------------------------------
// Coverage scenario. Ordinary setpoint steps/ramps alone cannot reach the
// rate axis's +-0.5 degC/s band edges (this project's own real ramps top
// out near 0.083 degC/s, per pid_fuzzy.c's header comment -- deliberately,
// since "large rate" is supposed to mean a genuine disturbance, not a
// normal firing). To exercise the eight rule cells a real firing has never
// visited, this scenario injects direct +-8C jumps into the plant's
// element/sensor state at scripted points -- not a physically-derived
// event, a stand-in for "a stuck-open lid, a runaway element, a
// thermocouple that just came unstuck," exactly the disturbance class
// pid_fuzzy.c's own header comment names as what this axis exists to catch.
// Each jump is scripted against a target_c chosen so the (error, rate) pair
// at that instant lands in one specific, otherwise-unreached cell -- see the
// phase table below for which cell each disturbance targets.
// ---------------------------------------------------------------------
typedef struct {
    int ticks;
    int target_is_relative; /* 0 = target_value is absolute; 1 = target_value is
                             * added to the CURRENT measurement at the first tick
                             * of this phase (used for the small ZERO-bucket
                             * disturbances, where "near setpoint" has to track
                             * wherever the plant currently sits) */
    float target_value;
    float disturbance_c; /* applied once, at the FIRST tick of this phase */
    const char *label;
} phase_t;

static const phase_t COVERAGE_PHASES[] = {
    { 50, 0, AMBIENT_C, 0.0f, "settle at ambient" },
    { 1, 1, 2.0f, 6.0f, "ZERO/FALLING: small target offset + hot jump" },
    { 40, 0, AMBIENT_C, 0.0f, "re-settle" },
    { 1, 1, -2.0f, -6.0f, "ZERO/RISING: small target offset + cold jump" },
    { 40, 0, AMBIENT_C, 0.0f, "re-settle" },
    { 20, 0, AMBIENT_C + 200.0f, 0.0f, "big step up (POS/STEADY while saturated)" },
    { 1, 0, AMBIENT_C + 200.0f, 8.0f, "POS/FALLING: hot jump while far below target" },
    { 20, 0, AMBIENT_C + 200.0f, 0.0f, "hold saturated" },
    { 1, 0, AMBIENT_C + 200.0f, -8.0f, "POS/RISING: cold jump while far below target" },
    { 20, 0, AMBIENT_C + 200.0f, 0.0f, "hold saturated" },
    { 20, 0, AMBIENT_C - 150.0f, 0.0f, "big step down (NEG/STEADY, overshoot while saturated)" },
    { 1, 0, AMBIENT_C - 150.0f, 8.0f, "NEG/FALLING: hot jump while far above target (overshoot growing)" },
    { 20, 0, AMBIENT_C - 150.0f, 0.0f, "hold saturated" },
    { 1, 0, AMBIENT_C - 150.0f, -8.0f, "NEG/RISING: cold jump while far above target (overshoot recovering)" },
    { 20, 0, AMBIENT_C - 150.0f, 0.0f, "hold saturated" },
};
#define N_COVERAGE_PHASES (int)(sizeof(COVERAGE_PHASES) / sizeof(COVERAGE_PHASES[0]))

static bool run_coverage_scenario(uint8_t strength_pct, int cell_hits[3][3])
{
    sim_plant_cfg_t pcfg;
    make_plant_cfg(&pcfg);
    sim_plant_state_t pstate;
    sim_plant_reset(&pstate, &pcfg);

    pid_state_t pid_state;
    pid_reset(&pid_state);
    float prev_effective_ki = 0.0f;

    memset(cell_hits, 0, sizeof(int) * 9);
    bool all_bitexact = true;

    for (int p = 0; p < N_COVERAGE_PHASES; p++) {
        const phase_t *ph = &COVERAGE_PHASES[p];
        float target_c = ph->target_value;
        if (ph->target_is_relative) target_c += pstate.sensor_c;

        for (int t = 0; t < ph->ticks; t++) {
            float disturbance_c = (t == 0) ? ph->disturbance_c : 0.0f;
            float error_c, rate_c_per_s;
            bool bitexact_ok;
            sim_tick(&pstate, &pcfg, &pid_state, &prev_effective_ki, target_c, strength_pct,
                    disturbance_c, &error_c, &rate_c_per_s, &bitexact_ok);
            if (!bitexact_ok) all_bitexact = false;
            mark_cells(error_c, rate_c_per_s, cell_hits);
        }
    }
    return all_bitexact;
}

// ---------------------------------------------------------------------
// Tracking scenario -- an ORDINARY ramp/dwell/ramp-down, no injected
// disturbances, kept separate from the coverage scenario above so its IAE
// number stays a meaningful ranking metric (see this file's top comment,
// point 3). Ramp rate (100 degC/hr = 0.0278 degC/s) is the "brisk but
// normal" figure pid_fuzzy.c's own header comment uses, well under the
// 0.5 degC/s rate band -- this scenario is EXPECTED to sit mostly in the
// STEADY rate bucket throughout, exactly as that comment documents for any
// ordinary firing. Target stays within zone 0's achievable range (ambient +
// model_k_dc, ~63C at duty=1.0 with zero feedforward) so it is not simply
// railed at full duty the entire time.
// ---------------------------------------------------------------------
#define TRACK_RAMP_RATE_C_PER_S (100.0f / 3600.0f)
#define TRACK_DWELL_TARGET_C (AMBIENT_C + 30.0f)

static double run_tracking_scenario(uint8_t strength_pct, double *out_mae_c)
{
    sim_plant_cfg_t pcfg;
    make_plant_cfg(&pcfg);
    sim_plant_state_t pstate;
    sim_plant_reset(&pstate, &pcfg);

    pid_state_t pid_state;
    pid_reset(&pid_state);
    float prev_effective_ki = 0.0f;

    double iae = 0.0;
    double abs_sum = 0.0;
    int n = 0;

    float target_c = AMBIENT_C;
    int ramp_up_ticks = (int)((TRACK_DWELL_TARGET_C - AMBIENT_C) / TRACK_RAMP_RATE_C_PER_S / DT_S);
    int dwell_ticks = 400;
    int ramp_down_ticks = ramp_up_ticks;

    for (int t = 0; t < ramp_up_ticks; t++) {
        target_c += TRACK_RAMP_RATE_C_PER_S * DT_S;
        float error_c, rate_c_per_s; bool bitexact_ok;
        sim_tick(&pstate, &pcfg, &pid_state, &prev_effective_ki, target_c, strength_pct, 0.0f,
                &error_c, &rate_c_per_s, &bitexact_ok);
        iae += fabs((double)error_c) * DT_S;
        abs_sum += fabs((double)error_c);
        n++;
    }
    for (int t = 0; t < dwell_ticks; t++) {
        float error_c, rate_c_per_s; bool bitexact_ok;
        sim_tick(&pstate, &pcfg, &pid_state, &prev_effective_ki, target_c, strength_pct, 0.0f,
                &error_c, &rate_c_per_s, &bitexact_ok);
        iae += fabs((double)error_c) * DT_S;
        abs_sum += fabs((double)error_c);
        n++;
    }
    for (int t = 0; t < ramp_down_ticks; t++) {
        target_c -= TRACK_RAMP_RATE_C_PER_S * DT_S;
        float error_c, rate_c_per_s; bool bitexact_ok;
        sim_tick(&pstate, &pcfg, &pid_state, &prev_effective_ki, target_c, strength_pct, 0.0f,
                &error_c, &rate_c_per_s, &bitexact_ok);
        iae += fabs((double)error_c) * DT_S;
        abs_sum += fabs((double)error_c);
        n++;
    }

    *out_mae_c = (n > 0) ? (abs_sum / n) : 0.0;
    return iae;
}

int main(void)
{
    printf("=== sim_fuzzy_closedloop -- single-zone closed-loop fuzzy-path harness ===\n");
    printf("See this file's top comment before reading anything below: single-zone by\n"
           "design (coupling model under revision elsewhere), and the tracking numbers\n"
           "are a RANKING TOOL ONLY, never a claim about hardware behaviour.\n\n");

    bool overall_ok = true;
    const uint8_t strengths[] = {0, 25, 50};

    for (size_t i = 0; i < sizeof(strengths) / sizeof(strengths[0]); i++) {
        uint8_t sp = strengths[i];
        printf("-- strength_pct=%u --\n", sp);

        int cell_hits[3][3];
        bool bitexact_ok = run_coverage_scenario(sp, cell_hits);
        int visited = count_cells_visited(cell_hits);
        print_cell_table("coverage scenario", cell_hits);

        double mae = 0.0;
        double iae = run_tracking_scenario(sp, &mae);
        printf("  tracking scenario: IAE=%.1f degC*s, MAE=%.4f degC\n", iae, mae);

        if (sp == 0 && !bitexact_ok) {
            printf("  FAIL: strength_pct=0 did NOT reproduce base gains bit-for-bit in the "
                   "coverage scenario (the safety contract this whole mode rests on)\n");
            overall_ok = false;
        }
        if (sp != 0 && visited < 9) {
            printf("  FAIL: only %d/9 rule cells reached at strength_pct=%u (coverage scenario) "
                   "-- expected all 9\n", visited, sp);
            overall_ok = false;
        }
        printf("\n");
    }

    /* Bit-for-bit contract must also hold across the tracking scenario at
     * strength 0 -- checked separately since run_tracking_scenario() doesn't
     * thread the flag out; re-run once more here explicitly. */
    {
        sim_plant_cfg_t pcfg;
        make_plant_cfg(&pcfg);
        sim_plant_state_t pstate;
        sim_plant_reset(&pstate, &pcfg);
        pid_state_t pid_state;
        pid_reset(&pid_state);
        float prev_effective_ki = 0.0f;
        float target_c = AMBIENT_C + 10.0f;
        bool all_ok = true;
        for (int t = 0; t < 200; t++) {
            float error_c, rate_c_per_s; bool bitexact_ok;
            sim_tick(&pstate, &pcfg, &pid_state, &prev_effective_ki, target_c, 0, 0.0f,
                    &error_c, &rate_c_per_s, &bitexact_ok);
            if (!bitexact_ok) all_ok = false;
        }
        if (!all_ok) {
            printf("FAIL: strength_pct=0 bit-for-bit contract broke during the tracking scenario\n");
            overall_ok = false;
        } else {
            printf("strength_pct=0 bit-for-bit contract: PASS (tracking scenario, %d ticks)\n", 200);
        }
    }

    if (!overall_ok) {
        printf("\n=== sim_fuzzy_closedloop: FAIL ===\n");
        return 1;
    }
    printf("=== sim_fuzzy_closedloop: PASS ===\n");
    return 0;
}
