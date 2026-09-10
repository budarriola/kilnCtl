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
    THERMAL_GUARD_TRIP_MIN_TEMP,       /* guard 6 */
    THERMAL_GUARD_TRIP_SENSOR_INVALID, /* guard 7 */
    THERMAL_GUARD_TRIP_FROZEN,         /* guard 8 */
    THERMAL_GUARD_TRIP_CROSS_ZONE,     /* guard 9 */
    THERMAL_GUARD_TRIP_RELAY_STALLED,  /* guards 1/2's relay-cycling discriminator --
                                        * see thermal_guard_input_t.relay_min_swing_c */
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
    /* 2026-08-27: the four thresholds this module used to hold as firmware
     * constants with no operator override at all -- the owner's "i dont realy
     * like magic numbers". Same 0-substitutes-a-default rule as every field
     * above; 0 does not disable. The named constants stay in thermal_guard.c
     * as the documented defaults. */
    float progress_duty_min;   /* guard 1 arms at/above this duty; 0 -> PROGRESS_DUTY_MIN */
    float progress_window_s;   /* guard 1's window; 0 -> PROGRESS_WINDOW_S. Note
                                * wrong_dir_window_s, if set, still overrides BOTH
                                * guards -- see thermal_guard.c's comment there. */
    float drift_hysteresis_c;  /* guard 4's settle band; 0 -> DRIFT_HYSTERESIS_C */
    float frozen_eps_c;        /* guard 7's movement epsilon; 0 -> FROZEN_EPS_C */
    /* Guard 1's ARRIVAL BAND -- added 2026-08-29 after guard 1 aborted a real
     * multi-segment firing in the middle of a healthy dwell. 0 -> PROGRESS_BAND_C.
     *
     * Guard 1 asks "heat is commanded and the zone is below setpoint, so is it
     * rising?". That question is only meaningful while the zone is still
     * CLIMBING TOWARD setpoint. Once a PID loop has arrived, it sits a little
     * below setpoint by construction (the steady-state offset a finite gain
     * leaves) and holds there at whatever duty the losses demand -- which on a
     * well-insulated kiln is easily above progress_duty_min. So a settled,
     * perfectly healthy dwell presents guard 1 with exactly its trip
     * condition: duty high, error positive, temperature not rising. Not rising
     * is the CORRECT behaviour there; it is what "settled" means.
     *
     * Within this many degrees of setpoint the guard therefore stops
     * demanding a rise and demands only that the zone does not FALL (guard
     * 2's falling-rate test, which is the shape a dead element actually takes
     * once the plant is already hot). Outside the band -- a ramp, a cold
     * start, a genuinely lagging zone -- guard 1 is unchanged. */
    float progress_band_c;
    /* Guard 1's climbing-branch window FLOOR, in seconds -- 2026-09-10,
     * docs/audits/esp_panic_after_zone0_guard_trip_2026-09-10.md. Unlike
     * every field above, 0 does NOT mean "substitute a firmware default":
     * it means "no derived floor available" (no trusted plant model this
     * tick), and thermal_guard_tick() leaves window_s completely alone in
     * that case -- byte-identical to before this field existed. When
     * nonzero (the caller has a valid model -- see
     * thermal_guard_derive_climb_window_floor_s() below, meant to be called
     * once per tick from the zone's cached ff_tau_s/ff_dead_time_s), it
     * raises guard 1's climbing window up to this floor if the configured/
     * fallback window would otherwise be shorter -- it can only lengthen
     * window_s, never shorten it, and it never touches guard 2's
     * falling-rate window. This is a per-tick LOCAL value the caller
     * computes fresh (same pattern profile_executor.c's guard_cfg_this_tick
     * already uses for sanity_rate_c_per_min), not a persisted zone_cfg_t
     * field -- the plant model can change (a fresh autotune) without an
     * operator ever touching Settings > Zones. */
    float climb_window_floor_s;
} thermal_guard_cfg_t;

