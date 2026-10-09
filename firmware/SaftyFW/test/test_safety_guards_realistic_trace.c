// Closed-loop, realistic-trace host tests for safety_guards.c's S1/S8
// guards. TODO.md Phase 4's open item: "Reuse a realistic thermal trace in
// the guard tests (current tests use synthetic step/ramp sequences, which
// were enough to prove each guard's boundary, but a closed-loop trace is
// still useful for S2/S8 tuning later)."
//
// KilnFW's own firmware/KilnFW/App/test/sim_plant.c is the "reuse" this
// item names, but it is not linked here: its header (sim_plant.h) pulls in
// firmware/KilnFW/App/drivers/persist/zones_config_json.h -- a 3500+ line
// KilnFW-only persistence header with no reason to be a build dependency of
// SaftyFW's host tests, and ARCHITECTURE.md's independence doctrine for this
// module (safety_guards.h's own top comment) argues against growing new
// cross-firmware coupling into this test tree for a fixture, not for
// production code. Instead this file implements the SAME first-order-lag
// heating model sim_plant.c uses (thermal mass + heat loss proportional to
// delta-T + a fixed transport delay), independently, scoped to exactly what
// S1/S8 need: a closed-loop, bang-bang-controlled trace and a runaway trace,
// both driving the real, unmodified safety_guards_tick() -- not a
// transcribed copy of ITS logic; only the plant is new, the guard under
// test is production code linked in unmodified via safety_guards.c.
#include <math.h>
#include <string.h>

#include "test_common.h"
#include "../src/safety_guards.h"

// Minimal FOPDT-with-dead-time plant, mirroring sim_plant.c's legacy
// (SIM_NODE_LEGACY) model: element_c integrates heater power against
// thermal mass and ambient loss; sensor_c is element_c delayed by a fixed
// transport lag through a ring buffer. Not shared code with sim_plant.c
// (see file comment) -- deliberately the same small, well-understood model,
// not a novel one, so nothing about the trace shape is a surprise.
#define TRACE_DELAY_MAX_STEPS 64

typedef struct {
    float ambient_c;
    float thermal_mass_j_per_c;
    float heater_power_w;
    float loss_coeff_w_per_c;
    float delay_s;
} trace_plant_cfg_t;

typedef struct {
    float element_c;
    float delay_ring[TRACE_DELAY_MAX_STEPS];
    int   delay_len;
    int   delay_head;
} trace_plant_state_t;

static void trace_plant_reset(trace_plant_state_t *st, const trace_plant_cfg_t *cfg)
{
    memset(st, 0, sizeof(*st));
    st->element_c = cfg->ambient_c;
    for (int i = 0; i < TRACE_DELAY_MAX_STEPS; i++) {
        st->delay_ring[i] = cfg->ambient_c;
    }
}

// Advances the plant by dt_s at heater duty in [0,1]; returns the delayed
// sensor reading for this tick.
static float trace_plant_step(trace_plant_state_t *st, const trace_plant_cfg_t *cfg, float duty, float dt_s)
{
    float power_in_w = cfg->heater_power_w * duty;
    float loss_w = cfg->loss_coeff_w_per_c * (st->element_c - cfg->ambient_c);
    float d_c = (power_in_w - loss_w) / cfg->thermal_mass_j_per_c * dt_s;
    st->element_c += d_c;

    int delay_steps = (int)(cfg->delay_s / dt_s + 0.5f);
    if (delay_steps < 0) {
        delay_steps = 0;
    }
    if (delay_steps >= TRACE_DELAY_MAX_STEPS) {
        delay_steps = TRACE_DELAY_MAX_STEPS - 1;
    }

    st->delay_ring[st->delay_head] = st->element_c;
    int read_idx = st->delay_head - delay_steps;
    if (read_idx < 0) {
        read_idx += TRACE_DELAY_MAX_STEPS;
    }
    float sensor_c = (st->delay_len > delay_steps) ? st->delay_ring[read_idx] : cfg->ambient_c;
    st->delay_head = (st->delay_head + 1) % TRACE_DELAY_MAX_STEPS;
    if (st->delay_len < TRACE_DELAY_MAX_STEPS) {
        st->delay_len++;
    }
    return sensor_c;
}

static safety_guard_input_t trace_base_input(void)
{
    safety_guard_input_t in;
    memset(&in, 0, sizeof(in));
    in.tc_valid = true;
    in.cj_c = 25.0f;
    in.estop_pressed = false;
    in.heat_commanded = false;
    in.context_valid = false;
    in.current_sensing_commissioned = true;
    in.sample_counter_advancing = true;
    in.link_up = true;
    return in;
}

