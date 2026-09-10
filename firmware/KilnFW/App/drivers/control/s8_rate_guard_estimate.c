// s8_rate_guard_estimate.c -- see s8_rate_guard_estimate.h for the full
// design rationale (docs/audits/s8_auto_calc_design_2026-09-09.md).
#include "s8_rate_guard_estimate.h"

#include <math.h>

s8_rate_guard_estimate_reason_t s8_rate_guard_estimate(const s8_rate_guard_zone_input_t *zones,
                                                        uint8_t zone_count, float *out_c_per_min)
{
    if (zones == NULL || out_c_per_min == NULL || zone_count == 0) {
        return S8_RATE_GUARD_ESTIMATE_NO_DATA;
    }
    if (zone_count > MAX31856_CHANNEL_COUNT) {
        zone_count = MAX31856_CHANNEL_COUNT;
    }

    // Find the VALID zone with the lowest fit_temp_c -- the coldest, and per
    // the retune audit's own finding, the FASTEST part of any firing. Using
    // that single point as the (unscheduled) global ceiling stays
    // conservative at every hotter point in a real firing, where k and tau
    // both fall together and the plant is physically slower -- see this
    // file's header comment for why extrapolating the OTHER direction (a
    // hot-only identification applied cold) would not be safe.
    bool  have_candidate = false;
    float best_fit_temp_c = 0.0f;
    float best_k_dc = 0.0f;
    float best_tau_s = 0.0f;

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
        if (!have_candidate || z->fit_temp_c < best_fit_temp_c) {
            have_candidate = true;
            best_fit_temp_c = z->fit_temp_c;
            best_k_dc = z->k_dc;
            best_tau_s = z->tau_s;
        }
    }

    if (!have_candidate) {
        return S8_RATE_GUARD_ESTIMATE_NO_DATA;
    }

    // First-order step response's initial slope at full duty (u=1.0):
    // dT/dt|t=0 = k_dc * u / tau_s, in degC/second. Converted to degC/minute
    // to match max_rate_c_per_min's own unit.
    float slope_c_per_min = (best_k_dc / best_tau_s) * 60.0f;

    float candidate = slope_c_per_min * S8_RATE_GUARD_ESTIMATE_MARGIN;

    if (!isfinite(candidate)) {
        // Defensive: a pathological tau_s near zero could overflow the
        // divide above despite passing the >0.0f check. Treat exactly like
        // "no usable data" rather than propagate a non-finite candidate --
        // the floor below is skipped deliberately for this one case, since
        // clamping +Inf into range would silently hide a broken
        // identification behind a normal-looking number.
        return S8_RATE_GUARD_ESTIMATE_NO_DATA;
    }

    if (candidate < S8_RATE_GUARD_ESTIMATE_FLOOR_C_PER_MIN) {
        candidate = S8_RATE_GUARD_ESTIMATE_FLOOR_C_PER_MIN;
    } else if (candidate > S8_RATE_GUARD_ESTIMATE_CEILING_C_PER_MIN) {
        candidate = S8_RATE_GUARD_ESTIMATE_CEILING_C_PER_MIN;
    }

    *out_c_per_min = candidate;
    return S8_RATE_GUARD_ESTIMATE_OK;
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
