#include "safety_guards.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Fallback defaults, substituted whenever a safety_guard_cfg_t field is 0
 * ("not configured") -- see safety_guards.h's doc comments. Values are
 * SAFETY_MODEL.md section 4's defaults for S1/S5/S11/S12, carried over
 * verbatim. */
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
#define OVERCURRENT_PCT_DEFAULT       150u    /* S14 */
#define OVERCURRENT_TIME_S_DEFAULT    30.0f   /* S14 */
#define UNDERCURRENT_FRACTION_DEFAULT 0.7f    /* S15 -- CT_COMMISSIONING_PLAN.md step 3, not config-exposed */
#define UNDERCURRENT_TIME_S_DEFAULT   30.0f   /* S15 */
#define RATE_WINDOW_S_DEFAULT         60.0f   /* S8 -- SAFETY_MODEL.md section 4 */

#define S1_OVER_CEILING_STREAK_TO_TRIP 3u /* ~300ms at safety_core's 100ms tick */

/* S8. Consecutive WINDOW EVALUATIONS (each rate_window_s long, not ticks)
 * whose average rate exceeded max_rate_c_per_min, required before tripping.
 * 2, not 1, and here is the reasoning:
 *
 * This guard measures an AVERAGE rate over a whole window using only the
 * window's two endpoint samples (baseline-sample-and-hold, see the S8 block
 * below for why a per-tick derivative is unusable at this module's ~100ms
 * tick). A two-point average is exact for a genuine sustained ramp, but it
 * has one real weakness: if the sample landing exactly on a window boundary
 * is itself a glitch -- a single-tick spurious reading that S5's fault-bit
 * checks did not catch (S5 only rejects SPI failures, NaN and MAX31856
 * OPEN/OVUV/TCRANGE, not an in-range-but-wrong sample) -- that one glitchy
 * endpoint can make a whole window's average look like a runaway even though
 * the kiln never moved.
 *
 * Requiring the SAME threshold crossing on two consecutive windows closes
 * that hole almost for free: a glitch that resolves by the very next tick is,
 * by construction, the boundary sample for at most one window on the "high"
 * side and becomes the STARTING sample for the next window, which then
 * measures back down from the inflated value -- so the same glitch cannot
 * make two consecutive windows both read high. A genuine sustained runaway,
 * by contrast, keeps climbing every window, so it clears both bars with
 * margin. This is the same "magnitude AND duration, one alone is never
 * enough" doctrine (SAFETY_MODEL.md section 2) applied across windows
 * instead of within one. */
#define S8_OVER_RATE_STREAK_TO_TRIP 2u

/* S9. Consecutive ticks of `any_current_present` required, once
 * trip_verify_s has already elapsed AND in->current_sensing_commissioned is
 * true, before the (deliberately unclearable) trip_ineffective latch is set.
 * Matches S1_OVER_CEILING_STREAK_TO_TRIP's idiom and magnitude.
 *
 * Scope, precisely (2026-08-27 audit, second pass): this streak defends
 * against a TRANSIENT spurious sample on an otherwise-trustworthy,
 * commissioned current-sensing chain -- e.g. one noisy ADC read. It is NOT,
 * by itself, a defence against an UNCOMMISSIONED channel's DC offset floor,
 * which current_presence_policy.c's `delta_counts > 25` fallback (active
 * whenever k_ct_v_per_a <= 0) reads as "current present" on every tick,
 * forever -- a systematic bias, not noise, that no streak length distinguishes
 * from a real weld. That case is handled separately, by gating progress of
 * this very streak on in->current_sensing_commissioned in the S9 block below
 * -- see that block's comment and safety_guards.h's doc comment on
 * current_sensing_commissioned for the full reasoning and why a first pass
 * at this fix (streak alone, no commissioning gate) did not actually close
 * the hole. 3 ticks (~300ms at the 100ms tick) is long enough that a single
 * transient sample cannot trigger it, short enough that it adds no
 * meaningful delay to detecting an actually-welded contactor on a
 * commissioned board, which will keep reading current every tick
 * indefinitely. */
#define S9_CURRENT_PRESENT_STREAK_TO_TRIP 3u

void safety_guards_reset(safety_guard_state_t *state)
{
    memset(state, 0, sizeof(*state));
}

void safety_guards_clear(safety_guard_state_t *state)
{
    safety_guards_reset(state);
}

#define S8_POST_MIN_S 5.0f
#define S8_POST_WIN_MIN_S 60.0f
#define S8_POST_WIN_MAX_S 600.0f

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

/* S5's single-tick "is this reading bad right now" test, factored out of
 * safety_guards_tick()'s S5 block so safety_guards_try_clear()'s immediate-
 * condition check (below) can ask the exact same question without
 * duplicating -- and risking drifting out of sync with -- the trip logic's
 * own definition of a bad read. See safety_guards_tick()'s S5 comment for
 * why each term is included. */
static bool s5_bad_read_now(const safety_guard_input_t *in)
{
    return in->spi_failed || !in->tc_valid || isnan(in->tc_c) ||
           ((in->fault_bits & (SAFETY_THERMO_FAULT_OPEN | SAFETY_THERMO_FAULT_OVUV |
                                SAFETY_THERMO_FAULT_TCRANGE)) != 0u);
}

/* 2026-08-23 hardware finding: safety_guards_try_clear() resets every
 * graduated guard's elapsed-time accumulator to zero (safety_guards_clear())
 * before its one-tick retest. For a guard whose trip condition needs several
 * seconds-to-minutes of SUSTAINED input to re-fire (S5/S12/S13/S6b below),
 * that reset means the retest tick almost never re-trips even when the
 * underlying hazard is still happening RIGHT NOW -- observed live: clearing
 * a latched S5 trip against a still-absent safety TC granted ~45s of
 * heating-enabled operation with a genuinely blind sensor, because the
 * cleared s5_bad_streak/s5_bad_elapsed_s had to re-accumulate from zero
 * before WARN, let alone TRIP, could fire again. safety_guards_try_clear()'s
 * own doc comment already documented the general "graduated guards don't
 * necessarily re-trip on this exact call" limitation as an accepted scope
 * limit -- what this function closes is the specific, worse case: heat gets
 * PERMITTED, not just "trip not yet re-confirmed", while the immediate,
 * single-tick-checkable condition is still true.
 *
 * Audit 2026-08-27 widened this from four guards to seven: S2, S3 and S11
 * were flagged above (in the original 2026-08-23 comment) as "reconstructable
 * this tick" or excluded only because their supporting value gets zeroed by
 * safety_guards_clear() -- neither reason survives inspection. S2/S3 read
 * nothing this function can't also read straight from `in`/`cfg` (the same
 * caller-reduced facts safety_guards_tick() itself uses), and S11's
 * seemingly-disqualifying "last tick's value" is `state->s11_last_c`, which
 * is still intact at THIS point in safety_guards_try_clear() -- this check
 * runs BEFORE safety_guards_clear() zeroes it, so "is the reading still
 * frozen at the value it tripped on" is answerable without trusting anything
 * that has been reset. Worst case before this widening was S3 (welded SSR):
 * a clear was accepted, relay_owner re-armed, and it took another full
 * stuck_on_time_s (20s default) of mains into a failed-closed element before
 * the guard could trip again -- repeatable indefinitely by re-clearing. S9
 * is the eighth graduated guard on the original list and is NOT handled here
 * even though it would otherwise qualify (trip_ineffective is a plain latch,
 * trivially "still true" every time) -- see safety_guards_try_clear()'s own
 * comment: S9/TRIP_INEFFECTIVE is refused unconditionally, before this
 * function is ever consulted, so recomputing its condition here would be
 * dead code guarding an unreachable branch.
 *
 * S1 and S8 (REVIEW_SAFTYFW_TRIP_PATH_2026-10-10 T1): handled above. S1's
 * single-tick level is the over-ceiling test its debounce counts; S8 re-evaluates
 * the window that tripped (state not yet zeroed) against the current reading.
 * Returning false for any other reason leaves
 * safety_guards_try_clear()'s existing one-tick-retest behaviour completely
 * unchanged for every guard not listed here. */
