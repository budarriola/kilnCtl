// Tests for sim_plant.c's opt-in three-node model (WI-1,
// docs/SCENARIO_SIMULATION.md sec 2.1/2.2). Every constant in this file
// is a TEST FIXTURE -- never shipped, never written into zones_config, a
// preset, or a firmware default.
//
// What is being checked, and why: the entire justification for the
// three-node model is the physical claim that a sensor mounted near the
// elements (high sensor_bias_p) LEADS the bulk load on a rise and FALLS
// FASTER than the load when the elements cut out, because its dominant heat
// source disappeared while the load -- holding nearly all the stored energy
// -- barely moves (sec 2.1's "physical claim being modelled" and sec 5.3's
// structural invariant #1). If this file's asserts do not hold, the sensor
// model is wrong and every scenario built on it (S2, S4, S5, S6, S10-S12) is
// void. The centre-mounted case (sensor_bias_p = 0) is the converse check:
// a sensor with all its conductance on the load must LAG it, same as any
// first-order filter on the load's own signal.
#include <math.h>
#include <stdio.h>

#include "sim_plant.h"
#include "test_common.h"

#define DT_S 1.0f

/* Test-fixture plant: an element 100x lighter (by capacity) than the bulk
 * load, so a duty step drives the element to a quasi-equilibrium with the
 * (still nearly stationary) load almost immediately -- at these constants
 * that quasi-equilibrium already sits at ~75% of the eventual, fully joint
 * steady state, so the element's own step response is dominated by a fast
 * mode with time constant on the order of tens of seconds, while the load
 * only reaches the same steady state on a mode roughly two orders of
 * magnitude slower (driven by its own capacity against the small ambient
 * loss conductances). A small, fast-responding sensor tip (its own bare tau
 * = c_s/g_s = 10s) then reports whichever weighted mix of the two the
 * placement bias (sensor_bias_p) selects. None of these numbers are
 * measured or physical; they exist only to give the two nodes a clearly
 * separated pair of effective time constants so the assertions below are
 * unambiguous. */
static sim_plant_cfg_t three_node_cfg(float sensor_bias_p)
{
    sim_plant_cfg_t p = {0};
    p.ambient_c = 20.0f;
    p.node_model = SIM_NODE_THREE;
    p.heater_power_w = 1000.0f;   /* P_max -- TEST FIXTURE */
    p.c_e_j_per_c = 50.0f;        /* element capacity -- TEST FIXTURE, 100x lighter than the load */
    p.c_l_j_per_c = 5000.0f;      /* load capacity -- TEST FIXTURE, slow bulk */
    p.c_s_j_per_c = 1.0f;         /* sensor tip capacity -- TEST FIXTURE, tiny */
    p.g_el_w_per_c = 1.0f;        /* element<->load conductance -- TEST FIXTURE */
    p.g_ea_w_per_c = 1.0f;        /* element->ambient loss -- TEST FIXTURE */
    p.g_la_w_per_c = 1.0f;        /* load->ambient loss -- TEST FIXTURE */
    p.sensor_tau_s = 10.0f;       /* sensor tip's own bare time constant -- TEST FIXTURE */
    p.sensor_bias_p = sensor_bias_p;
    p.load_mass_mult = 1.0f;
    /* sensor_delay_s / sensor_lag_tau_s left at 0: this test is isolating
     * the three-node placement effect itself, not the downstream transport
     * pipeline (already covered by the legacy-model tests). */
    return p;
}

/* Runs a duty step from 0 to `duty` for max_steps ticks, recording the
 * sensor NODE (pre-delay/lag; delay/lag are both 0 in this cfg so
 * state.sensor_c tracks sensor_node_c exactly) and load traces. */
static void run_step_response(const sim_plant_cfg_t *cfg, float duty, int max_steps,
                               float *sensor_c, float *load_c)
{
    sim_plant_state_t st;
    sim_plant_reset(&st, cfg);
    for (int i = 0; i < max_steps; i++) {
        sim_plant_three_node_step(&st, cfg, duty, DT_S);
        sensor_c[i] = st.sensor_c;
        load_c[i] = st.load_c;
    }
}

/* First index whose trace has covered `frac` of the way from `start` to the
 * trace's own final value (index max_steps-1) -- a standard normalized rise
 * time, robust to the two nodes having different DC gains. Returns
 * max_steps if it never gets there (should not happen at these fixture
 * constants over the chosen horizon). */
