// sim_credibility_gate_closedloop -- candidate 3 from
// docs/audits/sim_credibility_gate_real_cause_2026-09-10.md sec 6/8: does
// replaying the recorded SETPOINT through the REAL controller (instead of
// replaying the recorded DUTY open-loop, as sim_credibility_gate.c does)
// close the dwell-entry overshoot peak gap the open-loop gate leaves open?
//
// WHAT THIS LINKS (all real production .c, no mirror): pid.c
// (pid_update_terms/pid_seed_bumpless), heater_output.c (G2, the 60s PWM
// window), zone_coupling_solve.c (the coupled hold/climb solve -- pure
// functions, see its own header comment on why it takes scalars/a small
// neighbor array rather than the private zone_runtime_t), and
// profile_executor_feedforward.c (zone_feedforward()/zone_taper_climb_rate(),
// the terminal ease-off that is the actual mechanism under test). This file
// supplies its OWN `s_exec` global and zones_config_get_coupling*()/
// zones_config_get_ease_off_window_mult() fakes, the SAME "own executable,
// tiny fake accessors" convention test_zone_coupling_solve.c already uses
// for this exact module. Linking profile_executor.c itself (and therefore
// pid_family_zone_tick()) was deliberately avoided: that drags in
// FreeRTOS-stubbed executor machinery this diagnostic does not need, and
// every function actually linked here is a pure, non-static, directly
// callable production entry point already designed for host-side testing
// without it.
//
// WHAT THIS COSTS (read before trusting a single number this prints):
//
// 1. NOT non-circular the way sim_credibility_gate.c is. That gate replays
//    the recorded DUTY -- a real physical fact, uncontaminated by any
//    model of the plant -- so a mismatch is attributable to the plant
//    model alone. This harness instead recomputes duty from the recorded
//    SETPOINT using the SAME model of the plant (ff_k_dc/ff_tau_s/
//    ff_dead_time_s = g_k_dc/g_tau_s/g_dead_time_s, the values sim_kiln
//    itself uses) that the real feedforward carries. A mismatch here can
//    come from the PLANT model, the CONTROLLER's gains, or the
//    CONTROLLER's own model of the plant -- three confounded sources, not
//    one. Recorded actual_c stays the answer key for SCORING only (never
//    fed into anything upstream of the score), but the mechanism itself is
//    no longer plant-only. This is why every result below is printed as a
//    bounded diagnostic, never as a pass/fail gate bar.
//
// 2. GAINS PROVENANCE IS NOT ESTABLISHED, and this is stated plainly rather
//    than papered over. The two captures recorded no kp/ki/kd -- only
//    target_c/dwelling/duty/actual_c (sim_credibility_gate.c's own parser
//    reads the same lines and finds none either). Two candidate sources
//    were checked this session and BOTH are demonstrably unreliable for
//    the captures' actual timestamp (2026-09-02 19:15):
//      - tools/PcTools/config_presets/tuned_baseline_20260831.json (44h
//        before the capture): z1/z2 control_mode reads 0 (OFF) even though
//        both captures ran control_mode 2 throughout, and its model_tau_s
//        (166.9/129.1/114.8s) is the SAME stale-preset artifact
//        sim_measured_zone_constants.h's own header comment documents as
//        wrong by ~90-100s per zone against the live board (263.8/269.8/
//        270.9s) -- a known offender in this repo's "stale preset" bug
//        class, not a fresh source.
//      - control_get_zones() (live board, 2026-09-10, read-only): every
//        zone reports control_mode 3 (PID_FUZZY), not the captures'
//        plain PID (mode 2) -- a different control ALGORITHM, not just
//        different gains -- and Kp alone has moved from the preset's
//        0.0318/0.0361/0.0355 to 0.0318/0.0485/0.0631 (z1 +34%, z2 +78%).
//    Neither endpoint can be the captures' true gains and there is no
//    logged middle point, so this harness runs BOTH as a sensitivity
//    bracket (PRESET, LIVE) and draws no conclusion that depends on
//    picking one over the other. model_k_dc/tau_s/dead_time_s and the
//    coupling matrix are NOT drawn from either uncertain source -- they
//    come from sim_measured_zone_constants.h (matches live GET /api/zones
//    this session per that header's own provenance comment) and are held
//    fixed across both brackets so only kp/ki/kd differ between runs.
//
// 3. Fields added to zone_runtime_t/zones_cfg AFTER the 2026-09-02 capture
//    (approach_rate_cap_c_per_hr, per-zone ease_off_window_mult, the
//    coupled-solve diagonal-source flag) are left at their documented
//    ZERO-SENTINEL defaults here -- every one of those commits (af07e455,
//    d800a601, ef323b11) states its default reproduces the behaviour from
//    before the field existed, bit-for-bit. Verified by reading each
//    commit message this pass, not assumed.
//
// 4. Control-decision cadence is coarser than the open-loop gate's: duty is
//    recomputed once per CAPTURE tick (~5.2s, assumed to be the real
//    control loop's own period -- the captures carry no other signal to
//    derive it from), then held constant while the plant/relay-lag ODE is
//    still integrated at a 1s substep for numerical accuracy (matching
//    sim_credibility_gate.c's own substep). heater_output_duty() is
//    therefore called once per ~5.2s tick rather than once per 1s substep;
//    within a 60s PWM window this loses timing resolution but not PWM
//    density, since it still accumulates the correct on/off time.
//
// Exit codes: 0 ran and printed both brackets, 2 usage/parse error, 3 SKIP
// (captures missing, same convention as sim_credibility_gate.c). There is
// no pass/fail exit code -- see point 1 above.