static safety_guard_cfg_t trace_base_cfg(void)
{
    safety_guard_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.tc_placement_valid = true;
    cfg.tc_placement_mode = SAFETY_TC_EXTERNAL_OVERHEAT;
    cfg.tc_source = SAFETY_TC_SOURCE_OWN_J7;
    return cfg;
}

// S1: a closed-loop bang-bang controller regulating well under the ceiling,
// through a lagged/delayed sensor, must never nuisance-trip -- the harder
// version of test_safety_guards.c's plain "reading stays under ceiling"
// check, because here the READING lags the true plant state and overshoots
// the setpoint on every cycle the way a real bang-bang loop does.
static void test_s1_closed_loop_no_nuisance(void)
{
    TEST_SECTION("S1 realistic trace -- closed-loop bang-bang control never nuisance-trips");

    trace_plant_cfg_t pcfg = {
        .ambient_c = 20.0f,
        .thermal_mass_j_per_c = 5000.0f,
        .heater_power_w = 1500.0f,
        .loss_coeff_w_per_c = 5.0f,
        .delay_s = 3.0f,
    };
    trace_plant_state_t pst;
    trace_plant_reset(&pst, &pcfg);

    safety_guard_state_t s;
    safety_guards_reset(&s);
    safety_guard_cfg_t cfg = trace_base_cfg();
    cfg.abs_max_temp_c = 300.0f; /* commissioned ceiling, well above the 200C setpoint below */
    safety_guard_input_t in = trace_base_input();
    in.dt_s = 1.0f;

    const float setpoint_c = 200.0f;
    bool tripped = false;
    float max_seen_c = pcfg.ambient_c;
    for (int i = 0; i < 3600 && !tripped; i++) { /* one hour, 1s ticks */
        float duty = (pst.element_c < setpoint_c) ? 1.0f : 0.0f; /* bang-bang */
        float reading_c = trace_plant_step(&pst, &pcfg, duty, in.dt_s);
        in.tc_c = reading_c;
        if (reading_c > max_seen_c) {
            max_seen_c = reading_c;
        }
        tripped = safety_guards_tick(&s, &cfg, &in);
    }
    TEST_CHECK(!tripped, "a bang-bang loop regulating at 200C with a 300C ceiling never trips S1");
    TEST_CHECK(max_seen_c < cfg.abs_max_temp_c, "sanity: the trace itself never reached the ceiling either");
}

// S1: the same closed loop, but the heater relay sticks ON (a stuck-SSR /
// stuck-relay fault the guard exists to catch) instead of ever cycling off.
// The plant, still lagged and delayed exactly as above, must climb through
// the ceiling and S1 must trip -- proving the guard reacts to a genuine
// runaway riding on the same lagged sensor pipeline that the nuisance case
// above proved does NOT cause false trips.
static void test_s1_closed_loop_stuck_relay_trips(void)
{
    TEST_SECTION("S1 realistic trace -- stuck-on relay runaway trips S1");

    trace_plant_cfg_t pcfg = {
        .ambient_c = 20.0f,
        .thermal_mass_j_per_c = 5000.0f,
        .heater_power_w = 1500.0f,
        .loss_coeff_w_per_c = 5.0f,
        .delay_s = 3.0f,
    };
    trace_plant_state_t pst;
    trace_plant_reset(&pst, &pcfg);

    safety_guard_state_t s;
    safety_guards_reset(&s);
    safety_guard_cfg_t cfg = trace_base_cfg();
    cfg.abs_max_temp_c = 300.0f;
    safety_guard_input_t in = trace_base_input();
    in.dt_s = 1.0f;

    bool tripped = false;
    int trip_tick = -1;
    for (int i = 0; i < 7200 && !tripped; i++) { /* two hours, 1s ticks -- plenty for this plant to reach steady state above 300C */
        float reading_c = trace_plant_step(&pst, &pcfg, 1.0f /* stuck full-on */, in.dt_s);
        in.tc_c = reading_c;
        tripped = safety_guards_tick(&s, &cfg, &in);
        if (tripped) {
            trip_tick = i;
        }
    }
    TEST_CHECK(tripped, "a heater stuck full-on eventually trips S1 through the lagged/delayed sensor path");
    TEST_CHECK(s.reason == SAFETY_TRIP_OVERTEMP, "reason is SAFETY_TRIP_OVERTEMP (S1)");
    TEST_CHECK(trip_tick > 0 && trip_tick < 7200, "trip happened within the run, not on the very first tick (real thermal lag, not an instant reaction)");
}

