// sim_scenario_table -- the S0/S1/S3 rows (WI-4). See the header for the
// "adding a scenario is one entry" contract.
#include "sim_scenario_table.h"

#include <stddef.h>

#include "sim_high_temp.h"
#include "sim_measured_zone_constants.h"
#include "sim_mistune.h"

const char *const SIM_ARM_NAMES[SIM_ARM_COUNT] = {
    "A_PID", "A_PID_AT", "A_FUZZY25", "A_FUZZY50", "A_FUZZY_AT", "A_STATIC_MATCHED",
};

// Zone 0's real measured bench constants (sim_measured_zone_constants.h,
// live GET /api/zones 2026-09-10), used as-is by S0/S1's legacy plant, same
// convention every other harness in this tree (sim_fuzzy_closedloop.c,
// sim_iter_tune.c) already uses.
#define BENCH_K_DC   (g_k_dc[0])
#define BENCH_TAU_S  (g_tau_s[0])
#define BENCH_DEAD_TIME_S (g_dead_time_s[0])
#define BENCH_AMBIENT_C 24.0f

static sim_plant_cfg_t legacy_bench_plant(void)
{
    sim_plant_cfg_t p = {0};
    p.ambient_c = BENCH_AMBIENT_C;
    p.node_model = SIM_NODE_LEGACY;
    p.thermal_mass_j_per_c = BENCH_TAU_S;
    p.heater_power_w = BENCH_K_DC;
    p.loss_coeff_w_per_c = 1.0f;
    p.sensor_delay_s = BENCH_DEAD_TIME_S;
    p.sensor_lag_tau_s = 0.0f;
    return p;
}

// S3 (SENSOR_CENTRE) -- three-node model, sensor_bias_p = 0.0 (all
// conductance to the load, the centre-mounted reference case), built so its
// AGGREGATE (K, tau) matches S1's legacy plant EXACTLY at steady state and
// APPROXIMATELY in the transient (singular-perturbation argument below),
// per WI-4's own acceptance criterion (c): "S3's results match S1's within
// materiality, proving the three-node model at sensor_bias_p = 0 is not
// itself a confound."
//
// Derivation (TEST FIXTURE, not measured):
//   - g_ea_w_per_c = 0 (no direct element->ambient loss; every loss path
//     through the load, matching legacy's single loss term).
//   - g_la_w_per_c = 1.0, c_l_j_per_c = BENCH_TAU_S: at steady state, with
//     g_ea = 0, the E balance gives G_el*(E-L) = u*Pmax, and substituting
//     into the L balance gives L - Tamb = u*Pmax/G_la at steady state --
//     independent of G_el/C_e entirely. Setting Pmax=BENCH_K_DC and
//     G_la=1.0 reproduces legacy's DC gain (heater_power_w/loss_coeff_w_per_c
//     = K_dc/1.0) exactly, bit for bit at steady state.
//   - c_e_j_per_c = 13.0, g_el_w_per_c = 1.0: this makes the element node's
//     own relaxation time C_e/G_el = 13 s, roughly 20x faster than the
//     load's C_l/G_la = BENCH_TAU_S (~264 s on zone 0). By the standard
//     singular-perturbation argument, when the fast state (E) is much
//     faster than the slow state (L), E quasi-instantly satisfies
//     0 = u*Pmax - G_el*(E-L) (with g_ea=0), i.e. E = L + u*Pmax/G_el, and
//     substituting into dL/dt collapses the two-node system to EXACTLY
//     legacy's dL/dt = (u*Pmax - G_la*(L-Tamb))/C_l, to the extent the
//     separation holds (~20x here -- not exact, hence "within materiality,"
//     not "bit-identical," which is the whole point of this being a
//     SEPARATE code path from S1's legacy step per sim_plant.h's own
//     bit-identical-default contract). 13 s is also comfortably above the
//     dt_s = 1.0 s this suite runs at (forward-Euler stability wants the
//     fast mode's own tau well above dt; 13x margin is ample).
//   - sensor_bias_p = 0.0: the sensor sees ONLY the load (G_se=0), so its
//     placement contributes nothing here -- this scenario isolates "is the
//     three-node CODE PATH itself a confound," not sensor placement, which
//     is S2/S4 (WI-6).
//   - sensor_tau_s = 10.0 (TEST FIXTURE, same value test_sim_plant_three_
//     node.c uses for "a sheathed kiln TC"): a small additional lag on top
//     of legacy's sensor_delay_s (applied identically to both plants via
//     the shared transport-delay/lag pipeline), negligible against a ~264 s
//     bulk time constant.
static sim_plant_cfg_t three_node_centre_plant(void)
{
    sim_plant_cfg_t p = {0};
    p.ambient_c = BENCH_AMBIENT_C;
    p.node_model = SIM_NODE_THREE;
    p.heater_power_w = BENCH_K_DC;      /* P_max */
    p.c_e_j_per_c = 3.0f;               /* TEST FIXTURE: fast element mode, ~88x faster than the load */
    p.c_l_j_per_c = BENCH_TAU_S;        /* matches legacy thermal_mass_j_per_c exactly */
    p.c_s_j_per_c = 1.0f;               /* TEST FIXTURE: sensor tip capacity, only the ratio to sensor_tau_s matters */
    p.g_el_w_per_c = 1.0f;              /* TEST FIXTURE */
    p.g_ea_w_per_c = 0.0f;              /* all loss through the load, matching legacy's single loss term */
    p.g_la_w_per_c = 1.0f;              /* matches legacy loss_coeff_w_per_c exactly -- sets the DC gain */
    p.sensor_tau_s = 10.0f;             /* TEST FIXTURE, same convention as test_sim_plant_three_node.c */
    p.sensor_bias_p = 0.0f;             /* centre-mounted: sensor sees only the load */
    p.load_mass_mult = 1.0f;
    p.sensor_delay_s = BENCH_DEAD_TIME_S; /* same transport delay as legacy, applied to node S */
    p.sensor_lag_tau_s = 0.0f;
    return p;
}

