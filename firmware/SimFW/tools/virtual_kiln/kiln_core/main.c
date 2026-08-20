// main.c -- virtual_kiln's "control core": a tiny stdio batch harness around
// the REAL, UNMODIFIED KilnFW control/guard/render code:
//
//   firmware/KilnFW/App/drivers/pid.c            (pid_reset/pid_update_terms)
//   firmware/KilnFW/App/drivers/thermal_guard.c  (thermal_guard_reset/_tick)
//   firmware/KilnFW/App/drivers/heater_output.c  (heater_output_reset/_duty)
//
// compiled verbatim (see build_host.ps1) exactly the way
// firmware/KilnFW/App/test/build_host_tests.ps1 already proves these three
// files (plus thermo_combine.c, not needed here for a single TC channel)
// are pure, portable C. None of those .c files is modified, wrapped, or
// reimplemented by anything in this directory -- see this directory's
// README.md for the precise file-by-file accounting, and in particular for
// what is explicitly OUT of scope: profile_executor.c's ramp/dwell segment
// stepping, multi-zone ramp-lock, feedforward, relay_authority gating, and
// config-reload-while-running are NOT reproduced here, because that logic
// lives only in profile_executor.c (FreeRTOS/kiln_io/HTTP/NVS-coupled, not
// host-compilable) and reproducing it by hand in this harness would be
// exactly the "second implementation" the parent task's brief forbids.
// This harness drives ONE zone against a FIXED, externally-supplied
// setpoint -- "does PID + thermal_guard + heater_output regulate a zone and
// command relays correctly against real simulated sensor data", not "does a
// multi-segment firing profile execute correctly".
//
// What this file is NOT: profile_executor.c itself, which cannot be
// host-compiled (FreeRTOS.h, freertos/semphr.h, freertos/task.h, esp_log.h,
// and its own kiln_io_owner/zones_http/safety_link/run_state/sim_backend
// dependencies are all ESP-IDF/FreeRTOS-shaped -- see this directory's
// README.md for the file-by-file grep evidence). This file is a
// single-threaded, stdio-driven REPLACEMENT for a narrow slice of that
// task's *outer loop* only, for exactly ONE zone: it builds the same
// thermal_guard_input_t/pid inputs profile_executor.c's tick function
// builds for a PID-mode, single-thermocouple, no-feedforward zone (see
// that file's control loop, "Control mode, per active zone" /
// "Apply relays + guards, per active zone"), and calls the same library
// functions in the same order. See README.md, "Faithfulness to
// profile_executor.c's tick loop", for the field-by-field justification of
// every value below.
//
// Line protocol on stdin/stdout (text, one line in -> one line out; same
// design as virtual_dut/dut_core/main.c and for the same reason: the Python
// orchestrator already owns the real benchproto/TCP conversation with
// virtual_simfw and should not also have to speak benchproto in C):
//
//   RESET
//     -> resets pid_state (pid_reset), thermal_guard_state
//        (thermal_guard_reset), heater_output_state (heater_output_reset).
//        Reply: "OK".
//
//   CONFIG <kp> <ki> <kd> <d_filter_tau_s> <b> <pid_range_c>
//          <max_temp_c> <min_temp_c> <sanity_rate_c_per_min>
//          <window_ms> <min_on_ms> <min_off_ms>
//     -> overrides the default pid_cfg_t/thermal_guard_cfg_t/
//        heater_output_cfg_t values (see DEFAULT_* below) before the next
//        RESET. Does not itself reset state. Reply: "OK".
//
//   TICK <measurement_c> <sensor_ok:0|1> <setpoint_c> <dt_s>
//     -> one control tick for the single zone this process models.
//        measurement_c is the RAW (uncalibrated) combined reading (no
//        calibration offset is applied here -- this harness has no zone
//        config to read one from, and thermal_guard.c's own contract
//        requires the raw value regardless). If the zone's guard is
//        already latched tripped, this tick is a no-op that just re-reports
//        the latched state (mirrors profile_executor.c's "if
//        (s_exec.zones[zi].faulted) continue" -- a faulted zone is never
//        ticked again). Otherwise:
//          duty = sensor_ok ? pid_update_terms(..., ff_u=0.0f, &terms) : 0.0f
//          relay_on = heater_output_duty(&heater_state, &heater_cfg, duty, dt_ms)
//          guard tick with commanded_duty = relay_on ? (duty>0?duty:1.0f) : 0.0f
//        (profile_executor.c's own commanded_duty expression, verbatim --
//        see this directory's README).
//        On the tick that newly trips the guard, the relay is forced off
//        the same tick (mirrors escalate_guard_trip()'s force_zone_relay_off()
//        always following a trip, whether the caller treats it as a
//        whole-run fault or a per-zone one -- irrelevant here, there is only
//        one zone).
//        Reply is one space-separated line:
//
//          <is_tripped:0|1> <reason:int> <relay_on:0|1> <duty:float>
//          <p:float> <i:float> <d:float> <ff:float>
//
//        `reason` is thermal_guard_trip_t's numeric value (thermal_guard.h)
//        -- 0 == THERMAL_GUARD_TRIP_NONE.
//
// Any unrecognized line gets "ERR unknown command" and is otherwise
// ignored (does not crash, does not tick).
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

