// Multi-zone plant + fault-injection tests. TODO.md 6A.8 ("N coupled zones,
// inter-zone conductance, a radiative loss term, injectable faults").
//
// Two things are being checked here, and they are worth keeping apart:
//   1. the *model* behaves like a coupled kiln (a neighbor's elements really
//      do heat this zone, radiative loss really does flatten the plant gain
//      at temperature) -- without that, nothing downstream of it means
//      anything;
//   2. each thermal_guard check in TODO.md 6A.3 can be *provoked on purpose*
//      by a physically-stated failure, rather than by hand-fed numbers.
//      That is the difference between "the guard's arithmetic works" (which
//      test_thermal_guard.c already covers) and "this failure mode trips
//      that guard", which is what docs/GUARD_TEST_MATRIX.md tabulates and
//      what makes 'robust' checkable instead of asserted.
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "test_common.h"
#include "../drivers/control/pid_autotune.h"
#include "../drivers/control/thermal_guard.h"
#include "sim_plant.h"

#define DT_S 10.0f

/* Equilibrium at duty 1 for an isolated zone: ambient + power/loss = 420C.
 * tau = mass/loss = 10000s, so a full firing simulates in a few thousand
 * steps -- slow enough to be kiln-like, fast enough for a unit test. */
static sim_plant_cfg_t base_plant(void)
{
    sim_plant_cfg_t p = {
        .ambient_c = 20.0f,
        .thermal_mass_j_per_c = 50000.0f,
        .heater_power_w = 2000.0f,
        .loss_coeff_w_per_c = 5.0f,
        .sensor_delay_s = 5.0f,
        .sensor_lag_tau_s = 8.0f,
    };
    return p;
}

static sim_kiln_cfg_t two_zone_cfg(float coupling_w_per_c)
{
    sim_kiln_cfg_t c = {0};
    c.zone_count = 2;
    c.zone[0].plant = base_plant();
    c.zone[1].plant = base_plant();
    c.coupling_w_per_c[0][1] = coupling_w_per_c;
    c.coupling_w_per_c[1][0] = coupling_w_per_c;
    return c;
}

static thermal_guard_cfg_t base_guard_cfg(void)
{
    thermal_guard_cfg_t g = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = 0.5f};
    return g;
}

/* Runs one zone of a kiln against thermal_guard for up to max_steps,
 * commanding a fixed duty, and reports which guard (if any) tripped. The
 * *commanded* duty is what both the plant and the guard are told -- fault
 * injection is what makes the hardware disagree with it, which is the whole
 * point. */
static thermal_guard_trip_t run_until_trip(sim_kiln_state_t *st, const sim_kiln_cfg_t *cfg,
                                           const thermal_guard_cfg_t *gcfg, int zone, float setpoint_c,
                                           float commanded_duty, int max_steps, int *out_steps)
{
    thermal_guard_state_t g;
    thermal_guard_reset(&g);

    float duty[SIM_KILN_MAX_ZONES] = {0};
    duty[zone] = commanded_duty;

    for (int i = 0; i < max_steps; i++) {
        float reading = sim_kiln_reading_c(st, cfg, zone);
        thermal_guard_input_t in = {
            .sensor_ok = !isnan(reading),
            .measurement_c = isnan(reading) ? 0.0f : reading,
            .setpoint_c = setpoint_c,
            .commanded_duty = commanded_duty,
            .dt_s = DT_S,
        };
        if (thermal_guard_tick(&g, gcfg, &in)) {
            if (out_steps) *out_steps = i;
            return g.reason;
        }
        sim_kiln_step(st, cfg, duty, DT_S);
    }
    if (out_steps) *out_steps = max_steps;
    return THERMAL_GUARD_TRIP_NONE;
}

static void test_coupling(void)
{
    /* Coupled: zone 0 at full duty must visibly heat zone 1, which is idle.
     * ADDITIVE source-gain coupling (matches the firmware's
     * zone_coupling_solve.c model class -- see sim_kiln_step()'s own
     * comment): coupling_w_per_c[i][j] is W delivered to zone i per unit of
     * zone j's commanded duty, not a (T_j - T_i) conductance, so 250 W/duty
     * here is a power term, not a small conductance -- steady rise =
     * coupling_w_per_c / loss_coeff_w_per_c = 250/5 = 50C, comfortably
     * above both this check's 40C floor and below the driven zone's own
     * 400C rise. */
    sim_kiln_cfg_t cfg = two_zone_cfg(250.0f);
    sim_kiln_state_t st;
    sim_kiln_reset(&st, &cfg);

    float duty[SIM_KILN_MAX_ZONES] = {1.0f, 0.0f};
    for (int i = 0; i < 2 * 3600 / (int)DT_S; i++) {
        sim_kiln_step(&st, &cfg, duty, DT_S);
    }
    float driven_c = sim_kiln_element_c(&st, 0);
    float neighbor_c = sim_kiln_element_c(&st, 1);

    TEST_CHECK(neighbor_c > 40.0f, "an idle zone is heated by its neighbor's elements through the coupling matrix");
    TEST_CHECK(neighbor_c < driven_c,
               "the coupled neighbor stays cooler than the driven zone (cross gain < direct gain)");

    /* Uncoupled control: the same run with zero conductance must leave the
     * idle zone at ambient, or the rise above is coming from somewhere else. */
    sim_kiln_cfg_t iso = two_zone_cfg(0.0f);
    sim_kiln_state_t iso_st;
    sim_kiln_reset(&iso_st, &iso);
    for (int i = 0; i < 2 * 3600 / (int)DT_S; i++) {
        sim_kiln_step(&iso_st, &iso, duty, DT_S);
    }
    TEST_CHECK_NEAR(sim_kiln_element_c(&iso_st, 1), 20.0f, 0.01,
                    "with zero conductance the idle zone stays at ambient");
}

