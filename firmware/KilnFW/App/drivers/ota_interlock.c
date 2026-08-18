// ota_interlock.c -- see ota_interlock.h.
#include "ota_interlock.h"

#include <stdarg.h>
#include <stdio.h>

// snprintf-with-varargs helper so every refusal path below is a one-liner;
// a no-op when the caller passed NULL/0 (only wants the pass/fail result).
static void set_reason(char *reason_out, size_t reason_cap, const char *fmt, ...)
{
    if (!reason_out || reason_cap == 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(reason_out, reason_cap, fmt, ap);
    va_end(ap);
}

ota_interlock_result_t ota_interlock_check(const ota_interlock_snapshot_t *snap,
                                            const ota_interlock_zone_snapshot_t *zones,
                                            uint8_t zone_count, char *reason_out,
                                            size_t reason_cap)
{
    // 1. The single cross-processor update mutex -- cheapest check, and
    // orthogonal to kiln state (see ota_interlock.h's doc comment on why
    // this runs first).
    if (snap->other_update_in_progress) {
        set_reason(reason_out, reason_cap, "an update is already in progress");
        return OTA_INTERLOCK_REFUSED;
    }

    // 2. Safety link health -- UPDATE_PROTOCOL.md section 1: "Do not start a
    // transfer over a link that is already marginal."
    if (!snap->safety_link_up) {
        set_reason(reason_out, reason_cap, "safety link is down");
        return OTA_INTERLOCK_REFUSED;
    }

    // 3. Autotune -- deliberately driving a zone away from steady state.
    if (snap->autotune_active) {
        set_reason(reason_out, reason_cap, "autotune is running");
        return OTA_INTERLOCK_REFUSED;
    }

    // 4. Profile running or paused.
    if (snap->profile_state == OTA_INTERLOCK_PROFILE_RUNNING) {
        set_reason(reason_out, reason_cap, "a profile is running");
        return OTA_INTERLOCK_REFUSED;
    }
    if (snap->profile_state == OTA_INTERLOCK_PROFILE_PAUSED) {
        set_reason(reason_out, reason_cap, "a profile is paused");
        return OTA_INTERLOCK_REFUSED;
    }

    // 5. A firing that survived a reboot without a clean ending -- catches
    // what profile_state (always IDLE on a fresh boot) cannot see.
    if (snap->run_state_interrupted) {
        set_reason(reason_out, reason_cap,
                    "a firing was interrupted by a reboot and has not been acknowledged");
        return OTA_INTERLOCK_REFUSED;
    }

    // 6. Per-zone: heater commanded on, independent of profile_state (a
    // manual relay-on outside any run must also block).
    for (uint8_t i = 0; i < zone_count; i++) {
        if (!zones[i].active) {
            continue;
        }
        if (zones[i].heater_commanded) {
            set_reason(reason_out, reason_cap, "zone %u heater is commanded on", (unsigned)i);
            return OTA_INTERLOCK_REFUSED;
        }
    }

    // 7. Per-zone: temperature ceiling. A zone whose reading isn't currently
    // trustworthy is refused too, not skipped -- "no valid data -> the safe
    // default", same rule this codebase applies to commanding heat.
    for (uint8_t i = 0; i < zone_count; i++) {
        if (!zones[i].active) {
            continue;
        }
        if (!zones[i].actual_valid) {
            set_reason(reason_out, reason_cap, "zone %u temperature is not available",
                        (unsigned)i);
            return OTA_INTERLOCK_REFUSED;
        }
        if (zones[i].actual_c >= snap->temp_ceiling_c) {
            set_reason(reason_out, reason_cap, "zone %u is at %.0f C", (unsigned)i,
                        (double)zones[i].actual_c);
            return OTA_INTERLOCK_REFUSED;
        }
    }

    return OTA_INTERLOCK_OK;
}