static int rise_time_steps(const float *trace, int max_steps, float start, float frac)
{
    float final_v = trace[max_steps - 1];
    float target = start + frac * (final_v - start);
    for (int i = 0; i < max_steps; i++) {
        if ((final_v >= start && trace[i] >= target) ||
            (final_v < start && trace[i] <= target)) {
            return i;
        }
    }
    return max_steps;
}

/* The coupled (E,L) pair has two time constants -- a fast one set by
 * C_e/(G_el+G_ea) and a much slower one set by the overall capacity vs.
 * ambient loss (C_e+C_l)/(G_ea+G_la), roughly 2750s at this file's fixture
 * constants. rise_steps must run long enough for the LOAD trace itself to
 * approach its true asymptote, or the normalized-rise-time metric below
 * measures against a moving target instead of the real steady state. */
#define RISE_STEPS 15000

static void test_sensor_leads_load_near_element(void)
{
    TEST_SECTION("three-node: sensor_bias_p=0.8333 leads load on rise, falls faster on cutout");

    sim_plant_cfg_t cfg = three_node_cfg(5.0f / 6.0f); /* "5x closer to elements", sec 2.1 */
    const int rise_steps = RISE_STEPS;
    static float sensor_rise[RISE_STEPS], load_rise[RISE_STEPS];
    run_step_response(&cfg, 1.0f, rise_steps, sensor_rise, load_rise);

    /* Sensor leads the load throughout the rise: sensor_c - load_c > 0 at
     * every sample past the first couple of ticks (allow a brief settle-in
     * of the discretisation at t=0..1). */
    bool leads_throughout = true;
    for (int i = 2; i < rise_steps; i++) {
        if (sensor_rise[i] <= load_rise[i]) { leads_throughout = false; break; }
    }
    TEST_CHECK(leads_throughout, "sensor must lead (read hotter than) the load throughout the rise");

    int sensor_t63 = rise_time_steps(sensor_rise, rise_steps, cfg.ambient_c, 0.63f);
    int load_t63 = rise_time_steps(load_rise, rise_steps, cfg.ambient_c, 0.63f);
    TEST_CHECK(sensor_t63 * 3 <= load_t63,
               "sensor's 63% rise time must be at least 3x shorter than the load's");
    printf("  sensor_t63=%ds load_t63=%ds (ratio %.2fx)\n", sensor_t63, load_t63,
           (double)load_t63 / (sensor_t63 > 0 ? sensor_t63 : 1));

    /* Cut-out: start both nodes from the settled rise endpoint, drop duty to
     * 0, and compare the drop over the first 60s. */
    sim_plant_state_t st;
    sim_plant_reset(&st, &cfg);
    for (int i = 0; i < rise_steps; i++) {
        sim_plant_three_node_step(&st, &cfg, 1.0f, DT_S);
    }
    float sensor0 = st.sensor_c;
    float load0 = st.load_c;
    for (int i = 0; i < 60; i++) {
        sim_plant_three_node_step(&st, &cfg, 0.0f, DT_S);
    }
    float sensor_drop = sensor0 - st.sensor_c;
    float load_drop = load0 - st.load_c;
    TEST_CHECK(sensor_drop >= 5.0f * load_drop,
               "sensor must fall at least 5x faster than the load in the first 60s after cutout");
    printf("  60s after cutout: sensor_drop=%.4fC load_drop=%.4fC (ratio %.2fx)\n",
           (double)sensor_drop, (double)load_drop, (double)(sensor_drop / (load_drop > 1e-6f ? load_drop : 1e-6f)));
}

static void test_sensor_lags_load_centre_mounted(void)
{
    TEST_SECTION("three-node: sensor_bias_p=0.0 (centre-mounted) lags the load");

    sim_plant_cfg_t cfg = three_node_cfg(0.0f);
    const int rise_steps = RISE_STEPS;
    static float sensor_rise[RISE_STEPS], load_rise[RISE_STEPS];
    run_step_response(&cfg, 1.0f, rise_steps, sensor_rise, load_rise);

    /* All of the sensor's conductance goes to the load (g_se=0), so the
     * sensor is exactly a first-order lag on load_c and must read AT OR
     * BELOW it throughout a rise (never lead). */
    bool never_leads = true;
    for (int i = 2; i < rise_steps; i++) {
        if (sensor_rise[i] > load_rise[i] + 1e-4f) { never_leads = false; break; }
    }
    TEST_CHECK(never_leads, "centre-mounted sensor must never read hotter than the load during a rise");

    int sensor_t63 = rise_time_steps(sensor_rise, rise_steps, cfg.ambient_c, 0.63f);
    int load_t63 = rise_time_steps(load_rise, rise_steps, cfg.ambient_c, 0.63f);
    TEST_CHECK(sensor_t63 >= load_t63,
               "centre-mounted sensor's 63% rise time must be at least as slow as the load's own (a lag, not a lead)");
    printf("  sensor_t63=%ds load_t63=%ds\n", sensor_t63, load_t63);
}