static bool guard_condition_still_immediate(safety_trip_t reason, const safety_guard_cfg_t *cfg,
                                             const safety_guard_input_t *in,
                                             const safety_guard_state_t *state)
{
    switch (reason) {
    case SAFETY_TRIP_SENSOR_INVALID: /* S5 */
        return s5_bad_read_now(in);
    case SAFETY_TRIP_OVERTEMP: /* S1 -- reading still above the absolute ceiling right now
                                * (REVIEW_SAFTYFW_TRIP_PATH T1: the S1 debounce counts exactly
                                * this level; refusing here never loosens the guard). */
        /* Review saftyfx6 F4: an unknown reading (bad TC) is not a pass -- refuse. */
        return cfg->abs_max_temp_c > 0.0f && (!in->tc_valid || in->tc_c > cfg->abs_max_temp_c);
    case SAFETY_TRIP_RATE: { /* S8 -- the window that tripped (baseline + elapsed are still in
                              * `state`, this runs before safety_guards_clear()) measured
                              * against the CURRENT reading still exceeds the rate. A cooled
                              * kiln gives a smaller or negative delta and the clear is granted. */
        if (cfg->max_rate_c_per_min <= 0.0f) {
            return false;
        }
        if (!in->tc_valid) { /* review saftyfx6 F4: unknown reading refuses */
            return true;
        }
        /* Review saftyfx6 F2 + saftyfx7 MED-1: a grant is decided only from a
         * COMPLETED full post-trip window (s8_post_windows_done), never from a
         * partial one alone -- a few seconds of TC noise can fake a plateau on
         * a kiln still rising fast. Until the first full window completes the
         * frozen tripping window decides (refuses while it exceeds the limit).
         * Once a full window exists, refuse if EITHER the last full rate OR the
         * partial window now accumulating (>= S8_POST_MIN_S) is over the limit. */
        if (state->s8_post_windows_done == 1u) {
            /* review s8clrfx LOW-1: a grant needs TWO consecutive full post
             * windows at or under the limit (mirrors the trip streak), so one
             * outlier anchor cannot grant. After only one, refuse. */
            return true;
        }
        if (state->s8_post_windows_done >= 2u) {
            if (state->s8_post_last_rate_c_per_min > cfg->max_rate_c_per_min ||
                state->s8_post_prev_rate_c_per_min > cfg->max_rate_c_per_min) {
                return true;
            }
            if (state->s8_post_elapsed_s >= S8_POST_MIN_S) {
                float m = state->s8_post_elapsed_s / 60.0f;
                if (((in->tc_c - state->s8_post_start_c) / m) > cfg->max_rate_c_per_min) {
                    return true;
                }
            }
            return false;
        }
        if (state->s8_window_active && state->s8_window_elapsed_s > 0.0f) {
            float elapsed_min = state->s8_window_elapsed_s / 60.0f;
            return ((in->tc_c - state->s8_window_start_c) / elapsed_min) > cfg->max_rate_c_per_min;
        }
        return false;
    }
    case SAFETY_TRIP_OVER_SETPOINT: /* S2 -- still over max zone setpoint + margin right now */
        return in->context_valid && cfg->tc_placement_valid &&
               cfg->tc_placement_mode == SAFETY_TC_CHAMBER_AGREED && in->zone_count > 0u &&
               in->tc_valid &&
               in->tc_c > in->max_zone_setpoint_c + effective_f(cfg->overshoot_margin_c, OVERSHOOT_MARGIN_C_DEFAULT);
    case SAFETY_TRIP_LOAD_STUCK_ON: /* S3 -- current still present with nothing commanded on */
        return in->context_valid && in->any_current_present && !in->relay_commanded_recently;
    case SAFETY_TRIP_FROZEN_SENSOR: /* S11 -- reading still sitting at the value that tripped it.
                                      * state->s11_last_c has not been zeroed yet -- this check
                                      * runs before safety_guards_clear() -- so this is a real
                                      * comparison against the reading in effect at trip time, not
                                      * a guess against already-cleared state.
                                      *
                                      * 2026-08-27 audit: this used to also require in->heat_commanded,
                                      * which safety_core.c wires from any_current_present (see
                                      * safety_guard_input_t's own comment on heat_commanded). That
                                      * made the clause "structurally cannot fail" in the
                                      * false-returning direction -- by the time an operator reaches
                                      * CLEAR_TRIP after an S11 trip, relay_owner has already
                                      * de-energized K4 in response to is_tripped, current has
                                      * stopped, and any_current_present (hence heat_commanded) reads
                                      * false on every post-trip tick. The clause could never see the
                                      * one input combination it was written to refuse.
                                      *
                                      * heat_commanded cannot be rehabilitated here: it is defined to
                                      * be false exactly when it needs to matter (post-trip, relay
                                      * open), so there is no honest way to recompute "heat is still
                                      * being commanded" from a post-trip input. What DOES survive the
                                      * trip is the frozen-reading evidence itself -- whether the
                                      * safety TC is still reporting the identical value it tripped
                                      * on. That is real, self-resolving evidence: a merely-idle kiln
                                      * cools once K4 opens, so tc_c drifts away from s11_last_c within
                                      * a tick or two and the clear is granted; a genuinely stuck
                                      * sensor keeps reporting the same frozen value and the clear is
                                      * correctly refused until the reading actually moves (or the
                                      * sensor is replaced) -- matching SAFETY_MODEL.md's "a
                                      * genuinely static value ... while energy is going in, does not
                                      * happen in a real thermal system" reasoning, applied to "does
                                      * the evidence that caused the trip still hold" rather than to
                                      * whether heat happens to be commanded on this exact tick. */
        return in->tc_valid && state->s11_window_active && in->tc_c == state->s11_last_c;
    case SAFETY_TRIP_ENCLOSURE_TEMP: /* S12 -- cj_c still over cj_max_c right now */
        /* F4: an unknown cold junction (NaN / cj_invalid) means the
         * enclosure condition cannot be shown to have cleared -> still
         * immediate, so the clear is refused. */
        /* Review saftyfx6 F1: no tc_valid term; T4 lets S12 trip on a bad TC read. */
        return in->cj_invalid || isnan(in->cj_c) ||
               (in->cj_c > effective_f(cfg->cj_max_c, CJ_MAX_C_DEFAULT));
    case SAFETY_TRIP_BORROWED_STALE: /* S13 -- channel still not producing fresh samples */
        return (cfg->tc_source == SAFETY_TC_SOURCE_BORROWED_ZONE ||
                cfg->tc_source == SAFETY_TC_SOURCE_BOTH) &&
               !in->sample_counter_advancing;
    case SAFETY_TRIP_LINK_DEAD: /* S6b -- link still silent right now */
        return !in->link_up;
    default:
        return false;
    }
}

