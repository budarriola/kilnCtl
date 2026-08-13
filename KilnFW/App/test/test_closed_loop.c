// Closed-loop integration test: sim_plant + pid + heater_output +
// thermal_guard wired together the same way profile_executor.c wires the
// real hardware (minus relay_authority/FreeRTOS), run against a simple
// kiln model. This is the test TODO.md 6A.8 calls out as the one that
// "falsifies the design cheaply" -- if the four modules don't cooperate to
// reach and hold a setpoint without a guard false-tripping, that's a real
// bug, not a hardware problem.
#include <stdint.h>
#include <stdio.h>

#include "test_common.h"
#include "../drivers/pid.h"
#include "../drivers/thermal_guard.h"
#include "../drivers/heater_output.h"
#include "sim_plant.h"

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
        float duty = pid_update(&pid_state, &pid_cfg, setpoint_c, plant.sensor_c, dt_s, 0.0f);
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
        float duty = pid_update(&pid2, &pid_cfg, setpoint_c, p2.sensor_c, dt_s, 0.0f);
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
}
