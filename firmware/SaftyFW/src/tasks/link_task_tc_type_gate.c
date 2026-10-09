// link_task_tc_type_gate.c -- see link_task_tc_type_gate.h.
#include <stddef.h> // NULL

#include "link_task_tc_type_gate.h"

bool link_task_tc_type_gate_decide(const link_task_tc_type_gate_input_t *in,
                                    uint8_t context_flag_heat_requested,
                                    uint8_t context_flag_profile_running,
                                    uint8_t context_flag_heat_owner_active)
{
    if (in == NULL) {
        return false; // no input is not safe -- fail closed
    }

    // Check 1: hardware ground truth.
    if (in->relay_energized) {
        return false;
    }

    // Check 2: a recently-accepted enable request that may not have
    // physically closed the relay yet.
    if (in->enable_ever_seen &&
        in->enable_age_ms < LINK_TASK_TC_TYPE_GATE_ENABLE_RECENT_SAFE_WINDOW_MS) {
        return false;
    }

    // Check 3: the ESP's own "no active heat owner" report, fail-closed on
    // any staleness/unavailability.
    if (in->degraded_no_context) {
        return false;
    }
    if (!in->context_ever_received) {
        return false;
    }
    if (!in->context_lock_available) {
        return false;
    }
    if (!in->context_valid) {
        return false;
    }
    if (in->context_age_ms >= in->context_max_age_ms) {
        return false;
    }
    uint8_t unsafe_mask =
        (uint8_t)(context_flag_heat_requested | context_flag_profile_running |
                  context_flag_heat_owner_active);
    if (in->context_flags & unsafe_mask) {
        return false;
    }

    // Additional Pico-local heuristics, defense in depth on top of 1-3.
    if (in->relay_on_continuous) {
        return false;
    }
    if (in->any_current_present) {
        return false;
    }

    return true;
}