#include "../drivers/control/heater_output.h"
#include "../drivers/control/pid.h"
#include "../drivers/control/zone_coupling_solve.h"
#include "../drivers/hw/max31856_codec.h"
#include "sim_plant.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NZ 3
#define MAX_TICKS 20000
#define ASSUMED_RELAY_LAG_S 0.5f

#include "sim_measured_zone_constants.h"

#define BAR_RAMP_MAE_C        3.0f
#define BAR_DWELL_OFFSET_C    1.5f
#define BAR_DWELL_PEAK_C      2.0f

// ---- minimal zone_runtime_t stand-in -----------------------------------
// profile_executor_feedforward.c's zone_feedforward()/zone_taper_climb_rate()
// take a `zone_runtime_t *`, a struct declared in profile_executor_internal.h
// (private to the profile_executor.c split -- see that header's own doc
// comment). Rather than #include that header (which pulls in FreeRTOS/
// heater_output.h/thermal_guard.h/autotune_engine.h/on_off_trigger_decide.h
// transitively, entangling this file with executor machinery the top-of-file
// comment says to avoid), this defines its own struct with EXACTLY the
// field names/types those two functions and zone_coupling_qualifies_as_
// neighbor() read -- verified against profile_executor_internal.h's real
// field list during this pass. This is a structural risk stated rather than
// hidden: if that header ever reorders/renames these fields, this struct
// silently stops matching (the two are never in the same translation unit,
// so the compiler cannot catch the drift). Fields kept in the same order as
// the real struct for easier future diffing; every field the linked
// functions do not touch is simply omitted.
typedef struct {
    bool active;
    bool ff_hold_used_matrix;
    bool ff_hold_infeasible;
    uint8_t ff_hold_reason;
    bool ff_climb_used_matrix;
    bool ff_climb_infeasible;
    uint8_t ff_climb_reason;
    bool ff_membership_changed;
    uint32_t ff_membership_change_count;
    float ff_k_dc;
    float ff_tau_s;
    float ff_dead_time_s;
    bool ff_enabled;
    float duty;
    float actual_c;
    bool actual_valid;
    bool faulted;
    bool heat_blocked;
    zone_control_mode_t control_mode;
    pid_cfg_t pid_cfg;
    pid_state_t pid_state;
} zone_runtime_t;