/* Derives guard 1's climbing-branch window floor from a zone's identified
 * FOPDT plant model (tau_s, dead_time_s). Pure, host-testable, no I/O --
 * same discipline as s8_rate_guard_estimate.c's estimator, which this
 * mirrors: fails safe (returns 0.0f, meaning "no derived floor") on any
 * missing/non-finite/non-positive input rather than guessing, so a bad or
 * absent model can only fall back to thermal_guard.c's pre-existing
 * defaults/operator config -- it can never widen OR shrink the guard
 * silently. model_valid should be the same validity gate the caller already
 * applies to the model (e.g. profile_executor.c's zone_runtime_t.ff_enabled,
 * which zone_load_model() only sets once k_dc/tau_s/dead_time_s are all
 * finite and positive) -- this function does not re-derive that gate, only
 * defends against the specific sentinel/non-finite/<=0 cases documented at
 * its call site, since a caller's own validity flag has been wrong before
 * (s8_rate_guard_estimate.c's 2026-09-10 finding). Result is clamped to
 * [120s, 900s] -- see thermal_guard.c's CLIMB_WINDOW_FLOOR_MIN_S/MAX_S. */
float thermal_guard_derive_climb_window_floor_s(float tau_s, float dead_time_s, bool model_valid);

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
    /* setpoint_c means two different things depending on the caller, and
     * that ambiguity has already caused one real regression (guard 4's idle-
     * arming backstop, 2026-09-03): profile_executor.c fills setpoint_c with
     * a genuine target temperature the PID loop is driving toward. autotune_
     * engine.c's STEP method has no target at all -- open-loop, it fills
     * setpoint_c with a placeholder (historically the zone's max_temp_c
     * ceiling, or raw_c+headroom with no ceiling configured) purely to keep
     * guard 1's `error = setpoint_c - measurement_c` positive so guard 1's
     * rise-check branch (not guard 2's falling-rate branch) is what
     * evaluates a stalled STEP run. That placeholder is not a setpoint by
     * any definition a *drift-from-setpoint* check can use.
     *
     * no_setpoint makes that distinction explicit instead of leaving a
     * consumer to infer it from setpoint_c's magnitude. Default false (a
     * plain struct literal that doesn't name it, or memset(0), reads as
     * "setpoint_c is real") so every existing producer -- profile_executor.c
     * included, which this field's addition must not require touching --
     * keeps its current behavior with no source change. Only a producer
     * whose setpoint_c is a placeholder, not a target, sets this true.
     *
     * Any NEW guard that reads setpoint_c to mean "the target this zone is
     * trying to reach" (guard 4's drift check is the first; there will be
     * more) MUST check no_setpoint first and skip its setpoint-dependent
     * logic when true, exactly as guard 4 now does -- rather than compute
     * something from a placeholder value and pass every existing test
     * because profile_executor.c's tests never exercise the other producer.
     * Guards 1/2, which reason about the SIGN and magnitude of error/rise
     * rather than "are we near a real target", are deliberately unaffected
     * by this field -- autotune_engine.c's own progress_rise_check_relaxed
     * mechanism (below) is what covers STEP's guard 1/2 behavior instead. */
    bool  no_setpoint;
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
    /* peer_is_on_off[i] true means that channel belongs to a ZONE_TYPE_
     * ON_OFF zone (docs/ON_OFF_ZONE_PLAN.md sec 1, guard 9/cross-zone row):
     * an on/off device's channel is not comparable to a heater's, so it must
     * be excluded from the OTHER side of every cross-zone comparison too, not
     * just skipped when it is the zone being ticked (see on_off_zone below).
     * NULL means "no on/off zones in this snapshot" -- every peer is treated
     * as comparable, bit-identical to before this field existed. */
    const bool  *peer_is_on_off;
    /* True when THIS zone (the one thermal_guard_tick() is being called for)
     * is ZONE_TYPE_ON_OFF (docs/ON_OFF_ZONE_PLAN.md sec 1's guard table).
     * Guards 1 (heating-failed), 2 (wrong-direction), 3 (runaway) and 4
     * (drift) all read a signature that a correctly-operating on/off device
     * (a vent commanding duty 1.0 for hours with a flat or FALLING reading)
     * produces as its completely normal, healthy behaviour -- guard 1's trip
     * condition is identical to a working vent's signature, so there is no
     * way to relax any of the four rather than not run them at all. Guards 5
     * (max/min temp), 6 (sensor validity) and 7 (frozen sensor) are NOT
     * gated by this flag -- they protect the kiln/sensor regardless of what
     * the relay drives, and stay live whenever the zone has a thermocouple.
     * Defaults false (a plain struct literal or memset(0)) so every existing
     * caller/zone is bit-identical to before this field existed. */
    bool on_off_zone;

    /* Relaxes ONLY guard 1's "still rising" requirement for this tick.
     * Exists for autotune_engine.c's step test: a step response that has
     * genuinely reached (or is approaching) its asymptote legitimately
     * stops rising, and guard 1 previously could not tell that apart from a
     * dead element -- it fired on an honest, successful identification
     * exactly as the run was about to succeed (confirmed on the bench, zone
     * 0, 100% duty: aborted at 840s, 69.61C, "rose only 0.5C in 1.0min",
     * approaching a ~72C asymptote).
     *
     * IMPORTANT, found in review of the first version of this mechanism:
     * during a step test guard 2 (falling while heating, "every other guard
     * is unaffected" as this comment used to claim) is ALSO effectively
     * silenced by this flag in practice, even though its own code path never
     * reads it -- autotune_engine.c pins this run's setpoint_c to the
     * zone's ceiling for the whole test, so `climbing` (the arrival-band
     * check just above, in thermal_guard.c) is true on nearly every tick and
     * guard 2's falling branch is structurally almost never reached whether
     * or not this flag is set. autotune_engine.c therefore does NOT rely on
     * guard 2 to catch an element dying after this flag goes true -- it
     * tracks its own running-peak-vs-current-rise check
     * (step_rise_running_max_c / AUTOTUNE_ELEMENT_DEATH_DROP_C) and aborts
     * through escalate_and_abort() directly, the same trip reason and
     * severity guard 2 itself would have used, once a real drop is seen.
     * See that file's comments for the reasoning -- this field's own
     * scope is still "relax guard 1's rise check, nothing else in this
     * file's code", the caller-side mitigation for guard 2's practical
     * unreachability lives in autotune_engine.c, not here.
     *
     * Callers earn this by proving the element genuinely heats FIRST --
     * autotune_engine.c only ever sets it true once the zone's cumulative
     * rise from baseline has crossed a small ABSOLUTE alive threshold
     * (AUTOTUNE_ELEMENT_ALIVE_RISE_C, autotune_engine.c) with a genuine
     * onset already detected -- deliberately NOT finalize_fit()'s much
     * larger fit-trust threshold, which the first version of this mechanism
     * reused and which never actually engaged before guard 1 tripped on the
     * measured plant (see that constant's own comment for the arithmetic).
     * Before that threshold, and for a dead element that never reaches it,
     * guard 1 behaves exactly as it always has -- this field defaults false
     * (a plain struct literal that doesn't name it, or a memset(0), reads
     * as "not relaxed"), and profile_executor.c's own thermal_guard_input_t
     * construction never sets it, so an ordinary firing sees no behavior
     * change at all. */
    bool progress_rise_check_relaxed;

    /* Selects guards 1/2's RELAY-CYCLING discriminator for this tick, added
     * after review found the directional (climbing/falling) test from the
     * step-test path structurally unsafe for a bang-bang relay run --
     * autotune_engine.c's own comment on why the PWM-duty fix armed guards
     * 1/2 for every tick of a relay run (previously they were inert on this
     * path) explains how the exposure got here.
     *
     * A relay run does not behave like a step or a settled dwell: it is
     * deliberately, continuously crossing back and forth through the
     * setpoint, so `error > band` ("still climbing toward setpoint") and its
     * negation are both true many times a minute and say nothing about
     * health. Guard 1's "must be rising" and guard 2's "must not be falling"
     * tests each cover only ONE half of a cycle that a healthy run is
     * SUPPOSED to spend the other half doing -- evaluated against a single
     * window landing on a downswing (guard 2) or the cooling half of a
     * wide-hysteresis cycle (guard 1), either fires on a perfectly healthy
     * oscillation.
     *
     * The property that actually distinguishes a live element from a dead
     * one during relay cycling is not direction, it's AMPLITUDE: a live
     * element's reading must cross both switching-band edges (setpoint -/+
     * the run's hysteresis h) to have produced the cycling in the first
     * place -- relay_law_tick() only flips branches when the measurement
     * reaches setpoint-h or setpoint+h, so any element that is actually
     * driving the plant guarantees a swing of at least 2*h peak-to-trough,
     * before dead-time overshoot even adds to it. A dead or disconnected
     * element produces no such swing regardless of which way the branch
     * happens to be pointing -- the reading just sits flat (thermocouple
     * noise only, typically well under 1C) while the relay law dutifully
     * keeps switching on a schedule the plant never follows.
     *
     * So: when this field is > 0.0f, guards 1/2 stop asking "is it moving
     * the right way" and instead track the min/max reading across the same
     * rolling window, tripping THERMAL_GUARD_TRIP_RELAY_STALLED only if the
     * window's whole span (max - min) stays below this floor -- direction
     * never enters into it, so a downswing, a long cooling half-cycle, or a
     * wide operator-chosen hysteresis (h up to AUTOTUNE_RELAY_MAX_H_C) never
     * trips it as long as the element is genuinely producing the swing its
     * own switching band requires. autotune_engine.c sets this to
     * 2*relay_h (the physical floor above) only for AUTOTUNE_METHOD_RELAY;
     * every other caller leaves it at its zero default and guards 1/2 keep
     * their ordinary directional behaviour unchanged -- this field and
     * progress_rise_check_relaxed are mutually exclusive in practice (STEP
     * sets the latter, RELAY sets this one) but nothing here enforces that;
     * a nonzero value here simply takes priority for guards 1/2 on this
     * tick, and progress_rise_check_relaxed is ignored while it does. */
    float relay_min_swing_c;
} thermal_guard_input_t;