static void test_legacy_path_untouched(void)
{
    TEST_SECTION("three-node fields are inert under SIM_NODE_LEGACY (sim_plant_step unchanged)");

    /* Same cfg shape test_sim_kiln.c's base_plant() uses, so this is
     * comparing against a known-stable legacy trajectory shape, not a
     * fixture invented just for this file. node_model defaults to
     * SIM_NODE_LEGACY (0) via the {0}-initialisation below; the three-node
     * fields are left non-zero-but-nonsense on purpose, to prove
     * sim_plant_step() never reads them. */
    sim_plant_cfg_t cfg = {0};
    cfg.ambient_c = 20.0f;
    cfg.thermal_mass_j_per_c = 50000.0f;
    cfg.heater_power_w = 2000.0f;
    cfg.loss_coeff_w_per_c = 5.0f;
    cfg.sensor_delay_s = 5.0f;
    cfg.sensor_lag_tau_s = 8.0f;
    /* node_model == SIM_NODE_LEGACY (0) implicitly. Poison the three-node
     * fields with values that would visibly perturb the trajectory if
     * anything in sim_plant_step() accidentally read them. */
    cfg.c_e_j_per_c = 1.0f;
    cfg.c_l_j_per_c = 1.0f;
    cfg.c_s_j_per_c = 1.0f;
    cfg.g_el_w_per_c = 999.0f;
    cfg.g_ea_w_per_c = 999.0f;
    cfg.g_la_w_per_c = 999.0f;
    cfg.sensor_tau_s = 0.001f;
    cfg.sensor_bias_p = 1.0f;
    cfg.load_mass_mult = 1.0f;

    sim_plant_state_t st_poisoned, st_clean;
    sim_plant_reset(&st_poisoned, &cfg);

    sim_plant_cfg_t clean = cfg;
    clean.c_e_j_per_c = 0.0f;
    clean.c_l_j_per_c = 0.0f;
    clean.c_s_j_per_c = 0.0f;
    clean.g_el_w_per_c = 0.0f;
    clean.g_ea_w_per_c = 0.0f;
    clean.g_la_w_per_c = 0.0f;
    clean.sensor_tau_s = 0.0f;
    clean.sensor_bias_p = 0.0f;
    clean.load_mass_mult = 0.0f;
    sim_plant_reset(&st_clean, &clean);

    for (int i = 0; i < 500; i++) {
        sim_plant_step(&st_poisoned, &cfg, 0.5f, DT_S);
        sim_plant_step(&st_clean, &clean, 0.5f, DT_S);
    }
    TEST_CHECK(st_poisoned.element_c == st_clean.element_c,
               "sim_plant_step() must be bit-identical regardless of the (unused) three-node fields");
    TEST_CHECK(st_poisoned.sensor_c == st_clean.sensor_c,
               "sim_plant_step() sensor_c must be bit-identical regardless of the (unused) three-node fields");
}

/* ------------------------------------------------------------------------
 * WI-1's decomposition helper (docs/audits/three_node_decomposition_helper_2026-09-14.md).
 * Verification method: sim_plant_decompose_three_node() picks C so the
 * DOMINANT (slow) pole of the (E,L) linear system lands exactly on the
 * target tau -- an algebraic fact, not an approximation -- so it is
 * verified here by forward-simulating the resulting plant's LOAD node
 * step response and reading the dominant mode back out of the TAIL of
 * the trajectory (well past the fast pole's own decay), rather than by a
 * fixed-fraction rise-time metric that the fast pole would contaminate at
 * the smaller Bi-separation ratios this design deliberately covers (see
 * the doc's pole-separation table). K is read from the trajectory's own
 * converged endpoint (duty is a constant 1.0 for the whole run, so
 * steady state is reached well inside the 8*tau horizon used below). */