// ---- s_exec stand-in ----------------------------------------------------
// zone_feedforward()/zone_taper_climb_rate() read `s_exec.ambient_c`/
// `s_exec.target_rate_c_per_s`/`s_exec.dwelling`/`s_exec.zones[]` via an
// extern declared in profile_executor_internal.h and defined non-static in
// profile_executor.c. This file provides that same global directly, sized
// to what those two functions actually touch (MAX31856_CHANNEL_COUNT == 3
// == NZ on this board, checked against uart_task_ids.h's THERMO_CHANNEL_COUNT
// this pass) -- linking profile_executor.c itself is exactly what this file
// avoids (see top comment).
typedef struct {
    zone_runtime_t zones[NZ];
    bool dwelling;
    uint8_t segment_index;
    struct { float target_c; } segments[8];
    float target_c;
    float target_rate_c_per_s;
    float ambient_c;
} s_exec_state_t;

s_exec_state_t s_exec;

// ---- fakes for the zones_config_* accessors zone_coupling_solve.c /
// profile_executor_feedforward.c call (same convention as
// test_zone_coupling_solve.c's own fakes for this same module) ----
static float s_fake_coupling_row[NZ][NZ];

bool zones_config_get_coupling(uint8_t zone_index, float out_row[MAX31856_CHANNEL_COUNT])
{
    if (zone_index >= NZ) return false;
    memset(out_row, 0, sizeof(float) * MAX31856_CHANNEL_COUNT);
    for (int j = 0; j < NZ; j++) out_row[j] = s_fake_coupling_row[zone_index][j];
    return true;
}

bool zones_config_get_coupling_diag_k_dc(uint8_t zone_index, float *out_k_dc)
{
    (void)zone_index; (void)out_k_dc;
    return false; // "never identified on hardware" per control_get_zones() this session -- matches live board
}

// zone_model_at()/coupling_at() (5d3bc854's schedule seam): only reachable
// here via zone_load_model() (never called by this file -- ff_k_dc/tau_s/
// dead_time_s are set directly from g_k_dc/g_tau_s/g_dead_time_s below) and
// zone_feedforward()'s own coupling_row lookup respectively. zone_model_at()
// is therefore dead code in this harness and just needs a definition to
// link; coupling_at() DOES run every tick (T_c unused, bit-identical
// passthrough per that seam's own doc comment) so it forwards to the same
// fake zones_config_get_coupling() above, same convention
// test_profile_executor_prestart.c's own stub uses.
bool zone_model_at(uint8_t zone_index, float T_c, float *out_k_dc, float *out_tau_s, float *out_dead_time_s)
{
    (void)zone_index; (void)T_c; (void)out_k_dc; (void)out_tau_s; (void)out_dead_time_s;
    return false;
}
bool coupling_at(uint8_t zone_index, float T_c, float out_row[MAX31856_CHANNEL_COUNT])
{
    (void)T_c;
    return zones_config_get_coupling(zone_index, out_row);
}

const char *PE_TAG = "sim_credibility_gate_closedloop";

bool zones_config_get_ease_off_window_mult(uint8_t zone_index, float *out_mult)
{
    (void)zone_index;
    *out_mult = 2.0f; // live board reports 2.0 on every zone today; af07e455 (landed AFTER the captures)
                       // made 2.0 the sentinel default matching the #define it replaced -- bit-identical
                       // to captures-time behaviour either way
    return false; // false = "using the sentinel default", the documented no-override contract
}

// declared non-static in profile_executor_feedforward.c; called directly
extern float zone_feedforward(const zone_runtime_t *z, uint8_t zi, float setpoint_c, float rate_c_per_s,
                              float *out_hold);
extern float zone_taper_climb_rate(const zone_runtime_t *z, uint8_t zi, float target_c, float rate_c_per_s,
                                   float segment_target_c);

