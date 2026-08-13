#include <stdio.h>
#include <string.h>

#include "test_common.h"
#include "../drivers/thermal_guard.h"

static thermal_guard_input_t base_input(void)
{
    thermal_guard_input_t in;
    in.sensor_ok = true;
    in.measurement_c = 20.0f;
    in.setpoint_c = 20.0f;
    in.commanded_duty = 0.0f;
    in.dt_s = 10.0f;
    return in;
}

void run_test_thermal_guard(void)
{
    TEST_SECTION("thermal_guard");

    /* Guard 5: absolute max, immediate, no debounce. */
    {
        thermal_guard_state_t s;
        thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = 0.5f};
        thermal_guard_reset(&s);
        thermal_guard_input_t in = base_input();
        in.measurement_c = 1301.0f;
        bool tripped_now = thermal_guard_tick(&s, &cfg, &in);
        TEST_CHECK(tripped_now, "guard 5 max_temp trips on the very first over-limit tick");
        TEST_CHECK(s.reason == THERMAL_GUARD_TRIP_MAX_TEMP, "reason is MAX_TEMP");
        TEST_CHECK(thermal_guard_tick(&s, &cfg, &in) == false, "latched: second call reports not-newly-tripped");
        TEST_CHECK(s.is_tripped, "still tripped after the latch check");
    }

    /* Guard 5: absolute min. */
    {
        thermal_guard_state_t s;
        thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = 0.5f};
        thermal_guard_reset(&s);
        thermal_guard_input_t in = base_input();
        in.measurement_c = -25.0f;
        TEST_CHECK(thermal_guard_tick(&s, &cfg, &in), "guard 5 min_temp trips");
        TEST_CHECK(s.reason == THERMAL_GUARD_TRIP_MIN_TEMP, "reason is MIN_TEMP");
    }

    /* max_temp_c == 0 means "not set" -- no ceiling, even at absurd temps. */
    {
        thermal_guard_state_t s;
        thermal_guard_cfg_t cfg = {.max_temp_c = 0.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = 0.5f};
        thermal_guard_reset(&s);
        thermal_guard_input_t in = base_input();
        in.measurement_c = 5000.0f;
        TEST_CHECK(thermal_guard_tick(&s, &cfg, &in) == false, "max_temp_c==0 disables guard 5's ceiling");
    }

    /* Guard 6: sensor invalid, debounced over 3 consecutive bad reads. */
    {
        thermal_guard_state_t s;
        thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = 0.5f};
        thermal_guard_reset(&s);
        thermal_guard_input_t in = base_input();
        in.sensor_ok = false;
        TEST_CHECK(thermal_guard_tick(&s, &cfg, &in) == false, "1st bad read: not yet tripped");
        TEST_CHECK(thermal_guard_tick(&s, &cfg, &in) == false, "2nd bad read: not yet tripped");
        TEST_CHECK(thermal_guard_tick(&s, &cfg, &in) == true, "3rd consecutive bad read trips guard 6");
        TEST_CHECK(s.reason == THERMAL_GUARD_TRIP_SENSOR_INVALID, "reason is SENSOR_INVALID");
    }

    /* Guard 6: a good read in between resets the debounce streak. */
    {
        thermal_guard_state_t s;
        thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = 0.5f};
        thermal_guard_reset(&s);
        thermal_guard_input_t bad = base_input();
        bad.sensor_ok = false;
        thermal_guard_input_t good = base_input();
        thermal_guard_tick(&s, &cfg, &bad);
        thermal_guard_tick(&s, &cfg, &bad);
        thermal_guard_tick(&s, &cfg, &good);
        TEST_CHECK(s.sensor_fault_streak == 0, "a good read resets the fault streak");
        TEST_CHECK(thermal_guard_tick(&s, &cfg, &bad) == false, "streak restarted, needs 3 more");
    }

    /* Guard 1: heating commanded, below setpoint, not rising fast enough. */
    {
        thermal_guard_state_t s;
        thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = 10.0f};
        thermal_guard_reset(&s);
        thermal_guard_input_t in = base_input();
        in.setpoint_c = 500.0f;
        in.measurement_c = 20.0f;
        in.commanded_duty = 1.0f;
        bool tripped = false;
        for (int i = 0; i < 40 && !tripped; i++) {
            /* temperature barely moves despite full commanded duty */
            in.measurement_c += 0.01f;
            tripped = thermal_guard_tick(&s, &cfg, &in);
        }
        TEST_CHECK(tripped, "guard 1 trips when commanded heat produces far less than sanity_rate_c_per_min");
        TEST_CHECK(!tripped || s.reason == THERMAL_GUARD_TRIP_HEATING_FAILED, "reason is HEATING_FAILED");
    }

    /* Guard 1: healthy heating (rising at/above sanity rate) never trips. */
    {
        thermal_guard_state_t s;
        thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = 0.5f};
        thermal_guard_reset(&s);
        thermal_guard_input_t in = base_input();
        in.setpoint_c = 500.0f;
        in.measurement_c = 20.0f;
        in.commanded_duty = 1.0f;
        bool tripped = false;
        for (int i = 0; i < 60 && !tripped; i++) {
            in.measurement_c += 1.0f; /* 6C/min at dt_s=10 -- well above the 0.5C/min sanity floor */
            tripped = thermal_guard_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "guard 1 does not trip on healthy heating");
    }

    /* Guard 2: heating commanded while at/above setpoint and falling fast --
     * a miswired zone. */
    {
        thermal_guard_state_t s;
        thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = 0.5f};
        thermal_guard_reset(&s);
        thermal_guard_input_t in = base_input();
        in.setpoint_c = 500.0f;
        in.measurement_c = 600.0f; /* already above setpoint */
        in.commanded_duty = 1.0f;
        bool tripped = false;
        for (int i = 0; i < 20 && !tripped; i++) {
            in.measurement_c -= 2.0f; /* falling fast despite full commanded heat */
            tripped = thermal_guard_tick(&s, &cfg, &in);
        }
        TEST_CHECK(tripped, "guard 2 trips when heating commanded but temperature falls fast at/above setpoint");
        TEST_CHECK(!tripped || s.reason == THERMAL_GUARD_TRIP_WRONG_DIRECTION, "reason is WRONG_DIRECTION");
    }

    /* Guard 3: runaway -- heat commanded off but temperature keeps rising
     * (welded contact / shorted SSR). */
    {
        thermal_guard_state_t s;
        thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = 0.5f};
        thermal_guard_reset(&s);
        thermal_guard_input_t in = base_input();
        in.commanded_duty = 0.0f;
        in.measurement_c = 200.0f;
        bool tripped = false;
        for (int i = 0; i < 40 && !tripped; i++) {
            in.measurement_c += 3.0f; /* rising with heat supposedly off */
            tripped = thermal_guard_tick(&s, &cfg, &in);
        }
        TEST_CHECK(tripped, "guard 3 trips on sustained rise with commanded_duty==0");
        TEST_CHECK(!tripped || s.reason == THERMAL_GUARD_TRIP_RUNAWAY, "reason is RUNAWAY");
    }

    /* Guard 3: heat off and temperature holding steady never trips. */
    {
        thermal_guard_state_t s;
        thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = 0.5f};
        thermal_guard_reset(&s);
        thermal_guard_input_t in = base_input();
        in.commanded_duty = 0.0f;
        in.measurement_c = 200.0f;
        bool tripped = false;
        for (int i = 0; i < 40 && !tripped; i++) {
            tripped = thermal_guard_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "guard 3 does not trip on a normal cooldown/idle hold");
    }

    /* Guard 3, regression (found on hardware 2026-08-12 against the simulated
     * plant): a zone idling at duty 0 through the settle window and drifting
     * up slowly -- a dwell, a neighbour's heat arriving, ordinary coasting --
     * must NOT read as a welded contact. The original code divided the rise
     * measured over the whole off-window by the time since the settle window
     * ended, so the first tick past the window reported a 4.3C drift as
     * "257C/min" and tripped. 0.2C/min here is well under the 1.0C/min
     * threshold and must stay quiet for a long time. */
    {
        thermal_guard_state_t s;
        thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = 0.5f};
        thermal_guard_reset(&s);
        thermal_guard_input_t in = base_input();
        in.commanded_duty = 0.0f;
        in.measurement_c = 200.0f;
        in.dt_s = 10.0f;
        bool tripped = false;
        /* 90 ticks x 10s = 15 minutes, well past the 120s settle window. */
        for (int i = 0; i < 90 && !tripped; i++) {
            in.measurement_c += 0.2f * (in.dt_s / 60.0f); /* 0.2C/min */
            tripped = thermal_guard_tick(&s, &cfg, &in);
        }
        char detail[160];
        snprintf(detail, sizeof(detail), "guard 3 tolerates a slow 0.2C/min drift at duty 0 (detail=\"%s\")",
                 s.detail);
        TEST_CHECK(!tripped, detail);
    }

    /* ...and the rate it reports must be the real one. Same 3C/tick rise as
     * the trip case above, at a known dt: the reported rate has to land near
     * 18C/min, not the ~250C/min the divide-by-one-tick bug produced. */
    {
        thermal_guard_state_t s;
        thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = 0.5f};
        thermal_guard_reset(&s);
        thermal_guard_input_t in = base_input();
        in.commanded_duty = 0.0f;
        in.measurement_c = 200.0f;
        in.dt_s = 10.0f;
        bool tripped = false;
        for (int i = 0; i < 60 && !tripped; i++) {
            in.measurement_c += 3.0f; /* 3C per 10s tick = 18C/min */
            tripped = thermal_guard_tick(&s, &cfg, &in);
        }
        TEST_CHECK(tripped, "guard 3 still trips on a genuine welded-contact rise");
        /* The margin branch (20C above the off-window baseline) can legitimately
         * fire first at this rate; either way the message must not claim an
         * impossible rate. */
        TEST_CHECK(strstr(s.detail, "257") == NULL && strstr(s.detail, "C/min") != NULL,
                   "guard 3's reported rate is plausible, not a divide-by-one-tick artefact");
    }

    /* Guard 4: drift -- settle within setpoint, then sustained excursion
     * outside the drift band for the full drift period trips. */
    {
        thermal_guard_state_t s;
        thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = 0.5f};
        thermal_guard_reset(&s);
        thermal_guard_input_t in = base_input();
        in.setpoint_c = 500.0f;
        in.measurement_c = 500.0f; /* settled exactly at setpoint */
        in.commanded_duty = 0.5f;
        TEST_CHECK(thermal_guard_tick(&s, &cfg, &in) == false, "settling tick does not trip");
        TEST_CHECK(s.at_setpoint_window_active, "settle latch is set once within the drift band");
        in.measurement_c = 400.0f; /* 100C off, outside DRIFT_HYSTERESIS_C(25) */
        in.commanded_duty = 0.3f; /* below PROGRESS_DUTY_MIN so guards 1/2/3 stay quiet and this isolates guard 4 */
        bool tripped = false;
        for (int i = 0; i < 80 && !tripped; i++) {
            tripped = thermal_guard_tick(&s, &cfg, &in);
        }
        TEST_CHECK(tripped, "guard 4 trips on a sustained excursion after having settled");
        TEST_CHECK(!tripped || s.reason == THERMAL_GUARD_TRIP_DRIFT, "reason is DRIFT");
    }

    /* Guard 4: never having settled means the drift guard cannot fire, no
     * matter how far off setpoint the zone sits (that's guards 1/2/5's job,
     * not this one's). */
    {
        thermal_guard_state_t s;
        thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = 0.5f};
        thermal_guard_reset(&s);
        thermal_guard_input_t in = base_input();
        in.setpoint_c = 500.0f;
        in.measurement_c = 20.0f; /* cold start, never near setpoint */
        in.commanded_duty = 0.0f; /* also keeps guards 1/3 from firing here */
        bool tripped = false;
        for (int i = 0; i < 80 && !tripped; i++) {
            tripped = thermal_guard_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "guard 4 stays quiet before the zone has ever settled");
    }

    /* Guard 7: frozen sensor -- identical reading for the full window while
     * heat is commanded on. */
    {
        thermal_guard_state_t s;
        thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = 0.0f};
        thermal_guard_reset(&s);
        thermal_guard_input_t in = base_input();
        in.commanded_duty = 1.0f;
        in.measurement_c = 300.0f;
        bool tripped = false;
        for (int i = 0; i < 65 && !tripped; i++) { /* 65*10s = 650s > FROZEN_WINDOW_S(600) */
            tripped = thermal_guard_tick(&s, &cfg, &in);
        }
        TEST_CHECK(tripped, "guard 7 trips when the reading never changes while duty > 0");
        TEST_CHECK(!tripped || s.reason == THERMAL_GUARD_TRIP_FROZEN, "reason is FROZEN");
    }

    /* Guard 7: a reading that keeps changing never trips, even with duty>0
     * for a long time -- guards 1/3 (heating-failed/runaway) are what a
     * flatlined-but-changing sensor would hit, not this one. */
    {
        thermal_guard_state_t s;
        thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = 0.1f};
        thermal_guard_reset(&s);
        thermal_guard_input_t in = base_input();
        in.setpoint_c = 5000.0f; /* keep guard 1 from firing across this loop */
        in.commanded_duty = 1.0f;
        in.measurement_c = 300.0f;
        bool tripped = false;
        for (int i = 0; i < 65 && !tripped; i++) {
            in.measurement_c += 0.5f;
            tripped = thermal_guard_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "guard 7 does not trip when the reading is actually moving");
    }

    /* thermal_guard_clear() fully un-latches and resets windows. */
    {
        thermal_guard_state_t s;
        thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = 0.5f};
        thermal_guard_reset(&s);
        thermal_guard_input_t in = base_input();
        in.measurement_c = 2000.0f;
        thermal_guard_tick(&s, &cfg, &in);
        TEST_CHECK(s.is_tripped, "sanity: trip fired");
        thermal_guard_clear(&s);
        TEST_CHECK(!s.is_tripped, "clear() un-latches");
        in.measurement_c = 20.0f;
        TEST_CHECK(thermal_guard_tick(&s, &cfg, &in) == false, "post-clear tick with a safe reading does not re-trip");
    }
}
