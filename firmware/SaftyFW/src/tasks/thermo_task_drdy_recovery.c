// thermo_task_drdy_recovery.c -- see thermo_task_drdy_recovery.h for the
// full rationale.
#include "thermo_task_drdy_recovery.h"

bool thermo_task_drdy_missed_edge(uint32_t notifications, bool assume_ready,
                                   uint32_t drdy_pin_level)
{
    if (notifications != 0 || assume_ready) {
        return false;
    }
    return drdy_pin_level == 0;
}