#include "pid.h"
#include "thermal_guard.h"
#include "heater_output.h"

// profile_executor.c's own defaults (see that file's top-of-file #defines):
// HEATER_WINDOW_MS/HEATER_MIN_ON_MS/HEATER_MIN_OFF_MS, "used when a zone
// hasn't configured its own via zones_config_get_heater_cfg()". Hand-copied
// here for the same reason virtual_dut/dut_core/main.c hand-copies
// SAFTYFW_PERIOD_SAFETY_CORE_MS: profile_executor.c itself is not
// host-compilable, so there is nothing to #include the constant from.
#define DEFAULT_WINDOW_MS 60000u
#define DEFAULT_MIN_ON_MS 2000u
#define DEFAULT_MIN_OFF_MS 2000u

// thermal_guard.c's own fallback defaults (substituted whenever a cfg field
// is 0) -- max_temp_c/min_temp_c/sanity_rate_c_per_min are the three this
// harness exposes via CONFIG; the rest (wrong-dir/off-settle/runaway/
// drift/debounce/frozen) are left at 0 so thermal_guard.c's own internal
// fallbacks apply, exactly as a zone that has never had those specific
// TODO.md 6A.7 thresholds edited would see in real firmware.
#define DEFAULT_MAX_TEMP_C 1300.0f
#define DEFAULT_MIN_TEMP_C (-20.0f)
#define DEFAULT_SANITY_RATE_C_PER_MIN 0.5f

// Not read from any real zone config -- that is runtime NVS data, invisible
// to a static host build (see README.md). These are the SAME gains
// firmware's own host closed-loop test uses to validate this exact PID +
// thermal_guard + heater_output wiring (firmware/KilnFW/App/test/
// test_closed_loop.c), traceable to a real, already-host-tested source
// rather than invented for this file. A caller driving a real scenario
// against real per-kiln tuning overrides these via CONFIG.
#define DEFAULT_KP 0.01f
#define DEFAULT_KI 0.0005f
#define DEFAULT_KD 0.05f
#define DEFAULT_D_FILTER_TAU_S 30.0f
#define DEFAULT_B 1.0f
#define DEFAULT_PID_RANGE_C 50.0f

static pid_cfg_t s_pid_cfg;
static pid_state_t s_pid_state;
static thermal_guard_cfg_t s_guard_cfg;
static thermal_guard_state_t s_guard_state;
static heater_output_cfg_t s_heater_cfg;
static heater_output_state_t s_heater_state;

static void set_defaults(void)
{
    s_pid_cfg = (pid_cfg_t){
        .kp = DEFAULT_KP, .ki = DEFAULT_KI, .kd = DEFAULT_KD,
        .d_filter_tau_s = DEFAULT_D_FILTER_TAU_S, .b = DEFAULT_B,
        .pid_range_c = DEFAULT_PID_RANGE_C,
    };
    s_guard_cfg = (thermal_guard_cfg_t){
        .max_temp_c = DEFAULT_MAX_TEMP_C, .min_temp_c = DEFAULT_MIN_TEMP_C,
        .sanity_rate_c_per_min = DEFAULT_SANITY_RATE_C_PER_MIN,
        /* every other field 0 -> thermal_guard.c's own internal fallback */
    };
    s_heater_cfg = (heater_output_cfg_t){
        .window_ms = DEFAULT_WINDOW_MS, .min_on_ms = DEFAULT_MIN_ON_MS,
        .min_off_ms = DEFAULT_MIN_OFF_MS,
    };
}

static void do_reset(void)
{
    pid_reset(&s_pid_state);
    thermal_guard_reset(&s_guard_state);
    heater_output_reset(&s_heater_state);
    printf("OK\n");
    fflush(stdout);
}

