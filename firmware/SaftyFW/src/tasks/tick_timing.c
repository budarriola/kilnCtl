// tick_timing.c -- see tick_timing.h.
#include "tick_timing.h"

float tick_dt_compute_s(uint32_t now_ms, uint32_t prev_now_ms, bool have_prev_now_ms,
                         bool clock_stalled, float nominal_dt_s, float min_dt_s, float max_dt_s)
{
    if (!have_prev_now_ms || clock_stalled) {
        // No trustworthy measurement available this tick -- see this
        // function's header comment for why the fallback is nominal_dt_s
        // exactly, unclamped (it is not a measurement, so the clamp meant to
        // bound a measurement's drift does not apply to it).
        return nominal_dt_s;
    }

    // Wraparound-safe: both operands come from the same to_ms_since_boot()
    // clock, same reasoning safety_core.c's context_valid/reboot_grace_active
    // already document.
    uint32_t elapsed_ms = now_ms - prev_now_ms;
    float    measured_s = (float)elapsed_ms / 1000.0f;

    if (measured_s < min_dt_s) {
        return min_dt_s;
    }
    if (measured_s > max_dt_s) {
        return max_dt_s;
    }
    return measured_s;
}

bool snapshot_is_fresh(uint32_t now_ms, uint32_t timestamp_ms, uint32_t max_age_ms)
{
    // Same wraparound-safe unsigned subtraction as safety_core.c's own
    // context_valid/reboot_grace_active -- both operands come from the same
    // clock, so the difference wraps correctly regardless of which side is
    // numerically larger.
    uint32_t age_ms = now_ms - timestamp_ms;
    return age_ms < max_age_ms;
}