#define PID_D_FILTER_TAU_S 30.0f
#define PID_SETPOINT_WEIGHT_B 1.0f
#define PID_FUNCTIONAL_RANGE_C 25.0f

// ---- capture parsing -- identical minimal flat-JSON approach to
// sim_credibility_gate.c (duplicated rather than shared: this is measurement
// scaffolding, not the production control code the top comment's "no
// mirror" promise is about). ----
typedef struct {
    double t;
    float  target_c;
    bool   dwelling;
    int    segment_index;
    float  duty[NZ];
    float  actual_c[NZ];
    bool   actual_valid[NZ];
} tick_t;

static bool parse_float_after(const char *p, const char *key, float *out)
{
    const char *k = strstr(p, key);
    if (!k) return false;
    const char *colon = strchr(k, ':');
    if (!colon) return false;
    *out = strtof(colon + 1, NULL);
    return true;
}
static bool parse_double_after(const char *p, const char *key, double *out)
{
    const char *k = strstr(p, key);
    if (!k) return false;
    const char *colon = strchr(k, ':');
    if (!colon) return false;
    *out = strtod(colon + 1, NULL);
    return true;
}
static bool parse_bool_after(const char *p, const char *key, bool *out)
{
    const char *k = strstr(p, key);
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
    const char *k = strstr(p, key);
    if (!k) return false;
    const char *colon = strchr(k, ':');
    if (!colon) return false;
    *out = (int)strtol(colon + 1, NULL, 10);
    return true;
}
static const char *nth_key(const char *from, const char *key, int n)
{
    const char *p = from;
    for (int i = 0; i <= n; i++) {
        p = strstr(p, key);
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

typedef struct {
    float ramp_mae[NZ];
    float dwell_offset[NZ];
    float dwell_entry_peak_sim[NZ][8];
    float dwell_entry_peak_rec[NZ][8];
    int   seg_count;
} scores_t;

static void score_replay(const tick_t *ticks, int n, float sim_reading[][NZ], scores_t *s)
{
    memset(s, 0, sizeof(*s));
    double ramp_abs_sum[NZ] = {0}; int ramp_n[NZ] = {0};
    double dwell_sum[NZ] = {0}; int dwell_n[NZ] = {0};
    int max_seg = 0;
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
                if (i - seg_dwell_start_idx[z][seg] > 12) {
                    dwell_sum[z] += err;
                    dwell_n[z]++;
                }
                if ((i - seg_dwell_start_idx[z][seg]) * 5 < 600) {
                    float over_sim = sim_reading[i][z] - ticks[i].target_c;
                    float over_rec = ticks[i].actual_c[z] - ticks[i].target_c;
                    if (over_sim > s->dwell_entry_peak_sim[z][seg]) s->dwell_entry_peak_sim[z][seg] = over_sim;
                    if (over_rec > s->dwell_entry_peak_rec[z][seg]) s->dwell_entry_peak_rec[z][seg] = over_rec;
                }
            }
        }
    }
    s->seg_count = max_seg + 1;
    for (int z = 0; z < NZ; z++) {
        s->ramp_mae[z] = (ramp_n[z] > 0) ? (float)(ramp_abs_sum[z] / ramp_n[z]) : NAN;
        s->dwell_offset[z] = (dwell_n[z] > 0) ? (float)(dwell_sum[z] / dwell_n[z]) : NAN;
    }
}

// Precompute each segment's own final target_c (needed by zone_taper_climb_
// rate()'s segment_target_c argument) as the LAST recorded target_c seen at
// that segment_index -- self-consistent because target_c stops moving once
// dwelling starts for that segment.
static void compute_segment_targets(const tick_t *ticks, int n, float seg_target[8])
{
    for (int k = 0; k < 8; k++) seg_target[k] = 0.0f;
    for (int i = 0; i < n; i++) {
        int seg = ticks[i].segment_index;
        if (seg >= 0 && seg < 8) seg_target[seg] = ticks[i].target_c;
    }
}

