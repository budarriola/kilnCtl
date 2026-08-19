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
#define BORROWED_STALE_S_DEFAULT      10.0f   /* S13 */
#define BORROWED_STALE_TRIP_S_DEFAULT 60.0f   /* S13 */
#define OVERSHOOT_MARGIN_C_DEFAULT    75.0f   /* S2 */
#define OVERSHOOT_TIME_S_DEFAULT      120.0f  /* S2 */
#define I_PRESENT_A_DEFAULT           2.0f    /* S3/S4 */
#define CORRELATION_WINDOW_S_DEFAULT  150.0f  /* S3/S4 -- informational; the window itself is applied by the caller */
#define STUCK_ON_TIME_S_DEFAULT       20.0f   /* S3 */
#define LINK_TIMEOUT_S_DEFAULT        10.0f   /* S6b */
#define LINK_DEAD_HARD_S_DEFAULT      120.0f  /* S6b */
#define TRIP_VERIFY_S_DEFAULT         10.0f   /* S9 */
#define TC_DISAGREEMENT_C_DEFAULT     200.0f  /* S10 */
#define TC_DISAGREEMENT_TIME_S_DEFAULT 300.0f /* S10 */

#define S1_OVER_CEILING_STREAK_TO_TRIP 3u /* ~300ms at safety_core's 100ms tick */

void safety_guards_reset(safety_guard_state_t *state)
{
    memset(state, 0, sizeof(*state));
}

void safety_guards_clear(safety_guard_state_t *state)
{
    safety_guards_reset(state);
}

