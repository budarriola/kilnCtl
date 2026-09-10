// s8_rate_guard_estimate -- pure computation for auto-deriving the RP2040
// safety processor's S8 (implausible rate-of-rise) threshold from the
// identified per-zone plant model, instead of an operator-typed number.
// docs/audits/s8_auto_calc_design_2026-09-09.md is the design doc this
// implements; read that first for the reasoning this header only summarizes.
//
// WHY THIS EXISTS: docs/audits/s8_rate_guard_retune_2026-09-09.md found the
// bench's compiled S8 default (33.3 C/min) was derived from "2x the fastest
// shipped PROFILE ramp" -- a number with no relationship to what the plant
// can actually do. This module instead derives a candidate threshold from
// model_k_dc/model_tau_s (zones_config_get_model()), the same FOPDT
// parameters autotune/profile_executor already trust for feedforward.
//
// TEMPERATURE DEPENDENCE (the hard part -- see the design doc's own section):
// k and tau both fall by roughly the same factor at high temperature
// (PID_EXPANSION_PLAN.md / high_temperature_transfer_analysis), so a single
// identification's k/tau ratio is NOT assumed portable to a different
// operating point. This module does not attempt to schedule the threshold
// across temperature -- it deliberately picks the LOWEST model_fit_temp_c
// across all zones with a valid identification (the coldest, and per the
// audit's own finding, the FASTEST part of any firing) as its one evidence
// point, on the reasoning that a ceiling sized for the fastest-known
// operating point stays conservative (never accidentally loose) everywhere
// hotter, where the plant is slower. It refuses to guess at a hotter regime
// it has no data for: if every zone's only identification sits at a high
// fit_temp_c (commissioning skipped a cold-start run), this function
// declines to produce an estimate at all (S8_RATE_GUARD_ESTIMATE_NO_DATA)
// rather than deriving a number from an unrepresentative point and silently
// under-protecting the coldest, fastest part of a real firing.
//
// FAIL-SAFE DIRECTION: this function only ever produces a candidate at least
// S8_RATE_GUARD_ESTIMATE_FLOOR_C_PER_MIN and at most _CEILING_C_PER_MIN --
// the SAME two bounds firmware/SaftyFW/src/config_store.h's
// CONFIG_STORE_MAX_RATE_C_PER_MIN_FLOOR/_CEILING enforce independently on
// the Pico. Errs tight (nuisance-trip), never loose (missed runaway): the
// margin factor multiplies the identified plant's OWN fastest achievable
// rate, and the floor refuses to let a bad/near-zero identification produce
// an unusably-loose (or negative/zero) threshold.
//
// WHO COMPUTES, WHO ENFORCES: the ESP computes this candidate (it alone
// holds the plant model; the Pico has none) but never pushes it unchecked --
// see this header's own doc comment on s8_rate_guard_estimate() below for
// the trust boundary this implies, and the design doc's "who computes it"
// section for the full argument. The Pico's own floor/ceiling range check
// (config_params.c, CHECK_F32_RANGE_OR_ZERO) is the second, INDEPENDENT
// backstop: even a compromised or buggy ESP cannot push an absurd value
// past it. A hand-entered operator override always remains possible and
// takes priority -- this module only ever produces a SUGGESTION for
// safety_set_rate_guard(), never writes anything itself.
#ifndef S8_RATE_GUARD_ESTIMATE_H
#define S8_RATE_GUARD_ESTIMATE_H

#include <stdbool.h>
#include <stdint.h>

#include "MAX31856.h" /* MAX31856_CHANNEL_COUNT */