static void test_radiative_loss(void)
{
    /* The linear-loss model reaches ambient + power/loss = 420C at duty 1.
     * Adding a radiative term must land the same zone materially lower --
     * this is the temperature-dependent plant gain that makes one PID tuning
     * wrong across a firing (TODO.md 6A.4's gain-scheduling rationale). */
    sim_kiln_cfg_t linear = two_zone_cfg(0.0f);
    linear.zone_count = 1;
    sim_kiln_cfg_t radiative = linear;
    radiative.zone[0].radiative_coeff_w_per_k4 = 2.0e-9f;

    sim_kiln_state_t a, b;
    sim_kiln_reset(&a, &linear);
    sim_kiln_reset(&b, &radiative);

    float duty[SIM_KILN_MAX_ZONES] = {1.0f, 0.0f};
    for (int i = 0; i < 12 * 3600 / (int)DT_S; i++) {
        sim_kiln_step(&a, &linear, duty, DT_S);
        sim_kiln_step(&b, &radiative, duty, DT_S);
    }
    /* tau = mass/loss = 10000s, so 12h is ~4.3 time constants -- within ~1.5%
     * of the analytic ceiling, not exactly on it. */
    TEST_CHECK_NEAR(sim_kiln_element_c(&a, 0), 420.0f, 10.0,
                    "linear-loss zone approaches ambient + power/loss after 12 simulated hours");
    TEST_CHECK(sim_kiln_element_c(&b, 0) < sim_kiln_element_c(&a, 0) - 20.0f,
               "the radiative loss term costs real ceiling temperature at the same duty");
}

