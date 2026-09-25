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
//        autotune is active. Wired below for SYS_ACTION_WRITE_ZONES_CONFIG --
//        gates the CALLERS (zones_http_post.c, uart_bridge_ext_control.c's
//        SET_ZONE_PID/MODEL, kiln_cfg_http.c's apply submit, backup_import.c's
//        apply), never zones_config_set_*()/zones_config_accessors.h
//        themselves, because autotune_engine.c/adaptive_tune.c call those
//        accessors directly while a run IS active and must keep working
//        (docs/SYSTEM_MODE_GATE_PLAN.md gate-slices-2/4/5 spec).
//   Q3 -- factory reset / cfgfs format while running: refuse outright. Wired
//        below for SYS_ACTION_FACTORY_RESET/SYS_ACTION_CFGFS_FORMAT, same
//        two facts as Q1/Q2 -- PAUSED counts as running for both, same as Q2.
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

    case SYS_ACTION_WRITE_ZONES_CONFIG:
        // Q2: refuse ALL zone/relay/guard config writes -- not scoped to
        // which field changed -- while a firing or autotune is active,
        // PAUSED included (same profile_running fact as Q1 above, which
        // profile_executor_get_status() already reports true for PAUSED).
        if (snap->profile_running || snap->autotune_running) {
            if (reason != NULL && reason_cap > 0) {
                snprintf(reason, reason_cap,
                         "refused -- a firing or autotune run is active; zone configuration cannot be "
                         "changed until it ends");
            }
            return true;
        }
        return false;

    case SYS_ACTION_FACTORY_RESET:
    case SYS_ACTION_CFGFS_FORMAT:
        // Q3: refuse outright while running -- no ack, no override; this is
        // strictly more destructive than an ordinary zone-config write.
        if (snap->profile_running || snap->autotune_running) {
            if (reason != NULL && reason_cap > 0) {
                snprintf(reason, reason_cap,
                         "refused -- a firing or autotune run is active; this action is not available "
                         "until it ends");
            }
            return true;
        }
        return false;

    case SYS_ACTION_START_PROFILE:
    case SYS_ACTION_START_AUTOTUNE:
    case SYS_ACTION_OTA_START:
    default:
        // Not wired in this pass -- see this file's top comment. Correct
        // today because no caller invokes this gate for these actions;
        // profile/autotune start keep readiness_gate.h's own gate, and OTA
        // keeps ota_interlock.c's.
        return false;
    }
}
