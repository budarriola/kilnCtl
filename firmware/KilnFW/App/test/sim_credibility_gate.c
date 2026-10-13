// sim_credibility_gate -- ITER_TUNE_REDESIGN.md sec 6.5: the model's
// own credibility gate. Before any iter_tune result (sim_iter_tune.c) or
// sensitivity sweep (sim_wide_temp_sweep.c) is believed to mean anything
// about the real kiln, the extended sim_kiln must be shown to reproduce a
// recorded real firing. This file is that check.
//
// NOT A REFIT. This harness never adjusts a single model constant to make
// a capture match better. Every physical parameter it drives sim_kiln with
// -- model_k_dc/model_tau_s/model_dead_time_s and the coupling matrix -- is
// the same checked-in snapshot sim_iter_tune.c and sim_wide_temp_sweep.c
// already use (tools/PcTools/config_presets/tuned_baseline_20260831.json,
// coupling_matrix_20260831.json, adopted commit 78f2134). The per-run
// input taken from a capture is the recorded PID DUTY COMMAND sequence and
// the recorded SETPOINT sequence -- i.e. this replays the plant's response
// to what the real controller commanded, it does not re-derive or tune
// anything from the recorded temperatures themselves against a fit. The
// recorded temperatures are otherwise used exclusively as the answer key
// for comparison -- but NOT exclusively: run_replay() also derives
// ambient_c from each capture's own first-valid actual_c per zone (mean
// across zones) and feeds it into sim_plant_from_zone_cfg() as the plant's
// loss reference, not merely an initial condition. Steady dwell
// temperature is ambient + sum_i(coupling_w_per_c[i][j]*duty[j]), so
// dwell_offset -- one of the scored bars below -- shifts close to 1:1 with
// this capture-derived number. That makes ambient_c one free parameter
// taken from the answer key, on exactly the metric it most directly moves.
// It is NOT a fit (nothing is adjusted to reduce error against actual_c),
// but it is real per-capture leakage from actual_c into the model's
// operating point, and the two captures used here differ in ambient_c by
// ~0.26C. Treat dwell_offset as measured under that dependency, not as an
// unconditional zero-leakage number.
//
// Two DIFFERENT captures are scored independently against this one fixed
// model (no fitting to either): logs/coupling/noise_floor_p7_run1.jsonl
// (CALIBRATION, in name only -- "calibration" here means "looked at first",
// not "fit to") and logs/coupling/noise_floor_p7d_run1.jsonl (HOLD-OUT, a
// separate firing of the same profile taken ~3.6 hours later, same day, a
// different rested start). Both were part of the six repeats used to build
// noise_floor.json -- real, independent firings of the same recipe, not the
// same data twice. Because the model is not fit to either, this is in fact
// a STRONGER non-circularity posture than the plan's minimum ask (fit
// coupling on one, gate on the other): here the model sees neither
// capture's actual_c before being scored against both.
//
// REPLAY MECHANISM (why G2/G3/G4 are load-bearing, per plan sec 6.5's own
// note): the capture's recorded "duty" field is the PID's raw [0,1] demand,
// sampled every ~5.2 s -- it is NOT what the relay physically did. This
// harness zero-order-holds that recorded duty between capture ticks and
// steps it through the REAL heater_output.c (G2, the 60 s PWM window) at a
// 1 s substep, then sim_relay_lag_step() (G3, actuation lag), then
// sim_kiln_step() (G1 measured plant + coupling), then
// sim_max31856_quantize_tc() (G4) -- reconstructing the same physical
// pipeline the real relay/element/thermocouple went through, driven by the
// same commands the real PID issued. Comparison is at the capture's own
// sample times against the recorded actual_c.
//
// Linked as-is (plan sec 6.3): heater_output.c, sim_plant.c. No pid.c and
// no zone_coupling_solve.c are linked or called -- this gate never
// recomputes a control decision, only replays one.
//
// Exit codes: 0 pass, 1 fail (numbers printed), 2 usage/parse error,
// 3 SKIP -- a named capture file is missing. 3 is deliberate and distinct
// from 0: this repo has shipped checks that silently reported green with
// zero coverage when their input vanished (build_host_tests.ps1's own
// bookkeeping comment, and TODO.md's "hardcoded-path guard" note) and this
// gate must never join that list.
//
// Usage: sim_credibility_gate.exe <calibration.jsonl> <holdout.jsonl> \
//                                  <noise_floor.json>
// All three paths are gitignored/local-only; if any is absent this prints
// what is missing and exits 3.
//
// ===================================================================
// DO NOT CLOSE THE DWELL-OFFSET BAR WITH THE `cplval75` ROW SCALARS
// (1.12 / 1.19 / 1.31). This was adjudicated and REJECTED on
// 2026-09-14 -- docs/audits/credibility_gate_scalar_adoption_2026-09-14.md.
// ===================================================================
// Those three numbers are a DATED EMPIRICAL OBSERVATION about the
// currently adopted coupling matrix, measured on plateaus at 62-75 C
// (docs/audits/cplval75_coupling_verdict_2026-09-10.md). They are NOT
// identified physics, and multiplying them into model_k_dc / the
// coupling rows here does close this gate's dwell-offset bar -- which
// is exactly why the temptation needs a warning rather than a shrug.
// Four measured reasons not to, all from THIS gate's own two captures:
//
//   1. This gate's two dwell segments sit at delta-T ~17 C and ~32.5 C.
//      z0's residual sign crossing is at delta-T ~23 C, so these dwells
//      STRADDLE it. Reduced over each dwell's final 40%, z0's DC
//      residual (G*u - delta-T_observed) is +1.006 / +1.143 at seg0 and
//      -1.749 / -2.615 at seg1 -- it CHANGES SIGN inside this gate's own
//      data, on both captures independently. A per-row scale cannot
//      produce a sign change. The shape is wrong for z0, not the size.
//   2. The per-cell scale z0 actually asks for is 0.944 / 1.057 / 0.938 /
//      1.086 -- it straddles 1.0, and 1.12 is outside every cell.
//      Applying 1.12 takes z0's seg0 DC residual from +1.0 to +3.1 C
//      (CALIBRATION) and +1.1 to +3.3 C (HOLD-OUT): a ~3x DEGRADATION
//      of settled accuracy over half this gate's own range.
//   3. The pooled dwell_offset[z] below averages BOTH segments into one
//      number, so a sign change between them is invisible to the bar.
//      z0's hold-out cell already PASSES at scale 1 (-1.412); with the
//      scalars it becomes +1.473, marginally FURTHER from zero, while
//      its seg0 cell goes -0.287 -> +1.759 (6x worse). z0 contributes
//      nothing to closing this gate; z1 and z2 close it.
//   4. Adopting for z1/z2 ONLY does not work either: measured at
//      (1.00, 1.19, 1.31), z0 stays at -2.104 on CALIBRATION and this
//      gate STILL FAILS. And the published scalars are hotter than every
//      one of the twelve cells here asks for (z1 wants 1.11-1.16, z2
//      wants 1.21-1.30), so they are not even the best scale-class fit.
//
// Re-deriving a correction from THESE captures instead is worse: it
// destroys the non-circularity posture described at the top of this file
// and turns a failing independent check into a passing tautology.
//
// The gate closes when the coupling matrix is RE-IDENTIFIED ON HARDWARE
// (docs/COUPLING_JOINT_IDENTIFICATION_CAPTURE.md's column-by-column
// procedure -- joint holds cannot see z0's row, since >80% of z0's rise
// is neighbour heat) and this gate passes with NOTHING applied on top.
// Until then it is meant to fail, and iter_tune steps 6-9 stay gated.