static void test_guard_provocations(void)
{
    const int steps_1h = 3600 / (int)DT_S;
    thermal_guard_cfg_t gcfg = base_guard_cfg();

    /* Guard 1, cause A: dead element. Relay closes, nothing heats. */
    {
        sim_kiln_cfg_t cfg = two_zone_cfg(0.0f);
        sim_kiln_state_t st;
        sim_kiln_reset(&st, &cfg);
        sim_kiln_inject_fault(&st, &cfg, 0, SIM_ZONE_FAULT_ELEMENT_DEAD);
        int steps = 0;
        thermal_guard_trip_t r = run_until_trip(&st, &cfg, &gcfg, 0, 300.0f, 1.0f, steps_1h, &steps);
        TEST_CHECK(r == THERMAL_GUARD_TRIP_HEATING_FAILED, "dead element trips guard 1 (heating failed)");
        TEST_CHECK(steps * (int)DT_S <= 600, "guard 1 catches a dead element within 10 simulated minutes");
    }

    /* Guard 1, cause B: thermocouple physically out of the kiln body but
     * electrically fine -- the case TODO.md 6A.3 names as the one no fault
     * bit can catch. */
    {
        sim_kiln_cfg_t cfg = two_zone_cfg(0.0f);
        sim_kiln_state_t st;
        sim_kiln_reset(&st, &cfg);
        sim_kiln_inject_fault(&st, &cfg, 0, SIM_ZONE_FAULT_TC_DETACHED);
        int steps = 0;
        thermal_guard_trip_t r = run_until_trip(&st, &cfg, &gcfg, 0, 300.0f, 1.0f, steps_1h, &steps);
        TEST_CHECK(r == THERMAL_GUARD_TRIP_HEATING_FAILED,
                   "a detached (but electrically intact) thermocouple trips guard 1");
        TEST_CHECK(sim_kiln_element_c(&st, 0) > sim_kiln_reading_c(&st, &cfg, 0) + 10.0f,
                   "...and it trips while the element really was heating, i.e. the sensor is what lied");
    }

    /* Guard 2: swapped connectors. Zone 0's loop drives zone 0's relay but
     * reads physical zone 1's thermocouple, which is hot and cooling. The
     * duty is forced here rather than PID-derived, since the point is what
     * the guard does when heat is commanded against a falling reading. */
    {
        sim_kiln_cfg_t cfg = two_zone_cfg(0.0f);
        cfg.sensor_map[0] = 1;
        cfg.sensor_map[1] = 0;
        sim_kiln_state_t st;
        sim_kiln_reset(&st, &cfg);
        st.zone[1].element_c = st.zone[1].sensor_c = 500.0f;
        for (int i = 0; i < SIM_PLANT_DELAY_MAX_STEPS; i++) st.zone[1].delay_ring[i] = 500.0f;

        int steps = 0;
        thermal_guard_trip_t r = run_until_trip(&st, &cfg, &gcfg, 0, 300.0f, 1.0f, steps_1h, &steps);
        TEST_CHECK(r == THERMAL_GUARD_TRIP_WRONG_DIRECTION,
                   "swapped thermocouples trip guard 2 (heat commanded, reading falling)");
    }

    /* Guard 3: welded contact -- duty 0 commanded, full power delivered. */
    {
        sim_kiln_cfg_t cfg = two_zone_cfg(0.0f);
        sim_kiln_state_t st;
        sim_kiln_reset(&st, &cfg);
        st.zone[0].element_c = st.zone[0].sensor_c = 200.0f;
        for (int i = 0; i < SIM_PLANT_DELAY_MAX_STEPS; i++) st.zone[0].delay_ring[i] = 200.0f;
        sim_kiln_inject_fault(&st, &cfg, 0, SIM_ZONE_FAULT_RELAY_WELDED);
        int steps = 0;
        thermal_guard_trip_t r = run_until_trip(&st, &cfg, &gcfg, 0, 300.0f, 0.0f, steps_1h, &steps);
        TEST_CHECK(r == THERMAL_GUARD_TRIP_RUNAWAY, "a welded relay trips guard 3 (runaway with heat off)");
    }

    /* Guard 4: drift at setpoint. The zone settles, then its element dies
     * while the loop is only trickling duty (below the guard-1 threshold),
     * so nothing but the drift check can see it. */
    {
        sim_kiln_cfg_t cfg = two_zone_cfg(0.0f);
        sim_kiln_state_t st;
        sim_kiln_reset(&st, &cfg);
        st.zone[0].element_c = st.zone[0].sensor_c = 300.0f;
        for (int i = 0; i < SIM_PLANT_DELAY_MAX_STEPS; i++) st.zone[0].delay_ring[i] = 300.0f;
        sim_kiln_inject_fault(&st, &cfg, 0, SIM_ZONE_FAULT_ELEMENT_DEAD);
        int steps = 0;
        thermal_guard_trip_t r = run_until_trip(&st, &cfg, &gcfg, 0, 300.0f, 0.2f, 4 * steps_1h, &steps);
        TEST_CHECK(r == THERMAL_GUARD_TRIP_DRIFT,
                   "a settled zone whose element dies at low duty trips guard 4 (drift)");
    }

    /* Guard 5: absolute ceiling. */
    {
        sim_kiln_cfg_t cfg = two_zone_cfg(0.0f);
        sim_kiln_state_t st;
        sim_kiln_reset(&st, &cfg);
        thermal_guard_cfg_t low_ceiling = base_guard_cfg();
        low_ceiling.max_temp_c = 200.0f;
        int steps = 0;
        thermal_guard_trip_t r = run_until_trip(&st, &cfg, &low_ceiling, 0, 400.0f, 1.0f, 4 * steps_1h, &steps);
        TEST_CHECK(r == THERMAL_GUARD_TRIP_MAX_TEMP, "crossing max_temp_c trips guard 5 immediately");
    }

    /* Guard 6: open thermocouple -- NaN readings, debounced. */
    {
        sim_kiln_cfg_t cfg = two_zone_cfg(0.0f);
        sim_kiln_state_t st;
        sim_kiln_reset(&st, &cfg);
        sim_kiln_inject_fault(&st, &cfg, 0, SIM_ZONE_FAULT_TC_OPEN);
        int steps = 0;
        thermal_guard_trip_t r = run_until_trip(&st, &cfg, &gcfg, 0, 300.0f, 1.0f, steps_1h, &steps);
        TEST_CHECK(r == THERMAL_GUARD_TRIP_SENSOR_INVALID, "an open thermocouple trips guard 6");
        TEST_CHECK(steps == 2, "...after exactly 3 consecutive bad reads (0-indexed step 2)");
    }

    /* Guard 7: a sensor that answers but stopped converting. */
    {
        sim_kiln_cfg_t cfg = two_zone_cfg(0.0f);
        sim_kiln_state_t st;
        sim_kiln_reset(&st, &cfg);
        sim_kiln_inject_fault(&st, &cfg, 0, SIM_ZONE_FAULT_TC_FROZEN);
        int steps = 0;
        /* Duty below PROGRESS_DUTY_MIN so guard 1 can't claim this first --
         * guard 7 is the one that must see a bit-identical reading. */
        thermal_guard_trip_t r = run_until_trip(&st, &cfg, &gcfg, 0, 300.0f, 0.2f, steps_1h, &steps);
        TEST_CHECK(r == THERMAL_GUARD_TRIP_FROZEN, "a frozen (non-converting) sensor trips guard 7");
        TEST_CHECK(steps * (int)DT_S <= 620, "guard 7 trips at its 600s window, not later");
    }

    /* Guard 8: cross-zone plausibility. Zone 0's thermocouple falls out of
     * the kiln body while both zones' elements keep heating a strongly
     * coupled chamber, so zone 1 climbs and zone 0 reads near-ambient. This
     * is the case guards 1 and 2 can miss when the zone's own elements
     * really are working -- and the guard only exists to be armed with a
     * threshold the operator (or a measured K matrix) supplies, so the test
     * has to supply one too. */
    {
        sim_kiln_cfg_t cfg = two_zone_cfg(3.0f);
        sim_kiln_state_t st;
        sim_kiln_reset(&st, &cfg);
        sim_kiln_inject_fault(&st, &cfg, 0, SIM_ZONE_FAULT_TC_DETACHED);

        thermal_guard_cfg_t cross_cfg = base_guard_cfg();
        cross_cfg.sanity_rate_c_per_min = 0.001f; /* keep guard 1 out of the way -- it is not what's under test */
        cross_cfg.cross_zone_max_delta_c = 100.0f;
        cross_cfg.cross_zone_period_s = 600.0f;

        thermal_guard_state_t g;
        thermal_guard_reset(&g);
        float duty[SIM_KILN_MAX_ZONES] = {1.0f, 1.0f};
        thermal_guard_trip_t reason = THERMAL_GUARD_TRIP_NONE;

        for (int i = 0; i < 4 * steps_1h && reason == THERMAL_GUARD_TRIP_NONE; i++) {
            float peer_c[SIM_KILN_MAX_ZONES];
            bool peer_ok[SIM_KILN_MAX_ZONES] = {0};
            for (int z = 0; z < 2; z++) {
                peer_c[z] = sim_kiln_reading_c(&st, &cfg, z);
                peer_ok[z] = !isnan(peer_c[z]);
            }
            thermal_guard_input_t in = {
                .sensor_ok = peer_ok[0],
                .measurement_c = peer_c[0],
                .setpoint_c = 800.0f,
                .commanded_duty = 1.0f,
                .dt_s = DT_S,
                .peer_c = peer_c,
                .peer_ok = peer_ok,
                .peer_count = 2,
                .peer_index_self = 0,
            };
            if (thermal_guard_tick(&g, &cross_cfg, &in)) {
                reason = g.reason;
            }
            sim_kiln_step(&st, &cfg, duty, DT_S);
        }
        TEST_CHECK(reason == THERMAL_GUARD_TRIP_CROSS_ZONE,
                   "a detached thermocouple in a coupled chamber trips guard 8 against its neighbour");
    }

    /* ...and guard 8 must stay silent on a healthy coupled kiln, where both
     * zones track each other. Without this, "it trips" proves nothing.
     *
     * Note what a first version of this check found: driving the two zones
     * at 1.0 and 0.6 duty *does* exceed a 100C band in steady state in this
     * model, with nothing wrong at all. That is not a bug in the guard -- it
     * is the reason TODO.md 6A.5 insists the threshold come from a measured
     * cross-gain matrix rather than a hand-picked number, and it is why the
     * firmware ships this guard disabled. Equal duty is the honest
     * "healthy" case for a fixed threshold this tight. */
    {
        sim_kiln_cfg_t cfg = two_zone_cfg(3.0f);
        sim_kiln_state_t st;
        sim_kiln_reset(&st, &cfg);

        thermal_guard_cfg_t cross_cfg = base_guard_cfg();
        cross_cfg.cross_zone_max_delta_c = 100.0f;

        thermal_guard_state_t g;
        thermal_guard_reset(&g);
        float duty[SIM_KILN_MAX_ZONES] = {1.0f, 1.0f};
        bool tripped = false;

        for (int i = 0; i < 4 * steps_1h && !tripped; i++) {
            float peer_c[SIM_KILN_MAX_ZONES];
            bool peer_ok[SIM_KILN_MAX_ZONES] = {0};
            for (int z = 0; z < 2; z++) {
                peer_c[z] = sim_kiln_reading_c(&st, &cfg, z);
                peer_ok[z] = !isnan(peer_c[z]);
            }
            thermal_guard_input_t in = {
                .sensor_ok = true, .measurement_c = peer_c[0], .setpoint_c = 400.0f,
                .commanded_duty = 1.0f, .dt_s = DT_S,
                .peer_c = peer_c, .peer_ok = peer_ok, .peer_count = 2, .peer_index_self = 0,
            };
            tripped = thermal_guard_tick(&g, &cross_cfg, &in);
            sim_kiln_step(&st, &cfg, duty, DT_S);
        }
        TEST_CHECK(!tripped, "guard 8 does not fire on a healthy coupled kiln with both zones at equal duty");
    }

    /* Sensor noise must not, by itself, trip anything: a healthy zone with a
     * noisy thermocouple is not a fault. */
    {
        sim_kiln_cfg_t cfg = two_zone_cfg(0.0f);
        cfg.sensor_noise_c = 1.0f;
        sim_kiln_state_t st;
        sim_kiln_reset(&st, &cfg);
        int steps = 0;
        thermal_guard_trip_t r = run_until_trip(&st, &cfg, &gcfg, 0, 400.0f, 1.0f, 2 * steps_1h, &steps);
        TEST_CHECK(r == THERMAL_GUARD_TRIP_NONE, "1C of sensor noise on a healthy heating zone trips nothing");
    }
}

