// s8_rate_guard_estimate.c -- see s8_rate_guard_estimate.h for the full
// design rationale (docs/audits/s8_auto_calc_design_2026-09-09.md, plus the
// 2026-09-10 review addendum described inline in that header).
#include "s8_rate_guard_estimate.h"

#include <math.h>

/* Mirrors zones_config_accessors.h's ZONE_MODEL_FIT_TEMP_UNKNOWN (-273.15f)
 * exactly. Duplicated, not #included, deliberately: this module is a pure,
 * host-testable function with no FreeRTOS/hardware/accessor dependency
 * (same rationale zone_coupling_solve.c already established for this
 * directory), and zones_config_accessors.h pulls in esp_err.h/heater_
 * output.h/kiln_io.h/safety_link.h, none of which the host test build links
 * against. Kept in sync by this comment ONLY -- no automated cross-check
 * exists between this value and zones_config_accessors.h's today; if one is
 * added, model it on this file's floor/ceiling comment, which makes the
 * same "no such check exists" admission rather than claiming one it
 * doesn't have. */
#define S8_RATE_GUARD_ESTIMATE_FIT_TEMP_UNKNOWN (-273.15f)

s8_rate_guard_estimate_reason_t s8_rate_guard_estimate(const s8_rate_guard_zone_input_t *zones,
                                                        uint8_t zone_count, float *out_c_per_min)
{
    if (zones == NULL || out_c_per_min == NULL || zone_count == 0) {
        return S8_RATE_GUARD_ESTIMATE_NO_DATA;
    }
    if (zone_count > MAX31856_CHANNEL_COUNT) {
        zone_count = MAX31856_CHANNEL_COUNT;
    }

    // Find the VALID zone with the lowest fit_temp_c -- see this file's
    // header comment ("TEMPERATURE DEPENDENCE, HONESTLY RESTATED") for why
    // this is still the point picked, but is NOT the strong "conservative
    // everywhere hotter" guarantee the pre-2026-09-10 version of this
    // comment claimed.
    bool  have_candidate = false;
    float best_fit_temp_c = 0.0f;
    float best_k_dc = 0.0f;
    float best_tau_s = 0.0f;
    float best_coupling_sum = 0.0f;
    bool  best_coupling_provenance_ok = false;

    for (uint8_t i = 0; i < zone_count; i++) {
        const s8_rate_guard_zone_input_t *z = &zones[i];
        if (!z->valid) {
            continue;
        }
        if (!isfinite(z->k_dc) || z->k_dc <= 0.0f) {
            continue;
        }
        if (!isfinite(z->tau_s) || z->tau_s <= 0.0f) {
            continue;
        }
        if (!isfinite(z->fit_temp_c)) {
            continue;
        }
        // 2026-09-10 fix: ZONE_MODEL_FIT_TEMP_UNKNOWN (-273.15f) IS finite,
        // and is the lowest representable "plausible" temperature, so the
        // isfinite() check above never caught it -- it unconditionally won
        // the "lowest fit_temp_c" comparison below on every zone carrying
        // it (which, as of this review, is every zone migrated up from a
        // pre-v24 record and never re-identified since -- see
        // zones_config_json.h / zones_config_migrate.c). Reject it
        // explicitly rather than trusting the caller's `valid` flag alone,
        // since a live caller (safety_cfg_http.c's rate_guard_gather_and_
        // estimate()) built `valid` as `have_model && have_fit_ctx` with no
        // sentinel check at all.
        if (z->fit_temp_c == S8_RATE_GUARD_ESTIMATE_FIT_TEMP_UNKNOWN) {
            continue;
        }
        // Coupling contribution must be finite and non-negative -- a
        // negative value would UNDERSTATE the coupled worst-case basis,
        // which is exactly the failure mode this field exists to close
        // (see the struct's own comment). Treat it the same as any other
        // disqualifying input rather than silently clamping it to zero.
        if (!isfinite(z->coupling_gain_sum_c_per_duty) || z->coupling_gain_sum_c_per_duty < 0.0f) {
            continue;
        }
        if (!have_candidate || z->fit_temp_c < best_fit_temp_c) {
            have_candidate = true;
            best_fit_temp_c = z->fit_temp_c;
            best_k_dc = z->k_dc;
            best_tau_s = z->tau_s;
            best_coupling_sum = z->coupling_gain_sum_c_per_duty;
            best_coupling_provenance_ok = z->coupling_provenance_ok;
        }
    }

    if (!have_candidate) {
        return S8_RATE_GUARD_ESTIMATE_NO_DATA;
    }

    // 2026-09-10 fix (finding C, "PROVENANCE" in the header): an unproven
    // coupling matrix (measured off-diagonals, no member's coupling_diag_
    // k_dc ever identified on hardware -- the same condition zone_coupling_
    // solve.c's control path refuses whole) must not receive the smaller,
    // coupled-basis margin. Fold the coupling contribution out of the basis
    // entirely in that case. Note this is NOT the same thing as a
    // genuinely single-zone board (zone_count == 1): a single-zone board
    // has no other heater to couple from at all, so its own-zone k_dc/tau_s
    // fit already IS the full physical picture, and the ordinary
    // identification-error margin (1.3x) is the right one for it, same as
    // for a proven multi-zone board. The WIDER S8_RATE_GUARD_ESTIMATE_
    // MARGIN_UNCOUPLED (2.0x) is reserved for the case this basis cannot
    // see at all: a board with MORE than one zone (so a stuck relay on
    // another zone really could drive this TC) whose coupling data is not
    // trustworthy -- exactly the gap between "no coupling contribution in
    // the basis" and "no coupling risk in reality."
    float coupling_for_basis = best_coupling_provenance_ok ? best_coupling_sum : 0.0f;
    bool  multi_zone_unproven = (zone_count > 1) && !best_coupling_provenance_ok;
    float margin = multi_zone_unproven ? S8_RATE_GUARD_ESTIMATE_MARGIN_UNCOUPLED
                                        : S8_RATE_GUARD_ESTIMATE_MARGIN;

    // First-order step response's initial slope at full duty (u=1.0),
    // INCLUDING the other zones' coupled contribution at their own full
    // duty (when that contribution's provenance checks out) -- the actual
    // worst case, since every real firing starts with all zones at full
    // duty together and S8 watches one TC shared by all of them
    // (safety_guards.c has no per-zone concept):
    // dT/dt|t=0 = (k_dc + coupling_gain_sum) * u / tau_s, in degC/second.
    // Converted to degC/minute to match max_rate_c_per_min's own unit.
    float total_gain_c_per_duty = best_k_dc + coupling_for_basis;
    float slope_c_per_min = (total_gain_c_per_duty / best_tau_s) * 60.0f;

    float candidate = slope_c_per_min * margin;

    if (!isfinite(candidate)) {
        // Defensive: a pathological tau_s near zero could overflow the
        // divide above despite passing the >0.0f check. Treat exactly like
        // "no usable data" rather than propagate a non-finite candidate --
        // the floor below is skipped deliberately for this one case, since
        // clamping +Inf into range would silently hide a broken
        // identification behind a normal-looking number.
        return S8_RATE_GUARD_ESTIMATE_NO_DATA;
    }

    s8_rate_guard_estimate_reason_t reason = S8_RATE_GUARD_ESTIMATE_OK;
    if (candidate < S8_RATE_GUARD_ESTIMATE_FLOOR_C_PER_MIN) {
        candidate = S8_RATE_GUARD_ESTIMATE_FLOOR_C_PER_MIN;
        reason = S8_RATE_GUARD_ESTIMATE_OK_CLAMPED_FLOOR;
    } else if (candidate > S8_RATE_GUARD_ESTIMATE_CEILING_C_PER_MIN) {
        candidate = S8_RATE_GUARD_ESTIMATE_CEILING_C_PER_MIN;
        reason = S8_RATE_GUARD_ESTIMATE_OK_CLAMPED_CEILING;
    }

    *out_c_per_min = candidate;
    return reason;
}

s8_rate_guard_auto_decision_t s8_rate_guard_auto_decide(float candidate_c_per_min, float current_c_per_min,
                                                         bool current_is_set)
{
    // Arming a dormant guard is never a loosening -- see this function's
    // header comment for the full policy writeup.
    if (!current_is_set) {
        return S8_RATE_GUARD_AUTO_APPLY;
    }
    if (candidate_c_per_min <= current_c_per_min) {
        return S8_RATE_GUARD_AUTO_APPLY;
    }
    return S8_RATE_GUARD_AUTO_SUGGEST_ONLY;
}