#include "../drivers/control/heater_output.h"
#include "../drivers/hw/max31856_codec.h"
#include "sim_plant.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NZ 3
#define MAX_TICKS 20000

// ---- G1 measured data + coupling cross-gain: ONE shared source now
// (sim_measured_zone_constants.h), not a per-file literal -- this file,
// sim_iter_tune.c and sim_wide_temp_sweep.c each used to hand-copy
// tuned_baseline_20260831.json's model_k_dc/model_tau_s/model_dead_time_s,
// which went stale against the live board and silently invalidated a
// downstream "structural infeasibility" conclusion; see
// docs/audits/cplval75_coupling_verdict_2026-09-10.md sec 4/D2/D3 and that
// header's own comment for the live values and provenance. ----
#include "sim_measured_zone_constants.h"
// G3: relay actuation lag is NOT bench-measured (same posture as
// sim_iter_tune.c / docs/audits/iter_tune_redesign_sim_2026-09-09.md) --
// 0.5 s is an assumed placeholder, stated here rather than hidden.
#define ASSUMED_RELAY_LAG_S 0.5f

// ---- Pass criteria (plan sec 6.5, verbatim) ----
#define BAR_RAMP_MAE_C        3.0f
#define BAR_DWELL_OFFSET_C    1.5f
#define BAR_DWELL_PEAK_C      2.0f
#define BAR_NOISE_SPREAD_MULT 2.0f
// Minimum number of ticks observed inside a dwell-entry window before that
// (zone, segment) cell is trusted to mean anything. dwell_entry_peak_sim/rec
// are initialised to 0 and only ever raised (see score_replay()), so a
// segment that is barely reached -- or a sim that never reaches target --
// registers peak=0 and would otherwise PASS vacuously (rec_peak <= bar
// becomes true precisely because nothing happened). Six ticks at the ~5.2s
// capture rate is ~30s of dwell-entry data; below that the cell is reported
// UNEVALUABLE, not PASS.
#define MIN_PEAK_SAMPLES 6

