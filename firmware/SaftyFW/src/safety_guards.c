#include "safety_guards.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Fallback defaults, substituted whenever a safety_guard_cfg_t field is 0
 * ("not configured") -- see safety_guards.h's doc comments. Values are
 * SAFETY_MODEL.md section 4's defaults for S1/S5/S11/S12, carried over
 * verbatim. */
#define FIRING_MARGIN_C_DEFAULT       100.0f  /* S1 */
#define BAD_READ_COUNT_DEFAULT        10u     /* S5 */
#define BAD_READ_TIME_S_DEFAULT       5.0f    /* S5 */
#define BLIND_GRACE_S_DEFAULT         60.0f   /* S5 */
#define FROZEN_WINDOW_S_DEFAULT       600.0f  /* S11 */
#define CJ_WARN_C_DEFAULT             60.0f   /* S12 */
#define CJ_MAX_C_DEFAULT              85.0f   /* S12 */
#define CJ_TIME_S_DEFAULT             60.0f   /* S12 */

#define S1_OVER_CEILING_STREAK_TO_TRIP 3u /* ~300ms at safety_core's 100ms tick */

void safety_guards_reset(safety_guard_state_t *state)
{
    memset(state, 0, sizeof(*state));
}

void safety_guards_clear(safety_guard_state_t *state)
{
    safety_guards_reset(state);
}

static void trip(safety_guard_state_t *state, safety_trip_t reason, const char *fmt, ...)
{
    state->is_tripped = true;
    state->reason = reason;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(state->detail, sizeof(state->detail), fmt, ap);
    va_end(ap);
}

/* Generic "0 means not configured" substitution, same convention
 * thermal_guard.c's effective_f() uses. */
static float effective_f(float cfg_val, float fallback)
{
    return (cfg_val > 0.0f) ? cfg_val : fallback;
}

static uint16_t effective_u16(uint16_t cfg_val, uint16_t fallback)
{
    return (cfg_val != 0u) ? cfg_val : fallback;
}

