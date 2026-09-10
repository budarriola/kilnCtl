// s8_rate_guard_estimate -- pure computation for auto-deriving the RP2040
// safety processor's S8 (implausible rate-of-rise) threshold from the
// identified per-zone plant model, instead of an operator-typed number.
// docs/audits/s8_auto_calc_design_2026-09-09.md is the design doc this
// implements; read that first for the reasoning this header only summarizes.
// 2026-09-10 review (docs/audits/s8_auto_calc_design_2026-09-09.md's
// follow-up review) found five confirmed defects in the version described
// above and reworked the derivation below; the fixes are described inline
// at each affected point rather than re-narrated in full here.
//
// WHY THIS EXISTS: docs/audits/s8_rate_guard_retune_2026-09-09.md found the
// bench's compiled S8 default (33.3 C/min) was derived from "2x the fastest
// shipped PROFILE ramp" -- a number with no relationship to what the plant
// can actually do. This module instead derives a candidate threshold from
// model_k_dc/model_tau_s (zones_config_get_model()), the same FOPDT
// parameters autotune/profile_executor already trust for feedforward, PLUS
// (since this pass) the measured cross-zone coupling gains onto that same
// zone (zones_config_get_coupling()) -- see "REAL FIRINGS ARE COUPLED"
// below for why the own-zone-only version was wrong.
//
// TEMPERATURE DEPENDENCE, HONESTLY RESTATED: the original version of this
// header claimed picking the coldest zone's fit was "conservative" because
// k and tau both fall at high temperature. That reasoning does not survive
// contact with this document's OWN cited data: PID_EXPANSION_PLAN.md /
// high_temperature_transfer_analysis reports k and tau falling by roughly
// the SAME factor (~20x at 1200 C) as the kiln heats up. If k and tau fall
// together, k/tau ~ P/C (heater power over thermal mass) is approximately
// INVARIANT across the firing -- picking the coldest fit buys close to zero
// conservatism, not the "generous everywhere hotter" margin originally
// claimed. This module still picks the lowest-fit_temp_c zone (there is no
// data today to justify picking any other single point, and it is not
// LESS safe than the alternative), but the real protection against
// temperature drift is the MARGIN below, sized as a noise/model-error
// allowance, not as a stand-in for "protects the whole firing by
// construction." A temperature-scheduled threshold (the zone_model_at()/
// coupling_at() seam added in 5d3bc854) remains the honest long-term fix
// and is not attempted here.
//
// REAL FIRINGS ARE COUPLED, SO THE BASIS MUST BE TOO: S8
// (firmware/SaftyFW/src/safety_guards.c) watches ONE global
// `safety_tc_c` -- there is no per-zone S8. Every real firing starts with
// all three zones at full duty together, and the measured coupling matrix
// (docs/audits/high_temperature_transfer_analysis_2026-09-08.md,
// `zones_config_get_coupling()`) shows the off-diagonal contribution onto a
// zone is comparable to or larger than that zone's own diagonal gain (z0:
// own k_dc 31.96, but z1->z0 + z2->z0 = 27.32 + 21.72 = 49.04 -- the
// all-zones-firing gain is ~2.5x the own-zone-only basis the previous
// version used). A candidate built from k_dc alone therefore UNDERSTATES
// the actual worst-case initial slope by roughly that factor on this plant.
// This version's basis is (k_dc + coupling_gain_sum) / tau_s, where
// coupling_gain_sum is the sum of the OTHER zones' measured steady-state
// gain onto this one -- i.e. the same all-zones-full-duty scenario a stuck
// relay actually produces.
//
// MARGIN, HONESTLY SIZED: the previous version applied a 2.0x "margin" on
// top of the own-zone-only (uncoupled) basis and called the result
// "conservative." Two things were wrong with that: (1) since the basis was
// already missing the ~2.5x coupling contribution, a nominal 2.0x margin on
// the WRONG (too-small) basis is not actually more conservative than 1.0x
// on the right one -- on this plant the "2x margin, own-zone-only" number
// (z0: (31.96/166.9)*60*2.0 = 22.98 C/min) reads as if it has margin to
// spare, while a stuck-on relay driving all zones through this TC produces
// (81.0/166.9)*60 = 29.1 C/min from a full-duty START, i.e. the "safety
// margin" was in the WRONG direction versus a real runaway. (2) A full-duty
// stuck relay IS exactly the basis this function now computes (own gain +
// coupling gain, at u=1.0) -- there is no further multiplicative headroom
// to add for "how much worse could a runaway be," since duty cannot exceed
// 1.0. What legitimately remains to size a margin for is measurement/model
// error in the identified k_dc/tau_s/coupling values themselves, not "how
// much worse than measured could reality be" -- that is now folded into the
// basis directly. S8_RATE_GUARD_ESTIMATE_MARGIN is therefore 1.3 (30%
// headroom for identification error), not 2.0.
//
// FAIL-SAFE DIRECTION: this function only ever produces a candidate at least
// S8_RATE_GUARD_ESTIMATE_FLOOR_C_PER_MIN and at most _CEILING_C_PER_MIN --
// the SAME two bounds firmware/SaftyFW/src/config_store.h's
// CONFIG_STORE_MAX_RATE_C_PER_MIN_FLOOR/_CEILING enforce independently on
// the Pico.
// 2026-09-10 CORRECTION (opus review of 431019ba/0820dfa6/fb495e05): the
// 37.9/36.2/28.9 C/min figures previously quoted here (and in the design
// doc) were computed from tools/PcTools/config_presets/tuned_baseline_
// 20260831.json -- a PRESET FILE, not this board -- combined with the
// LIVE coupling sums, which is exactly the mixture that inflated the
// result. This board's own model_k_dc/tau_s
// (docs/audits/cplval75_coupling_verdict_2026-09-10.md) are 39.2459/263.8,
// 31.9669/269.8, 31.6810/270.9; with the live coupling sums (49.04/36.45/
// 20.75) the honest z0/z1/z2 candidates are **26.10 / 19.78 / 15.10 C/min**.
// z2 lands 0.7% above the 15.0 floor -- i.e. on THIS board's live data the
// floor is very nearly the value this guard clamps to for the coldest
// zone, the opposite of the claim this comment used to make. See finding B
// below before trusting any of these three numbers as a safety margin.
// s8_rate_guard_estimate_reason_t distinguishes an unclamped derivation
// (S8_RATE_GUARD_ESTIMATE_OK) from a floor- or ceiling-clamped one
// (_OK_CLAMPED_FLOOR / _OK_CLAMPED_CEILING) so a caller -- and an operator
// looking at commissioning data -- can tell "this number came from your
// plant" from "this number came from a bound because your plant's own
// number was out of range," which the previous single OK value for both
// cases could not distinguish.
//
// WHO COMPUTES, WHO ENFORCES: the ESP computes this candidate (it alone
// holds the plant model; the Pico has none) but never pushes it unchecked --
// see this header's own doc comment on s8_rate_guard_estimate() below for
// the trust boundary this implies, and the design doc's "who computes it"
// section for the full argument. The Pico's own floor/ceiling range check
// (config_params.c, RANGE_F32_RANGE_OR_ZERO) is a SEPARATE plausibility
// bound with the SAME [15, 60] range this module already clamps its own
// output to -- it cannot reject anything this function emits (its job is to
// bound a hand-entered MANUAL value, which does not pass through this
// module at all) and is not an independent backstop against a
// compromised/buggy ESP for the AUTO path specifically; that claim is
// removed below. A hand-entered operator override always remains possible
// and takes priority -- this module only ever produces a SUGGESTION for
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
 * comment plus docs/audits/s8_auto_calc_design_2026-09-09.md. NOTE (2026-
 * 09-10 review): the previous version of this comment claimed
 * test_s8_rate_guard_estimate.c performs a text-scan cross-check against
 * config_store.h's literal values, the way check_safety_trip_words_sync.ps1
 * does for a different pair of constants. It does not -- that test file has
 * no file I/O and never references config_store.h. No such check exists
 * today; keeping the two headers in sync is manual, by this comment alone,
 * until a real cross-check is written. */