#define DECOMP_HORIZON_TAU 8.0f
#define DECOMP_STEPS 16000

/* Runs the decomposed plant's LOAD-node step response (E,L only; sensor
 * fields are left zeroed -- out of scope for this helper, see sim_plant.h)
 * for DECOMP_STEPS ticks spanning DECOMP_HORIZON_TAU*tau_s, and reads the
 * dominant mode back out of two tail samples. Returns false if the
 * decompose call itself refused. */
static bool decompose_measure(float k, float tau_s, float bi, float phi,
                               float *k_meas, float *tau_meas)
{
    sim_plant_decompose_req_t req = { .k = k, .tau_s = tau_s, .bi = bi, .phi = phi };
    sim_plant_cfg_t cfg = {0};
    if (!sim_plant_decompose_three_node(&req, &cfg)) return false;

    cfg.node_model = SIM_NODE_THREE;
    cfg.ambient_c = 0.0f;   /* theta == absolute value here; simplifies residual math below */
    cfg.load_mass_mult = 1.0f;
    /* Sensor fields deliberately left at 0 -- out of scope, see sim_plant.h;
     * g_s == 0 so the sensor node never diverges and is simply unread here. */

    TEST_CHECK(cfg.heater_power_w > 0.0f && cfg.c_e_j_per_c > 0.0f && cfg.c_l_j_per_c > 0.0f &&
               cfg.g_el_w_per_c > 0.0f && cfg.g_ea_w_per_c >= 0.0f && cfg.g_la_w_per_c >= 0.0f,
               "decompose must produce finite, strictly positive capacities/element conductance");

    float dt = tau_s / (DECOMP_STEPS / DECOMP_HORIZON_TAU); /* dt = tau/2000 */
    sim_plant_state_t st;
    sim_plant_reset(&st, &cfg);

    /* Three EQUALLY spaced tail samples (3*tau, 5*tau, 7*tau -- span 2*tau
     * each), fit to y(t) = A - B*exp(-t/tau) via the standard 3-point
     * exponential-fit identity below. Deliberately does NOT treat the
     * horizon endpoint as "the" steady state -- at only 1*tau past the last
     * sample, the slow mode alone still has ~37% of its own residual left,
     * which upstream (an earlier version of this test) turned into a ~13%
     * systematic tau bias by understating every residual by a near-constant
     * amount. Fitting A directly from three tail points removes that
     * requirement entirely; it needs no independent "already converged"
     * point at all, only that the FAST mode has decayed by t1 (checked by
     * the round-trip tolerance itself: any residual fast-mode contribution
     * left at t1 would need to fit an inconsistent decay ratio between the
     * two spans, showing up as a tau miss). */
    int idx_t1 = (int)((3.0f * tau_s) / dt);
    int idx_t2 = (int)((5.0f * tau_s) / dt);
    int idx_t3 = (int)((7.0f * tau_s) / dt);
    float y1 = 0.0f, y2 = 0.0f, y3 = 0.0f;
    for (int i = 0; i < DECOMP_STEPS; i++) {
        sim_plant_three_node_step(&st, &cfg, 1.0f, dt);
        if (i == idx_t1) y1 = st.load_c;
        if (i == idx_t2) y2 = st.load_c;
        if (i == idx_t3) y3 = st.load_c;
    }

    /* y(t)=A-B*exp(-t/tau), samples evenly spaced by span=2*tau:
     *   (y3-y2)/(y2-y1) = exp(-span/tau)  ->  tau = span/ln((y2-y1)/(y3-y2))
     *   A = y1 + (y2-y1)/(1 - exp(-span/tau))                              */
    float d1 = y2 - y1;
    float d2 = y3 - y2;
    TEST_CHECK(d1 > 0.0f && d2 > 0.0f && d1 > d2,
               "tail increments must be positive and shrinking (monotone approach to steady state)");
    if (!(d1 > 0.0f && d2 > 0.0f && d1 > d2)) { *k_meas = -1.0f; *tau_meas = -1.0f; return true; }

    float span = (float)(idx_t2 - idx_t1) * dt;
    *tau_meas = span / logf(d1 / d2);
    float r = expf(-span / *tau_meas); /* == d2/d1, recomputed for clarity */
    *k_meas = y1 + d1 / (1.0f - r);
    return true;
}