// S2/S4 (SENSOR_NEAR_ELEMENT[_FAST_RAMP]) -- same three-node build as
// three_node_centre_plant() above, EXCEPT sensor_bias_p = 5/6 (~0.8333): the
// "5x closer to the elements than to the load" reading sec 2.1 defines as a
// 5:1 conductance ratio. Tune stays MATCHED TO THE CENTRE-MOUNTED FIT (the
// plan's own wording for S2/S4: "matched to the centre-mounted fit") --
// i.e. model_k_dc/tau_s/dead_time_s are set to the SAME bench numbers S1/S3
// use, on purpose: this is "the tune a centre-mounted commissioning would
// have produced, then the sensor got relocated," not a re-identification.
static sim_plant_cfg_t three_node_near_element_plant(void)
{
    sim_plant_cfg_t p = three_node_centre_plant();
    p.sensor_bias_p = 5.0f / 6.0f; /* ~0.8333, sec 2.1's 5:1 conductance-ratio reading */
    return p;
}

// S5/S6 (MASS_HEAVY/MASS_LIGHT) -- three-node, centre-mounted (placement is
// not this scenario's variable), load_mass_mult != 1.0. Tune stays matched
// to load_mass_mult == 1.0 (the table's model_k_dc/tau_s below), i.e. "ware
// added/removed AFTER tuning" per sec 2.2 -- the plant's bulk capacity moves,
// nothing about the installed tune does.
static sim_plant_cfg_t three_node_mass_plant(float load_mass_mult)
{
    sim_plant_cfg_t p = three_node_centre_plant();
    p.load_mass_mult = load_mass_mult;
    return p;
}

// S9 (TUNE_SLOW_INTEGRAL) -- "3-node, matched" per the plan's own table:
// centre-mounted (placement not this scenario's variable), matched plant;
// the mismatch is entirely in the TUNE (model_dead_time_s tracking a
// 3x-too-long tau, sec 2.4/SIM_MISTUNE_SLOW_INTEGRAL), installed on the
// model_* fields below, not on the plant.
static sim_plant_cfg_t three_node_matched_plant(void)
{
    return three_node_centre_plant();
}

// S12 (COMPOUND_WORST) -- all three mismatch directions stacked: near-element
// sensor, heavy load, and (via model_* below) a HOT tune.
static sim_plant_cfg_t three_node_compound_plant(void)
{
    sim_plant_cfg_t p = three_node_centre_plant();
    p.sensor_bias_p = 5.0f / 6.0f;
    p.load_mass_mult = 3.0f;
    return p;
}

