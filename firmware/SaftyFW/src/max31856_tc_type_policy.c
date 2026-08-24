// max31856_tc_type_policy.c -- see max31856_tc_type_policy.h.
#include "max31856_tc_type_policy.h"

#include "max31856.h" // MAX31856_TC_TYPE_T -- pure header, safe for a host-tested file

bool max31856_tc_type_is_valid(uint8_t tc_type)
{
    return tc_type <= MAX31856_TC_TYPE_T;
}
