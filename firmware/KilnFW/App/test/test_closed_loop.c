// Closed-loop integration test: sim_plant + pid + heater_output +
// thermal_guard wired together the same way profile_executor.c wires the
// real hardware (minus relay_authority/FreeRTOS), run against a simple
// kiln model. This is the test TODO.md 6A.8 calls out as the one that
// "falsifies the design cheaply" -- if the four modules don't cooperate to
// reach and hold a setpoint without a guard false-tripping, that's a real
// bug, not a hardware problem.
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "test_common.h"
#include "../drivers/control/pid.h"
#include "../drivers/control/pid_fuzzy.h"
#include "../drivers/control/thermal_guard.h"
#include "../drivers/control/heater_output.h"
#include "sim_plant.h"

/* Mirrors profile_executor.c's ZONE_CONTROL_MODE_PID_FUZZY per-tick wiring
 * exactly (pid_fuzzy_prepare_gains(), a static function there and so not
 * directly callable from a host test): read error_rate_c_per_s from
 * state->d_filtered as it stands BEFORE this tick's pid_update_terms() call
 * (hazard 1 -- the filtered derivative pid.c already maintains, no dSP/dt
 * term, see pid_fuzzy.c/profile_executor.c's matching comments), compute the
 * fuzzy-adjusted gains, bump-transfer Ki via pid.c's own
 * pid_rescale_integral_for_new_ki() (hazard 3), then call pid_update() with
 * the adjusted gains. *prev_ki starts at 0.0f (matches
 * zone_runtime_t::fuzzy_prev_effective_ki's cold-start convention) and is
 * updated by this call for the caller to pass back in next tick.
 *
 * Caveat: this is a hand-written MIRROR of pid_fuzzy_prepare_gains(), not a
 * call into it -- that function is `static` in profile_executor.c and (like
 * pid_family_zone_tick(), see mandatory-test-1's comment below) is not
 * reachable from a host test without either exposing it or reconstructing
 * zone_runtime_t/s_exec here. Every test in this file that calls
 * fuzzy_tick() is therefore exercising this copy of the wiring, not the
 * production one -- a future change to pid_fuzzy_prepare_gains() (a
 * reordered step, a dropped rescale call, a different signal fed to
 * error_rate_c_per_s) would silently diverge from this mirror and go
 * undetected by every test below. */
static float fuzzy_tick(pid_state_t *state, const pid_cfg_t *base_cfg, uint8_t strength_pct,
                        float *prev_ki, float setpoint, float measurement, float dt_s, float ff_u)
{
    float error_c = setpoint - measurement;
    float error_rate_c_per_s = state->d_filtered;
    float kp, ki, kd;
    pid_fuzzy_adjust(error_c, error_rate_c_per_s, 20.0f, 0.5f, base_cfg->kp, base_cfg->ki, base_cfg->kd,
                     strength_pct, &kp, &ki, &kd);
    pid_rescale_integral_for_new_ki(state, *prev_ki, ki);
    *prev_ki = ki;
    pid_cfg_t adjusted = *base_cfg;
    adjusted.kp = kp;
    adjusted.ki = ki;
    adjusted.kd = kd;
    return pid_update(state, &adjusted, setpoint, measurement, dt_s, ff_u, ff_u);
}

/* Warm-starts a plant already close to a setpoint (element_c/sensor_c/the
 * whole delay ring set to warm_c), the same technique this file's existing
 * guard-3 test uses -- so a test that wants to exercise NORMAL PID math
 * (inside pid_range_c) doesn't have to first simulate the very slow cold
 * climb sim_plant.c's thermal mass otherwise requires. Outside pid_range_c
 * the controller ignores Kp/Ki/Kd entirely (pid.c's functional-range
 * blending), so any test comparing classic vs fuzzy gains needs to start
 * warm or it is comparing two full-on outputs that were never going to
 * differ regardless of what this whole feature does. */
