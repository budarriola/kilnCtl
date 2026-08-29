#include "thermal_guard.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Fallback defaults, substituted by effective_f()/effective_ticks() below
 * whenever a zone's thermal_guard_cfg_t field is 0 ("not configured") --
 * see thermal_guard.h's doc comment. Values and rationale are TODO.md 6A.3's
 * own first-pass numbers, carried over verbatim from when these were the
 * only values that existed. */
#define PROGRESS_DUTY_MIN 0.5f
#define PROGRESS_WINDOW_S 300.0f
#define WRONG_DIR_RATE_C_PER_MIN 1.0f
/* Guard 1's arrival band -- see thermal_guard_cfg_t.progress_band_c for the
 * full reasoning. 3 C is chosen against the two things that have to fit
 * inside it: a time-proportioning window's own ripple (under 2 C on this
 * bench at a 60 s window and full duty) and a PID's steady-state offset. It
 * is deliberately small, because everything inside the band is a degree of
 * dead-element detection traded for a firing that does not abort itself: a
 * ramp presents errors of tens of degrees, so a dead element on the way up
 * is still caught exactly as before. */
#define PROGRESS_BAND_C 3.0f
#define WRONG_DIR_WINDOW_S 120.0f
#define OFF_SETTLE_S 120.0f
#define RUNAWAY_RATE_C_PER_MIN 1.0f
#define RUNAWAY_MARGIN_C 20.0f
#define DRIFT_HYSTERESIS_C 25.0f
#define DRIFT_PERIOD_S 600.0f
#define SENSOR_FAULT_DEBOUNCE_TICKS 3u
#define FROZEN_WINDOW_S 600.0f
/* Guard 7's "unchanged" tolerance -- see the guard-7 comment at its call
 * site for why bit-exact comparison cannot work on real hardware. Chosen
 * against the ANCHOR (the reading at window-start, not the previous tick):
 * +/-0.02C alternating dither around a steady anchor can differ from that
 * anchor by up to 0.04C on either side, so 0.03C alone is not enough
 * margin -- 0.05C clears that with room, while staying far below any real
 * thermal movement accumulated over a whole window (even a slow 0.5C/min
 * drift is >0.05C within about 6s). */
#define FROZEN_EPS_C 0.05f
/* Guard 8's window. Only the *period* has a default -- the delta threshold
 * deliberately does not; see thermal_guard_cfg_t.cross_zone_max_delta_c. */
#define CROSS_ZONE_PERIOD_S_DEFAULT 600.0f

void thermal_guard_reset(thermal_guard_state_t *state)
{
    memset(state, 0, sizeof(*state));
}

void thermal_guard_clear(thermal_guard_state_t *state)
{
    thermal_guard_reset(state);
}

static void trip(thermal_guard_state_t *state, thermal_guard_trip_t reason, const char *fmt, ...)
{
    state->is_tripped = true;
    state->reason = reason;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(state->detail, sizeof(state->detail), fmt, ap);
    va_end(ap);
}

/* Generic "0 means not configured" substitution -- used for every
 * thermal_guard_cfg_t threshold field, not just rates, hence the name. */
static float effective_f(float cfg_val, float fallback)
{
    return (cfg_val > 0.0f) ? cfg_val : fallback;
}

static uint8_t effective_ticks(float cfg_val, uint8_t fallback)
{
    return (cfg_val > 0.0f) ? (uint8_t)cfg_val : fallback;
}