typedef struct {
    float kp[NZ], ki[NZ], kd[NZ];
    const char *label;
} gain_set_t;

// Sensitivity bracket -- see file header point 2. Neither is known to be
// the captures' true 2026-09-02 19:15 gains.
static const gain_set_t GAIN_PRESET = {
    .kp = {0.0318f, 0.0361f, 0.0355f},
    .ki = {0.0002f, 0.0003f, 0.0003f},
    .kd = {0.6526f, 0.6874f, 0.6598f},
    .label = "PRESET (tuned_baseline_20260831.json, 44h before capture -- known stale on tau_s/control_mode)",
};
static const gain_set_t GAIN_LIVE = {
    .kp = {0.0318f, 0.0485f, 0.0631f},
    .ki = {0.0001f, 0.0002f, 0.0002f},
    .kd = {0.8401f, 1.0548f, 1.0690f},
    .label = "LIVE (control_get_zones(), 2026-09-10 -- different control_mode (fuzzy) than captures (plain PID))",
};

static void run_closedloop_replay(const tick_t *ticks, int n, const gain_set_t *gains, float sim_reading[][NZ])
{
    memset(&s_exec, 0, sizeof(s_exec));

    float ambient_c = 0.0f; int ambient_n = 0;
    for (int z = 0; z < NZ; z++)
        for (int i = 0; i < n; i++)
            if (ticks[i].actual_valid[z]) { ambient_c += ticks[i].actual_c[z]; ambient_n++; break; }
    ambient_c = (ambient_n > 0) ? (ambient_c / (float)ambient_n) : 24.0f;
    s_exec.ambient_c = ambient_c;

    for (int z = 0; z < NZ; z++)
        for (int j = 0; j < NZ; j++)
            s_fake_coupling_row[z][j] = (z == j) ? 0.0f : g_coupling_coeff[z][j];

    float seg_target[8];
    compute_segment_targets(ticks, n, seg_target);

    sim_kiln_cfg_t kcfg;
    memset(&kcfg, 0, sizeof(kcfg));
    kcfg.zone_count = NZ;
    for (int z = 0; z < NZ; z++) {
        zone_cfg_t zcfg;
        memset(&zcfg, 0, sizeof(zcfg));
        zcfg.model_k_dc = g_k_dc[z];
        zcfg.model_tau_s = g_tau_s[z];
        zcfg.model_dead_time_s = g_dead_time_s[z];
        sim_plant_cfg_t pcfg;
        if (!sim_plant_from_zone_cfg(&zcfg, ambient_c, &pcfg)) { fprintf(stderr, "sim_plant_from_zone_cfg failed\n"); exit(2); }
        kcfg.zone[z].plant = pcfg;
        kcfg.zone[z].radiative_coeff_w_per_k4 = 0.0f;
    }
    memcpy(kcfg.coupling_w_per_c, s_fake_coupling_row, sizeof(kcfg.coupling_w_per_c));

    sim_kiln_state_t kstate;
    sim_kiln_reset(&kstate, &kcfg);

    heater_output_cfg_t hcfg;
    memset(&hcfg, 0, sizeof(hcfg));
    hcfg.window_ms = 60000;
    hcfg.min_on_ms = 10000;
    hcfg.min_off_ms = 100;
    heater_output_state_t hstate[NZ];
    sim_relay_lag_t rlag[NZ];

    for (int z = 0; z < NZ; z++) {
        kstate.zone[z].element_c = ambient_c;
        kstate.zone[z].sensor_c = ambient_c;
        heater_output_reset(&hstate[z]);
        memset(&rlag[z], 0, sizeof(rlag[z]));

        s_exec.zones[z].active = true;
        s_exec.zones[z].ff_enabled = true;
        s_exec.zones[z].ff_k_dc = g_k_dc[z];
        s_exec.zones[z].ff_tau_s = g_tau_s[z];
        s_exec.zones[z].ff_dead_time_s = g_dead_time_s[z];
        s_exec.zones[z].control_mode = ZONE_CONTROL_MODE_PID;
        s_exec.zones[z].actual_valid = true;
        s_exec.zones[z].actual_c = ambient_c;
        s_exec.zones[z].pid_cfg.kp = gains->kp[z];
        s_exec.zones[z].pid_cfg.ki = gains->ki[z];
        s_exec.zones[z].pid_cfg.kd = gains->kd[z];
        s_exec.zones[z].pid_cfg.d_filter_tau_s = PID_D_FILTER_TAU_S;
        s_exec.zones[z].pid_cfg.b = PID_SETPOINT_WEIGHT_B;
        s_exec.zones[z].pid_cfg.pid_range_c = PID_FUNCTIONAL_RANGE_C;
        pid_reset(&s_exec.zones[z].pid_state);
    }

    double sim_t = ticks[0].t;
    for (int z = 0; z < NZ; z++)
        sim_reading[0][z] = sim_max31856_quantize_tc(sim_kiln_reading_c(&kstate, &kcfg, z));

    for (int i = 1; i < n; i++) {
        double dt_d = ticks[i].t - ticks[i - 1].t;
        float dt_s = (dt_d > 0.1 && dt_d < 30.0) ? (float)dt_d : 5.2f;
        uint32_t dt_ms = (uint32_t)(dt_s * 1000.0f);

        int seg = ticks[i].segment_index;
        if (seg < 0) seg = 0;
        if (seg >= 8) seg = 7;
        s_exec.dwelling = ticks[i].dwelling;
        s_exec.segment_index = (uint8_t)seg;
        s_exec.target_c = ticks[i].target_c;
        s_exec.target_rate_c_per_s = ticks[i].dwelling ? 0.0f
            : (float)((ticks[i].target_c - ticks[i - 1].target_c) / dt_s);

        // Stage 1: every zone's actual_c/actual_valid updated FIRST (matches
        // profile_executor.c's per-tick order: the sensing loop populates
        // s_exec.zones[].actual_c for every zone before the control loop
        // runs, so build_coupling_neighbor_array() -- called from inside
        // zone_feedforward() below -- always sees THIS tick's readings for
        // every zone, not a stale one for zones not yet processed).
        for (int z = 0; z < NZ; z++)
            s_exec.zones[z].actual_c = sim_max31856_quantize_tc(sim_kiln_reading_c(&kstate, &kcfg, z));

        // Stage 2: real feedforward + real PID per zone.
        bool relay_cmd[NZ];
        for (int z = 0; z < NZ; z++) {
            float ff_rate = s_exec.target_rate_c_per_s;
            if (!s_exec.dwelling && ff_rate != 0.0f) {
                ff_rate = zone_taper_climb_rate(&s_exec.zones[z], (uint8_t)z, s_exec.target_c, ff_rate,
                                                seg_target[seg]);
            }
            float ff_hold = 0.0f;
            float u_ff = zone_feedforward(&s_exec.zones[z], (uint8_t)z, s_exec.target_c, ff_rate, &ff_hold);
            if (s_exec.zones[z].ff_membership_changed) {
                pid_seed_bumpless(&s_exec.zones[z].pid_state, &s_exec.zones[z].pid_cfg, s_exec.target_c,
                                  s_exec.zones[z].actual_c, s_exec.zones[z].duty, u_ff, ff_hold);
            }
            float duty = pid_update_terms(&s_exec.zones[z].pid_state, &s_exec.zones[z].pid_cfg, s_exec.target_c,
                                          s_exec.zones[z].actual_c, dt_s, u_ff, ff_hold, NULL);
            s_exec.zones[z].duty = duty;
            relay_cmd[z] = heater_output_duty(&hstate[z], &hcfg, duty, dt_ms);
        }

        // Stage 3: plant/relay-lag ODE at a 1s substep for numerical
        // accuracy, holding this tick's relay decision constant across it
        // (same substep discipline as sim_credibility_gate.c's run_replay(),
        // just driven by a freshly computed decision instead of a ZOH'd
        // recorded one).
        double target_t = ticks[i].t;
        while (sim_t < target_t - 0.001) {
            float step_s = 1.0f;
            if (target_t - sim_t < 1.0) step_s = (float)(target_t - sim_t);
            float sim_duty[NZ];
            for (int z = 0; z < NZ; z++) {
                bool relay_actual = sim_relay_lag_step(&rlag[z], relay_cmd[z], ASSUMED_RELAY_LAG_S, step_s);
                sim_duty[z] = relay_actual ? 1.0f : 0.0f;
            }
            sim_kiln_step(&kstate, &kcfg, sim_duty, step_s);
            sim_t += step_s;
        }
        for (int z = 0; z < NZ; z++)
            sim_reading[i][z] = sim_max31856_quantize_tc(sim_kiln_reading_c(&kstate, &kcfg, z));
    }
}