#ifdef __cplusplus
extern "C" {
#endif

/* Mirrors firmware/SaftyFW/src/config_store.h's CONFIG_STORE_MAX_RATE_C_PER_
 * MIN_FLOOR/_CEILING exactly -- see that macro's doc comment for the
 * measured basis (65 recorded bench captures, firmware's own 60s-window
 * estimator peaking at 7.69 C/min; extended simulator 13.1-13.5 C/min).
 * Duplicated rather than shared because KilnFW and SaftyFW are separate
 * firmware images with no common header today; kept in sync by this
 * comment plus docs/audits/s8_auto_calc_design_2026-09-09.md, and by
 * test_s8_rate_guard_estimate.c's own cross-check against the literal
 * values quoted in config_store.h (a text-scan test, same precedent as
 * this codebase's other cross-repo constant-sync checks, e.g.
 * check_safety_trip_words_sync.ps1). */
#define S8_RATE_GUARD_ESTIMATE_FLOOR_C_PER_MIN   15.0f
#define S8_RATE_GUARD_ESTIMATE_CEILING_C_PER_MIN 60.0f

/* 2x the identified plant's own fastest achievable dT/dt at full duty --
 * same "2x measured peak" convention c43323a2 and this codebase's other
 * safety-margin choices already use, just applied to a measured plant
 * capability instead of a shipped profile's authored ramp. */
#define S8_RATE_GUARD_ESTIMATE_MARGIN 2.0f

typedef enum {
    S8_RATE_GUARD_ESTIMATE_OK = 0,
    /* No zone has a usable identification (model_k_dc/model_tau_s > 0 AND
     * model_fit_temp_c != ZONE_MODEL_FIT_TEMP_UNKNOWN) -- an uncommissioned
     * or never-autotuned board. Caller must not silently arm S8 with a
     * guessed number; fall back to floor or require a manual value. */
    S8_RATE_GUARD_ESTIMATE_NO_DATA,
} s8_rate_guard_estimate_reason_t;

/* Per-zone input -- the caller (a future MCP tool / HTTP handler) builds
 * this from zones_config_get_model()/zones_config_get_model_fit_context()
 * for each commissioned zone. `valid` must be false unless BOTH calls
 * succeeded AND k_dc/tau_s are finite and > 0.0f AND fit_temp_c is not
 * ZONE_MODEL_FIT_TEMP_UNKNOWN -- this function does not re-derive validity
 * from raw accessor failure codes, to keep it a pure function with no
 * dependency on zones_config_accessors.h's runtime state. */
typedef struct {
    bool  valid;
    float k_dc;       /* model_k_dc, degC per unit duty at steady state */
    float tau_s;      /* model_tau_s, seconds */
    float fit_temp_c; /* model_fit_temp_c -- the operating point this zone's
                        * k_dc/tau_s were identified at */
} s8_rate_guard_zone_input_t;

/* Computes the auto-calc candidate. Picks the single VALID zone whose
 * fit_temp_c is LOWEST (see this header's top comment for why "lowest" is
 * the conservative choice), computes that zone's own maximum achievable
 * slope at full duty (k_dc / tau_s, converted from degC/s to degC/min),
 * applies S8_RATE_GUARD_ESTIMATE_MARGIN (in degC/min), then clamps to
 * [S8_RATE_GUARD_ESTIMATE_FLOOR_C_PER_MIN, _CEILING_C_PER_MIN].
 *
 * Returns S8_RATE_GUARD_ESTIMATE_OK with *out_c_per_min set, or
 * S8_RATE_GUARD_ESTIMATE_NO_DATA (out_c_per_min untouched) if no zone in
 * `zones[0..zone_count-1]` is valid. zone_count above MAX31856_CHANNEL_COUNT
 * is treated as MAX31856_CHANNEL_COUNT (defensive, matches this codebase's
 * other array-bound conventions); NULL zones/out_c_per_min, or zone_count
 * == 0, also returns NO_DATA. */
s8_rate_guard_estimate_reason_t s8_rate_guard_estimate(const s8_rate_guard_zone_input_t *zones,
                                                        uint8_t zone_count, float *out_c_per_min);

/* WRITE POLICY (docs/audits/s8_auto_calc_design_2026-09-09.md's "Part 3 --
 * the write path" section has the full writeup; this is the short version
 * next to the function it governs).
 *
 * Chose "tighten-auto-apply, loosen-requires-confirm" over the two pure
 * options the design doc's Part 2 left open:
 *   - Pure auto-apply (every re-identification silently overwrites the Pico's
 *     armed threshold) was rejected: a bad identification -- corrupted
 *     model_k_dc/tau_s, or a fit run at an unrepresentative operating point --
 *     could silently RAISE the threshold, i.e. reduce protection, with no
 *     operator ever looking at the new number. "Autocalculate for real
 *     kilns" does not mean "never let a human notice the guard moved
 *     the wrong way."
 *   - Pure suggest-and-confirm (every candidate, tighter or looser, waits
 *     for an operator click) was rejected too: it reproduces exactly the
 *     staleness problem that motivated this feature -- a guard that sits at
 *     its old, possibly-wrong number until someone remembers to go confirm
 *     it, which is the "hand-entered number is the wrong design long-term"
 *     complaint restated.
 *   - The middle path implemented here auto-applies only the direction that
 *     can never make the guard less safe (tightening, or arming a dormant
 *     guard for the first time) and requires an explicit confirm for the
 *     one direction that can (loosening an already-armed guard). This is
 *     genuinely fail-safe, not merely appealing: the two decisions are not
 *     symmetric risks being averaged, they are a safety-relevant one
 *     (loosen) and a safety-neutral-or-positive one (tighten) with
 *     deliberately different handling.
 *
 * `current_is_set` false (guard currently DORMANT, max_rate_c_per_min == 0 /
 * never commissioned, safety_guards.c's own "0 disables the check" contract)
 * always returns APPLY: an unset guard enforces no ceiling at all -- moving
 * from "no ceiling" to any finite one, however derived, cannot be a
 * loosening. When `current_is_set` is true and `candidate_c_per_min` is
 * greater than `current_c_per_min`, this ALWAYS returns SUGGEST_ONLY -- the
 * caller (the HTTP write path, not this pure function) must never write that
 * candidate without a separate, explicit operator confirmation, and must
 * still read back and verify whatever it does eventually write (see
 * s8_rate_guard_estimate.c's own "never write anything itself" note above --
 * that remains true of this function too; it only classifies, it never
 * calls into safety_link). */
typedef enum {
    S8_RATE_GUARD_AUTO_APPLY = 0,    /* tightens, ties, or arms a dormant guard -- safe to write immediately */
    S8_RATE_GUARD_AUTO_SUGGEST_ONLY, /* would LOOSEN an armed guard -- must not be written without an
                                       * explicit operator confirm */
} s8_rate_guard_auto_decision_t;

s8_rate_guard_auto_decision_t s8_rate_guard_auto_decide(float candidate_c_per_min, float current_c_per_min,
                                                         bool current_is_set);

#ifdef __cplusplus
}
#endif

#endif // S8_RATE_GUARD_ESTIMATE_H