#define S8_RATE_GUARD_ESTIMATE_FLOOR_C_PER_MIN   15.0f
#define S8_RATE_GUARD_ESTIMATE_CEILING_C_PER_MIN 60.0f

/* 30% headroom for identification error (k_dc/tau_s/coupling measurement
 * and fit noise) on top of the all-zones-full-duty basis computed below.
 * NOT "2x measured peak" -- that framing (this macro's previous value,
 * 2.0f, applied to an own-zone-only basis) was reviewed 2026-09-10 and
 * found backwards: a stuck-on relay driving every zone through S8's single
 * shared TC is a duty=1.0 event, i.e. exactly the (k_dc + coupling_gain_sum)
 * basis this module now computes -- there is no physical "worse than full
 * duty" to buy extra margin against, so multiplying that already-worst-case
 * basis by 2x is not a safety margin, it is just a bigger number. What
 * legitimately needs headroom is that the plant identification itself is
 * a fit against noisy captures, not ground truth -- 1.3x is sized for that,
 * not for "how much worse could a runaway be." See this header's top
 * comment ("MARGIN, HONESTLY SIZED") for the full reasoning and the worked
 * bench numbers. */
#define S8_RATE_GUARD_ESTIMATE_MARGIN 1.3f

/* 2026-09-10 finding B (opus review): 1.3x was sized for "the identification
 * is a fit against noisy captures, not ground truth" -- but the only plant
 * this basis has real data for (docs/audits/cplval75_coupling_verdict_2026-
 * 09-10.md) shows the coupling matrix under-predicting settled gain by a
 * SYSTEMATIC 12/19/31% per row (z0/z1/z2), not symmetric noise. Dividing
 * 1.3 by those deficits leaves effective margin of 1.16x/1.09x/0.99x -- z2,
 * the coldest zone and the one this module actually picks on this board,
 * has NO headroom left once the known deficit is backed out, before any
 * allowance for further, still-unmeasured fit noise.
 * DECISION: do not paper over this with a bigger multiplier chosen to make
 * the arithmetic come out to 1.3x again -- a bigger number invented to
 * cancel a specific, cited deficit is exactly the "looks principled, is
 * not" failure this review exists to catch, and it does nothing for the
 * *unmeasured* noise the margin is also supposed to cover. Instead:
 * S8_RATE_GUARD_ESTIMATE_MARGIN (1.3x) is scoped to a basis whose
 * provenance has actually been checked -- see coupling_provenance_ok below
 * and this header's "PROVENANCE" section -- and a board whose coupling
 * matrix fails that check (this bench, today) must not receive the coupled
 * 1.3x treatment at all; it falls back to the wider, historical
 * own-zone-only S8_RATE_GUARD_ESTIMATE_MARGIN_UNCOUPLED margin below. Once
 * this plant's coupling matrix is re-identified and its per-row deficit is
 * re-measured at (ideally) <5%, 1.3x is defensible again; until then this
 * board should not auto-derive S8 from the coupled basis at all (see
 * finding C and the module's overall verdict in the 2026-09-10 review). */