static void run_and_print(const char *label, const tick_t *ticks, int n, const gain_set_t *gains)
{
    static float sim_reading[MAX_TICKS][NZ];
    run_closedloop_replay(ticks, n, gains, sim_reading);
    scores_t sc;
    score_replay(ticks, n, sim_reading, &sc);
    printf("-- %s / gains=%s --\n", label, gains->label);
    for (int z = 0; z < NZ; z++) {
        printf("  z%d: ramp MAE=%.3fC (bar %.1f) | dwell offset=%.3fC (bar +/-%.1f)\n",
               z, sc.ramp_mae[z], BAR_RAMP_MAE_C, sc.dwell_offset[z], BAR_DWELL_OFFSET_C);
        for (int seg = 0; seg < sc.seg_count && seg < 8; seg++) {
            printf("      seg%d dwell-entry overshoot peak: sim=%.3fC rec=%.3fC diff=%.3fC (bar %.1f)\n",
                   seg, sc.dwell_entry_peak_sim[z][seg], sc.dwell_entry_peak_rec[z][seg],
                   fabsf(sc.dwell_entry_peak_sim[z][seg] - sc.dwell_entry_peak_rec[z][seg]), BAR_DWELL_PEAK_C);
        }
    }
    printf("\n");
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: sim_credibility_gate_closedloop <calibration.jsonl> <holdout.jsonl>\n");
        return 2;
    }
    for (int i = 1; i <= 2; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) {
            printf("SKIP: sim_credibility_gate_closedloop -- required input missing: %s\n", argv[i]);
            return 3;
        }
        fclose(f);
    }
    static tick_t cal_ticks[MAX_TICKS];
    static tick_t hold_ticks[MAX_TICKS];
    int cal_n = load_capture(argv[1], cal_ticks, MAX_TICKS);
    int hold_n = load_capture(argv[2], hold_ticks, MAX_TICKS);
    if (cal_n <= 0 || hold_n <= 0) {
        fprintf(stderr, "failed to parse captures (cal_n=%d hold_n=%d)\n", cal_n, hold_n);
        return 2;
    }

    printf("=== sim_credibility_gate_closedloop -- candidate 3 (closed-loop replay) ===\n");
    printf("SEE THIS FILE'S TOP COMMENT before trusting any number below: gains\n"
           "provenance for kp/ki/kd is NOT established (point 2) and this replay\n"
           "is NOT non-circular the way sim_credibility_gate.c is (point 1).\n\n");

    run_and_print("CALIBRATION", cal_ticks, cal_n, &GAIN_PRESET);
    run_and_print("CALIBRATION", cal_ticks, cal_n, &GAIN_LIVE);
    run_and_print("HOLD-OUT", hold_ticks, hold_n, &GAIN_PRESET);
    run_and_print("HOLD-OUT", hold_ticks, hold_n, &GAIN_LIVE);

    return 0;
}
