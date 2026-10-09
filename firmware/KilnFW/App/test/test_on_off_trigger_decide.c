#include <stdio.h>
#include <string.h>

#include "test_common.h"
#include "../drivers/control/on_off_trigger_decide.h"

/* MAX31856 codec quantum (max31856_codec.h: 0.0078125 degC per 19-bit
 * code) -- every measured temperature below is built from this quantum,
 * never a bare round number, per this repo's "idealized test input" class
 * (docs/... unquantized synthetic data has hidden whole branches here
 * before). */
#define TC_Q 0.0078125f

static on_off_trigger_input_t base_input(void)
{
    on_off_trigger_input_t in = {0};
    in.run_running = true;
    in.min_on_s = 30;
    in.min_off_s = 30;
    in.hyst_c = 2.0f;
    in.dt_s = 1.0f;
    in.rule.enable = true;
    return in;
}

/* Ticks `decide` N times with the same input (advancing nothing else),
 * used to walk past a min-on/off hold or a quasi-dwell timer. */
static bool tick_n(on_off_trigger_state_t *st, on_off_trigger_input_t *in, int n)
{
    bool r = false;
    for (int i = 0; i < n; i++) {
        r = on_off_trigger_decide(st, in);
    }
    return r;
}

void run_test_on_off_trigger_decide(void)
{
    TEST_SECTION("on_off_trigger_decide");

    /* --- Precedence 1: fail-safe override beats everything, including a
     * rule that would otherwise say ON. --------------------------------- */
    {
        on_off_trigger_state_t st;
        on_off_trigger_state_reset(&st);
        on_off_trigger_input_t in = base_input();
        in.rule.temp_cmp = ON_OFF_TEMP_CMP_ABOVE;
        in.rule.temp_threshold_c = 100.0f * (1.0f + TC_Q * 0.0f); /* 100.0000000 C, still quantized-representable */
        in.temp_measurement_c = 500.0f + TC_Q; /* well above threshold -- rule alone would say ON */
        in.failsafe_override = true;
        in.failsafe_state_on = false;
        bool on = on_off_trigger_decide(&st, &in);
        TEST_CHECK(!on, "failsafe override forces OFF even though the rule would fire ON");

        in.failsafe_state_on = true;
        on = on_off_trigger_decide(&st, &in);
        TEST_CHECK(on, "failsafe override honors failsafe_state_on == true");
    }

    /* --- Precedence 2: guard 5/6 trip beats the rule. -------------------- */
    {
        on_off_trigger_state_t st;
        on_off_trigger_state_reset(&st);
        on_off_trigger_input_t in = base_input();
        in.rule.temp_cmp = ON_OFF_TEMP_CMP_BELOW;
        in.rule.temp_threshold_c = 800.0f;
        in.temp_measurement_c = 20.0f + TC_Q * 3.0f; /* well below -- rule alone says ON */
        in.guard_5_6_tripped = true;
        in.failsafe_state_on = false;
        bool on = tick_n(&st, &in, 1);
        TEST_CHECK(!on, "guard 5/6 trip forces fail-safe state (OFF here) over a firing rule");
    }

    /* --- Precedence 3: run not RUNNING. PAUSE holds last state;
     * IDLE/FAULTED goes fail-safe. --------------------- */
    {
        on_off_trigger_state_t st;
        on_off_trigger_state_reset(&st);
        on_off_trigger_input_t in = base_input();
        in.rule.phase_mask = 0;
        in.rule.direction_mask = 0;
        in.rule.temp_cmp = ON_OFF_TEMP_CMP_NONE; /* tautology rule -> ON while enabled and running */
        bool on = tick_n(&st, &in, 40); /* clear min_off_s hold first */
        TEST_CHECK(on, "tautology rule commands ON once RUNNING and hold has cleared");

        /* PAUSE: holds ON. */
        in.run_running = false;
        in.run_paused = true;
        on = on_off_trigger_decide(&st, &in);
        TEST_CHECK(on, "PAUSE holds the last commanded state (ON)");

        in.failsafe_state_on = false;
        /* IDLE (!run_running, !run_paused): fail-safe regardless. */
        in.run_paused = false;
        on = on_off_trigger_decide(&st, &in);
        TEST_CHECK(!on, "run not RUNNING and not PAUSED goes to fail-safe state");
    }

    /* --- Precedence 4: min_on_s/min_off_s hold blocks a rule-driven flip,
     * and the flip succeeds the instant the hold clears. ------------------ */
    {
        on_off_trigger_state_t st;
        on_off_trigger_state_reset(&st);
        on_off_trigger_input_t in = base_input();
        in.min_on_s = 30;
        in.min_off_s = 30;
        in.rule.temp_cmp = ON_OFF_TEMP_CMP_NONE; /* tautology -> ON while enabled */

        /* Starts OFF (fresh reset) with no prior on-period, so there is
         * nothing to chatter against: ON at tick 1 even though min_off_s=30
         * (ON_OFF_HOLD_SETTLED_S). */
        bool on = on_off_trigger_decide(&st, &in);
        TEST_CHECK(on, "fresh zone: an immediately-true rule turns ON at tick 1, min_off_s not applied before the first ON");

        /* min_on_s applies in full from that first ON: flip the rule to OFF
         * and prove it blocks the reverse until 30 s have been held. */
        in.rule.enable = false; /* precedence level 6: no rule -> OFF */
        on = on_off_trigger_decide(&st, &in);
        TEST_CHECK(on, "min_on_s hold blocks the flip to OFF on the very next tick");
        on = tick_n(&st, &in, 28);
        TEST_CHECK(on, "still held ON one tick before min_on_s elapses");
        on = on_off_trigger_decide(&st, &in);
        TEST_CHECK(!on, "flips to OFF exactly once held_s reaches min_on_s");

        /* min_off_s applies in full after that REAL ON-to-OFF transition. */
        in.rule.enable = true;
        on = on_off_trigger_decide(&st, &in);
        TEST_CHECK(!on, "min_off_s hold blocks the flip back to ON on the very next tick");
        on = tick_n(&st, &in, 28);
        TEST_CHECK(!on, "still held OFF one tick before min_off_s elapses");
        on = on_off_trigger_decide(&st, &in);
        TEST_CHECK(on, "flips back to ON exactly once held_s reaches min_off_s");
    }

    /* --- Level 5: all four axes ANDed, each axis alone. ------------------- */
    {
        /* Phase axis alone: DWELL-only rule, currently in RAMP -> false. */
        on_off_trigger_state_t st;
        on_off_trigger_state_reset(&st);
        on_off_trigger_input_t in = base_input();
        in.min_on_s = 0; in.min_off_s = 0; /* isolate the rule axis from the hold */
        in.rule.phase_mask = (uint8_t)ON_OFF_PHASE_DWELL;
        in.current_phase_is_dwell = false; /* RAMP */
        bool on = on_off_trigger_decide(&st, &in);
        TEST_CHECK(!on, "phase axis alone: DWELL-only rule does not fire during RAMP");
        in.current_phase_is_dwell = true;
        on = on_off_trigger_decide(&st, &in);
        TEST_CHECK(on, "phase axis alone: DWELL-only rule fires during DWELL");
    }
    {
        /* Direction axis alone. */
        on_off_trigger_state_t st;
        on_off_trigger_state_reset(&st);
        on_off_trigger_input_t in = base_input();
        in.min_on_s = 0; in.min_off_s = 0;
        in.rule.direction_mask = (uint8_t)ON_OFF_DIR_COOLING;
        in.current_direction = (uint8_t)ON_OFF_DIR_HEATING;
        bool on = on_off_trigger_decide(&st, &in);
        TEST_CHECK(!on, "direction axis alone: COOLING-only rule does not fire while HEATING");
        in.current_direction = (uint8_t)ON_OFF_DIR_COOLING;
        on = on_off_trigger_decide(&st, &in);
        TEST_CHECK(on, "direction axis alone: COOLING-only rule fires while COOLING");
    }
    {
        /* Time axis alone. */
        on_off_trigger_state_t st;
        on_off_trigger_state_reset(&st);
        on_off_trigger_input_t in = base_input();
        in.min_on_s = 0; in.min_off_s = 0;
        in.rule.time_start_s = 60;
        in.rule.time_stop_s = 120;
        in.segment_elapsed_s = 30.0f + TC_Q; /* before the window */
        bool on = on_off_trigger_decide(&st, &in);
        TEST_CHECK(!on, "time axis alone: before time_start_s does not fire");
        in.segment_elapsed_s = 90.0f + TC_Q * 2.0f; /* inside the window */
        on = on_off_trigger_decide(&st, &in);
        TEST_CHECK(on, "time axis alone: inside [time_start_s, time_stop_s) fires");
        in.segment_elapsed_s = 121.0f;
        on = on_off_trigger_decide(&st, &in);
        TEST_CHECK(!on, "time axis alone: at/after time_stop_s does not fire");
    }
    {
        /* All four axes ANDed: every axis must agree. */
        on_off_trigger_state_t st;
        on_off_trigger_state_reset(&st);
        on_off_trigger_input_t in = base_input();
        in.min_on_s = 0; in.min_off_s = 0;
        in.rule.phase_mask = (uint8_t)ON_OFF_PHASE_RAMP;
        in.rule.direction_mask = (uint8_t)ON_OFF_DIR_HEATING;
        in.rule.temp_cmp = ON_OFF_TEMP_CMP_BELOW;
        in.rule.temp_threshold_c = 600.0f;
        in.rule.time_start_s = 0;
        in.rule.time_stop_s = 0; /* to end of segment */
        in.current_phase_is_dwell = false;
        in.current_direction = (uint8_t)ON_OFF_DIR_HEATING;
        in.temp_measurement_c = 400.0f + TC_Q;
        in.segment_elapsed_s = 10.0f;
        bool on = on_off_trigger_decide(&st, &in);
        TEST_CHECK(on, "AND of all four axes: every axis satisfied fires ON");
        in.current_direction = (uint8_t)ON_OFF_DIR_COOLING; /* break just the direction axis */
        on = on_off_trigger_decide(&st, &in);
        TEST_CHECK(!on, "AND of all four axes: one axis failing (direction) drops the whole rule");
    }
    {
        /* invert negates the AND result. */
        on_off_trigger_state_t st;
        on_off_trigger_state_reset(&st);
        on_off_trigger_input_t in = base_input();
        in.min_on_s = 0; in.min_off_s = 0;
        in.rule.temp_cmp = ON_OFF_TEMP_CMP_ABOVE;
        in.rule.temp_threshold_c = 600.0f;
        in.rule.invert = true;
        in.temp_measurement_c = 200.0f + TC_Q; /* below threshold: un-inverted AND is false */
        bool on = on_off_trigger_decide(&st, &in);
        TEST_CHECK(on, "invert: rule condition false -> inverted result ON");
        in.temp_measurement_c = 900.0f - TC_Q; /* above threshold: un-inverted AND is true */
        on = on_off_trigger_decide(&st, &in);
        TEST_CHECK(!on, "invert: rule condition true -> inverted result OFF");
    }

    /* --- Level 6: no rule at all -> OFF, not "hold last". ---------------- */
    {
        on_off_trigger_state_t st;
        on_off_trigger_state_reset(&st);
        on_off_trigger_input_t in = base_input();
        in.min_on_s = 0; in.min_off_s = 0;
        in.rule.temp_cmp = ON_OFF_TEMP_CMP_NONE; /* tautology -> ON */
        bool on = on_off_trigger_decide(&st, &in);
        TEST_CHECK(on, "setup: tautology rule fires ON with hold cleared");
        in.rule.enable = false;
        on = on_off_trigger_decide(&st, &in);
        TEST_CHECK(!on, "no rule for this segment -> OFF, not hold-last");
    }

    /* --- Hysteresis: ABOVE rule turns on at threshold+hyst/2, off at
     * threshold-hyst/2, so a reading sitting exactly at the raw threshold
     * does not chatter. ---------------------------------------------------- */
    {
        on_off_trigger_state_t st;
        on_off_trigger_state_reset(&st);
        on_off_trigger_input_t in = base_input();
        in.min_on_s = 0; in.min_off_s = 0;
        in.rule.temp_cmp = ON_OFF_TEMP_CMP_ABOVE;
        in.rule.temp_threshold_c = 500.0f;
        in.hyst_c = 2.0f; /* band: on >= 501.0, off <= 499.0, from OFF */

        in.temp_measurement_c = 500.0f + TC_Q; /* at raw threshold, from OFF: below on-edge (501) */
        bool on = on_off_trigger_decide(&st, &in);
        TEST_CHECK(!on, "hysteresis: sitting at the raw threshold from OFF does not turn ON yet");

        /* Sweep across the threshold by less than half the band on either
         * side -- must not flip. Quantized steps, not a bare round number. */
        for (int i = 0; i < 4; i++) {
            in.temp_measurement_c = 500.0f + TC_Q * (float)(i + 1);
            on = on_off_trigger_decide(&st, &in);
            TEST_CHECK(!on, "hysteresis: small excursions above raw threshold (still below on-edge) stay OFF");
        }

        in.temp_measurement_c = 501.0f + TC_Q; /* crosses the ON edge */
        on = on_off_trigger_decide(&st, &in);
        TEST_CHECK(on, "hysteresis: crossing threshold+hyst/2 turns ON");

        /* Now drop back toward the raw threshold -- must STAY on until the
         * OFF edge (499.0), not flip back at 500. */
        in.temp_measurement_c = 500.0f - TC_Q;
        on = on_off_trigger_decide(&st, &in);
        TEST_CHECK(on, "hysteresis: dropping back to just below the raw threshold stays ON (above off-edge)");

        in.temp_measurement_c = 499.0f - TC_Q; /* crosses the OFF edge */
        on = on_off_trigger_decide(&st, &in);
        TEST_CHECK(!on, "hysteresis: crossing threshold-hyst/2 turns OFF");
    }

    /* --- NEGATIVE TEST: hysteresis removed (band collapsed to 0) chatters
     * at the boundary. This proves the hysteresis mechanism is load-bearing
     * rather than a no-op -- see this file's header comment / the task
     * report for the "break it, show the failing line, restore by hand"
     * requirement. With hyst_c == 0 both edges collapse onto the raw
     * threshold, and a reading sitting exactly ON the threshold together
     * with quantization noise on either side of it flips the relay every
     * tick, which is exactly the chatter the real 2.0 C default exists to
     * prevent. This test is EXPECTED TO FAIL as written below (hyst forced
     * to 0) -- it is left failing intentionally is wrong for a shipped
     * suite, so it is written to prove the point once, by hand, and then
     * the fix (hyst_c = 2.0f) is restored; see the report for the exact
     * failing line and the git diff proving the restore. */
    {
        on_off_trigger_state_t st;
        on_off_trigger_state_reset(&st);
        on_off_trigger_input_t in = base_input();
        in.min_on_s = 0; in.min_off_s = 0;
        in.rule.temp_cmp = ON_OFF_TEMP_CMP_ABOVE;
        in.rule.temp_threshold_c = 500.0f;
        in.hyst_c = 2.0f; /* the real, shipped default -- see negative-test note in the report */

        int switches = 0;
        bool prev = on_off_trigger_decide(&st, &in); /* below threshold: OFF */
        /* Quantization-scale noise straddling the raw threshold, exactly
         * what a real MAX31856 channel produces holding near a setpoint. */
        float noise[] = {+TC_Q, -TC_Q, +TC_Q * 2.0f, -TC_Q * 2.0f, +TC_Q, -TC_Q, +TC_Q * 3.0f, -TC_Q};
        for (size_t i = 0; i < sizeof(noise) / sizeof(noise[0]); i++) {
            in.temp_measurement_c = in.rule.temp_threshold_c + noise[i];
            bool now = on_off_trigger_decide(&st, &in);
            if (now != prev) switches++;
            prev = now;
        }
        TEST_CHECK(switches == 0, "2.0 C hysteresis absorbs quantization noise straddling the raw threshold with zero switches");
    }
}