typedef struct {
    double t;
    float  target_c;
    bool   dwelling;
    int    segment_index;
    float  duty[NZ];
    float  actual_c[NZ];
    bool   actual_valid[NZ];
} tick_t;

// ---- minimal flat-JSON field extraction: every field this gate needs
// appears as a literal "\"key\":" substring with a unique meaning on each
// telemetry line (top-level keys appear once; per-zone keys "duty" and
// "actual_c" appear exactly NZ times, in zone order 0,1,2, which is the
// order this parser consumes them in). Not a general JSON parser -- do not
// reuse this for anything with nested arrays of arbitrary shape. ----
static const char *find_key(const char *from, const char *key)
{
    return strstr(from, key);
}

static bool parse_float_after(const char *p, const char *key, float *out)
{
    const char *k = find_key(p, key);
    if (!k) return false;
    const char *colon = strchr(k, ':');
    if (!colon) return false;
    *out = strtof(colon + 1, NULL);
    return true;
}

static bool parse_double_after(const char *p, const char *key, double *out)
{
    const char *k = find_key(p, key);
    if (!k) return false;
    const char *colon = strchr(k, ':');
    if (!colon) return false;
    *out = strtod(colon + 1, NULL);
    return true;
}

static bool parse_bool_after(const char *p, const char *key, bool *out)
{
    const char *k = find_key(p, key);
    if (!k) return false;
    const char *colon = strchr(k, ':');
    if (!colon) return false;
    colon++;
    while (isspace((unsigned char)*colon)) colon++;
    *out = (strncmp(colon, "true", 4) == 0);
    return true;
}

static bool parse_int_after(const char *p, const char *key, int *out)
{
    const char *k = find_key(p, key);
    if (!k) return false;
    const char *colon = strchr(k, ':');
    if (!colon) return false;
    *out = (int)strtol(colon + 1, NULL, 10);
    return true;
}

// Returns the position just after the colon following the Nth (0-based)
// occurrence of `key` at/after `from`, or NULL if there are fewer than
// N+1 occurrences. `key` must include the trailing quote but NOT the colon
// (e.g. "\"duty\"") so the colon that actually terminates the key name is
// found fresh each time, rather than assuming key already ends in ':'.
static const char *nth_key(const char *from, const char *key, int n)
{
    const char *p = from;
    for (int i = 0; i <= n; i++) {
        p = find_key(p, key);
        if (!p) return NULL;
        p += strlen(key);
    }
    const char *colon = strchr(p, ':');
    return colon ? colon + 1 : NULL;
}

static int load_capture(const char *path, tick_t *out, int max_ticks)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    static char line[262144];
    int n = 0;
    while (n < max_ticks && fgets(line, sizeof(line), f)) {
        tick_t tk;
        memset(&tk, 0, sizeof(tk));
        if (!parse_double_after(line, "\"t\":", &tk.t)) continue;
        if (!parse_float_after(line, "\"target_c\":", &tk.target_c)) continue;
        parse_bool_after(line, "\"dwelling\":", &tk.dwelling);
        parse_int_after(line, "\"segment_index\":", &tk.segment_index);
        bool ok = true;
        for (int z = 0; z < NZ; z++) {
            const char *dp = nth_key(line, "\"duty\"", z);
            const char *ap = nth_key(line, "\"actual_c\"", z);
            const char *vp = nth_key(line, "\"actual_valid\"", z);
            if (!dp || !ap || !vp) { ok = false; break; }
            tk.duty[z] = strtof(dp, NULL);
            tk.actual_c[z] = strtof(ap, NULL);
            while (isspace((unsigned char)*vp)) vp++;
            tk.actual_valid[z] = (strncmp(vp, "true", 4) == 0);
        }
        if (!ok) continue;
        out[n++] = tk;
    }
    fclose(f);
    return n;
}

// ---- G1 coupling: sim_kiln_step() now uses the ADDITIVE source-gain
// coupling model, the same model class the firmware's zone_coupling_solve.c
// solves (G = diag(model_k_dc) + coupling_coeff). In that model class the
// measured cross-gain g_coupling_coeff[i][j] IS the conductance sim_kiln
// wants, directly -- no fit needed. (The previous version of this file ran
// sim_kiln_coupling_fit_iterative() here to compensate for the retired
// temperature-difference exchange model's loading behaviour; that function
// no longer exists -- see docs/audits/sim_credibility_gate_real_cause_2026-09-10.md.) ----
static float g_fitted_coupling_w_per_c[SIM_KILN_MAX_ZONES][SIM_KILN_MAX_ZONES];
static bool  g_coupling_fitted = false;

