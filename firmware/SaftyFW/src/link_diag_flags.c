// link_diag_flags.c -- see link_diag_flags.h.
#include "link_diag_flags.h"

#include "kilnlink/kilnlink_diag.h" // KILNLINK_DIAG_FLAG_* -- pure wire-format header

uint8_t link_diag_flags_compute(bool calibration_missing, bool sim_context_seen,
                                 bool clear_trip_diag_present, bool tc_reconfig_gave_up,
                                 bool abs_max_temp_disabled, bool rate_guard_disabled)
{
    uint8_t flags = 0;
    if (sim_context_seen) {
        flags |= (uint8_t)KILNLINK_DIAG_FLAG_SIM_CONTEXT_SEEN;
    }
    if (calibration_missing) {
        flags |= (uint8_t)KILNLINK_DIAG_FLAG_CALIBRATION_MISSING;
    }
    // KILNLINK_DIAG_FLAG_ESTOP_UNWIRED_SUSPECT: never set -- see this file's
    // header comment.
    if (clear_trip_diag_present) {
        flags |= (uint8_t)KILNLINK_DIAG_FLAG_CLEAR_TRIP_DIAG_PRESENT;
    }
    if (tc_reconfig_gave_up) {
        flags |= (uint8_t)KILNLINK_DIAG_FLAG_TC_RECONFIG_GAVE_UP;
    }
    if (abs_max_temp_disabled) {
        flags |= (uint8_t)KILNLINK_DIAG_FLAG_S1_ABS_MAX_TEMP_DISABLED;
    }
    if (rate_guard_disabled) {
        flags |= (uint8_t)KILNLINK_DIAG_FLAG_S8_RATE_GUARD_DISABLED;
    }
    return flags;
}