static void check_decompose_roundtrip(float k, float tau_s, float bi, float phi,
                                       float tol_k_frac, float tol_tau_frac, const char *label)
{
    float k_meas = 0.0f, tau_meas = 0.0f;
    bool ok = decompose_measure(k, tau_s, bi, phi, &k_meas, &tau_meas);
    TEST_CHECK(ok, "decompose must succeed for a physical (Bi,phi)");
    if (!ok) return;

    float k_err = fabsf(k_meas - k) / k;
    float tau_err = fabsf(tau_meas - tau_s) / tau_s;
    printf("  %s: Bi=%.3f phi=%.3f  k target=%.4f measured=%.4f (err %.3f%%)  tau target=%.2f measured=%.2f (err %.3f%%)\n",
           label, (double)bi, (double)phi, (double)k, (double)k_meas, (double)(k_err * 100.0f),
           (double)tau_s, (double)tau_meas, (double)(tau_err * 100.0f));
    TEST_CHECK(k_err <= tol_k_frac, "round-trip K must match target within tolerance");
    TEST_CHECK(tau_err <= tol_tau_frac, "round-trip tau must match target within tolerance");
}

/* Round-trip at the bench anchor (z0: k=42.731, tau=255.6, per the factorial
 * design doc) across the corners of the (Bi,phi) design space, and at a
 * kiln-scale anchor (order-of-magnitude derived from plant_sim.py's PHYS_*
 * constants -- P_max=2500W/zone, a linear (non-radiative) steady-state
 * conductance from PHYS_WALL_R_K_PER_W + 1/(PHYS_OUTER_H_W_PER_M2K*A), and
 * PHYS_THERMAL_MASS_J_PER_K -- giving k~=4480, tau~=90200s; TEST FIXTURE,
 * not a literal port of plant_sim.py's nonlinear model, only its magnitude).
 * Tolerance: 1% on K (an exact algebraic DC-gain match; the only error
 * source is forward-Euler discretization at dt=tau/2000) and 3% on tau (the
 * tail-fit is exact in the continuous-time limit; 3% covers discretization
 * plus the residual fast-mode contamination still present at 4*tau even at
 * this design's least-separated pole ratios, see decompose_measure()'s
 * comment). */
static void test_decompose_roundtrip(void)
{
    TEST_SECTION("decompose_three_node: round-trip (K,tau) at bench and kiln scale");

    const float bench_k = 42.731f, bench_tau = 255.6f;
    const float kiln_k = 4480.0f, kiln_tau = 90200.0f;
    const float bi_lo = 0.3f, bi_hi = 1.5f;    /* design doc's A8 levels */
    const float phi_lo = 0.25f, phi_mid = 0.5f, phi_hi = 0.75f; /* A7 levels + anchor */

    check_decompose_roundtrip(bench_k, bench_tau, bi_lo, phi_lo, 0.01f, 0.03f, "bench Bi=lo phi=lo");
    check_decompose_roundtrip(bench_k, bench_tau, bi_lo, phi_hi, 0.01f, 0.03f, "bench Bi=lo phi=hi");
    check_decompose_roundtrip(bench_k, bench_tau, bi_hi, phi_lo, 0.01f, 0.03f, "bench Bi=hi phi=lo");
    check_decompose_roundtrip(bench_k, bench_tau, bi_hi, phi_hi, 0.01f, 0.03f, "bench Bi=hi phi=hi");
    check_decompose_roundtrip(bench_k, bench_tau, bi_hi, phi_mid, 0.01f, 0.03f, "bench Bi=hi phi=mid (anchor)");

    check_decompose_roundtrip(kiln_k, kiln_tau, bi_lo, phi_lo, 0.01f, 0.03f, "kiln Bi=lo phi=lo");
    check_decompose_roundtrip(kiln_k, kiln_tau, bi_hi, phi_hi, 0.01f, 0.03f, "kiln Bi=hi phi=hi");
}

/* The property the factorial actually depends on: two DIFFERENT (Bi,phi)
 * decompositions of the SAME (k,tau) must produce plants whose aggregate
 * step responses agree, even though their internal (E,L) split and
 * fast/slow amplitude weighting differ. Compared in the tail (t >= 4*tau),
 * past the fast mode -- the two decompositions' fast poles sit at different
 * absolute rates, so an early-time comparison would correctly show them
 * disagreeing and that is not a defect. */