bool thermal_guard_tick(thermal_guard_state_t *state, const thermal_guard_cfg_t *cfg,
                        const thermal_guard_input_t *in)
{
    if (state->is_tripped) {
        return false; /* already latched -- caller should have stopped driving anyway */
    }

    /* --- Guard 6: sensor validity, debounced ------------------------------
     * TODO.md 10.8: in->sensor_ok is the caller's combined verdict across
     * every thermocouple channel assigned to this zone (thermo_combine.c),
     * not necessarily one channel -- see thermal_guard_input_t's doc comment.
     * No change needed here: this guard was always just consuming whatever
     * bool the caller decided "the reading" was trustworthy. */
    if (!in->sensor_ok) {
        state->sensor_fault_streak++;
        if (state->sensor_fault_streak >= effective_ticks(cfg->sensor_fault_debounce_ticks, SENSOR_FAULT_DEBOUNCE_TICKS)) {
            trip(state, THERMAL_GUARD_TRIP_SENSOR_INVALID,
                 "sensor invalid for %u consecutive reads", (unsigned)state->sensor_fault_streak);
            return true;
        }
        /* A bad read also can't feed any window below with a real number --
         * skip the rest of this tick's checks rather than reasoning about a
         * value that isn't trustworthy. */
        return false;
    }
    state->sensor_fault_streak = 0;

    /* --- Guard 5: absolute limits, immediate, no debounce ------------------ */
    if (cfg->max_temp_c > 0.0f && in->measurement_c >= cfg->max_temp_c) {
        trip(state, THERMAL_GUARD_TRIP_MAX_TEMP, "%.1fC >= max_temp_c %.1fC", (double)in->measurement_c,
             (double)cfg->max_temp_c);
        return true;
    }
    if (in->measurement_c <= cfg->min_temp_c) {
        trip(state, THERMAL_GUARD_TRIP_MIN_TEMP, "%.1fC <= min_temp_c %.1fC", (double)in->measurement_c,
             (double)cfg->min_temp_c);
        return true;
    }

    /* --- Guard 7: frozen sensor -------------------------------------------- */
    /* A "stuck" reading must be compared with a tolerance band, not bit-exact
     * equality: a live MAX31856 in K-type has ~0.0078C ADC resolution but
     * dithers by 0.01-0.1C read-to-read from thermal/electrical noise even
     * while the junction is genuinely steady. Bit-exact comparison meant this
     * guard's window reset almost every tick on real hardware and the 120s
     * window never completed -- confirmed on the bench: readings moved every
     * single second for 6 minutes straight. See FROZEN_EPS_C's own comment
     * above for the exact value and why. */
    if (in->commanded_duty > 0.0f) {
        if (!state->frozen_window_active || fabsf(in->measurement_c - state->frozen_last_c) > effective_f(cfg->frozen_eps_c, FROZEN_EPS_C)) {
            state->frozen_window_active = true;
            state->frozen_last_c = in->measurement_c;
            state->frozen_elapsed_s = 0.0f;
        } else {
            state->frozen_elapsed_s += in->dt_s;
            if (state->frozen_elapsed_s >= effective_f(cfg->frozen_window_s, FROZEN_WINDOW_S)) {
                trip(state, THERMAL_GUARD_TRIP_FROZEN, "reading unchanged at %.1fC for %.0fs while duty > 0",
                     (double)in->measurement_c, (double)state->frozen_elapsed_s);
                return true;
            }
        }
    } else {
        state->frozen_window_active = false;
    }

    float rate_cfg = effective_f(cfg->sanity_rate_c_per_min, 0.5f);

    /* --- Guards 1 & 2: heating-failed / wrong-direction --------------------
     * Both share one rolling window over "duty is at/above the progress
     * threshold" periods; which guard applies depends on which way the
     * error points. */
    if (in->commanded_duty >= effective_f(cfg->progress_duty_min, PROGRESS_DUTY_MIN)) {
        float error = in->setpoint_c - in->measurement_c;
        if (!state->progress_window_active) {
            state->progress_window_active = true;
            state->progress_window_start_c = in->measurement_c;
            state->progress_window_elapsed_s = 0.0f;
        } else {
            state->progress_window_elapsed_s += in->dt_s;
            /* wrong_dir_window_s is exposed to operators (Settings > Zones) as
             * "how long heat may be commanded without the temperature
             * responding" -- that description covers BOTH branches below, not
             * just the falling-while-heating case its name suggests. Applying
             * it only to the error<=0 branch left guard 1 (the case that
             * actually matters when a heating element dies) stuck on the
             * hardcoded PROGRESS_WINDOW_S with no per-zone override at all --
             * confirmed on the bench: a dead element was only caught after a
             * fixed 5 minutes regardless of the operator's configured 60s.
             * Use the same effective_f() substitution for both branches so
             * the field means what its UI label says. Each branch keeps its
             * OWN pre-existing fallback (PROGRESS_WINDOW_S=300s for guard 1,
             * WRONG_DIR_WINDOW_S=120s for guard 2) so an unconfigured zone
             * (wrong_dir_window_s == 0) behaves exactly as it did before this
             * fix -- only a zone that has actually set wrong_dir_window_s
             * sees the new behaviour of it applying to guard 1 too. */
            /* THE ARRIVAL BAND, added 2026-08-29. `error > 0` is not the
             * same question as "is this zone still climbing toward
             * setpoint", and treating it as such is what aborted a healthy
             * three-segment firing on this bench mid-dwell: zone 0 was
             * holding 50.7 C against a 52.0 C setpoint at full duty --
             * settled, 1.3 C of steady-state offset, exactly as a PID
             * should -- and guard 1 read "duty high, below setpoint, not
             * rising" and tripped with "rose only -0.2C in 1min". Demanding
             * a rise from a loop that has already arrived is demanding that
             * it overshoot.
             *
             * So the rise test now applies only OUTSIDE the band. Inside it
             * the zone still has to answer for itself -- it must not FALL --
             * which is the shape a dead element takes once the plant is hot,
             * and is guard 2's existing test applied to a case that
             * previously had no test at all. See
             * thermal_guard_cfg_t.progress_band_c. */
            float band_c = effective_f(cfg->progress_band_c, PROGRESS_BAND_C);
            bool climbing = (error > band_c);
            float window_s = effective_f(cfg->wrong_dir_window_s,
                                          climbing
                                              ? effective_f(cfg->progress_window_s, PROGRESS_WINDOW_S)
                                              : WRONG_DIR_WINDOW_S);
            if (state->progress_window_elapsed_s >= window_s) {
                float delta = in->measurement_c - state->progress_window_start_c;
                float elapsed_min = state->progress_window_elapsed_s / 60.0f;
                if (climbing) {
                    /* Guard 1: heating, below setpoint, must be rising. */
                    float expected = rate_cfg * elapsed_min;
                    if (delta < expected) {
                        trip(state, THERMAL_GUARD_TRIP_HEATING_FAILED,
                             "heating but rose only %.1fC in %.1fmin (need >=%.1fC)", (double)delta,
                             (double)elapsed_min, (double)expected);
                        return true;
                    }
                } else {
                    /* Guard 2: heating while at, above, or within the arrival
                     * band of setpoint, and falling faster than the
                     * wrong-direction threshold -- a miswired zone driving
                     * full output and making things worse, or an element that
                     * has died during a dwell. */
                    float falling_c_per_min = -delta / elapsed_min;
                    if (falling_c_per_min > effective_f(cfg->wrong_dir_rate_c_per_min, WRONG_DIR_RATE_C_PER_MIN)) {
                        trip(state, THERMAL_GUARD_TRIP_WRONG_DIRECTION,
                             "heating commanded but temperature falling %.2fC/min", (double)falling_c_per_min);
                        return true;
                    }
                }
                /* Window satisfied (or the falling-but-under-threshold case
                 * for guard 2) -- slide to a fresh window rather than
                 * growing forever. */
                state->progress_window_start_c = in->measurement_c;
                state->progress_window_elapsed_s = 0.0f;
            }
        }
    } else {
        state->progress_window_active = false;
    }

    /* --- Guard 3: runaway with heat off (welded contact) ------------------- */
    if (in->commanded_duty <= 0.0f) {
        /* Computed once per tick so the settle threshold and the "sample too
         * short to rate" cutoff below always agree on the same window,
         * whatever this zone's override is -- the false positive this guard
         * fixed on hardware (see below) was exactly a mismatch of this kind. */
        float off_settle_s = effective_f(cfg->off_settle_s, OFF_SETTLE_S);
        if (!state->off_window_active) {
            state->off_window_active = true;
            state->off_window_elapsed_s = 0.0f;
            state->runaway_baseline_c = in->measurement_c;
        } else {
            state->off_window_elapsed_s += in->dt_s;
            if (state->off_window_elapsed_s >= off_settle_s) {
                /* Re-baseline exactly once, at the instant the settle window
                 * ends, so the rise and the time it is divided by cover the
                 * SAME interval.
                 *
                 * Found on hardware 2026-08-12 (simulated plant, TODO.md
                 * 6A.8): measuring delta from the start of the off-window but
                 * dividing by the time since settle ended reported a 4.3C
                 * rise as "257.74C/min" on the first tick past the window,
                 * because that tick's denominator is one dt. Any zone sitting
                 * at duty 0 through the settle window and drifting up by even
                 * a fraction of a degree -- a dwell, a neighbour's heat
                 * arriving through the chamber, ordinary coasting after a
                 * ramp -- would trip a welded-contact fault. That is a
                 * false positive on the guard whose whole job is to be
                 * believed when it fires. */
                if (!state->runaway_rate_baseline_valid) {
                    state->runaway_rate_baseline_valid = true;
                    state->runaway_rate_baseline_c = in->measurement_c;
                    state->runaway_rate_elapsed_s = 0.0f;
                } else {
                    state->runaway_rate_elapsed_s += in->dt_s;
                }
                /* The margin check keeps the ORIGINAL baseline: "20C above
                 * where it was when heat was commanded off" is the absolute
                 * statement, independent of when the rate window started. */
                float delta = in->measurement_c - state->runaway_baseline_c;
                float rate_delta = in->measurement_c - state->runaway_rate_baseline_c;
                float elapsed_min = state->runaway_rate_elapsed_s / 60.0f;
                /* Below ~a third of the settle window the sample is too short
                 * to divide by; the margin check still applies meanwhile. */
                float rate = (elapsed_min >= (off_settle_s / 3.0f) / 60.0f) ? rate_delta / elapsed_min : 0.0f;
                float runaway_rate_cfg = effective_f(cfg->runaway_rate_c_per_min, RUNAWAY_RATE_C_PER_MIN);
                float runaway_margin_cfg = effective_f(cfg->runaway_margin_c, RUNAWAY_MARGIN_C);
                if (rate > runaway_rate_cfg || delta > runaway_margin_cfg) {
                    trip(state, THERMAL_GUARD_TRIP_RUNAWAY,
                         "heat commanded off %.0fs but temperature rose %.1fC (rate %.2fC/min) -- possible welded relay",
                         (double)state->off_window_elapsed_s, (double)delta, (double)rate);
                    return true;
                }
            }
        }
    } else {
        state->off_window_active = false;
        state->runaway_rate_baseline_valid = false;
    }

    /* --- Guard 4: drift at setpoint -----------------------------------------
     * "Settled" once the zone has ever come within DRIFT_HYSTERESIS_C of
     * setpoint (at_setpoint_window_active, reused as that latch); from then
     * on a *sustained* excursion back outside that band for DRIFT_PERIOD_S
     * trips. A brief excursion (a dwell wandering, a ramp segment just
     * starting) resets the sustained-excursion timer but doesn't un-settle
     * the zone -- matches TODO.md 6A.3's "loose and slow, a kiln legitimately
     * wanders" intent, deliberately looser than a 3D printer's 4C/40s. Note
     * this deliberately does NOT use the bang-bang hysteresis band
     * (heater_output.h, typically ~2C) -- that band is tight enough that a
     * normal dwell cycling the relay would spend most of its time just
     * outside it, which would make this guard fire on completely healthy
     * operation. */
    float abs_error = fabsf(in->setpoint_c - in->measurement_c);
    float drift_band_c = effective_f(cfg->drift_hysteresis_c, DRIFT_HYSTERESIS_C);
    if (abs_error <= drift_band_c) {
        state->at_setpoint_window_active = true;
        state->at_setpoint_elapsed_s = 0.0f;
    } else if (state->at_setpoint_window_active) {
        state->at_setpoint_elapsed_s += in->dt_s;
        if (state->at_setpoint_elapsed_s >= effective_f(cfg->drift_period_s, DRIFT_PERIOD_S)) {
            trip(state, THERMAL_GUARD_TRIP_DRIFT, "drifted >%.0fC from setpoint for %.0fs after settling",
                 (double)drift_band_c, (double)state->at_setpoint_elapsed_s);
            return true;
        }
    }

    /* --- Guard 8: cross-zone plausibility ----------------------------------
     * Two zones in one chamber cannot disagree by more than
     * cross_zone_max_delta_c for longer than cross_zone_period_s. This is the
     * check that catches a thermocouple which fell out of the kiln body even
     * when guards 1 and 2 are satisfied, because that zone's elements really
     * are heating the chamber -- its neighbors climb, it doesn't.
     *
     * Disabled unless the caller supplies BOTH a threshold and peer readings;
     * see thermal_guard_cfg_t.cross_zone_max_delta_c for why the threshold
     * has no default. Compared against the *worst* disagreeing peer rather
     * than an average: with three zones, an average would let one badly wrong
     * channel hide behind a healthy one. */
    if (cfg->cross_zone_max_delta_c > 0.0f && in->peer_c && in->peer_count > 0) {
        float worst_delta = 0.0f;
        int worst_peer = -1;
        for (uint8_t i = 0; i < in->peer_count; i++) {
            if (i == in->peer_index_self) continue;
            if (in->peer_ok && !in->peer_ok[i]) continue; /* untrustworthy reading -- guard 6's problem, not this one */
            float delta = fabsf(in->measurement_c - in->peer_c[i]);
            if (delta > worst_delta) {
                worst_delta = delta;
                worst_peer = (int)i;
            }
        }

        if (worst_peer >= 0 && worst_delta > cfg->cross_zone_max_delta_c) {
            state->cross_zone_elapsed_s += in->dt_s;
            float period_s = (cfg->cross_zone_period_s > 0.0f) ? cfg->cross_zone_period_s
                                                               : CROSS_ZONE_PERIOD_S_DEFAULT;
            if (state->cross_zone_elapsed_s >= period_s) {
                trip(state, THERMAL_GUARD_TRIP_CROSS_ZONE,
                     "%.1fC differs from zone %d's %.1fC by %.1fC (>%.1fC) for %.0fs",
                     (double)in->measurement_c, worst_peer, (double)in->peer_c[worst_peer],
                     (double)worst_delta, (double)cfg->cross_zone_max_delta_c,
                     (double)state->cross_zone_elapsed_s);
                return true;
            }
        } else {
            state->cross_zone_elapsed_s = 0.0f;
        }
    }

    return false;
}