bool safety_guards_try_clear(safety_guard_state_t *state, const safety_guard_cfg_t *cfg,
                              const safety_guard_input_t *in)
{
    /* S9/TRIP_INEFFECTIVE: unconditional refusal, no recompute needed.
     * ARCHITECTURE.md section 9 and SAFETY_MODEL.md section 4 both describe
     * this trip as having "no exit except power removal at the breaker" --
     * it means K4 was told to open and current is STILL flowing, i.e. the
     * contactor is welded and mains may be live on fused contacts regardless
     * of anything this firmware commands. A CLEAR_TRIP here would wipe
     * trip_ineffective, re-arm K4, and silence the one alarm whose required
     * response is "go to the breaker", while doing nothing about the welded
     * contacts themselves. Checked here, ahead of and independent of
     * guard_condition_still_immediate(), so it can never be bypassed by that
     * function's guard-specific logic; also enforced at the wire layer by
     * link_frame_decide_clear_trip() (audit 2026-08-27) so the refusal is
     * visible with a reason before this function is even reached, not just
     * failing silently here. */
    if (state->is_tripped && state->trip_ineffective) {
        return false;
    }

    /* Refuse before touching any accumulator at all, for the guards whose
     * trip condition is a plain level this tick's `in` (and, for S11, the
     * not-yet-zeroed `state`) already answers -- see
     * guard_condition_still_immediate()'s own comment for which guards and
     * why this check has to run BEFORE safety_guards_clear() zeroes the
     * state that would otherwise mask it. `state` is left completely
     * untouched here (still is_tripped, same reason, same accumulators it
     * had on entry) -- a refused clear must look exactly like a clear that
     * was never attempted. */
    if (state->is_tripped && guard_condition_still_immediate(state->reason, cfg, in, state)) {
        return false;
    }
    safety_guards_clear(state);
    return !safety_guards_tick(state, cfg, in);
}

bool safety_guards_clear_trip_occurrence_matches(bool bound, uint8_t wire_trip_seq,
                                                  uint8_t current_trip_seq)
{
    return !bound || wire_trip_seq == current_trip_seq;
}

safety_clear_trip_outcome_t safety_guards_decide_clear_trip_outcome(bool was_tripped,
                                                                      bool occurrence_matches,
                                                                      bool try_clear_result)
{
    if (!was_tripped) {
        return SAFETY_CLEAR_TRIP_OUTCOME_REFUSED_NOTHING_LATCHED;
    }
    if (!occurrence_matches) {
        return SAFETY_CLEAR_TRIP_OUTCOME_REFUSED_STALE_OCCURRENCE;
    }
    return try_clear_result ? SAFETY_CLEAR_TRIP_OUTCOME_ACCEPTED
                             : SAFETY_CLEAR_TRIP_OUTCOME_REFUSED_STILL_TRIPPED;
}