static void ensure_coupling_fitted(void)
{
    if (g_coupling_fitted) return;
    for (int i = 0; i < NZ; i++) {
        for (int j = 0; j < NZ; j++) {
            g_fitted_coupling_w_per_c[i][j] = (i == j) ? 0.0f : g_coupling_coeff[i][j];
        }
    }
    g_coupling_fitted = true;
}

// ---- simulated replay: zero-order-hold recorded duty through the real
// heater_output.c window + G3 relay lag + G1 plant/coupling, 1 s substep,
// sampled back out at each capture tick. Fills sim_reading[n][NZ]. ----
// Optional opt-in three-node plant (2026-10-07 dwell-peak work). OFF by
// default so the gate's checked-in verdict is unchanged. Enabled by the
// optional trailing args `--three-node <bi> <phi> <sensor_bias_p>
// [sensor_tau_s [delay_scale [dc_comp]]]`. The (bi, phi, bias) triple is NOT identified from any
// capture -- see docs/audits/credibility_gate_dwell_peak_2026-10-07.md.
static struct { bool on; float bi, phi, bias_p, sensor_tau_s, delay_scale; bool dc_comp; } g_tn = {false, 0, 0, 0, 10.0f, 1.0f, false};

static void run_replay(const tick_t *ticks, int n, float sim_reading[][NZ])
{
    ensure_coupling_fitted();
    sim_kiln_cfg_t kcfg;
    memset(&kcfg, 0, sizeof(kcfg));
    kcfg.zone_count = NZ;

    // ambient = mean of the first tick with a valid reading per zone
    float ambient_c = 0.0f;
    int ambient_n = 0;
    for (int z = 0; z < NZ; z++) {
        for (int i = 0; i < n; i++) {
            if (ticks[i].actual_valid[z]) { ambient_c += ticks[i].actual_c[z]; ambient_n++; break; }
        }
    }
    ambient_c = (ambient_n > 0) ? (ambient_c / (float)ambient_n) : 24.0f;

    memcpy(kcfg.coupling_w_per_c, g_fitted_coupling_w_per_c, sizeof(kcfg.coupling_w_per_c));
    for (int z = 0; z < NZ; z++) {
        zone_cfg_t zcfg;
        memset(&zcfg, 0, sizeof(zcfg));
        zcfg.model_k_dc = g_k_dc[z];
        zcfg.model_tau_s = g_tau_s[z];
        zcfg.model_dead_time_s = g_dead_time_s[z];
        sim_plant_cfg_t pcfg;
        if (!sim_plant_from_zone_cfg(&zcfg, ambient_c, &pcfg)) {
            fprintf(stderr, "sim_plant_from_zone_cfg failed for zone %d\n", z);
            exit(2);
        }
        if (g_tn.on) {
            sim_plant_decompose_req_t rq = { g_k_dc[z], g_tau_s[z], g_tn.bi, g_tn.phi };
            if (!sim_plant_decompose_three_node(&rq, &pcfg)) {
                fprintf(stderr, "sim_plant_decompose_three_node failed for zone %d\n", z);
                exit(2);
            }
            pcfg.node_model = SIM_NODE_THREE;
            pcfg.c_s_j_per_c = 1.0f;
            pcfg.sensor_tau_s = g_tn.sensor_tau_s;
            pcfg.sensor_bias_p = g_tn.bias_p;
            pcfg.load_mass_mult = 1.0f;
            pcfg.sensor_delay_s *= g_tn.delay_scale;
            if (g_tn.dc_comp) {
                // Keep the SENSOR-observed DC gain equal to the measured k_dc /
                // coupling_coeff (both were measured at the thermocouple, not at
                // the element): E gain is k by construction of the decomposition,
                // the sensor sees rho_s of it. Scale own power and this zone's
                // coupling row by 1/rho_s. Derived from the topology, not fit.
                float gl = pcfg.g_el_w_per_c, gla = pcfg.g_la_w_per_c;
                float rho_s = pcfg.sensor_bias_p + (1.0f - pcfg.sensor_bias_p) * gl / (gl + gla);
                pcfg.heater_power_w /= rho_s;
                for (int j = 0; j < NZ; j++) kcfg.coupling_w_per_c[z][j] = g_fitted_coupling_w_per_c[z][j] / rho_s;
            }
        }
        if (getenv("KILN_GATE_SENSOR_LAG_S")) pcfg.sensor_lag_tau_s = strtof(getenv("KILN_GATE_SENSOR_LAG_S"), NULL); // diagnostic only
        kcfg.zone[z].plant = pcfg;
        kcfg.zone[z].radiative_coeff_w_per_k4 = 0.0f; // this profile stays well below cone temp
    }

    sim_kiln_state_t kstate;
    sim_kiln_reset(&kstate, &kcfg);
    for (int z = 0; z < NZ; z++) {
        kstate.zone[z].element_c = ambient_c;
        kstate.zone[z].sensor_c = ambient_c;
    }

    heater_output_state_t hstate[NZ];
    heater_output_cfg_t hcfg;
    memset(&hcfg, 0, sizeof(hcfg));
    hcfg.window_ms = 60000;
    hcfg.min_on_ms = 10000;
    hcfg.min_off_ms = 100;
    sim_relay_lag_t rlag[NZ];
    for (int z = 0; z < NZ; z++) {
        heater_output_reset(&hstate[z]);
        memset(&rlag[z], 0, sizeof(rlag[z]));
    }

    double sim_t = ticks[0].t;
    float last_duty[NZ];
    for (int z = 0; z < NZ; z++) last_duty[z] = ticks[0].duty[z];
    for (int z = 0; z < NZ; z++) sim_reading[0][z] = sim_max31856_quantize_tc(sim_kiln_reading_c(&kstate, &kcfg, z));

    for (int i = 1; i < n; i++) {
        double target_t = ticks[i].t;
        for (int z = 0; z < NZ; z++) last_duty[z] = ticks[i - 1].duty[z];
        while (sim_t < target_t - 0.001) {
            float step_s = 1.0f;
            if (target_t - sim_t < 1.0) step_s = (float)(target_t - sim_t);
            float sim_duty[NZ];
            for (int z = 0; z < NZ; z++) {
                bool relay_cmd = heater_output_duty(&hstate[z], &hcfg, last_duty[z], (uint32_t)(step_s * 1000.0f));
                bool relay_actual = sim_relay_lag_step(&rlag[z], relay_cmd, ASSUMED_RELAY_LAG_S, step_s);
                sim_duty[z] = relay_actual ? 1.0f : 0.0f;
            }
            sim_kiln_step(&kstate, &kcfg, sim_duty, step_s);
            sim_t += step_s;
        }
        for (int z = 0; z < NZ; z++) sim_reading[i][z] = sim_max31856_quantize_tc(sim_kiln_reading_c(&kstate, &kcfg, z));
    }
}