/* Margin for the OWN-ZONE-ONLY basis (coupling_gain_sum treated as 0,
 * whether because a genuinely single-zone board has nothing to couple from,
 * or because coupling_provenance_ok is false and this module refuses to
 * trust an unproven matrix -- see PROVENANCE below). This is the historical
 * pre-2026-09-10 factor: unlike the coupled basis, this one is NOT "already
 * the physical worst case" (a stuck relay on a multi-zone board still drives
 * every OTHER zone's heater too, which this basis cannot see), so it keeps
 * the larger, more conservative multiplier rather than 1.3x. Finding D
 * (2026-09-10 review): the previous code applied 1.3x uniformly, including
 * to boards with no usable coupling data, while claiming in a caller
 * comment ("no worse than the previous own-zone-only basis") that the
 * result was unchanged from the pre-coupling 2.0x version -- it was not
 * (2.0x -> 1.3x is a 1.54x loosening on exactly the boards least equipped to
 * detect a cross-zone runaway). This macro makes that claim true again. */
#define S8_RATE_GUARD_ESTIMATE_MARGIN_UNCOUPLED 2.0f

// PROVENANCE (2026-09-10 finding C): zone_coupling_solve.c's control path
// (coupling_matrix_provenance_ok()) refuses a coupling matrix WHOLE, falling
// back to the uncoupled per-zone basis, whenever it carries measured
// off-diagonals but no member's coupling_diag_k_dc has ever been identified
// on hardware (COUPLING_SOLVE_FALLBACK_MIXED_PROVENANCE) -- exactly this
// bench's condition today. Before this fix, this module (a SAFETY threshold)
// read the same zones_config_get_coupling() data raw, with no equivalent
// check, deriving numbers from a matrix the control path deliberately will
// not touch. That was backwards: a safety estimator should trust unproven
// data LESS than a feedforward path, not fail to notice the same guard
// exists. The caller now passes `coupling_provenance_ok`, computed with the
// SAME rule coupling_matrix_provenance_ok() uses (mirrored, not shared,
// since KilnFW's HTTP layer does not link zone_coupling_solve.c's static
// helper); see s8_rate_guard_estimate.c for how it changes the margin used.
typedef enum {
    /* Candidate derived directly from the identified plant, unclamped by
     * either bound -- the normal case on a commissioned, coupling-aware
     * board whose coupling matrix has also passed the provenance check
     * above. */
    S8_RATE_GUARD_ESTIMATE_OK = 0,
    /* Derived candidate was below the floor and was raised to it -- the
     * caller/operator should know this number came from a bound, not from
     * the plant, e.g. a near-degenerate identification or a coupling-free
     * single-zone board. Previously indistinguishable from _OK. */
    S8_RATE_GUARD_ESTIMATE_OK_CLAMPED_FLOOR,
    /* Derived candidate was above the ceiling and was capped to it -- same
     * "tell the operator this isn't the plant's real number" reasoning. */
    S8_RATE_GUARD_ESTIMATE_OK_CLAMPED_CEILING,
    /* No zone has a usable identification: model_k_dc/model_tau_s > 0 AND
     * model_fit_temp_c is a REAL identified operating point, not the
     * ZONE_MODEL_FIT_TEMP_UNKNOWN sentinel (-273.15f,
     * zones_config_accessors.h) -- an uncommissioned or never-autotuned
     * board, OR a board whose zones were migrated up from a pre-v24 record
     * that stamped the sentinel into every zone (zones_config_json.h /
     * zones_config_migrate.c) and have not been re-identified since. 2026-
     * 09-10 review: the previous version of this function validated
     * fit_temp_c with isfinite() only, and -273.15f IS finite -- being the
     * lowest representable "plausible" temperature, the sentinel
     * unconditionally WON the coldest-zone selection below on every board
     * carrying it (which, as of this review, is every zone on this bench).
     * Caller must not silently arm S8 with a guessed number; fall back to
     * floor or require a manual value. */
    S8_RATE_GUARD_ESTIMATE_NO_DATA,
} s8_rate_guard_estimate_reason_t;