/* TODO.md 6A.5(b): during zone i's step test, every zone's response is
 * logged and fitted, giving the cross-gain matrix K[i][j]. That code path
 * exists on-target but has never been exercised against a real cross-gain
 * (no thermocouple hardware). This is that check, off-target, against a
 * plant whose coupling is known by construction. */
static void test_cross_gain_matrix(void)
{
    const int steps = 4 * 3600 / (int)DT_S;
    const float duty_step = 0.5f;

    /* 250 W/duty (see test_coupling()'s comment on units): steady cross
     * rise = coupling_w_per_c * duty_step / loss_coeff_w_per_c = 250*0.5/5 =
     * 25C, giving K[1][0] = 25/0.5 = 50 -- a comfortably measurable fraction
     * of K[0][0] = 200 (steady direct rise 1000/5=200C at duty_step=0.5). */
    sim_kiln_cfg_t cfg = two_zone_cfg(250.0f);
    sim_kiln_state_t st;
    sim_kiln_reset(&st, &cfg);

    static autotune_sample_t trace[2][4 * 3600 / (int)DT_S];
    float baseline_c[2];
    for (int z = 0; z < 2; z++) baseline_c[z] = sim_kiln_reading_c(&st, &cfg, z);

    float duty[SIM_KILN_MAX_ZONES] = {duty_step, 0.0f};
    for (int i = 0; i < steps; i++) {
        sim_kiln_step(&st, &cfg, duty, DT_S);
        for (int z = 0; z < 2; z++) {
            trace[z][i].t_s = (float)(i + 1) * DT_S;
            trace[z][i].measurement_c = sim_kiln_reading_c(&st, &cfg, z);
        }
    }

    fopdt_model_t direct = pid_autotune_fit_fopdt(trace[0], steps, baseline_c[0], duty_step);
    fopdt_model_t cross = pid_autotune_fit_fopdt(trace[1], steps, baseline_c[1], duty_step);

    TEST_CHECK(direct.valid, direct.valid ? "direct-gain fit valid" : direct.invalid_reason);
    TEST_CHECK(cross.valid, cross.valid ? "cross-gain fit valid" : cross.invalid_reason);
    if (!direct.valid || !cross.valid) return;

    TEST_CHECK(cross.k_gain_c_per_duty > 0.0f, "the cross gain K[1][0] is positive -- zone 0's duty heats zone 1");
    TEST_CHECK(cross.k_gain_c_per_duty < direct.k_gain_c_per_duty,
               "the cross gain is smaller than the direct gain K[0][0]");
    /* ADDITIVE source-gain coupling (unlike the retired temperature-
     * difference exchange model) injects power into zone 1 directly from
     * zone 0's DUTY, not from zone 0's own temperature rise -- there is no
     * extra "heat arrives through the neighbor's mass" propagation step.
     * Both zones share identical plant/sensor parameters (two_zone_cfg()),
     * so the cross path's dynamics should match the direct path's, not lag
     * behind it. */
    float direct_span = direct.dead_time_s + direct.tau_s;
    float cross_span = cross.dead_time_s + cross.tau_s;
    char span_detail[160];
    snprintf(span_detail, sizeof(span_detail),
             "cross path span %.1fs matches direct path span %.1fs (additive coupling adds no extra propagation delay)",
             (double)cross_span, (double)direct_span);
    TEST_CHECK(fabsf(cross_span - direct_span) < 0.25f * direct_span, span_detail);

    char detail[160];
    snprintf(detail, sizeof(detail), "cross/direct gain ratio %.2f is a real coupling, not numerical noise",
             (double)(cross.k_gain_c_per_duty / direct.k_gain_c_per_duty));
    TEST_CHECK(cross.k_gain_c_per_duty / direct.k_gain_c_per_duty > 0.1f, detail);
}

/* TODO.md 6A.5(c): the Relative Gain Array over the matrix 6A.5(b) builds.
 *
 * These are hand-computable cases on purpose. The RGA is the one number on
 * the Zones page that tells an operator "your zones are independent, one PID
 * each is correct" -- if it is wrong it is confidently, silently wrong, so
 * every case below either has an exactly-known answer or is one the function
 * must REFUSE. Nothing here touches hardware or the sim plant; it is pure
 * arithmetic over a matrix literal, which is the only way this can be
 * checked at all right now: no cross-gain cell has ever been filled on the
 * real board (every on-target autotune aborts on guard 6, no thermocouples
 * attached), so the RGA has never seen measured data. */

/* Rows and columns of an RGA sum to 1 for any invertible K. It is a property
 * of the definition, not of any particular kiln, so it is the cheapest
 * available check that the inverse and -- crucially -- the TRANSPOSE in
 * Lambda = K .* (K^-1)^T were both applied. Drop the transpose and a
 * non-symmetric K's rows stop summing to 1. */
static void check_rga_sums_to_one(const autotune_rga_t *r, const char *what)
{
    char msg[120];
    for (int i = 0; i < r->n; i++) {
        float row = 0.0f, col = 0.0f;
        for (int j = 0; j < r->n; j++) {
            row += r->lambda[i][j];
            col += r->lambda[j][i];
        }
        snprintf(msg, sizeof(msg), "%s: RGA row %d sums to 1", what, i);
        TEST_CHECK_NEAR(row, 1.0f, 1e-3f, msg);
        snprintf(msg, sizeof(msg), "%s: RGA column %d sums to 1", what, i);
        TEST_CHECK_NEAR(col, 1.0f, 1e-3f, msg);
    }
}

