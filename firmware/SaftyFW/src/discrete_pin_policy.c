// discrete_pin_policy.c -- see discrete_pin_policy.h.
#include "discrete_pin_policy.h"

bool discrete_pin_policy_estop_asserted(bool gpio9_high)
{
    return gpio9_high; // HIGH == pressed / cut wire / unfitted == STOP, no inversion
}

bool discrete_pin_policy_main_fault_asserted(bool gpio10_high)
{
    return !gpio10_high; // LOW == asserted (open-collector pulled low)
}
