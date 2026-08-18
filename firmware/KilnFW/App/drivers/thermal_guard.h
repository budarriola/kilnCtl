// thermal_guard -- the Klipper/Marlin-class thermal-protection suite.
// TODO.md section 6A.3.
//
// Pure function of (config, input) -> verdict, same host-testability
// reasoning as pid.h: no FreeRTOS, no ESP-IDF, no logging, no I/O, no
// relay access. The executor owns the state struct (one per zone) and acts
// on the verdict (drops relays, asserts a SAFETY_FAULT_SRC_* bit); this
// module never touches a relay itself.
//
// Implemented here: guards 1 (heating-failed/no-progress), 2
// (wrong-direction), 3 (runaway with heat off), 4 (drift at setpoint), 5
// (absolute limits), 6 (sensor validity), 7 (frozen sensor), and -- added
// 2026-08-12, once the executor gained concurrent multi-zone execution
// (TODO.md 6A.5(a)) -- 8 (cross-zone plausibility). Guard 8 ships
// **disabled by default**: its threshold is supposed to come from a
// measured cross-gain matrix, which no hardware has ever produced, so
// cross_zone_max_delta_c defaults to 0 (off) rather than to a hand-picked
// number pretending to be validated. See its field comment below.
// NOT implemented here:
//   - Guard 9 (control-tick liveness) is explicitly NOT this module's job
//     per TODO.md 6A.3/6A.7 ("a control loop cannot be its own watchdog") --
//     it's a second, independent task in profile_executor.c.
//
// Latching, always (TODO.md 6A.3's "common trip semantics"): once tripped,
// this module reports is_tripped == true on every subsequent call until the
// caller explicitly thermal_guard_clear()s it -- there is no auto-recovery
// when the underlying condition clears.
#ifndef THERMAL_GUARD_H
#define THERMAL_GUARD_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    THERMAL_GUARD_TRIP_NONE = 0,
    THERMAL_GUARD_TRIP_HEATING_FAILED, /* guard 1 */
    THERMAL_GUARD_TRIP_WRONG_DIRECTION, /* guard 2 */
    THERMAL_GUARD_TRIP_RUNAWAY,        /* guard 3 -- welded contact / shorted SSR */
    THERMAL_GUARD_TRIP_DRIFT,          /* guard 4 */
    THERMAL_GUARD_TRIP_MAX_TEMP,       /* guard 5 */
    THERMAL_GUARD_TRIP_MIN_TEMP,       /* guard 5 */
    THERMAL_GUARD_TRIP_SENSOR_INVALID, /* guard 6 */
    THERMAL_GUARD_TRIP_FROZEN,         /* guard 7 */
    THERMAL_GUARD_TRIP_CROSS_ZONE,     /* guard 8 */
} thermal_guard_trip_t;

/* Per-zone thresholds. max_temp_c/min_temp_c come from zone_cfg_t
 * (user-configurable), as do every field below through the rest of the
 * struct (TODO.md 6A.3's "every threshold above is config, not a constant" --
 * closed 2026-08-16 for the thresholds it names explicitly: wrong-dir
 * rate/window, off-settle, runaway rate/margin, drift period, sensor
 * debounce count, frozen window). Every one of these follows the same
 * "0 means not configured, thermal_guard.c substitutes its own firmware-wide
 * default" convention as sanity_rate_c_per_min -- unlike cross_zone_max_delta_c
 * below, 0 here does NOT disable the guard, since these protect against
 * failures a kiln can hit with no operator tuning at all and must stay armed
 * by default. Guard 4's own settle/drift band is DRIFT_HYSTERESIS_C
 * (thermal_guard.c, still a firmware constant -- not asked for by TODO.md's
 * list) -- deliberately NOT the bang-bang hysteresis (heater_output.h owns
 * that one; it's too tight for this guard, see thermal_guard.c's guard-4
 * comment). Guard 1's PROGRESS_WINDOW_S/PROGRESS_DUTY_MIN are likewise still
 * firmware constants -- also not named in TODO.md's list. */
typedef struct {
    float max_temp_c;              /* guard 5 -- mandatory in spirit; 0 means "not set", treated as no ceiling */
    float min_temp_c;               /* guard 5 -- default -20 */
    float sanity_rate_c_per_min;    /* guards 1/2's rate threshold -- zone_cfg_t.sanity_rate_c_per_min,
                                     * 0 substituted with a default by the caller (same rule
                                     * profile_executor.c already applies elsewhere) */
    float wrong_dir_window_s;       /* guard 2's rolling window; 0 -> WRONG_DIR_WINDOW_S */
    float wrong_dir_rate_c_per_min; /* guard 2's falling-rate trip threshold; 0 -> WRONG_DIR_RATE_C_PER_MIN */
    float off_settle_s;             /* guard 3's settle time before the rate/margin checks start; 0 -> OFF_SETTLE_S */
    float runaway_rate_c_per_min;   /* guard 3's rate trip threshold; 0 -> RUNAWAY_RATE_C_PER_MIN */
    float runaway_margin_c;         /* guard 3's absolute-rise trip threshold; 0 -> RUNAWAY_MARGIN_C */
    float drift_period_s;           /* guard 4's sustained-excursion window; 0 -> DRIFT_PERIOD_S */
    float sensor_fault_debounce_ticks; /* guard 6's consecutive-bad-read count; 0 -> SENSOR_FAULT_DEBOUNCE_TICKS */
    float frozen_window_s;          /* guard 7's value-identity window; 0 -> FROZEN_WINDOW_S */
    /* Guard 8. **0 disables the check**, and that is the shipped default:
     * TODO.md 6A.5's last bullet is explicit that this threshold should be
     * informed by the measured cross-gain matrix K, and no real K has ever
     * been captured (no thermocouple hardware). Hard-coding a plausible
     * number and calling the guard "done" is exactly what that bullet says
     * not to do, so the mechanism ships disabled and the number stays the
     * operator's (or, later, the autotune matrix's) to supply. A kiln that
     * genuinely stratifies needs a generous value -- 150C is the figure
     * TODO.md floats as a starting guess, not a validated one. */
    float cross_zone_max_delta_c;
    float cross_zone_period_s;      /* 0 substituted with CROSS_ZONE_PERIOD_S_DEFAULT (600s) */
} thermal_guard_cfg_t;

