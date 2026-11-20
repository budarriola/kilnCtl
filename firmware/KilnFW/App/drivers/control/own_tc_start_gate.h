// own_tc_start_gate.h -- AT-19/AT-24: refuse to START a new firing while the Pico reports its own
// (safety) thermocouple faulted. The Pico keeps a 60..120 s ride-through allowance across a benign
// reboot of an interrupted firing and cannot tell a resume from a new start, so the ESP (which can)
// refuses the new start. Resume (profile_executor_resume) does not pass through here.
// Fail-open on unknown state only: link down / no status leaves other interlocks to decide.
#ifndef OWN_TC_START_GATE_H
#define OWN_TC_START_GATE_H

#include <stdbool.h>
#include <stdint.h>

#include "../common/uart_task_ids.h"

#define OWN_TC_START_REFUSAL_MSG "Safety thermocouple faulted"

static inline bool own_tc_start_must_refuse(bool link_up, uint8_t flags, uint8_t tc_fault)
{
    if (!link_up || (flags & SAFETY_FLAG_TC_NOT_INSTALLED) != 0u) {
        return false;
    }
    return (tc_fault & (THERMO_FAULT_OPEN | THERMO_FAULT_OVUV | THERMO_FAULT_TCRANGE)) != 0u;
}

#endif
