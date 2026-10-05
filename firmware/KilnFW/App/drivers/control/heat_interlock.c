// heat_interlock.c -- see heat_interlock.h. Pure, no ESP-IDF/FreeRTOS
// dependency, host-testable exactly like ota_interlock.c.
#include "heat_interlock.h"

#include <stdio.h>

heat_interlock_result_t heat_interlock_check(const heat_interlock_snapshot_t *snap, char *reason_out,
                                              size_t reason_cap)
{
    if (!snap || !snap->update_in_progress) {
        return HEAT_INTERLOCK_OK;
    }

    if (reason_out && reason_cap > 0) {
        const char *who;
        switch (snap->update_context) {
            case HEAT_INTERLOCK_UPDATE_ESP:  who = "an ESP firmware update"; break;
            case HEAT_INTERLOCK_UPDATE_PICO: who = "a Pico firmware update"; break;
            case HEAT_INTERLOCK_UPDATE_NONE:
            default:
                // Should not happen (update_in_progress true with NONE means
                // the glue forgot to set update_context) -- fail with a
                // still-specific-enough reason rather than crash, same "no
                // valid data -> the safe default" rule ota_interlock.c
                // follows for !actual_valid.
                who = "a firmware update";
                break;
        }
        if (snap->update_context == HEAT_INTERLOCK_UPDATE_ESP) {
            // The ESP-side update may be a GitHub fetch that is stuck or unwanted; name the way out.
            // Must stay within HEAT_INTERLOCK_REASON_MAX (96) -- test_heat_interlock.c checks it.
            snprintf(reason_out, reason_cap,
                     "%s is in progress; heat blocked (POST /api/update/fetch/cancel)", who);
        } else {
            snprintf(reason_out, reason_cap, "%s is in progress -- heat cannot be commanded until it finishes",
                     who);
        }
    }
    return HEAT_INTERLOCK_REFUSED;
}