/* Per-zone input -- the caller (safety_cfg_http.c's rate_guard_gather_and_
 * estimate()) builds this from zones_config_get_model()/
 * zones_config_get_model_fit_context()/zones_config_get_coupling() for each
 * commissioned zone. `valid` must be false unless model_k_dc/model_tau_s
 * are finite and > 0.0f AND fit_temp_c is a real identified value, i.e. NOT
 * ZONE_MODEL_FIT_TEMP_UNKNOWN -- this function does not re-derive validity
 * from raw accessor failure codes, to keep it a pure function with no
 * dependency on zones_config_accessors.h's runtime state, but it DOES
 * independently reject the UNKNOWN sentinel value itself (see NO_DATA's
 * comment above) rather than trusting the caller's `valid` flag alone for
 * that specific case, since that flag is exactly what one live caller got
 * wrong. */
typedef struct {
    bool  valid;
    float k_dc;       /* model_k_dc, degC per unit duty at steady state, this
                        * zone's OWN heater only */
    float tau_s;      /* model_tau_s, seconds */
    float fit_temp_c; /* model_fit_temp_c -- the operating point this zone's
                        * k_dc/tau_s were identified at */
    /* Sum of the OTHER zones' measured steady-state gain onto THIS zone's
     * thermocouple (degC per unit duty each, summed) -- i.e. the row of
     * zones_config_get_coupling() for this zone with the diagonal entry
     * excluded. Required, not optional: S8 watches one TC shared by every
     * zone (safety_guards.c has no per-zone concept), so a caller that
     * fills this with 0.0f on a multi-zone board is not being conservative,
     * it is silently reproducing the ~2.5x-too-small basis the 2026-09-10
     * review found (docs/audits/high_temperature_transfer_analysis_2026-09-
     * 08.md's z0 row: 27.32 + 21.72 = 49.04 against an own k_dc of 31.96).
     * 0.0f is only correct for a genuinely single-zone board with no other
     * heaters to couple from. Must be finite and >= 0.0f; a negative value
     * (which would UNDERSTATE the coupled basis) is treated the same as a
     * non-finite one -- see s8_rate_guard_estimate.c. */
    float coupling_gain_sum_c_per_duty;
    /* 2026-09-10 addition (finding C, "PROVENANCE" above): true only if
     * EITHER this board has no measured off-diagonal coupling at all (a
     * genuinely single-zone board -- nothing to prove), OR every zone's
     * coupling_diag_k_dc has been identified on hardware, mirroring
     * coupling_matrix_provenance_ok()'s rule exactly. When false, this
     * module ignores coupling_gain_sum_c_per_duty (treats it as 0 for the
     * basis) and uses S8_RATE_GUARD_ESTIMATE_MARGIN_UNCOUPLED instead of
     * S8_RATE_GUARD_ESTIMATE_MARGIN, regardless of what value the caller
     * put in coupling_gain_sum_c_per_duty -- an unproven matrix must not
     * receive the smaller, coupled-basis margin. */
    bool coupling_provenance_ok;
} s8_rate_guard_zone_input_t;