float safety_guards_deciding_threshold_c(safety_trip_t reason, const safety_guard_cfg_t *cfg)
{
    switch (reason) {
    case SAFETY_TRIP_OVERTEMP:
        /* S1. Reports the configured absolute ceiling (abs_max_temp_c) --
         * owner decision 2026-09-24: this IS the ceiling that decides a trip,
         * unconditionally (the safety processor is a backup and must never
         * run tighter than the ESP's own limit; the former firing_max_c/
         * firing_margin_c min() tightening was removed). abs_max_temp_c has
         * no substituted default (safety_guards.h: "0 = not commissioned,
         * guard never trips"), so if this guard tripped, this value is real
         * and non-zero -- never a silently-substituted fallback. */
        return cfg->abs_max_temp_c;
    case SAFETY_TRIP_OVER_SETPOINT: /* S2 */
        return effective_f(cfg->overshoot_margin_c, OVERSHOOT_MARGIN_C_DEFAULT);
    case SAFETY_TRIP_LOAD_STUCK_ON: /* S3 */
        return effective_f(cfg->i_present_a, I_PRESENT_A_DEFAULT);
    case SAFETY_TRIP_RATE: /* S8. max_rate_c_per_min has no substituted default
                             * (safety_guards.h: "0 = not commissioned, guard
                             * never trips"), so if this guard tripped, this
                             * value is real and non-zero -- same reasoning as
                             * S1's abs_max_temp_c just above. */
        return cfg->max_rate_c_per_min;
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

/* S12 evaluation, shared by the normal path and the bad-TC-read path (T4,
 * REVIEW_SAFTYFW_TRIP_PATH_2026-10-10). Returns true if it tripped. */
static bool s12_evaluate(safety_guard_state_t *state, const safety_guard_cfg_t *cfg,
                         const safety_guard_input_t *in)
{
    {
        float cj_max = effective_f(cfg->cj_max_c, CJ_MAX_C_DEFAULT);
        float cj_warn = effective_f(cfg->cj_warn_c, CJ_WARN_C_DEFAULT);

        if (in->cj_invalid || isnan(in->cj_c)) {
            /* Guard review F4: an unknown cold junction (CJRANGE, i.e.
             * enclosure beyond the part's range -- the hottest possible
             * S12 case) is never a pass. Raise the WARN and HOLD the
             * accumulator (neither advance nor reset), and
             * guard_condition_still_immediate() refuses to clear an S12
             * trip while cj is unknown. */
            state->s12_warn = true;
        } else if (in->cj_c > cj_max) {
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

bool safety_guards_tick(safety_guard_state_t *state, const safety_guard_cfg_t *cfg,
                         const safety_guard_input_t *in)
{
    /* Level-tracked, non-latching, evaluated before anything else and on
     * EVERY path through this function including the already-tripped one --
     * "the CT-fed guards are not watching" is a fact about this board, not
     * about this tick's outcome, so it must not disappear the moment
     * something trips. See safety_guard_input_t::current_sensing_disabled. */
    state->ct_guards_disabled = in->current_sensing_disabled;

    if (state->is_tripped) {
        /* Review saftyfx6 F2: after an S8 trip keep measuring a fresh rate so the
         * clear refusal in guard_condition_still_immediate() is bounded in time.
         * Only used by the clear check; never touches the trip path. */
        if (state->reason == SAFETY_TRIP_RATE && in->tc_valid) {
            if (!state->s8_post_active) {
                state->s8_post_active = true;
                state->s8_post_start_c = in->tc_c;
                state->s8_post_elapsed_s = 0.0f;
                /* review s8clrfx LOW-3: latch + clamp the post window at trip time
                 * so a mid-trip rate_window_s change cannot shorten or stick it. */
                float lw = effective_f(cfg->rate_window_s, RATE_WINDOW_S_DEFAULT);
                state->s8_post_win_s = lw < S8_POST_WIN_MIN_S ? S8_POST_WIN_MIN_S
                                       : (lw > S8_POST_WIN_MAX_S ? S8_POST_WIN_MAX_S : lw);
            } else {
                state->s8_post_elapsed_s += in->dt_s;
                float win = state->s8_post_win_s;
                if (state->s8_post_elapsed_s >= win) {
                    state->s8_post_prev_rate_c_per_min = state->s8_post_last_rate_c_per_min;
                    if (state->s8_post_windows_done < 2u) {
                        state->s8_post_windows_done++;
                    }
                    state->s8_post_last_rate_c_per_min =
                        (in->tc_c - state->s8_post_start_c) / (state->s8_post_elapsed_s / 60.0f);
                    state->s8_post_start_c = in->tc_c;
                    state->s8_post_elapsed_s = 0.0f;
                }
            }
        }
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
                /* Three independent bars, neither alone sufficient (2026-08-27
                 * audit, regression from commit 4962421 -- and a second pass
                 * the same day after the first fix was found insufficient):
                 * trip_verify_s elapsing is necessary but NOT sufficient --
                 * unlike every other graduated guard in this file (S1's
                 * streak, S5/S12/S13's count-and-time pairs), the original
                 * code let a single post-threshold tick with
                 * any_current_present true latch this unconditionally, with
                 * no debounce, no context_valid gate, and no check that the
                 * current measurement itself was trustworthy.
                 *
                 * in->context_valid is deliberately NOT required (2026-10-09
                 * review F3, superseding the c3542d97 rationale): S3 needs
                 * the ESP's heat command, S9 only the Pico's own
                 * de-energize command. The current-trust concern is
                 * covered by the debounce streak and the commissioned split
                 * below.
                 *
                 * The debounce streak (S9_CURRENT_PRESENT_STREAK_TO_TRIP
                 * consecutive ticks, below) catches a TRANSIENT spurious
                 * reading -- a single noisy sample. It does NOT catch a
                 * PERSISTENT one: current_presence_policy.h's own header
                 * comment documents that an uncommissioned k_ct_v_per_a makes
                 * any_current_present run a deliberately sensitive counts-
                 * domain fallback that reads a channel's DC offset floor as
                 * "current present" on every single tick, forever -- a
                 * systematic bias, not noise, that 3 consecutive ticks (or
                 * 3000) cannot distinguish from a real weld. That is what
                 * in->current_sensing_commissioned exists to gate (see its
                 * own doc comment in safety_guards.h for why
                 * config_store's calibration_missing cannot answer this):
                 * only once the current chain is actually calibrated does
                 * the streak progress toward the unclearable trip_ineffective
                 * latch. Uncommissioned, the same finding is still reported
                 * -- loudly, as SAFETY_MODEL.md section 4 insists S9 must be
                 * ("the single most valuable guard after S1" specifically
                 * because it converts a silent failure into a loud one) --
                 * just as a non-latching WARN (s9_uncommissioned_warn, same
                 * idiom as s4_warn/s10_warn below) instead of a latch neither
                 * commissioning nor CLEAR_TRIP can ever undo.
                 *
                 * None of this makes S9's TRIP_INEFFECTIVE clearable again:
                 * once latched, it is still refused unconditionally by
                 * safety_guards_try_clear() and link_frame_decide_clear_trip().
                 * This only changes what evidence is required to set it. */
                /* No in->context_valid gate (guard review 2026-10-09, F3): S9
                 * reasons about the Pico's OWN de-energize command, which it
                 * always knows. Gating on ESP context made S9 blind in exactly
                 * the case it exists for (K4 welded while the ESP is dead). */
                if (state->s9_verify_elapsed_s >= verify_th && in->any_current_present) {
                    if (in->current_sensing_disabled) {
                        /* No CT is fitted. any_current_present here is the
                         * op-amp offset floor read through an uncalibrated
                         * chain, not evidence that K4 failed to open. Do not
                         * progress the streak, and do NOT raise
                         * s9_uncommissioned_warn -- that warn's meaning is
                         * "commission the current chain and S9 comes back",
                         * which is not true on a board that has no sensor to
                         * commission. state->ct_guards_disabled (set at the
                         * top of this function) is the honest report. */
                        state->s9_current_present_streak = 0;
                        state->s9_uncommissioned_warn = false;
                    } else if (in->current_sensing_commissioned) {
                        state->s9_uncommissioned_warn = false;
                        if (state->s9_current_present_streak < UINT8_MAX) {
                            state->s9_current_present_streak++;
                        }
                        if (state->s9_current_present_streak >= S9_CURRENT_PRESENT_STREAK_TO_TRIP) {
                            state->trip_ineffective = true;
                            trip(state, SAFETY_TRIP_INEFFECTIVE,
                                 "K4 de-energized for %.1fs (>= trip_verify_s %.1fs) but current still "
                                 "present for %u consecutive ticks",
                                 (double)state->s9_verify_elapsed_s, (double)verify_th,
                                 (unsigned)state->s9_current_present_streak);
                            return true; /* newly escalated -- caller reacts once, same contract as any new trip */
                        }
                    } else {
                        /* Uncommissioned current sensing: cannot trust this
                         * enough to progress toward the unclearable latch,
                         * but the finding is real evidence and must not be
                         * silenced -- WARN instead (non-latching, cleared the
                         * instant the condition stops holding). */
                        state->s9_uncommissioned_warn = true;
                        state->s9_current_present_streak = 0;
                    }
                } else {
                    state->s9_current_present_streak = 0;
                    state->s9_uncommissioned_warn = false;
                }
            } else {
                /* relay_owner has not (yet) reported K4 de-energized -- do
                 * not start the clock on a verification window that hasn't
                 * actually begun. */
                state->s9_verify_active = false;
                state->s9_verify_elapsed_s = 0.0f;
                state->s9_current_present_streak = 0;
                state->s9_uncommissioned_warn = false;
            }
        }
        return false; /* already latched -- caller should have de-energized K4 already */
    }

    /* --- S16: config_store RAM integrity corrupted twice this boot ---------
     * config_store_flash.c's config_store_check_ram_integrity() (ticked by
     * link_task at config_check_period_s cadence) already reloaded from the
     * flash-truth copy and forced calibration_missing on the FIRST
     * corruption -- that alone is WARN territory, not a trip, per
     * SAFETY_MODEL.md section 4. This module only ever sees the input
     * already collapsed to "did a SECOND one happen" (config_integrity_trip,
     * safety_core_build_input() reading config_store_ram_integrity_
     * recurrence_pending()), so there is nothing graduated to do here:
     * unconditional, like S6a/S7 just below, because "RAM is being actively
     * corrupted" is exactly the class of fact that must not wait on
     * context_valid or any other gate to be believed. */
    if (in->config_integrity_trip) {
        trip(state, SAFETY_TRIP_CONFIG_CORRUPT,
             "config_store RAM integrity corrupted twice this boot (recurrence)");
        return true;
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
     * matching S12/S2's own "reset on a healthy tick" pattern.
     *
     * reboot_grace_active (KilnFW/TODO.md's ANNOUNCE_REBOOT line) gates ONLY
     * the two trip() calls below -- it never touches the elapsed-time
     * accumulator. That is the whole mechanism that makes "the window
     * expiring reverts to exactly the same behavior as if ANNOUNCE_REBOOT
     * had never arrived" true: elapsed keeps counting real silence the
     * entire time, so if the ESP is still quiet once the window closes,
     * whichever threshold was already crossed fires on the very next tick
     * with no grace period of its own -- there is no separate "second
     * chance" timer hiding in this suppression. */
    if (in->link_up) {
        state->s6b_link_down_elapsed_s = 0.0f;
    } else {
        state->s6b_link_down_elapsed_s += in->dt_s;
        float hard = effective_f(cfg->link_dead_hard_s, LINK_DEAD_HARD_S_DEFAULT);
        if (state->s6b_link_down_elapsed_s >= hard) {
            if (!in->reboot_grace_active) {
                trip(state, SAFETY_TRIP_LINK_DEAD, "link silent for %.1fs (>= link_dead_hard_s %.1fs), unconditional",
                     (double)state->s6b_link_down_elapsed_s, (double)hard);
                return true;
            }
            /* Suppressed: the ESP announced this reboot and the grace
             * window is still open. Nothing else about this tick changes --
             * no other guard reads reboot_grace_active, and relay_owner's
             * energize/ARM path has no way to observe this field at all. */
        }
        float soft = effective_f(cfg->link_timeout_s, LINK_TIMEOUT_S_DEFAULT);
        if (state->s6b_link_down_elapsed_s >= soft && in->any_current_present) {
            if (!in->reboot_grace_active) {
                trip(state, SAFETY_TRIP_LINK_DEAD, "link silent for %.1fs (>= link_timeout_s %.1fs) with current present",
                     (double)state->s6b_link_down_elapsed_s, (double)soft);
                return true;
            }
            /* Suppressed, same reasoning as the hard-backstop branch above. */
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
    bool bad_read = s5_bad_read_now(in);
    bool tc_usable = true; /* false on a bad read: S2/S10 hold, see context_guards */

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
                /* Declared-not-installed escape hatch (config param 0x0211,
                 * safety_core.c's inversion into
                 * in->safety_tc_not_installed_declared). SAFETY_MODEL.md
                 * section 4's whole point for S5 is that a PERMANENTLY
                 * blind processor must not silently preside over a firing
                 * -- but "permanently blind" here means "no sensor was ever
                 * wired up", a state the operator has explicitly declared,
                 * not an unexplained hardware fault. Promoting to TRIP in
                 * that case would just be a second, redundant way of saying
                 * what s5_warn (set unconditionally above, regardless of
                 * this branch) already says, while permanently blocking
                 * every bench task this board is needed for. The trade this
                 * makes is deliberately asymmetric and is NOT a weakening
                 * on its own: safety_core_request_enable() refuses the ON
                 * direction unconditionally whenever this same config field
                 * is 0 (see that function's own comment), so downgrading
                 * THIS trip to a permanent warning never grants heat by
                 * itself -- the enable path is the one actually doing the
                 * refusing, every time, with no time-boxing or accumulator
                 * to reset. s5_bad_streak/s5_bad_elapsed_s keep
                 * accumulating exactly as before (this branch does not
                 * touch them) so diagnostics -- "how long has it actually
                 * been blind" -- are unaffected. */
                if (in->safety_tc_not_installed_declared) {
                    state->s5_not_installed = true;
                } else {
                    trip(state, SAFETY_TRIP_SENSOR_INVALID,
                         "blind for %.1fs (>= blind_grace_s %.1fs), %u consecutive bad reads",
                         (double)state->s5_bad_elapsed_s, (double)grace, (unsigned)state->s5_bad_streak);
                    return true;
                }
            }
        }

        /* A bad read can't feed S1/S11/S12/S8 below a real number -- skip
         * those rather than reasoning about a value that isn't
         * trustworthy (same discipline thermal_guard.c's guard 6 uses for
         * guards 1/2/3/4/7 below it). 2026-10-09 review F6: the
         * TC-independent context guards (S3, S4, S13, S14, S15) must NOT
         * be suspended by a blind TC, so jump to the context block; S2 and
         * S10 inside it consume tc_c and hold their state while
         * tc_usable is false. */
        /* T4: the cold junction is a separate MAX31856 register, usually still
         * valid on the commonest fault (open TC). Run S12 on a valid CJ so the
         * enclosure guard is not blind; S5 is untouched. */
        if (!in->cj_invalid && !isnan(in->cj_c) && s12_evaluate(state, cfg, in)) {
            return true;
        }
        tc_usable = false;
        goto context_guards;
    }

    state->s5_bad_streak = 0;
    state->s5_bad_elapsed_s = 0.0f;
    state->s5_warn = false;
    state->s5_not_installed = false;

    /* --- S1: absolute over-temperature ---------------------------------------
     * abs_max_temp_c == 0 means "not commissioned" -- never trip, and never
     * accumulate a streak toward one (SAFETY_MODEL.md section 4, S1: "has
     * no default and must be commissioned"). */
    if (cfg->abs_max_temp_c > 0.0f) {
        /* Owner decision 2026-09-24: the safety processor is a backup in
         * case the ESP fails, and must not run a tighter limit than the
         * ESP's own ceiling. S1 always trips at abs_max_temp_c alone --
         * the SET_FIRING_CEILING (0x09) tightening layered on top of it
         * (firing_max_c + firing_margin_c, min()'d against abs_max_temp_c)
         * has been removed. */
        float ceiling = cfg->abs_max_temp_c;

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
    if (in->tc_valid && s12_evaluate(state, cfg, in)) {
        return true;
    }

    /* --- S8: implausible rate of rise -----------------------------------------
     * SAFETY_MODEL.md section 4, S8: "d(safety_tc_c)/dt > max_rate_c_per_min
     * sustained for rate_window_s". Ships disabled: max_rate_c_per_min == 0
     * means "not commissioned, never trip" -- the same convention as S1's
     * abs_max_temp_c, and for the identical reason: "the correct threshold
     * depends on the kiln's mass, element power and insulation, and nobody
     * has ever measured this kiln's maximum legitimate ramp rate." A
     * substituted magnitude here would be a missed-trip risk if guessed too
     * high, and exactly the nuisance-trip generator SAFETY_MODEL.md section
     * 2 forbids if guessed too low -- neither is a call this module gets to
     * make silently.
     *
     * Measured as an AVERAGE rate over one whole rate_window_s window
     * (baseline-sample-and-hold: remember the reading at the start of the
     * window, compare against the reading rate_window_s later), never a
     * per-tick instantaneous derivative. At this module's ~100ms tick, a
     * per-tick dT/dt amplifies ordinary MAX31856 read noise by roughly
     * dt_s's own reciprocal in minutes -- a mere 0.1C jitter between two
     * consecutive 100ms ticks already reads as 60C/min, comfortably past any
     * threshold anyone would ever commission -- which would turn every board
     * that enables this guard into a nuisance-trip generator on a perfectly
     * healthy sensor, the opposite of the doctrine this file is built on.
     * Averaging over the full window is also what makes the guard
     * "sustained" in the sense SAFETY_MODEL.md means it: a fast transient
     * cannot move a whole-window average past the threshold by itself.
     *
     * Requiring TWO consecutive over-threshold window evaluations
     * (S8_OVER_RATE_STREAK_TO_TRIP, see its own comment above) before
     * tripping is the second, independent bar: it is what keeps a single
     * glitchy sample landing on one window boundary from being read as a
     * runaway, without weakening the response to a genuine sustained climb,
     * which clears both windows with margin. */
    if (in->tc_valid) {
        if (cfg->max_rate_c_per_min > 0.0f) {
            if (!state->s8_window_active) {
                state->s8_window_active = true;
                state->s8_window_start_c = in->tc_c;
                state->s8_window_elapsed_s = 0.0f;
            } else {
                state->s8_window_elapsed_s += in->dt_s;
                float window_th = effective_f(cfg->rate_window_s, RATE_WINDOW_S_DEFAULT);
                if (state->s8_window_elapsed_s >= window_th) {
                    float delta_c = in->tc_c - state->s8_window_start_c;
                    float elapsed_min = state->s8_window_elapsed_s / 60.0f;
                    float rate_c_per_min = delta_c / elapsed_min;

                    if (rate_c_per_min > cfg->max_rate_c_per_min) {
                        if (state->s8_over_rate_streak < UINT8_MAX) {
                            state->s8_over_rate_streak++;
                        }
                    } else {
                        state->s8_over_rate_streak = 0;
                    }

                    if (state->s8_over_rate_streak >= S8_OVER_RATE_STREAK_TO_TRIP) {
                        trip(state, SAFETY_TRIP_RATE,
                             "%.1fC/min > max_rate_c_per_min %.1fC/min over %.0fs window (%.1fC -> %.1fC), "
                             "%u consecutive windows",
                             (double)rate_c_per_min, (double)cfg->max_rate_c_per_min,
                             (double)state->s8_window_elapsed_s, (double)state->s8_window_start_c,
                             (double)in->tc_c, (unsigned)state->s8_over_rate_streak);
                        return true;
                    }

                    /* Window complete either way: slide to a fresh window
                     * starting now rather than growing the baseline further.
                     * A sliding window keeps the guard responsive to a
                     * runaway that starts partway through what would
                     * otherwise be an arbitrarily long accumulation, and
                     * keeps every window's rate calculation an honest
                     * rate_window_s-long average rather than a stretched
                     * one. */
                    state->s8_window_start_c = in->tc_c;
                    state->s8_window_elapsed_s = 0.0f;
                }
            }
        } else {
            state->s8_window_active = false;
            state->s8_window_elapsed_s = 0.0f;
            state->s8_over_rate_streak = 0;
        }
    }
    /* A bad read (bad_read above) already returned before reaching here, so
     * this block never runs against an invalid/NaN/stale sample -- it simply
     * leaves the window paused (state untouched) across the bad ticks, the
     * same "don't advance on garbage, don't reset on a transient either"
     * treatment S1's streak gets. If the sensor stays bad long enough, S5
     * trips first via its own, independent, faster path (bad_read_count_
     * threshold / bad_read_time_s / blind_grace_s), which fires on this same
     * tick's early return -- S8 never gets a chance to see the recovering
     * value at all until S5 either clears or this whole guard set is
     * latched by S5's own trip. */

context_guards:
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
        for (int ch = 0; ch < 3; ch++) {
            state->s14_over_elapsed_s[ch] = 0.0f;
            state->s14_warn[ch] = false;
            state->s15_under_elapsed_s[ch] = 0.0f;
            state->s15_warn[ch] = false;
        }
    } else {
        /* --- S2/S3/S4 SIM_PLANT disable (TODO.md "Honour the SIM_PLANT
         * flag"). See safety_guard_input_t::sim_plant_disable_active's own
         * comment for how this bool got here and why it can only ever be
         * true in a Pico firmware deliberately built with SAFTYFW_HONOR_
         * SIM_PLANT. Scoped to S2/S3/S4 ONLY (TODO.md's own scope) -- S10/
         * S13/S14/S15 below are NOT gated by this and always evaluate
         * normally, sim-plant context or not, since they do not correlate
         * against the plant model the way S2/S3/S4 do. When active, S2/S3/
         * S4 go inactive for the tick, the identical reset shape
         * !in->context_valid gets just above -- an old accumulating window
         * from before SIM_PLANT started reporting must not silently resume
         * and trip on its pre-disable progress once it stops. */
        if (in->sim_plant_disable_active) {
            state->s2_over_elapsed_s = 0.0f;
            state->s3_stuck_elapsed_s = 0.0f;
            state->s4_warn = false;
        } else {
        /* --- S2: sustained excess over setpoint --------------------------------
         * CHAMBER_AGREED only -- comparing a shell/exhaust reading against a
         * chamber setpoint is meaningless (SAFETY_MODEL.md section 4, S2).
         * zone_count == 0 means "no active zones this tick", same inactive
         * treatment as stale context. */
        if (!tc_usable) {
            /* bad TC read: hold the S2 accumulator (F6), as before this tick's early return */
        } else if (cfg->tc_placement_valid && cfg->tc_placement_mode == SAFETY_TC_CHAMBER_AGREED &&
            in->zone_count > 0u && in->tc_valid) {
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
        if (in->current_sensing_disabled) {
            /* INERT, not passing. Without this branch a CT-less board trips
             * SAFETY_TRIP_LOAD_STUCK_ON within stuck_on_time_s of boot, off
             * the uncalibrated presence heuristic -- see
             * safety_guard_input_t::current_sensing_disabled. */
            state->s3_stuck_elapsed_s = 0.0f;
        } else if (in->any_current_present && !in->relay_commanded_recently) {
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
        state->s4_warn = !in->current_sensing_disabled && in->relay_commanded_continuously &&
                          !in->any_current_present;
        }

        /* --- S10: safety TC disagrees with every zone TC -- WARN only ----------
         * CHAMBER_AGREED only, same reasoning as S2 (SAFETY_MODEL.md section
         * 4, S10). Compares against the caller-supplied *nearest* valid zone
         * reading, never the mean -- kilns stratify. */
        if (!tc_usable) {
            /* bad TC read: hold the S10 state (F6) */
        } else if (cfg->tc_placement_valid && cfg->tc_placement_mode == SAFETY_TC_CHAMBER_AGREED &&
            in->zone_count > 0u && in->tc_valid) {
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

        /* --- S14: zone current above its measured normal -- WARN only ---------
         * COMMISSIONING_UX.md section 3.2. Per channel: the channel's own
         * i_normal_a must actually be measured (i_normal_valid[ch]) -- a
         * channel that has never been measured is SKIPPED ENTIRELY, no
         * accumulation, no warn, reports inactive (s14_warn[ch] stays false,
         * s14_over_elapsed_s[ch] stays 0) -- never treated as "passing",
         * matching S6a/S6b's DEGRADED_NO_CONTEXT "inactive, not pessimistic,
         * not optimistic" rule applied per channel instead of per guard.
         * WARN, never TRIP (SAFETY_MODEL.md section 2's "WARN is the default
         * for a new guard"; over-current is the breakers' job by design,
         * section 3/7) -- this guard exists to catch a CT on the wrong jack
         * or an element/wiring change, not to protect against element
         * failure. Magnitude is a PERCENTAGE of the channel's own recorded
         * normal, never an absolute figure and never compared across
         * channels -- three zones on one kiln can legitimately differ 2x in
         * draw. */
        if (cfg->zone_ct_channel_valid) {
            /* --- Generalised per-channel attribution (CT_CHANNEL_MASK.md
             * step 4). member(ch) = { z : zone_ct_channel[z] == ch }. Every
             * channel is evaluated against the sum of its own commanded
             * members' normals, and S15 -- which needs a SHARED CT to have a
             * deficit worth attributing -- is gated to channels with at least
             * two members.
             *
             * This arm runs ONLY for a record that committed the map. Both
             * legacy maps collapse onto the two branches below exactly:
             * identity {0,1,2} gives every channel one member, so the sum is
             * that one zone's normal and S15 is inert everywhere (per_zone);
             * {2,2,2} gives channels 0/1 no members (inert) and channel 2 all
             * three, i.e. the summed branch. test_safety_guards.c proves both
             * collapses against the legacy arms' own numbers.
             *
             * The one deliberate difference from the per_zone arm below: a
             * member zone's "commanded now" fact is read per ZONE
             * (relay_commanded_now_for_zone) rather than through
             * ct_channel_map's channel -> relay indirection. See the
             * zone_ct_channel comment in safety_guards.h. */
            for (int ch = 0; ch < 3; ch++) {
                int members[3];
                int member_count = 0;
                for (int z = 0; z < 3; z++) {
                    if (cfg->zone_ct_channel[z] == (uint8_t)ch) {
                        members[member_count++] = z;
                    }
                }

                bool any_commanded = false;
                bool commanded_normals_known = true;
                float expected_sum_a = 0.0f;
                for (int m = 0; m < member_count; m++) {
                    int z = members[m];
                    if (!in->relay_commanded_now_for_zone[z]) {
                        continue;
                    }
                    any_commanded = true;
                    if (!cfg->i_normal_valid[z] || cfg->i_normal_a[z] <= 0.0f) {
                        /* Same rule as both legacy arms: one commanded member
                         * with no measured normal makes the expected sum
                         * unknowable, not merely wrong -- skip the whole
                         * channel entirely rather than compare against a
                         * partial sum that is guaranteed too small. */
                        commanded_normals_known = false;
                        break;
                    }
                    expected_sum_a += cfg->i_normal_a[z];
                }
                /* member_count == 0 means nothing is wired behind this
                 * channel, so it reports inert -- never a false pass. */
                bool sum_valid = member_count > 0 && any_commanded && commanded_normals_known;

                bool s14_active = !in->current_sensing_disabled && sum_valid &&
                                   in->amps_valid[ch];
                if (!s14_active) {
                    state->s14_over_elapsed_s[ch] = 0.0f;
                    state->s14_warn[ch] = false;
                } else {
                    uint16_t pct = effective_u16(cfg->overcurrent_pct, OVERCURRENT_PCT_DEFAULT);
                    float threshold_a = expected_sum_a * (float)pct / 100.0f;
                    if (in->amps[ch] > threshold_a) {
                        state->s14_over_elapsed_s[ch] += in->dt_s;
                        float time_th = effective_f(cfg->overcurrent_time_s, OVERCURRENT_TIME_S_DEFAULT);
                        state->s14_warn[ch] = (state->s14_over_elapsed_s[ch] >= time_th);
                    } else {
                        state->s14_over_elapsed_s[ch] = 0.0f;
                        state->s14_warn[ch] = false;
                    }
                }

                /* S15: only a SHARED channel (>= 2 members) can produce a
                 * deficit that has to be attributed to one of several zones.
                 * A channel with exactly one member has no attribution
                 * problem -- and no separate guard: an open heater there is
                 * already an under-reading on that channel, which is S14's
                 * domain, exactly as in the per_zone arm below where S15 is
                 * inert. The deficit is ONE scalar per channel, tested
                 * against each commanded member's own threshold, with the
                 * same "consistent with any one of them, not all of them"
                 * reading as the summed arm. */
                bool s15_base_active = member_count >= 2 && !in->current_sensing_disabled &&
                                        sum_valid && in->amps_valid[ch];
                float deficit_a = s15_base_active ? (expected_sum_a - in->amps[ch]) : 0.0f;
                for (int m = 0; m < member_count; m++) {
                    int z = members[m];
                    bool active = s15_base_active && in->relay_commanded_now_for_zone[z] &&
                                  cfg->i_normal_valid[z] && cfg->i_normal_a[z] > 0.0f;
                    if (!active) {
                        state->s15_under_elapsed_s[z] = 0.0f;
                        state->s15_warn[z] = false;
                        continue;
                    }
                    float threshold_a = cfg->i_normal_a[z] * UNDERCURRENT_FRACTION_DEFAULT;
                    if (deficit_a > threshold_a) {
                        state->s15_under_elapsed_s[z] += in->dt_s;
                        state->s15_warn[z] = (state->s15_under_elapsed_s[z] >= UNDERCURRENT_TIME_S_DEFAULT);
                    } else {
                        state->s15_under_elapsed_s[z] = 0.0f;
                        state->s15_warn[z] = false;
                    }
                }
            }
        } else if (!cfg->ct_topology_summed) {
            for (int ch = 0; ch < 3; ch++) {
                bool active = !in->current_sensing_disabled && cfg->i_normal_valid[ch] &&
                              cfg->i_normal_a[ch] > 0.0f &&
                              in->amps_valid[ch] && in->relay_commanded_now_for_ct[ch];
                if (!active) {
                    state->s14_over_elapsed_s[ch] = 0.0f;
                    state->s14_warn[ch] = false;
                    continue;
                }
                uint16_t pct = effective_u16(cfg->overcurrent_pct, OVERCURRENT_PCT_DEFAULT);
                float threshold_a = cfg->i_normal_a[ch] * (float)pct / 100.0f;
                if (in->amps[ch] > threshold_a) {
                    state->s14_over_elapsed_s[ch] += in->dt_s;
                    float time_th = effective_f(cfg->overcurrent_time_s, OVERCURRENT_TIME_S_DEFAULT);
                    state->s14_warn[ch] = (state->s14_over_elapsed_s[ch] >= time_th);
                } else {
                    state->s14_over_elapsed_s[ch] = 0.0f;
                    state->s14_warn[ch] = false;
                }
            }
            /* No shared CT in this topology -- S15 (open-heater deficit) has
             * nothing to attribute a deficit against, so it stays inert. */
            for (int z = 0; z < 3; z++) {
                state->s15_under_elapsed_s[z] = 0.0f;
                state->s15_warn[z] = false;
            }
        } else {
            /* --- Summed topology: one CT (channel 2 / "channel 3", GPIO28)
             * reads every zone at once (CT_COMMISSIONING_PLAN.md step 3).
             * Channels 0/1 have no sensor behind them at all -- report
             * inert, never a false pass on either warn -- so only channel 2
             * (index CT_SUMMED_CHANNEL) ever evaluates S14 here, compared
             * against the SUM of i_normal_a[] for every zone commanded on
             * right now, not a single zone's own normal. */
            state->s14_over_elapsed_s[0] = 0.0f; state->s14_warn[0] = false;
            state->s14_over_elapsed_s[1] = 0.0f; state->s14_warn[1] = false;

            const int CT_SUMMED_CHANNEL = 2;
            bool commanded_normals_known = true;
            float expected_sum_a = 0.0f;
            bool any_commanded = false;
            for (int z = 0; z < 3; z++) {
                if (!in->relay_commanded_now_for_zone[z]) {
                    continue;
                }
                any_commanded = true;
                if (!cfg->i_normal_valid[z] || cfg->i_normal_a[z] <= 0.0f) {
                    /* A commanded zone with no measured normal makes the
                     * expected sum unknowable, not merely wrong -- treat
                     * exactly like S14's per-channel "never measured, skip
                     * entirely" rule, applied to the whole sum. */
                    commanded_normals_known = false;
                    break;
                }
                expected_sum_a += cfg->i_normal_a[z];
            }
            bool sum_valid = any_commanded && commanded_normals_known;

            bool s14_active = !in->current_sensing_disabled && sum_valid &&
                               in->amps_valid[CT_SUMMED_CHANNEL];
            if (!s14_active) {
                state->s14_over_elapsed_s[CT_SUMMED_CHANNEL] = 0.0f;
                state->s14_warn[CT_SUMMED_CHANNEL] = false;
            } else {
                uint16_t pct = effective_u16(cfg->overcurrent_pct, OVERCURRENT_PCT_DEFAULT);
                float threshold_a = expected_sum_a * (float)pct / 100.0f;
                if (in->amps[CT_SUMMED_CHANNEL] > threshold_a) {
                    state->s14_over_elapsed_s[CT_SUMMED_CHANNEL] += in->dt_s;
                    float time_th = effective_f(cfg->overcurrent_time_s, OVERCURRENT_TIME_S_DEFAULT);
                    state->s14_warn[CT_SUMMED_CHANNEL] =
                        (state->s14_over_elapsed_s[CT_SUMMED_CHANNEL] >= time_th);
                } else {
                    state->s14_over_elapsed_s[CT_SUMMED_CHANNEL] = 0.0f;
                    state->s14_warn[CT_SUMMED_CHANNEL] = false;
                }
            }

            /* --- S15 (new): per-zone under-current / open-heater WARN.
             * CT_COMMISSIONING_PLAN.md step 3: "commanded sum minus measured
             * > 0.7x that zone's normal for 30s". Only meaningful when the
             * expected sum itself is known (sum_valid) and the shared CT
             * reading is fresh -- otherwise a deficit cannot be attributed
             * to any single zone and every zone's S15 stays inert, same
             * "skip entirely, never a false pass" discipline as S14.
             *
             * Opus review of 51c084f/c49bb0e, finding 3: `deficit_a` below
             * is ONE global scalar (the shared CT's total shortfall), tested
             * in this loop against EACH commanded zone's own, individually
             * smaller, threshold. If one heater element opens, the resulting
             * deficit can clear more than one commanded zone's 0.7x-normal
             * bar at once -- s15_warn[z] setting for several zones does NOT
             * mean several zones are faulty; it means one shared-CT deficit
             * is consistent with the fault being in any one of the zones
             * whose bar it cleared, and the guard cannot narrow further than
             * that with only one CT. Read/report s15_warn as "one of these
             * commanded zones" per-zone, never "each flagged zone" as
             * independently faulty -- see GUARD_TEST_MATRIX.md/
             * CURRENT_SENSE.md's S15 entries for the operator-facing
             * wording this drives. */
            bool s15_base_active = !in->current_sensing_disabled && sum_valid &&
                                    in->amps_valid[CT_SUMMED_CHANNEL];
            float deficit_a = s15_base_active ? (expected_sum_a - in->amps[CT_SUMMED_CHANNEL]) : 0.0f;
            for (int z = 0; z < 3; z++) {
                bool active = s15_base_active && in->relay_commanded_now_for_zone[z] &&
                              cfg->i_normal_valid[z] && cfg->i_normal_a[z] > 0.0f;
                if (!active) {
                    state->s15_under_elapsed_s[z] = 0.0f;
                    state->s15_warn[z] = false;
                    continue;
                }
                float threshold_a = cfg->i_normal_a[z] * UNDERCURRENT_FRACTION_DEFAULT;
                if (deficit_a > threshold_a) {
                    state->s15_under_elapsed_s[z] += in->dt_s;
                    state->s15_warn[z] = (state->s15_under_elapsed_s[z] >= UNDERCURRENT_TIME_S_DEFAULT);
                } else {
                    state->s15_under_elapsed_s[z] = 0.0f;
                    state->s15_warn[z] = false;
                }
            }
        }
    }

    /* Not tripped this tick -- S9's verification window has nothing to
     * verify yet (it only means anything once is_tripped is true, handled
     * at the top of this function). Keep it clean so a stray earlier value
     * cannot leak into a future trip's verification window. */
    state->s9_verify_active = false;
    state->s9_verify_elapsed_s = 0.0f;
    state->s9_current_present_streak = 0;
    state->s9_uncommissioned_warn = false;

    return false;
}

uint16_t safety_guards_warn_mask(const safety_guard_state_t *state)
{
    if (state == NULL) {
        return 0u;
    }

    uint16_t mask = 0u;
    if (state->s4_warn) {
        mask |= (uint16_t)(1u << 3);
    }
    if (state->s5_warn) {
        mask |= (uint16_t)(1u << 4);
    }
    if (state->s9_uncommissioned_warn) {
        mask |= (uint16_t)(1u << 9);
    }
    if (state->s10_warn) {
        mask |= (uint16_t)(1u << 10);
    }
    if (state->s12_warn) {
        mask |= (uint16_t)(1u << 12);
    }
    if (state->s13_warn) {
        mask |= (uint16_t)(1u << 13);
    }
    for (int z = 0; z < 3; z++) {
        if (state->s14_warn[z]) {
            mask |= (uint16_t)(1u << 14);
            break;
        }
    }
    for (int z = 0; z < 3; z++) {
        if (state->s15_warn[z]) {
            mask |= (uint16_t)(1u << 15);
            break;
        }
    }
    return mask;
}