/* One call's worth of input. measurement_c must be the RAW (uncalibrated)
 * reading -- TODO.md 6A.7 is explicit that a calibration offset must not be
 * able to hide an out-of-range sensor from guards 5/6. sensor_ok folds
 * together spi_failed/isnan/THERMO_FAULT_OPEN|OVUV|TCRANGE -- the caller
 * (which already has MAX31856Reading) computes this so thermal_guard.c
 * doesn't need to know that header's bit layout.
 *
 * TODO.md 10.8 (multi-thermocouple-per-zone, 2026-08-17): with more than one
 * thermocouple channel assigned to a zone, sensor_ok/measurement_c are the
 * zone's COMBINED reading (thermo_combine.c's mean of that zone's valid
 * channels), not one channel's. Guard 6's "invalid" therefore already means
 * "every channel assigned to this zone is invalid" wherever this struct's
 * sensor_ok came from a combined read -- the caller (profile_executor.c)
 * is what changed to produce that; this module's own guard-6 code below is
 * unchanged, because it was always written against an opaque caller-decided
 * bool and never needed to know how many channels fed it. */
typedef struct {
    bool  sensor_ok;
    float measurement_c;
    float setpoint_c;
    float commanded_duty; /* what this tick decided to drive, 0..1, AFTER any
                           * safety-refusal -- guards reason about what was
                           * actually commanded, not what control wanted */
    float dt_s;

    /* Guard 8's view of the other zones this tick, from the SAME snapshot
     * (profile_executor.c reads every channel once per tick precisely so
     * control, guards, and history see one consistent picture). Entries are
     * raw readings, same contract as measurement_c above; peer_ok[i] false
     * means that channel had no trustworthy reading and is skipped rather
     * than compared against garbage. peer_index_self names this zone's own
     * slot so it can be excluded. Leave peer_count 0 (and the pointers NULL)
     * to skip guard 8 entirely -- a single-zone run has nothing to compare
     * against, which is one of the two reasons the guard can be inactive;
     * the other is cross_zone_max_delta_c == 0. */
    const float *peer_c;
    const bool  *peer_ok;
    uint8_t      peer_count;
    uint8_t      peer_index_self;
} thermal_guard_input_t;

typedef struct {
    bool     is_tripped;
    thermal_guard_trip_t reason;
    char     detail[96];

    /* Guards 1/2: rolling window over commanded-heat periods. */
    bool  progress_window_active;
    float progress_window_start_c;
    float progress_window_elapsed_s;

    /* Guard 3: sustained duty==0 while temperature still rises.
     * runaway_baseline_c is where the temperature was when heat was commanded
     * off (the margin check's reference); the rate check uses its own
     * baseline, latched when the settle window ends, so its rise and its
     * elapsed time cover the same interval -- see thermal_guard.c for the
     * false positive that cost. */
    bool  off_window_active;
    float off_window_elapsed_s;
    float runaway_baseline_c;
    bool  runaway_rate_baseline_valid;
    float runaway_rate_baseline_c;
    float runaway_rate_elapsed_s;

    /* Guard 4: at_setpoint_window_active latches true once the zone has ever
     * settled within DRIFT_HYSTERESIS_C of setpoint; at_setpoint_elapsed_s
     * then tracks continuous time spent OUTSIDE that band since settling
     * (reset to 0 on every tick back inside it) -- see thermal_guard.c. */
    bool  at_setpoint_window_active;
    float at_setpoint_elapsed_s;

    /* Guard 6: consecutive-bad-read debounce. */
    uint8_t sensor_fault_streak;

    /* Guard 7: value-identity window. */
    bool  frozen_window_active;
    float frozen_last_c;
    float frozen_elapsed_s;

    /* Guard 8: continuous time this zone has been implausibly far from its
     * furthest-away peer. Reset the moment it comes back inside the band --
     * real kilns stratify transiently, and only a *sustained* disagreement
     * says a thermocouple has left the building. */
    float cross_zone_elapsed_s;
} thermal_guard_state_t;

void thermal_guard_reset(thermal_guard_state_t *state);

/* Explicit operator acknowledgement -- the only way out of a latched trip
 * (TODO.md 6A.3: "no auto-recovery when the condition clears"). Resets
 * every window too, so the zone starts the next run clean. */
void thermal_guard_clear(thermal_guard_state_t *state);

/* Evaluates one tick. Returns true if this call is the one that newly
 * tripped it (state->is_tripped was false, now true) -- the caller uses
 * this to log/assert the fault source exactly once, not every tick
 * afterward. Once tripped, further calls are no-ops that just re-report
 * is_tripped == true until thermal_guard_clear(). */
bool thermal_guard_tick(thermal_guard_state_t *state, const thermal_guard_cfg_t *cfg,
                        const thermal_guard_input_t *in);

#ifdef __cplusplus
}
#endif

#endif // THERMAL_GUARD_H