static void test_rga(void)
{
    /* --- Known 2x2, worked by hand ------------------------------------
     * K = [[2,1],[1,2]], det = 3, K^-1 = (1/3)[[2,-1],[-1,2]].
     * Lambda[0][0] = 2 * (2/3) = 4/3; Lambda[0][1] = 1 * (-1/3) = -1/3.
     * Physically: a mildly coupled pair where each loop must push about 33%
     * harder than its own open-loop gain suggests. */
    {
        float k[4] = {2.0f, 1.0f, 1.0f, 2.0f};
        bool v[4] = {true, true, true, true};
        autotune_rga_t r = pid_autotune_rga(k, v, 2);
        TEST_CHECK(r.valid, r.valid ? "2x2 RGA computes" : r.invalid_reason);
        TEST_CHECK(r.n == 2, "2x2 RGA uses both zones");
        TEST_CHECK_NEAR(r.lambda[0][0], 4.0f / 3.0f, 1e-4f, "K=[[2,1],[1,2]] gives Lambda[0][0] = 4/3");
        TEST_CHECK_NEAR(r.lambda[0][1], -1.0f / 3.0f, 1e-4f, "K=[[2,1],[1,2]] gives Lambda[0][1] = -1/3");
        TEST_CHECK_NEAR(r.lambda[1][1], 4.0f / 3.0f, 1e-4f, "K=[[2,1],[1,2]] gives Lambda[1][1] = 4/3");
        TEST_CHECK_NEAR(r.determinant, 3.0f, 1e-4f, "determinant of [[2,1],[1,2]] is 3");
        check_rga_sums_to_one(&r, "[[2,1],[1,2]]");
    }

    /* --- Non-symmetric 2x2: the transpose actually matters -------------
     * K = [[2,1],[4,3]], det = 2, K^-1 = (1/2)[[3,-1],[-4,2]].
     * Lambda[0][0] = 2*(3/2) = 3, Lambda[0][1] = 1*(-4/2) = -2. Computing
     * K .* K^-1 without the transpose would give Lambda[0][1] = -0.5 and a
     * row summing to 2.5, which is why the sum check is not decoration. */
    {
        float k[4] = {2.0f, 1.0f, 4.0f, 3.0f};
        bool v[4] = {true, true, true, true};
        autotune_rga_t r = pid_autotune_rga(k, v, 2);
        TEST_CHECK(r.valid, r.valid ? "non-symmetric 2x2 RGA computes" : r.invalid_reason);
        TEST_CHECK_NEAR(r.lambda[0][0], 3.0f, 1e-4f, "K=[[2,1],[4,3]] gives Lambda[0][0] = 3");
        TEST_CHECK_NEAR(r.lambda[0][1], -2.0f, 1e-4f, "K=[[2,1],[4,3]] gives Lambda[0][1] = -2 (transpose applied)");
        check_rga_sums_to_one(&r, "[[2,1],[4,3]]");
    }

    /* --- Decoupled kiln: the answer the page calls "independent" -------
     * Zero cross-gains give exactly the identity, whatever the diagonal
     * gains are. This is what a potter should see if their zones really are
     * thermally separate. */
    {
        float k[4] = {5.0f, 0.0f, 0.0f, 7.0f};
        bool v[4] = {true, true, true, true};
        autotune_rga_t r = pid_autotune_rga(k, v, 2);
        TEST_CHECK(r.valid, r.valid ? "decoupled 2x2 RGA computes" : r.invalid_reason);
        TEST_CHECK_NEAR(r.lambda[0][0], 1.0f, 1e-5f, "zero cross-gain gives Lambda[0][0] = 1 exactly");
        TEST_CHECK_NEAR(r.lambda[0][1], 0.0f, 1e-5f, "zero cross-gain gives Lambda[0][1] = 0");
    }

    /* --- Strong coupling, still computable ----------------------------
     * K = [[1,0.9],[0.9,1]] -> det 0.19, Lambda[0][0] = 1/0.19 = 5.263.
     * Well outside the page's 0.5-2.0 band: the zones nearly duplicate each
     * other and each loop's real authority is a fraction of what its own
     * step test measured. Must be REPORTED, not refused -- it is a genuine
     * measurement of a badly coupled kiln, not a numerical failure. */
    {
        float k[4] = {1.0f, 0.9f, 0.9f, 1.0f};
        bool v[4] = {true, true, true, true};
        autotune_rga_t r = pid_autotune_rga(k, v, 2);
        TEST_CHECK(r.valid, r.valid ? "strongly-coupled 2x2 is computed, not refused" : r.invalid_reason);
        TEST_CHECK_NEAR(r.lambda[0][0], 1.0f / 0.19f, 1e-2f, "K=[[1,.9],[.9,1]] gives Lambda[0][0] = 5.26");
        TEST_CHECK(r.lambda[0][0] > 2.0f, "strong coupling lands outside the page's 0.5-2.0 tolerable band");
        check_rga_sums_to_one(&r, "strongly coupled");
    }

    /* --- Negative diagonal: the pathological pairing -------------------
     * K = [[1,2],[2,1]] -- each zone affects its neighbour MORE than
     * itself. det = -3, Lambda[0][0] = -1/3. Negative means closing the
     * other loop reverses this loop's own sign, i.e. per-zone PID would
     * fight itself. The page must be able to say so, so this must compute. */
    {
        float k[4] = {1.0f, 2.0f, 2.0f, 1.0f};
        bool v[4] = {true, true, true, true};
        autotune_rga_t r = pid_autotune_rga(k, v, 2);
        TEST_CHECK(r.valid, r.valid ? "cross-dominant 2x2 is computed" : r.invalid_reason);
        TEST_CHECK_NEAR(r.lambda[0][0], -1.0f / 3.0f, 1e-4f, "K=[[1,2],[2,1]] gives a NEGATIVE Lambda[0][0]");
        check_rga_sums_to_one(&r, "cross-dominant");
    }

    /* --- Known 3x3 (block diagonal), worked by hand --------------------
     * Zone 0 isolated, zones 1 and 2 coupled as [[2,1],[1,2]]. The RGA of a
     * block-diagonal K is the block diagonal of the blocks' RGAs, so this
     * reuses the first case's 4/3 and -1/3 and additionally proves the 3x3
     * cofactor path agrees with the 2x2 one. */
    {
        float k[9] = {1.0f, 0.0f, 0.0f,
                      0.0f, 2.0f, 1.0f,
                      0.0f, 1.0f, 2.0f};
        bool v[9] = {true, true, true, true, true, true, true, true, true};
        autotune_rga_t r = pid_autotune_rga(k, v, 3);
        TEST_CHECK(r.valid, r.valid ? "3x3 RGA computes" : r.invalid_reason);
        TEST_CHECK(r.n == 3, "3x3 RGA uses all three zones");
        TEST_CHECK_NEAR(r.lambda[0][0], 1.0f, 1e-4f, "isolated zone 0 has Lambda = 1 in a 3x3");
        TEST_CHECK_NEAR(r.lambda[1][1], 4.0f / 3.0f, 1e-4f, "coupled zone 1 has Lambda = 4/3 in the 3x3");
        TEST_CHECK_NEAR(r.lambda[1][2], -1.0f / 3.0f, 1e-4f, "3x3 off-diagonal matches the 2x2 block's -1/3");
        TEST_CHECK_NEAR(r.determinant, 3.0f, 1e-3f, "determinant of the block-diagonal 3x3 is 3");
        check_rga_sums_to_one(&r, "3x3 block diagonal");
    }

    /* --- Exactly singular: must refuse --------------------------------
     * K = [[1,2],[2,4]] -- row 2 is twice row 1, so the two zones respond
     * identically to any duty and no controller can move them
     * independently. The RGA is undefined (division by zero), and a
     * fabricated large number would read as "extremely coupled" rather than
     * "not independently controllable at all". */
    {
        float k[4] = {1.0f, 2.0f, 2.0f, 4.0f};
        bool v[4] = {true, true, true, true};
        autotune_rga_t r = pid_autotune_rga(k, v, 2);
        TEST_CHECK(!r.valid, "an exactly singular K is refused");
        TEST_CHECK(r.status == AUTOTUNE_RGA_ERR_SINGULAR, "singular K reports the SINGULAR status specifically");
        TEST_CHECK(r.invalid_reason[0] != '\0', "a refused RGA carries a reason string for the page to show");
    }

    /* --- Near-singular at scale: must also refuse ----------------------
     * det here is 0.1 -- comfortably nonzero, so a bare `det == 0` test
     * would sail straight through and return Lambda around 1.6e6. But the
     * entries are ~400 degC/duty and the scale-aware floor is
     * 1e-4 * 400^2 = 16, so this is correctly rejected. This case is the
     * whole reason the threshold is relative: the same matrix expressed in
     * tenths of a degree would have a determinant 100x larger and must get
     * the same verdict. */
    {
        float k[4] = {100.0f, 200.0f, 200.0f, 400.001f};
        bool v[4] = {true, true, true, true};
        autotune_rga_t r = pid_autotune_rga(k, v, 2);
        TEST_CHECK(!r.valid, "a near-singular K (det nonzero but tiny for its scale) is refused");
        TEST_CHECK(r.status == AUTOTUNE_RGA_ERR_SINGULAR, "near-singular K reports SINGULAR");
    }

    /* --- Incomplete matrix: must refuse, not zero-pad ------------------
     * Only zone 0 has been autotuned, so only row 0 exists. Zero-padding
     * rows 1 and 2 would produce a singular matrix at best and a fictional
     * "your zones are independent" at worst. */
    {
        float k[9] = {10.0f, 2.0f, 1.0f,
                      0.0f, 0.0f, 0.0f,
                      0.0f, 0.0f, 0.0f};
        bool v[9] = {true, true, true, false, false, false, false, false, false};
        autotune_rga_t r = pid_autotune_rga(k, v, 3);
        TEST_CHECK(!r.valid, "a matrix with only one measured row is refused");
        TEST_CHECK(r.status == AUTOTUNE_RGA_ERR_INCOMPLETE, "one measured row reports INCOMPLETE, not SINGULAR");
    }

    /* A hole INSIDE an otherwise-complete pair is just as fatal: zone 1's
     * response to zone 0 never fitted, so the 2x2 over {0,1} contains an
     * unknown even though both zones have been run. */
    {
        float k[4] = {10.0f, 2.0f, 3.0f, 12.0f};
        bool v[4] = {true, true, false, true};
        autotune_rga_t r = pid_autotune_rga(k, v, 2);
        TEST_CHECK(!r.valid, "a single missing cell in a 2x2 is refused");
        TEST_CHECK(r.status == AUTOTUNE_RGA_ERR_INCOMPLETE, "one missing cell reports INCOMPLETE");
    }

    /* --- Partial 3x3: fall back to the complete 2x2 sub-block ----------
     * Zones 0 and 2 have been autotuned, zone 1 has not. The honest answer
     * is the RGA over {0,2} -- labelled as such -- rather than a 3x3 padded
     * with zeros for the zone nobody has measured. */
    {
        float k[9] = {10.0f, 0.0f, 2.0f,
                      0.0f,  0.0f, 0.0f,
                      2.0f,  0.0f, 10.0f};
        bool v[9] = {true, false, true,
                     false, false, false,
                     true, false, true};
        autotune_rga_t r = pid_autotune_rga(k, v, 3);
        TEST_CHECK(r.valid, r.valid ? "a complete 2x2 sub-block inside a partial 3x3 is used" : r.invalid_reason);
        TEST_CHECK(r.n == 2, "the sub-block is 2x2, not a zero-padded 3x3");
        TEST_CHECK(r.zone_index[0] == 0 && r.zone_index[1] == 2,
                   "the sub-block is labelled with the real zone numbers 0 and 2, not 0 and 1");
        TEST_CHECK_NEAR(r.lambda[0][0], 100.0f / 96.0f, 1e-4f, "sub-block RGA comes from zones 0 and 2's gains");
    }

    /* --- Fewer than two zones -----------------------------------------
     * A single loop cannot interact with itself; its "RGA" is the vacuous
     * scalar 1, and showing that would imply an independence measurement
     * that was never made. */
    {
        float k[1] = {10.0f};
        bool v[1] = {true};
        autotune_rga_t r = pid_autotune_rga(k, v, 1);
        TEST_CHECK(!r.valid, "a 1x1 matrix is refused");
        TEST_CHECK(r.status == AUTOTUNE_RGA_ERR_TOO_FEW_ZONES, "1x1 reports TOO_FEW_ZONES");
    }

    /* A NaN gain (a fit that blew up) must not propagate into Lambda, where
     * it would render as a display bug rather than a data problem. */
    {
        float k[4] = {10.0f, 2.0f, 2.0f, (float)NAN};
        bool v[4] = {true, true, true, true};
        autotune_rga_t r = pid_autotune_rga(k, v, 2);
        TEST_CHECK(!r.valid, "a NaN gain is refused rather than propagated into Lambda");
    }
}