typedef struct {
    bool     is_tripped;
    thermal_guard_trip_t reason;
    char     detail[96];

    /* Guards 1/2: rolling window over commanded-heat periods. */
    bool  progress_window_active;
    float progress_window_start_c;
    float progress_window_elapsed_s;
    /* Relay-cycling discriminator's own running extremes across the SAME
     * window as the fields above -- see thermal_guard_input_t.relay_min_swing_c.
     * Tracked unconditionally whenever the window is active (trivially cheap,
     * two comparisons a tick) so a mid-run switch of relay_min_swing_c from 0
     * to nonzero -- not that any caller does this today -- would already see
     * a populated window instead of one sample wide. */
    float progress_window_min_c;
    float progress_window_max_c;

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
    /* Guard 4's arming backstop -- cumulative time spent NOT actively
     * demanding heat (commanded_duty below progress_duty_min, the same
     * threshold guard 1 uses), accumulated regardless of at_setpoint_
     * window_active. A zone that starts a run already hot (or otherwise
     * never settles within DRIFT_HYSTERESIS_C even once) used to leave
     * at_setpoint_window_active permanently false, which meant guard 4's
     * sustained-excursion clock could never even start -- see thermal_
     * guard.c's guard-4 comment for the full reasoning, and for why this is
     * gated on duty (not plain wall-clock time): a zone actively climbing
     * toward a distant setpoint at high duty can legitimately take far
     * longer than DRIFT_PERIOD_S to first close a 25C gap on a
     * heavy/well-insulated kiln, and gating on duty keeps that case from
     * ever arming this backstop -- guard 1 already owns "commanding heat
     * but not progressing". This field only accumulates while the zone is
     * NOT trying, which is exactly the shape of the hot-start gap it
     * exists to close. */
    float idle_elapsed_s;

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