// S8: a legitimate closed-loop firing profile (bounded duty, well under the
// commissioned rate ceiling once averaged through the same lag/delay
// pipeline) must never nuisance-trip S8, even though the *instantaneous*
// slope right after a duty step momentarily looks steep -- this is the
// "closed-loop trace, not a synthetic ramp" case S8's own tests do not
// cover, since every existing S8 test in test_safety_guards.c drives a
// hand-picked linear ramp directly, never a lagged plant's step response.
static void test_s8_closed_loop_no_nuisance(void)
{
    TEST_SECTION("S8 realistic trace -- closed-loop ramp under the commissioned ceiling never nuisance-trips");

    trace_plant_cfg_t pcfg = {
        .ambient_c = 20.0f,
        .thermal_mass_j_per_c = 20000.0f, /* larger mass -> gentler, more realistic ramp */
        .heater_power_w = 2000.0f,
        .loss_coeff_w_per_c = 4.0f,
        .delay_s = 5.0f,
    };
    trace_plant_state_t pst;
    trace_plant_reset(&pst, &pcfg);

    safety_guard_state_t s;
    safety_guards_reset(&s);
    safety_guard_cfg_t cfg = trace_base_cfg();
    cfg.abs_max_temp_c = 0.0f; /* isolate S8 from S1, same convention test_safety_guards.c uses */
    cfg.max_rate_c_per_min = 30.0f; /* commissioned ceiling */
    cfg.rate_window_s = 60.0f;
    safety_guard_input_t in = trace_base_input();
    in.dt_s = 1.0f;

    bool tripped = false;
    for (int i = 0; i < 5400 && !tripped; i++) { /* 90 minutes, full-power ramp then hold */
        float duty = 1.0f;
        float reading_c = trace_plant_step(&pst, &pcfg, duty, in.dt_s);
        in.tc_c = reading_c;
        tripped = safety_guards_tick(&s, &cfg, &in);
    }
    TEST_CHECK(!tripped, "this plant's full-power ramp, averaged over rate_window_s through the lagged sensor, never exceeds max_rate_c_per_min");
}

// S8: the same closed-loop plant but with a much smaller thermal mass (a
// materially faster kiln, or a rate ceiling commissioned too tight for the
// installed hardware) DOES sustain an over-threshold average across two
// consecutive windows and must trip -- proving S8 fires on a realistic
// trace's sustained average, not only on the hand-built linear ramps
// test_safety_guards.c already covers.
static void test_s8_closed_loop_fast_plant_trips(void)
{
    TEST_SECTION("S8 realistic trace -- a materially faster plant sustains an over-threshold average and trips");

    trace_plant_cfg_t pcfg = {
        .ambient_c = 20.0f,
        .thermal_mass_j_per_c = 2000.0f, /* small mass -> fast ramp */
        .heater_power_w = 3000.0f,
        .loss_coeff_w_per_c = 3.0f,
        .delay_s = 2.0f,
    };
    trace_plant_state_t pst;
    trace_plant_reset(&pst, &pcfg);

    safety_guard_state_t s;
    safety_guards_reset(&s);
    safety_guard_cfg_t cfg = trace_base_cfg();
    cfg.abs_max_temp_c = 0.0f; /* isolate S8 from S1 */
    cfg.max_rate_c_per_min = 30.0f;
    cfg.rate_window_s = 60.0f;
    safety_guard_input_t in = trace_base_input();
    in.dt_s = 1.0f;

    bool tripped = false;
    for (int i = 0; i < 1800 && !tripped; i++) { /* 30 minutes is plenty for this small a mass */
        float reading_c = trace_plant_step(&pst, &pcfg, 1.0f, in.dt_s);
        in.tc_c = reading_c;
        tripped = safety_guards_tick(&s, &cfg, &in);
    }
    TEST_CHECK(tripped, "a materially faster plant's sustained average rate trips S8 within 30 minutes");
    TEST_CHECK(s.reason == SAFETY_TRIP_RATE, "reason is SAFETY_TRIP_RATE (S8)");
}

void run_test_safety_guards_realistic_trace(void)
{
    test_s1_closed_loop_no_nuisance();
    test_s1_closed_loop_stuck_relay_trips();
    test_s8_closed_loop_no_nuisance();
    test_s8_closed_loop_fast_plant_trips();
}