bool safety_guards_tick(safety_guard_state_t *state, const safety_guard_cfg_t *cfg,
                         const safety_guard_input_t *in)
{
    if (state->is_tripped) {
        return false; /* already latched -- caller should have de-energized K4 already */
    }

    /* --- S7: E-stop ---------------------------------------------------------
     * GPIO9 HIGH (contact open) is already debounced 50ms by discrete_task
     * before it reaches this struct (safety_guard_input_t's doc comment).
     * No further conditions, no debounce here -- the fastest guard in the
     * set (SAFETY_MODEL.md section 4). Checked first: it is the one signal
     * that should never be delayed by anything else this tick does. */
    if (in->estop_pressed) {
        trip(state, SAFETY_TRIP_ESTOP, "E-stop asserted (GPIO9 high, debounced)");
        return true;
    }

    /* --- S5: safety thermocouple invalid, graduated -------------------------
     * Bad read = SPI transfer failure, NaN, or MAX31856 fault bits
     * OPEN/OVUV/TCRANGE -- deliberately NOT TCHIGH/TCLOW (S1's job) and NOT
     * CJHIGH/CJLOW/CJRANGE alone (a cold-junction complaint, S12's job).
     * !tc_valid is included explicitly rather than relied upon implicitly:
     * ARCHITECTURE.md section 6 says tc_c is NaN when !valid, and isnan()
     * below would eventually catch that too, but checking tc_valid directly
     * means a producer bug that sets valid=false without actually setting
     * tc_c to NaN still gets caught as a bad read rather than silently
     * falling through to S1/S11/S12 below with an un-flagged garbage
     * value. */
    bool bad_read = in->spi_failed || !in->tc_valid || isnan(in->tc_c) ||
                     ((in->fault_bits & (SAFETY_THERMO_FAULT_OPEN | SAFETY_THERMO_FAULT_OVUV |
                                          SAFETY_THERMO_FAULT_TCRANGE)) != 0u);

    if (bad_read) {
        if (state->s5_bad_streak < UINT16_MAX) {
            state->s5_bad_streak++;
        }
        state->s5_bad_elapsed_s += in->dt_s;

        uint16_t count_th = effective_u16(cfg->bad_read_count_threshold, BAD_READ_COUNT_DEFAULT);
        float time_th = effective_f(cfg->bad_read_time_s, BAD_READ_TIME_S_DEFAULT);

        /* Both bars must clear -- SAFETY_MODEL.md section 4, S5: "a burst
         * of 10 reads in 50ms at some hypothetical fast poll rate
         * shouldn't warn; needs both". */
        if (state->s5_bad_streak >= count_th && state->s5_bad_elapsed_s >= time_th) {
            state->s5_warn = true;

            float grace = effective_f(cfg->blind_grace_s, BLIND_GRACE_S_DEFAULT);
            if (state->s5_bad_elapsed_s >= grace) {
                trip(state, SAFETY_TRIP_SENSOR_INVALID,
                     "blind for %.1fs (>= blind_grace_s %.1fs), %u consecutive bad reads",
                     (double)state->s5_bad_elapsed_s, (double)grace, (unsigned)state->s5_bad_streak);
                return true;
            }
        }

        /* A bad read can't feed S1/S11/S12 below a real number -- skip the
         * rest of this tick's checks rather than reasoning about a value
         * that isn't trustworthy (same discipline thermal_guard.c's guard 6
         * uses for guards 1/2/3/4/7 below it). */
        return false;
    }

    state->s5_bad_streak = 0;
    state->s5_bad_elapsed_s = 0.0f;
    state->s5_warn = false;

    /* --- S1: absolute over-temperature ---------------------------------------
     * abs_max_temp_c == 0 means "not commissioned" -- never trip, and never
     * accumulate a streak toward one (SAFETY_MODEL.md section 4, S1: "has
     * no default and must be commissioned"). */
    if (cfg->abs_max_temp_c > 0.0f) {
        float ceiling;
        if (cfg->tc_placement_mode == SAFETY_TC_CHAMBER_AGREED && cfg->firing_max_valid) {
            /* The ceiling can only ever tighten: min() means a hostile or
             * buggy ESP asking for more headroom gets clamped, never
             * obeyed (SAFETY_MODEL.md section 4, S1). */
            float requested = cfg->firing_max_c + effective_f(cfg->firing_margin_c, FIRING_MARGIN_C_DEFAULT);
            ceiling = (requested < cfg->abs_max_temp_c) ? requested : cfg->abs_max_temp_c;
        } else {
            /* EXTERNAL_OVERHEAT: fixed, always, firing_max_c ignored
             * entirely regardless of firing_max_valid (SAFETY_MODEL.md
             * section 4, S1: "applied to an externally-mounted sensor it
             * would be nonsense"). CHAMBER_AGREED with no firing running
             * (firing_max_valid == false) lands here too, which is exactly
             * "ceiling = abs_max_temp_c" per the doc. */
            ceiling = cfg->abs_max_temp_c;
        }

        if (in->tc_valid && in->tc_c > ceiling) {
            if (state->s1_over_ceiling_streak < UINT8_MAX) {
                state->s1_over_ceiling_streak++;
            }
            if (state->s1_over_ceiling_streak >= S1_OVER_CEILING_STREAK_TO_TRIP) {
                trip(state, SAFETY_TRIP_OVERTEMP, "%.1fC > ceiling %.1fC for %u consecutive valid readings",
                     (double)in->tc_c, (double)ceiling, (unsigned)state->s1_over_ceiling_streak);
                return true;
            }
        } else {
            state->s1_over_ceiling_streak = 0;
        }
    } else {
        state->s1_over_ceiling_streak = 0;
    }

    /* --- S11: frozen safety reading -------------------------------------------
     * Active reading identical (to full resolution) for frozen_window_s AND
     * heat_commanded true throughout. See safety_guard_input_t's comment on
     * heat_commanded for why gating on a plain caller-supplied bool -- which
     * defaults to false when nothing better is known -- is the honest,
     * conservative answer in a context-free build: it keeps this guard
     * dormant on an idle kiln without ignoring its own qualifier. */
    if (in->tc_valid && in->heat_commanded) {
        if (!state->s11_window_active || in->tc_c != state->s11_last_c) {
            state->s11_window_active = true;
            state->s11_last_c = in->tc_c;
            state->s11_elapsed_s = 0.0f;
        } else {
            state->s11_elapsed_s += in->dt_s;
            if (state->s11_elapsed_s >= effective_f(cfg->frozen_window_s, FROZEN_WINDOW_S_DEFAULT)) {
                trip(state, SAFETY_TRIP_FROZEN_SENSOR,
                     "reading unchanged at %.1fC for %.0fs while heat commanded",
                     (double)in->tc_c, (double)state->s11_elapsed_s);
                return true;
            }
        }
    } else {
        state->s11_window_active = false;
    }

    /* --- S12: cold junction / enclosure over-temperature -----------------------
     * Two independent thresholds: cj_warn_c is a plain level crossing
     * (WARN only, no timer -- it is reported every tick it is exceeded, not
     * latched); cj_max_c needs cj_time_s of sustained excess to TRIP. */
    if (in->tc_valid) {
        float cj_max = effective_f(cfg->cj_max_c, CJ_MAX_C_DEFAULT);
        float cj_warn = effective_f(cfg->cj_warn_c, CJ_WARN_C_DEFAULT);

        if (in->cj_c > cj_max) {
            state->s12_warn = true;
            state->s12_over_max_elapsed_s += in->dt_s;
            if (state->s12_over_max_elapsed_s >= effective_f(cfg->cj_time_s, CJ_TIME_S_DEFAULT)) {
                trip(state, SAFETY_TRIP_ENCLOSURE_TEMP, "cold junction %.1fC > cj_max_c %.1fC for %.0fs",
                     (double)in->cj_c, (double)cj_max, (double)state->s12_over_max_elapsed_s);
                return true;
            }
        } else if (in->cj_c > cj_warn) {
            state->s12_warn = true;
            state->s12_over_max_elapsed_s = 0.0f;
        } else {
            state->s12_warn = false;
            state->s12_over_max_elapsed_s = 0.0f;
        }
    }

    return false;
}