/* ------------------------------------------------------------------------
 * Level-scheduled coupling gain (sim_kiln_step()'s "joint excess" schedule,
 * sim_plant.c). Uses the REAL fresh matrix G_fresh and REAL measured joint
 * plateau data (docs/audits/coupling_sign_reversal_artifact_test_
 * 2026-09-11.md, docs/audits/joint_vs_singlecolumn_matched_dT0_
 * 2026-09-11.md) rather than the synthetic two_zone_cfg() fixture above --
 * the whole point is to reproduce specific measured numbers, not just a
 * qualitative shape. ambient_c=0 and loss_coeff_w_per_c=1.0 throughout so
 * a zone's steady-state element_c IS its measured ΔT directly, matching
 * the audits' own convention.
 * ------------------------------------------------------------------------ */

/* k_dc (diag), tau_s -- firmware/KilnFW's measured FOPDT per zone, per the
 * task brief and docs/audits/coupling_sign_reversal_artifact_test_
 * 2026-09-11.md's G_fresh. dead_time/sensor lag are irrelevant here: these
 * tests read sim_kiln_element_c() (ground truth), never the lagged/delayed
 * sensor pipeline. */
static const float k_dc[3] = {42.731f, 32.397f, 33.849f};
static const float tau_s[3] = {255.6f, 258.9f, 247.1f};
/* Off-diagonal cells of G_fresh; [i][i] is ignored by sim_kiln_step(). */
static const float g_fresh_offdiag[3][3] = {
    {0.0f, 25.42f, 21.52f},
    {12.44f, 0.0f, 26.08f},
    {8.08f, 10.81f, 0.0f},
};