static void test_decompose_invariance(void)
{
    TEST_SECTION("decompose_three_node: two (Bi,phi) decompositions of the same (K,tau) agree in the tail");

    const float k = 42.731f, tau_s = 255.6f;
    float k1, tau1, k2, tau2;
    bool ok1 = decompose_measure(k, tau_s, 0.3f, 0.25f, &k1, &tau1);
    bool ok2 = decompose_measure(k, tau_s, 1.5f, 0.75f, &k2, &tau2);
    TEST_CHECK(ok1 && ok2, "both decompositions must succeed");
    if (!ok1 || !ok2) return;

    printf("  decomp A (Bi=0.3,phi=0.25): k=%.4f tau=%.2f | decomp B (Bi=1.5,phi=0.75): k=%.4f tau=%.2f\n",
           (double)k1, (double)tau1, (double)k2, (double)tau2);
    TEST_CHECK(fabsf(k1 - k2) / k <= 0.01f,
               "two decompositions of the same target must agree on measured K within 1%");
    TEST_CHECK(fabsf(tau1 - tau2) / tau_s <= 0.03f,
               "two decompositions of the same target must agree on measured tau within 3%");
}

/* Degenerate/non-physical inputs must be REFUSED (false, *out untouched),
 * never silently clamped -- a clamped cell would look valid and quietly
 * test something other than what it claims (task requirement). */
static void test_decompose_refuses_degenerate_inputs(void)
{
    TEST_SECTION("decompose_three_node: degenerate inputs are refused, not clamped");

    sim_plant_cfg_t out;
    sim_plant_decompose_req_t req;

    req = (sim_plant_decompose_req_t){ .k = 42.731f, .tau_s = 255.6f, .bi = 0.0f, .phi = 0.5f };
    TEST_CHECK(!sim_plant_decompose_three_node(&req, &out), "Bi == 0 must be refused (g_el would be infinite)");

    req = (sim_plant_decompose_req_t){ .k = 42.731f, .tau_s = 255.6f, .bi = -1.0f, .phi = 0.5f };
    TEST_CHECK(!sim_plant_decompose_three_node(&req, &out), "Bi < 0 must be refused");

    req = (sim_plant_decompose_req_t){ .k = 42.731f, .tau_s = 255.6f, .bi = 1.0f, .phi = -0.1f };
    TEST_CHECK(!sim_plant_decompose_three_node(&req, &out), "phi < 0 must be refused");

    req = (sim_plant_decompose_req_t){ .k = 42.731f, .tau_s = 255.6f, .bi = 1.0f, .phi = 1.1f };
    TEST_CHECK(!sim_plant_decompose_three_node(&req, &out), "phi > 1 must be refused");

    req = (sim_plant_decompose_req_t){ .k = 0.0f, .tau_s = 255.6f, .bi = 1.0f, .phi = 0.5f };
    TEST_CHECK(!sim_plant_decompose_three_node(&req, &out), "k <= 0 must be refused");

    req = (sim_plant_decompose_req_t){ .k = 42.731f, .tau_s = 0.0f, .bi = 1.0f, .phi = 0.5f };
    TEST_CHECK(!sim_plant_decompose_three_node(&req, &out), "tau <= 0 must be refused");

    /* phi at the exact boundaries (0, 1) IS physical (all loss on one node)
     * and must succeed -- only outside [0,1] is refused. */
    req = (sim_plant_decompose_req_t){ .k = 42.731f, .tau_s = 255.6f, .bi = 1.0f, .phi = 0.0f };
    TEST_CHECK(sim_plant_decompose_three_node(&req, &out), "phi == 0.0 (boundary) must succeed, not be refused");
    req = (sim_plant_decompose_req_t){ .k = 42.731f, .tau_s = 255.6f, .bi = 1.0f, .phi = 1.0f };
    TEST_CHECK(sim_plant_decompose_three_node(&req, &out), "phi == 1.0 (boundary) must succeed, not be refused");
}

/* ---- sim_kiln_step() wiring (credibility-gate dwell-peak work, 2026-10-07) ----
 * A SIM_NODE_THREE zone inside sim_kiln_step() must (a) with zero coupling
 * and no radiation reproduce sim_plant_three_node_step() bit-for-bit,
 * (b) read its thermocouple off the sensor node, (c) take neighbour-duty
 * coupling into its ELEMENT node and (d) leave a SIM_NODE_LEGACY neighbour
 * on the unchanged one-node path. */