bool safety_guards_try_clear(safety_guard_state_t *state, const safety_guard_cfg_t *cfg,
                              const safety_guard_input_t *in)
{
    safety_guards_clear(state);
    return !safety_guards_tick(state, cfg, in);
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

float safety_guards_deciding_threshold_c(safety_trip_t reason, const safety_guard_cfg_t *cfg)
{
    switch (reason) {
    case SAFETY_TRIP_OVERTEMP:
        /* S1. Reports the configured absolute ceiling (abs_max_temp_c), NOT
         * the possibly-tighter effective_ceiling = min(abs_max_temp_c,
         * firing_max_c + firing_margin_c) that actually decided a trip
         * during a firing with a ceiling in effect -- safety_guards_tick()
         * computes that min() internally and does not expose it. abs_max_temp_c
         * has no substituted default (safety_guards.h: "0 = not commissioned,
         * guard never trips"), so if this guard tripped, this value is real
         * and non-zero -- never a silently-substituted fallback. */
        return cfg->abs_max_temp_c;
    case SAFETY_TRIP_OVER_SETPOINT: /* S2 */
        return effective_f(cfg->overshoot_margin_c, OVERSHOOT_MARGIN_C_DEFAULT);
    case SAFETY_TRIP_LOAD_STUCK_ON: /* S3 */
        return effective_f(cfg->i_present_a, I_PRESENT_A_DEFAULT);
    case SAFETY_TRIP_FROZEN_SENSOR: /* S11 */
        return effective_f(cfg->frozen_window_s, FROZEN_WINDOW_S_DEFAULT);
    case SAFETY_TRIP_ENCLOSURE_TEMP: /* S12 */
        return effective_f(cfg->cj_max_c, CJ_MAX_C_DEFAULT);
    case SAFETY_TRIP_BORROWED_STALE: /* S13 */
        return effective_f(cfg->borrowed_stale_trip_s, BORROWED_STALE_TRIP_S_DEFAULT);
    default:
        /* S5 (dual count+time threshold, no single magnitude), S6a/S7
         * (boolean conditions, no magnitude at all), S6b (a hard-backstop
         * *time*, already covered qualitatively by trip_reason itself), S9
         * (escalation of an existing trip, not a fresh threshold crossing) --
         * none of these has one meaningful number to report here. NaN, never
         * a guessed value, matching this whole protocol's "NaN, never 0, when
         * a field does not apply" discipline. */
        return NAN;
    }
}

bool safety_guards_tick(safety_guard_state_t *state, const safety_guard_cfg_t *cfg,
                         const safety_guard_input_t *in)
{
    if (state->is_tripped) {
        /* --- S9: trip ineffective / contactor welded -------------------------
         * The one guard that must keep evaluating after a trip -- everything
         * else stops mattering once K4 should be open, but "did it actually
         * open" is exactly the question this asks (SAFETY_MODEL.md section 4,
         * S9). Escalates once: trip_ineffective latches independently of
         * is_tripped (already true) so the caller can tell "tripped" apart
         * from "tripped AND still conducting", and reason is overwritten to
         * SAFETY_TRIP_INEFFECTIVE -- ARCHITECTURE.md section 9's trip-code
         * comment calls this one "escalation, not a cause" for exactly this
         * reason: once it fires, it is the more urgent fact to report. */
        if (!state->trip_ineffective) {
            if (in->relay_deenergized) {
                state->s9_verify_active = true;
                state->s9_verify_elapsed_s += in->dt_s;
                float verify_th = effective_f(cfg->trip_verify_s, TRIP_VERIFY_S_DEFAULT);
                if (state->s9_verify_elapsed_s >= verify_th && in->any_current_present) {
                    state->trip_ineffective = true;
                    trip(state, SAFETY_TRIP_INEFFECTIVE,
                         "K4 de-energized for %.1fs (>= trip_verify_s %.1fs) but current still present",
                         (double)state->s9_verify_elapsed_s, (double)verify_th);
                    return true; /* newly escalated -- caller reacts once, same contract as any new trip */
                }
            } else {
                /* relay_owner has not (yet) reported K4 de-energized -- do
                 * not start the clock on a verification window that hasn't
                 * actually begun. */
                state->s9_verify_active = false;
                state->s9_verify_elapsed_s = 0.0f;
            }
        }
        return false; /* already latched -- caller should have de-energized K4 already */
    }

    /* --- S6a: main controller explicitly asserts fault -----------------------
     * mainFault (GPIO10, active low), already debounced 200ms by
     * discrete_task -- unambiguous, no further conditions (SAFETY_MODEL.md
     * section 4, S6a). Checked early, alongside S7, because both are
     * unconditional electrical signals rather than thermal evidence. */
    if (in->main_fault_asserted) {
        trip(state, SAFETY_TRIP_MAIN_FAULT, "mainFault asserted (GPIO10 low, debounced)");
        return true;
    }

    /* --- S6b: the link has gone quiet -----------------------------------------
     * Two-tier, hazard-keyed rather than a flat timeout (SAFETY_MODEL.md
     * section 4, S6b / section 2's "absence of information is not evidence
     * of danger -- except when heat is on"): a quiet link with current
     * flowing trips at link_timeout_s; a quiet link with no current only
     * warns and keeps watching; link_dead_hard_s is an unconditional
     * backstop regardless of current, because "no current right now" is a
     * weak statement given a 60s heater window the ESP could have died
     * inside of. link_up resets the elapsed timer every tick it is true,
     * matching S12/S2's own "reset on a healthy tick" pattern. */
    if (in->link_up) {
        state->s6b_link_down_elapsed_s = 0.0f;
    } else {
        state->s6b_link_down_elapsed_s += in->dt_s;
        float hard = effective_f(cfg->link_dead_hard_s, LINK_DEAD_HARD_S_DEFAULT);
        if (state->s6b_link_down_elapsed_s >= hard) {
            trip(state, SAFETY_TRIP_LINK_DEAD, "link silent for %.1fs (>= link_dead_hard_s %.1fs), unconditional",
                 (double)state->s6b_link_down_elapsed_s, (double)hard);
            return true;
        }
        float soft = effective_f(cfg->link_timeout_s, LINK_TIMEOUT_S_DEFAULT);
        if (state->s6b_link_down_elapsed_s >= soft && in->any_current_present) {
            trip(state, SAFETY_TRIP_LINK_DEAD, "link silent for %.1fs (>= link_timeout_s %.1fs) with current present",
                 (double)state->s6b_link_down_elapsed_s, (double)soft);
            return true;
        }
        /* Quiet link, no current: WARN territory (reported by the caller
         * from s6b_link_down_elapsed_s > 0), not a trip. Keep watching. */
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
            /* isfinite() guard: a non-finite requested (NaN OR -Infinity --
             * garbled/hostile firing_max_c from the link) must fall back to
             * abs_max_temp_c rather than enter the comparison below. NaN
             * already fell through by luck of IEEE754 "any compare with NaN
             * is false" semantics, but -Infinity does NOT -- "-Inf < finite"
             * is true, so without this guard ceiling would latch to
             * -Infinity and S1 would trip on every subsequent tick forever
             * (a permanent nuisance-trip, not a missed-trip risk). */
            ceiling = (isfinite(requested) && requested < cfg->abs_max_temp_c) ? requested : cfg->abs_max_temp_c;
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

    /* --- Context-dependent guards: S2, S3, S4, S10, S13 -----------------------
     * SAFETY_MODEL.md section 5, rule 2: "stale context is no context" -- and
     * ARCHITECTURE.md section 9: context-dependent guards report *inactive*
     * on stale/absent/mismatched context, never pessimistic (no silent pass,
     * no silent trip). in->context_valid is the caller's single collapse of
     * "never received / stale / DEGRADED_NO_CONTEXT" into one fact, so this
     * module does not need to re-derive any of those conditions itself. When
     * false, every guard below resets its own timer/level state rather than
     * merely skipping the check -- an old accumulating window from before
     * context went stale must not silently resume and trip on its
     * pre-staleness progress once context returns. */
    if (!in->context_valid) {
        state->s2_over_elapsed_s = 0.0f;
        state->s3_stuck_elapsed_s = 0.0f;
        state->s4_warn = false;
        state->s10_warn = false;
        state->s10_disagree_elapsed_s = 0.0f;
        state->s13_stale_elapsed_s = 0.0f;
        state->s13_warn = false;
    } else {
        /* --- S2: sustained excess over setpoint --------------------------------
         * CHAMBER_AGREED only -- comparing a shell/exhaust reading against a
         * chamber setpoint is meaningless (SAFETY_MODEL.md section 4, S2).
         * zone_count == 0 means "no active zones this tick", same inactive
         * treatment as stale context. */
        if (cfg->tc_placement_mode == SAFETY_TC_CHAMBER_AGREED && in->zone_count > 0u && in->tc_valid) {
            float margin = effective_f(cfg->overshoot_margin_c, OVERSHOOT_MARGIN_C_DEFAULT);
            if (in->tc_c > in->max_zone_setpoint_c + margin) {
                state->s2_over_elapsed_s += in->dt_s;
                float time_th = effective_f(cfg->overshoot_time_s, OVERSHOOT_TIME_S_DEFAULT);
                if (state->s2_over_elapsed_s >= time_th) {
                    trip(state, SAFETY_TRIP_OVER_SETPOINT,
                         "%.1fC > max setpoint %.1fC + overshoot_margin_c %.1fC for %.0fs",
                         (double)in->tc_c, (double)in->max_zone_setpoint_c, (double)margin,
                         (double)state->s2_over_elapsed_s);
                    return true;
                }
            } else {
                state->s2_over_elapsed_s = 0.0f;
            }
        } else {
            state->s2_over_elapsed_s = 0.0f;
        }

        /* --- S3: load active with no heat commanded ----------------------------
         * Presence/absence, not a current-magnitude test (SAFETY_MODEL.md
         * section 3/4, S3). relay_commanded_recently is already the ESP's
         * relay_recent_mask-derived "was anything commanded on in the last
         * correlation_window_s" fact -- this module trusts it as a
         * caller-computed input the same way it trusts context_valid. */
        if (in->any_current_present && !in->relay_commanded_recently) {
            state->s3_stuck_elapsed_s += in->dt_s;
            float stuck_th = effective_f(cfg->stuck_on_time_s, STUCK_ON_TIME_S_DEFAULT);
            if (state->s3_stuck_elapsed_s >= stuck_th) {
                trip(state, SAFETY_TRIP_LOAD_STUCK_ON,
                     "current present with nothing commanded on for %.0fs (>= stuck_on_time_s %.1fs)",
                     (double)state->s3_stuck_elapsed_s, (double)stuck_th);
                return true;
            }
        } else {
            state->s3_stuck_elapsed_s = 0.0f;
        }

        /* --- S4: heat commanded but load inactive -- WARN only, never TRIP -----
         * A design decision, not an oversight (SAFETY_MODEL.md section 4,
         * S4): a dead element ruins a firing, it does not start one. */
        state->s4_warn = in->relay_commanded_continuously && !in->any_current_present;

        /* --- S10: safety TC disagrees with every zone TC -- WARN only ----------
         * CHAMBER_AGREED only, same reasoning as S2 (SAFETY_MODEL.md section
         * 4, S10). Compares against the caller-supplied *nearest* valid zone
         * reading, never the mean -- kilns stratify. */
        if (cfg->tc_placement_mode == SAFETY_TC_CHAMBER_AGREED && in->zone_count > 0u && in->tc_valid) {
            float disagree_c = effective_f(cfg->tc_disagreement_c, TC_DISAGREEMENT_C_DEFAULT);
            float diff = in->tc_c - in->nearest_zone_measured_c;
            if (diff < 0.0f) {
                diff = -diff;
            }
            if (diff > disagree_c) {
                state->s10_disagree_elapsed_s += in->dt_s;
                float time_th = effective_f(cfg->tc_disagreement_time_s, TC_DISAGREEMENT_TIME_S_DEFAULT);
                state->s10_warn = (state->s10_disagree_elapsed_s >= time_th);
            } else {
                state->s10_disagree_elapsed_s = 0.0f;
                state->s10_warn = false;
            }
        } else {
            state->s10_disagree_elapsed_s = 0.0f;
            state->s10_warn = false;
        }

        /* --- S13: borrowed channel not updating -- graduated, like S5 ---------
         * BORROWED_ZONE/BOTH only. sample_counter_advancing is the caller's
         * already-computed "did the zone's sample_counter move since last
         * frame" fact (SAFETY_MODEL.md section 4, S13) -- this module only
         * owns the elapsed-time accumulation and the two thresholds. */
        if (cfg->tc_source == SAFETY_TC_SOURCE_BORROWED_ZONE || cfg->tc_source == SAFETY_TC_SOURCE_BOTH) {
            if (!in->sample_counter_advancing) {
                state->s13_stale_elapsed_s += in->dt_s;
                float warn_th = effective_f(cfg->borrowed_stale_s, BORROWED_STALE_S_DEFAULT);
                float trip_th = effective_f(cfg->borrowed_stale_trip_s, BORROWED_STALE_TRIP_S_DEFAULT);
                if (state->s13_stale_elapsed_s >= warn_th) {
                    state->s13_warn = true;
                    if (state->s13_stale_elapsed_s >= trip_th) {
                        trip(state, SAFETY_TRIP_BORROWED_STALE,
                             "borrowed sample_counter stale for %.1fs (>= borrowed_stale_trip_s %.1fs)",
                             (double)state->s13_stale_elapsed_s, (double)trip_th);
                        return true;
                    }
                }
            } else {
                state->s13_stale_elapsed_s = 0.0f;
                state->s13_warn = false;
            }
        } else {
            state->s13_stale_elapsed_s = 0.0f;
            state->s13_warn = false;
        }
    }

    /* Not tripped this tick -- S9's verification window has nothing to
     * verify yet (it only means anything once is_tripped is true, handled
     * at the top of this function). Keep it clean so a stray earlier value
     * cannot leak into a future trip's verification window. */
    state->s9_verify_active = false;
    state->s9_verify_elapsed_s = 0.0f;

    return false;
}