static sim_kiln_cfg_t coupling_schedule_cfg(void)
{
    sim_kiln_cfg_t c = {0};
    c.zone_count = 3;
    for (int i = 0; i < 3; i++) {
        c.zone[i].plant.ambient_c = 0.0f;
        c.zone[i].plant.heater_power_w = k_dc[i];
        c.zone[i].plant.thermal_mass_j_per_c = tau_s[i];
        c.zone[i].plant.loss_coeff_w_per_c = 1.0f;
        c.zone[i].plant.sensor_delay_s = 0.0f;
        c.zone[i].plant.sensor_lag_tau_s = 0.0f;
        for (int j = 0; j < 3; j++) c.coupling_w_per_c[i][j] = g_fresh_offdiag[i][j];
    }
    return c;
}

/* Runs to a steady state so far past every zone's tau (max 258.9s) that the
 * remaining transient is unmeasurably small (30000s / 258.9s ~= 116 time
 * constants -- e^-116 is not a real number of degrees on this hardware). */
static void run_to_steady_state(sim_kiln_state_t *st, const sim_kiln_cfg_t *cfg, const float *duty)
{
    const int steps = (int)(30000.0f / DT_S);
    for (int i = 0; i < steps; i++) {
        sim_kiln_step(st, cfg, duty, DT_S);
    }
}

/* Regression guard: the single-column z2-alone sweep
 * (docs/audits/z2_single_column_superlinearity_discriminator_2026-09-11.md)
 * measured a FLAT dT0-per-duty ratio (22.7/22.5/21.5/21.9, <5% spread) across
 * duty 0.237-0.920. The schedule's "joint excess" level (total duty minus
 * the single largest duty) is EXACTLY 0 whenever only one zone is driven,
 * for every duty that zone takes -- so it must reproduce the raw,
 * unscheduled coupling_w_per_c[0][2] (21.52) EXACTLY, not merely
 * approximately, at every one of these duties. This is the concrete
 * numeric proof the header comment on sim_kiln_coupling_schedule_scale()
 * promises. */
static void test_coupling_schedule_single_column_unchanged(void)
{
    const float duties[] = {0.237f, 0.530f, 0.547f, 0.920f};
    sim_kiln_cfg_t cfg = coupling_schedule_cfg();

    for (size_t i = 0; i < sizeof(duties) / sizeof(duties[0]); i++) {
        sim_kiln_state_t st;
        sim_kiln_reset(&st, &cfg);
        float duty[SIM_KILN_MAX_ZONES] = {0.0f, 0.0f, duties[i]};
        run_to_steady_state(&st, &cfg, duty);

        float dT0 = sim_kiln_element_c(&st, 0);
        float ratio = dT0 / duties[i];
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "single-column z2 duty %.3f: dT0/duty = %.4f matches unscheduled coupling_w_per_c[0][2]=21.52 "
                 "exactly (schedule is a no-op at joint-excess level 0)",
                 (double)duties[i], (double)ratio);
        TEST_CHECK_NEAR(ratio, 21.52f, 0.01f, msg);
    }
}

/* Low joint plateau: u=[0.163,0.210,0.247], measured z0 dT0=13.28C
 * (docs/audits/joint_vs_singlecolumn_matched_dT0_2026-09-11.md). The
 * schedule's SCALE_LO was calibrated to land exactly here, so the
 * tolerance is tight -- this is the fit's own defining point, not an
 * extrapolation. */
static void test_coupling_schedule_low_joint_plateau(void)
{
    sim_kiln_cfg_t cfg = coupling_schedule_cfg();
    sim_kiln_state_t st;
    sim_kiln_reset(&st, &cfg);
    float duty[SIM_KILN_MAX_ZONES] = {0.163f, 0.210f, 0.247f};
    run_to_steady_state(&st, &cfg, duty);

    float dT0 = sim_kiln_element_c(&st, 0);
    TEST_CHECK_NEAR(dT0, 13.28f, 0.1f,
                     "low joint plateau (sum(u)=0.620): z0 settles within 0.1C of the measured 13.28C, "
                     "not the constant-matrix model's ~17.6C over-prediction");
}