// S10/S11 (KILN_HIGH_T[_SCHEDULED]) -- kiln-scale three-node plant, sec 2.3/
// WI-2, with sensor_bias_p set to the owner's near-element reading (these
// two scenarios stack "high temperature" on top of "near-element sensor",
// per the plan's own table). base_g_ea/g_la are recorded on the scenario row
// itself (sim_scenario_t::base_g_ea_w_per_c/base_g_la_w_per_c) so the runner
// can re-derive the T_REF-anchored (scale==1.0) values sim_high_temp_scale_
// conductances() needs every tick -- sim_high_temp_kiln_scale_cfg() already
// installs the SAME base values into g_ea_w_per_c/g_la_w_per_c (scale==1.0
// at construction), so the row's base_g_* fields are simply read back off
// the constructed cfg immediately below, never duplicated by hand.
static sim_plant_cfg_t kiln_scale_near_element_plant(float *out_base_g_ea, float *out_base_g_la)
{
    sim_plant_cfg_t p;
    sim_high_temp_kiln_scale_cfg(&p);
    p.sensor_bias_p = 5.0f / 6.0f;
    if (out_base_g_ea) *out_base_g_ea = p.g_ea_w_per_c;
    if (out_base_g_la) *out_base_g_la = p.g_la_w_per_c;
    return p;
}

// S10's ONE upfront tune, "at 200 C" per the plan's table: derive the
// apparent (k_dc, tau_s) the s(T) scale gives at a 200 C reference and hand
// those to SIM_MISTUNE_MATCHED (mismatch factors 1.0, i.e. "install the tune
// this apparent model implies," not a deliberately-wrong mistune -- S10's
// mismatch is entirely the untracked high-temperature growth, not the tune
// step itself). Steady-state DC gain and tau both share the loss-side
// conductance denominator (sec 1.1/2.3's own derivation, g_ea=0 here so all
// loss is through g_la): k_dc(T) = heater_power_w / g_la(T), tau(T) =
// (c_l_j_per_c*load_mass_mult) / g_la(T). dead_time_s is the transport delay
// and is NOT a function of conductance, so it is left at the kiln-scale
// plant's own sensor_delay_s, unscaled.
static void kiln_scale_tune_at(const sim_plant_cfg_t *base_plant, float base_g_ea, float base_g_la,
                               float reference_temp_c, float *out_k_dc, float *out_tau_s, float *out_l_s)
{
    sim_plant_cfg_t scaled = *base_plant;
    sim_high_temp_scale_conductances(&scaled, base_g_ea, base_g_la, reference_temp_c);
    float mult = (base_plant->load_mass_mult > 0.0f) ? base_plant->load_mass_mult : 1.0f;
    *out_k_dc = base_plant->heater_power_w / scaled.g_la_w_per_c;
    *out_tau_s = (base_plant->c_l_j_per_c * mult) / scaled.g_la_w_per_c;
    *out_l_s = base_plant->sensor_delay_s;
}

