// current_presence_policy.c -- see current_presence_policy.h.
#include "current_presence_policy.h"

bool current_presence_is_flowing(uint32_t counts_avg, uint16_t zero_counts, float i_present_a,
                                   float k_ct_v_per_a, float gain)
{
    int32_t delta_counts = (int32_t)counts_avg - (int32_t)zero_counts;
    if (delta_counts < 0) {
        delta_counts = 0; // same "max(0, ...)" convention as cs_counts_to_amps()
    }

    if (k_ct_v_per_a > 0.0f && i_present_a > 0.0f && gain > 0.0f) {
        // Commissioned: invert cs_counts_to_amps()'s exact formula --
        //   amps = v_adc / (gain * sqrt2 * k_ct)
        //   v_adc = delta_counts * vref / full_scale
        // -- to get the counts threshold equivalent to "amps > i_present_a",
        // so a calibrated channel's presence decision is bit-for-bit the
        // same as the old "amps > i_present_a" comparison always was.
        float v_present = i_present_a * gain * CURRENT_PRESENCE_POLICY_SQRT2 * k_ct_v_per_a;
        float counts_threshold_f =
            v_present * CURRENT_PRESENCE_POLICY_ADC_FULL_SCALE / CURRENT_PRESENCE_POLICY_ADC_VREF_V;
        if (counts_threshold_f < 0.0f) {
            counts_threshold_f = 0.0f;
        }
        return (float)delta_counts > counts_threshold_f;
    }

    // Not commissioned: fall back to a fixed, sensitive counts-domain floor
    // -- see this file's header comment for the safe-direction reasoning.
    return (uint32_t)delta_counts > CURRENT_PRESENCE_POLICY_FALLBACK_MARGIN_COUNTS;
}
