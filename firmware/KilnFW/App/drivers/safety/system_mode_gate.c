#include "system_mode_gate.h"

#include <stdio.h>
#include <string.h>

// One table, one legible switch -- same "one record reviewed as a unit"
// discipline route_tier_table.h uses for auth (system_mode_gate.h's top
// comment). Adding a new action or tightening an existing rule means editing
// exactly one case here, never hunting across callers.
//
// Owner decisions this pass encodes (docs/SYSTEM_MODE_GATE_PLAN.md section 5,
// 2026-09-25):
//   Q1 -- manual relay writes: BLANKET-REFUSE (not claimed-relays-only) any
//        manual relay write while a firing OR autotune session is active,
//        regardless of which relay or which transport asked.
//   Q2 -- zones/config writes: REFUSE ALL (not scoped) while a firing or
//        autotune is active. Not wired in this pass -- SYS_ACTION_WRITE_
//        ZONES_CONFIG falls through to the default OK below.
//   Q3 -- factory reset / cfgfs format while running: refuse outright. Not
//        wired in this pass -- SYS_ACTION_FACTORY_RESET/SYS_ACTION_CFGFS_
//        FORMAT fall through to the default OK below.
//   Q4 -- HTTP status for a new refusal: 409, decided at each HTTP call site
//        (this module has no notion of HTTP), leaving OTA's existing 428
//        untouched.
bool system_mode_gate_check(sys_action_t action, const sys_mode_snapshot_t *snap, char *reason,
                             size_t reason_cap)
{
    if (snap == NULL) {
        // No facts is not a green light -- same "NULL cannot mean allowed"
        // rule readiness_gate_evaluate() enforces for the same reason.
        if (reason != NULL && reason_cap > 0) {
            snprintf(reason, reason_cap,
                     "refused -- system mode facts unavailable; cannot confirm this action is safe "
                     "right now");
        }
        return true;
    }

    switch (action) {
    case SYS_ACTION_RAW_RELAY_DEBUG_WRITE:
        // Q1: blanket refusal, any relay, while either engine is active --
        // reuses the same two facts profile start/autotune start already
        // gate on becoming true, never a re-derivation of "is a run active."
        if (snap->profile_running || snap->autotune_running) {
            if (reason != NULL && reason_cap > 0) {
                snprintf(reason, reason_cap,
                         "refused -- a firing or autotune run is active; manual relay control is "
                         "not available until it ends");
            }
            return true;
        }
        return false;

    case SYS_ACTION_START_PROFILE:
    case SYS_ACTION_START_AUTOTUNE:
    case SYS_ACTION_WRITE_ZONES_CONFIG:
    case SYS_ACTION_FACTORY_RESET:
    case SYS_ACTION_CFGFS_FORMAT:
    case SYS_ACTION_OTA_START:
    default:
        // Not wired in this pass -- see this file's top comment. Correct
        // today because no caller invokes this gate for these actions yet;
        // profile/autotune start keep readiness_gate.h's own gate, and OTA
        // keeps ota_interlock.c's.
        return false;
    }
}