typedef struct {
    float ramp_mae[NZ];
    float dwell_offset[NZ];
    // per (zone, segment) dwell-entry overshoot peak, sim vs recorded
    float dwell_entry_peak_sim[NZ][8];
    float dwell_entry_peak_rec[NZ][8];
    int   dwell_entry_peak_n[NZ][8];  // ticks observed in the dwell-entry window
    int   seg_count;
} scores_t;

static void score_replay(const tick_t *ticks, int n, float sim_reading[][NZ], scores_t *s)
{
    memset(s, 0, sizeof(*s));
    double ramp_abs_sum[NZ] = {0}; int ramp_n[NZ] = {0};
    double dwell_sum[NZ] = {0}; int dwell_n[NZ] = {0};
    int max_seg = 0;
    bool prev_dwelling = false;
    int seg_dwell_start_idx[NZ][8];
    for (int z = 0; z < NZ; z++) for (int k = 0; k < 8; k++) seg_dwell_start_idx[z][k] = -1;

    for (int i = 0; i < n; i++) {
        int seg = ticks[i].segment_index;
        if (seg > max_seg) max_seg = seg;
        if (seg >= 8) continue;
        for (int z = 0; z < NZ; z++) {
            if (!ticks[i].actual_valid[z]) continue;
            float err = sim_reading[i][z] - ticks[i].actual_c[z];
            if (!ticks[i].dwelling) {
                ramp_abs_sum[z] += fabsf(err);
                ramp_n[z]++;
            } else {
                if (seg_dwell_start_idx[z][seg] < 0) seg_dwell_start_idx[z][seg] = i;
                // steady-state offset: skip first 60s (~12 ticks at 5s) of dwell entry
                if (i - seg_dwell_start_idx[z][seg] > 12) {
                    dwell_sum[z] += err;
                    dwell_n[z]++;
                }
                // dwell-entry overshoot peak (above target), first 10 min of dwell
                if ((i - seg_dwell_start_idx[z][seg]) * 5 < 600) {
                    float over_sim = sim_reading[i][z] - ticks[i].target_c;
                    float over_rec = ticks[i].actual_c[z] - ticks[i].target_c;
                    if (over_sim > s->dwell_entry_peak_sim[z][seg]) s->dwell_entry_peak_sim[z][seg] = over_sim;
                    if (over_rec > s->dwell_entry_peak_rec[z][seg]) s->dwell_entry_peak_rec[z][seg] = over_rec;
                    s->dwell_entry_peak_n[z][seg]++;
                }
            }
        }
        (void)prev_dwelling;
    }
    s->seg_count = max_seg + 1;
    for (int z = 0; z < NZ; z++) {
        s->ramp_mae[z] = (ramp_n[z] > 0) ? (float)(ramp_abs_sum[z] / ramp_n[z]) : NAN;
        s->dwell_offset[z] = (dwell_n[z] > 0) ? (float)(dwell_sum[z] / dwell_n[z]) : NAN;
    }
}

