// tc_type_reapply_policy.c -- see tc_type_reapply_policy.h for why this is
// split out of link_task.c/thermo_task.c: it must be linkable into a plain
// host-test executable with zero hardware/RTOS dependencies.
#include "tc_type_reapply_policy.h"

bool tc_type_reapply_policy_should_reapply(uint8_t prev_tc_type, uint8_t new_tc_type)
{
    return prev_tc_type != new_tc_type;
}