/* Computes the auto-calc candidate. Picks the single VALID zone whose
 * fit_temp_c is LOWEST (see this header's top comment for the honest,
 * reduced strength of this "conservative" claim), computes that zone's
 * maximum achievable slope at full duty INCLUDING the other zones' coupled
 * contribution ONLY if that zone's coupling_provenance_ok is true -- (k_dc +
 * coupling_gain_sum_c_per_duty) / tau_s, converted from degC/s to degC/min.
 * Margin is S8_RATE_GUARD_ESTIMATE_MARGIN (1.3x) UNLESS this is a multi-zone
 * board (zone_count > 1) whose coupling_provenance_ok is false, in which
 * case coupling is ignored (basis is k_dc / tau_s alone) AND
 * S8_RATE_GUARD_ESTIMATE_MARGIN_UNCOUPLED (2.0x) applies instead -- a
 * genuinely single-zone board (zone_count == 1) has no other heater to be
 * missing coupling protection against, so it keeps the ordinary 1.3x margin
 * even with coupling_gain_sum_c_per_duty == 0. Then clamps to
 * [S8_RATE_GUARD_ESTIMATE_FLOOR_C_PER_MIN, _CEILING_C_PER_MIN].
 *
 * Returns S8_RATE_GUARD_ESTIMATE_OK (unclamped), _OK_CLAMPED_FLOOR/_CEILING
 * (clamped -- see those enumerators' comments), with *out_c_per_min set in
 * all three OK* cases; or S8_RATE_GUARD_ESTIMATE_NO_DATA (out_c_per_min
 * untouched) if no zone in `zones[0..zone_count-1]` is valid, INCLUDING the
 * case where every candidate zone's fit_temp_c is the
 * ZONE_MODEL_FIT_TEMP_UNKNOWN sentinel. zone_count above
 * MAX31856_CHANNEL_COUNT is treated as MAX31856_CHANNEL_COUNT (defensive,
 * matches this codebase's other array-bound conventions); NULL zones/
 * out_c_per_min, or zone_count == 0, also returns NO_DATA. */
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