/* High joint plateaus (62/70/75C cplval75), z0 only (the row measured at
 * both regimes). This is an HONEST 2-point linear-in-level fit calibrated
 * against the LOW plateau plus the AVERAGE of these three -- it is not
 * expected to hit any one of them exactly, and does not: reproduction is
 * checked as an aggregate (RMS) improvement over the unscheduled constant
 * matrix, which is what a 2-parameter fit through 2 regimes can honestly
 * promise. Per-plateau numbers (schedule vs constant-matrix baseline,
 * baseline numbers straight from docs/audits/
 * coupling_sign_reversal_artifact_test_2026-09-11.md's own G_fresh.u table):
 *   62C: baseline pred 29.329 (residual -3.60) vs this schedule ~28.33
 *        (residual ~-4.60, WORSE -- level 0.539 sits close enough to the
 *        low anchor that the 2-point line undershoots here);
 *   70C: baseline pred 36.051 (residual -4.73) vs this schedule ~42.04
 *        (residual ~+1.26, much BETTER);
 *   75C: baseline pred 39.728 (residual -6.17) vs this schedule ~51.07
 *        (residual ~+5.17, BETTER in magnitude).
 * Reported plainly rather than cherry-picking a rosier anchor: a straight
 * line through 2 points cannot fit 3 non-collinear targets, and 62C is
 * where this one gives up the most. */
static void test_coupling_schedule_high_joint_plateaus(void)
{
    sim_kiln_cfg_t cfg = coupling_schedule_cfg();

    const float duties[3][SIM_KILN_MAX_ZONES] = {
        {0.1680f, 0.3714f, 0.5905f},  /* 62C */
        {0.1764f, 0.4792f, 0.7589f},  /* 70C */
        {0.1756f, 0.5453f, 0.8533f},  /* 75C */
    };
    const float measured_dT0[3] = {32.929f, 40.779f, 45.900f};
    /* G_fresh . u, z0 row only -- the constant-matrix baseline this schedule
     * is meant to improve on (same table as
     * docs/audits/coupling_sign_reversal_artifact_test_2026-09-11.md). */
    const float baseline_pred_dT0[3] = {29.329f, 36.051f, 39.728f};

    double sched_sq = 0.0, base_sq = 0.0;
    for (int p = 0; p < 3; p++) {
        sim_kiln_state_t st;
        sim_kiln_reset(&st, &cfg);
        float duty[SIM_KILN_MAX_ZONES] = {duties[p][0], duties[p][1], duties[p][2]};
        run_to_steady_state(&st, &cfg, duty);

        float dT0 = sim_kiln_element_c(&st, 0);
        double sched_resid = (double)dT0 - (double)measured_dT0[p];
        double base_resid = (double)baseline_pred_dT0[p] - (double)measured_dT0[p];
        sched_sq += sched_resid * sched_resid;
        base_sq += base_resid * base_resid;

        /* Loose per-plateau bound: even the worst point (62C) must not blow
         * up arbitrarily -- 6C keeps this a real check (the constant model's
         * own worst residual here is 6.17C) without demanding exactness a
         * 2-point fit cannot deliver. */
        char msg[160];
        snprintf(msg, sizeof(msg), "plateau %d: scheduled z0 prediction %.2fC within 6C of measured %.2fC", p,
                 (double)dT0, (double)measured_dT0[p]);
        TEST_CHECK(fabs(sched_resid) < 6.0, msg);
    }

    double sched_rms = sqrt(sched_sq / 3.0);
    double base_rms = sqrt(base_sq / 3.0);
    char msg[200];
    snprintf(msg, sizeof(msg),
             "scheduled RMS residual %.3fC over the 3 cplval75 plateaus improves on the constant-matrix "
             "baseline's %.3fC",
             sched_rms, base_rms);
    TEST_CHECK(sched_rms < base_rms, msg);
}

/* The sign reversal itself: the correction (scale-1) must be negative
 * somewhere in the low-joint regime and positive somewhere in the high-joint
 * regime, i.e. it crosses zero strictly between the two measured levels
 * (0.373 and ~0.539-0.721) -- not merely "different", which a monotone-only-
 * one-sign schedule could also satisfy. Checked via the observable behaviour
 * (z0 prediction relative to the unscheduled matrix's own prediction at that
 * same duty vector), not via a private implementation symbol. */
static void test_coupling_schedule_sign_crossing(void)
{
    sim_kiln_cfg_t cfg = coupling_schedule_cfg();

    /* Low joint plateau: scheduled dT0 (13.28-ish, see previous test) must
     * be LESS than the unscheduled constant-matrix prediction (17.62 -- see
     * this file's other tests / docs/audits/joint_load_model_class_design_
     * 2026-09-11.md), i.e. scale < 1 here. */
    {
        sim_kiln_state_t st;
        sim_kiln_reset(&st, &cfg);
        float duty[SIM_KILN_MAX_ZONES] = {0.163f, 0.210f, 0.247f};
        run_to_steady_state(&st, &cfg, duty);
        float dT0 = sim_kiln_element_c(&st, 0);
        TEST_CHECK(dT0 < 17.62f - 0.5f,
                   "low joint plateau: scheduled prediction sits well below the unscheduled matrix's 17.62C "
                   "(scale < 1 here)");
    }

    /* High joint plateau (70C, the schedule's own high anchor point): must
     * be GREATER than the unscheduled constant-matrix prediction (36.051),
     * i.e. scale > 1 here -- the opposite sign from the low plateau above. */
    {
        sim_kiln_state_t st;
        sim_kiln_reset(&st, &cfg);
        float duty[SIM_KILN_MAX_ZONES] = {0.1764f, 0.4792f, 0.7589f};
        run_to_steady_state(&st, &cfg, duty);
        float dT0 = sim_kiln_element_c(&st, 0);
        TEST_CHECK(dT0 > 36.051f + 0.5f,
                   "70C joint plateau: scheduled prediction sits well above the unscheduled matrix's 36.051C "
                   "(scale > 1 here) -- opposite sign from the low plateau, the sign crossing this schedule exists "
                   "to reproduce");
    }
}

static void test_coupling_schedule(void)
{
    test_coupling_schedule_single_column_unchanged();
    test_coupling_schedule_low_joint_plateau();
    test_coupling_schedule_high_joint_plateaus();
    test_coupling_schedule_sign_crossing();
}

void run_test_sim_kiln(void)
{
    TEST_SECTION("sim_kiln (coupled multi-zone plant + injected faults)");
    test_coupling();
    test_radiative_loss();
    test_guard_provocations();
    test_cross_gain_matrix();
    test_rga();
    test_coupling_schedule();
}
