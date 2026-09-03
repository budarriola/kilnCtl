#include <stdio.h>
#include <string.h>

#include "test_common.h"
#include "../drivers/thermal_guard.h"

static thermal_guard_input_t base_input(void)
{
    /* Zero-initialized, not just declared, so every field this function
     * doesn't set explicitly below (peer_c/peer_ok/peer_count/
     * peer_index_self, and now progress_rise_check_relaxed) reads as its
     * documented "off"/"not configured" value rather than whatever
     * garbage happened to be on the stack. Without this, an uninitialized
     * progress_rise_check_relaxed could read nonzero by pure stack-content
     * luck and silently relax guard 1 in tests that exist specifically to
     * prove it trips -- exactly the false-negative class this whole task
     * is about not shipping. */
    thermal_guard_input_t in = {0};
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

    /* Guard 1 relaxation (progress_rise_check_relaxed) -- autotune_engine.c's
     * "element proven" latch, see thermal_guard_input_t's own comment for
     * the bench defect this exists to fix (an honest step test approaching
     * its asymptote tripping guard 1 for "not rising" when it had already
     * proven the element heats). Tested here at the pure thermal_guard.c
     * level -- autotune_engine.c's own tests cover the latch that DRIVES
     * this flag; these prove the flag itself does what it claims. */

    /* THE CASE THE GUARD EXISTS FOR: a genuinely dead element (commanded
     * heat, zero rise, relaxation flag left false/unset exactly as it would
     * be for real -- a dead element never crosses the min-rise threshold
     * that earns the relaxation) must still trip, explicitly with the flag
     * named here even though it's the zero-init default. */
    {
        thermal_guard_state_t s;
        thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = 0.5f};
        thermal_guard_reset(&s);
        thermal_guard_input_t in = base_input();
        in.setpoint_c = 500.0f;
        in.measurement_c = 20.0f;
        in.commanded_duty = 1.0f;
        in.progress_rise_check_relaxed = false; /* explicit -- this is the case that must never be masked */
        bool tripped = false;
        for (int i = 0; i < 40 && !tripped; i++) {
            /* dead element: commanded heat, ZERO rise (not even the 0.01C
             * drift the earlier guard-1 test allows) */
            tripped = thermal_guard_tick(&s, &cfg, &in);
        }
        TEST_CHECK(tripped, "a dead element (no relaxation, no rise at all) must still trip guard 1 -- this "
                            "is the hazard the guard exists to catch and must never be maskable");
        TEST_CHECK(!tripped || s.reason == THERMAL_GUARD_TRIP_HEATING_FAILED, "reason is HEATING_FAILED");
    }

    /* THE RELAXATION ITSELF: the identical dead/flat trace, but with
     * progress_rise_check_relaxed=true throughout -- must NOT trip guard 1.
     * This is what autotune_engine.c sets once it has ALREADY proven the
     * element heats; a real autotune run never reaches this flag=true state
     * without first showing genuine rise (see the autotune_engine.c-level
     * tests), but the flag's own mechanism must behave correctly regardless
     * of how a caller arrived at it. */
    {
        thermal_guard_state_t s;
        thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = 0.5f};
        thermal_guard_reset(&s);
        thermal_guard_input_t in = base_input();
        in.setpoint_c = 500.0f;
        in.measurement_c = 20.0f;
        in.commanded_duty = 1.0f;
        in.progress_rise_check_relaxed = true;
        bool tripped = false;
        for (int i = 0; i < 40 && !tripped; i++) {
            tripped = thermal_guard_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "progress_rise_check_relaxed=true must relax guard 1's rise requirement -- an "
                             "identical flat trace that trips above must NOT trip here");
    }

    /* SCOPE: relaxation covers guard 1 ONLY -- guard 2 (falling while
     * heating, at/above setpoint) must still trip even with
     * progress_rise_check_relaxed=true. Proves the relaxation cannot be
     * (mis)used to blind the OTHER heating-related guard in the same
     * progress window. */
    {
        thermal_guard_state_t s;
        thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = 0.5f};
        thermal_guard_reset(&s);
        thermal_guard_input_t in = base_input();
        in.setpoint_c = 500.0f;
        in.measurement_c = 600.0f; /* already above setpoint -- guard 2's branch */
        in.commanded_duty = 1.0f;
        in.progress_rise_check_relaxed = true;
        bool tripped = false;
        for (int i = 0; i < 20 && !tripped; i++) {
            in.measurement_c -= 2.0f; /* falling fast despite full commanded heat */
            tripped = thermal_guard_tick(&s, &cfg, &in);
        }
        TEST_CHECK(tripped, "guard 2 must trip regardless of progress_rise_check_relaxed -- the relaxation "
                            "is scoped to guard 1's rise check only");
        TEST_CHECK(!tripped || s.reason == THERMAL_GUARD_TRIP_WRONG_DIRECTION, "reason is WRONG_DIRECTION");
    }

    /* BOUNDARY: delta == expected EXACTLY must NOT trip -- guard 1's
     * "delta < expected" is a strict-less-than on purpose (equality means
     * the zone cleared the bar). Two ticks only, both using exact-in-float
     * integers (20.0, 25.0, 300.0, 60.0, 1.0), so no float-accumulation
     * error can nudge either side of the comparison: tick 1 activates the
     * window at measurement_c=20.0; tick 2 uses dt_s=300.0 (the DEFAULT
     * 300s window in one jump) and measurement_c=25.0, giving
     * delta=5.0 == expected=(1.0 C/min * 5.0 min)=5.0 exactly. */
    {
        thermal_guard_state_t s;
        thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = 1.0f};
        thermal_guard_reset(&s);
        thermal_guard_input_t in = base_input();
        in.setpoint_c = 500.0f;
        in.measurement_c = 20.0f;
        in.commanded_duty = 1.0f;
        in.dt_s = 1.0f;
        TEST_CHECK(thermal_guard_tick(&s, &cfg, &in) == false, "tick 1 (window activation) must not trip");
        in.dt_s = 300.0f;
        in.measurement_c = 25.0f;
        bool tripped = thermal_guard_tick(&s, &cfg, &in);
        TEST_CHECK(!tripped, "delta (5.0C) == expected (1.0C/min * 5.0min = 5.0C) EXACTLY must NOT trip -- "
                             "equality means the zone cleared the bar, not missed it");
    }

    /* PRECISION: a genuine near-miss (delta=4.96C, expected=5.00C) that used
     * to round to the SAME "5.0C" at one decimal in the trip message --
     * making a real, correct trip look like a delta==expected boundary bug
     * to anyone reading the log (the exact confusion a bench trip produced:
     * "rose only 0.5C in 1.0min (need >=0.5C)"). Same two-tick construction
     * as the boundary test above. Confirmed to FAIL (message showed "5.0C
     * ... need >=5.0C", indistinguishable from the boundary case) against
     * the pre-fix %.1f format; restoring %.2f makes the two cases visibly
     * different -- see this task's own negative-test report for the exact
     * captured pre-fix string. */
    {
        thermal_guard_state_t s;
        thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = 1.0f};
        thermal_guard_reset(&s);
        thermal_guard_input_t in = base_input();
        in.setpoint_c = 500.0f;
        in.measurement_c = 20.0f;
        in.commanded_duty = 1.0f;
        in.dt_s = 1.0f;
        thermal_guard_tick(&s, &cfg, &in);
        in.dt_s = 300.0f;
        in.measurement_c = 24.96f;
        bool tripped = thermal_guard_tick(&s, &cfg, &in);
        TEST_CHECK(tripped, "delta (4.96C) < expected (5.00C) must trip -- a genuine, if small, shortfall");
        TEST_CHECK(strstr(s.detail, "4.96") != NULL,
                  "the trip message must show enough precision (4.96, not 5.0) to prove this was a real "
                  "shortfall, not a delta==expected boundary case -- see PRECISION comment above");
        TEST_CHECK(strstr(s.detail, "5.00") != NULL, "the 'need >=' side must show the same precision");
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

    /* Guard 4: before having settled, a SHORT excursion (well under one
     * DRIFT_PERIOD_S) still does not trip -- a normal cold start needs at
     * least that much grace before the sustained-excursion clock can even
     * arm. 80 ticks * 10s = 800s > DRIFT_PERIOD_S(600) alone would already
     * have armed under the fix below, so this uses a shorter run
     * (59 ticks = 590s, just under the 600s arming threshold) to isolate
     * "still not armed" from "armed but not yet sustained long enough". */
    {
        thermal_guard_state_t s;
        thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = 0.5f};
        thermal_guard_reset(&s);
        thermal_guard_input_t in = base_input();
        in.setpoint_c = 500.0f;
        in.measurement_c = 20.0f; /* cold start, never near setpoint */
        in.commanded_duty = 0.0f; /* also keeps guards 1/3 from firing here */
        bool tripped = false;
        for (int i = 0; i < 59 && !tripped; i++) { /* 59*10s = 590s, under DRIFT_PERIOD_S(600) */
            tripped = thermal_guard_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "guard 4 stays quiet before the zone has ever settled, within one drift period's grace");
        TEST_CHECK(!s.at_setpoint_window_active, "settle latch never set -- this zone genuinely never got close");
    }

    /* Guard 4 ARMING FIX (2026-09-03, profile_executor.c hot-start ramp-lock
     * defect): a zone that NEVER settles must still eventually trip if it
     * sits outside the drift band forever -- previously at_setpoint_window_
     * active never latched true for such a zone, so the sustained-excursion
     * clock could never start and this guard was permanently inert on it
     * (guards 1/2/7 are no help either: they all gate on nonzero/>=0.5
     * commanded duty, and a zone that is HOTTER than setpoint the whole time
     * -- the reachable hot-start case -- commands none). Fix: the guard also
     * arms once run_elapsed_s has passed a full DRIFT_PERIOD_S even without
     * ever settling. Two full periods (one to arm, one sustained after)
     * must elapse before the trip. */
    {
        thermal_guard_state_t s;
        thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = 0.5f};
        thermal_guard_reset(&s);
        thermal_guard_input_t in = base_input();
        in.setpoint_c = 100.0f;
        in.measurement_c = 150.0f; /* HOT start, 50C above setpoint, never settles -- the reachable case */
        in.commanded_duty = 0.0f; /* a hot zone commands no heat -- guards 1/2/3 structurally cannot see this */
        bool tripped = false;
        int trip_tick = -1;
        for (int i = 0; i < 130 && !tripped; i++) { /* up to 1300s; trip expected around 1200s (2*DRIFT_PERIOD_S) */
            tripped = thermal_guard_tick(&s, &cfg, &in);
            if (tripped) trip_tick = i;
        }
        TEST_CHECK(tripped, "guard 4 eventually trips a zone that starts hot and never settles, once armed by run duration");
        TEST_CHECK(!tripped || s.reason == THERMAL_GUARD_TRIP_DRIFT, "reason is DRIFT");
        TEST_CHECK(!tripped || (trip_tick >= 118 && trip_tick <= 121),
                   "trips right around 2*DRIFT_PERIOD_S (1200s = tick 120), not early and not late");
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

    /* TODO.md 6A.3's remaining named thresholds, per-zone override (2026-08-16):
     * a zone that sets sensor_fault_debounce_ticks=1 trips guard 6 on the
     * very first bad read, not the third -- and a zone that leaves it at 0
     * still gets the firmware default (3), covered by the debounce test
     * above using the same all-zero cfg every other pre-existing test uses. */
    {
        thermal_guard_state_t s;
        thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = 0.5f,
                                    .sensor_fault_debounce_ticks = 1.0f};
        thermal_guard_reset(&s);
        thermal_guard_input_t in = base_input();
        in.sensor_ok = false;
        TEST_CHECK(thermal_guard_tick(&s, &cfg, &in) == true, "debounce override=1 trips guard 6 on the 1st bad read");
    }

    /* frozen_window_s override: a zone that sets it to 20s trips guard 7
     * after 3 ticks (30s) at dt_s=10, not the 65 ticks the firmware default
     * (600s) needs -- same input pattern as the unmodified-cfg guard 7 test
     * above, just faster. */
    {
        thermal_guard_state_t s;
        thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = 0.0f,
                                    .frozen_window_s = 20.0f};
        thermal_guard_reset(&s);
        thermal_guard_input_t in = base_input();
        in.commanded_duty = 1.0f;
        in.measurement_c = 300.0f;
        bool tripped = false;
        for (int i = 0; i < 5 && !tripped; i++) {
            tripped = thermal_guard_tick(&s, &cfg, &in);
        }
        TEST_CHECK(tripped, "frozen_window_s override=20 trips guard 7 well before the 600s default would");
        TEST_CHECK(!tripped || s.reason == THERMAL_GUARD_TRIP_FROZEN, "reason is FROZEN");
    }

    /* runaway_margin_c override: a zone that tightens the margin to 5C trips
     * guard 3 on a rise the firmware default (20C) would still be quiet
     * about at the same point in the same rise used by the "does not trip on
     * a normal cooldown/idle hold" test above (which never rises at all --
     * this uses a small, steady rise instead so the tighter margin, not the
     * rate check, is what fires). */
    {
        thermal_guard_state_t s;
        thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = 0.5f,
                                    .runaway_margin_c = 5.0f, .runaway_rate_c_per_min = 1000.0f};
        thermal_guard_reset(&s);
        thermal_guard_input_t in = base_input();
        in.commanded_duty = 0.0f;
        in.measurement_c = 200.0f;
        bool tripped = false;
        /* 0.05C/tick x 10s dt -> 0.3C/min, well under even a very loose rate
         * threshold -- isolates the margin check, which crosses 5C at tick 100. */
        for (int i = 0; i < 150 && !tripped; i++) {
            in.measurement_c += 0.05f;
            tripped = thermal_guard_tick(&s, &cfg, &in);
        }
        TEST_CHECK(tripped, "runaway_margin_c override=5 trips guard 3 on a rise the 20C default would still tolerate");
        TEST_CHECK(!tripped || s.reason == THERMAL_GUARD_TRIP_RUNAWAY, "reason is RUNAWAY");
    }

    /* Defect (1) fix: wrong_dir_window_s must control guard 1
     * (THERMAL_GUARD_TRIP_HEATING_FAILED), not just guard 2. A zone that
     * configures a short 60s window trips at 60s, not the hardcoded 300s
     * PROGRESS_WINDOW_S default -- this is exactly the bench failure: a dead
     * heating element only got caught after a fixed 5 minutes, ignoring the
     * operator's configured 60s. */
    {
        thermal_guard_state_t s;
        thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = 10.0f,
                                    .wrong_dir_window_s = 60.0f};
        thermal_guard_reset(&s);
        thermal_guard_input_t in = base_input();
        in.setpoint_c = 500.0f;
        in.measurement_c = 20.0f;
        in.commanded_duty = 1.0f; /* dead element: heat commanded, temperature never responds */
        int trip_tick = -1;
        for (int i = 0; i < 40; i++) {
            if (thermal_guard_tick(&s, &cfg, &in)) {
                trip_tick = i;
                break;
            }
        }
        TEST_CHECK(trip_tick >= 0, "guard 1 trips a dead element with wrong_dir_window_s configured");
        TEST_CHECK(s.reason == THERMAL_GUARD_TRIP_HEATING_FAILED, "reason is HEATING_FAILED");
        /* dt_s=10, so trip_tick*10s must land at the configured 60s window
         * (tick index 5, i.e. elapsed 60s), not at 300s (tick index 29). */
        char detail[160];
        snprintf(detail, sizeof(detail), "trip landed at tick %d (%.0fs) -- expected ~60s, not 300s", trip_tick,
                 (trip_tick + 1) * 10.0);
        TEST_CHECK(trip_tick >= 0 && trip_tick <= 6, detail);
    }

    /* Same scenario, but with wrong_dir_window_s left at 0 ("not configured")
     * -- must NOT trip at 60s; the 300s PROGRESS_WINDOW_S fallback still
     * applies, proving the fix didn't change unconfigured-zone behaviour. */
    {
        thermal_guard_state_t s;
        thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = 10.0f};
        thermal_guard_reset(&s);
        thermal_guard_input_t in = base_input();
        in.setpoint_c = 500.0f;
        in.measurement_c = 20.0f;
        in.commanded_duty = 1.0f;
        bool tripped = false;
        /* 6 ticks x 10s = 60s -- would already have tripped if the 60s
         * window from the configured-zone test above leaked in here. */
        for (int i = 0; i < 6; i++) {
            tripped = thermal_guard_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "unconfigured zone (wrong_dir_window_s==0) does not trip guard 1 at 60s");
    }

    /* Defect (2) fix: dither of a couple hundredths of a degree -- well
     * within a real MAX31856's read-to-read noise -- must NOT reset the
     * frozen-sensor window. This is the exact scenario the bit-exact
     * comparison got wrong on hardware (readings moved every second for 6
     * minutes straight and the guard never fired). */
    {
        thermal_guard_state_t s;
        thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = 0.0f};
        thermal_guard_reset(&s);
        thermal_guard_input_t in = base_input();
        in.commanded_duty = 1.0f;
        in.measurement_c = 300.0f;
        bool tripped = false;
        for (int i = 0; i < 65 && !tripped; i++) { /* 65*10s = 650s > FROZEN_WINDOW_S(600) */
            in.measurement_c = 300.0f + ((i % 2 == 0) ? 0.02f : -0.02f); /* +/-0.02C dither */
            tripped = thermal_guard_tick(&s, &cfg, &in);
        }
        TEST_CHECK(tripped, "guard 7 still trips a stuck sensor despite +/-0.02C read-to-read dither");
        TEST_CHECK(!tripped || s.reason == THERMAL_GUARD_TRIP_FROZEN, "reason is FROZEN");
    }

    /* Negative case for the epsilon band: a move clearly above the band
     * (0.1C, well over FROZEN_EPS_C=0.03C) each tick must keep resetting the
     * window, same as before this fix -- the tolerance must not be so loose
     * that it swallows a sensor that is actually moving. */
    {
        thermal_guard_state_t s;
        thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = 0.01f};
        thermal_guard_reset(&s);
        thermal_guard_input_t in = base_input();
        in.setpoint_c = 5000.0f; /* keep guard 1 quiet across this loop */
        in.commanded_duty = 1.0f;
        in.measurement_c = 300.0f;
        bool tripped = false;
        for (int i = 0; i < 65 && !tripped; i++) {
            in.measurement_c += 0.1f; /* well above FROZEN_EPS_C every tick */
            tripped = thermal_guard_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "guard 7 does not trip when the reading moves clearly above the epsilon band");
    }


    /* ---- v8 per-zone overrides (2026-08-27) --------------------------------
     * The owner asked for the remaining hardcoded thresholds to become
     * operator settings. Each pair below proves the same two things the eight
     * older overrides are held to: a configured value actually changes when
     * the guard fires, and 0 still means the firmware constant, so a board
     * nobody has configured behaves exactly as it did before. */

    /* progress_duty_min: guard 1 arms above the configured duty. At duty 0.2
     * the 0.5 default leaves the guard disarmed entirely; an override of 0.1
     * arms it, and a dead element then trips it. */
    {
        thermal_guard_state_t s;
        thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = 5.0f,
                                    .wrong_dir_window_s = 30.0f, .progress_duty_min = 0.1f};
        thermal_guard_reset(&s);
        thermal_guard_input_t in = base_input();
        in.commanded_duty = 0.2f;
        in.setpoint_c = 500.0f;
        in.measurement_c = 100.0f; /* dead element: never rises */
        bool tripped = false;
        for (int i = 0; i < 10 && !tripped; i++) {
            tripped = thermal_guard_tick(&s, &cfg, &in);
        }
        TEST_CHECK(tripped, "progress_duty_min override=0.1 arms guard 1 at a duty the 0.5 default ignores");
        TEST_CHECK(!tripped || s.reason == THERMAL_GUARD_TRIP_HEATING_FAILED, "reason is HEATING_FAILED");
    }
    {
        thermal_guard_state_t s;
        thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = 5.0f,
                                    .wrong_dir_window_s = 30.0f, .progress_duty_min = 0.0f};
        thermal_guard_reset(&s);
        thermal_guard_input_t in = base_input();
        in.commanded_duty = 0.2f;
        in.setpoint_c = 500.0f;
        in.measurement_c = 100.0f;
        bool tripped = false;
        for (int i = 0; i < 10 && !tripped; i++) {
            tripped = thermal_guard_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "progress_duty_min==0 keeps the 0.5 default, so duty 0.2 leaves guard 1 disarmed");
    }

    /* progress_window_s: guard 1's own window, separate from guard 2's. With
     * wrong_dir_window_s left at 0 the heating branch used to be stuck on the
     * 300s constant; an override of 30s trips inside 4 ticks at dt_s=10. */
    {
        thermal_guard_state_t s;
        thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = 5.0f,
                                    .progress_window_s = 30.0f};
        thermal_guard_reset(&s);
        thermal_guard_input_t in = base_input();
        in.commanded_duty = 1.0f;
        in.setpoint_c = 500.0f;
        in.measurement_c = 100.0f;
        bool tripped = false;
        for (int i = 0; i < 6 && !tripped; i++) {
            tripped = thermal_guard_tick(&s, &cfg, &in);
        }
        TEST_CHECK(tripped, "progress_window_s override=30 trips guard 1 long before the 300s default");
        TEST_CHECK(!tripped || s.reason == THERMAL_GUARD_TRIP_HEATING_FAILED, "reason is HEATING_FAILED");
    }
    {
        thermal_guard_state_t s;
        thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = 5.0f};
        thermal_guard_reset(&s);
        thermal_guard_input_t in = base_input();
        in.commanded_duty = 1.0f;
        in.setpoint_c = 500.0f;
        in.measurement_c = 100.0f;
        bool tripped = false;
        for (int i = 0; i < 6 && !tripped; i++) {
            tripped = thermal_guard_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "progress_window_s==0 keeps the 300s default -- 60s of dead element is not yet a trip");
    }

    /* drift_hysteresis_c: how far out of band counts as drifting. A 5C
     * override plus a 30s drift period trips on a 10C excursion that the 25C
     * default treats as still settled. */
    {
        thermal_guard_state_t s;
        thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = 0.0f,
                                    .drift_period_s = 30.0f, .drift_hysteresis_c = 5.0f};
        thermal_guard_reset(&s);
        thermal_guard_input_t in = base_input();
        in.setpoint_c = 500.0f;
        in.measurement_c = 500.0f; /* settle first */
        thermal_guard_tick(&s, &cfg, &in);
        in.measurement_c = 510.0f; /* 10C out: outside 5C, inside 25C */
        bool tripped = false;
        for (int i = 0; i < 6 && !tripped; i++) {
            tripped = thermal_guard_tick(&s, &cfg, &in);
        }
        TEST_CHECK(tripped, "drift_hysteresis_c override=5 trips guard 4 on a 10C excursion");
        TEST_CHECK(!tripped || s.reason == THERMAL_GUARD_TRIP_DRIFT, "reason is DRIFT");
    }
    {
        thermal_guard_state_t s;
        thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = 0.0f,
                                    .drift_period_s = 30.0f};
        thermal_guard_reset(&s);
        thermal_guard_input_t in = base_input();
        in.setpoint_c = 500.0f;
        in.measurement_c = 500.0f;
        thermal_guard_tick(&s, &cfg, &in);
        in.measurement_c = 510.0f;
        bool tripped = false;
        for (int i = 0; i < 6 && !tripped; i++) {
            tripped = thermal_guard_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "drift_hysteresis_c==0 keeps the 25C default, so a 10C excursion is still settled");
    }

    /* frozen_eps_c: how much movement still counts as stuck. A sensor
     * dithering 0.5C/tick is moving as far as the 0.05C default is concerned;
     * an override of 1.0C correctly calls it frozen. */
    {
        thermal_guard_state_t s;
        thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = 0.0f,
                                    .frozen_window_s = 20.0f, .frozen_eps_c = 1.0f};
        thermal_guard_reset(&s);
        thermal_guard_input_t in = base_input();
        in.commanded_duty = 1.0f;
        bool tripped = false;
        for (int i = 0; i < 6 && !tripped; i++) {
            in.measurement_c = 300.0f + ((i % 2) ? 0.5f : 0.0f);
            tripped = thermal_guard_tick(&s, &cfg, &in);
        }
        TEST_CHECK(tripped, "frozen_eps_c override=1.0 calls a 0.5C dither frozen");
        TEST_CHECK(!tripped || s.reason == THERMAL_GUARD_TRIP_FROZEN, "reason is FROZEN");
    }
    {
        thermal_guard_state_t s;
        thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = 0.0f,
                                    .frozen_window_s = 20.0f};
        thermal_guard_reset(&s);
        thermal_guard_input_t in = base_input();
        in.commanded_duty = 1.0f;
        bool tripped = false;
        for (int i = 0; i < 6 && !tripped; i++) {
            in.measurement_c = 300.0f + ((i % 2) ? 0.5f : 0.0f);
            tripped = thermal_guard_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "frozen_eps_c==0 keeps the 0.05C default, so a 0.5C dither reads as movement");
    }

    /* Guard 1's verdict follows the CONFIGURED rate, on one identical trace.
     * 2026-08-29, the bench's real failure: zone 0 carried
     * sanity_rate_c_per_min = 5.0 (a real kiln's figure) with a 60s window,
     * and every firing died at t=62s on "rose only 0.0C in 1.0min (need
     * >=5.0C)" -- while the jig genuinely was heating, just at well under
     * 1C/min. The fix was config, not code, so what needs proving is that
     * the number really is what decides: the SAME trace must fail at 5.0 and
     * pass at 0.2. A guard that had quietly hardcoded either value would
     * pass one of these two blocks and fail the other. */
    {
        /* 0.5C/min: a plausible slow-jig rise (0.0833C per 10s tick), run
         * for 5 windows' worth so a sliding window cannot hide the verdict. */
        const float rise_per_tick_c = 0.0833f; /* ~0.5 C/min at dt_s=10 */
        struct { float rate; bool expect_trip; const char *what; } cases[] = {
            {5.0f, true,  "5.0 C/min (the kiln figure this bench had) trips on a 0.5C/min rise"},
            {0.2f, false, "0.2 C/min (the jig figure) passes the very same rise"},
        };
        for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
            thermal_guard_state_t s;
            thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f,
                                       .sanity_rate_c_per_min = cases[c].rate,
                                       .wrong_dir_window_s = 60.0f};
            thermal_guard_reset(&s);
            thermal_guard_input_t in = base_input();
            in.setpoint_c = 500.0f;
            in.measurement_c = 20.0f;
            in.commanded_duty = 1.0f;
            bool tripped = false;
            for (int i = 0; i < 30 && !tripped; i++) { /* 300s = 5 x 60s window */
                in.measurement_c += rise_per_tick_c;
                tripped = thermal_guard_tick(&s, &cfg, &in);
            }
            TEST_CHECK(tripped == cases[c].expect_trip, cases[c].what);
            if (cases[c].expect_trip) {
                TEST_CHECK(!tripped || s.reason == THERMAL_GUARD_TRIP_HEATING_FAILED,
                           "and the reason is HEATING_FAILED, not some other guard");
            }
        }
    }

    /* ...and 0.2 C/min is still a real dead-element check, not a disable.
     * The whole point of keeping the floor above zero: an element that has
     * actually died produces ~no rise, and must still be caught. */
    {
        thermal_guard_state_t s;
        thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f,
                                   .sanity_rate_c_per_min = 0.2f,
                                   .wrong_dir_window_s = 60.0f};
        thermal_guard_reset(&s);
        thermal_guard_input_t in = base_input();
        in.setpoint_c = 500.0f;
        in.measurement_c = 20.0f;
        in.commanded_duty = 1.0f;
        bool tripped = false;
        for (int i = 0; i < 30 && !tripped; i++) {
            in.measurement_c += 0.001f; /* dead element: sensor noise, no heat */
            tripped = thermal_guard_tick(&s, &cfg, &in);
        }
        TEST_CHECK(tripped, "a dead element still trips guard 1 at the jig's 0.2 C/min");
        TEST_CHECK(!tripped || s.reason == THERMAL_GUARD_TRIP_HEATING_FAILED, "reason is HEATING_FAILED");
    }

    /* The slow rate must NOT slow the runaway side of the same module.
     * Guard 3 reads runaway_rate_c_per_min / runaway_margin_c, never
     * sanity_rate_c_per_min -- assert that, so nobody later "simplifies"
     * the two rates into one field and makes a relaxed dead-element floor
     * mean a relaxed overheat reaction. */
    {
        thermal_guard_state_t s;
        thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f,
                                   .sanity_rate_c_per_min = 0.2f,
                                   .off_settle_s = 30.0f,
                                   .runaway_rate_c_per_min = 10.0f,
                                   .runaway_margin_c = 25.0f};
        thermal_guard_reset(&s);
        thermal_guard_input_t in = base_input();
        in.setpoint_c = 100.0f;
        in.measurement_c = 100.0f;
        in.commanded_duty = 0.0f; /* heat OFF, and still climbing -- welded contact */
        bool tripped = false;
        int ticks = 0;
        for (; ticks < 40 && !tripped; ticks++) {
            in.measurement_c += 3.0f; /* 18C/min, past runaway_rate_c_per_min */
            tripped = thermal_guard_tick(&s, &cfg, &in);
        }
        TEST_CHECK(tripped, "guard 3 still trips with sanity_rate_c_per_min at 0.2");
        TEST_CHECK(!tripped || s.reason == THERMAL_GUARD_TRIP_RUNAWAY, "reason is RUNAWAY");
        /* Reaction speed, not just eventual detection: off_settle_s(30s) + one
         * more tick is the earliest this can fire, and it must still be that. */
        TEST_CHECK(ticks <= 6, "and it fires as soon as the settle window allows -- the slow "
                               "dead-element floor does not delay overheat detection");
    }


    /* ------------------------------------------------------------------
     * Guard 1's ARRIVAL BAND (progress_band_c), added 2026-08-29.
     *
     * Found by a real three-segment firing on the bench: zone 0 was holding
     * 50.7 C against a 52.0 C setpoint at full duty -- settled, 1.3 C of
     * steady-state offset, exactly what a PID with finite gain does -- and
     * guard 1 aborted the whole firing with "heating but rose only -0.2C in
     * 1min". Demanding a rise from a loop that has arrived is demanding that
     * it overshoot.
     * ------------------------------------------------------------------ */

    /* THE REGRESSION. A settled dwell inside the band, at full duty, with the
     * temperature flat: must NOT trip. This is the exact bench scenario. */
    {
        thermal_guard_state_t s;
        thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f,
                                   .sanity_rate_c_per_min = 0.5f, .wrong_dir_window_s = 60.0f};
        thermal_guard_reset(&s);
        thermal_guard_input_t in = base_input();
        in.setpoint_c = 52.0f;
        in.commanded_duty = 1.0f;   /* holding hard against the losses */
        bool tripped = false;
        for (int i = 0; i < 200 && !tripped; i++) {
            /* Dithering by 0.1 C, not held bit-exact. A real settled junction
             * moves this much read to read, and holding it perfectly still
             * would trip guard 7 (FROZEN) instead -- which would make this
             * test pass or fail for a reason that has nothing to do with the
             * arrival band. 0.1 C exceeds FROZEN_EPS_C (0.05) so guard 7's
             * window keeps resetting, exactly as it does on hardware. */
            in.measurement_c = (i % 2) ? 50.8f : 50.7f;
            tripped = thermal_guard_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "a settled dwell 1.3C below setpoint at full duty does not trip guard 1 "
                             "-- not rising is what 'settled' MEANS");
    }

    /* NEGATIVE TEST for the band -- prove it can still fail. Same duty, same
     * flat temperature, but now genuinely far below setpoint: this is a
     * ramp with a dead element, and it MUST still trip. Without this, the
     * band above could have been implemented as "guard 1 never fires" and
     * the regression test would not have noticed. */
    {
        thermal_guard_state_t s;
        thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f,
                                   .sanity_rate_c_per_min = 0.5f, .wrong_dir_window_s = 60.0f};
        thermal_guard_reset(&s);
        thermal_guard_input_t in = base_input();
        in.setpoint_c = 500.0f;
        in.measurement_c = 50.7f;   /* 449 C below setpoint -- unambiguously climbing */
        in.commanded_duty = 1.0f;
        bool tripped = false;
        for (int i = 0; i < 200 && !tripped; i++) {
            tripped = thermal_guard_tick(&s, &cfg, &in);
        }
        TEST_CHECK(tripped, "a dead element on a RAMP still trips guard 1 -- the band did not "
                            "disable the guard, it scoped it");
        TEST_CHECK(!tripped || s.reason == THERMAL_GUARD_TRIP_HEATING_FAILED, "reason is HEATING_FAILED");
    }

    /* The band's edge, from the outside. Error just OUTSIDE the default 3 C
     * band still demands a rise -- so the boundary is where the comment says
     * it is, not several degrees away. */
    {
        thermal_guard_state_t s;
        thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f,
                                   .sanity_rate_c_per_min = 0.5f, .wrong_dir_window_s = 60.0f};
        thermal_guard_reset(&s);
        thermal_guard_input_t in = base_input();
        in.setpoint_c = 52.0f;
        in.measurement_c = 48.0f;   /* 4 C below setpoint -- outside the 3 C band */
        in.commanded_duty = 1.0f;
        bool tripped = false;
        for (int i = 0; i < 200 && !tripped; i++) {
            tripped = thermal_guard_tick(&s, &cfg, &in);
        }
        TEST_CHECK(tripped, "4 C below setpoint is outside the 3 C band, so guard 1 still demands a rise");
    }

    /* Inside the band is NOT a free pass: a zone that is FALLING while heat
     * is commanded is still caught, by guard 2's rate test. This is the case
     * that previously had no test at all -- before this change, 0 < error <=
     * band went to guard 1's rise check, and after it the falling check has
     * to be the thing that covers a dead element during a dwell. */
    {
        thermal_guard_state_t s;
        thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f,
                                   .sanity_rate_c_per_min = 0.5f, .wrong_dir_window_s = 60.0f,
                                   .wrong_dir_rate_c_per_min = 0.3f};
        thermal_guard_reset(&s);
        thermal_guard_input_t in = base_input();
        in.setpoint_c = 52.0f;
        in.measurement_c = 51.0f;   /* inside the band */
        in.commanded_duty = 1.0f;
        bool tripped = false;
        for (int i = 0; i < 200 && !tripped; i++) {
            /* 0.1 C per 10 s tick = 0.6 C/min. Deliberately slow: the zone
             * has to still be INSIDE the band when the 60 s window closes,
             * or it leaves the band on the way down and guard 1's rise check
             * catches it instead -- which would prove nothing about the
             * falling branch this case exists for. */
            in.measurement_c -= 0.1f;
            tripped = thermal_guard_tick(&s, &cfg, &in);
        }
        TEST_CHECK(tripped, "an element dying DURING a dwell still trips -- inside the band the zone "
                            "must not fall, even though it need not rise");
        TEST_CHECK(!tripped || s.reason == THERMAL_GUARD_TRIP_WRONG_DIRECTION, "reason is WRONG_DIRECTION");
    }

    /* progress_band_c is configurable on the same 0-means-default rule as
     * every other field, and a widened band really does widen. */
    {
        thermal_guard_state_t s;
        thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f,
                                   .sanity_rate_c_per_min = 0.5f, .wrong_dir_window_s = 60.0f,
                                   .progress_band_c = 10.0f};
        thermal_guard_reset(&s);
        thermal_guard_input_t in = base_input();
        in.setpoint_c = 52.0f;
        in.commanded_duty = 1.0f;
        bool tripped = false;
        for (int i = 0; i < 200 && !tripped; i++) {
            /* 8 C below setpoint: outside the 3 C default band, inside a
             * configured 10 C one. Dithered for the same guard-7 reason as
             * the settled-dwell case above. */
            in.measurement_c = (i % 2) ? 44.1f : 44.0f;
            tripped = thermal_guard_tick(&s, &cfg, &in);
        }
        TEST_CHECK(!tripped, "progress_band_c=10 puts an 8 C error inside the band, so no rise is demanded");
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
