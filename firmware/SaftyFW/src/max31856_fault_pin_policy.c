// max31856_fault_pin_policy.c -- see max31856_fault_pin_policy.h.
#include "max31856_fault_pin_policy.h"

bool max31856_fault_pin_asserted(bool fault_gpio_high)
{
    return !fault_gpio_high;
}