// Minimal noise_floor.json reader: pulls dwell_entry_overshoot_peak_c
// entries keyed "z<zone>:dwell_entry_overshoot_peak_c:<segment>" ->
// noise_floor_c, by scanning for each key literally. Same flat-substring
// approach as the capture parser -- adequate because these keys are unique
// strings, not because this is a real JSON parser.
static bool read_noise_floor(const char *path, int zone, int seg, float *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    fread(buf, 1, (size_t)sz, f);
    buf[sz] = 0;
    fclose(f);
    char key[128];
    snprintf(key, sizeof(key), "\"z%d:dwell_entry_overshoot_peak_c:%d\"", zone, seg);
    const char *k = strstr(buf, key);
    bool ok = false;
    if (k) {
        const char *nf = strstr(k, "\"noise_floor_c\":");
        // don't run past the next entry's opening quote
        const char *next_entry = strstr(k + strlen(key), "\": {");
        if (nf && (!next_entry || nf < next_entry + 4000)) {
            *out = strtof(nf + strlen("\"noise_floor_c\":"), NULL);
            ok = true;
        }
    }
    free(buf);
    return ok;
}

int main(int argc, char **argv)
{
    if (argc >= 8 && strcmp(argv[4], "--three-node") == 0) {
        g_tn.on = true;
        g_tn.bi = strtof(argv[5], NULL);
        g_tn.phi = strtof(argv[6], NULL);
        g_tn.bias_p = strtof(argv[7], NULL);
        if (argc >= 9) g_tn.sensor_tau_s = strtof(argv[8], NULL);
        if (argc >= 10) g_tn.delay_scale = strtof(argv[9], NULL);
        if (argc >= 11) g_tn.dc_comp = (atoi(argv[10]) != 0);
        argc = 4;
    }
    if (argc != 4) {
        fprintf(stderr, "usage: sim_credibility_gate <calibration.jsonl> <holdout.jsonl> <noise_floor.json>"
                        " [--three-node <bi> <phi> <sensor_bias_p> [sensor_tau_s [delay_scale [dc_comp]]]]\n");
        return 2;
    }
    const char *cal_path = argv[1];
    const char *hold_path = argv[2];
    const char *nf_path = argv[3];

    for (int i = 1; i <= 3; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) {
            printf("SKIP: sim_credibility_gate -- required input missing: %s\n"
                   "  Captures under logs/coupling/*.jsonl are gitignored/local-only\n"
                   "  (see .gitignore). This gate refuses to report a result without them\n"
                   "  rather than silently passing.\n", argv[i]);
            return 3;
        }
        fclose(f);
    }

    static tick_t cal_ticks[MAX_TICKS];
    static tick_t hold_ticks[MAX_TICKS];
    int cal_n = load_capture(cal_path, cal_ticks, MAX_TICKS);
    int hold_n = load_capture(hold_path, hold_ticks, MAX_TICKS);
    if (cal_n <= 0 || hold_n <= 0) {
        fprintf(stderr, "failed to parse captures (cal_n=%d hold_n=%d)\n", cal_n, hold_n);
        return 2;
    }

    static float cal_sim[MAX_TICKS][NZ];
    static float hold_sim[MAX_TICKS][NZ];
    run_replay(cal_ticks, cal_n, cal_sim);
    run_replay(hold_ticks, hold_n, hold_sim);

    if (getenv("KILN_GATE_DUMP")) { // diagnostic only: z0..2 duty/rec/sim around first dwell entry, calibration capture
        int d0 = -1;
        for (int i = 0; i < cal_n; i++) if (cal_ticks[i].dwelling) { d0 = i; break; }
        for (int i = (d0 > 40 ? d0 - 40 : 0); i < cal_n && i < d0 + 130; i += 5)
            printf("DUMP i=%d dw=%d tgt=%.1f | duty %.2f %.2f %.2f | rec %.2f %.2f %.2f | sim %.2f %.2f %.2f\n", i, cal_ticks[i].dwelling, cal_ticks[i].target_c,
                   cal_ticks[i].duty[0], cal_ticks[i].duty[1], cal_ticks[i].duty[2],
                   cal_ticks[i].actual_c[0], cal_ticks[i].actual_c[1], cal_ticks[i].actual_c[2],
                   cal_sim[i][0], cal_sim[i][1], cal_sim[i][2]);
    }
    scores_t cal_s, hold_s;
    score_replay(cal_ticks, cal_n, cal_sim, &cal_s);
    score_replay(hold_ticks, hold_n, hold_sim, &hold_s);

    printf("=== sim_credibility_gate (ITER_TUNE_REDESIGN.md sec 6.5) ===\n");
    printf("calibration: %s (%d ticks)\n", cal_path, cal_n);
    printf("hold-out:    %s (%d ticks)\n", hold_path, hold_n);
    printf("model: fixed, checked-in G1 params. Coupling is the ADDITIVE\n"
           "  source-gain model (sim_kiln_step(): coupling_w_per_c[i][j] * duty[j]),\n"
           "  matching the firmware's zone_coupling_solve.c model class -- the\n"
           "  measured cross-gain matrix is used directly, no fit needed (see\n"
           "  docs/audits/sim_credibility_gate_real_cause_2026-09-10.md).\n"
           "  Relay lag (G3) is the same UNMEASURED 0.5s placeholder\n"
           "  sim_iter_tune.c uses, not a bench-measured value.\n\n");
    printf("Structural ceiling of the literal sec 6.2 own-duty mapping\n"
           "  (h=loss_coeff=1.0 is a provably free/inert scale here -- see\n"
           "  sim_plant_from_zone_cfg()'s header comment; rescaling h together\n"
           "  with heater_power_w/thermal_mass leaves the ODE, and therefore this\n"
           "  ceiling, unchanged, so there is no alternative h that raises it):\n"
           "  a zone driven at duty=1.0 forever, with NO coupling contribution,\n"
           "  approaches ambient + model_k_dc and can never exceed it:\n");
    {
        float ambient_c = 0.0f; int ambient_n = 0;
        for (int z = 0; z < NZ; z++)
            for (int i = 0; i < cal_n; i++)
                if (cal_ticks[i].actual_valid[z]) { ambient_c += cal_ticks[i].actual_c[z]; ambient_n++; break; }
        ambient_c = (ambient_n > 0) ? ambient_c / (float)ambient_n : 24.0f;
        for (int z = 0; z < NZ; z++)
            printf("    z%d: ambient(%.1f) + model_k_dc(%.2f) = %.2fC ceiling, own-duty-only\n",
                   z, ambient_c, g_k_dc[z], ambient_c + g_k_dc[z]);
    }
    printf("  Coupling from other zones adds its own independent power term (the\n"
           "  additive model, unlike the retired temperature-difference exchange\n"
           "  model, has no feasibility ceiling coupled to model_k_dc[j] -- see\n"
           "  the retraction in sec 3 of the audit doc above); a zone's total rise\n"
           "  is its own ceiling PLUS whatever every other zone's duty adds.\n\n");
    printf("Coupling matrix used this run (measured cross-gain, used directly as\n"
           "  coupling_w_per_c -- no fit or feasibility bound in this model class):\n");
    for (int i = 0; i < NZ; i++) {
        for (int j = 0; j < NZ; j++) {
            if (i == j) continue;
            printf("    coupling_coeff[%d][%d] (measured)=%.2fC  ->  coupling_w_per_c[%d][%d] (used)=%.5f W\n",
                   i, j, g_coupling_coeff[i][j], i, j, g_fitted_coupling_w_per_c[i][j]);
        }
    }
    printf("\n");

    bool pass = true;
    int ramp_pass = 0, ramp_fail = 0;
    int dwell_pass = 0, dwell_fail = 0;
    int peak_pass = 0, peak_fail = 0, peak_uneval = 0;
    int noise_pass = 0, noise_fail = 0, noise_uneval = 0;
    const char *labels[2] = {"CALIBRATION", "HOLD-OUT"};
    scores_t *sc[2] = {&cal_s, &hold_s};
    for (int r = 0; r < 2; r++) {
        printf("-- %s --\n", labels[r]);
        for (int z = 0; z < NZ; z++) {
            bool ramp_ok = !isnan(sc[r]->ramp_mae[z]) && sc[r]->ramp_mae[z] <= BAR_RAMP_MAE_C;
            bool dwell_ok = !isnan(sc[r]->dwell_offset[z]) && fabsf(sc[r]->dwell_offset[z]) <= BAR_DWELL_OFFSET_C;
            printf("  z%d: ramp MAE=%.3fC (bar %.1f) %s | dwell offset=%.3fC (bar +/-%.1f) %s\n",
                   z, sc[r]->ramp_mae[z], BAR_RAMP_MAE_C, ramp_ok ? "PASS" : "FAIL",
                   sc[r]->dwell_offset[z], BAR_DWELL_OFFSET_C, dwell_ok ? "PASS" : "FAIL");
            if (ramp_ok) ramp_pass++; else ramp_fail++;
            if (dwell_ok) dwell_pass++; else dwell_fail++;
            if (!ramp_ok || !dwell_ok) pass = false;
            for (int seg = 0; seg < sc[r]->seg_count && seg < 8; seg++) {
                int nsamp = sc[r]->dwell_entry_peak_n[z][seg];
                if (nsamp < MIN_PEAK_SAMPLES) {
                    printf("      seg%d dwell-entry overshoot peak: sim=%.3fC rec=%.3fC (n=%d ticks, need >=%d) UNEVALUABLE\n",
                           seg, sc[r]->dwell_entry_peak_sim[z][seg], sc[r]->dwell_entry_peak_rec[z][seg],
                           nsamp, MIN_PEAK_SAMPLES);
                    peak_uneval++;
                    continue; // not counted toward pass or fail -- too few samples to mean anything
                }
                float diff = fabsf(sc[r]->dwell_entry_peak_sim[z][seg] - sc[r]->dwell_entry_peak_rec[z][seg]);
                bool peak_ok = diff <= BAR_DWELL_PEAK_C;
                printf("      seg%d dwell-entry overshoot peak: sim=%.3fC rec=%.3fC diff=%.3fC (bar %.1f, n=%d) %s\n",
                       seg, sc[r]->dwell_entry_peak_sim[z][seg], sc[r]->dwell_entry_peak_rec[z][seg], diff,
                       BAR_DWELL_PEAK_C, nsamp, peak_ok ? "PASS" : "FAIL");
                if (peak_ok) peak_pass++; else peak_fail++;
                if (!peak_ok) pass = false;
            }
        }
    }

    printf("\n-- noise-floor spread check (calibration vs hold-out, both scored\n"
           "   against the same fixed model, spread vs 2x noise_floor.json) --\n");
    for (int z = 0; z < NZ; z++) {
        for (int seg = 0; seg < cal_s.seg_count && seg < 8 && seg < hold_s.seg_count; seg++) {
            float nfc;
            if (!read_noise_floor(nf_path, z, seg, &nfc)) continue;
            if (cal_s.dwell_entry_peak_n[z][seg] < MIN_PEAK_SAMPLES ||
                hold_s.dwell_entry_peak_n[z][seg] < MIN_PEAK_SAMPLES) {
                printf("  z%d seg%d: UNEVALUABLE (cal n=%d, hold n=%d, need >=%d each)\n",
                       z, seg, cal_s.dwell_entry_peak_n[z][seg], hold_s.dwell_entry_peak_n[z][seg],
                       MIN_PEAK_SAMPLES);
                noise_uneval++;
                continue;
            }
            float spread = fabsf(cal_s.dwell_entry_peak_sim[z][seg] - hold_s.dwell_entry_peak_sim[z][seg]);
            bool optimistic = spread <= nfc; // model spread smaller than or equal to real spread: tolerated
            bool within_2x = spread <= BAR_NOISE_SPREAD_MULT * nfc;
            printf("  z%d seg%d: sim spread=%.3fC  real noise_floor=%.3fC  2x=%.3fC  %s\n",
                   z, seg, spread, nfc, 2.0f * nfc,
                   optimistic ? "OPTIMISTIC (tolerated, sec 3.1)" : (within_2x ? "PESSIMISTIC but within 2x (PASS)" : "PESSIMISTIC, EXCEEDS 2x (FAIL)"));
            if (!optimistic && !within_2x) { noise_fail++; pass = false; } else { noise_pass++; }
        }
    }

    printf("\n-- honest per-bar tally (this run) --\n");
    printf("  ramp MAE:          %d pass, %d fail\n", ramp_pass, ramp_fail);
    printf("  dwell offset:      %d pass, %d fail\n", dwell_pass, dwell_fail);
    printf("  dwell-entry peak:  %d pass, %d fail, %d unevaluable\n", peak_pass, peak_fail, peak_uneval);
    printf("  noise-floor spread:%d pass, %d fail, %d unevaluable\n", noise_pass, noise_fail, noise_uneval);

    printf("\n=== RESULT: %s ===\n", pass ? "GATE PASSES" : "GATE FAILS");
    if (!pass) {
        printf("Per plan sec 6.5: the plan stops here and reports. Results from\n"
               "sim_iter_tune.c / sim_wide_temp_sweep.c should NOT be treated as\n"
               "evidence about the real kiln until this gate is closed. This is not\n"
               "a single known-open bar -- see the per-bar tally above for which\n"
               "bars and how many cells actually failed this run.\n");
    }
    return pass ? 0 : 1;
}