static const sim_scenario_t TABLE[] = {
    {
        .id = "S0_NULL_SLOW",
        .plant = { 0 }, /* filled below via designated init workaround -- see note */
        .ambient_c = BENCH_AMBIENT_C,
        .ramp_rate_c_per_hr = 40.0f,
        .t1_offset_c = 15.0f, .t2_offset_c = 25.0f,
        .sep_expected = false,
        .sep_reason = "Control: legacy bench plant, matched tune, a slow ramp every arm converges on. "
                      "If THIS one separates, the harness itself is broken, not the controller.",
    },
    {
        .id = "S1_BASELINE",
        .plant = { 0 },
        .ambient_c = BENCH_AMBIENT_C,
        .ramp_rate_c_per_hr = 150.0f,
        .t1_offset_c = 15.0f, .t2_offset_c = 30.0f,
        .sep_expected = true, /* WI-6 RE-PIN, first measurement 2026-09-14: measured_separated=yes
                               * (d_lag_equiv_c=0.17, d_steady=0.25, d_peak=0.68 -- ENTRY_PEAK_C is
                               * the material objective here). This IS the 1570a65a finding itself,
                               * and A_STATIC_MATCHED reproduces it inside materiality (classified
                               * GAIN_ONLY by this suite's own sec 5.3 #3 check) -- exactly what
                               * 1570a65a's correction says should happen on this bench condition. */
        .sep_reason = "Reproduces the 1570a65a bench condition; anchors every other scenario. "
                      "Re-pinned yes at first measurement (was left unpinned by WI-4, which only "
                      "asserted S0 and S3 against S1, never S1's own arm separation) -- measured "
                      "separation classifies GAIN_ONLY, matching 1570a65a's own correction.",
    },
    {
        .id = "S3_SENSOR_CENTRE",
        .plant = { 0 },
        .ambient_c = BENCH_AMBIENT_C,
        .ramp_rate_c_per_hr = 150.0f,
        .t1_offset_c = 15.0f, .t2_offset_c = 30.0f,
        .sep_expected = true, /* WI-6 RE-PIN, first measurement 2026-09-14: measured_separated=yes,
                               * essentially identical to S1's own numbers (WI-4's own acceptance
                               * criterion (c) already showed S3 matches S1 within materiality on
                               * every arm) -- S3 mirroring S1's real separation is confirmation the
                               * three-node model at sensor_bias_p=0 is NOT itself a confound, not a
                               * new/different finding. */
        .sep_reason = "Isolates \"3-node model\" from \"sensor placement\": sensor_bias_p=0 (centre-"
                      "mounted). Re-pinned yes at first measurement to match S1's own (also re-pinned) "
                      "separation -- S3 mirrors S1 to within materiality (WI-4 acceptance criterion c), "
                      "so this is the SAME finding as S1's, not independent evidence.",
    },
    {
        .id = "S2_SENSOR_NEAR_ELEMENT",
        .plant = { 0 },
        .ambient_c = BENCH_AMBIENT_C,
        .ramp_rate_c_per_hr = 150.0f,
        .t1_offset_c = 15.0f, .t2_offset_c = 30.0f,
        .sep_expected = true,
        .sep_reason = "Owner's headline case: sensor_bias_p=5/6 (~0.8333, sec 2.1's 5:1 conductance-"
                      "ratio reading), tune matched to the CENTRE-mounted fit (same model_* as S1/S3) "
                      "-- the mismatch is entirely 'the sensor got relocated after commissioning.'",
    },
    {
        .id = "S4_SENSOR_NEAR_FAST_RAMP",
        .plant = { 0 },
        .ambient_c = BENCH_AMBIENT_C,
        .ramp_rate_c_per_hr = 300.0f,
        .t1_offset_c = 15.0f, .t2_offset_c = 30.0f,
        .sep_expected = true,
        .sep_reason = "Same placement mismatch as S2 at 2x the ramp rate -- the literature says "
                      "overshoot grows with rate under this kind of sensor lead/lag mismatch.",
    },
    {
        .id = "S5_MASS_HEAVY",
        .plant = { 0 },
        .ambient_c = BENCH_AMBIENT_C,
        .ramp_rate_c_per_hr = 150.0f,
        .t1_offset_c = 15.0f, .t2_offset_c = 30.0f,
        .sep_expected = true,
        .sep_reason = "load_mass_mult=3.0, tune matched to mult=1.0 -- ware added after tuning; the "
                      "installed tune is now under-aggressive for the new bulk capacity.",
    },
    {
        .id = "S6_MASS_LIGHT",
        .plant = { 0 },
        .ambient_c = BENCH_AMBIENT_C,
        .ramp_rate_c_per_hr = 150.0f,
        .t1_offset_c = 15.0f, .t2_offset_c = 30.0f,
        .sep_expected = false, /* WI-6 RE-PIN, first measurement 2026-09-14: measured_separated=no
                               * (max pairwise diff on any objective stayed under 0.5 degC -- d_lag_
                               * equiv_c=0.17, d_steady=0.22, d_peak=0.00, d_under=0.05). The plan's
                               * a priori "yes" assumed the lighter-bulk direction would visibly
                               * over-drive this plant/tune combination; measured, it does not clear
                               * materiality on this specific SIMC tune. Report only, not adjudicated
                               * further here (an opus review reads this). */
        .sep_reason = "load_mass_mult=0.5, tune matched to mult=1.0 -- kiln emptied after tuning, the "
                      "aggressive direction. Re-pinned no at first measurement: separation on this "
                      "plant/tune combination stays under the 0.5 degC materiality line on all four "
                      "objectives (was pinned yes a priori by the plan; measurement corrects it).",
    },
    {
        .id = "S7_TUNE_HOT",
        .plant = { 0 },
        .ambient_c = BENCH_AMBIENT_C,
        .ramp_rate_c_per_hr = 150.0f,
        .t1_offset_c = 15.0f, .t2_offset_c = 30.0f,
        .sep_expected = true,
        .sep_reason = "3-node matched plant; model_* handed to the tuner is SIM_MISTUNE_HOT (k*0.5, "
                      "tau*2.0, L*0.5 vs the true plant) -- 'never good on the real kiln,' aggressive "
                      "direction (SIMC reports higher Kp for a model that looks slower/lower-gain).",
    },
    {
        .id = "S8_TUNE_COLD",
        .plant = { 0 },
        .ambient_c = BENCH_AMBIENT_C,
        .ramp_rate_c_per_hr = 150.0f,
        .t1_offset_c = 15.0f, .t2_offset_c = 30.0f,
        .sep_expected = true,
        .sep_reason = "3-node matched plant; model_* is SIM_MISTUNE_COLD (k*2.0, tau*0.5, L*2.0) -- "
                      "timid direction, expect slow settle and large undershoot.",
    },
    {
        .id = "S9_TUNE_SLOW_INTEGRAL",
        .plant = { 0 },
        .ambient_c = BENCH_AMBIENT_C,
        .ramp_rate_c_per_hr = 150.0f,
        .t1_offset_c = 15.0f, .t2_offset_c = 30.0f,
        .sep_expected = true, /* WI-6 RE-PIN, 2026-09-14, REVIEWED 2026-09-14 (PID_RANGE_C/S9-vs-
                               * research pass): measured_separated=yes, driven by d_under=0.7414 degC
                               * (ENTRY_UNDERSHOOT_C, dwell-entry transient) plus a borderline
                               * d_lag_equiv_c=0.5833 degC-equiv (the max-PAIRWISE diff across all six
                               * arms; the PID-vs-FUZZY50 pair alone reads exactly 0.5000, at the
                               * materiality line, not over it). VERDICT (see this suite's
                               * classification, not asserted as final): this is NOT actually a
                               * contradiction of docs/research/fuzzy_ramp_tracking_2026-09-13.md.
                               * That doc's claim is specifically about STEADY-STATE ramp-following lag
                               * (governed by Kv=Ki*P(0), where the two axis-aligned "error large, rate
                               * STEADY" cells leave Ki at x1.0). entry_undershoot_c is a DWELL-ENTRY
                               * TRANSIENT metric -- exactly the step-response overshoot/undershoot case
                               * the same doc says the Kp-push cells ARE the right/intended lever for.
                               * The measured applied_ki_mult_mean here is x1.0635 (a real 6.35%
                               * increase, not exactly 1.0), consistent with the doc's own caveat that
                               * inference blends continuously across the 3x3 grid rather than snapping
                               * to the two named axis cells -- some off-axis blending nudges Ki a
                               * little even though the axis cells themselves do not. Classified
                               * GAIN_ONLY by this suite's own sec 5.3 #3 check (A_STATIC_MATCHED
                               * reproduces the separation): the effect is the ordinary Kp-driven
                               * transient response to a slightly different trajectory approaching the
                               * dwell (caused by the mistuned Ti), not the fuzzy layer doing anything
                               * integral-specific. Still a re-pin per measurement, not a re-derivation
                               * of the research doc -- flagged for a human/opus read to confirm this
                               * reading, per this plan's own instruction not to report findings as
                               * conclusions here. */
        .sep_reason = "3-node matched plant; model_* is SIM_MISTUNE_SLOW_INTEGRAL (tau*3.0 fed to the "
                      "tuner's Ti only, dead-time/gain untouched) -- isolates the integral term. "
                      "Measured separation (d_under=0.7414 degC) traces to the DWELL-ENTRY TRANSIENT, "
                      "not steady ramp-following lag -- see the sep_expected comment for why this reads "
                      "as consistent with, not contradicting, docs/research/fuzzy_ramp_tracking_"
                      "2026-09-13.md once the two are distinguished. Classified GAIN_ONLY -- see "
                      "CLASSIFICATION line. Flagged for a human/opus review to confirm the reading, per "
                      "this plan's own instruction not to report findings as conclusions here.",
    },
    {
        .id = "S10_KILN_HIGH_T",
        .plant = { 0 },
        .ambient_c = 24.0f,
        .ramp_rate_c_per_hr = 150.0f,
        .t1_offset_c = 576.0f, .t2_offset_c = 1226.0f, /* -> T1=600C, T2=1250C off a 24C ambient */
        .sep_expected = false, /* WI-6 RE-PIN, 2026-09-14; REVISED 2026-09-14 (PID_RANGE_C
                               * investigation). The original re-pin blamed PID_RANGE_C (25 degC,
                               * shared bench-scale #define) for near-total bang-bang saturation. That
                               * diagnosis was WRONG: PID_RANGE_C mirrors the REAL production constant
                               * PID_FUNCTIONAL_RANGE_C (profile_executor_internal.h) -- a genuine
                               * controller parameter, fixed in absolute degrees C in shipped firmware
                               * too, not a harness-only fixture, and not scaled to plant/tau by any
                               * real installation either. Rescaling it in the harness to manufacture
                               * separation would have tested a hypothetical firmware behaviour this
                               * board does not have.
                               *
                               * The ACTUAL root cause was a TEST FIXTURE bug in sim_high_temp.h's
                               * kiln-scale plant: c_l_j_per_c=2,000,000 J/C against
                               * heater_power_w=2500 W gives a near-ambient full-duty heating rate of
                               * only P/C_l = 4.5 C/hr -- over 30x slower than this scenario's own
                               * 150 C/hr commanded ramp. A plant physically unable to keep up with its
                               * own commanded ramp pins duty at 1.0 (pid.c's final u clamp fires
                               * regardless of pid_range_c) for most of the firing, independent of
                               * gains -- THAT is why every arm read identical. Fixed by rescaling every
                               * capacity (c_e/c_l/c_s) down by the same 133.33x factor (steady-state
                               * g_ea/g_la/heater_power_w untouched, so the documented ~1288 C full-duty
                               * asymptote is unaffected) -- see sim_high_temp.c's own comment.
                               *
                               * Re-measured post-fix: saturated-duty fraction (SATFRAC, this suite's
                               * new instrumentation) dropped from near-total to ~0.286, and the applied
                               * gain multipliers now move substantially (x0.7589 Kp / x0.9623 Ki,
                               * vs ~1.0000 uniformly before) -- fuzzy genuinely engages. Despite that,
                               * measured_separated is STILL no on all four objectives (max pairwise
                               * diff ~0.03-0.08 degC/degC-equiv) -- a materially different, no longer
                               * degenerate result than before, but still not separating. Report as a
                               * numeric finding, not a conclusion: with saturation no longer dominant,
                               * a real 24% Kp / 4% Ki spread produces no measurable difference in
                               * lag/steady/entry outcomes at kiln scale -- worth a human/opus read, not
                               * resolved further here. */
        .sep_reason = "Kiln-scale 3-node plant, s(T) conductance scaling ON, sensor_bias_p=5/6, tuned "
                      "ONCE at 200C and never rescheduled -- the regime the ~4W bench cannot reach. "
                      "Carries [f_rad=0.05 ASSUMED] (sim_high_temp.h). Re-pinned no post-fixture-fix: "
                      "saturated_duty_frac ~0.286 (down from near-total), gains now genuinely move "
                      "(x0.76 Kp / x0.96 Ki), yet outcomes still do not separate beyond materiality -- "
                      "see the sep_expected comment for the full mechanism and what changed 2026-09-14.",
    },
    {
        .id = "S11_KILN_HIGH_T_SCHEDULED",
        .plant = { 0 },
        .ambient_c = 24.0f,
        .ramp_rate_c_per_hr = 150.0f,
        .t1_offset_c = 576.0f, .t2_offset_c = 1226.0f,
        .sep_expected = false, /* WI-6 RE-PIN, 2026-09-14; REVISED 2026-09-14, same fixture fix as
                               * S10 (see that scenario's comment for the full mechanism -- PID_RANGE_C
                               * was NOT the cause, sim_high_temp.h's c_l_j_per_c/heater_power_w
                               * mismatch was). Post-fix: saturated_duty_frac ~0.285 (was near-total),
                               * gains move (x0.759 Kp / x0.962 Ki), re-tuning per segment does not
                               * close the gap any more than S10's single upfront tune did -- still
                               * measured_separated=no on all four objectives. This now genuinely
                               * distinguishes "schedule vs. no schedule" (both non-degenerate) rather
                               * than "fuzzy never got to act either way," and the answer is: neither
                               * arrangement separates at kiln scale, for a reason this suite does not
                               * resolve further -- flagged for a human/opus read alongside S10. */
        .sep_reason = "Same plant as S10, but RE-TUNED at the start temperature of each of the 4 "
                      "segments (SIM_MISTUNE_MATCHED against the apparent s(T)-scaled model at that "
                      "temperature). Re-pinned no post-fixture-fix, same as S10 (see its comment) -- "
                      "saturated_duty_frac ~0.285, gains genuinely move, per-segment retuning still "
                      "does not produce separation from the single-tune S10.",
    },
    {
        .id = "S12_COMPOUND_WORST",
        .plant = { 0 },
        .ambient_c = BENCH_AMBIENT_C,
        .ramp_rate_c_per_hr = 200.0f,
        .t1_offset_c = 15.0f, .t2_offset_c = 30.0f,
        .sep_expected = true,
        .sep_reason = "All three bench-scale mismatches stacked: sensor_bias_p=5/6, load_mass_mult=3.0, "
                      "model_* = SIM_MISTUNE_HOT, at 200C/hr -- most likely place for a fuzzy benefit or "
                      "a fuzzy failure to be visible.",
    },
};

