#include "ct_blind_policy.h"

bool ct_blind_treat_as_present(bool ct_sensing_active, bool clock_stalled, bool current_fresh, uint32_t now_ms,
                               uint32_t timestamp_ms)
{
    if (!ct_sensing_active || clock_stalled || current_fresh) {
        return false;
    }
    uint32_t age_ms = now_ms - timestamp_ms; // unsigned wrap-safe; timestamp 0 ages from boot
    return age_ms > CT_BLIND_GRACE_MS;
}
