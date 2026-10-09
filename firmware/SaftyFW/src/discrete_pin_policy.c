// discrete_pin_policy.c -- see discrete_pin_policy.h.
#include "discrete_pin_policy.h"

bool discrete_pin_policy_estop_asserted(bool gpio9_high)
{
    return gpio9_high; // HIGH == pressed / cut wire / unfitted == STOP, no inversion
}

bool discrete_pin_policy_estop_asserted_ex(bool gpio9_high, uint8_t active_level)
{
    // Only the explicit ACTIVE_LOW value inverts. Everything else -- 0, a
    // legacy record's zero byte, erased flash's 0xFF, a value written by
    // some future firmware this build does not understand -- falls through
    // to ACTIVE_HIGH, the fail-safe polarity that treats a lost signal as a
    // stop. See the header for why the default must never be data-dependent
    // in the other direction.
    if (active_level == DISCRETE_PIN_POLICY_ESTOP_ACTIVE_LOW) {
        return !gpio9_high;
    }
    return discrete_pin_policy_estop_asserted(gpio9_high);
}

bool discrete_pin_policy_main_fault_asserted(bool gpio10_high)
{
    return !gpio10_high; // LOW == asserted (open-collector pulled low)
}