#define SIM_SCENARIO_COUNT_EXPECTED 13
_Static_assert(sizeof(TABLE) / sizeof(TABLE[0]) == SIM_SCENARIO_COUNT_EXPECTED,
               "sim_scenario_table.c: TABLE grew or shrank without a deliberate review of "
               "SIM_SCENARIO_COUNT_EXPECTED -- bump the constant here, on purpose, when adding a row "
               "(WI-4 ships S0/S1/S3 only; S2/S4-S12 are WI-6).");

// Mutable copy the runner reads from -- sim_plant_cfg_t is too large/complex
// (nested TEST FIXTURE floats) to hand-write as a C89-compatible nested
// designated initializer inline above without either duplicating
// legacy_bench_plant()/three_node_centre_plant()'s logic per row (exactly
// the kind of duplication WI-1's own compatibility contract warns against)
// or relying on GNU statement-expressions. Patched once, before first use,
// by sim_scenario_table_init() below -- NOT per-call, so the table is still
// effectively `static const` from every caller's perspective after main()
// calls this once.
static sim_scenario_t g_table[SIM_SCENARIO_COUNT_EXPECTED];
static bool g_table_ready = false;

void sim_scenario_table_init(void);
void sim_scenario_table_init(void)
{
    if (g_table_ready) return;
    for (int i = 0; i < SIM_SCENARIO_COUNT_EXPECTED; i++) {
        g_table[i] = TABLE[i];
        /* g_k_dc[0]/g_tau_s[0]/g_dead_time_s[0] are runtime array reads, not
         * compile-time constants, so they cannot appear in TABLE's static
         * initializer above (MSVC C2099) -- patched here instead, once. */
        g_table[i].model_k_dc = BENCH_K_DC;
        g_table[i].model_tau_s = BENCH_TAU_S;
        g_table[i].model_dead_time_s = BENCH_DEAD_TIME_S;
    }
    g_table[0].plant = legacy_bench_plant();                    /* S0 */
    g_table[1].plant = legacy_bench_plant();                    /* S1 */
    g_table[2].plant = three_node_centre_plant();                /* S3 */
    g_table[3].plant = three_node_near_element_plant();          /* S2 -- model_* left at BENCH_* (centre-mounted fit) */
    g_table[4].plant = three_node_near_element_plant();          /* S4 -- same, faster ramp */
    g_table[5].plant = three_node_mass_plant(3.0f);              /* S5 */
    g_table[6].plant = three_node_mass_plant(0.5f);              /* S6 */
    g_table[7].plant = three_node_matched_plant();               /* S7 -- model_* patched below to MISTUNE_HOT */
    g_table[8].plant = three_node_matched_plant();               /* S8 -- model_* patched below to MISTUNE_COLD */
    g_table[9].plant = three_node_matched_plant();               /* S9 -- model_* patched below to MISTUNE_SLOW_INTEGRAL */
    g_table[12].plant = three_node_compound_plant();             /* S12 -- model_* patched below to MISTUNE_HOT */

    /* S7/S8/S9/S12: the tuner is handed a deliberately-wrong model. Factors
     * come from sim_mistune_factors() -- the SAME named constants
     * test_sim_mistune.c pins -- applied to the TRUE bench FOPDT, never
     * hardcoded here a second time. */
    {
        sim_mistune_factors_t hot = sim_mistune_factors(SIM_MISTUNE_HOT);
        g_table[7].model_k_dc = BENCH_K_DC * hot.mismatch_k;
        g_table[7].model_tau_s = BENCH_TAU_S * hot.mismatch_tau;
        g_table[7].model_dead_time_s = BENCH_DEAD_TIME_S * hot.mismatch_l;

        g_table[12].model_k_dc = BENCH_K_DC * hot.mismatch_k;
        g_table[12].model_tau_s = BENCH_TAU_S * hot.mismatch_tau;
        g_table[12].model_dead_time_s = BENCH_DEAD_TIME_S * hot.mismatch_l;
    }
    {
        sim_mistune_factors_t cold = sim_mistune_factors(SIM_MISTUNE_COLD);
        g_table[8].model_k_dc = BENCH_K_DC * cold.mismatch_k;
        g_table[8].model_tau_s = BENCH_TAU_S * cold.mismatch_tau;
        g_table[8].model_dead_time_s = BENCH_DEAD_TIME_S * cold.mismatch_l;
    }
    {
        sim_mistune_factors_t slow_i = sim_mistune_factors(SIM_MISTUNE_SLOW_INTEGRAL);
        g_table[9].model_k_dc = BENCH_K_DC * slow_i.mismatch_k;
        g_table[9].model_tau_s = BENCH_TAU_S * slow_i.mismatch_tau;
        g_table[9].model_dead_time_s = BENCH_DEAD_TIME_S * slow_i.mismatch_l;
    }

    /* S10/S11: kiln-scale, s(T) dynamic conductance scaling. */
    {
        float base_g_ea = 0.0f, base_g_la = 0.0f;
        sim_plant_cfg_t kiln_plant = kiln_scale_near_element_plant(&base_g_ea, &base_g_la);
        /* sim_high_temp_kiln_scale_cfg() sets its OWN ambient_c=20.0f (its
         * own physical anchor, sec 2.3), independent of this table's
         * BENCH_AMBIENT_C convention. S10/S11's row above declares
         * ambient_c=24.0f (matching every other scenario's ramp-schedule
         * convention, sc->ambient_c) -- the plant's own ambient_c MUST be
         * kept in lock-step with that, or the reset state (which starts
         * pstate.sensor_c at plant.ambient_c) starts BELOW the runner's
         * structural floor_c = sc->ambient_c - 1, tripping a spurious
         * refusal on tick zero, before capture even begins (found during
         * WI-6 bring-up, 2026-09-14: "sensor reading left [23.0, ...]"). */
        kiln_plant.ambient_c = 24.0f;

        g_table[10].plant = kiln_plant;                          /* S10 */
        g_table[10].high_temp_dynamic_scale = true;
        g_table[10].retune_per_segment = false;
        g_table[10].base_g_ea_w_per_c = base_g_ea;
        g_table[10].base_g_la_w_per_c = base_g_la;
        {
            float k_dc, tau_s, l_s;
            kiln_scale_tune_at(&kiln_plant, base_g_ea, base_g_la, 200.0f, &k_dc, &tau_s, &l_s);
            g_table[10].model_k_dc = k_dc;
            g_table[10].model_tau_s = tau_s;
            g_table[10].model_dead_time_s = l_s;
        }

        g_table[11].plant = kiln_plant;                          /* S11 */
        g_table[11].high_temp_dynamic_scale = true;
        g_table[11].retune_per_segment = true;
        g_table[11].base_g_ea_w_per_c = base_g_ea;
        g_table[11].base_g_la_w_per_c = base_g_la;
        /* model_* here is only the fallback used if the runner is ever asked
         * to score this row's aggregate FOPDT directly (it is not, once
         * retune_per_segment recomputes per segment) -- set to the same
         * 200C tune as S10 so an accidental read is at least a valid model,
         * never zero/refused. */
        g_table[11].model_k_dc = g_table[10].model_k_dc;
        g_table[11].model_tau_s = g_table[10].model_tau_s;
        g_table[11].model_dead_time_s = g_table[10].model_dead_time_s;
    }

    g_table_ready = true;
}

const sim_scenario_t *sim_scenario_table(void)
{
    sim_scenario_table_init();
    return g_table;
}

const int SIM_SCENARIO_COUNT = SIM_SCENARIO_COUNT_EXPECTED;