static void do_config(const char *args)
{
    pid_cfg_t p = s_pid_cfg;
    thermal_guard_cfg_t g = s_guard_cfg;
    heater_output_cfg_t h = s_heater_cfg;
    unsigned window_ms = h.window_ms, min_on_ms = h.min_on_ms, min_off_ms = h.min_off_ms;
    int n = sscanf(args, "%f %f %f %f %f %f %f %f %f %u %u %u",
                   &p.kp, &p.ki, &p.kd, &p.d_filter_tau_s, &p.b, &p.pid_range_c,
                   &g.max_temp_c, &g.min_temp_c, &g.sanity_rate_c_per_min,
                   &window_ms, &min_on_ms, &min_off_ms);
    if (n != 12) {
        printf("ERR bad CONFIG args (need 12, got %d)\n", n);
        fflush(stdout);
        return;
    }
    h.window_ms = window_ms;
    h.min_on_ms = min_on_ms;
    h.min_off_ms = min_off_ms;
    s_pid_cfg = p;
    s_guard_cfg = g;
    s_heater_cfg = h;
    printf("OK\n");
    fflush(stdout);
}

static void do_tick(float measurement_c, int sensor_ok_flag, float setpoint_c, float dt_s)
{
    bool sensor_ok = sensor_ok_flag != 0;

    if (s_guard_state.is_tripped) {
        // Mirrors profile_executor.c's "if (... || s_exec.zones[zi].faulted)
        // continue" -- a latched zone is never ticked again; the relay
        // stays off (force_zone_relay_off() already ran the tick it
        // tripped). No pid_update_terms()/heater_output_duty() call here on
        // purpose, same as real firmware.
        printf("%d %d %d %.6f %.6f %.6f %.6f %.6f\n",
               1, (int)s_guard_state.reason, 0, 0.0, 0.0, 0.0, 0.0, 0.0);
        fflush(stdout);
        return;
    }

    uint32_t dt_ms = (uint32_t)(dt_s * 1000.0f + 0.5f);

    // profile_executor.c's ZONE_CONTROL_MODE_PID branch, ff_u hardcoded to
    // 0.0f (no autotune-identified plant model exists in this harness --
    // see README.md; this is byte-for-byte the behavior of every zone that
    // has never been autotuned in real firmware, not a simplification).
    float duty = 0.0f;
    pid_terms_t terms = {0};
    if (sensor_ok) {
        duty = pid_update_terms(&s_pid_state, &s_pid_cfg, setpoint_c, measurement_c, dt_s, 0.0f, &terms);
    }

    bool relay_on = heater_output_duty(&s_heater_state, &s_heater_cfg, duty, dt_ms);

    // profile_executor.c's own commanded_duty expression, verbatim:
    // "z->relay_commanded_on ? (z->duty > 0.0f ? z->duty : 1.0f) : 0.0f".
    float commanded_duty = relay_on ? (duty > 0.0f ? duty : 1.0f) : 0.0f;

    thermal_guard_input_t gin = {
        .sensor_ok = sensor_ok,
        .measurement_c = measurement_c, // RAW, no calibration offset (see header comment)
        .setpoint_c = setpoint_c,
        .commanded_duty = commanded_duty,
        .dt_s = dt_s,
        .peer_c = NULL, .peer_ok = NULL, .peer_count = 0, .peer_index_self = 0, // guard 8 inert: single zone, nothing to compare against
    };
    bool newly_tripped = thermal_guard_tick(&s_guard_state, &s_guard_cfg, &gin);

    if (newly_tripped) {
        // escalate_guard_trip()'s force_zone_relay_off() always follows a
        // trip, on the same tick.
        heater_output_force_off(&s_heater_state);
        relay_on = false;
        duty = 0.0f;
    }

    printf("%d %d %d %.6f %.6f %.6f %.6f %.6f\n",
           s_guard_state.is_tripped ? 1 : 0, (int)s_guard_state.reason,
           relay_on ? 1 : 0, (double)duty,
           (double)terms.p, (double)terms.i, (double)terms.d, (double)terms.ff);
    fflush(stdout);
}

int main(void)
{
    set_defaults();
    do_reset(); // boot state: relays off, integral 0, guard clear.

    char line[256];
    while (fgets(line, sizeof(line), stdin) != NULL) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }

        if (strncmp(line, "RESET", 5) == 0) {
            do_reset();
        } else if (strncmp(line, "CONFIG ", 7) == 0) {
            do_config(line + 7);
        } else if (strncmp(line, "TICK ", 5) == 0) {
            float measurement_c = 0.0f, setpoint_c = 0.0f, dt_s = 0.0f;
            int sensor_ok = 0;
            if (sscanf(line + 5, "%f %d %f %f", &measurement_c, &sensor_ok, &setpoint_c, &dt_s) == 4) {
                do_tick(measurement_c, sensor_ok, setpoint_c, dt_s);
            } else {
                printf("ERR bad TICK args\n");
                fflush(stdout);
            }
        } else if (strlen(line) == 0) {
            // ignore blank lines
        } else {
            printf("ERR unknown command\n");
            fflush(stdout);
        }
    }
    return 0;
}