static void test_kiln_step_three_node_wiring(void)
{
    sim_kiln_cfg_t kcfg = {0};
    kcfg.zone_count = 2;
    kcfg.zone[0].plant = three_node_cfg(0.8333f);
    kcfg.zone[1].plant = three_node_cfg(0.8333f);
    sim_kiln_state_t ks;
    sim_kiln_reset(&ks, &kcfg);

    sim_plant_cfg_t solo_cfg = three_node_cfg(0.8333f);
    sim_plant_state_t solo;
    sim_plant_reset(&solo, &solo_cfg);

    bool identical = true;
    for (int i = 0; i < 2000; i++) {
        float duty[2] = { (i / 100) % 2 ? 0.0f : 1.0f, 0.0f };
        sim_kiln_step(&ks, &kcfg, duty, DT_S);
        sim_plant_three_node_step(&solo, &solo_cfg, duty[0], DT_S);
        if (ks.zone[0].element_c != solo.element_c || ks.zone[0].load_c != solo.load_c ||
            ks.zone[0].sensor_node_c != solo.sensor_node_c || ks.zone[0].sensor_c != solo.sensor_c) {
            identical = false;
            break;
        }
    }
    TEST_CHECK(identical, "uncoupled three-node zone in sim_kiln_step must match sim_plant_three_node_step bit-for-bit");
    TEST_CHECK(ks.zone[1].load_c == 20.0f && ks.zone[1].element_c == 20.0f,
               "an undriven, uncoupled three-node zone must stay at ambient");
    TEST_CHECK(ks.zone[0].sensor_c != ks.zone[0].element_c, "reading must come from the sensor node, not the element");

    /* (c) neighbour-duty coupling lands in the element node. */
    kcfg.coupling_w_per_c[1][0] = 50.0f;
    sim_kiln_reset(&ks, &kcfg);
    for (int i = 0; i < 200; i++) {
        float duty[2] = { 1.0f, 0.0f };
        sim_kiln_step(&ks, &kcfg, duty, DT_S);
    }
    TEST_CHECK(ks.zone[1].element_c > 20.5f, "neighbour duty must heat a three-node zone's element via coupling");
    TEST_CHECK(ks.zone[1].element_c > ks.zone[1].load_c, "coupling heat enters the element first, then the load");

    /* (d) a legacy neighbour is untouched by the three-node branch. */
    sim_kiln_cfg_t lcfg = {0};
    lcfg.zone_count = 2;
    for (int z = 0; z < 2; z++) {
        lcfg.zone[z].plant.ambient_c = 20.0f;
        lcfg.zone[z].plant.heater_power_w = 40.0f;
        lcfg.zone[z].plant.thermal_mass_j_per_c = 200.0f;
        lcfg.zone[z].plant.loss_coeff_w_per_c = 1.0f;
    }
    lcfg.zone[1].plant = kcfg.zone[0].plant; /* mixed: zone 0 legacy, zone 1 three-node */
    lcfg.coupling_w_per_c[0][1] = 5.0f;
    sim_kiln_state_t lks;
    sim_kiln_reset(&lks, &lcfg);
    sim_plant_cfg_t lone = lcfg.zone[0].plant;
    sim_plant_state_t lone_s;
    sim_plant_reset(&lone_s, &lone);
    bool legacy_ok = true;
    for (int i = 0; i < 500; i++) {
        float duty[2] = { 0.5f, 0.0f };
        sim_kiln_step(&lks, &lcfg, duty, DT_S);
        sim_plant_step(&lone_s, &lone, duty[0], DT_S);
        if (lks.zone[0].element_c != lone_s.element_c) { legacy_ok = false; break; }
    }
    TEST_CHECK(legacy_ok, "legacy zone beside a three-node zone must follow the unchanged one-node path");
}

void run_test_sim_plant_three_node(void)
{
    /* Catches this TU being linked against a stale/mismatched sim_plant.o --
     * see sim_plant.h's SIM_PLANT_ASSERT_ABI_FRESH() comment. */
    SIM_PLANT_ASSERT_ABI_FRESH();
    test_sensor_leads_load_near_element();
    test_sensor_lags_load_centre_mounted();
    test_legacy_path_untouched();
    test_decompose_roundtrip();
    test_decompose_invariance();
    test_decompose_refuses_degenerate_inputs();
    test_kiln_step_three_node_wiring();
}