static void warm_start_plant(sim_plant_state_t *p, const sim_plant_cfg_t *cfg, float warm_c)
{
    sim_plant_reset(p, cfg);
    p->element_c = warm_c;
    p->sensor_c = warm_c;
    for (int i = 0; i < SIM_PLANT_DELAY_MAX_STEPS; i++) {
        p->delay_ring[i] = warm_c;
    }
}

void run_test_closed_loop(void)
{
    TEST_SECTION("closed_loop (sim_plant + pid + thermal_guard + heater_output)");

    sim_plant_cfg_t plant_cfg = {
        .ambient_c = 20.0f,
        .thermal_mass_j_per_c = 50000.0f,
        .heater_power_w = 2000.0f,
        .loss_coeff_w_per_c = 5.0f, /* equilibrium at duty=1: ambient + power/loss = 420C */
        .sensor_delay_s = 5.0f,
        .sensor_lag_tau_s = 8.0f,
    };
    sim_plant_state_t plant;
    sim_plant_reset(&plant, &plant_cfg);

    pid_cfg_t pid_cfg = {.kp = 0.01f, .ki = 0.0005f, .kd = 0.05f, .d_filter_tau_s = 30.0f, .b = 1.0f, .pid_range_c = 50.0f};
    pid_state_t pid_state;
    pid_reset(&pid_state);

    thermal_guard_cfg_t guard_cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = 0.5f};
    thermal_guard_state_t guard_state;
    thermal_guard_reset(&guard_state);

    heater_output_cfg_t heater_cfg = {.window_ms = 60000, .min_on_ms = 2000, .min_off_ms = 2000};
    heater_output_state_t heater_state;
    heater_output_reset(&heater_state);

    const float setpoint_c = 300.0f;
    const float dt_s = 10.0f;
    const uint32_t dt_ms = (uint32_t)(dt_s * 1000.0f);
    const int steps = (int)((4.0f * 3600.0f) / dt_s); /* 4 simulated hours */

    bool guard_tripped = false;
    thermal_guard_trip_t trip_reason = THERMAL_GUARD_TRIP_NONE;
    int trip_step = -1;

    for (int i = 0; i < steps; i++) {
        float duty = pid_update(&pid_state, &pid_cfg, setpoint_c, plant.sensor_c, dt_s, 0.0f, 0.0f);
        bool relay_on = heater_output_duty(&heater_state, &heater_cfg, duty, dt_ms);

        thermal_guard_input_t in = {
            .sensor_ok = true,
            .measurement_c = plant.sensor_c,
            .setpoint_c = setpoint_c,
            .commanded_duty = relay_on ? 1.0f : 0.0f,
            .dt_s = dt_s,
        };
        if (thermal_guard_tick(&guard_state, &guard_cfg, &in) && !guard_tripped) {
            guard_tripped = true;
            trip_reason = guard_state.reason;
            trip_step = i;
        }

        sim_plant_step(&plant, &plant_cfg, relay_on ? 1.0f : 0.0f, dt_s);
    }

    char detail[160];
    snprintf(detail, sizeof(detail), "no false guard trip over a healthy 4h run (tripped=%d reason=%d at step %d, detail=\"%s\")",
             guard_tripped, (int)trip_reason, trip_step, guard_state.detail);
    TEST_CHECK(!guard_tripped, detail);

    TEST_CHECK_NEAR(plant.sensor_c, setpoint_c, 10.0, "sensor settles within 10C of setpoint after 4 simulated hours");

    /* A cold zone must actually demand heat, not sit idle. */
    {
        sim_plant_state_t p2;
        sim_plant_reset(&p2, &plant_cfg);
        pid_state_t pid2;
        pid_reset(&pid2);
        float duty = pid_update(&pid2, &pid_cfg, setpoint_c, p2.sensor_c, dt_s, 0.0f, 0.0f);
        TEST_CHECK_NEAR(duty, 1.0f, 1e-6, "280C below setpoint (outside pid_range_c) commands full duty");
    }

    /* Guard 3 (runaway) must still fire in this same closed-loop harness if
     * the relay is stuck ON despite the model reporting it commanded OFF --
     * i.e. the guard is actually wired to something that can trip, not just
     * quietly along for the ride for the whole healthy run above. */
    {
        sim_plant_state_t p3;
        sim_plant_reset(&p3, &plant_cfg);
        p3.element_c = p3.sensor_c = 200.0f;
        for (int i = 0; i < SIM_PLANT_DELAY_MAX_STEPS; i++) p3.delay_ring[i] = 200.0f;
        thermal_guard_state_t g3;
        thermal_guard_reset(&g3);
        bool tripped = false;
        for (int i = 0; i < 60 && !tripped; i++) {
            /* Plant actually driven full-on (a welded relay), guard told duty==0 (what was commanded). */
            sim_plant_step(&p3, &plant_cfg, 1.0f, dt_s);
            thermal_guard_input_t in = {
                .sensor_ok = true, .measurement_c = p3.sensor_c, .setpoint_c = setpoint_c,
                .commanded_duty = 0.0f, .dt_s = dt_s,
            };
            tripped = thermal_guard_tick(&g3, &guard_cfg, &in);
        }
        TEST_CHECK(tripped, "guard 3 fires against the plant model when heat stays on despite duty==0 commanded");
    }

    /* PID_EXPANSION_PLAN.md Phase 3, mandatory test 1, HONESTLY RELABELED:
     * this is a determinism check on pid_update()/pid_update_terms(), NOT a
     * regression guard on the pid_family_zone_tick() extraction in
     * profile_executor.c. pid_family_zone_tick() is `static` there (reads
     * s_exec.target_c/target_rate, zone_runtime_t's load-cap/cooling-hold
     * bookkeeping, and calls zone_feedforward()/heater_output_duty()) and so
     * cannot be called from this host test without either exposing it
     * outside its translation unit or reconstructing a large slice of
     * profile_executor.c's internal state here -- both would be contorting
     * production code for the sake of one test, which the task that added
     * this comment declined to do. What this test actually proves: running
     * the identical setup through pid_update() twice yields bit-identical
     * output, i.e. pid_update() itself has no hidden nondeterminism (no
     * uninitialized reads, no reliance on anything but its own arguments and
     * state struct). What it does NOT cover: whether pid_family_zone_tick()'s
     * extraction changed the classic-PID call path's behavior at all -- the
     * evidence for that fidelity is the textual diff of profile_executor.c
     * against its pre-extraction version (reviewed by hand when the
     * extraction landed), not anything this test file exercises. A future
     * change to pid_family_zone_tick()'s classic-PID branch that altered
     * behavior would NOT be caught here. */
    {
        sim_plant_state_t p_a, p_b;
        sim_plant_reset(&p_a, &plant_cfg);
        sim_plant_reset(&p_b, &plant_cfg);
        pid_state_t s_a, s_b;
        pid_reset(&s_a);
        pid_reset(&s_b);
        bool bit_identical = true;
        for (int i = 0; i < 200; i++) {
            float u_a = pid_update(&s_a, &pid_cfg, setpoint_c, p_a.sensor_c, dt_s, 0.0f, 0.0f);
            float u_b = pid_update(&s_b, &pid_cfg, setpoint_c, p_b.sensor_c, dt_s, 0.0f, 0.0f);
            if (u_a != u_b) {
                bit_identical = false;
            }
            sim_plant_step(&p_a, &plant_cfg, u_a > 0.5f ? 1.0f : 0.0f, dt_s);
            sim_plant_step(&p_b, &plant_cfg, u_b > 0.5f ? 1.0f : 0.0f, dt_s);
        }
        TEST_CHECK(bit_identical, "determinism check (NOT a pid_family_zone_tick() regression guard, see comment above): "
                                   "two identical pid_update() runs produce bit-identical duty every tick");
    }

    /* Mandatory test 2: strength_pct=0 fuzzy mode must be identical to
     * classic PID, run through the actual per-tick wiring algorithm
     * (fuzzy_tick() above), not just pid_fuzzy_adjust() in isolation --
     * proves the *integration*, including the hazard-3 rescale call every
     * tick, degrades to exactly today's behavior at the safe default. */
    {
        sim_plant_state_t p_classic, p_fuzzy0;
        sim_plant_reset(&p_classic, &plant_cfg);
        sim_plant_reset(&p_fuzzy0, &plant_cfg);
        pid_state_t s_classic, s_fuzzy0;
        pid_reset(&s_classic);
        pid_reset(&s_fuzzy0);
        float prev_ki = 0.0f;
        bool bit_identical = true;
        for (int i = 0; i < 200; i++) {
            float u_classic = pid_update(&s_classic, &pid_cfg, setpoint_c, p_classic.sensor_c, dt_s, 0.0f, 0.0f);
            float u_fuzzy0 = fuzzy_tick(&s_fuzzy0, &pid_cfg, 0, &prev_ki, setpoint_c, p_fuzzy0.sensor_c, dt_s, 0.0f);
            if (u_classic != u_fuzzy0) {
                bit_identical = false;
            }
            sim_plant_step(&p_classic, &plant_cfg, u_classic > 0.5f ? 1.0f : 0.0f, dt_s);
            sim_plant_step(&p_fuzzy0, &plant_cfg, u_fuzzy0 > 0.5f ? 1.0f : 0.0f, dt_s);
        }
        TEST_CHECK(bit_identical, "strength_pct=0 fuzzy wiring produces bit-identical duty to classic PID, every tick");
    }
    {
        /* Negative test: prove the check above is not vacuous -- the same
         * wiring at strength_pct=100 (a real adjustment) must diverge from
         * classic PID at some point in the run. If it didn't, the fuzzy
         * layer wouldn't actually be doing anything and the "=0 must equal
         * classic" test above would be trivially true for the wrong reason. */
        sim_plant_state_t p_classic, p_fuzzy100;
        warm_start_plant(&p_classic, &plant_cfg, setpoint_c - 20.0f); /* inside pid_range_c=50 -- normal PID math from tick 1 */
        warm_start_plant(&p_fuzzy100, &plant_cfg, setpoint_c - 20.0f);
        pid_state_t s_classic, s_fuzzy100;
        pid_reset(&s_classic);
        pid_reset(&s_fuzzy100);
        float prev_ki = 0.0f;
        bool ever_diverged = false;
        for (int i = 0; i < 200; i++) {
            float u_classic = pid_update(&s_classic, &pid_cfg, setpoint_c, p_classic.sensor_c, dt_s, 0.0f, 0.0f);
            float u_fuzzy100 = fuzzy_tick(&s_fuzzy100, &pid_cfg, 100, &prev_ki, setpoint_c, p_fuzzy100.sensor_c, dt_s, 0.0f);
            if (u_classic != u_fuzzy100) {
                ever_diverged = true;
            }
            sim_plant_step(&p_classic, &plant_cfg, u_classic > 0.5f ? 1.0f : 0.0f, dt_s);
            sim_plant_step(&p_fuzzy100, &plant_cfg, u_fuzzy100 > 0.5f ? 1.0f : 0.0f, dt_s);
        }
        TEST_CHECK(ever_diverged, "sanity: strength_pct=100 actually changes the duty trajectory vs classic PID "
                                   "(proves the strength=0 bit-identical test above is a real check, not vacuous)");
    }

    /* Mandatory test 3: bump-transfer at the closed-loop level -- a gain
     * change mid-run (the fuzzy layer moving Ki tick to tick, driven by
     * real plant dynamics rather than a hand-picked step) must not step the
     * commanded duty. Run fuzzy mode for a while, capture consecutive
     * ticks' duty, and assert no single tick-to-tick jump exceeds a small
     * bound once the loop is past its initial cold-start transient. */
    {
        sim_plant_state_t p;
        warm_start_plant(&p, &plant_cfg, setpoint_c - 20.0f); /* inside pid_range_c -- gains actually drive duty */
        pid_state_t s;
        pid_reset(&s);
        float prev_ki = 0.0f;
        float prev_u = NAN;
        float max_step = 0.0f;
        for (int i = 0; i < 400; i++) {
            float u = fuzzy_tick(&s, &pid_cfg, 100, &prev_ki, setpoint_c, p.sensor_c, dt_s, 0.0f);
            if (i > 30 && isfinite(prev_u)) { /* past the cold-start ramp-in */
                float step = fabsf(u - prev_u);
                if (step > max_step) max_step = step;
            }
            prev_u = u;
            sim_plant_step(&p, &plant_cfg, u > 0.5f ? 1.0f : 0.0f, dt_s);
        }
        char detail[96];
        snprintf(detail, sizeof(detail), "largest tick-to-tick duty step in steady fuzzy operation: %.4f", (double)max_step);
        TEST_CHECK(max_step < 0.15f, detail);
        /* The negative counterpart proving pid_rescale_integral_for_new_ki()
         * is load-bearing (not just present) lives in test_pid.c, not here:
         * this closed-loop trajectory's error/rate move smoothly tick to
         * tick, so pid_fuzzy_adjust()'s Ki output also moves smoothly (the
         * rule table's membership is a continuous interpolation, not a
         * step function) -- there is no single tick here where Ki jumps far
         * enough for even an UN-rescaled run to visibly step, which would
         * make a "delete the rescale call" variant of this test pass
         * vacuously for the wrong reason (small Ki delta, not a correct
         * fix). test_pid.c's negative test instead drives a real 40% Ki
         * step directly -- the size an actual rule-table cell change can
         * produce -- and confirms an UN-rescaled version of that same-sized
         * step really does move the output, which is the failure mode this
         * mechanism exists to prevent. */
    }

    /* Mandatory test 4: 4-simulated-hour closed-loop run with fuzzy mode at
     * strength_pct=100, mirroring the existing classic-PID run at the top of
     * this file -- fuzzy must not destabilize a plant classic PID already
     * handles cleanly: no false guard trip, settles near setpoint. */
    {
        sim_plant_state_t plant_f;
        sim_plant_reset(&plant_f, &plant_cfg);
        pid_state_t pid_state_f;
        pid_reset(&pid_state_f);
        float prev_ki_f = 0.0f;

        thermal_guard_state_t guard_state_f;
        thermal_guard_reset(&guard_state_f);

        heater_output_state_t heater_state_f;
        heater_output_reset(&heater_state_f);

        bool guard_tripped_f = false;
        thermal_guard_trip_t trip_reason_f = THERMAL_GUARD_TRIP_NONE;
        int trip_step_f = -1;

        for (int i = 0; i < steps; i++) {
            float duty = fuzzy_tick(&pid_state_f, &pid_cfg, 100, &prev_ki_f, setpoint_c, plant_f.sensor_c, dt_s, 0.0f);
            bool relay_on = heater_output_duty(&heater_state_f, &heater_cfg, duty, dt_ms);

            thermal_guard_input_t in = {
                .sensor_ok = true,
                .measurement_c = plant_f.sensor_c,
                .setpoint_c = setpoint_c,
                .commanded_duty = relay_on ? 1.0f : 0.0f,
                .dt_s = dt_s,
            };
            if (thermal_guard_tick(&guard_state_f, &guard_cfg, &in) && !guard_tripped_f) {
                guard_tripped_f = true;
                trip_reason_f = guard_state_f.reason;
                trip_step_f = i;
            }

            sim_plant_step(&plant_f, &plant_cfg, relay_on ? 1.0f : 0.0f, dt_s);
        }

        char detail_f[160];
        snprintf(detail_f, sizeof(detail_f),
                "no false guard trip over a healthy 4h fuzzy-mode run (tripped=%d reason=%d at step %d, detail=\"%s\")",
                guard_tripped_f, (int)trip_reason_f, trip_step_f, guard_state_f.detail);
        TEST_CHECK(!guard_tripped_f, detail_f);
        TEST_CHECK_NEAR(plant_f.sensor_c, setpoint_c, 10.0, "fuzzy mode settles within 10C of setpoint after 4 simulated hours, same as classic PID");
    }
}
